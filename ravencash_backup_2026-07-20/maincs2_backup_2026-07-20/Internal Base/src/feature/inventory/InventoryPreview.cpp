#include "InventoryPreview.h"

#include <Windows.h>
#include <d3d11.h>

#include "../../../ext/minhook/MinHook.h"
#include "../../menu/Menu.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/memory/Patterns.h"
#include "../skinchanger/SCLogger.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

extern SCLogger g_Logger;

namespace {

using GetResourceViewFn = ID3D11ShaderResourceView *(__fastcall *)(
    void *, void *, char, char, const char *);
using RunScriptFn = void(__fastcall *)(void *, void *, const char *,
                                       const char *, std::uint64_t);

enum class PreviewState : int {
  Unavailable,
  Ready,
  Creating,
  Loading,
  Live,
};

GetResourceViewFn s_originalGetResourceView = nullptr;
void *s_hookTarget = nullptr;

std::mutex s_textureMutex;
ID3D11ShaderResourceView *s_capturedTexture = nullptr;
ID3D11ShaderResourceView *s_frameTexture = nullptr;

RunScriptFn s_runScript = nullptr;
std::uintptr_t s_mainMenuPanelStorage = 0;
std::uintptr_t s_uiEngineStorage = 0;

std::mutex s_panelMutex;
std::uint16_t s_definitionIndex = 0;
int s_paintKit = -1;
bool s_autoRotate = true;
bool s_panelCreated = false;
int s_createAttempts = 0;
ULONGLONG s_lastCreateAttempt = 0;

std::atomic<PreviewState> s_state{PreviewState::Unavailable};
std::atomic<bool> s_loggedCapture{false};
std::atomic<bool> s_tickedThisFrame{false};

bool ContainsInsensitive(const char *text, const char *needle) {
  if (!text || !needle || !needle[0])
    return false;
  const std::size_t needleLength = std::strlen(needle);
  for (const char *cursor = text; *cursor; ++cursor) {
    std::size_t i = 0;
    while (i < needleLength && cursor[i] &&
           std::tolower(static_cast<unsigned char>(cursor[i])) ==
               std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
    if (i == needleLength)
      return true;
  }
  return false;
}

bool CopyTextureName(void *descriptor, char (&out)[256]) {
  if (!descriptor)
    return false;
  __try {
    const auto base =
        *reinterpret_cast<const std::uintptr_t **>(descriptor);
    if (!base)
      return false;
    const auto holder = reinterpret_cast<const char **>(base[1]);
    const char *name = holder ? *holder : nullptr;
    if (!name)
      return false;

    std::size_t length = 0;
    for (; length < sizeof(out) - 1; ++length) {
      const unsigned char value = static_cast<unsigned char>(name[length]);
      if (value == 0)
        break;
      if (value < 0x20 || value > 0x7E)
        return false;
      out[length] = static_cast<char>(value);
    }
    if (length == 0 || length == sizeof(out) - 1)
      return false;
    out[length] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
    return false;
  }
}

bool IsUsableTexture(ID3D11ShaderResourceView *view) {
  if (!view)
    return false;
  __try {
    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    if (!resource)
      return false;

    ID3D11Texture2D *texture = nullptr;
    const HRESULT result = resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
    resource->Release();
    if (FAILED(result) || !texture)
      return false;

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    texture->Release();
    return description.Width >= 192 && description.Height >= 128 &&
           description.Width <= 4096 && description.Height <= 4096;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool SafeAddRef(ID3D11ShaderResourceView *view) {
  if (!view)
    return false;
  __try {
    view->AddRef();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void CaptureTexture(ID3D11ShaderResourceView *view, const char *name) {
  if (!IsUsableTexture(view) || !SafeAddRef(view))
    return;

  ID3D11ShaderResourceView *oldTexture = nullptr;
  {
    std::lock_guard<std::mutex> lock(s_textureMutex);
    if (s_capturedTexture == view) {
      view->Release();
      s_state.store(PreviewState::Live, std::memory_order_release);
      return;
    }
    oldTexture = s_capturedTexture;
    s_capturedTexture = view;
  }
  if (oldTexture)
    oldTexture->Release();

  s_state.store(PreviewState::Live, std::memory_order_release);
  if (!s_loggedCapture.exchange(true, std::memory_order_acq_rel))
    g_Logger.Log("[InventoryPreview] captured 3D item texture: %s", name);
}

ID3D11ShaderResourceView *__fastcall HookedGetResourceView(
    void *renderer, void *descriptor, char resourceType, char alternateView,
    const char *debugName) {
  ID3D11ShaderResourceView *view =
      s_originalGetResourceView
          ? s_originalGetResourceView(renderer, descriptor, resourceType,
                                      alternateView, debugName)
          : nullptr;

  if (!view || !Menu::IsOpen || alternateView != 0)
    return view;

  char textureName[256]{};
  if (CopyTextureName(descriptor, textureName) &&
      ContainsInsensitive(textureName, "raven_inventory_item_preview"))
    CaptureTexture(view, textureName);
  return view;
}

bool ValidateGetResourceView(std::uintptr_t address) {
  if (!address)
    return false;
  __try {
    static constexpr unsigned char expected[] = {0x41, 0x80, 0xFF, 0xFF};
    return std::memcmp(reinterpret_cast<const void *>(address + 0xAE),
                       expected, sizeof(expected)) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void *SafeGetUiEngine() {
  if (!s_uiEngineStorage)
    return nullptr;
  __try {
    return *reinterpret_cast<void **>(s_uiEngineStorage);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void *SafeGetMainMenuUiPanel() {
  if (!s_mainMenuPanelStorage)
    return nullptr;
  __try {
    void *clientPanel = *reinterpret_cast<void **>(s_mainMenuPanelStorage);
    if (!clientPanel)
      return nullptr;
    return *reinterpret_cast<void **>(
        reinterpret_cast<std::uintptr_t>(clientPanel) + sizeof(void *));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool SafeRunScript(const char *script) {
  if (!script || !s_runScript)
    return false;
  void *engine = SafeGetUiEngine();
  void *panel = SafeGetMainMenuUiPanel();
  if (!engine || !panel)
    return false;
  __try {
    s_runScript(engine, panel, script, "", 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::string BuildPanelScript(std::uint16_t definitionIndex, int paintKit,
                             bool autoRotate) {
  std::string script;
  script.reserve(2100);
  script += "(function(){";
  script += "var root=$.GetContextPanel();if(!root)return;";
  script += "var old=root.FindChildTraverse('RavenInventoryPreviewHost');";
  script += "if(old)old.DeleteAsync(0.0);";
  script += "var host=$.CreatePanel('Panel',root,'RavenInventoryPreviewHost',{"
            "hittest:false,style:'width:760px;height:340px;position:0px 0px "
            "0px;opacity:0.01;overflow:clip;'});";
  script += "var preview=$.CreatePanel('MapItemPreviewPanel',host,"
            "'RavenInventoryItemPreview3D',{map:'ui/xpshop_item',"
            "camera:'camera_weapon_7','require-composition-layer':true,"
            "'composition-layer-texture-name':"
            "'raven_inventory_item_preview',player:false,"
            "initial_entity:'item',active_item_idx:0,mouse_rotate:false,"
            "auto_recenter:true,panzoom_enabled:false,"
            "'transparent-background':true,'disable-depth-of-field':true,"
            "'pin-fov':'vertical',"
            "csm_split_plane0_distance_override:'500.0',"
            "hide_while_waiting_for_composite_materials:false,hittest:false,"
            "style:'width:760px;height:340px;'});";
  script +=
      "var item=InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(";
  script += std::to_string(definitionIndex);
  script += ",";
  script += std::to_string(paintKit > 0 ? paintKit : 0);
  script += ");if(preview&&item){preview.SetActiveItem(0);"
            "preview.SetItemItemId(item,'');"
            "preview.SetRotationLimits(60,45);"
            "preview.SetAutoRotateAmount(";
  script += autoRotate ? "20,-2" : "0,0";
  script += ");preview.SetAutoRotatePeriod(6,6);"
            "preview.SetRenderInterval(1);}";
  script += "})();";
  return script;
}

} // namespace

namespace InventoryPreview {

bool InstallHook() {
    static const char* resourceViewPattern = "raven_inventory_item_preview";
      Patterns::RenderSystemDX11::GetResourceView;

  const std::uintptr_t address =
      Memory::PatternScan("rendersystemdx11.dll", resourceViewPattern);
  if (!ValidateGetResourceView(address)) {
    g_Logger.Log("[InventoryPreview] GetResourceView validation failed");
    return false;
  }

  s_hookTarget = reinterpret_cast<void *>(address);
  const MH_STATUS createStatus =
      MH_CreateHook(s_hookTarget, &HookedGetResourceView,
                    reinterpret_cast<void **>(&s_originalGetResourceView));
  const MH_STATUS enableStatus =
      createStatus == MH_OK ? MH_EnableHook(s_hookTarget)
                            : MH_ERROR_NOT_CREATED;
  if (createStatus != MH_OK ||
      (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)) {
    g_Logger.Log("[InventoryPreview] hook failed: create=%d enable=%d",
                 static_cast<int>(createStatus),
                 static_cast<int>(enableStatus));
    s_hookTarget = nullptr;
    s_originalGetResourceView = nullptr;
    return false;
  }

  s_runScript = reinterpret_cast<RunScriptFn>(Memory::PatternScan(
      "panorama.dll",
      Patterns::Panorama::RunScript));
  s_mainMenuPanelStorage = Memory::ResolveRipRelative(
      "client.dll", Patterns::Client::PMainMenuPanel, 5, 9);
  s_uiEngineStorage = Memory::ResolveRipRelative(
      "client.dll", Patterns::Client::PUiEngine, 10, 14);

  if (!s_runScript || !s_mainMenuPanelStorage || !s_uiEngineStorage) {
    g_Logger.Log("[InventoryPreview] Panorama dependencies unavailable");
    s_state.store(PreviewState::Unavailable, std::memory_order_release);
  } else {
    s_state.store(PreviewState::Ready, std::memory_order_release);
    g_Logger.Log("[InventoryPreview] 3D item preview ready");
  }
  return true;
}

void ResetTexture() {
  ID3D11ShaderResourceView *captured = nullptr;
  ID3D11ShaderResourceView *frame = nullptr;
  {
    std::lock_guard<std::mutex> lock(s_textureMutex);
    captured = s_capturedTexture;
    frame = s_frameTexture;
    s_capturedTexture = nullptr;
    s_frameTexture = nullptr;
  }
  if (frame)
    frame->Release();
  if (captured)
    captured->Release();
}

void ClosePanel() {
  SafeRunScript(
      "(function(){var p=$.GetContextPanel().FindChildTraverse("
      "'RavenInventoryPreviewHost');if(p)p.DeleteAsync(0.0);})();");
  {
    std::lock_guard<std::mutex> lock(s_panelMutex);
    s_panelCreated = false;
    s_definitionIndex = 0;
    s_paintKit = -1;
    s_createAttempts = 0;
    s_lastCreateAttempt = 0;
  }
  ResetTexture();
  if (s_runScript && s_mainMenuPanelStorage && s_uiEngineStorage)
    s_state.store(PreviewState::Ready, std::memory_order_release);
}

void Shutdown() {
  ClosePanel();
  if (s_hookTarget) {
    MH_DisableHook(s_hookTarget);
    MH_RemoveHook(s_hookTarget);
  }
  s_hookTarget = nullptr;
  s_originalGetResourceView = nullptr;
  s_runScript = nullptr;
  s_mainMenuPanelStorage = 0;
  s_uiEngineStorage = 0;
  s_state.store(PreviewState::Unavailable, std::memory_order_release);
}

void BeginFrame() {
  s_tickedThisFrame.store(false, std::memory_order_release);
  ID3D11ShaderResourceView *previous = nullptr;
  {
    std::lock_guard<std::mutex> lock(s_textureMutex);
    previous = s_frameTexture;
    s_frameTexture = s_capturedTexture;
    if (s_frameTexture)
      s_frameTexture->AddRef();
  }
  if (previous)
    previous->Release();
}

void EndFrame() {
  ID3D11ShaderResourceView *frame = nullptr;
  {
    std::lock_guard<std::mutex> lock(s_textureMutex);
    frame = s_frameTexture;
    s_frameTexture = nullptr;
  }
  if (frame)
    frame->Release();

  bool panelCreated = false;
  {
    std::lock_guard<std::mutex> lock(s_panelMutex);
    panelCreated = s_panelCreated;
  }
  if (panelCreated &&
      !s_tickedThisFrame.load(std::memory_order_acquire))
    ClosePanel();
}

void Tick(std::uint16_t definitionIndex, int paintKit, bool autoRotate) {
  s_tickedThisFrame.store(true, std::memory_order_release);
  if (definitionIndex == 0 || !s_runScript || !s_mainMenuPanelStorage ||
      !s_uiEngineStorage)
    return;

  const ULONGLONG now = GetTickCount64();
  bool selectionChanged = false;
  bool shouldCreate = false;
  {
    std::lock_guard<std::mutex> lock(s_panelMutex);
    selectionChanged = definitionIndex != s_definitionIndex ||
                       paintKit != s_paintKit || autoRotate != s_autoRotate;
    if (selectionChanged) {
      s_definitionIndex = definitionIndex;
      s_paintKit = paintKit;
      s_autoRotate = autoRotate;
      s_panelCreated = false;
      s_createAttempts = 0;
    }

    const bool retry = s_panelCreated && !IsLive() && s_createAttempts < 3 &&
                       now - s_lastCreateAttempt >= 2500;
    shouldCreate = !s_panelCreated || retry;
    if (shouldCreate && now - s_lastCreateAttempt < 350)
      shouldCreate = false;
    if (shouldCreate) {
      s_lastCreateAttempt = now;
      ++s_createAttempts;
    }
  }

  if (selectionChanged)
    ResetTexture();
  if (!shouldCreate)
    return;

  s_state.store(PreviewState::Creating, std::memory_order_release);
  const std::string script =
      BuildPanelScript(definitionIndex, paintKit, autoRotate);
  const bool created = SafeRunScript(script.c_str());
  {
    std::lock_guard<std::mutex> lock(s_panelMutex);
    s_panelCreated = created;
  }
  s_state.store(created ? PreviewState::Loading : PreviewState::Creating,
                std::memory_order_release);
}

ID3D11ShaderResourceView *GetFrameTexture() {
  std::lock_guard<std::mutex> lock(s_textureMutex);
  return s_frameTexture;
}

bool IsLive() {
  return s_state.load(std::memory_order_acquire) == PreviewState::Live;
}

const char *GetStatusText() {
  switch (s_state.load(std::memory_order_acquire)) {
  case PreviewState::Live:
    return "SOURCE 2 3D";
  case PreviewState::Loading:
    return "LOADING MATERIAL";
  case PreviewState::Creating:
    return "CREATING PREVIEW";
  case PreviewState::Ready:
    return "SELECT AN ITEM";
  default:
    return "3D PREVIEW UNAVAILABLE";
  }
}

} // namespace InventoryPreview
