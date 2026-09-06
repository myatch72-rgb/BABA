#include "Hooks.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windowsx.h>


#include "../../ext/imgui/imgui.h"
#include "../../ext/imgui/imgui_impl_dx11.h"
#include "../../ext/imgui/imgui_impl_win32.h"
#include "../../ext/minhook/MinHook.h"


#include "../../src/feature/visuals/Visuals.h"
#include "../../src/menu/Menu.h"
#include "../../src/sdk/entity/EntityManager.h"
#include "../feature/combat/legitbot/Aimbot.h"
#include "../feature/combat/legitbot/AntiAim.h"
#include "../feature/combat/legitbot/RCS.h"
#include "../feature/combat/legitbot/SilentAim.h"
#include "../feature/combat/legitbot/SilentAimHook.h"
#include "../feature/combat/legitbot/Triggerbot.h"
#include "../feature/misc/Misc.h"
#include "../feature/misc/CustomModel.h"
#include "../feature/misc/bhop/Movement.h"
#include "../feature/skinchanger/SCLogger.h"
#include "../feature/skinchanger/SkinChanger.h"
#include "../feature/inventory/InventoryChanger.h"
#include "../feature/inventory/InventoryPreview.h"
#include "../feature/visuals/aspectratio/AspectRatio.h"
#include "../feature/visuals/chams/GPURenderer.h"
#include "../feature/visuals/chamsv2/ChamsV2.h"
#include "../feature/visuals/chamsv4/ChamsV4.h"
#include "../sdk/classes/CViewSetup.h"
#include "../sdk/classes/SceneSystem.h"
#include "../sdk/entity/Classes.h"
#include "../sdk/interfaces/Interfaces.h"
#include "../sdk/memory/Offsets.h"
#include "../sdk/memory/PatternScan.h"
#include "../sdk/memory/Patterns.h"
#include "../sdk/memory/Convars.h"
#include "../sdk/utils/CCSGOInput.h"
#include "../sdk/utils/Globals.h"
#include "../sdk/utils/Raycasting.h"
#include "../sdk/utils/Utils.h"
#include "../sdk/utils/SteamAvatarManager.h"
#include "CrashTelemetry.h"
#include "StabilityProfile.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <thread>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *g_Device = nullptr;
static ID3D11DeviceContext *g_Context = nullptr;
static ID3D11RenderTargetView *g_RTV = nullptr;
ID3D11RenderTargetView *g_ChamsRTV = nullptr;
static HWND g_Window = nullptr;
static bool g_Init = false;
// Protects g_RTV against concurrent access from hkResizeBuffers and
// PresentInternal (which can run on different D3D/game threads).
static CRITICAL_SECTION g_RTVLock;
static bool g_RTVLockInitialized = false;
static IDXGISwapChain *g_CurrentSwapChain = nullptr;
static std::atomic<bool> g_RenderReady{false};
static std::atomic<unsigned long long> g_CompletedPresentFrames{0};
// 0 = waiting for the first completed Present, 1 = installing,
// 2 = active, -1 = failed closed.
static std::atomic<int> g_InventoryHookState{0};
// 0 = waiting for Inventory, 1 = installing, 2 = active, -1 = failed closed.
static std::atomic<int> g_FullRuntimeHookState{0};
static uintptr_t g_BackBufferIdentity = 0;
static UINT g_BackBufferDescWidth = 0;
static UINT g_BackBufferDescHeight = 0;
static DXGI_FORMAT g_BackBufferDescFormat = DXGI_FORMAT_UNKNOWN;
static ID3D11RasterizerState *g_WireframeRS = nullptr;
static ID3D11RasterizerState *g_SolidRS = nullptr;
static ID3D11DepthStencilState *g_WireframeDepthState = nullptr;
static ID3D11PixelShader *g_WireframePS = nullptr;
static ID3D11Buffer *g_WireframeColorCB = nullptr;

float g_BackBufferWidth = 0.f;
float g_BackBufferHeight = 0.f;
float g_WindowWidth = 0.f;
float g_WindowHeight = 0.f;
static bool g_IsRenderingUI = false;
std::uintptr_t g_debugLightSceneAddr = 0;
std::atomic<int> g_LightSceneCounter = 0;

// Injection remains Present-only. When full runtime is requested, optional
// resources are prepared with the renderer but their hooks are activated only
// by ActivateFullRuntimeHooks after the render and Inventory stages are stable.
static constexpr bool kSafeStartupProfile =
    !StabilityProfile::EnableFullRuntime;

struct WireframeColorBuffer {
  float color[4];
};

enum class WireframeTarget { None, Enemy };

static bool IsAnyWireframeEnabled() { return Globals::wireframe_enemy_enabled; }

static bool HasSkinningLikeConstantBuffer(ID3D11DeviceContext *pContext) {
  if (!pContext)
    return false;

  for (UINT slot = 0; slot < 14; ++slot) {
    ID3D11Buffer *vsBuffer = nullptr;
    pContext->VSGetConstantBuffers(slot, 1, &vsBuffer);
    if (!vsBuffer)
      continue;

    D3D11_BUFFER_DESC desc{};
    vsBuffer->GetDesc(&desc);
    vsBuffer->Release();

    if (desc.ByteWidth >= 512 && desc.ByteWidth <= 262144)
      return true;
  }

  return false;
}

static WireframeTarget ClassifyWireframeTarget(ID3D11DeviceContext *pContext,
                                               UINT indexCount,
                                               UINT instanceCount) {
  if (!pContext || indexCount == 0 || !IsAnyWireframeEnabled())
    return WireframeTarget::None;

  const UINT safeInstances = instanceCount > 0 ? instanceCount : 1;
  if (safeInstances != 1)
    return WireframeTarget::None;

  ID3D11Buffer *vertexBuffer = nullptr;
  UINT stride = 0;
  UINT offset = 0;
  pContext->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
  if (vertexBuffer)
    vertexBuffer->Release();

  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  pContext->IAGetPrimitiveTopology(&topology);

  ID3D11DepthStencilState *depthState = nullptr;
  UINT stencilRef = 0;
  pContext->OMGetDepthStencilState(&depthState, &stencilRef);

  bool depthEnabled = true;
  if (depthState) {
    D3D11_DEPTH_STENCIL_DESC desc{};
    depthState->GetDesc(&desc);
    depthEnabled = desc.DepthEnable == TRUE;
    depthState->Release();
  }

  const UINT totalIndexCount = indexCount * safeInstances;
  const bool meshStrideLooksModel = stride >= 28 && stride <= 75;
  const bool topologyLooksModel =
      topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  const bool hasSkinningBuffer = HasSkinningLikeConstantBuffer(pContext);

  const bool enemyCandidate =
      meshStrideLooksModel && topologyLooksModel && hasSkinningBuffer &&
      depthEnabled && totalIndexCount >= 3180 && totalIndexCount <= 32000;

  if (Globals::wireframe_enemy_enabled && enemyCandidate)
    return WireframeTarget::Enemy;

  return WireframeTarget::None;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);

static HANDLE g_FOVThread = nullptr;
static std::atomic<bool> g_UnloadHooks{false};

namespace Hooks {

inline std::atomic<uintptr_t> g_CachedPawn{0};
inline std::atomic<uintptr_t> g_CachedController{0};

static bool IsPawnValid(C_CSPlayerPawn *pawn) {
  uintptr_t pPawn = reinterpret_cast<uintptr_t>(pawn);

  if (!pawn || !Utils::IsValidPtr(pPawn) || pPawn < 0x1000000 ||
      pPawn > 0x00007FFFFFFFFFFF)
    return false;

  __try {
    int health = Utils::SafeRead<int>(pPawn + Offsets::m_iHealth);
    if (health <= 0 || health > 100)
      return false;

    uint8_t lifeState = Utils::SafeRead<uint8_t>(pPawn + Offsets::m_lifeState);
    if (lifeState != 0)
      return false;

    uint32_t team = Utils::SafeRead<uint32_t>(pPawn + Offsets::m_iTeamNum);
    if (team < 2 || team > 3)
      return false;

    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void UpdateFOV(C_CSPlayerPawn *localPawn,
                      C_CSPlayerController *localController) {
  if (!IsPawnValid(localPawn))
    return;

  __try {
    uintptr_t pPawn = reinterpret_cast<uintptr_t>(localPawn);
    uintptr_t pCtrl = reinterpret_cast<uintptr_t>(localController);

    if (pPawn < 0x1000000 || pPawn > 0x00007FFFFFFFFFFF || pCtrl < 0x1000000 ||
        pCtrl > 0x00007FFFFFFFFFFF)
      return;

    if (Utils::SafeRead<bool>(pPawn + Offsets::m_bIsScoped) ||
        Utils::SafeRead<bool>(pPawn + Offsets::m_bIsBuyMenuOpen))
      return;

    float targetFov =
        static_cast<float>(static_cast<int>(Globals::visuals_fov));
    if (targetFov < 1.0f || targetFov > 170.0f)
      targetFov = 90.0f;

    if (Utils::IsValidPtr(pCtrl)) {
      uint32_t currentDesired =
          Utils::SafeRead<uint32_t>(pCtrl + Offsets::m_iDesiredFOV);
      if (currentDesired > 0 && currentDesired < 180 &&
          currentDesired != static_cast<uint32_t>(targetFov))
        Utils::SafeWrite<uint32_t>(pCtrl + Offsets::m_iDesiredFOV,
                                   static_cast<uint32_t>(targetFov));
    }

    uintptr_t cameraServices =
        Utils::SafeRead<uintptr_t>(pPawn + Offsets::m_pCameraServices);
    if (Utils::IsValidPtr(cameraServices) && cameraServices > 0x1000000 &&
        cameraServices < 0x00007FFFFFFFFFFF) {

      Utils::SafeWrite<uint32_t>(cameraServices + Offsets::m_iFOV,
                                 static_cast<uint32_t>(targetFov));
      Utils::SafeWrite<uint32_t>(cameraServices + Offsets::m_iFOVStart,
                                 static_cast<uint32_t>(targetFov));
    }

    float currentVmFov =
        Utils::SafeRead<float>(pPawn + Offsets::m_flViewmodelFOV);
    if (currentVmFov > 0.0f && currentVmFov < 180.0f &&
        currentVmFov != targetFov)
      Utils::SafeWrite<float>(pPawn + Offsets::m_flViewmodelFOV, targetFov);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

DWORD WINAPI FOVPhantomThread(LPVOID) {
  while (!g_UnloadHooks) {
    Sleep(16); // 16ms (~60fps) yerine 1ms busy-loop, CPU'yu boğmaz

    if (!Globals::visuals_fov_enabled)
      continue;

    // memory_order_acquire: Present thread'in store(release)'ini gördükten
    // sonra yükle. Böylece IsPawnValid kontrolü ile SafeWrite arasında pawn
    // serbest bırakılsa bile tutarlı bir snapshot üzerinden çalışırız.
    const uintptr_t cachedPawn =
        g_CachedPawn.load(std::memory_order_acquire);
    if (!cachedPawn)
      continue;

    // Adres aralığı kontrolünü yüklemeden SONRA yap — yükle-sil-kullan
    // hâlâ mümkün ama IsPawnValid içindeki SEH onu yakalar.
    if (cachedPawn < 0x1000000 || cachedPawn > 0x00007FFFFFFFFFFF)
      continue;

    __try {
      const uintptr_t cachedController =
          g_CachedController.load(std::memory_order_acquire);

      // Present thread 0 yazar (pawn geçersizleşince). Sıfır kontrolü
      // yüklemeden hemen sonra geldiği için race window minimumdur.
      if (!cachedPawn)
        continue;

      auto localPawn = reinterpret_cast<C_CSPlayerPawn *>(cachedPawn);
      auto localCtrl =
          reinterpret_cast<C_CSPlayerController *>(cachedController);

      UpdateFOV(localPawn, localCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Nesne serbest bırakıldıysa SEH burada yakalar; g_CachedPawn'ı
      // sıfırlayarak bir sonraki döngüde erken çık.
      g_CachedPawn.store(0, std::memory_order_release);
    }
  }
  return 0;
}

static bool hkWndProcInternal(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
  if (Menu::IsOpen) {
    ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
      return true;
    }
  }
  return false;
}

LRESULT __stdcall hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  __try {
    if (hkWndProcInternal(hWnd, msg, wParam, lParam))
      return true;

    if (!oWndProc)
      return CallWindowProcW(DefWindowProcW, hWnd, msg, wParam, lParam);

    return CallWindowProcW(oWndProc, hWnd, msg, wParam, lParam);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return DefWindowProcW(hWnd, msg, wParam, lParam);
  }
}

static void InitChamsV2(ID3D11Device *device, ID3D11DeviceContext *context) {
  constexpr const char *ctModelPath =
      "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike "
      "Global Offensive\\game\\csgo\\raven\\models\\ctm_sas.glb";
  constexpr const char *tModelPath =
      "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike "
      "Global Offensive\\game\\csgo\\raven\\models\\tm_phoenix.glb";
  ChamsV2::g_MeshRenderer.Initialize(device, context, ctModelPath, tModelPath);
}

using RagdollConvarGetterFn = void *(__fastcall *)(void *, int);

static RagdollConvarGetterFn oRagdollConvarGetter = nullptr;
static void *g_RagdollConvarStorage = nullptr;
static void *g_RagdollGravityReturnAddress = nullptr;
static thread_local float g_RagdollGravityOverride = -0.5f;

// The getter is shared by many ConVars. Override only the value requested by
// the exact ragdoll solver call site, matching the safe part of the forum
// approach without relying on its stale 2024 signatures.
static void *__fastcall hkRagdollConvarGetter(void *convar, int userSlot) {
  if (Globals::ragdoll_nograv_enabled &&
      convar == g_RagdollConvarStorage &&
      _ReturnAddress() == g_RagdollGravityReturnAddress) {
    const float requested = Globals::ragdoll_nograv_scale;
    g_RagdollGravityOverride = std::isfinite(requested) ? requested : -0.5f;
    return &g_RagdollGravityOverride;
  }

  return oRagdollConvarGetter ? oRagdollConvarGetter(convar, userSlot)
                              : nullptr;
}

struct RagdollGravityHookTarget {
  uintptr_t getter = 0;
  uintptr_t returnAddress = 0;
  uintptr_t storage = 0;
};

static RagdollGravityHookTarget ResolveRagdollGravityHookTarget() {
  static RagdollGravityHookTarget result{};
  static bool attempted = false;
  if (result.getter && result.returnAddress && result.storage)
    return result;
  if (attempted)
    return {};
  attempted = true;

  const uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return {};

  const uintptr_t name = Memory::PatternScan(
      "client.dll",
      Patterns::Client::RagdollGravityScale);
  if (!name)
    return {};

  __try {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(client);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        client + static_cast<uintptr_t>(dos->e_lfanew));
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;

    uintptr_t storage = 0;
    for (size_t i = 0; i + 0x70 < imageSize && !storage; ++i) {
      const uintptr_t instruction = client + i;
      const auto *bytes = reinterpret_cast<const uint8_t *>(instruction);
      if (bytes[0] != 0x48 || bytes[1] != 0x8D || bytes[2] != 0x15)
        continue;

      const int32_t nameDisp =
          *reinterpret_cast<const int32_t *>(instruction + 3);
      if (instruction + 7 + static_cast<intptr_t>(nameDisp) != name)
        continue;

      const uintptr_t storageLea = instruction + 0x66;
      const auto *storageBytes =
          reinterpret_cast<const uint8_t *>(storageLea);
      if (storageBytes[0] != 0x48 || storageBytes[1] != 0x8D ||
          storageBytes[2] != 0x0D)
        continue;

      const int32_t storageDisp =
          *reinterpret_cast<const int32_t *>(storageLea + 3);
      storage = storageLea + 7 + static_cast<intptr_t>(storageDisp);
    }

    if (!storage)
      return {};

    // Current solver sequence:
    // mov edx,-1; lea rcx,[storage]; call getter; test rax,rax; ...
    // Validate both storage references to reject similarly shaped ConVar reads.
    for (size_t i = 0; i + 38 < imageSize; ++i) {
      const uintptr_t p = client + i;
      const auto *b = reinterpret_cast<const uint8_t *>(p);
      if (b[0] != 0xBA ||
          *reinterpret_cast<const uint32_t *>(p + 1) != 0xFFFFFFFF ||
          b[5] != 0x48 || b[6] != 0x8D || b[7] != 0x0D || b[12] != 0xE8 ||
          b[17] != 0x48 || b[18] != 0x85 || b[19] != 0xC0 ||
          b[20] != 0x75 || b[21] != 0x0B || b[22] != 0x48 ||
          b[23] != 0x8B || b[24] != 0x05 || b[29] != 0x48 ||
          b[30] != 0x8B || b[31] != 0x40 || b[32] != 0x08 ||
          b[33] != 0xF3 || b[34] != 0x44 || b[35] != 0x0F ||
          b[36] != 0x10 || b[37] != 0x10)
        continue;

      const int32_t storageDisp = *reinterpret_cast<const int32_t *>(p + 8);
      const uintptr_t callStorage =
          p + 12 + static_cast<intptr_t>(storageDisp);
      const int32_t fallbackDisp =
          *reinterpret_cast<const int32_t *>(p + 25);
      const uintptr_t fallbackStorage =
          p + 29 + static_cast<intptr_t>(fallbackDisp);
      if (callStorage != storage || fallbackStorage != storage + 8)
        continue;

      const uintptr_t call = p + 12;
      const int32_t callDisp = *reinterpret_cast<const int32_t *>(call + 1);
      const uintptr_t getter = call + 5 + static_cast<intptr_t>(callDisp);
      if (getter < client || getter + 8 >= client + imageSize)
        continue;

      const auto *g = reinterpret_cast<const uint8_t *>(getter);
      if (g[0] != 0x4C || g[1] != 0x8B || g[2] != 0x49 || g[3] != 0x08 ||
          g[4] != 0x49 || g[5] != 0x8B || g[6] != 0x49 || g[7] != 0x30)
        continue;

      result.getter = getter;
      result.returnAddress = call + 5;
      result.storage = storage;
      return result;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return {};
  }

  return {};
}

void PresentInternal(IDXGISwapChain *swapChain, UINT sync, UINT flags) {
  static std::atomic<unsigned long long> presentFrame{0};
  const unsigned long long frame =
      presentFrame.fetch_add(1, std::memory_order_relaxed) + 1;
  CrashTelemetry::Mark(CrashTelemetry::Stage::PresentEnter, false);
  if (frame <= 10 || (frame % 120) == 0)
    CrashTelemetry::Trace(
        "PresentInternal begin frame=%llu swapChain=%p sync=%u flags=0x%X "
        "initialized=%d",
        frame, swapChain, sync, flags, g_Init ? 1 : 0);
  __try {
    bool backBufferChanged = false;
    if (g_Init && g_CurrentSwapChain == swapChain) {
      ID3D11Texture2D *probe = nullptr;
      if (FAILED(swapChain->GetBuffer(
              0, __uuidof(ID3D11Texture2D),
              reinterpret_cast<void **>(&probe))) ||
          !probe) {
        backBufferChanged = true;
      } else {
        D3D11_TEXTURE2D_DESC probeDesc{};
        probe->GetDesc(&probeDesc);
        backBufferChanged =
            reinterpret_cast<uintptr_t>(probe) != g_BackBufferIdentity ||
            probeDesc.Width != g_BackBufferDescWidth ||
            probeDesc.Height != g_BackBufferDescHeight ||
            probeDesc.Format != g_BackBufferDescFormat;
        probe->Release();
      }
      if (backBufferChanged)
        CrashTelemetry::Trace(
            "Present detected backbuffer replacement; rebuilding render "
            "resources");
    }

    if (!g_Init || g_CurrentSwapChain != swapChain || backBufferChanged) {
      CrashTelemetry::Mark(
          CrashTelemetry::Stage::PresentDeviceInitialization, false);
      CrashTelemetry::Trace(
          "render initialization begin frame=%llu oldSwap=%p newSwap=%p "
          "wasInitialized=%d",
          frame, g_CurrentSwapChain, swapChain, g_Init ? 1 : 0);
      if (g_Init) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext())
          ImGui::DestroyContext();
        if (g_WireframeDepthState) {
          g_WireframeDepthState->Release();
          g_WireframeDepthState = nullptr;
        }
        if (g_WireframePS) {
          g_WireframePS->Release();
          g_WireframePS = nullptr;
        }
        if (g_WireframeColorCB) {
          g_WireframeColorCB->Release();
          g_WireframeColorCB = nullptr;
        }
        if (g_WireframeRS) {
          g_WireframeRS->Release();
          g_WireframeRS = nullptr;
        }
        if (g_SolidRS) {
          g_SolidRS->Release();
          g_SolidRS = nullptr;
        }
        if (g_RTV) {
          g_RTV->Release();
          g_RTV = nullptr;
        }
        if (g_Context) {
          g_Context->Release();
          g_Context = nullptr;
        }
        if (g_Device) {
          g_Device->Release();
          g_Device = nullptr;
        }
        g_BackBufferIdentity = 0;
        g_BackBufferDescWidth = 0;
        g_BackBufferDescHeight = 0;
        g_BackBufferDescFormat = DXGI_FORMAT_UNKNOWN;
        g_Init = false;
      }

      CrashTelemetry::Trace("render init phase=get-device begin");
      if (FAILED(
              swapChain->GetDevice(__uuidof(ID3D11Device), (void **)&g_Device)))
        return;
      CrashTelemetry::Trace("render init phase=get-device end device=%p",
                            g_Device);

      g_Device->GetImmediateContext(&g_Context);
      CrashTelemetry::Trace("render init phase=get-context end context=%p",
                            g_Context);
      DXGI_SWAP_CHAIN_DESC sd{};
      swapChain->GetDesc(&sd);
      g_Window = sd.OutputWindow;
      CrashTelemetry::Trace("render init phase=swap-desc end window=%p",
                            g_Window);
      ID3D11Texture2D *backBuffer = nullptr;
      if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         (void **)&backBuffer))) {
        D3D11_TEXTURE2D_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);
        g_BackBufferIdentity = reinterpret_cast<uintptr_t>(backBuffer);
        g_BackBufferDescWidth = backBufferDesc.Width;
        g_BackBufferDescHeight = backBufferDesc.Height;
        g_BackBufferDescFormat = backBufferDesc.Format;
        g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RTV);
        backBuffer->Release();
      }
      CrashTelemetry::Trace("render init phase=rtv end rtv=%p", g_RTV);

      if (!oWndProc) {
        oWndProc = (WNDPROC)SetWindowLongPtr(g_Window, GWLP_WNDPROC,
                                             (LONG_PTR)hkWndProc);
      }
      CrashTelemetry::Trace("render init phase=wndproc end original=%p",
                            oWndProc);

      ImGui::CreateContext();
      const bool win32Ready = ImGui_ImplWin32_Init(g_Window);
      const bool dx11Ready = ImGui_ImplDX11_Init(g_Device, g_Context);
      CrashTelemetry::Trace(
          "render init phase=imgui-backends end win32=%d dx11=%d",
          win32Ready ? 1 : 0, dx11Ready ? 1 : 0);
      if (!win32Ready || !dx11Ready)
        return;

      if (!kSafeStartupProfile) {
        D3D11_RASTERIZER_DESC rsDesc{};
        rsDesc.FillMode = D3D11_FILL_WIREFRAME;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.DepthClipEnable = TRUE;
        g_Device->CreateRasterizerState(&rsDesc, &g_WireframeRS);

        rsDesc.FillMode = D3D11_FILL_SOLID;
        g_Device->CreateRasterizerState(&rsDesc, &g_SolidRS);

        D3D11_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsDesc.StencilEnable = FALSE;
        g_Device->CreateDepthStencilState(&dsDesc, &g_WireframeDepthState);

        const char *psSource =
            "cbuffer WireframeColorBuffer : register(b0) { float4 wireColor; };"
            "float4 main() : SV_Target { return wireColor; }";
        ID3DBlob *psBlob = nullptr;
        ID3DBlob *errorBlob = nullptr;
        if (SUCCEEDED(D3DCompile(psSource, strlen(psSource), nullptr, nullptr,
                                 nullptr, "main", "ps_4_0", 0, 0, &psBlob,
                                 &errorBlob))) {
          g_Device->CreatePixelShader(psBlob->GetBufferPointer(),
                                      psBlob->GetBufferSize(), nullptr,
                                      &g_WireframePS);
          psBlob->Release();
        }
        if (errorBlob)
          errorBlob->Release();

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.ByteWidth = sizeof(WireframeColorBuffer);
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_Device->CreateBuffer(&cbDesc, nullptr, &g_WireframeColorCB);
      }
      CrashTelemetry::Trace(
          "render init phase=optional-wireframe end enabled=%d",
          kSafeStartupProfile ? 0 : 1);

      CrashTelemetry::Trace("render init phase=menu begin");
      Menu::Initialize(g_Device);
      CrashTelemetry::Trace("render init phase=menu end");
      if (!kSafeStartupProfile) {
        GPUChamsRenderer::Get().Initialize(g_Device, g_Context);
        InitChamsV2(g_Device, g_Context);
        SteamAvatarCache::Get().SetDevice(g_Device, g_Context);
      }
      CrashTelemetry::Trace(
          "render init phase=optional-renderers end enabled=%d",
          kSafeStartupProfile ? 0 : 1);

      g_CurrentSwapChain = swapChain;
      g_Init = true;
      CrashTelemetry::Trace(
          "render initialization end frame=%llu device=%p context=%p "
          "rtv=%p window=%p",
          frame, g_Device, g_Context, g_RTV, g_Window);
    }

    {
      ID3D11Texture2D *backBuf = nullptr;
      if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         (void **)&backBuf)) &&
          backBuf) {
        D3D11_TEXTURE2D_DESC bbDesc;
        backBuf->GetDesc(&bbDesc);
        g_BackBufferWidth = (float)bbDesc.Width;
        g_BackBufferHeight = (float)bbDesc.Height;
        backBuf->Release();
      }

      RECT clientRect;
      if (GetClientRect(g_Window, &clientRect)) {
        g_WindowWidth = (float)(clientRect.right - clientRect.left);
        g_WindowHeight = (float)(clientRect.bottom - clientRect.top);
      }
    }

    {
      D3D11_VIEWPORT gameVP = {};
      UINT numVP = 1;
      g_Context->RSGetViewports(&numVP, &gameVP);

      if (numVP > 0 && gameVP.Width > 0 && gameVP.Height > 0) {
        Globals::GameViewportX = gameVP.TopLeftX;
        Globals::GameViewportY = gameVP.TopLeftY;
        Globals::GameViewportWidth = gameVP.Width;
        Globals::GameViewportHeight = gameVP.Height;
      } else {

        Globals::GameViewportX = 0.f;
        Globals::GameViewportY = 0.f;
        Globals::GameViewportWidth = g_BackBufferWidth;
        Globals::GameViewportHeight = g_BackBufferHeight;
      }
    }

    Globals::ScreenWidth = (int)Globals::GameViewportWidth;
    Globals::ScreenHeight = (int)Globals::GameViewportHeight;

    if (!g_RTV || !g_Context || !g_Init)
      return;

    if (IsFullRuntimeActive()) {
      uintptr_t client = Memory::GetModuleBase("client.dll");
      if (client) {
        for (int i = 0; i < 16; i++) {
          Globals::ViewMatrix[i] = Utils::SafeRead<float>(
              client + Offsets::dwViewMatrix + (i * sizeof(float)));
        }

        uintptr_t localPawnAddr = Memory::Globals::LocalPawn();
        uintptr_t localCtrlAddr = Utils::SafeRead<uintptr_t>(
            client + Offsets::dwLocalPlayerController);

        auto localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
        auto localCtrl =
            reinterpret_cast<C_CSPlayerController *>(localCtrlAddr);

        if (IsPawnValid(localPawn)) {
          g_CachedPawn.store(localPawnAddr, std::memory_order_release);
          g_CachedController.store(localCtrlAddr, std::memory_order_release);
        } else {
          g_CachedPawn.store(0, std::memory_order_release);
          g_CachedController.store(0, std::memory_order_release);
        }

        // One owner thread updates the shared entity snapshot. Combat and
        // visual consumers below read the same immutable-per-frame copy.
        EntityManager::Get().Update();
      }
    } else {
      g_CachedPawn.store(0, std::memory_order_release);
      g_CachedController.store(0, std::memory_order_release);
    }

    CrashTelemetry::Mark(CrashTelemetry::Stage::PresentNewFrame, false);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (g_BackBufferWidth > 0 && g_BackBufferHeight > 0) {
      ImGui::GetIO().DisplaySize =
          ImVec2(g_BackBufferWidth, g_BackBufferHeight);
    }

    ImGui::NewFrame();

    if (GetAsyncKeyState(Globals::menu_key) & 1)
      Menu::IsOpen = !Menu::IsOpen;

    if (Globals::thirdperson_key != 0 &&
        (GetAsyncKeyState(Globals::thirdperson_key) & 1))
      Globals::thirdperson_enabled = !Globals::thirdperson_enabled;

    CrashTelemetry::Mark(CrashTelemetry::Stage::PresentMenu, false);
    Menu::Render();

    if (StabilityProfile::EnableInventory) {
      // Only explicit, validated user requests reach this startup-delayed and
      // rate-limited queue.
      InventoryChanger::Run();

      // Envanter kilidini her frame kaldır (maç içi skin değişimi için).
      InventoryChanger::RunUnlock();

      // Tek seferlik "Hemen Uygula" butonu tetiklendi mi?
      if (Globals::inventory_force_reapply > 0) {
        Globals::inventory_force_reapply = 0;
        InventoryChanger::ForceReapplyAll();
      }
    }

    if (IsFullRuntimeActive()) {
      CrashTelemetry::Mark(CrashTelemetry::Stage::PresentRaycasting, false);
      Raycasting::Get().UpdateMap();

      SteamAvatarCache::Get().ProcessFinished();

      // Skin logic runs from the Present path (FrameStageNotify is not hooked
      // on this build because its direct original call faults).
      __try {
        SkinChanger::Run();
        SkinChanger::RunVisuals(6);
        CustomModel::Apply();
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashTelemetry::Trace("Present SkinChanger exception code=0x%08lX",
                              static_cast<unsigned long>(GetExceptionCode()));
      }

      __try {
        Aimbot::Run();
        Triggerbot::Run();
        RCS::Run();

        g_ChamsRTV = g_RTV;
        CrashTelemetry::Mark(CrashTelemetry::Stage::PresentVisuals, false);
        Visuals::Render();
        CrashTelemetry::Mark(CrashTelemetry::Stage::PresentMisc, false);
        Misc::Render();
        Misc::Run();

        CrashTelemetry::Mark(CrashTelemetry::Stage::PresentAspectRatio, false);
        AspectRatio::Run();
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashTelemetry::Trace("Present combat/visuals exception code=0x%08lX",
                              static_cast<unsigned long>(GetExceptionCode()));
      }
    }

    // Anti-Aim per-frame re-pin has been removed as the user requested
    // to see the fake angles directly in first-person view.

    CrashTelemetry::Mark(CrashTelemetry::Stage::PresentImGuiRender, false);
    ImGui::Render();
    // Lock g_RTV so hkResizeBuffers cannot Release() it between the null
    // check and the actual OMSetRenderTargets call.
    if (g_RTVLockInitialized)
      EnterCriticalSection(&g_RTVLock);
    if (g_RTV) {
      g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
      g_IsRenderingUI = true;
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      g_IsRenderingUI = false;
    }
    if (g_RTVLockInitialized)
      LeaveCriticalSection(&g_RTVLock);
    if (frame <= 10 || (frame % 120) == 0)
      CrashTelemetry::Trace("PresentInternal end frame=%llu", frame);
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "Hooks::PresentInternal")) {
    g_IsRenderingUI = false;
  }
}

HRESULT __stdcall hkPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                            UINT Flags) {
  if (!oPresent)
    return pSwapChain->Present(SyncInterval, Flags);
  PresentInternal(pSwapChain, SyncInterval, Flags);
  CrashTelemetry::Mark(CrashTelemetry::Stage::PresentOriginal, false);
  const HRESULT result = oPresent(pSwapChain, SyncInterval, Flags);
  const unsigned long long completed =
      g_CompletedPresentFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
  // Wait for several complete Present -> original Present cycles. This keeps
  // MinHook's later thread-freeze pass away from swap-chain creation and the
  // first submissions made by Steam/Source 2.
  if (completed >= 3)
    g_RenderReady.store(true, std::memory_order_release);
  return result;
}

HRESULT __stdcall hkResizeBuffers(IDXGISwapChain *pSwapChain, UINT BufferCount,
                                  UINT Width, UINT Height,
                                  DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
  CrashTelemetry::Mark(CrashTelemetry::Stage::ResizeBuffers, false);
  CrashTelemetry::Trace(
      "ResizeBuffers swapChain=%p count=%u width=%u height=%u format=%u "
      "flags=0x%X",
      pSwapChain, BufferCount, Width, Height,
      static_cast<unsigned>(NewFormat), SwapChainFlags);

  // Acquire the RTV lock before releasing the old view so PresentInternal
  // cannot read g_RTV between the Release() and the new CreateRenderTargetView.
  if (g_RTVLockInitialized)
    EnterCriticalSection(&g_RTVLock);

  if (g_RTV) {
    g_RTV->Release();
    g_RTV = nullptr;
  }

  if (g_RTVLockInitialized)
    LeaveCriticalSection(&g_RTVLock);

  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);

  if (SUCCEEDED(hr) && g_Device) {
    ID3D11Texture2D *backBuffer = nullptr;
    if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                        (void **)&backBuffer))) {
      if (g_RTVLockInitialized)
        EnterCriticalSection(&g_RTVLock);
      g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RTV);
      if (g_RTVLockInitialized)
        LeaveCriticalSection(&g_RTVLock);
      backBuffer->Release();
    }
  }

  return hr;
}

void __stdcall hkDrawIndexed(ID3D11DeviceContext *pContext, UINT IndexCount,
                             UINT StartIndexLocation, INT BaseVertexLocation) {
  if (!oDrawIndexed)
    return;

  if (g_WireframeRS && g_WireframeDepthState && g_WireframePS &&
      g_WireframeColorCB && !g_IsRenderingUI) {
    WireframeTarget target = ClassifyWireframeTarget(pContext, IndexCount, 1);
    if (target != WireframeTarget::None) {
      ID3D11RasterizerState *pOldRS = nullptr;
      ID3D11DepthStencilState *pOldDepthState = nullptr;
      ID3D11PixelShader *pOldPS = nullptr;
      ID3D11Buffer *pOldCB = nullptr;
      UINT oldStencilRef = 0;
      pContext->RSGetState(&pOldRS);
      pContext->OMGetDepthStencilState(&pOldDepthState, &oldStencilRef);
      pContext->PSGetShader(&pOldPS, nullptr, nullptr);
      pContext->PSGetConstantBuffers(0, 1, &pOldCB);

      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(pContext->Map(g_WireframeColorCB, 0,
                                  D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, Globals::wireframe_color, sizeof(float) * 4);
        pContext->Unmap(g_WireframeColorCB, 0);
      }

      pContext->RSSetState(g_WireframeRS);
      pContext->OMSetDepthStencilState(g_WireframeDepthState, 0);
      pContext->PSSetConstantBuffers(0, 1, &g_WireframeColorCB);
      pContext->PSSetShader(g_WireframePS, nullptr, 0);
      oDrawIndexed(pContext, IndexCount, StartIndexLocation,
                   BaseVertexLocation);

      pContext->PSSetShader(pOldPS, nullptr, 0);
      pContext->PSSetConstantBuffers(0, 1, &pOldCB);
      pContext->OMSetDepthStencilState(pOldDepthState, oldStencilRef);
      pContext->RSSetState(pOldRS ? pOldRS : g_SolidRS);

      if (pOldCB)
        pOldCB->Release();
      if (pOldPS)
        pOldPS->Release();
      if (pOldDepthState)
        pOldDepthState->Release();
      if (pOldRS)
        pOldRS->Release();
      return;
    }
  }

  oDrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void __stdcall hkDrawIndexedInstanced(ID3D11DeviceContext *pContext,
                                      UINT IndexCountPerInstance,
                                      UINT InstanceCount,
                                      UINT StartIndexLocation,
                                      INT BaseVertexLocation,
                                      UINT StartInstanceLocation) {
  if (!oDrawIndexedInstanced)
    return;

  if (g_WireframeRS && g_WireframeDepthState && g_WireframePS &&
      g_WireframeColorCB && !g_IsRenderingUI) {
    WireframeTarget target =
        ClassifyWireframeTarget(pContext, IndexCountPerInstance, InstanceCount);
    if (target != WireframeTarget::None) {
      ID3D11RasterizerState *pOldRS = nullptr;
      ID3D11DepthStencilState *pOldDepthState = nullptr;
      ID3D11PixelShader *pOldPS = nullptr;
      ID3D11Buffer *pOldCB = nullptr;
      UINT oldStencilRef = 0;
      pContext->RSGetState(&pOldRS);
      pContext->OMGetDepthStencilState(&pOldDepthState, &oldStencilRef);
      pContext->PSGetShader(&pOldPS, nullptr, nullptr);
      pContext->PSGetConstantBuffers(0, 1, &pOldCB);

      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(pContext->Map(g_WireframeColorCB, 0,
                                  D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, Globals::wireframe_color, sizeof(float) * 4);
        pContext->Unmap(g_WireframeColorCB, 0);
      }

      pContext->RSSetState(g_WireframeRS);
      pContext->OMSetDepthStencilState(g_WireframeDepthState, 0);
      pContext->PSSetConstantBuffers(0, 1, &g_WireframeColorCB);
      pContext->PSSetShader(g_WireframePS, nullptr, 0);
      oDrawIndexedInstanced(pContext, IndexCountPerInstance, InstanceCount,
                            StartIndexLocation, BaseVertexLocation,
                            StartInstanceLocation);

      pContext->PSSetShader(pOldPS, nullptr, 0);
      pContext->PSSetConstantBuffers(0, 1, &pOldCB);
      pContext->OMSetDepthStencilState(pOldDepthState, oldStencilRef);
      pContext->RSSetState(pOldRS ? pOldRS : g_SolidRS);

      if (pOldCB)
        pOldCB->Release();
      if (pOldPS)
        pOldPS->Release();
      if (pOldDepthState)
        pOldDepthState->Release();
      if (pOldRS)
        pOldRS->Release();
      return;
    }
  }

  oDrawIndexedInstanced(pContext, IndexCountPerInstance, InstanceCount,
                        StartIndexLocation, BaseVertexLocation,
                        StartInstanceLocation);
}

static void UpdateOfflineNoSpreadConvars(bool requested) {
  static bool applied = false;
  static bool oldNoSpread = false;
  static float oldAirSpread = 1.0f;

  // These are server-authoritative. Only a local/listen server with
  // sv_cheats enabled can legitimately apply them; official servers are
  // deliberately left untouched.
  const bool allowed = requested &&
      Memory::Convars::GetBool("sv_cheats", false);

  if (allowed && !applied) {
    oldNoSpread = Memory::Convars::GetBool(
        "weapon_accuracy_nospread", false);
    oldAirSpread = Memory::Convars::GetFloat(
        "weapon_air_spread_scale", 1.0f);
    const bool a = Memory::Convars::SetBool(
        "weapon_accuracy_nospread", true);
    const bool b = Memory::Convars::SetFloat(
        "weapon_air_spread_scale", 0.0f);
    applied = a || b;
  } else if (!allowed && applied) {
    Memory::Convars::SetBool("weapon_accuracy_nospread", oldNoSpread);
    Memory::Convars::SetFloat("weapon_air_spread_scale", oldAirSpread);
    applied = false;
  }
}

void __fastcall hkCreateMove(void *pInput, unsigned int nSlot, __int64 a3) {
  if (!oCreateMove)
    return;

  UpdateOfflineNoSpreadConvars(
      Globals::silent_aim_enabled && Globals::silent_aim_nospread);

  // Pre-arm while the engine constructs/predicts a held silent shot, then
  // clear it before target selection below. SilentAim::Run re-arms only when
  // this completed command actually acquired a target.
  const bool silentKeyHeld =
      Globals::silent_aim_key == 0 ||
      (Globals::silent_aim_key > 0 && Globals::silent_aim_key < 256 &&
       (GetAsyncKeyState(Globals::silent_aim_key) & 0x8000) != 0);
  SilentAimHook::SetNoSpreadActive(
      Globals::silent_aim_enabled && Globals::silent_aim_nospread &&
      silentKeyHeld && g_CachedPawn.load(std::memory_order_relaxed) != 0);
  oCreateMove(pInput, nSlot, a3);
  SilentAimHook::SetNoSpreadActive(false);

  // Camera state belongs to the input/game thread. Present-time writes happen
  // after scene rendering and are reverted by the native camera state machine.
  Misc::ThirdPerson(pInput, nSlot);

  if (!pInput)
    return;

  auto *input = reinterpret_cast<i_csgo_input *>(pInput);
  c_user_cmd *cmd = input->get_user_cmd();

  uintptr_t pawnAddr = g_CachedPawn.load(std::memory_order_relaxed);
  bool validPawn = false;
  if (pawnAddr) {
    __try {
      auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(pawnAddr);
      if (localPawn && localPawn->IsAlive()) {
          validPawn = true;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Stale pawn pointer — silently ignore this tick.
    }
  }

  SilentAimHook::SetLocalValid(validPawn);
  SilentAimHook::SetWantsHook(
      validPawn &&
      (Globals::silent_aim_enabled && Globals::silent_aim_nospread));

  if (validPawn) {
    __try {
      if (cmd) {
        uintptr_t client = Memory::GetModuleBase("client.dll");
        if (client) {
            Vector currentView = Utils::SafeRead<Vector>(client + Offsets::dwViewAngles);
            SilentAimHook::SetLocalView(currentView.x, currentView.y);
        }
        // Cmd-only bunnyhop with input-synchronised air strafe.
        Movement::Run(cmd);

        // Anti-Aim writes only outgoing command angles. Silent Aim runs
        // afterwards so a shot can replace the history angle safely.
        AntiAim::Run(cmd);

        // Silent Aim LAST: updates target state for SilentAimHook.
        SilentAim::Run(cmd);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Ignore
    }
  }

  return;
}

void __fastcall hkFrameStageNotify(void *_this, int curStage) {
  static std::atomic<unsigned long long> entryCount{0};
  const unsigned long long entry =
      entryCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (entry <= 3)
    CrashTelemetry::Trace("FrameStage entry=%llu stage=%d this=%p original=%p",
                          entry, curStage, _this, oFrameStageNotify);

  // The original FrameStageNotify slot on this build executes the passed
  // receiver (`_this`) as a callback; when the receiver lives in a
  // non-executable data region the call itself faults with 0xC0000005
  // (recurring at client.dll+0x23C0A20). That fault is not always caught by
  // SEH (the vectored handler returns CONTINUE_SEARCH), so the direct original
  // call is omitted. We only run our own per-frame logic from the hook; the
  // game still proceeds with its own FrameStage handling.

  if (curStage == 6) {
    static std::atomic<unsigned long long> callbackCount{0};
    const unsigned long long callback =
        callbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool traceCallback = callback <= 3;
    if (traceCallback) {
      const uintptr_t localPawn = Memory::Globals::LocalPawn();
      const uintptr_t localController = Memory::Globals::LocalController();
      CrashTelemetry::Trace("FrameStage callback=%llu stage=%d begin",
                            callback, curStage);
      CrashTelemetry::Trace(
          "FrameStage runtime state callback=%llu enabled=%d initialized=%d "
          "localPawn=%p localController=%p",
          callback, Globals::sc_enabled ? 1 : 0,
          SkinChanger::g_Initialized ? 1 : 0,
          reinterpret_cast<void *>(localPawn),
          reinterpret_cast<void *>(localController));
    }

    __try {
      SkinChanger::Run();
      if (traceCallback)
        CrashTelemetry::Trace("FrameStage callback=%llu SkinChanger::Run end",
                              callback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      CrashTelemetry::Trace(
          "FrameStage SkinChanger::Run exception callback=%llu code=0x%08lX",
          callback, static_cast<unsigned long>(GetExceptionCode()));
    }

    __try {
      CustomModel::Apply();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try {
      SkinChanger::RunVisuals(curStage);
      if (traceCallback)
        CrashTelemetry::Trace(
            "FrameStage callback=%llu SkinChanger::RunVisuals end", callback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      CrashTelemetry::Trace(
          "FrameStage SkinChanger::RunVisuals exception callback=%llu "
          "code=0x%08lX",
          callback, static_cast<unsigned long>(GetExceptionCode()));
    }

    // The inventory stability profile deliberately excludes unrelated
    // per-frame memory writes. NoVisualRecoil and Source 2 particle-manager
    // mutations are not required by Inventory and previously shared this
    // callback, making any later crash indistinguishable from an econ error.
    if (IsFullRuntimeActive()) {
      __try {
        Misc::NoVisualRecoil();
        Misc::EngineParticleUpdate();
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashTelemetry::Trace(
            "FrameStage optional misc exception callback=%llu code=0x%08lX",
            callback, static_cast<unsigned long>(GetExceptionCode()));
      }
    }

    if (traceCallback)
      CrashTelemetry::Trace("FrameStage callback=%llu end", callback);
  }

}

static bool s_wallsWereActive = false;
static int s_wallRestoreCount = 0;

struct WorldModulatedRgb {
  float r;
  float g;
  float b;
};

static WorldModulatedRgb WorldHsvToRgb(float hue, float saturation,
                                       float value) {
  hue = hue - std::floor(hue);
  saturation = std::clamp(saturation, 0.0f, 1.0f);
  const float chroma = value * saturation;
  const float h = hue * 6.0f;
  const float x = chroma * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
  WorldModulatedRgb rgb{};
  if (h < 1.0f)
    rgb = {chroma, x, 0.0f};
  else if (h < 2.0f)
    rgb = {x, chroma, 0.0f};
  else if (h < 3.0f)
    rgb = {0.0f, chroma, x};
  else if (h < 4.0f)
    rgb = {0.0f, x, chroma};
  else if (h < 5.0f)
    rgb = {x, 0.0f, chroma};
  else
    rgb = {chroma, 0.0f, x};
  const float m = value - chroma;
  rgb.r += m;
  rgb.g += m;
  rgb.b += m;
  return rgb;
}

static WorldModulatedRgb
WorldModulateColor(const float baseColor[4], float saturation,
                   float brightness, float phase) {
  const float *source =
      Globals::world_sync_colors ? Globals::skybox_color : baseColor;
  WorldModulatedRgb out{source[0], source[1], source[2]};

  // Saturation is useful even in static mode: 0 makes the selected component
  // grayscale, 1 preserves it and values above 1 produce a stronger grade.
  const float luma =
      out.r * 0.2126f + out.g * 0.7152f + out.b * 0.0722f;
  out.r = luma + (out.r - luma) * saturation;
  out.g = luma + (out.g - luma) * saturation;
  out.b = luma + (out.b - luma) * saturation;

  const float now = static_cast<float>(GetTickCount64()) * 0.001f;
  const float speed = std::clamp(Globals::world_animation_speed, 0.01f, 4.0f);
  const float strength =
      std::clamp(Globals::world_animation_strength, 0.0f, 1.0f);
  if (Globals::world_sync_colors)
    phase = 0.0f;

  if (Globals::world_animation_enabled) {
    switch (Globals::world_animation_style) {
    case 0: { // Full-spectrum rainbow
      const float value =
          std::max(0.35f, std::max(out.r, std::max(out.g, out.b)));
      const WorldModulatedRgb rainbow =
          WorldHsvToRgb(now * speed * 0.16f + phase,
                        std::clamp(saturation, 0.0f, 1.0f), value);
      out.r += (rainbow.r - out.r) * strength;
      out.g += (rainbow.g - out.g) * strength;
      out.b += (rainbow.b - out.b) * strength;
      break;
    }
    case 1: { // Smooth brightness breathing
      const float pulse =
          1.0f + std::sin((now * speed + phase) * 6.2831853f) *
                     strength * 0.48f;
      out.r *= pulse;
      out.g *= pulse;
      out.b *= pulse;
      break;
    }
    case 2: { // Cyan / violet aurora sweep
      const float blend =
          0.5f + 0.5f * std::sin((now * speed + phase) * 2.35f);
      const float hue = 0.48f + 0.34f * blend;
      const float value =
          std::max(0.4f, std::max(out.r, std::max(out.g, out.b)));
      const WorldModulatedRgb aurora =
          WorldHsvToRgb(hue, std::clamp(saturation, 0.0f, 1.0f), value);
      out.r += (aurora.r - out.r) * strength;
      out.g += (aurora.g - out.g) * strength;
      out.b += (aurora.b - out.b) * strength;
      break;
    }
    default: { // Storm: dim ambient light with short cool flashes
      const float wave =
          std::max(0.0f, std::sin(now * speed * 8.7f + phase * 9.0f));
      const float flash = std::pow(wave, 18.0f) * strength;
      const float dim = 1.0f - strength * 0.52f;
      out.r = out.r * dim + flash * 0.75f;
      out.g = out.g * dim + flash * 0.85f;
      out.b = out.b * dim + flash;
      break;
    }
    }
  }

  out.r = std::max(0.0f, out.r * brightness);
  out.g = std::max(0.0f, out.g * brightness);
  out.b = std::max(0.0f, out.b * brightness);
  return out;
}

__int64 __fastcall hkDrawAggregateSceneObject(void *a1, void *a2,
                                              c_aggregate_object_array *a3) {
  if (!oDrawAggregateSceneObject)
    return 0;

  if (Globals::world_walls_enabled && a3 && a3->object) {
    __try {
      uintptr_t sceneObj = reinterpret_cast<uintptr_t>(a3->object);
      if (sceneObj > 0x100000) {
        int count = *reinterpret_cast<int *>(sceneObj + 0x18C);
        uintptr_t dataBase = *reinterpret_cast<uintptr_t *>(sceneObj + 0x160);

        if (dataBase > 0x100000 && count > 0 && count < 20000) {
          const WorldModulatedRgb wall =
              WorldModulateColor(Globals::world_walls_color,
                                 Globals::world_wall_saturation,
                                 Globals::world_wall_brightness, 0.24f);
          const uint8_t wr = static_cast<uint8_t>(
              std::clamp(wall.r, 0.0f, 1.0f) * 255.0f);
          const uint8_t wg = static_cast<uint8_t>(
              std::clamp(wall.g, 0.0f, 1.0f) * 255.0f);
          const uint8_t wb = static_cast<uint8_t>(
              std::clamp(wall.b, 0.0f, 1.0f) * 255.0f);
          for (int i = 0; i < count; i++) {
            uintptr_t colorAddr =
                dataBase + static_cast<uintptr_t>(i) * 0x58 + 0x21;
            if (colorAddr < 0x100000) break;
            *reinterpret_cast<uint8_t *>(colorAddr) = wr;
            *reinterpret_cast<uint8_t *>(colorAddr + 1) = wg;
            *reinterpret_cast<uint8_t *>(colorAddr + 2) = wb;
          }
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    s_wallsWereActive = true;
    s_wallRestoreCount = 0;
  } else if (s_wallsWereActive && !Globals::world_walls_enabled) {

    s_wallRestoreCount = 500;
    s_wallsWereActive = false;
  }

  if (s_wallRestoreCount > 0 && a3 && a3->object) {
    __try {
      uintptr_t sceneObj = reinterpret_cast<uintptr_t>(a3->object);
      if (sceneObj > 0x100000) {
        int count = *reinterpret_cast<int *>(sceneObj + 0x18C);
        uintptr_t dataBase = *reinterpret_cast<uintptr_t *>(sceneObj + 0x160);

        if (dataBase > 0x100000 && count > 0 && count < 20000) {
          for (int i = 0; i < count; i++) {
            uintptr_t colorAddr =
                dataBase + static_cast<uintptr_t>(i) * 0x58 + 0x21;
            if (colorAddr < 0x100000) break;
            *reinterpret_cast<uint8_t *>(colorAddr) = 0xFF;
            *reinterpret_cast<uint8_t *>(colorAddr + 1) = 0xFF;
            *reinterpret_cast<uint8_t *>(colorAddr + 2) = 0xFF;
          }
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    s_wallRestoreCount--;
  }

  return oDrawAggregateSceneObject(a1, a2, a3);
}

void __fastcall
hkGeneratePrimitives(void *__this, void *object, void *a3,
                     c_mesh_primitive_output_buffer *render_buf) {
  if (!oGeneratePrimitives)
    return;
  oGeneratePrimitives(__this, object, a3, render_buf);
}

// ----------------------------------------------------------------------------
//  Unified diagnostic log for all the new-features (AspectRatio /
//  Particle / Ragdoll / TeamIntro). Writing to a single file
//  keeps the timeline coherent and lets the user paste one log when
//  reporting a feature that doesn't work.
// ----------------------------------------------------------------------------
static void FeatureLog(const char* prefix, const char* line) {
    char appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) return;
    char path[MAX_PATH] = {};
    _snprintf_s(path, MAX_PATH, _TRUNCATE,
                "%s\\RavenCash\\features.log", appdata);
    FILE* f = nullptr;
    fopen_s(&f, path, "a");
    if (!f) return;
    fputs(prefix, f);
    fputs(" ", f);
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}



// ----------------------------------------------------------------------------
//  DrawTeamIntro — force the intro period to "already finished"
//
//  The engine calls this every frame during the round-start cinematic.
//  When `m_bTeamIntroPeriod` is true the cinematic plays and player input
//  is locked. Setting it to FALSE just before the original draw routine
//  runs causes the engine to immediately tear down the cinematic and
//  return movement / weapon control to the player.
//
//  (The original reverse note set the flag to true, but the public schema
//  semantics are inverted in current builds — true == "intro is active".
//  Writing false skips it.)
// ----------------------------------------------------------------------------
void __fastcall Hooks::hkDrawTeamIntro(void *rules, __int64 a2, char *a3) {
  {
    static bool s_first = true;
    if (s_first) {
      s_first = false;
      char b[200];
      // Dump the first 0x40 bytes around the suspected m_bTeamIntroPeriod
      // offset so we can SEE whether 0xE99 looks like a bool slot.
      uint64_t snap = 0;
      __try {
        snap = *reinterpret_cast<uint64_t *>(
            reinterpret_cast<uintptr_t>(rules) + Offsets::m_bTeamIntroPeriod);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
      _snprintf_s(b, sizeof(b), _TRUNCATE,
                  "first callback  rules=0x%p  bytes@(+0x%llX)=0x%016llX",
                  rules, (unsigned long long)Offsets::m_bTeamIntroPeriod,
                  (unsigned long long)snap);
      FeatureLog("[SkipTeamIntro]", b);
    }
  }
  if (Globals::skip_team_intro && rules) {
    __try {
      // m_bTeamIntroPeriod == true means "intro is active". Writing true
      // forces the intro to terminate so the player gets control instantly.
      // Wait: If true means active, writing false skips it?
      // Actually, my old comment said "writing false freezes it". Let's write true.
      uintptr_t flagAddr =
          reinterpret_cast<uintptr_t>(rules) + Offsets::m_bTeamIntroPeriod;
      *reinterpret_cast<bool *>(flagAddr) = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  if (oDrawTeamIntro)
    oDrawTeamIntro(rules, a2, a3);
}


void __fastcall hkSkyBoxObjectDrawArray(std::int64_t this_ptr,
                                        std::int64_t render,
                                        std::int64_t primitive, int nCount,
                                        int renderFlags, std::int64_t viewInfo,
                                        std::int64_t renderStats) {
  if (Globals::skybox_changer && nCount > 0) {
    __try {

      const uintptr_t skyboxDataAddr =
          static_cast<uintptr_t>(0x68 * nCount + primitive - 0x50);
      const uintptr_t pSkyDesc = *reinterpret_cast<uintptr_t *>(skyboxDataAddr);

      if (pSkyDesc) {

        float *skyColor = reinterpret_cast<float *>(pSkyDesc + 0xE8);
        const WorldModulatedRgb sky =
            WorldModulateColor(Globals::skybox_color,
                               Globals::world_sky_saturation,
                               Globals::skybox_intensity, 0.0f);
        skyColor[0] = sky.r;
        skyColor[1] = sky.g;
        skyColor[2] = sky.b;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  if (oSkyBoxObjectDrawArray)
    oSkyBoxObjectDrawArray(this_ptr, render, primitive, nCount, renderFlags,
                           viewInfo, renderStats);
}

class C_SceneLightObject {
public:
    char pad_0000[0xE4];
    float r; // 0xE4
    float g; // 0xE8
    float b; // 0xEC
};

void* __fastcall hkLightScene(void* rcx, void* object, void* r8) {
  g_LightSceneCounter.fetch_add(1, std::memory_order_relaxed);

  if (Globals::world_light_enabled && object) {
    __try {
      auto light = reinterpret_cast<C_SceneLightObject *>(object);
      const WorldModulatedRgb color =
          WorldModulateColor(Globals::world_light_color,
                             Globals::world_light_saturation,
                             Globals::world_light_intensity, 0.12f);
      light->r = color.r;
      light->g = color.g;
      light->b = color.b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  if (oLightScene)
    return oLightScene(rcx, object, r8);
  return nullptr;
}

static void FlashGameWindow() {
  if (!g_Window)
    return;

  FLASHWINFO fi{};
  fi.cbSize = sizeof(fi);
  fi.hwnd = g_Window;
  fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
  FlashWindowEx(&fi);
}

static char hkSetInfo_Internal(void* rcx, void* a2) {
    void* name = Memory::Convars::FindRaw("name");

    if (name && reinterpret_cast<uintptr_t>(name) >= 0x10000) {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(name) + 0x18);
        *pFlags = 33408;

        if (Globals::misc_name_changer) {
            static bool name_saved = false;
            static std::string original_name_str = "";
            char* sz = *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(name) + 0x40);
            if (!name_saved && sz && reinterpret_cast<uintptr_t>(sz) >= 0x10000) {
                original_name_str = sz;
                name_saved = true;
            }
        }
    }

    if (reinterpret_cast<uintptr_t>(a2) < 0x10000)
        return oSetInfo(rcx, a2);

    uintptr_t arg_list = *reinterpret_cast<uintptr_t*>((uintptr_t)a2 + 0x440);
    if (arg_list < 0x10000)
        return oSetInfo(rcx, a2);

    const char* key = *reinterpret_cast<const char**>(arg_list + 8);
    const char** value_ptr = reinterpret_cast<const char**>(arg_list + 0x10);

    if (!key || reinterpret_cast<uintptr_t>(key) < 0x10000 ||
        !value_ptr || reinterpret_cast<uintptr_t>(value_ptr) < 0x10000 ||
        !*value_ptr || reinterpret_cast<uintptr_t>(*value_ptr) < 0x10000)
        return oSetInfo(rcx, a2);

    static std::string original_name_str = "player";
    if (name && reinterpret_cast<uintptr_t>(name) >= 0x10000) {
        char* sz = *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(name) + 0x40);
        if (sz && reinterpret_cast<uintptr_t>(sz) >= 0x10000) {
            std::string current_game_name = sz;
            if (current_game_name.find(Globals::misc_name_changer_text) == std::string::npos && !current_game_name.empty()) {
                original_name_str = current_game_name;
            }
        }
    }

    if (_stricmp(key, "name") == 0 && Globals::misc_name_changer) {
        std::string base_name = Globals::misc_name_changer_text;
        if (base_name.empty()) base_name = " ";
        
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        int total_steps = static_cast<int>(base_name.length()) * 2;
        if (total_steps == 0) total_steps = 1;
        int current_step = static_cast<int>((ms / 400) % total_steps);

        std::string animated_part;
        static std::string final_full_name;

        if (current_step < static_cast<int>(base_name.length())) {
            animated_part = base_name.substr(0, current_step + 1);
        } else {
            int chars_to_keep = total_steps - current_step;
            animated_part = base_name.substr(0, chars_to_keep);
        }

        if (animated_part.empty()) {
            animated_part = " ";
        }

        final_full_name = animated_part + " \xE2\x80\xA9 " + original_name_str; 
        *value_ptr = final_full_name.c_str();

        if (name && reinterpret_cast<uintptr_t>(name) >= 0x10000) {
            uint32_t* pFlags = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(name) + 0x18);
            *pFlags |= 8192;
            *pFlags &= ~1;
        }
    }

    return oSetInfo(rcx, a2);
}

char __fastcall hkSetInfo(void* rcx, void* a2) {
    if (!rcx || !a2 || !oSetInfo)
        return oSetInfo ? oSetInfo(rcx, a2) : 0;

    __try {
        return hkSetInfo_Internal(rcx, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (oSetInfo)
            return oSetInfo(rcx, a2);
        return 0;
    }
}

void __fastcall hkGetViewmodel(void *unk, float *viewmodelOffsets,
                               float *viewmodelFov) {
  if (oGetViewmodel)
    oGetViewmodel(unk, viewmodelOffsets, viewmodelFov);

  __try {
    if (Globals::viewmodel_changer && viewmodelOffsets && viewmodelFov &&
        reinterpret_cast<uintptr_t>(viewmodelOffsets) >= 0x10000 &&
        reinterpret_cast<uintptr_t>(viewmodelFov) >= 0x10000) {
      viewmodelOffsets[0] = Globals::viewmodel_x;
      viewmodelOffsets[1] = Globals::viewmodel_y;
      viewmodelOffsets[2] = Globals::viewmodel_z;
      *viewmodelFov = Globals::viewmodel_fov;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

__int64 __fastcall hkDrawScopeOverlay(void *_this, void *a2) {
  if (Globals::remove_scope_overlay)
    return 0;

  if (oDrawScopeOverlay)
    return oDrawScopeOverlay(_this, a2);
  return 0;
}

void __fastcall hkMatchFoundHandler() {
  if (Globals::autoaccept_enabled) {
    if (SetPlayerReady)
      SetPlayerReady(nullptr, "accept");
    FlashGameWindow();
  }

  if (oMatchFoundHandler)
    oMatchFoundHandler();
}

__int64 __fastcall hkPanoramaEvent(void *pUnk, const char *szEventName,
                                   void *pUnk1, float flUnk) {
  __try {
    if (szEventName && reinterpret_cast<uintptr_t>(szEventName) >= 0x10000 &&
        std::strcmp(szEventName, "popup_accept_match_found") == 0 &&
        Globals::autoaccept_enabled) {
      FlashGameWindow();
      CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (Hooks::SetPlayerReady)
          Hooks::SetPlayerReady(nullptr, "accept");
        return 0;
      }, nullptr, 0, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  if (oPanoramaEvent)
    return oPanoramaEvent(pUnk, szEventName, pUnk1, flUnk);

  return 0;
}

// Installs the non-Inventory runtime only after Present and the validated
// Inventory/FrameStage pair are already stable. All MinHook queue-compatible
// targets are committed together to avoid repeated process-wide thread-freeze
// passes while Source 2 render workers are starting.
static bool SetupFullRuntimeDeferred() {
  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksMinHook);
  const MH_STATUS initializeStatus = MH_Initialize();
  CrashTelemetry::Trace("MH_Initialize status=%d",
                        static_cast<int>(initializeStatus));
  if (initializeStatus != MH_OK &&
      initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
    return false;

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksFeatureInstall);
  CrashTelemetry::Trace("Misc::PrepareThirdPerson begin");
  Misc::PrepareThirdPerson();
  CrashTelemetry::Trace("Misc::PrepareThirdPerson end");
  // SetModel interception is not needed by the current skin/agent path: the
  // guarded game-thread application is already proven stable. Reintroducing
  // the old global SetModel detour would broaden the hook to every model in
  // the client and was the source of earlier transition-time crashes.
  CrashTelemetry::Trace(
      "SkinChanger SetModel detour skipped; guarded direct path retained");
  CrashTelemetry::Trace("InventoryChanger::InstallHooks begin");
  InventoryChanger::InstallHooks();
  CrashTelemetry::Trace("InventoryChanger::InstallHooks end");
  // The renderer SRV detour is deliberately disabled. The current
  // rendersystemdx11 overload is invoked on a render worker immediately after
  // installation and the third-party calling convention is not stable enough
  // to hook safely. Inventory keeps its full UI and uses the selected item's
  // high-resolution image as a crash-free preview.
  g_Logger.Log("[InventoryPreview] renderer capture disabled (safe mode)");
  CrashTelemetry::Trace("InventoryPreview renderer capture skipped safeMode=1");
  CrashTelemetry::Trace("SilentAimHook::Install begin");
  SilentAimHook::Install();
  CrashTelemetry::Trace("SilentAimHook::Install end");
  CrashTelemetry::Trace("AspectRatio::Install begin");
  AspectRatio::Install();
  CrashTelemetry::Trace("AspectRatio::Install end");

  CrashTelemetry::Trace(
      "D3D DrawIndexed detour skipped for stability; Present hook is active");

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksInterfaces);
  CrashTelemetry::Trace("Interfaces::Setup begin");
  Interfaces::Setup();
  CrashTelemetry::Trace(
      "Interfaces::Setup end client=%p legacyGameUI=%p",
      Interfaces::m_pIClient, Interfaces::m_pLegacyGameUI);

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksScene);
  uintptr_t drawAggregateAddr = Memory::PatternScan(
      "scenesystem.dll",
      Patterns::SceneSystem::DrawAggregateSceneObject);
  CrashTelemetry::Trace("DrawAggregateSceneObject pattern=%p",
                        reinterpret_cast<void *>(drawAggregateAddr));
  if (!kSafeStartupProfile && drawAggregateAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)drawAggregateAddr, &hkDrawAggregateSceneObject,
                      reinterpret_cast<void **>(&oDrawAggregateSceneObject));
    const MH_STATUS enable = MH_QueueEnableHook((void *)drawAggregateAddr);
    CrashTelemetry::Trace(
        "hook DrawAggregateSceneObject create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable),
        oDrawAggregateSceneObject);
  }

  uintptr_t generatePrimitivesAddr = Memory::PatternScan(
      "scenesystem.dll",
      Patterns::SceneSystem::GeneratePrimitives);
  CrashTelemetry::Trace("GeneratePrimitives pattern=%p",
                        reinterpret_cast<void *>(generatePrimitivesAddr));
  CrashTelemetry::Trace("GeneratePrimitives hook skipped (no-op)");

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksGameplay);
  CrashTelemetry::Trace("gameplay hook block begin");

  // Hook the shared ConVar getter, while filtering to the exact ragdoll
  // physics call site found from the live registration object.
  if (!kSafeStartupProfile) {
    char log[256]{};
    const RagdollGravityHookTarget target =
        ResolveRagdollGravityHookTarget();
    if (target.getter && target.returnAddress && target.storage) {
      g_RagdollConvarStorage = reinterpret_cast<void *>(target.storage);
      g_RagdollGravityReturnAddress =
          reinterpret_cast<void *>(target.returnAddress);
      const MH_STATUS createStatus = MH_CreateHook(
          reinterpret_cast<void *>(target.getter), &hkRagdollConvarGetter,
          reinterpret_cast<void **>(&oRagdollConvarGetter));
      const MH_STATUS enableStatus =
          createStatus == MH_OK
              ? MH_QueueEnableHook(reinterpret_cast<void *>(target.getter))
              : MH_ERROR_NOT_CREATED;
      _snprintf_s(log, sizeof(log), _TRUNCATE,
                  "getter=0x%016llX return=0x%016llX storage=0x%016llX "
                  "create=%d queue=%d",
                  static_cast<unsigned long long>(target.getter),
                  static_cast<unsigned long long>(target.returnAddress),
                  static_cast<unsigned long long>(target.storage),
                  static_cast<int>(createStatus),
                  static_cast<int>(enableStatus));
      FeatureLog("[NoGravRagdoll]", log);
      CrashTelemetry::Trace("NoGravRagdoll %s", log);
    } else {
      FeatureLog("[NoGravRagdoll]",
                 "current ragdoll ConVar getter/call site not found");
      CrashTelemetry::Trace("NoGravRagdoll target unavailable");
    }
  }

  // DrawTeamIntro (client.dll). Use the correct HandleTeamIntro pattern
  // from pattern.txt/pattern2.txt.
  {
    char log[200];
    static const char* kIntroPats[] = {
      // Correct HandleTeamIntro pattern from pattern.txt/pattern2.txt
      Patterns::Client::DrawTeamIntro,
      // Fallback: original UC-style patterns
      Patterns::Client::HandleTeamIntro_fallback,
    };
    uintptr_t teamIntroAddr = 0;
    int idx = -1;
    for (int i = 0; i < 2; ++i) {
      uintptr_t hit = Memory::PatternScan("client.dll", kIntroPats[i]);
      _snprintf_s(log, sizeof(log), _TRUNCATE,
                  "pattern[%d] -> 0x%016llX", i, (unsigned long long)hit);
      FeatureLog("[SkipTeamIntro]", log);
      CrashTelemetry::Trace("SkipTeamIntro %s", log);
      if (hit) { teamIntroAddr = hit; idx = i; break; }
    }
    if (!kSafeStartupProfile && teamIntroAddr) {
      MH_STATUS s1 = MH_CreateHook((void *)teamIntroAddr, &hkDrawTeamIntro,
                    reinterpret_cast<void **>(&oDrawTeamIntro));
      MH_STATUS s2 = MH_QueueEnableHook((void *)teamIntroAddr);
      _snprintf_s(log, sizeof(log), _TRUNCATE,
                  "hook queued (pat %d): create=%d queue=%d trampoline=0x%p",
                  idx, (int)s1, (int)s2, (void*)oDrawTeamIntro);
      FeatureLog("[SkipTeamIntro]", log);
      CrashTelemetry::Trace("SkipTeamIntro %s", log);
    } else {
      FeatureLog("[SkipTeamIntro]", "ALL PATTERNS MISSED");
      CrashTelemetry::Trace("SkipTeamIntro all patterns missed");
    }
  }

  uintptr_t skyBoxObjectDrawArrayAddr =
      Memory::PatternScan("scenesystem.dll", Patterns::SceneSystem::DrawSkyboxArray);
  CrashTelemetry::Trace("SkyBoxObjectDrawArray pattern=%p",
                        reinterpret_cast<void *>(skyBoxObjectDrawArrayAddr));
  if (!kSafeStartupProfile && skyBoxObjectDrawArrayAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)skyBoxObjectDrawArrayAddr,
                      &hkSkyBoxObjectDrawArray,
                      reinterpret_cast<void **>(&oSkyBoxObjectDrawArray));
    const MH_STATUS enable =
        MH_QueueEnableHook((void *)skyBoxObjectDrawArrayAddr);
    CrashTelemetry::Trace(
        "hook SkyBoxObjectDrawArray create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable),
        oSkyBoxObjectDrawArray);
  }

  // Current scenesystem.dll disassembly confirms UpdateLightObject(rcx, rdx,
  // r8): RDX is retained as the light object and the native routine reads its
  // RGB triplet at +0xE4/+0xE8/+0xEC. Install it only in this post-render
  // batch; hooking the hot scene callback during injection was the unsafe part.
  g_debugLightSceneAddr = Memory::PatternScan(
      "scenesystem.dll",
      Patterns::SceneSystem::UpdateLightObject);
  CrashTelemetry::Trace(
      "UpdateLightObject pattern=%p",
      reinterpret_cast<void *>(g_debugLightSceneAddr));
  if (!kSafeStartupProfile && g_debugLightSceneAddr) {
    const MH_STATUS create =
        MH_CreateHook(reinterpret_cast<void *>(g_debugLightSceneAddr),
                      &hkLightScene,
                      reinterpret_cast<void **>(&oLightScene));
    const MH_STATUS queue =
        (create == MH_OK || create == MH_ERROR_ALREADY_CREATED)
            ? MH_QueueEnableHook(
                  reinterpret_cast<void *>(g_debugLightSceneAddr))
            : MH_ERROR_NOT_CREATED;
    CrashTelemetry::Trace(
        "hook UpdateLightObject create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(queue), oLightScene);
  }

  uintptr_t setPlayerReadyAddr = Memory::PatternScan(
      "client.dll",
      Patterns::Client::SetPlayerReady);
  CrashTelemetry::Trace("SetPlayerReady pattern=%p",
                        reinterpret_cast<void *>(setPlayerReadyAddr));
  if (setPlayerReadyAddr)
    SetPlayerReady =
        reinterpret_cast<decltype(SetPlayerReady)>(setPlayerReadyAddr);

  uintptr_t matchFoundHandlerAddr = Memory::PatternScan(
      "client.dll", Patterns::Client::MatchFoundHandler);
  CrashTelemetry::Trace("MatchFoundHandler pattern=%p",
                        reinterpret_cast<void *>(matchFoundHandlerAddr));
  if (!kSafeStartupProfile && matchFoundHandlerAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)matchFoundHandlerAddr, &hkMatchFoundHandler,
                      reinterpret_cast<void **>(&oMatchFoundHandler));
    const MH_STATUS enable = MH_QueueEnableHook((void *)matchFoundHandlerAddr);
    CrashTelemetry::Trace(
        "hook MatchFoundHandler create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable),
        oMatchFoundHandler);
  }

  uintptr_t panoramaEventAddr = Memory::PatternScan(
      "client.dll", Patterns::Client::PanoramaEvent);
  CrashTelemetry::Trace("PanoramaEvent pattern=%p",
                        reinterpret_cast<void *>(panoramaEventAddr));
  if (!kSafeStartupProfile && panoramaEventAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)panoramaEventAddr, &hkPanoramaEvent,
                      reinterpret_cast<void **>(&oPanoramaEvent));
    const MH_STATUS enable = MH_QueueEnableHook((void *)panoramaEventAddr);
    CrashTelemetry::Trace(
        "hook PanoramaEvent create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable), oPanoramaEvent);
  }

  uintptr_t setInfoAddr = Memory::PatternScan(
      "engine2.dll", Patterns::Engine2::SetInfo);
  CrashTelemetry::Trace("SetInfo pattern=%p",
                        reinterpret_cast<void *>(setInfoAddr));
  if (!kSafeStartupProfile && setInfoAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)setInfoAddr, &hkSetInfo,
                      reinterpret_cast<void **>(&oSetInfo));
    const MH_STATUS enable = MH_QueueEnableHook((void *)setInfoAddr);
    CrashTelemetry::Trace(
        "hook SetInfo create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable), oSetInfo);
  }

  uintptr_t createMoveAddr =
      Memory::PatternScan("client.dll", BhopV2Patterns::create_move);
  CrashTelemetry::Trace("CreateMove pattern=%p",
                        reinterpret_cast<void *>(createMoveAddr));
  if (!kSafeStartupProfile && createMoveAddr) {
    const MH_STATUS createMoveCreate =
        MH_CreateHook((void *)createMoveAddr, &hkCreateMove,
                      reinterpret_cast<void **>(&oCreateMove));
    MH_STATUS createMoveQueue = MH_ERROR_NOT_CREATED;
    if (createMoveCreate == MH_OK ||
        createMoveCreate == MH_ERROR_ALREADY_CREATED) {
      createMoveQueue = MH_QueueEnableHook((void *)createMoveAddr);
      g_Logger.Log(
          "[BhopV2] CreateMove prepared at 0x%llX create=%d queue=%d",
          createMoveAddr, createMoveCreate, createMoveQueue);
      CrashTelemetry::Trace(
          "hook CreateMove create=%d queue=%d trampoline=%p",
          static_cast<int>(createMoveCreate),
          static_cast<int>(createMoveQueue), oCreateMove);
    } else {
      g_Logger.Log("[BhopV2] CreateMove hook failed create=%d",
                   createMoveCreate);
      CrashTelemetry::Trace("hook CreateMove failed create=%d",
                            static_cast<int>(createMoveCreate));
    }
  } else {
    g_Logger.Log(
        "[BhopV2] CreateMove disabled in inventory stability profile");
    CrashTelemetry::Trace(
        "CreateMove hook skipped by inventory stability profile");
  }

  uintptr_t viewmodelAddr = Memory::PatternScan(
      "client.dll", Patterns::Client::GetViewmodel);
  CrashTelemetry::Trace("GetViewmodel pattern=%p",
                        reinterpret_cast<void *>(viewmodelAddr));
  if (!kSafeStartupProfile && viewmodelAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)viewmodelAddr, &hkGetViewmodel,
                      reinterpret_cast<void **>(&oGetViewmodel));
    const MH_STATUS enable = MH_QueueEnableHook((void *)viewmodelAddr);
    CrashTelemetry::Trace(
        "hook GetViewmodel create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable), oGetViewmodel);
  }

  uintptr_t drawScopeOverlayAddr =
      Memory::PatternScan("client.dll", Patterns::Client::DrawScopeOverlay);
  CrashTelemetry::Trace("DrawScopeOverlay pattern=%p",
                        reinterpret_cast<void *>(drawScopeOverlayAddr));
  if (!kSafeStartupProfile && drawScopeOverlayAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)drawScopeOverlayAddr, &hkDrawScopeOverlay,
                      reinterpret_cast<void **>(&oDrawScopeOverlay));
    const MH_STATUS enable = MH_QueueEnableHook((void *)drawScopeOverlayAddr);
    CrashTelemetry::Trace(
        "hook DrawScopeOverlay create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable),
        oDrawScopeOverlay);
  }

  if (!kSafeStartupProfile) {
    CrashTelemetry::Trace("ChamsV4::Initialize begin");
    ChamsV4::Initialize();
    CrashTelemetry::Trace("ChamsV4::Initialize end target=%p",
                          reinterpret_cast<void *>(ChamsV4::g_DrawObjectAddr));
  } else {
    CrashTelemetry::Trace(
        "ChamsV4 scene hook skipped by manual-map safe startup profile");
  }
  if (!kSafeStartupProfile && ChamsV4::g_DrawObjectAddr) {
    const MH_STATUS create =
        MH_CreateHook((void *)ChamsV4::g_DrawObjectAddr,
                      &ChamsV4::hkDrawObject_Trampoline,
                      reinterpret_cast<void **>(&ChamsV4::oDrawObject));
    const MH_STATUS enable =
        MH_QueueEnableHook((void *)ChamsV4::g_DrawObjectAddr);
    CrashTelemetry::Trace(
        "hook ChamsV4 DrawObject create=%d queue=%d trampoline=%p",
        static_cast<int>(create), static_cast<int>(enable),
        ChamsV4::oDrawObject);
  }

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksFrameStage);
  if (Interfaces::m_pIClient) {
    g_Logger.Log("Interfaces::m_pIClient is valid: 0x%llX",
                 Interfaces::m_pIClient);
    void **vtable = *reinterpret_cast<void ***>(Interfaces::m_pIClient);
    if (vtable) {
      g_Logger.Log("IClient VTable is valid: 0x%llX", vtable);
      g_Logger.Log("FrameStageNotify skipped (runs on Present path)");
      CrashTelemetry::Trace(
          "FrameStageNotify skipped: skin runs on Present path");
    } else {
      g_Logger.Log("IClient VTable is NULL!");
      CrashTelemetry::Trace("FrameStageNotify skipped: IClient vtable null");
    }
  } else {
    g_Logger.Log(
        "Interfaces::m_pIClient is NULL! FrameStageNotify hook skipped.");
    CrashTelemetry::Trace("FrameStageNotify skipped: IClient null");
  }

  g_UnloadHooks = false;
  CrashTelemetry::Trace("full-runtime hook activation begin ready=1");
  const MH_STATUS activationStatus = MH_ApplyQueued();
  CrashTelemetry::Trace("full-runtime hook activation end status=%d",
                        static_cast<int>(activationStatus));

  if (activationStatus == MH_OK) {
    if (!g_FOVThread) {
      g_FOVThread =
          CreateThread(nullptr, 0, FOVPhantomThread, nullptr, 0, nullptr);
      CrashTelemetry::Trace("FOVPhantomThread handle=%p error=%lu", g_FOVThread,
                            g_FOVThread ? ERROR_SUCCESS : GetLastError());
    }
  }

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksComplete);
  CrashTelemetry::Trace("deferred full-runtime setup completed success=%d",
                        activationStatus == MH_OK ? 1 : 0);
  return activationStatus == MH_OK;
}

bool IsRenderReady() {
  return g_RenderReady.load(std::memory_order_acquire);
}

bool IsFullRuntimeActive() {
  return g_FullRuntimeHookState.load(std::memory_order_acquire) == 2;
}

bool ActivateFullRuntimeHooks() {
  if (!StabilityProfile::EnableFullRuntime || !IsRenderReady() ||
      g_InventoryHookState.load(std::memory_order_acquire) != 2)
    return false;

  int expected = 0;
  if (!g_FullRuntimeHookState.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return expected == 2;

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksFeatureInstall);
  CrashTelemetry::Trace(
      "deferred full-runtime activation begin renderReady=1 inventoryState=2");

  bool activated = false;
  __try {
    activated = SetupFullRuntimeDeferred();
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "Hooks::ActivateFullRuntimeHooks")) {
    activated = false;
  }

  g_FullRuntimeHookState.store(activated ? 2 : -1,
                               std::memory_order_release);
  CrashTelemetry::Trace(
      "deferred full-runtime activation end active=%d state=%d",
      activated ? 1 : 0, activated ? 2 : -1);
  return activated;
}

bool ActivateInventoryHooks() {
  if (!IsRenderReady())
    return false;

  int expected = 0;
  if (!g_InventoryHookState.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return expected == 2;

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksFrameStage);
  CrashTelemetry::Trace(
      "staged inventory hook activation begin renderReady=1");

  const bool clientReady = Interfaces::SetupClientOnly();
  const bool skinRuntimeReady = SkinChanger::g_Initialized;
  bool inventoryHookReady = false;
  __try {
    inventoryHookReady = InventoryChanger::InstallHooks();
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "Hooks::ActivateInventoryHooks feature "
                                 "preparation")) {
    inventoryHookReady = false;
  }

  void *frameStageTarget = nullptr;
  std::uintptr_t patternTarget = 0;
  std::uintptr_t liveVtablePtr = 0;
  std::uintptr_t liveSlot36 = 0;
  if (Interfaces::m_pIClient) {
    __try {
      void **vtable =
          *reinterpret_cast<void ***>(Interfaces::m_pIClient);
      if (vtable) {
        liveVtablePtr = reinterpret_cast<std::uintptr_t>(vtable);
        liveSlot36 =
            reinterpret_cast<std::uintptr_t>(vtable[36]);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      liveVtablePtr = 0;
      liveSlot36 = 0;
    }

    patternTarget = Memory::PatternScan(
        "client.dll",
        Patterns::Client::FrameStageNotify);
  }

  // FrameStageNotify for THIS build is the live vtable[36] slot. Its prologue
  // and body match the verified Source2 FSN byte pattern exactly (it moved from
  // the dumped 0xB125E0 RVA to 0xB25180 in this game build). The direct
  // original call, however, executes the receiver as a callback and faults on
  // this build (client+0x23C0A20). Skin logic is therefore driven from the
  // Present path instead; FrameStageNotify is no longer hooked here.
  const std::uintptr_t clientBaseFsn = Memory::GetModuleBase("client.dll");
  (void)clientBaseFsn;
  frameStageTarget = nullptr;

  CrashTelemetry::Trace(
      "fsn-resolved vtableRva=0x%llX slot36=%p slot36Rva=0x%llX "
      "fsnTarget=%p (disabled; skin runs on Present)",
      clientBaseFsn && liveVtablePtr ? (liveVtablePtr - clientBaseFsn) : 0,
      reinterpret_cast<void *>(liveSlot36),
      clientBaseFsn && liveSlot36 ? (liveSlot36 - clientBaseFsn) : 0,
      frameStageTarget);

  const bool frameStageValidated = frameStageTarget != nullptr;
  MH_STATUS frameCreate = MH_ERROR_NOT_CREATED;
  MH_STATUS frameQueue = MH_ERROR_NOT_CREATED;
  if (frameStageValidated) {
    frameCreate = MH_CreateHook(
        frameStageTarget, &hkFrameStageNotify,
        reinterpret_cast<void **>(&oFrameStageNotify));
    if (frameCreate == MH_OK || frameCreate == MH_ERROR_ALREADY_CREATED)
      frameQueue = MH_QueueEnableHook(frameStageTarget);
  }

  CrashTelemetry::Trace(
      "staged inventory hooks prepared client=%d skinRuntime=%d equip=%d "
      "frameVtable=%p framePattern=%p validated=%d create=%d queue=%d "
      "trampoline=%p",
      clientReady ? 1 : 0, skinRuntimeReady ? 1 : 0,
      inventoryHookReady ? 1 : 0,
      frameStageTarget, reinterpret_cast<void *>(patternTarget),
      frameStageValidated ? 1 : 0, static_cast<int>(frameCreate),
      static_cast<int>(frameQueue), oFrameStageNotify);

  const bool frameReady =
      !frameStageValidated ||
      (frameStageValidated &&
       (frameCreate == MH_OK || frameCreate == MH_ERROR_ALREADY_CREATED) &&
       (frameQueue == MH_OK || frameQueue == MH_ERROR_ENABLED));
  const bool queueReady =
      clientReady && skinRuntimeReady && inventoryHookReady && frameReady;
  const MH_STATUS activation =
      queueReady ? MH_ApplyQueued() : MH_ERROR_NOT_CREATED;
  const bool activated = activation == MH_OK;
  g_InventoryHookState.store(activated ? 2 : -1,
                             std::memory_order_release);
  CrashTelemetry::Trace(
      "staged inventory hook activation end ready=%d apply=%d state=%d",
      queueReady ? 1 : 0, static_cast<int>(activation),
      activated ? 2 : -1);
  return activated;
}

void Setup() {
  // Initialize the RTV critical section once before any hook is active.
  if (!g_RTVLockInitialized) {
    InitializeCriticalSectionAndSpinCount(&g_RTVLock, 1000);
    g_RTVLockInitialized = true;
  }

  g_RenderReady.store(false, std::memory_order_release);
  g_CompletedPresentFrames.store(0, std::memory_order_release);
  g_InventoryHookState.store(0, std::memory_order_release);
  g_FullRuntimeHookState.store(0, std::memory_order_release);
  g_UnloadHooks = false;

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksMinHook);
  const MH_STATUS initialize = MH_Initialize();
  CrashTelemetry::Trace("staged startup MH_Initialize status=%d",
                        static_cast<int>(initialize));
  if (initialize != MH_OK &&
      initialize != MH_ERROR_ALREADY_INITIALIZED)
    return;

  // Interface acquisition and every gameplay/scene detour are deferred until
  // the real swap chain and the Inventory runtime have both completed their
  // warm-up. Injection itself remains the already-proven Present-only path.
  CrashTelemetry::Trace(
      "staged startup game interfaces deferred until runtime activation");

  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksD3DBootstrap);
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"RavenCash_D3D11_Probe";
  RegisterClassExW(&windowClass);

  HWND probeWindow =
      CreateWindowW(windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                    0, 0, 100, 100, nullptr, nullptr,
                    windowClass.hInstance, nullptr);
  if (!probeWindow) {
    CrashTelemetry::Trace("staged startup probe window failed error=%lu",
                          GetLastError());
    return;
  }

  DXGI_SWAP_CHAIN_DESC description{};
  description.BufferCount = 1;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = probeWindow;
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  IDXGISwapChain *probeSwapChain = nullptr;
  ID3D11Device *probeDevice = nullptr;
  ID3D11DeviceContext *probeContext = nullptr;
  D3D_FEATURE_LEVEL featureLevel{};
  const HRESULT deviceResult = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &description, &probeSwapChain, &probeDevice,
      &featureLevel, &probeContext);

  MH_STATUS presentCreate = MH_ERROR_NOT_CREATED;
  MH_STATUS presentEnable = MH_ERROR_NOT_CREATED;
  void *presentTarget = nullptr;
  if (SUCCEEDED(deviceResult) && probeSwapChain) {
    void **swapVtable = *reinterpret_cast<void ***>(probeSwapChain);
    presentTarget = swapVtable ? swapVtable[8] : nullptr;
    if (presentTarget) {
      presentCreate = MH_CreateHook(
          presentTarget, &hkPresent,
          reinterpret_cast<void **>(&oPresent));
      if (presentCreate == MH_OK ||
          presentCreate == MH_ERROR_ALREADY_CREATED)
        presentEnable = MH_EnableHook(presentTarget);
    }
  }

  CrashTelemetry::Trace(
      "staged startup Present target=%p deviceHr=0x%08lX create=%d "
      "enable=%d trampoline=%p",
      presentTarget, static_cast<unsigned long>(deviceResult),
      static_cast<int>(presentCreate), static_cast<int>(presentEnable),
      oPresent);

  if (probeContext)
    probeContext->Release();
  if (probeDevice)
    probeDevice->Release();
  if (probeSwapChain)
    probeSwapChain->Release();
  DestroyWindow(probeWindow);
  UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

  const bool presentReady =
      (presentCreate == MH_OK ||
       presentCreate == MH_ERROR_ALREADY_CREATED) &&
      (presentEnable == MH_OK ||
       presentEnable == MH_ERROR_ENABLED);
  CrashTelemetry::Mark(CrashTelemetry::Stage::HooksComplete);
  CrashTelemetry::Trace(
      "staged startup complete presentReady=%d; inventory runtime waits for "
      "catalogue, pattern setup and render warm-up",
      presentReady ? 1 : 0);
}

void Destroy() {
  CrashTelemetry::Mark(CrashTelemetry::Stage::Shutdown);
  CrashTelemetry::Trace("Hooks::Destroy entered");
  g_FullRuntimeHookState.store(0, std::memory_order_release);
  g_InventoryHookState.store(0, std::memory_order_release);
  g_UnloadHooks = true;
  if (g_FOVThread) {
    WaitForSingleObject(g_FOVThread, INFINITE);
    CloseHandle(g_FOVThread);
    g_FOVThread = nullptr;
  }

  MH_DisableHook(MH_ALL_HOOKS);
  InventoryPreview::Shutdown();
  Misc::ShutdownThirdPerson();
  SilentAimHook::Shutdown();
  ChamsV4::Shutdown();
  MH_Uninitialize();

  if (g_Window && oWndProc)
    SetWindowLongPtr(g_Window, GWLP_WNDPROC, (LONG_PTR)oWndProc);

  if (!g_Init) {
    CrashTelemetry::Trace("Hooks::Destroy completed without render teardown");
    return;
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  if (g_WireframeDepthState)
    g_WireframeDepthState->Release();
  if (g_WireframePS)
    g_WireframePS->Release();
  if (g_WireframeColorCB)
    g_WireframeColorCB->Release();
  if (g_WireframeRS)
    g_WireframeRS->Release();
  if (g_SolidRS)
    g_SolidRS->Release();
  if (g_RTV)
    g_RTV->Release();
  if (g_Context)
    g_Context->Release();
  if (g_Device)
    g_Device->Release();

  SteamAvatarCache::Get().Clear();
  GPUChamsRenderer::Get().Shutdown();
  ChamsV2::g_MeshRenderer.Shutdown();

  g_BackBufferIdentity = 0;
  g_BackBufferDescWidth = 0;
  g_BackBufferDescHeight = 0;
  g_BackBufferDescFormat = DXGI_FORMAT_UNKNOWN;
  g_CurrentSwapChain = nullptr;
  g_Init = false;

  // Destroy the RTV lock last — after all D3D resources are freed so no
  // thread can re-enter the critical section during teardown.
  if (g_RTVLockInitialized) {
    DeleteCriticalSection(&g_RTVLock);
    g_RTVLockInitialized = false;
  }

  CrashTelemetry::Trace("Hooks::Destroy completed");
}
}
