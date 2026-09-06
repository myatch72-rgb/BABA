#include "Misc.h"
#include "CustomModel.h"
#include "../../../ext/imgui/imgui.h"
#include "../../sdk/entity/EntityManager.h"
#include "../../sdk/interfaces/Interfaces.h"
#include "../../sdk/memory/Globals.h"
#include "../../sdk/memory/Offsets.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/memory/Patterns.h"
#include "../../sdk/utils/CCSGOInput.h"
#include "../../sdk/utils/Globals.h"
#include "../../sdk/utils/Raycasting.h"
#include "../../sdk/utils/Utils.h"
#include "../../sdk/utils/Vector.h"
#include "../../sdk/utils/SteamAvatarManager.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mmsystem.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;

namespace {
constexpr uint8_t MOVETYPE_NOCLIP = 7;
constexpr uint8_t MOVETYPE_LADDER = 9;
std::atomic_bool g_thirdPersonRequested{false};
std::atomic<float> g_thirdPersonDistance{120.0f};
using NativeThirdPersonInit = void(__fastcall*)(void*, int);
NativeThirdPersonInit g_nativeThirdPersonInit = nullptr;
uintptr_t g_cameraCheatGateBranch = 0;
bool g_cameraGateOwned = false;
bool g_thirdPersonOwnsCamera = false;
ULONGLONG g_lastThirdPersonResolveAttempt = 0;

float HudScale() {
  return std::clamp(Globals::hud_scale, 0.80f, 1.30f);
}

float HudOpacity() {
  return std::clamp(Globals::hud_opacity, 0.45f, 1.0f);
}

const float* HudAccentSource() {
  return Globals::hud_sync_menu_accent ? Globals::menu_accent_color
                                        : Globals::hud_accent_color;
}

ImU32 HudAccent(float alpha = 1.0f) {
  const float* color = HudAccentSource();
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(color[0], color[1], color[2],
             std::clamp(alpha * HudOpacity(), 0.0f, 1.0f)));
}

ImU32 HudColor(int red, int green, int blue, int alpha) {
  const int scaledAlpha = std::clamp(
      static_cast<int>(static_cast<float>(alpha) * HudOpacity()), 0, 255);
  return IM_COL32(red, green, blue, scaledAlpha);
}

float HudViewportRight() {
  if (Globals::GameViewportWidth > 1)
    return Globals::GameViewportX + Globals::GameViewportWidth;
  return ImGui::GetIO().DisplaySize.x;
}

float HudViewportTop() {
  return Globals::GameViewportHeight > 1 ? Globals::GameViewportY : 0.0f;
}

float HudRightStackY() {
  const float scale = HudScale();
  float y = HudViewportTop() + 14.0f * scale;
  if (Globals::watermark_enabled)
    y += 37.0f * scale;
  if (Globals::hud_status_enabled)
    y += 38.0f * scale;
  return y;
}
} // namespace

static void Misc_LogThirdPerson(const char* line) {
  if (!line)
    return;
  char appdata[MAX_PATH] = {};
  if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
    return;
  std::error_code ec;
  fs::path dir = fs::path(appdata) / "RavenCash";
  fs::create_directories(dir, ec);
  FILE* file = nullptr;
  fopen_s(&file, (dir / "features.log").string().c_str(), "a");
  if (!file)
    return;
  std::fprintf(file, "[ThirdPerson] %s\n", line);
  std::fclose(file);
}

static bool Misc_ResolveThirdPerson() {
  if (g_nativeThirdPersonInit && g_cameraCheatGateBranch)
    return true;

  const ULONGLONG now = GetTickCount64();
  if (now - g_lastThirdPersonResolveAttempt < 1000)
    return false;
  g_lastThirdPersonResolveAttempt = now;

  const uintptr_t initHandler = Memory::PatternScan(
      "client.dll",
      Patterns::Client::ThirdPersonOnHandler);

  // ConCommand_thirdperson içindeki: 4C 8B 05 (g_pInput) sonrası gelen CALL
  // talimatını resolve edip initializer adresini al.
  uintptr_t initializer = 0;
  if (initHandler) {
    __try {
      // Pattern içindeki "4C 8B 05 ? ? ? ? 41 8B 80 50 0B 00 00" bloğunu bul,
      // ardından gelen E8 (CALL) yönlendirmesini çöz.
      for (int i = 0; i < 80; ++i) {
        const uintptr_t p = initHandler + i;
        if (Utils::SafeRead<uint8_t>(p)     == 0x4C &&
            Utils::SafeRead<uint8_t>(p + 1) == 0x8B &&
            Utils::SafeRead<uint8_t>(p + 2) == 0x05) {
          // 3 byte ileri git: 4C 8B 05 XX XX XX XX (7 byte), sonra 41 8B 80
          // ondan sonra E8 CALL varsa resolve et
          for (int j = i + 7; j < i + 40; ++j) {
            const uintptr_t q = initHandler + j;
            if (Utils::SafeRead<uint8_t>(q) == 0xE8) {
              const int32_t rel = Utils::SafeRead<int32_t>(q + 1);
              const uintptr_t target = q + 5 + static_cast<intptr_t>(rel);
              if (target > 0x10000 && target < 0x00007FFFFFFFFFFF) {
                initializer = target;
              }
              break;
            }
          }
          break;
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  // Gate: ThirdPersonReset pattern
  // 75 10 offseti pattern içinde byte 6'da yer alır.
  const uintptr_t resetFn = Memory::PatternScan(
      "client.dll",
      Patterns::Client::ThirdPersonReset);
  const uintptr_t branch = resetFn ? resetFn + 0x7 : 0;
  if (!initializer || !branch ||
      Utils::SafeRead<uint8_t>(branch) != 0x75 ||
      Utils::SafeRead<uint8_t>(branch + 1) != 0x10) {
    g_nativeThirdPersonInit = nullptr;
    g_cameraCheatGateBranch = 0;
    char failLog[256] = {};
    std::snprintf(failLog, sizeof(failLog),
                  "resolve failed: initHandler=0x%llX initializer=0x%llX "
                  "resetFn=0x%llX branch=0x%llX branchByte=0x%02X",
                  static_cast<unsigned long long>(initHandler),
                  static_cast<unsigned long long>(initializer),
                  static_cast<unsigned long long>(resetFn),
                  static_cast<unsigned long long>(branch),
                  branch ? Utils::SafeRead<uint8_t>(branch) : 0xFF);
    Misc_LogThirdPerson(failLog);
    return false;
  }

  g_nativeThirdPersonInit =
      reinterpret_cast<NativeThirdPersonInit>(initializer);
  g_cameraCheatGateBranch = branch;
  char status[160] = {};
  std::snprintf(status, sizeof(status),
                "resolved initializer=0x%llX gateBranch=0x%llX",
                static_cast<unsigned long long>(initializer),
                static_cast<unsigned long long>(branch));
  Misc_LogThirdPerson(status);
  return true;
}

static bool Misc_WriteExecutableByte(uintptr_t address, uint8_t value) {
  if (!address)
    return false;
  DWORD oldProtection = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(address), 1,
                      PAGE_EXECUTE_READWRITE, &oldProtection))
    return false;
  bool written = false;
  __try {
    *reinterpret_cast<volatile uint8_t*>(address) = value;
    written = (*reinterpret_cast<volatile uint8_t*>(address) == value);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    written = false;
  }
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(address), 1, oldProtection, &ignored);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address),
                        1);
  return written;
}

static bool Misc_SetCameraGateBypass(bool enabled) {
  if (enabled) {
    if (g_cameraGateOwned)
      return true;
    if (!Misc_ResolveThirdPerson())
      return false;
    if (Utils::SafeRead<uint8_t>(g_cameraCheatGateBranch) != 0x75 ||
        Utils::SafeRead<uint8_t>(g_cameraCheatGateBranch + 1) != 0x10)
      return false;
    if (!Misc_WriteExecutableByte(g_cameraCheatGateBranch, 0xEB))
      return false;
    g_cameraGateOwned = true;
    Misc_LogThirdPerson("camera-only cheat gate bypass enabled");
    return true;
  }

  if (!g_cameraGateOwned)
    return true;
  const uint8_t current = Utils::SafeRead<uint8_t>(g_cameraCheatGateBranch);
  const bool restored =
      current == 0x75 ||
      (current == 0xEB &&
       Misc_WriteExecutableByte(g_cameraCheatGateBranch, 0x75));
  if (restored) {
    g_cameraGateOwned = false;
    Misc_LogThirdPerson("camera-only cheat gate restored");
  }
  return restored;
}

static void Misc_SetPawnThirdPerson(bool enabled) {
  const uintptr_t pawn = Memory::Globals::LocalPawn();
  if (!Utils::IsValidPtr(pawn))
    return;
  const uintptr_t vtable = Utils::SafeRead<uintptr_t>(pawn);
  const uintptr_t function =
      Utils::SafeRead<uintptr_t>(vtable + 311 * sizeof(uintptr_t));
  if (!Utils::IsValidPtr(vtable) || !Utils::IsValidPtr(function))
    return;
  __try {
    reinterpret_cast<void(__fastcall*)(void*, bool)>(function)(
        reinterpret_cast<void*>(pawn), enabled);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Misc_LogThirdPerson("pawn camera visibility transition raised exception");
  }
}

static bool s_hitsoundInitialized = false;
static int s_previousTotalHits = 0;
static float s_hitmarkerAlpha = 0.f;
static std::chrono::steady_clock::time_point s_hitmarkerStartTime;
static float s_lastLocalHitTime = -1000.0f;

static float Misc_GetTime() {
  return static_cast<float>(GetTickCount64()) / 1000.0f;
}

void Misc::InitHitsound() {
  if (s_hitsoundInitialized)
    return;
  s_hitsoundInitialized = true;

  Globals::hitsound_files.clear();

  try {
    if (fs::exists(Globals::hitsound_dir) &&
        fs::is_directory(Globals::hitsound_dir)) {
      for (const auto &entry : fs::directory_iterator(Globals::hitsound_dir)) {
        if (!entry.is_regular_file())
          continue;

        std::string ext = entry.path().extension().string();
        for (auto &c : ext)
          c = (char)tolower(c);

        if (ext == ".wav") {
          Globals::hitsound_files.push_back(entry.path().filename().string());
        }
      }
    }
  } catch (...) {
  }
}

void Misc::PlayHitsound() {
  if (!Globals::hitsound_enabled)
    return;
  if (Globals::hitsound_selected < 0 ||
      Globals::hitsound_selected >= (int)Globals::hitsound_files.size())
    return;

  try {
    std::string fullPath = Globals::hitsound_dir + "\\" +
                           Globals::hitsound_files[Globals::hitsound_selected];
    PlaySoundA(fullPath.c_str(), NULL,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
  } catch (...) {
  }
}

static bool s_deathsoundInitialized = false;
static std::unordered_map<uintptr_t, bool> s_ds_enemyAlive;

void Misc::InitDeathSound() {
  if (s_deathsoundInitialized)
    return;
  s_deathsoundInitialized = true;

  Globals::deathsound_files.clear();

  try {
    if (fs::exists(Globals::deathsound_dir) &&
        fs::is_directory(Globals::deathsound_dir)) {
      for (const auto &entry :
           fs::directory_iterator(Globals::deathsound_dir)) {
        if (!entry.is_regular_file())
          continue;

        std::string ext = entry.path().extension().string();
        for (auto &c : ext)
          c = (char)tolower(c);

        if (ext == ".wav") {
          Globals::deathsound_files.push_back(entry.path().filename().string());
        }
      }
    }
  } catch (...) {
  }
}

static void PlayDeathSoundFile(const std::string &filePath) {

  PlaySoundA(filePath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void Misc::DeathSoundUpdate() {
  if (!Globals::deathsound_enabled) {
    s_ds_enemyAlive.clear();
    return;
  }
  if (Globals::deathsound_selected < 0 ||
      Globals::deathsound_selected >= (int)Globals::deathsound_files.size())
    return;

  auto entities = EntityManager::Get().GetEntities();
  std::unordered_set<uintptr_t> seen;
  seen.reserve(entities.size());

  float now = Misc_GetTime();
  for (const auto &ent : entities) {
    if (!ent.isEnemy || !ent.pawn)
      continue;

    uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
    if (!Utils::IsValidPtr(pawnAddr))
      continue;

    seen.insert(pawnAddr);
    bool aliveNow = Utils::SafeAlive(ent.pawn);
    auto it = s_ds_enemyAlive.find(pawnAddr);
    bool wasAlive = (it == s_ds_enemyAlive.end()) ? aliveNow : it->second;

    if (wasAlive && !aliveNow && (now - s_lastLocalHitTime) <= 1.0f) {
      std::string fullPath =
          Globals::deathsound_dir + "\\" +
          Globals::deathsound_files[Globals::deathsound_selected];
      PlayDeathSoundFile(fullPath);
    }

    s_ds_enemyAlive[pawnAddr] = aliveNow;
  }

  for (auto it = s_ds_enemyAlive.begin(); it != s_ds_enemyAlive.end();) {
    if (seen.find(it->first) == seen.end())
      it = s_ds_enemyAlive.erase(it);
    else
      ++it;
  }
}

void Misc::ThirdPerson(void* input, unsigned int slot) {
  const bool requested =
      g_thirdPersonRequested.load(std::memory_order_acquire);

  if (!input || slot > 3) {
    if (!requested)
      Misc_SetCameraGateBypass(false);
    return;
  }

  const uintptr_t inputAddress = reinterpret_cast<uintptr_t>(input);
  const uintptr_t slotBase =
      inputAddress + static_cast<uintptr_t>(slot) * 0x928;

  __try {
    if (requested) {
      if (!Misc_SetCameraGateBypass(true))
        return;

      const bool active = Utils::SafeRead<bool>(slotBase + 0x229);
      const int mode = Utils::SafeRead<int>(slotBase + 0x6A8);
      if ((!active || mode != 0) && g_nativeThirdPersonInit)
        g_nativeThirdPersonInit(input, static_cast<int>(slot));

      Utils::SafeWrite<bool>(slotBase + 0x229, true);
      Utils::SafeWrite<int>(slotBase + 0x6A8, 0);
      Utils::SafeWrite<float>(
          slotBase + 0x238,
          g_thirdPersonDistance.load(std::memory_order_relaxed));
      if (!g_thirdPersonOwnsCamera) {
        Misc_SetPawnThirdPerson(true);
        g_thirdPersonOwnsCamera = true;
        Misc_LogThirdPerson("camera enabled through native slot initializer");
      }
    } else {
      if (g_thirdPersonOwnsCamera) {
        Misc_SetPawnThirdPerson(false);
        Utils::SafeWrite<bool>(slotBase + 0x229, false);
        Utils::SafeWrite<int>(slotBase + 0x6A8, 0);
        g_thirdPersonOwnsCamera = false;
        Misc_LogThirdPerson("camera disabled");
      }
      Misc_SetCameraGateBypass(false);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Misc_LogThirdPerson("runtime transition raised exception");
    if (!requested)
      Misc_SetCameraGateBypass(false);
  }
}

void Misc::PrepareThirdPerson() {
  Misc_ResolveThirdPerson();
}

void Misc::ShutdownThirdPerson() {
  g_thirdPersonRequested.store(false, std::memory_order_release);
  if (g_thirdPersonOwnsCamera)
    Misc_SetPawnThirdPerson(false);
  g_thirdPersonOwnsCamera = false;
  Misc_SetCameraGateBypass(false);
}

void Misc::SyncThirdPersonRequest() {
  g_thirdPersonDistance.store(
      std::clamp(Globals::thirdperson_distance, 30.0f, 240.0f),
      std::memory_order_relaxed);
  g_thirdPersonRequested.store(Globals::thirdperson_enabled,
                               std::memory_order_release);
}

void Misc::AntiFlash() {
  if (!Globals::antiflash_enabled)
    return;

  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;

    uintptr_t localPawnAddr =
        Memory::Globals::LocalPawn();
    if (!Utils::IsValidPtr(localPawnAddr) || localPawnAddr < 0x1000000)
      return;

    int health = Utils::SafeRead<int>(localPawnAddr + Offsets::m_iHealth);
    if (health <= 0)
      return;

    Utils::SafeWrite<float>(localPawnAddr + Offsets::m_flFlashDuration, 0.0f);
    Utils::SafeWrite<float>(localPawnAddr + Offsets::m_flFlashMaxAlpha, 0.0f);
    Utils::SafeWrite<float>(localPawnAddr + Offsets::m_flFlashScreenshotAlpha, 0.0f);
    Utils::SafeWrite<float>(localPawnAddr + Offsets::m_flFlashOverlayAlpha, 0.0f);
    Utils::SafeWrite<bool>(localPawnAddr + Offsets::m_bFlashBuildUp, false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void Misc::NoVisualRecoil() {
  if (!Globals::novisualrecoil_enabled)
    return;

  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;

    uintptr_t localPawnAddr =
        Memory::Globals::LocalPawn();
    if (!Utils::IsValidPtr(localPawnAddr))
      return;

    int health = Utils::SafeRead<int>(localPawnAddr + Offsets::m_iHealth);
    if (health <= 0)
      return;

    Vector zero = {0.0f, 0.0f, 0.0f};

    // 1. Aim Punch Services — zero the angles AND invalidate tick counters
    //    to prevent the prediction system from interpolating back
    uintptr_t aimPunchServices = Utils::SafeRead<uintptr_t>(
        localPawnAddr + Offsets::m_pAimPunchServices);
    if (Utils::IsValidPtr(aimPunchServices)) {
      // m_predictableBaseAngle (0x50) and velocity (0x5C)
      Utils::SafeWrite<Vector>(aimPunchServices + Offsets::m_vAimPunchAngle,
                               zero);
      Utils::SafeWrite<Vector>(aimPunchServices + Offsets::m_vAimPunchAngleVel,
                               zero);
      // m_unpredictableBaseAngle (0xA4)
      Utils::SafeWrite<Vector>(aimPunchServices + 0xA4, zero);

      // Zero the tick counters so interpolation doesn't snap back:
      // m_predictableBaseTick (0x48) — set to 0 to invalidate prediction
      Utils::SafeWrite<int>(aimPunchServices + 0x48, 0);
      // m_predictableBaseTickInterpAmount (0x4C) — set to 0
      Utils::SafeWrite<float>(aimPunchServices + 0x4C, 0.0f);
      // m_unpredictableBaseTick (0xA0) — set to 0
      Utils::SafeWrite<int>(aimPunchServices + 0xA0, 0);
    }

    // 2. Camera Services — zero the view punch angle AND its tick
    uintptr_t cameraServices =
        Utils::SafeRead<uintptr_t>(localPawnAddr + Offsets::m_pCameraServices);
    if (Utils::IsValidPtr(cameraServices)) {
      // m_vecCsViewPunchAngle (0x48)
      Utils::SafeWrite<Vector>(cameraServices + Offsets::m_vViewPunchAngle,
                               zero);
      // m_nCsViewPunchAngleTick (0x54) — set to 0 to invalidate
      Utils::SafeWrite<int>(cameraServices + 0x54, 0);
      // m_flCsViewPunchAngleTickRatio (0x58) — set to 0
      Utils::SafeWrite<float>(cameraServices + 0x58, 0.0f);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

namespace {
struct TonemapSnapshot {
  float exposureMin{};
  float exposureMax{};
  float speedUp{};
  float speedDown{};
  float smoothing{};
};

std::unordered_map<uintptr_t, TonemapSnapshot> g_worldTonemapSnapshots;
uintptr_t g_worldLastEntityList = 0;
ULONGLONG g_worldLastEntityScan = 0;

bool WorldGetEntityClassName(uintptr_t entity, char *buffer,
                             size_t bufferSize) {
  if (!entity || !buffer || bufferSize < 2 || !Utils::IsValidPtr(entity))
    return false;
  buffer[0] = '\0';

  // CEntityInstance::GetSchemaClassInfo is vfunc 44 in the current client.
  // Calling it is guarded both by pointer validation and SEH because entity
  // slots can disappear between an entity-list read and this call.
  __try {
    const uintptr_t vtable = Utils::SafeRead<uintptr_t>(entity);
    if (!Utils::IsValidPtr(vtable))
      return false;
    const uintptr_t getSchemaClassInfo =
        Utils::SafeRead<uintptr_t>(vtable + 44 * sizeof(uintptr_t));
    if (!Utils::IsValidPtr(getSchemaClassInfo))
      return false;

    void *classInfo = nullptr;
    reinterpret_cast<void(__fastcall *)(void *, void **)>(
        getSchemaClassInfo)(reinterpret_cast<void *>(entity), &classInfo);
    if (!classInfo ||
        !Utils::IsValidPtr(reinterpret_cast<uintptr_t>(classInfo)))
      return false;

    const uintptr_t className = Utils::SafeRead<uintptr_t>(
        reinterpret_cast<uintptr_t>(classInfo) + 0x8);
    return Utils::IsValidPtr(className) &&
           Utils::SafeReadString(className, buffer, bufferSize) &&
           buffer[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    buffer[0] = '\0';
    return false;
  }
}

bool WorldEntityIsTonemap(uintptr_t entity) {
  char name[96]{};
  if (!WorldGetEntityClassName(entity, name, sizeof(name)))
    return false;
  return std::strcmp(name, "C_TonemapController2") == 0 ||
         std::strcmp(
             name,
             "C_TonemapController2Alias_env_tonemap_controller2") == 0;
}

TonemapSnapshot WorldCaptureTonemap(uintptr_t entity) {
  TonemapSnapshot value{};
  value.exposureMin =
      Utils::SafeRead<float>(entity + Offsets::world_tonemap_exposure_min);
  value.exposureMax =
      Utils::SafeRead<float>(entity + Offsets::world_tonemap_exposure_max);
  value.speedUp =
      Utils::SafeRead<float>(entity + Offsets::world_tonemap_speed_up);
  value.speedDown =
      Utils::SafeRead<float>(entity + Offsets::world_tonemap_speed_down);
  value.smoothing =
      Utils::SafeRead<float>(entity + Offsets::world_tonemap_smoothing);
  return value;
}

void WorldApplyTonemap(uintptr_t entity) {
  if (!Utils::IsValidPtr(entity))
    return;
  const float range =
      std::max(0.0f, Globals::cinematic_post_exposure_range);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_exposure_min,
                          Globals::cinematic_post_exposure - range);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_exposure_max,
                          Globals::cinematic_post_exposure + range);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_speed_up,
                          std::max(0.01f, Globals::cinematic_post_adapt_up));
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_speed_down,
                          std::max(0.01f, Globals::cinematic_post_adapt_down));
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_smoothing,
                          std::max(0.0f, Globals::cinematic_post_smoothing));
}

void WorldRestoreTonemap(uintptr_t entity, const TonemapSnapshot &value) {
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_exposure_min,
                          value.exposureMin);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_exposure_max,
                          value.exposureMax);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_speed_up,
                          value.speedUp);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_speed_down,
                          value.speedDown);
  Utils::SafeWrite<float>(entity + Offsets::world_tonemap_smoothing,
                          value.smoothing);
}

void WorldRestoreTonemapSnapshots() {
  for (const auto &[entity, snapshot] : g_worldTonemapSnapshots) {
    if (WorldEntityIsTonemap(entity))
      WorldRestoreTonemap(entity, snapshot);
  }
  g_worldTonemapSnapshots.clear();
  Globals::cinematic_runtime_tonemap_count = 0;
}

void WorldForgetAllSnapshots() {
  g_worldTonemapSnapshots.clear();
  Globals::cinematic_runtime_tonemap_count = 0;
}

void CinematicTonemapUpdateImpl() {
    const uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;
    const uintptr_t entityList =
        Utils::SafeRead<uintptr_t>(client + Offsets::dwEntityList);
    if (!Utils::IsValidPtr(entityList)) {
      WorldForgetAllSnapshots();
      g_worldLastEntityList = 0;
      return;
    }

    // Never dereference snapshots belonging to a previous entity list. Pawn
    // changes are intentionally ignored here: they happen on respawn and must
    // not replace the map controller's true original values with our override.
    if (g_worldLastEntityList && g_worldLastEntityList != entityList) {
      WorldForgetAllSnapshots();
      g_worldLastEntityScan = 0;
    }
    g_worldLastEntityList = entityList;

    const ULONGLONG now = GetTickCount64();
    const bool needsScan =
        g_worldLastEntityScan == 0 || now - g_worldLastEntityScan >= 600;
    if (needsScan) {
      g_worldLastEntityScan = now;
      std::unordered_set<uintptr_t> seenTonemap;
      int tonemapCount = 0;

      for (int i = 0; i < 1024; ++i) {
        const uintptr_t entry = Utils::SafeRead<uintptr_t>(
            entityList + (8 * ((i & 0x7FFF) >> 9) + 16));
        if (!Utils::IsValidPtr(entry))
          continue;
        const uintptr_t entity = Utils::SafeRead<uintptr_t>(
            entry + 112 * (i & 0x1FF));
        if (!Utils::IsValidPtr(entity) || entity < 0x1000000)
          continue;

        char className[96]{};
        if (!WorldGetEntityClassName(entity, className, sizeof(className)))
          continue;
        if (
            std::strcmp(className, "C_TonemapController2") == 0 ||
            std::strcmp(
                className,
                "C_TonemapController2Alias_env_tonemap_controller2") == 0) {
          ++tonemapCount;
          seenTonemap.insert(entity);
          if (!g_worldTonemapSnapshots.contains(entity))
            g_worldTonemapSnapshots.emplace(entity,
                                             WorldCaptureTonemap(entity));
        }
      }

      Globals::cinematic_runtime_tonemap_count = tonemapCount;

      for (auto it = g_worldTonemapSnapshots.begin();
           it != g_worldTonemapSnapshots.end();) {
        it = seenTonemap.contains(it->first)
                 ? std::next(it)
                 : g_worldTonemapSnapshots.erase(it);
      }
    }

    // Reapply cached, class-verified objects every rendered frame. Networked
    // map values may be refreshed by the client between scans; this keeps the
    // result stable without repeating 1024 schema vfunc calls every frame.
    for (const auto &[entity, snapshot] : g_worldTonemapSnapshots)
      WorldApplyTonemap(entity);
}
} // namespace

void Misc::WorldManipulationUpdate() {
  if (!Globals::cinematic_post_enabled &&
      !g_worldTonemapSnapshots.empty())
    WorldRestoreTonemapSnapshots();
  if (!Globals::cinematic_post_enabled)
    return;

  __try {
    CinematicTonemapUpdateImpl();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Fail closed: a disappearing entity must never escape into Present.
  }
}

void Misc::CinematicPostProcessRender() {
  if (!Globals::cinematic_post_enabled)
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float x = Globals::GameViewportWidth > 1
                      ? Globals::GameViewportX
                      : 0.0f;
  const float y = Globals::GameViewportHeight > 1
                      ? Globals::GameViewportY
                      : 0.0f;
  const float width = Globals::GameViewportWidth > 1
                          ? static_cast<float>(Globals::GameViewportWidth)
                          : display.x;
  const float height = Globals::GameViewportHeight > 1
                           ? static_cast<float>(Globals::GameViewportHeight)
                           : display.y;
  if (width < 32.0f || height < 32.0f)
    return;

  const float intensity =
      std::clamp(Globals::cinematic_post_intensity, 0.0f, 1.0f);
  const float right = x + width;
  const float bottom = y + height;

  const int tintRed = std::clamp(
      static_cast<int>(Globals::cinematic_post_tint[0] * 255.0f), 0, 255);
  const int tintGreen = std::clamp(
      static_cast<int>(Globals::cinematic_post_tint[1] * 255.0f), 0, 255);
  const int tintBlue = std::clamp(
      static_cast<int>(Globals::cinematic_post_tint[2] * 255.0f), 0, 255);
  const int tintAlpha = std::clamp(
      static_cast<int>(Globals::cinematic_post_tint[3] * intensity * 255.0f),
      0, 96);
  if (tintAlpha > 0) {
    draw->AddRectFilled(ImVec2(x, y), ImVec2(right, bottom),
                        IM_COL32(tintRed, tintGreen, tintBlue, tintAlpha));
  }

  const float vignette =
      std::clamp(Globals::cinematic_post_vignette, 0.0f, 1.0f) * intensity;
  if (vignette > 0.001f) {
    const int edgeAlpha =
        std::clamp(static_cast<int>(210.0f * vignette), 0, 210);
    const ImU32 dark = IM_COL32(0, 0, 0, edgeAlpha);
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float edgeX = width * (0.12f + vignette * 0.12f);
    const float edgeY = height * (0.13f + vignette * 0.11f);

    draw->AddRectFilledMultiColor(
        ImVec2(x, y), ImVec2(x + edgeX, bottom),
        dark, clear, clear, dark);
    draw->AddRectFilledMultiColor(
        ImVec2(right - edgeX, y), ImVec2(right, bottom),
        clear, dark, dark, clear);
    draw->AddRectFilledMultiColor(
        ImVec2(x, y), ImVec2(right, y + edgeY),
        dark, dark, clear, clear);
    draw->AddRectFilledMultiColor(
        ImVec2(x, bottom - edgeY), ImVec2(right, bottom),
        clear, clear, dark, dark);
  }

  const float grain =
      std::clamp(Globals::cinematic_post_grain, 0.0f, 1.0f) * intensity;
  if (grain > 0.002f) {
    const uint32_t frameSeed =
        static_cast<uint32_t>(GetTickCount64() / 45u);
    const int grainCount =
        std::clamp(static_cast<int>(80.0f + grain * 520.0f), 80, 600);
    const int grainAlpha =
        std::clamp(static_cast<int>(10.0f + grain * 34.0f), 8, 44);

    auto hash = [](uint32_t value) {
      value ^= value >> 16;
      value *= 0x7FEB352Du;
      value ^= value >> 15;
      value *= 0x846CA68Bu;
      value ^= value >> 16;
      return value;
    };

    for (int i = 0; i < grainCount; ++i) {
      const uint32_t seed =
          hash(frameSeed ^ (static_cast<uint32_t>(i) * 0x9E3779B9u));
      const float px =
          x + static_cast<float>(seed & 0xFFFFu) / 65535.0f * width;
      const float py =
          y + static_cast<float>((seed >> 16) & 0xFFFFu) / 65535.0f * height;
      const int shade = (seed & 1u) ? 238 : 12;
      const float size = (seed & 8u) ? 1.25f : 0.85f;
      draw->AddRectFilled(
          ImVec2(px, py), ImVec2(px + size, py + size),
          IM_COL32(shade, shade, shade, grainAlpha));
    }
  }

  const float letterbox =
      std::clamp(Globals::cinematic_post_letterbox, 0.0f, 0.18f);
  if (letterbox > 0.0005f) {
    const float barHeight = height * letterbox;
    draw->AddRectFilled(ImVec2(x, y), ImVec2(right, y + barHeight),
                        IM_COL32(0, 0, 0, 255));
    draw->AddRectFilled(ImVec2(x, bottom - barHeight),
                        ImVec2(right, bottom), IM_COL32(0, 0, 0, 255));
  }
}

struct SmokeTimerInfo {
  std::chrono::steady_clock::time_point startTime;
  Vector position;
};
static std::map<uintptr_t, SmokeTimerInfo> s_smokeTimers;

void Misc::SmokeColorChanger() {
  if (!Globals::smoke_color_enabled && !Globals::nosmoke_enabled &&
      !Globals::smoketimer_enabled)
    return;

  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;

    uintptr_t entityList =
        Utils::SafeRead<uintptr_t>(client + Offsets::dwEntityList);
    if (!Utils::IsValidPtr(entityList))
      return;

    for (int i = 0; i < 1024; i++) {

      uintptr_t entry = Utils::SafeRead<uintptr_t>(
          entityList + (8 * ((i & 0x7FFF) >> 9) + 16));
      if (!Utils::IsValidPtr(entry))
        continue;

      uintptr_t entity =
          Utils::SafeRead<uintptr_t>(entry + (112 * (i & 0x1FF)));
      if (!Utils::IsValidPtr(entity) || entity < 0x1000000)
        continue;

      uintptr_t entityIdentity = Utils::SafeRead<uintptr_t>(entity + 0x10);
      if (!Utils::IsValidPtr(entityIdentity) || entityIdentity < 0x1000000)
        continue;

      uintptr_t designerNamePtr =
          Utils::SafeRead<uintptr_t>(entityIdentity + 0x20);
      if (!Utils::IsValidPtr(designerNamePtr))
        continue;

      char className[64] = {};
      if (!Utils::SafeReadString(designerNamePtr, className, sizeof(className)))
        continue;

      if (strcmp(className, "smokegrenade_projectile") != 0)
        continue;

      if (Globals::smoketimer_enabled) {
        int smokeTick =
            Utils::SafeRead<int>(entity + Offsets::m_nSmokeEffectTickBegin);
        bool didSmoke =
            Utils::SafeRead<bool>(entity + Offsets::m_bDidSmokeEffect);

        // If the game actually set the smoke tick, it means it popped!
        if (smokeTick > 0) {
          if (s_smokeTimers.find(entity) == s_smokeTimers.end()) {
            uintptr_t sceneNode =
                Utils::SafeRead<uintptr_t>(entity + Offsets::m_pGameSceneNode);
            if (sceneNode) {
              Vector origin =
                  Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
              s_smokeTimers[entity] = {std::chrono::steady_clock::now(),
                                       origin};
            }
          }
        }
      }

      if (Globals::nosmoke_enabled) {
        Utils::SafeWrite<bool>(entity + Offsets::m_bDidSmokeEffect, true);
        Utils::SafeWrite<int>(entity + Offsets::m_nSmokeEffectTickBegin, 0);
      }

      if (Globals::smoke_color_enabled) {
        Vector smokeColor = {Globals::smoke_color[0] * 255.f,
                             Globals::smoke_color[1] * 255.f,
                             Globals::smoke_color[2] * 255.f};

        Utils::SafeWrite<Vector>(entity + Offsets::m_vSmokeColor, smokeColor);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static bool ReadHitsoundData(int &outTotalHits) {
  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return false;

    uintptr_t localPawnAddr =
        Memory::Globals::LocalPawn();
    if (!Utils::IsValidPtr(localPawnAddr) || localPawnAddr < 0x1000000)
      return false;

    int health = Utils::SafeRead<int>(localPawnAddr + Offsets::m_iHealth);
    if (health <= 0)
      return false;

    uintptr_t pBulletServices =
        Utils::SafeRead<uintptr_t>(localPawnAddr + Offsets::m_pBulletServices);
    if (!Utils::IsValidPtr(pBulletServices) || pBulletServices < 0x1000000)
      return false;

    outTotalHits =
        Utils::SafeRead<int>(pBulletServices + Offsets::m_totalHitsOnServer);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Misc::HitManagerUpdate() {
  if (!Globals::hitsound_enabled && !Globals::hitmarker_enabled &&
      !Globals::native_kill_lightning_enabled)
    return;

  int totalHits = 0;
  if (!ReadHitsoundData(totalHits))
    return;

  if (totalHits != s_previousTotalHits) {
    if (totalHits == 0 && s_previousTotalHits != 0) {

    } else {
      if (totalHits > s_previousTotalHits)
        s_lastLocalHitTime = Misc_GetTime();

      if (Globals::hitsound_enabled && Globals::hitsound_selected >= 0)
        PlayHitsound();

      if (Globals::hitmarker_enabled) {
        s_hitmarkerAlpha = 255.f;
        s_hitmarkerStartTime = std::chrono::steady_clock::now();
      }
    }
  }

  s_previousTotalHits = totalHits;
}

void Misc::HitmarkerRender() {
  if (!Globals::hitmarker_enabled)
    return;

  if (s_hitmarkerAlpha > 0.f) {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_hitmarkerStartTime)
                        .count();

    if (duration >= 500) {
      s_hitmarkerAlpha = 0.f;
    } else {
      s_hitmarkerAlpha = 255.f * (1.f - (float)duration / 500.f);
    }

    if (s_hitmarkerAlpha > 0.f) {
      ImDrawList *dl = ImGui::GetBackgroundDrawList();
      ImVec2 center =
          ImVec2(Globals::GameViewportX + Globals::ScreenWidth / 2.f,
                 Globals::GameViewportY + Globals::ScreenHeight / 2.f);

      const float SIZE = 12.f;
      const float GAP = 4.f;

      ImU32 colBg = IM_COL32(0, 0, 0, (int)s_hitmarkerAlpha);

      ImU32 colFg = ImGui::ColorConvertFloat4ToU32(
          ImVec4(Globals::hitmarker_color[0], Globals::hitmarker_color[1],
                 Globals::hitmarker_color[2],
                 (s_hitmarkerAlpha / 255.f) * Globals::hitmarker_color[3]));

      dl->AddLine(ImVec2(center.x - SIZE, center.y - SIZE),
                  ImVec2(center.x - GAP, center.y - GAP), colBg, 3.0f);
      dl->AddLine(ImVec2(center.x - SIZE, center.y + SIZE),
                  ImVec2(center.x - GAP, center.y + GAP), colBg, 3.0f);
      dl->AddLine(ImVec2(center.x + SIZE, center.y - SIZE),
                  ImVec2(center.x + GAP, center.y - GAP), colBg, 3.0f);
      dl->AddLine(ImVec2(center.x + SIZE, center.y + SIZE),
                  ImVec2(center.x + GAP, center.y + GAP), colBg, 3.0f);

      dl->AddLine(ImVec2(center.x - SIZE, center.y - SIZE),
                  ImVec2(center.x - GAP, center.y - GAP), colFg, 1.5f);
      dl->AddLine(ImVec2(center.x - SIZE, center.y + SIZE),
                  ImVec2(center.x - GAP, center.y + GAP), colFg, 1.5f);
      dl->AddLine(ImVec2(center.x + SIZE, center.y - SIZE),
                  ImVec2(center.x + GAP, center.y - GAP), colFg, 1.5f);
      dl->AddLine(ImVec2(center.x + SIZE, center.y + SIZE),
                  ImVec2(center.x + GAP, center.y + GAP), colFg, 1.5f);
    }
  }
}

struct SpectatorData {
  struct SpectatorInfo {
    std::string name;
    uint64_t steamID;
  };
  std::vector<SpectatorInfo> spectators;
};

static SpectatorData s_specData;

static uintptr_t ResolveHandle(uintptr_t entityListAddr, uint32_t handle) {
  __try {
    if (!handle || handle == 0xFFFFFFFF || !entityListAddr)
      return 0;

    uintptr_t listPtr = Utils::SafeRead<uintptr_t>(entityListAddr);
    if (!Utils::IsValidPtr(listPtr))
      return 0;

    uintptr_t entry = Utils::SafeRead<uintptr_t>(
        listPtr + (8 * ((handle & 0x7FFF) >> 9) + 16));
    if (!Utils::IsValidPtr(entry))
      return 0;

    uintptr_t entPtr =
        Utils::SafeRead<uintptr_t>(entry + (112 * (handle & 0x1FF)));
    if (!Utils::IsValidPtr(entPtr))
      return 0;

    return entPtr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

static uintptr_t GetObserverTarget(uintptr_t pawnAddr) {
  __try {
    if (!pawnAddr || !Utils::IsValidPtr(pawnAddr))
      return 0;

    uintptr_t observerServices =
        Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pObserverServices);
    if (!Utils::IsValidPtr(observerServices))
      return 0;

    uint32_t targetHandle = Utils::SafeRead<uint32_t>(
        observerServices + Offsets::m_hObserverTarget);
    if (!targetHandle || targetHandle == 0xFFFFFFFF)
      return 0;

    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return 0;
    uintptr_t entityListAddr = client + Offsets::dwEntityList;

    return ResolveHandle(entityListAddr, targetHandle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

static bool ReadPlayerName(uintptr_t controllerAddr, char *outBuf,
                           size_t maxLen) {
  __try {
    if (!controllerAddr || !Utils::IsValidPtr(controllerAddr))
      return false;

    uintptr_t nameBufferAddr = controllerAddr + Offsets::m_iszPlayerName;
    return Utils::SafeReadString(nameBufferAddr, outBuf, maxLen);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Misc::SpectatorListUpdate() {
  s_specData.spectators.clear();

  if (!Globals::speclist_enabled)
    return;

  try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;

    uintptr_t entityListAddr = client + Offsets::dwEntityList;
    uintptr_t listPtr = Utils::SafeRead<uintptr_t>(entityListAddr);
    if (!listPtr)
      return;

    uintptr_t localCtrlAddr = Utils::SafeRead<uintptr_t>(client + Offsets::dwLocalPlayerController);
    if (!localCtrlAddr || !Utils::IsValidPtr(localCtrlAddr))
      return;

    // Resolve local player pawn from controller first, fallback to dwLocalPlayerPawn
    uintptr_t localPawnAddr = 0;
    uint32_t localPawnHandle = Utils::SafeRead<uint32_t>(localCtrlAddr + Offsets::m_hPlayerPawn);
    if (localPawnHandle && localPawnHandle != 0xFFFFFFFF)
      localPawnAddr = ResolveHandle(entityListAddr, localPawnHandle);

    if (!localPawnAddr)
      localPawnAddr = Memory::Globals::LocalPawn();

    // Check if local player is alive
    bool localAlive = Utils::SafeRead<bool>(localCtrlAddr + Offsets::m_bPawnIsAlive, false);
    uintptr_t spectatedPawn = 0;
    if (!localAlive) {
      // If we are dead, resolve our local observer pawn to find our spectated target
      uint32_t localObsHandle = Utils::SafeRead<uint32_t>(
          localCtrlAddr + Offsets::m_hObserverPawn);
      uintptr_t localObsPawn = 0;
      if (localObsHandle && localObsHandle != 0xFFFFFFFF)
        localObsPawn = ResolveHandle(entityListAddr, localObsHandle);

      if (localObsPawn) {
        spectatedPawn = GetObserverTarget(localObsPawn);
      } else if (localPawnAddr && Utils::IsValidPtr(localPawnAddr)) {
        spectatedPawn = GetObserverTarget(localPawnAddr);
      }
    }

    for (int i = 0; i < 64; i++) {
      uintptr_t listEntry = Utils::SafeRead<uintptr_t>(listPtr + ((8 * (i & 0x7FFF) >> 9) + 16));
      if (!Utils::IsValidPtr(listEntry))
        continue;

      uintptr_t controllerPtr = Utils::SafeRead<uintptr_t>(listEntry + 112 * (i & 0x1FF));
      if (!Utils::IsValidPtr(controllerPtr))
        continue;

      if (controllerPtr == localCtrlAddr)
        continue;

      // Check if the spectator controller itself is alive (alive players don't spectate)
      bool pawnIsAlive = Utils::SafeRead<bool>(controllerPtr + Offsets::m_bPawnIsAlive, false);
      if (pawnIsAlive)
        continue;

      // Retrieve the observer pawn of this dead player controller
      uint32_t obsPawnHandle = Utils::SafeRead<uint32_t>(
          controllerPtr + Offsets::m_hObserverPawn);
      uintptr_t resolvedPawn = 0;
      if (obsPawnHandle && obsPawnHandle != 0xFFFFFFFF)
        resolvedPawn = ResolveHandle(entityListAddr, obsPawnHandle);

      if (!resolvedPawn)
        continue;

      uintptr_t specTarget = GetObserverTarget(resolvedPawn);
      if (!specTarget)
        continue;

      bool isSpectatingUs = (localAlive && localPawnAddr != 0 && specTarget == localPawnAddr);
      bool isSpectatingOurTarget = (!localAlive && spectatedPawn != 0 && specTarget == spectatedPawn);

      if (isSpectatingUs || isSpectatingOurTarget) {
        char nameBuf[128] = {};
        if (ReadPlayerName(controllerPtr, nameBuf, sizeof(nameBuf)) && nameBuf[0] != '\0') {
          uint64_t steamID = Utils::SafeRead<uint64_t>(controllerPtr + Offsets::m_steamID);
          s_specData.spectators.push_back({ std::string(nameBuf), steamID });
        }
      }
    }
  } catch (...) {
  }
}

// External fonts loaded in Menu.cpp::Initialize.
extern ImFont *widget_font;
extern ImFont *widget_font_big;

void Misc::SpectatorListRender() {
  if (!Globals::speclist_enabled) return;

  // ==================================================================
  //  Modern Spectator List — crimson-accented dark panel, draggable.
  //  Circular avatars, alternating row tint, count badge in red.
  // ==================================================================
  ImFont *titleFont = widget_font_big ? widget_font_big : ImGui::GetFont();
  ImFont *bodyFont  = widget_font     ? widget_font     : ImGui::GetFont();
  const float scale = HudScale();
  const float titleSize = 14.f * scale;
  const float bodySize  = 13.f * scale;

  const float pad      = 14.f * scale;
  const float headerH  = 34.f * scale;
  const float rowH     = 32.f * scale;
  const float avatarSz = 22.f * scale;
  const float panelW   = 248.f * scale;
  const size_t count   = s_specData.spectators.size();
  const float bodyH    = count == 0 ? 30.f * scale
                                    : (float)count * rowH + 6.f * scale;
  const float panelH   = headerH + 1.f + bodyH + 8.f * scale;
  const float rounding = 8.f * scale;

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize   |
      ImGuiWindowFlags_NoScrollbar|
      ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoBackground;

  ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
  ImGui::SetNextWindowPos(
      ImVec2(HudViewportRight() - panelW - 14.f * scale,
             HudRightStackY()),
      ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

  ImGui::Begin("##spec_list", nullptr, flags);

  ImVec2 winPos = ImGui::GetWindowPos();
  const float panelX = winPos.x;
  const float panelY = winPos.y;
  ImDrawList *dl = ImGui::GetWindowDrawList();

  // -- Soft drop shadow --
  for (int s = 10; s > 0; --s) {
    int a = (11 - s) * 4;
    dl->AddRect(
        ImVec2(panelX - s, panelY - s + 2.f),
        ImVec2(panelX + panelW + s, panelY + panelH + s + 2.f),
        IM_COL32(0, 0, 0, a), rounding + s, 0, 1.f);
  }

  // -- Panel body --
  dl->AddRectFilled(
      ImVec2(panelX, panelY),
      ImVec2(panelX + panelW, panelY + panelH),
      HudColor(14, 15, 18, 238), rounding);
  dl->AddRect(
      ImVec2(panelX, panelY),
      ImVec2(panelX + panelW, panelY + panelH),
      HudColor(50, 52, 60, 220), rounding, 0, 1.f);

  // Top crimson rail (1.5 px) — like the watermark.
  dl->AddRectFilled(
      ImVec2(panelX + rounding * 0.5f, panelY),
      ImVec2(panelX + panelW - rounding * 0.5f, panelY + 1.5f),
      HudAccent(0.94f));

  // -- Header --
  // Pulsing red dot + "SPECTATORS" label.
  {
    float t = (float)ImGui::GetTime();
    float pulse = 0.55f + 0.45f * (0.5f + 0.5f * sinf(t * 2.4f));
    ImVec2 dotC(panelX + pad + 3.f, panelY + headerH * 0.5f);
    dl->AddCircleFilled(dotC, 5.5f,
                        HudAccent(0.27f * pulse), 18);
    dl->AddCircleFilled(dotC, 2.8f, HudAccent(), 16);
  }
  const char *title = "SPECTATORS";
  ImVec2 titleSz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.f, title);
  dl->AddText(titleFont, titleSize,
              ImVec2(panelX + pad + 16.f,
                     panelY + (headerH - titleSz.y) * 0.5f),
              IM_COL32(242, 244, 250, 255), title);

  // Count badge — crimson when populated, dim when empty.
  char countStr[8];
  snprintf(countStr, sizeof(countStr), "%zu", count);
  ImVec2 cntSz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.f, countStr);
  float badgeW = (std::max)(cntSz.x + 16.f, 26.f);
  float badgeH = 20.f;
  ImVec2 badgeP(panelX + panelW - pad - badgeW,
                panelY + (headerH - badgeH) * 0.5f);
  ImU32 badgeBg = count > 0
      ? HudAccent(0.88f)
      : HudColor(40, 42, 50, 230);
  ImU32 badgeFg = count > 0
      ? IM_COL32(255, 255, 255, 255)
      : IM_COL32(150, 152, 162, 255);
  dl->AddRectFilled(badgeP, ImVec2(badgeP.x + badgeW, badgeP.y + badgeH),
                    badgeBg, badgeH * 0.5f);
  dl->AddText(titleFont, titleSize,
              ImVec2(badgeP.x + (badgeW - cntSz.x) * 0.5f,
                     badgeP.y + (badgeH - cntSz.y) * 0.5f),
              badgeFg, countStr);

  // Header / body separator.
  dl->AddRectFilled(
      ImVec2(panelX + pad, panelY + headerH),
      ImVec2(panelX + panelW - pad, panelY + headerH + 1.f),
      IM_COL32(40, 42, 50, 220));

  // -- Body --
  float rowY = panelY + headerH + 6.f;
  if (count == 0) {
    const char *empty = "No spectators";
    ImVec2 sz = bodyFont->CalcTextSizeA(bodySize, FLT_MAX, 0.f, empty);
    dl->AddText(bodyFont, bodySize,
                ImVec2(panelX + (panelW - sz.x) * 0.5f, rowY + 6.f),
                IM_COL32(110, 113, 122, 200), empty);
  } else {
    int idx = 0;
    for (const auto &spec : s_specData.spectators) {
      // Alternating row tint for readability.
      if ((idx & 1) == 0) {
        dl->AddRectFilled(
            ImVec2(panelX + 6.f, rowY + 1.f),
            ImVec2(panelX + panelW - 6.f, rowY + rowH - 1.f),
            IM_COL32(255, 255, 255, 6), 4.f);
      }

      ID3D11ShaderResourceView *av =
          SteamAvatarCache::Get().GetAvatar(spec.steamID);
      ImVec2 avTL(panelX + pad, rowY + (rowH - avatarSz) * 0.5f);
      ImVec2 avBR(avTL.x + avatarSz, avTL.y + avatarSz);
      ImVec2 avC ((avTL.x + avBR.x) * 0.5f, (avTL.y + avBR.y) * 0.5f);
      float  avR = avatarSz * 0.5f;

      if (av) {
        // Round mask: draw the image and overlay a circular ring.
        // ImGui can't truly mask, but stacking a same-bg circle border
        // around the square gives a clean round look at small sizes.
        dl->AddImageRounded(
            (ImTextureID)av, avTL, avBR,
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, 255), avR);
        dl->AddCircle(avC, avR, HudAccent(0.80f), 24, 1.2f);
      } else {
        dl->AddCircleFilled(avC, avR, IM_COL32(40, 42, 50, 255), 24);
        dl->AddCircle      (avC, avR, HudAccent(0.72f), 24, 1.2f);
        char init[2] = { (spec.name[0]
            ? (char)(spec.name[0] >= 'a' && spec.name[0] <= 'z'
                ? spec.name[0] - 32 : spec.name[0])
            : '?'), 0 };
        ImVec2 iSz = bodyFont->CalcTextSizeA(bodySize, FLT_MAX, 0.f, init);
        dl->AddText(bodyFont, bodySize,
                    ImVec2(avC.x - iSz.x * 0.5f, avC.y - iSz.y * 0.5f),
                    IM_COL32(220, 222, 230, 255), init);
      }

      ImVec2 nmSz = bodyFont->CalcTextSizeA(
          bodySize, FLT_MAX, 0.f, spec.name.c_str());
      dl->AddText(bodyFont, bodySize,
                  ImVec2(avBR.x + 12.f, rowY + (rowH - nmSz.y) * 0.5f),
                  IM_COL32(230, 232, 240, 255), spec.name.c_str());

      rowY += rowH;
      ++idx;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}

static bool s_bt_active = false;
static bool s_bt_wasTicking = false;
static std::chrono::steady_clock::time_point s_bt_plantTime;
static int s_bt_bombSite = -1;

static bool s_bt_defusing = false;
static bool s_bt_wasDefusing = false;
static bool s_bt_defuserHasKit = false;
static std::chrono::steady_clock::time_point s_bt_defuseStartTime;

static constexpr float BOMB_TIMER = 40.f;
static constexpr float DEFUSE_TIME_KIT = 5.f;
static constexpr float DEFUSE_TIME_NOKIT = 10.f;

struct BombState {
  bool valid = false;
  bool ticking = false;
  bool defused = false;
  bool beingDefused = false;
  bool defuserHasKit = false;
  int bombSite = -1;
  uintptr_t bombAddress = 0;
  float c4BlowTime = 0.f;
};

static BombState ReadBombState() {
  BombState state = {};

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return state;

  uintptr_t pBombSystem =
      Utils::SafeRead<uintptr_t>(client + Offsets::dwPlantedC4);
  if (!Utils::IsValidPtr(pBombSystem))
    return state;

  uintptr_t bomb = Utils::SafeRead<uintptr_t>(pBombSystem);
  if (!Utils::IsValidPtr(bomb) || bomb < 0x1000000)
    return state;

  state.bombAddress = bomb;

  bool ticking = Utils::SafeRead<bool>(bomb + Offsets::m_bBombTicking);
  bool defused = Utils::SafeRead<bool>(bomb + Offsets::m_bBombDefused);

  state.valid = true;
  state.ticking = ticking;
  state.defused = defused;

  state.c4BlowTime = Utils::SafeRead<float>(bomb + Offsets::m_flC4Blow);

  if (!state.ticking)
    return state;

  state.bombSite = Utils::SafeRead<int>(bomb + Offsets::m_nBombSite);

  bool beingDefused = Utils::SafeRead<bool>(bomb + Offsets::m_bBeingDefused);
  state.beingDefused = beingDefused;

  if (beingDefused) {

    uint32_t defuserHandle =
        Utils::SafeRead<uint32_t>(bomb + Offsets::m_hBombDefuser);
    if (defuserHandle && defuserHandle != 0xFFFFFFFF) {

      C_BaseEntity *defuserEnt =
          EntityManager::Get().GetEntityFromHandle(defuserHandle);
      uintptr_t defuserAddr = reinterpret_cast<uintptr_t>(defuserEnt);
      if (Utils::IsValidPtr(defuserAddr) && defuserAddr > 0x1000000) {

        uintptr_t itemServices =
            Utils::SafeRead<uintptr_t>(defuserAddr + Offsets::m_pItemServices);
        if (Utils::IsValidPtr(itemServices)) {

          state.defuserHasKit =
              Utils::SafeRead<bool>(itemServices + Offsets::m_bHasDefuser);
        }
      }
    }
  }

  return state;
}

static uintptr_t s_bt_lastBombAddress = 0;
static float s_bt_lastC4BlowTime = 0.f;

void Misc::BombTimerUpdate() {
  if (!Globals::bombtimer_enabled) {
    s_bt_active = false;
    s_bt_wasDefusing = false;
    return;
  }

  BombState state = ReadBombState();

  if (!state.valid || !state.ticking || state.defused) {
    s_bt_active = false;
    s_bt_defusing = false;
    s_bt_wasDefusing = false;
    return;
  }

  auto now = std::chrono::steady_clock::now();

  if (state.bombAddress != s_bt_lastBombAddress ||
      state.c4BlowTime != s_bt_lastC4BlowTime) {
    s_bt_lastBombAddress = state.bombAddress;
    s_bt_lastC4BlowTime = state.c4BlowTime;

    s_bt_plantTime = now;
    s_bt_bombSite = state.bombSite;
    s_bt_active = true;
    s_bt_defusing = false;
    s_bt_wasDefusing = false;
  }

  if (!s_bt_active)
    return;

  float elapsed = std::chrono::duration<float>(now - s_bt_plantTime).count();
  if (elapsed >= BOMB_TIMER) {
    s_bt_active = false;
    s_bt_wasTicking = false;
    return;
  }

  if (state.bombSite >= 0)
    s_bt_bombSite = state.bombSite;

  if (state.beingDefused) {
    if (!s_bt_wasDefusing) {

      s_bt_defuseStartTime = now;
      s_bt_defuserHasKit = state.defuserHasKit;
    }
    s_bt_defusing = true;
    s_bt_wasDefusing = true;
  } else {

    s_bt_defusing = false;
    s_bt_wasDefusing = false;
  }
}

// -----------------------------------------------------------------
//  Small stylized C4 icon, drawn programmatically so we don't need
//  to ship a texture. Sits inside the bomb-timer ring.
//
//   antenna
//      │
//   ┌──┴──┐
//   │░░░░░│  ← display strip (subtle)
//   │▓▓▓▓▓│  ← body
//   │▓▓▓▓▓│
//   │  ●  │  ← pulsing red LED
//   └─────┘
// -----------------------------------------------------------------
static void DrawC4Icon(ImDrawList *dl, ImVec2 center, float size, ImU32 bodyCol,
                       ImU32 outlineCol, ImU32 ledCol) {
  // body
  float w = size * 0.78f, h = size * 0.90f;
  ImVec2 tl(center.x - w * 0.5f, center.y - h * 0.5f);
  ImVec2 br(center.x + w * 0.5f, center.y + h * 0.5f);
  dl->AddRectFilled(tl, br, bodyCol, 3.f);
  dl->AddRect      (tl, br, outlineCol, 3.f, 0, 1.0f);

  // subtle "display strip" near the top of the body — gives the
  // little block a recognisable silhouette without any text.
  ImVec2 stTl(tl.x + w * 0.16f, tl.y + h * 0.16f);
  ImVec2 stBr(br.x - w * 0.16f, tl.y + h * 0.36f);
  dl->AddRectFilled(stTl, stBr,
                    IM_COL32(20, 22, 26, 220), 1.5f);
  dl->AddRect(stTl, stBr, outlineCol, 1.5f, 0, 1.0f);

  // tiny antenna sticking up from the top-center
  float antX = center.x;
  dl->AddLine(ImVec2(antX, tl.y),
              ImVec2(antX, tl.y - size * 0.28f),
              outlineCol, 1.4f);
  dl->AddCircleFilled(ImVec2(antX, tl.y - size * 0.30f), 1.8f,
                      outlineCol, 10);

  // pulsing red LED at the body's bottom-center
  float t = (float)ImGui::GetTime();
  float pulse = 0.5f + 0.5f * sinf(t * 4.5f);
  ImVec2 led(center.x, br.y - h * 0.18f);
  dl->AddCircleFilled(led, 3.0f,
                      IM_COL32(240, 52, 62, (int)(130.f * pulse)), 16);
  dl->AddCircleFilled(led, 1.7f, ledCol, 14);
}

void Misc::BombTimerRender() {
  if (!Globals::bombtimer_enabled || !s_bt_active) return;

  ImDrawList *dl = ImGui::GetBackgroundDrawList();
  if (!dl) return;

  auto now = std::chrono::steady_clock::now();
  float elapsed = std::chrono::duration<float>(now - s_bt_plantTime).count();
  float remaining = BOMB_TIMER - elapsed;
  if (remaining <= 0.f) { s_bt_active = false; return; }

  // ================================================================
  //  Circular Bomb Timer — top-center
  //
  //  Outer ring   = countdown progress (crimson → deep red)
  //  Inner ring   = defuse progress (white, only while defusing)
  //  Center       = stylized C4 icon + seconds remaining underneath
  //  Bottom chip  = bombsite (A / B)
  // ================================================================
  ImFont *fSm = widget_font     ? widget_font     : ImGui::GetFont();
  ImFont *fLg = widget_font_big ? widget_font_big : ImGui::GetFont();
  const float scale = HudScale();
  const float fSmSize = (fSm ? fSm->FontSize : 14.f) * scale;
  const float fLgSize = (fLg ? fLg->FontSize : 22.f) * scale;

  const float radius   = 56.f * scale; // outer ring radius
  const float thick    = 7.f * scale;  // ring thickness
  const float gap      = 4.f * scale;  // gap between outer and inner ring
  const float innerR   = radius - thick - gap;
  const float innerTh  = 4.f * scale;

  const float cx = Globals::GameViewportX +
                   Globals::GameViewportWidth * 0.5f;
  const float cy = Globals::GameViewportY + 28.f * scale + radius;
  const ImVec2 ctr(cx, cy);

  // Time-fraction (1.0 → just planted, 0.0 → boom).
  float frac = std::clamp(remaining / BOMB_TIMER, 0.f, 1.f);

  // Ring color shifts from crimson → deep red as time runs out.
  ImU32 ringCol = remaining < 10.f
      ? HudColor(255, 60, 70, 255)
      : HudAccent();
  ImU32 ringGlow = remaining < 10.f
      ? HudColor(255, 60, 70, 90)
      : HudAccent(0.35f);
  ImU32 trackCol = HudColor(28, 30, 36, 240);
  ImU32 bodyBg   = HudColor(14, 15, 18, 235);

  // -- Disc body --
  // Soft outer halo (more intense when nearing detonation)
  float haloA = 60.f + 90.f * (1.f - frac);
  if (remaining < 10.f) {
    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 8.f);
    haloA += pulse * 60.f;
  }
  dl->AddCircleFilled(ctr, radius + 6.f * scale,
                      remaining < 10.f
                          ? HudColor(255, 60, 70, (int)haloA)
                          : HudAccent(haloA / 255.f),
                      64);
  // Body fill
  dl->AddCircleFilled(ctr, radius - 1.f, bodyBg, 64);

  // -- Track (full ring, dim) --
  dl->AddCircle(ctr, radius - thick * 0.5f, trackCol, 96, thick);

  // -- Progress arc (counter-clockwise from 12 o'clock) --
  {
    const int   seg   = 96;
    const float start = -3.14159265f * 0.5f;
    const float end   = start + 3.14159265f * 2.f * frac;
    dl->PathArcTo(ctr, radius - thick * 0.5f, start, end, seg);
    dl->PathStroke(ringCol, 0, thick);

    // Glow trail just outside the active arc
    dl->PathArcTo(ctr, radius - thick * 0.5f, start, end, seg);
    dl->PathStroke(ringGlow, 0, thick + 3.f);
  }

  // -- Defuse arc (inner ring) --
  if (s_bt_defusing) {
    float defuseTime = s_bt_defuserHasKit ? DEFUSE_TIME_KIT
                                          : DEFUSE_TIME_NOKIT;
    float defuseEl   = std::chrono::duration<float>(now - s_bt_defuseStartTime)
                           .count();
    float defuseFrac = std::clamp(defuseEl / defuseTime, 0.f, 1.f);
    float defuseRem  = (std::max)(0.f, defuseTime - defuseEl);
    bool  canDefuse  = defuseRem < remaining;

    ImU32 inTrk = IM_COL32(28, 30, 36, 240);
    ImU32 inCol = canDefuse
        ? IM_COL32(80, 220, 255, 255)
        : IM_COL32(255, 90, 90, 255);
    dl->AddCircle(ctr, innerR - innerTh * 0.5f, inTrk, 80, innerTh);
    {
      const int   seg   = 80;
      const float start = -3.14159265f * 0.5f;
      const float end   = start + 3.14159265f * 2.f * defuseFrac;
      dl->PathArcTo(ctr, innerR - innerTh * 0.5f, start, end, seg);
      dl->PathStroke(inCol, 0, innerTh);
    }
  }

  // -- Center: C4 icon + seconds --
  // Icon sits slightly above center to leave room for the number.
  float iconSize = innerR * 0.62f;
  ImVec2 iconC(ctr.x, ctr.y - iconSize * 0.20f);
  DrawC4Icon(dl, iconC, iconSize,
             IM_COL32(46, 48, 56, 255),     // body
             IM_COL32(110, 115, 125, 255),  // outline
             IM_COL32(255, 245, 245, 255)); // led core

  // Seconds remaining (big), under the icon.
  char timerText[16];
  if (remaining >= 10.f) snprintf(timerText, sizeof(timerText), "%.1f", remaining);
  else                   snprintf(timerText, sizeof(timerText), "%.2f", remaining);
  ImVec2 tSz = fLg->CalcTextSizeA(fLgSize, FLT_MAX, 0.f, timerText);
  ImVec2 tPos(ctr.x - tSz.x * 0.5f, ctr.y + iconSize * 0.45f);
  dl->AddText(fLg, fLgSize, ImVec2(tPos.x + 1.f, tPos.y + 1.f),
              IM_COL32(0, 0, 0, 220), timerText);
  dl->AddText(fLg, fLgSize, tPos, ringCol, timerText);

  // -- Site chip below the ring --
  const char *siteLetter = s_bt_bombSite == 0 ? "BOMBSITE  A"
                          : s_bt_bombSite == 1 ? "BOMBSITE  B"
                          : "BOMBSITE  ?";
  ImU32 chipBg = HudColor(20, 22, 26, 235);
  ImU32 chipBd = HudAccent(0.90f);
  ImVec2 sSz = fSm->CalcTextSizeA(fSmSize, FLT_MAX, 0.f, siteLetter);
  float chipW = sSz.x + 22.f * scale;
  float chipH = sSz.y + 8.f * scale;
  ImVec2 chipP(ctr.x - chipW * 0.5f,
               ctr.y + radius + 8.f * scale);
  dl->AddRectFilled(chipP, ImVec2(chipP.x + chipW, chipP.y + chipH),
                    chipBg, chipH * 0.5f);
  dl->AddRect      (chipP, ImVec2(chipP.x + chipW, chipP.y + chipH),
                    chipBd, chipH * 0.5f, 0, 1.2f);
  dl->AddText(fSm, fSmSize,
              ImVec2(chipP.x + (chipW - sSz.x) * 0.5f,
                     chipP.y + (chipH - sSz.y) * 0.5f),
              IM_COL32(240, 242, 248, 255), siteLetter);

  // -- Defuse status line under the chip --
  if (s_bt_defusing) {
    float defuseTime = s_bt_defuserHasKit ? DEFUSE_TIME_KIT
                                          : DEFUSE_TIME_NOKIT;
    float defuseEl   = std::chrono::duration<float>(now - s_bt_defuseStartTime)
                           .count();
    float defuseRem  = (std::max)(0.f, defuseTime - defuseEl);
    bool  canDefuse  = defuseRem < remaining;

    char defuseText[64];
    snprintf(defuseText, sizeof(defuseText),
             "DEFUSING %.1fs  %s",
             defuseRem,
             canDefuse ? "(IN TIME)" : "(TOO LATE)");
    ImU32 defCol = canDefuse
        ? IM_COL32(80, 220, 255, 255)
        : IM_COL32(255, 90, 90, 255);
    ImVec2 dSz = fSm->CalcTextSizeA(fSmSize, FLT_MAX, 0.f, defuseText);
    dl->AddText(fSm, fSmSize,
                ImVec2(ctr.x - dSz.x * 0.5f,
                       chipP.y + chipH + 6.f),
                defCol, defuseText);
  }
}

struct BulletTrace {
  float startPos[3];
  float endPos[3];
  float spawnTime;
  float totalDist;
};

static std::vector<BulletTrace> s_bt_traces;
static std::mutex s_bt_traceMutex;
static int s_bt_lastShotsFired = 0;

static constexpr float BT_RAY_LENGTH = 8000.0f;
static constexpr float BT_BULLET_SPEED = 8000.0f;

static float BT_GetTime() { return Misc_GetTime(); }

static bool BT_ReadAndDetectShot() {
  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return false;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!Utils::IsValidPtr(localPawnAddr) || localPawnAddr < 0x1000000)
    return false;

  int currentShots =
      Utils::SafeRead<int>(localPawnAddr + Offsets::m_iShotsFired);

  if (currentShots > s_bt_lastShotsFired && s_bt_lastShotsFired >= 0) {
    s_bt_lastShotsFired = currentShots;

    uintptr_t sceneNode =
        Utils::SafeRead<uintptr_t>(localPawnAddr + Offsets::m_pGameSceneNode);
    if (!Utils::IsValidPtr(sceneNode))
      return false;

    Vector origin =
        Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
    Vector viewOffset =
        Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecViewOffset);
    float eyeX = origin.x + viewOffset.x;
    float eyeY = origin.y + viewOffset.y;
    float eyeZ = origin.z + viewOffset.z;

    if (eyeX == 0.f && eyeY == 0.f && eyeZ == 0.f)
      return false;

    float pitch = 0.f, yaw = 0.f;
    uintptr_t viewAnglesAddr = client + Offsets::dwViewAngles;
    pitch = Utils::SafeRead<float>(viewAnglesAddr);
    yaw = Utils::SafeRead<float>(viewAnglesAddr + 4);

    // Apply weapon recoil (aim punch) to get the actual bullet direction.
    //
    // CS2 splits recoil into two angles:
    //   m_vViewPunchAngle  — purely visual kick, displayed at *2.0 scale.
    //                        Bullets DO NOT use this.
    //   m_vAimPunchAngle   — applied 1:1 to the actual bullet direction.
    //
    // The previous build multiplied aim-punch by 2.0, which made traces
    // follow the *camera kick* instead of the real bullet path.
    // The correct shoot-angles formula is:
    //
    //     shootAngles = viewAngles + m_vAimPunchAngle
    Vector aimPunch = {0, 0, 0};
    uintptr_t aimPunchServices = Utils::SafeRead<uintptr_t>(
        localPawnAddr + Offsets::m_pAimPunchServices);
    if (Utils::IsValidPtr(aimPunchServices)) {
      aimPunch =
          Utils::SafeRead<Vector>(aimPunchServices + Offsets::m_vAimPunchAngle);
    }
    pitch += aimPunch.x;
    yaw   += aimPunch.y;

    constexpr float PI_F = 3.14159265358979323846f;
    constexpr float D2R = PI_F / 180.0f;
    float cp = cosf(pitch * D2R);
    float sp = sinf(pitch * D2R);
    float cy = cosf(yaw * D2R);
    float sy = sinf(yaw * D2R);
    float dirX = cp * cy;
    float dirY = cp * sy;
    float dirZ = -sp;

    // Offset the starting point to simulate weapon muzzle (down and to the
    // right)
    float rcy = cosf((yaw - 90.0f) * D2R);
    float rsy = sinf((yaw - 90.0f) * D2R);
    Vector rightVec(rcy, rsy, 0.0f);

    Vector startPos(eyeX, eyeY, eyeZ);
    startPos = startPos + rightVec * 6.0f;
    startPos.z -= 4.0f;

    Vector endPos(eyeX + dirX * BT_RAY_LENGTH, eyeY + dirY * BT_RAY_LENGTH,
                  eyeZ + dirZ * BT_RAY_LENGTH);

    // Stop at walls
    Vector hitPoint;
    if (Raycasting::Get().TraceRay(Vector(eyeX, eyeY, eyeZ), endPos,
                                   hitPoint)) {
      endPos = hitPoint;
    }

    BulletTrace t;
    t.startPos[0] = startPos.x;
    t.startPos[1] = startPos.y;
    t.startPos[2] = startPos.z;
    t.endPos[0] = endPos.x;
    t.endPos[1] = endPos.y;
    t.endPos[2] = endPos.z;
    t.spawnTime = BT_GetTime();

    float dx = t.endPos[0] - startPos.x;
    float dy = t.endPos[1] - startPos.y;
    float dz = t.endPos[2] - startPos.z;
    t.totalDist = sqrtf(dx * dx + dy * dy + dz * dz);

    std::lock_guard<std::mutex> lock(s_bt_traceMutex);
    s_bt_traces.push_back(t);
    return true;
  }

  s_bt_lastShotsFired = currentShots;
  return false;
}

void Misc::BulletTracerUpdate() {
  if (!Globals::bullettracer_enabled) {
    std::lock_guard<std::mutex> lock(s_bt_traceMutex);
    s_bt_traces.clear();
    s_bt_lastShotsFired = 0;
    return;
  }

  BT_ReadAndDetectShot();
}

void Misc::BulletTracerRender() {
  if (!Globals::bullettracer_enabled)
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;

  float now = BT_GetTime();
  float trailLife = Globals::bullettracer_traillife;
  float thickness = Globals::bullettracer_thickness;

  int colR = (int)(Globals::bullettracer_color[0] * 255.f);
  int colG = (int)(Globals::bullettracer_color[1] * 255.f);
  int colB = (int)(Globals::bullettracer_color[2] * 255.f);

  std::lock_guard<std::mutex> lock(s_bt_traceMutex);

  float maxAge = trailLife + 2.0f;
  s_bt_traces.erase(std::remove_if(s_bt_traces.begin(), s_bt_traces.end(),
                                   [now, maxAge](const BulletTrace &t) {
                                     return (now - t.spawnTime) > maxAge;
                                   }),
                    s_bt_traces.end());

  float screenW = Globals::GameViewportWidth;
  float screenH = Globals::GameViewportHeight;

  for (const auto &t : s_bt_traces) {
    float age = now - t.spawnTime;

    float travelTime = t.totalDist / BT_BULLET_SPEED;
    if (travelTime < 0.01f)
      travelTime = 0.01f;
    float bulletFrac = age / travelTime;
    if (bulletFrac > 1.0f)
      bulletFrac = 1.0f;

    float trailAge = age - travelTime;
    float trailAlpha = 1.0f;
    if (trailAge > 0.0f) {
      trailAlpha = 1.0f - (trailAge / trailLife);
      if (trailAlpha <= 0.0f)
        continue;
      trailAlpha *= trailAlpha;
    }

    constexpr int SEGMENTS = 16;
    ImVec2 pts[SEGMENTS + 1];
    bool ok[SEGMENTS + 1] = {};

    for (int s = 0; s <= SEGMENTS; s++) {
      float segFrac = (float)s / (float)SEGMENTS * bulletFrac;

      float pos3d[3];
      pos3d[0] = t.startPos[0] + (t.endPos[0] - t.startPos[0]) * segFrac;
      pos3d[1] = t.startPos[1] + (t.endPos[1] - t.startPos[1]) * segFrac;
      pos3d[2] = t.startPos[2] + (t.endPos[2] - t.startPos[2]) * segFrac;

      Vector worldPos(pos3d[0], pos3d[1], pos3d[2]);
      Vector screenPos;
      ok[s] = Utils::WorldToScreen(worldPos, screenPos, Globals::ViewMatrix,
                                   screenW, screenH);
      if (ok[s]) {
        pts[s] = ImVec2(screenPos.x, screenPos.y);
      }
    }

    for (int s = 0; s < SEGMENTS; s++) {
      if (!ok[s] || !ok[s + 1])
        continue;

      float segFrac = (float)s / (float)SEGMENTS;

      float brightness = 0.2f + 0.8f * segFrac;
      int alpha = (int)(trailAlpha * brightness * 220.0f);
      if (alpha <= 0)
        continue;
      if (alpha > 255)
        alpha = 255;

      ImU32 lineCol = IM_COL32(colR, colG, colB, alpha);
      draw->AddLine(pts[s], pts[s + 1], lineCol, thickness);

      int glowA = (int)(trailAlpha * brightness * 40.0f);
      if (glowA > 255)
        glowA = 255;
      ImU32 glowCol = IM_COL32(colR, colG, colB, glowA);
      draw->AddLine(pts[s], pts[s + 1], glowCol, thickness * 3.5f);
    }

    if (bulletFrac < 1.0f) {
      float headPos[3];
      headPos[0] = t.startPos[0] + (t.endPos[0] - t.startPos[0]) * bulletFrac;
      headPos[1] = t.startPos[1] + (t.endPos[1] - t.startPos[1]) * bulletFrac;
      headPos[2] = t.startPos[2] + (t.endPos[2] - t.startPos[2]) * bulletFrac;

      Vector headWorld(headPos[0], headPos[1], headPos[2]);
      Vector headScreen;
      if (Utils::WorldToScreen(headWorld, headScreen, Globals::ViewMatrix,
                               screenW, screenH)) {
        int ha = (int)(trailAlpha * 255.0f);
        draw->AddCircleFilled(ImVec2(headScreen.x, headScreen.y), 5.0f,
                              IM_COL32(colR, colG, colB, ha));
        draw->AddCircleFilled(ImVec2(headScreen.x, headScreen.y), 2.5f,
                              IM_COL32(255, 255, 255, ha));
      }
    }

    if (bulletFrac >= 1.0f && trailAlpha > 0.05f) {
      Vector endWorld(t.endPos[0], t.endPos[1], t.endPos[2]);
      Vector endScreen;
      if (Utils::WorldToScreen(endWorld, endScreen, Globals::ViewMatrix,
                               screenW, screenH)) {
        float sz = 4.0f * trailAlpha;
        draw->AddCircleFilled(
            ImVec2(endScreen.x, endScreen.y), sz,
            IM_COL32(colR, colG, colB, (int)(trailAlpha * 200.0f)));
        draw->AddCircle(ImVec2(endScreen.x, endScreen.y), sz * 1.8f,
                        IM_COL32(255, 255, 255, (int)(trailAlpha * 120.0f)), 0,
                        1.5f);
      }
    }
  }
}


// ============================================================================
//  CrosshairIndicatorRender — 3-D wallbang square at the player's crosshair.
//
//  Pipeline:
//    1. Read the local eye position + view angles.
//    2. Build a forward unit vector and trace it through the kd-tree's
//       Raycasting cache up to xhair_indicator_max_range.
//    3. If the ray hit something:
//         (a) recover the surface normal at the impact
//         (b) probe forward in small steps along the view direction; the
//             first step that falls into open air gives the back-face
//             distance, i.e. the wall thickness
//         (c) thickness < xhair_indicator_pen_thickness  →  GREEN
//             thickness >= that value or no exit found    →  RED
//    4. Build a small surface-aligned quad (4 corners) around the impact,
//       project each corner with WorldToScreen and stroke the polygon.
//
//  Everything below is pure CPU + ImGui draw-list — no engine touches.
// ============================================================================
namespace
{
    inline Vector Cross(const Vector &a, const Vector &b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    }
    inline float Dot(const Vector &a, const Vector &b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    inline Vector Normalize(const Vector &v)
    {
        float l = std::sqrt(Dot(v, v));
        if (l < 1e-6f) return {};
        return { v.x / l, v.y / l, v.z / l };
    }
}

void Misc::CrosshairIndicatorRender()
{
    if (!Globals::xhair_indicator_enabled) return;
    if (!Raycasting::Get().IsLoaded())     return;

    // --- Acquire the local pawn + eye + view direction -------------------
    C_CSPlayerPawn *pawn = EntityManager::Get().GetLocalPawn();
    if (!pawn || !Utils::SafeAlive(pawn)) return;

    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) return;

    uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(pawn);
    uintptr_t sceneNode =
        Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
    if (!Utils::IsValidPtr(sceneNode)) return;

    Vector origin     = Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
    Vector viewOffset = Utils::SafeRead<Vector>(pawnAddr  + Offsets::m_vecViewOffset);
    Vector eye = { origin.x + viewOffset.x,
                   origin.y + viewOffset.y,
                   origin.z + viewOffset.z };
    if (eye.x == 0.f && eye.y == 0.f && eye.z == 0.f) return;

    Vector viewAngles = Utils::SafeRead<Vector>(client + Offsets::dwViewAngles);
    Vector dir{};
    Utils::AngleVectors(viewAngles, dir);   // forward unit vector
    dir = Normalize(dir);
    if (dir.x == 0.f && dir.y == 0.f && dir.z == 0.f) return;

    // --- Primary trace: eye → first geometry hit ------------------------
    const float maxRange = (std::max)(64.f, Globals::xhair_indicator_max_range);
    Vector farPoint = { eye.x + dir.x * maxRange,
                        eye.y + dir.y * maxRange,
                        eye.z + dir.z * maxRange };
    Vector hitPoint{};
    bool hit = Raycasting::Get().TraceRay(eye, farPoint, hitPoint);
    Vector squareCenter = hit ? hitPoint : farPoint;

    // --- Penetrability test --------------------------------------------
    //
    //  Single-sided collision triangles in CS2's .tri data mean a
    //  small-radius hull-sphere probe inside the wall finds nothing
    //  (the front-face triangle is now BEHIND the probe and the back
    //  face is several units AHEAD). The old algorithm therefore
    //  reported almost every wall as "OPEN".
    //
    //  Reliable replacement: from a point just past the front face,
    //  cast a forward ray of `maxPen` units. The Möller–Trumbore
    //  intersection in Raycasting::RayHitsTriangle is winding-order
    //  independent so it WILL hit the wall's back face from inside.
    //
    //      front face hit  ─▶│                │◀─ back face
    //      ─────────────────►│   inside wall  │─────► (forward trace)
    //                        │                │
    //                        ▲                ▲
    //                     probe_start       probe_end (= hitPoint + dir*maxPen)
    //
    //    forward trace HITS something  →  back face within maxPen → PENETRABLE
    //    forward trace finds NOTHING   →  wall ≥ maxPen thick     → SOLID
    //
    //  We additionally re-test with a forward TraceHullSphere at the
    //  far end: if there's no geometry within ~6u of probe_end, we
    //  must be in open air (the back face triangle was just missing
    //  from the .tri pack) — still penetrable. This catches both the
    //  "real wallbang" and the "missing-mesh edge case".
    bool penetrable = false;
    if (hit) {
        const float maxPen = (std::max)(4.f, Globals::xhair_indicator_pen_thickness);
        Vector probe_start = { hitPoint.x + dir.x * 1.0f,
                               hitPoint.y + dir.y * 1.0f,
                               hitPoint.z + dir.z * 1.0f };
        Vector probe_end   = { hitPoint.x + dir.x * maxPen,
                               hitPoint.y + dir.y * maxPen,
                               hitPoint.z + dir.z * maxPen };
        Vector back_hit{};
        if (Raycasting::Get().TraceRay(probe_start, probe_end, back_hit)) {
            // Found the wall's back face within maxPen → thin wall.
            penetrable = true;
        } else {
            // No back face hit. Either:
            //   (a) the wall is thicker than maxPen, OR
            //   (b) the wall's back face was missing from the .tri
            //       data (mostly happens on map props).
            //
            // Decide via a wide hull probe at the far end — empty
            // air there means we DID exit the wall.
            if (!Raycasting::Get().TraceHullSphere(probe_end, 6.0f))
                penetrable = true;   // open space past supposed back face
            // otherwise stays SOLID
        }
    } else {
        // Trace reached max range without hitting anything (sky / void).
        penetrable = true;
    }

    // --- Pick the right surface-tangent basis --------------------------
    //  When we have a surface normal we lay the square FLAT on the wall.
    //  When we don't (sky shot, no normal), we fall back to a
    //  camera-facing square so it still looks like a square on screen.
    Vector normal{};
    bool haveNormal = false;
    if (hit) {
        haveNormal = Raycasting::Get().GetSurfaceNormal(hitPoint, 8.0f, normal);
        if (haveNormal) normal = Normalize(normal);
    }

    Vector axisA, axisB;
    if (haveNormal && (normal.x != 0.f || normal.y != 0.f || normal.z != 0.f)) {
        // Build two perpendicular tangents to the surface normal.
        Vector ref = (std::fabs(normal.z) > 0.9f)
                         ? Vector{ 1.f, 0.f, 0.f }
                         : Vector{ 0.f, 0.f, 1.f };
        axisA = Normalize(Cross(normal, ref));
        axisB = Normalize(Cross(normal, axisA));
        // Nudge the square slightly OFF the wall so the lines don't z-fight.
        squareCenter.x += normal.x * 0.25f;
        squareCenter.y += normal.y * 0.25f;
        squareCenter.z += normal.z * 0.25f;
    } else {
        // Camera-facing fallback.
        Vector worldUp{ 0.f, 0.f, 1.f };
        axisA = Normalize(Cross(dir, worldUp));
        axisB = Normalize(Cross(axisA, dir));
        if (axisA.x == 0.f && axisA.y == 0.f && axisA.z == 0.f)
            axisA = { 1.f, 0.f, 0.f };
        if (axisB.x == 0.f && axisB.y == 0.f && axisB.z == 0.f)
            axisB = { 0.f, 0.f, 1.f };
    }

    // --- Build the four corners ----------------------------------------
    const float halfSize = (std::max)(0.5f, Globals::xhair_indicator_size);
    Vector corners[4] = {
        { squareCenter.x + axisA.x * halfSize + axisB.x * halfSize,
          squareCenter.y + axisA.y * halfSize + axisB.y * halfSize,
          squareCenter.z + axisA.z * halfSize + axisB.z * halfSize },
        { squareCenter.x - axisA.x * halfSize + axisB.x * halfSize,
          squareCenter.y - axisA.y * halfSize + axisB.y * halfSize,
          squareCenter.z - axisA.z * halfSize + axisB.z * halfSize },
        { squareCenter.x - axisA.x * halfSize - axisB.x * halfSize,
          squareCenter.y - axisA.y * halfSize - axisB.y * halfSize,
          squareCenter.z - axisA.z * halfSize - axisB.z * halfSize },
        { squareCenter.x + axisA.x * halfSize - axisB.x * halfSize,
          squareCenter.y + axisA.y * halfSize - axisB.y * halfSize,
          squareCenter.z + axisA.z * halfSize - axisB.z * halfSize },
    };

    // --- Project to screen ---------------------------------------------
    const float screenW = Globals::GameViewportWidth;
    const float screenH = Globals::GameViewportHeight;
    ImVec2 sp[4];
    for (int i = 0; i < 4; ++i) {
        Vector screen{};
        if (!Utils::WorldToScreen(corners[i], screen, Globals::ViewMatrix,
                                  screenW, screenH))
            return;   // any corner behind the camera → bail this frame
        sp[i] = ImVec2(screen.x, screen.y);
    }

    // --- Color selection -----------------------------------------------
    const float *cf = penetrable
        ? Globals::xhair_indicator_color_pen
        : Globals::xhair_indicator_color_solid;
    const int   r  = (int)(cf[0] * 255.f);
    const int   g  = (int)(cf[1] * 255.f);
    const int   b  = (int)(cf[2] * 255.f);
    const int   a  = (int)(cf[3] * 255.f);
    const ImU32 line   = IM_COL32(r, g, b, a);
    const ImU32 fillC  = IM_COL32(r, g, b, a / 5);

    ImDrawList *draw = ImGui::GetBackgroundDrawList();

    // Fill (very faint) — useful when the square is far away and the
    // outline alone is hard to see.
    if (Globals::xhair_indicator_fill) {
        const ImVec2 pts[4] = { sp[0], sp[1], sp[2], sp[3] };
        draw->AddConvexPolyFilled(pts, 4, fillC);
    }

    // Outline only — center dot and distance/state label removed by
    // request. The square + its color are now the entire visual.
    const float thickness =
        (std::max)(1.0f, Globals::xhair_indicator_thickness);
    for (int i = 0; i < 4; ++i)
        draw->AddLine(sp[i], sp[(i + 1) % 4], line, thickness);
}

namespace {
using CacheParticleEffectFn = int *(__fastcall *)(
    void *, int *, const char *, int, void *, void *, void *, int);
using SetParticleControlPointFn = bool(__fastcall *)(
    void *, int, int, const Vector *, float);
using DestroyParticleFn = void(__fastcall *)(void *, int, bool, bool);

struct NativeParticleApi {
  uintptr_t managerStorage = 0;
  CacheParticleEffectFn cache = nullptr;
  SetParticleControlPointFn setControlPoint = nullptr;
  DestroyParticleFn destroy = nullptr;
  ULONGLONG lastResolveAttempt = 0;
  bool loggedReady = false;
};

struct TimedNativeParticle {
  int handle = -1;
  float destroyAt = 0.0f;
};

static constexpr std::array<BoneID, 17> kKillDustBones = {{
    BoneID::Head,      BoneID::Neck,      BoneID::Chest,
    BoneID::Spine2,    BoneID::Pelvis,    BoneID::ShoulderL,
    BoneID::ElbowL,    BoneID::HandL,     BoneID::ShoulderR,
    BoneID::ElbowR,    BoneID::HandR,     BoneID::HipL,
    BoneID::KneeL,     BoneID::FootHeelL, BoneID::HipR,
    BoneID::KneeR,     BoneID::FootHeelR,
}};

struct KillDustCapsule {
  uint8_t first;
  uint8_t second;
  float radiusStart;
  float radiusEnd;
};

static constexpr std::array<KillDustCapsule, 16> kKillDustCapsules = {{
        {0, 1, 5.4f, 4.2f},   {1, 2, 5.2f, 9.5f},
        {2, 3, 9.8f, 9.0f},   {3, 4, 9.0f, 7.8f},
        {1, 5, 4.8f, 4.6f},   {5, 6, 4.4f, 3.5f},
        {6, 7, 3.5f, 2.7f},   {1, 8, 4.8f, 4.6f},
        {8, 9, 4.4f, 3.5f},   {9, 10, 3.5f, 2.7f},
        {4, 11, 7.0f, 5.4f},  {11, 12, 5.5f, 4.2f},
        {12, 13, 4.2f, 3.3f}, {4, 14, 7.0f, 5.4f},
        {14, 15, 5.5f, 4.2f}, {15, 16, 4.2f, 3.3f},
    }};

struct NativeKillEnemyState {
  Vector lastHead{};
  std::array<Vector, kKillDustBones.size()> lastBones{};
  std::array<uint8_t, kKillDustBones.size()> validBones{};
  int health = 0;
  bool initialized = false;
  bool alive = false;
  bool pendingDeath = false;
  bool consumedForDeath = false;
  bool fallbackQueued = false;
  float lastSeenAt = -1000.0f;
  float lastLocalDamageAt = -1000.0f;
  float deathAt = -1000.0f;
  float lastBoneSampleAt = -1000.0f;
};

struct PendingConfirmedLightningKill {
  float detectedAt = 0.0f;
  float nextAttemptAt = 0.0f;
  bool dustDissolveSpawned = false;
};

struct DeathDustPoint {
  Vector position{};
  Vector velocity{};
  float phase = 0.0f;
  float size = 1.0f;
  float dissolveStart = 0.4f;
};

struct DeathDustDissolve {
  Vector center{};
  float spawnTime = 0.0f;
  float duration = 1.8f;
  int density = 1;
  std::array<float, 4> color{};
  std::vector<DeathDustPoint> points;
};

NativeParticleApi s_nativeParticles;
std::unordered_map<uintptr_t, NativeKillEnemyState> s_nativeKillEnemies;
std::vector<TimedNativeParticle> s_nativeTimedParticles;
std::deque<PendingConfirmedLightningKill> s_pendingConfirmedLightningKills;
std::array<int, 6> s_snowHandles = {-1, -1, -1, -1, -1, -1};

struct NativeMolotovOverlay {
  std::array<int, 6> handles = {-1, -1, -1, -1, -1, -1};
  int style = -1;
  float nextRefreshAt = 0.0f;
  float lastSeenAt = 0.0f;
};

std::unordered_map<uintptr_t, NativeMolotovOverlay>
    s_nativeMolotovOverlays;
void *s_lastNativeParticleManager = nullptr;
float s_nextSnowSpawnAttempt = 0.0f;
float s_nextSnowControlUpdate = 0.0f;
int s_snowFailureCount = 0;
int s_lightningFailureCount = 0;
bool s_snowCircuitOpen = false;
bool s_lightningCircuitOpen = false;
bool s_snowWasEnabled = false;
bool s_lightningWasEnabled = false;
uintptr_t s_killCounterController = 0;
int s_previousLocalRoundKills = 0;
bool s_killCounterInitialized = false;
float s_lastKillCounterAvailableAt = -1000.0f;
std::vector<DeathDustDissolve> s_deathDustDissolves;
std::mutex s_deathDustDissolveMutex;

void NativeParticleLog(const char *line) {
  if (!line)
    return;
  char appdata[MAX_PATH]{};
  if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
    return;
  std::error_code ec;
  const fs::path dir = fs::path(appdata) / "RavenCash";
  fs::create_directories(dir, ec);
  FILE *file = nullptr;
  fopen_s(&file, (dir / "features.log").string().c_str(), "a");
  if (!file)
    return;
  std::fprintf(file, "[EngineParticles] %s\n", line);
  std::fclose(file);
}

bool ResolveNativeParticleApi() {
  if (s_nativeParticles.managerStorage && s_nativeParticles.cache &&
      s_nativeParticles.setControlPoint && s_nativeParticles.destroy)
    return true;

  const ULONGLONG now = GetTickCount64();
  if (now - s_nativeParticles.lastResolveAttempt < 1500)
    return false;
  s_nativeParticles.lastResolveAttempt = now;

  const uintptr_t managerRef = Memory::PatternScan(
      "client.dll",
      Patterns::Client::PParticleManager);
  const uintptr_t cache = Memory::PatternScan(
      "client.dll", Patterns::Client::CacheParticleEffect);
  const uintptr_t setControlPoint = Memory::PatternScan(
      "client.dll",
      Patterns::Client::CreateParticleEffect);
  const uintptr_t destroy = Memory::PatternScan(
      "client.dll", Patterns::Client::DestroyParticle);

  if (!managerRef || !cache || !setControlPoint || !destroy) {
    NativeParticleLog("signature resolution failed; native effects disabled");
    return false;
  }

  __try {
    const int32_t displacement =
        *reinterpret_cast<const int32_t *>(managerRef + 3);
    s_nativeParticles.managerStorage =
        managerRef + 7 + static_cast<intptr_t>(displacement);
    s_nativeParticles.cache =
        reinterpret_cast<CacheParticleEffectFn>(cache);
    s_nativeParticles.setControlPoint =
        reinterpret_cast<SetParticleControlPointFn>(setControlPoint);
    s_nativeParticles.destroy = reinterpret_cast<DestroyParticleFn>(destroy);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s_nativeParticles = {};
    return false;
  }

  if (!s_nativeParticles.loggedReady) {
    s_nativeParticles.loggedReady = true;
    char line[256]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "ready manager_storage=0x%016llX cache=0x%016llX "
                "set_cp=0x%016llX destroy=0x%016llX",
                static_cast<unsigned long long>(
                    s_nativeParticles.managerStorage),
                static_cast<unsigned long long>(cache),
                static_cast<unsigned long long>(setControlPoint),
                static_cast<unsigned long long>(destroy));
    NativeParticleLog(line);
  }
  return true;
}

void *GetNativeParticleManager() {
  if (!ResolveNativeParticleApi())
    return nullptr;
  return reinterpret_cast<void *>(Utils::SafeRead<uintptr_t>(
      s_nativeParticles.managerStorage));
}

void DestroyNativeParticle(void *manager, int &handle) {
  if (handle == -1)
    return;
  if (manager && s_nativeParticles.destroy) {
    __try {
      // Native call sites use both flags cleared for ordinary effect cleanup.
      s_nativeParticles.destroy(manager, handle, false, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  handle = -1;
}

bool UpdateNativeParticleControlPoint(void *manager, int handle,
                                      int controlPoint,
                                      const Vector &position) {
  if (!manager || handle == -1 || !s_nativeParticles.setControlPoint)
    return false;

  __try {
    return s_nativeParticles.setControlPoint(
        manager, handle, controlPoint, &position, 0.0f);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

int SpawnNativeParticle(void *manager, const char *name,
                        const Vector &controlPoint0,
                        const Vector *controlPoint1 = nullptr) {
  if (!manager || !name || !s_nativeParticles.cache ||
      !s_nativeParticles.setControlPoint)
    return -1;

  int handle = -1;
  __try {
    // Attachment type 2 with no owner is the client's native world-space
    // path. All optional attachment/context arguments remain null.
    s_nativeParticles.cache(manager, &handle, name, 2, nullptr, nullptr,
                            nullptr, 0);
    if (handle == -1)
      return -1;
    if (!UpdateNativeParticleControlPoint(manager, handle, 0,
                                          controlPoint0)) {
      DestroyNativeParticle(manager, handle);
      return -1;
    }
    if (controlPoint1 &&
        !UpdateNativeParticleControlPoint(manager, handle, 1,
                                          *controlPoint1)) {
      DestroyNativeParticle(manager, handle);
      return -1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    handle = -1;
  }
  return handle;
}

bool SpawnNativeKillLightning(void *manager, const Vector &headPosition) {
  if (!manager)
    return false;

  static constexpr std::array<const char *, 3> kLightningEffects = {{
      "particles/raven_fx/lightning_kill_blue.vpcf",
      "particles/raven_fx/lightning_kill_green.vpcf",
      "particles/raven_fx/lightning_kill_purple.vpcf",
  }};
  const int style = std::clamp(Globals::native_kill_lightning_style, 0, 2);
  const int handle =
      SpawnNativeParticle(manager, kLightningEffects[style], headPosition);
  if (handle == -1)
    return false;

  // The resource emits twelve rope particles with a 0.4 s lifetime. Keep its
  // manager handle briefly longer so Source 2 can finish decay, then release.
  s_nativeTimedParticles.push_back({handle, Misc_GetTime() + 0.80f});
  return true;
}

bool ReadLocalRoundKills(int &outKills, uintptr_t &outController) {
  outKills = 0;
  outController = Memory::Globals::LocalController();
  if (!Utils::IsValidPtr(outController))
    return false;

  const uintptr_t trackingServices = Utils::SafeRead<uintptr_t>(
      outController + Offsets::m_pActionTrackingServices);
  if (!Utils::IsValidPtr(trackingServices))
    return false;

  const int kills = Utils::SafeRead<int>(
      trackingServices + Offsets::m_iNumRoundKills);
  if (kills < 0 || kills > 64)
    return false;

  outKills = kills;
  return true;
}

bool IsUsableLightningPosition(const Vector &position) {
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z) && position.Length() > 1.0f;
}

void SampleKillDustBones(C_CSPlayerPawn *pawn,
                         NativeKillEnemyState &state, float now) {
  if (!pawn)
    return;

  const uintptr_t pawnAddress = reinterpret_cast<uintptr_t>(pawn);
  const uintptr_t sceneNode = Utils::SafeRead<uintptr_t>(
      pawnAddress + Offsets::m_pGameSceneNode);
  if (!Utils::IsValidPtr(sceneNode))
    return;
  const uintptr_t boneArray = Utils::SafeRead<uintptr_t>(
      sceneNode + Offsets::m_modelState + 0x80);
  if (!Utils::IsValidPtr(boneArray))
    return;

  for (size_t i = 0; i < kKillDustBones.size(); ++i) {
    const Vector position = Utils::SafeRead<Vector>(
        boneArray + static_cast<int>(kKillDustBones[i]) * 0x20);
    if (!IsUsableLightningPosition(position))
      continue;

    // Reject corrupt matrices without discarding a legitimate pose close to
    // world origin. A player skeleton never spans hundreds of world units.
    if (IsUsableLightningPosition(state.lastHead)) {
      const Vector delta = position - state.lastHead;
      if (delta.Length() > 180.0f)
        continue;
    }

    state.lastBones[i] = position;
    state.validBones[i] = 1;
  }
  state.lastBoneSampleAt = now;
}

void BuildFallbackKillDustPose(NativeKillEnemyState &state) {
  if (!IsUsableLightningPosition(state.lastHead))
    return;

  const Vector head = state.lastHead;
  static const std::array<Vector, kKillDustBones.size()> offsets = {{
      {0.0f, 0.0f, 0.0f},    {0.0f, 0.0f, -7.0f},
      {0.0f, 0.0f, -17.0f},  {0.0f, 0.0f, -28.0f},
      {0.0f, 0.0f, -40.0f},  {-8.0f, 0.0f, -10.0f},
      {-13.0f, 0.0f, -22.0f}, {-17.0f, 0.0f, -33.0f},
      {8.0f, 0.0f, -10.0f},  {13.0f, 0.0f, -22.0f},
      {17.0f, 0.0f, -33.0f}, {-6.0f, 0.0f, -42.0f},
      {-7.0f, 0.0f, -59.0f}, {-8.0f, 0.0f, -74.0f},
      {6.0f, 0.0f, -42.0f},  {7.0f, 0.0f, -59.0f},
      {8.0f, 0.0f, -74.0f},
  }};

  for (size_t i = 0; i < offsets.size(); ++i) {
    if (state.validBones[i])
      continue;
    state.lastBones[i] = head + offsets[i];
    state.validBones[i] = 1;
  }
}

bool SpawnDeathDustDissolve(NativeKillEnemyState &state) {
  if (!Globals::kill_dust_dissolve_enabled)
    return true;

  size_t validCount = 0;
  for (const uint8_t valid : state.validBones)
    validCount += valid ? 1u : 0u;
  if (validCount < 8)
    BuildFallbackKillDustPose(state);

  validCount = 0;
  for (const uint8_t valid : state.validBones)
    validCount += valid ? 1u : 0u;
  if (validCount < 8)
    return false;

  DeathDustDissolve effect{};
  effect.spawnTime = Misc_GetTime();
  effect.duration =
      std::clamp(Globals::kill_dust_dissolve_duration, 0.6f, 4.0f);
  effect.density =
      std::clamp(Globals::kill_dust_dissolve_density, 0, 2);
  for (size_t i = 0; i < effect.color.size(); ++i)
    effect.color[i] =
        std::clamp(Globals::kill_dust_dissolve_color[i], 0.0f, 1.0f);

  static constexpr std::array<float, 3> kDensityMultipliers = {
      0.48f, 0.82f, 1.20f};
  const float densityMultiplier = kDensityMultipliers[effect.density];
  effect.points.reserve(static_cast<size_t>(150.0f +
                                            170.0f * densityMultiplier));

  auto random01 = [](uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7FEB352Du;
    seed ^= seed >> 15;
    seed *= 0x846CA68Bu;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0x00FFFFFFu) /
           static_cast<float>(0x01000000u);
  };

  auto normalize = [](const Vector &value) {
    const float length = value.Length();
    if (length <= 0.0001f)
      return Vector{};
    return value * (1.0f / length);
  };

  auto cross = [](const Vector &a, const Vector &b) {
    return Vector{a.y * b.z - a.z * b.y,
                  a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x};
  };

  auto appendPoint = [&effect](const Vector &position,
                               const Vector &velocity, float size,
                               float dissolveStart) {
    DeathDustPoint point{};
    point.position = position;
    point.velocity = velocity;
    const size_t index = effect.points.size();
    point.phase = static_cast<float>(
        (index * 2654435761u) & 0xFFFFu) /
                  65535.0f * 6.28318530718f;
    point.size = size;
    point.dissolveStart = dissolveStart;
    effect.points.push_back(point);
  };

  // The head is sampled as a small ellipsoid instead of a single bone point.
  // This removes the old skeleton-dot appearance and gives the silhouette a
  // recognizable model volume from every camera angle.
  if (state.validBones[0]) {
    const int headSamples =
        std::array<int, 3>{22, 36, 52}[effect.density];
    for (int sample = 0; sample < headSamples; ++sample) {
      const uint32_t seed = 0xA341316Cu +
                            static_cast<uint32_t>(sample) * 0x9E3779B9u;
      const float r0 = random01(seed);
      const float r1 = random01(seed ^ 0x68E31DA4u);
      const float r2 = random01(seed ^ 0xB5297A4Du);
      const float z = r0 * 2.0f - 1.0f;
      const float angle = r1 * 6.28318530718f;
      const float radial =
          std::sqrt(std::max(0.0f, 1.0f - z * z));
      const float volumeRadius = std::cbrt(std::max(r2, 0.001f));
      Vector outward{std::cos(angle) * radial,
                     std::sin(angle) * radial, z};
      Vector offset{outward.x * 5.5f * volumeRadius,
                    outward.y * 5.5f * volumeRadius,
                    outward.z * 6.4f * volumeRadius};
      Vector velocity = outward * (0.35f + r1 * 0.75f);
      velocity.z += 3.0f + r2 * 5.0f;
      appendPoint(state.lastBones[0] + offset, velocity,
                  0.72f + r0 * 0.62f, 0.25f + r1 * 0.40f);
    }
  }

  // Fill each anatomical segment as a tapered 3-D capsule. Random points are
  // distributed through the capsule volume, so the result reads as a dusty
  // chams/model shell rather than particles drawn along skeleton lines.
  for (size_t capsuleIndex = 0;
       capsuleIndex < kKillDustCapsules.size(); ++capsuleIndex) {
    const KillDustCapsule &capsule = kKillDustCapsules[capsuleIndex];
    if (!state.validBones[capsule.first] ||
        !state.validBones[capsule.second])
      continue;

    const Vector &from = state.lastBones[capsule.first];
    const Vector &to = state.lastBones[capsule.second];
    const Vector segment = to - from;
    const float segmentLength = segment.Length();
    if (segmentLength < 0.5f || segmentLength > 70.0f)
      continue;

    const Vector axis = normalize(segment);
    const Vector reference =
        std::fabs(axis.z) < 0.82f ? Vector{0.0f, 0.0f, 1.0f}
                                 : Vector{0.0f, 1.0f, 0.0f};
    const Vector basisU = normalize(cross(axis, reference));
    const Vector basisV = normalize(cross(axis, basisU));
    const int sampleCount =
        std::max(6, static_cast<int>(
                        std::ceil(segmentLength * densityMultiplier)));

    for (int sample = 0; sample < sampleCount; ++sample) {
      const uint32_t seed =
          static_cast<uint32_t>(capsuleIndex + 1u) * 0x9E3779B9u +
          static_cast<uint32_t>(sample + 1u) * 0x85EBCA6Bu;
      const float r0 = random01(seed);
      const float r1 = random01(seed ^ 0xC2B2AE35u);
      const float r2 = random01(seed ^ 0x27D4EB2Fu);
      const float r3 = random01(seed ^ 0x165667B1u);

      const float along =
          std::clamp((static_cast<float>(sample) + 0.18f + r0 * 0.64f) /
                         static_cast<float>(sampleCount),
                     0.0f, 1.0f);
      const float radius =
          capsule.radiusStart +
          (capsule.radiusEnd - capsule.radiusStart) * along;
      const float volumeRadius = std::sqrt(r1) * radius;
      const float angle = r2 * 6.28318530718f;
      const Vector radialDirection =
          basisU * std::cos(angle) + basisV * std::sin(angle);
      const Vector centerLine = from + segment * along;
      const Vector position =
          centerLine + radialDirection * volumeRadius;

      Vector velocity =
          radialDirection * (0.30f + r3 * 1.15f);
      velocity.z += 2.8f + r0 * 6.2f;
      appendPoint(position, velocity, 0.58f + r2 * 0.78f,
                  0.23f + r3 * 0.43f);
    }
  }

  if (effect.points.size() < 40)
    return false;

  Vector center{};
  for (const DeathDustPoint &point : effect.points)
    center = center + point.position;
  effect.center = center * (1.0f / static_cast<float>(effect.points.size()));

  std::lock_guard<std::mutex> lock(s_deathDustDissolveMutex);
  while (s_deathDustDissolves.size() >= 8)
    s_deathDustDissolves.erase(s_deathDustDissolves.begin());
  s_deathDustDissolves.push_back(std::move(effect));
  return true;
}

void UpdateNativeKillEnemySnapshots(const std::vector<Entity_t> &entities,
                                    float now) {
  std::unordered_set<uintptr_t> seen;
  seen.reserve(entities.size());

  for (const auto &entity : entities) {
    if (!entity.isEnemy || !entity.pawn)
      continue;

    const uintptr_t pawn = reinterpret_cast<uintptr_t>(entity.pawn);
    const uintptr_t controller =
        reinterpret_cast<uintptr_t>(entity.controller);
    if (!Utils::IsValidPtr(pawn))
      continue;

    // Controller identity survives pawn death/removal and therefore keeps the
    // last valid head position available after the corpse leaves entity-list
    // polling. Fall back to the pawn only for unusual bot/controller states.
    const uintptr_t key =
        Utils::IsValidPtr(controller) ? controller : pawn;
    seen.insert(key);

    NativeKillEnemyState &state = s_nativeKillEnemies[key];
    const bool aliveNow = Utils::SafeAlive(entity.pawn);
    const int healthNow =
        Utils::SafeRead<int>(pawn + Offsets::m_iHealth);

    Vector head = Utils::GetBonePos(entity.pawn, BoneID::Head);
    if (!IsUsableLightningPosition(head)) {
      head = entity.pawn->m_vOldOrigin();
      head.z += 72.0f;
    }
    if (IsUsableLightningPosition(head))
      state.lastHead = head;

    const bool deathTransition =
        state.initialized && state.alive && !aliveNow;
    if ((aliveNow && (now - state.lastBoneSampleAt) >= 0.04f) ||
        deathTransition) {
      SampleKillDustBones(entity.pawn, state, now);
    }

    if (state.initialized) {
      if (!state.alive && aliveNow) {
        // New life/round: allow this controller to produce a new effect.
        state.pendingDeath = false;
        state.consumedForDeath = false;
        state.fallbackQueued = false;
        state.lastLocalDamageAt = -1000.0f;
      }

      if (state.alive && !aliveNow) {
        state.pendingDeath = true;
        state.consumedForDeath = false;
        state.fallbackQueued = false;
        state.deathAt = now;
      }

      // m_totalHitsOnServer tells us that a local hit landed. Pairing its
      // timestamp with the only enemy whose health just dropped disambiguates
      // simultaneous deaths caused by teammates.
      if (healthNow < state.health &&
          (now - s_lastLocalHitTime) <= 0.40f) {
        state.lastLocalDamageAt = now;
      }
    }

    state.health = healthNow;
    state.alive = aliveNow;
    state.initialized = true;
    state.lastSeenAt = now;
  }

  // A pawn can be removed before a later frame observes lifeState=dead.
  // Preserve its controller snapshot and synthesize the missing transition.
  for (auto &[key, state] : s_nativeKillEnemies) {
    if (seen.find(key) != seen.end() || !state.initialized)
      continue;
    if (state.alive && (now - state.lastSeenAt) <= 0.75f) {
      state.alive = false;
      state.pendingDeath = true;
      state.consumedForDeath = false;
      state.fallbackQueued = false;
      state.deathAt = now;
    }
  }
}

NativeKillEnemyState *SelectLightningKillCandidate(float now,
                                                    bool allowConsumed) {
  NativeKillEnemyState *best = nullptr;
  float bestScore = -1000000.0f;

  for (auto &[_, state] : s_nativeKillEnemies) {
    if (!state.initialized || !IsUsableLightningPosition(state.lastHead))
      continue;
    if (!allowConsumed && state.consumedForDeath)
      continue;

    float score = 0.0f;
    const float deathAge = now - state.deathAt;
    const float damageAge = now - state.lastLocalDamageAt;
    const float seenAge = now - state.lastSeenAt;

    if (state.pendingDeath && deathAge >= 0.0f && deathAge <= 3.0f)
      score += 1000.0f - deathAge * 80.0f;
    if (damageAge >= 0.0f && damageAge <= 3.0f)
      score += 700.0f - damageAge * 100.0f;
    if (!state.alive)
      score += 250.0f;
    if (seenAge >= 0.0f && seenAge <= 4.0f)
      score += 150.0f - seenAge * 20.0f;

    if (score > bestScore) {
      bestScore = score;
      best = &state;
    }
  }

  // Before the queue's grace period expires, accept only a real death or a
  // locally damaged target. This prevents an unrelated living enemy from
  // winning merely because it was the most recently rendered pawn.
  if (!allowConsumed && bestScore < 500.0f)
    return nullptr;
  return best;
}

void QueueConfirmedLightningKills(int count, float now) {
  count = std::clamp(count, 0, 8);
  while (count-- > 0 && s_pendingConfirmedLightningKills.size() < 8)
    s_pendingConfirmedLightningKills.push_back({now, now, false});
}

void UpdateConfirmedLightningQueue(void *manager, float now) {
  while (!s_pendingConfirmedLightningKills.empty()) {
    PendingConfirmedLightningKill &pending =
        s_pendingConfirmedLightningKills.front();
    if (now < pending.nextAttemptAt)
      return;

    NativeKillEnemyState *candidate =
        SelectLightningKillCandidate(now, false);
    // Kill counters can update before the victim's final pawn state. Wait a
    // short time for the exact snapshot; after that, use the newest cached
    // enemy position so a confirmed local kill never silently loses its FX.
    if (!candidate && (now - pending.detectedAt) < 0.75f)
      return;
    if (!candidate)
      candidate = SelectLightningKillCandidate(now, true);
    if (!candidate)
      return;

    if (Globals::kill_dust_dissolve_enabled &&
        !pending.dustDissolveSpawned) {
      pending.dustDissolveSpawned =
          SpawnDeathDustDissolve(*candidate);
    }

    const bool dustComplete =
        !Globals::kill_dust_dissolve_enabled ||
        pending.dustDissolveSpawned;
    bool lightningComplete =
        !Globals::native_kill_lightning_enabled ||
        s_lightningCircuitOpen;
    bool lightningAttemptFailed = false;
    if (!lightningComplete && manager) {
      lightningComplete =
          SpawnNativeKillLightning(manager, candidate->lastHead);
      lightningAttemptFailed = !lightningComplete;
    }

    if (dustComplete && lightningComplete) {
      candidate->pendingDeath = false;
      candidate->consumedForDeath = true;
      candidate->fallbackQueued = true;
      s_pendingConfirmedLightningKills.pop_front();
      s_lightningFailureCount = 0;
      if (Globals::native_kill_lightning_enabled &&
          Globals::kill_dust_dissolve_enabled) {
        NativeParticleLog(
            "server-confirmed local kill spawned lightning and dust dissolve");
      } else if (Globals::kill_dust_dissolve_enabled) {
        NativeParticleLog(
            "server-confirmed local kill spawned model dust dissolve");
      } else {
        NativeParticleLog(
            "server-confirmed local kill spawned colored lightning");
      }
      continue;
    }

    pending.nextAttemptAt = now + 0.10f;
    if ((now - pending.detectedAt) > 2.0f) {
      s_pendingConfirmedLightningKills.pop_front();
      if (lightningAttemptFailed && ++s_lightningFailureCount >= 2) {
        s_lightningCircuitOpen = true;
        NativeParticleLog(
            "confirmed kill lightning failed repeatedly; circuit opened");
      } else if (!dustComplete) {
        NativeParticleLog(
            "confirmed kill dust dissolve had no usable pose snapshot");
      }
    }
    return;
  }
}

void DestroyNativeSnowLayers(void *manager) {
  for (int &handle : s_snowHandles)
    DestroyNativeParticle(manager, handle);
}

void UpdateNativeSnow(void *manager, C_CSPlayerPawn *localPawn) {
  const float now = Misc_GetTime();
  if (!Globals::world_snow_enabled) {
    DestroyNativeSnowLayers(manager);
    if (s_snowWasEnabled)
      NativeParticleLog("world snow disabled and native handles released");
    s_snowWasEnabled = false;
    s_snowFailureCount = 0;
    s_snowCircuitOpen = false;
    s_nextSnowSpawnAttempt = 0.0f;
    s_nextSnowControlUpdate = 0.0f;
    return;
  }

  if (!s_snowWasEnabled) {
    s_snowWasEnabled = true;
    s_snowFailureCount = 0;
    s_snowCircuitOpen = false;
    s_nextSnowSpawnAttempt = now + 0.20f;
    s_nextSnowControlUpdate = now;
    NativeParticleLog("world snow requested; staged native creation armed");
  }

  if (!manager || !localPawn || !Utils::SafeAlive(localPawn) ||
      s_snowCircuitOpen)
    return;

  Vector playerOrigin = localPawn->m_vOldOrigin();
  const uintptr_t pawn = reinterpret_cast<uintptr_t>(localPawn);
  const uintptr_t sceneNode =
      Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pGameSceneNode);
  if (Utils::IsValidPtr(sceneNode)) {
    const Vector absolute =
        Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
    if (absolute.Length() > 0.0f)
      playerOrigin = absolute;
  }

  // The bundled weather resources form complementary inner/outer layers:
  //   snow.vpcf       -> dense flakes inside a 300-unit radius
  //   snow_outer.vpcf -> sparse flakes in the 800-900 unit outer ring
  // Both definitions emit around CP1, distance-cull against CP1 and use CP2
  // as their allow-render gate. The previous snow_drift_128 grid ignored
  // this layout and rendered nine smoke-textured drift volumes above the
  // player, which looked like fog instead of falling snow.
  struct SnowLayerDefinition {
    const char *effect;
    float offsetX;
    float offsetY;
    int minimumDensity;
  };
  static constexpr std::array<SnowLayerDefinition, 6> kSnowLayers = {{
      {"particles/rain_fx/snow.vpcf", 0.0f, 0.0f, 0},
      {"particles/rain_fx/snow_outer.vpcf", 0.0f, 0.0f, 0},
      {"particles/rain_fx/snow_dense_a.vpcf", 24.0f, -18.0f, 1},
      {"particles/rain_fx/snow_outer_dense_a.vpcf", -28.0f, 20.0f, 1},
      {"particles/rain_fx/snow_blizzard_b.vpcf", -22.0f, 18.0f, 2},
      {"particles/rain_fx/snow_outer_blizzard_b.vpcf", 26.0f, -20.0f, 2},
  }};
  const Vector allowRender = {1.0f, 0.0f, 0.0f};
  Vector emitterOrigin = playerOrigin;
  emitterOrigin.z += 280.0f;
  const int density = std::clamp(Globals::world_snow_density, 0, 2);

  // Density changes are applied live. Layers above the selected level are
  // released immediately instead of surviving until the next map transition.
  for (int i = 0; i < static_cast<int>(s_snowHandles.size()); ++i) {
    if (kSnowLayers[i].minimumDensity > density)
      DestroyNativeParticle(manager, s_snowHandles[i]);
  }

  int missingIndex = -1;
  for (int i = 0; i < static_cast<int>(s_snowHandles.size()); ++i) {
    if (kSnowLayers[i].minimumDensity <= density &&
        s_snowHandles[i] == -1) {
      missingIndex = i;
      break;
    }
  }

  if (missingIndex != -1 && now >= s_nextSnowSpawnAttempt) {
    int &handle = s_snowHandles[missingIndex];
    Vector layerOrigin = emitterOrigin;
    layerOrigin.x += kSnowLayers[missingIndex].offsetX;
    layerOrigin.y += kSnowLayers[missingIndex].offsetY;
    handle = SpawnNativeParticle(manager, kSnowLayers[missingIndex].effect,
                                 layerOrigin, &layerOrigin);
    if (handle != -1 &&
        !UpdateNativeParticleControlPoint(manager, handle, 2, allowRender))
      DestroyNativeParticle(manager, handle);

    s_nextSnowSpawnAttempt = now + 0.06f;
    if (handle == -1) {
      if (++s_snowFailureCount >= 3) {
        s_snowCircuitOpen = true;
        DestroyNativeSnowLayers(manager);
        NativeParticleLog(
            "native snow layer creation failed; circuit opened until toggle reset");
      }
    } else {
      s_snowFailureCount = 0;
      if (missingIndex == 1 + density * 2) {
        char line[128]{};
        _snprintf_s(
            line, sizeof(line), _TRUNCATE,
            "native snow ready (level=%d, continuous_layers=%d, patched_weather=1)",
            density, 2 + density * 2);
        NativeParticleLog(line);
      }
    }
    return;
  }

  // Follow the local player at 10 Hz. Updating both CP0 and CP1 keeps the
  // environment traces and the actual emission sphere aligned while CP2
  // remains explicitly enabled.
  if (now < s_nextSnowControlUpdate)
    return;
  s_nextSnowControlUpdate = now + 0.10f;

  for (int i = 0; i < static_cast<int>(s_snowHandles.size()); ++i) {
    int &handle = s_snowHandles[i];
    if (handle == -1)
      continue;
    Vector layerOrigin = emitterOrigin;
    layerOrigin.x += kSnowLayers[i].offsetX;
    layerOrigin.y += kSnowLayers[i].offsetY;
    if (!UpdateNativeParticleControlPoint(manager, handle, 0, layerOrigin) ||
        !UpdateNativeParticleControlPoint(manager, handle, 1, layerOrigin) ||
        !UpdateNativeParticleControlPoint(manager, handle, 2, allowRender)) {
      DestroyNativeParticle(manager, handle);
      s_nextSnowSpawnAttempt = now + 1.0f;
    }
  }

}

void DestroyNativeMolotovOverlay(void *manager,
                                  NativeMolotovOverlay &overlay) {
  for (int &handle : overlay.handles)
    DestroyNativeParticle(manager, handle);
}

void DestroyAllNativeMolotovOverlays(void *manager) {
  for (auto &[_, overlay] : s_nativeMolotovOverlays)
    DestroyNativeMolotovOverlay(manager, overlay);
  s_nativeMolotovOverlays.clear();
}

bool IsUsableInfernoPoint(const Vector &point) {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z) && point.Length() > 1.0f &&
         std::fabs(point.x) < 100000.0f &&
         std::fabs(point.y) < 100000.0f &&
         std::fabs(point.z) < 100000.0f;
}

void UpdateNativeMolotovColor(void *manager, float now) {
  if (!Globals::particle_color_enabled || !manager) {
    if (!s_nativeMolotovOverlays.empty())
      DestroyAllNativeMolotovOverlays(manager);
    return;
  }

  const uintptr_t client = Memory::GetModuleBase("client.dll");
  const uintptr_t entityList =
      client ? Utils::SafeRead<uintptr_t>(client + Offsets::dwEntityList) : 0;
  if (!Utils::IsValidPtr(entityList))
    return;

  static constexpr std::array<const char *, 3> kMolotovEffects = {{
      "particles/raven_fx/molotov_blue.vpcf",
      "particles/raven_fx/molotov_green.vpcf",
      "particles/raven_fx/molotov_purple.vpcf",
  }};

  const int style = std::clamp(Globals::particle_color_style, 0, 2);
  std::vector<uintptr_t> seenInfernos;
  seenInfernos.reserve(8);

  for (int i = 0; i < 1024; ++i) {
      const uintptr_t entry = Utils::SafeRead<uintptr_t>(
          entityList + (8 * ((i & 0x7FFF) >> 9) + 16));
      if (!Utils::IsValidPtr(entry))
        continue;

      const uintptr_t entity =
          Utils::SafeRead<uintptr_t>(entry + (112 * (i & 0x1FF)));
      if (!Utils::IsValidPtr(entity))
        continue;

      const uintptr_t identity =
          Utils::SafeRead<uintptr_t>(entity + 0x10);
      if (!Utils::IsValidPtr(identity))
        continue;

      const uintptr_t designerName =
          Utils::SafeRead<uintptr_t>(identity + 0x20);
      if (!Utils::IsValidPtr(designerName))
        continue;

      char className[48]{};
      if (!Utils::SafeReadString(designerName, className,
                                 sizeof(className)) ||
          std::strcmp(className, "inferno") != 0)
        continue;

      int fireCount =
          Utils::SafeRead<int>(entity + Offsets::m_infernoFireCount);
      if (fireCount <= 0 || fireCount > 64)
        continue;

      std::vector<Vector> activePoints;
      activePoints.reserve(static_cast<size_t>(fireCount));
      for (int fire = 0; fire < fireCount; ++fire) {
        const bool burning = Utils::SafeRead<bool>(
            entity + Offsets::m_infernoFireIsBurning + fire);
        if (!burning)
          continue;

        Vector point = Utils::SafeRead<Vector>(
            entity + Offsets::m_infernoFirePositions +
            static_cast<uintptr_t>(fire) * sizeof(Vector));
        if (IsUsableInfernoPoint(point))
          activePoints.push_back(point);
      }

      if (activePoints.empty())
        continue;

      seenInfernos.push_back(entity);
      NativeMolotovOverlay &overlay = s_nativeMolotovOverlays[entity];
      overlay.lastSeenAt = now;

      if (overlay.style != style) {
        DestroyNativeMolotovOverlay(manager, overlay);
        overlay.style = style;
        overlay.nextRefreshAt = 0.0f;
      }

      if (now < overlay.nextRefreshAt)
        continue;

      DestroyNativeMolotovOverlay(manager, overlay);

      // Six evenly distributed anchors cover a normal inferno footprint
      // without multiplying the 128-particle overlay across all 64 points.
      const size_t anchorCount =
          (std::min)(overlay.handles.size(), activePoints.size());
      for (size_t anchor = 0; anchor < anchorCount; ++anchor) {
        const size_t pointIndex =
            (anchor * activePoints.size()) / anchorCount;
        Vector origin = activePoints[pointIndex];
        origin.z += 2.0f;
        overlay.handles[anchor] =
            SpawnNativeParticle(manager, kMolotovEffects[style], origin);
      }

      // The source particles have randomized 0.5-4.0 s lifetimes. Refresh
      // below the midpoint to avoid visible gaps while keeping the live
      // collection count bounded.
      overlay.nextRefreshAt = now + 2.15f;
  }

  for (auto it = s_nativeMolotovOverlays.begin();
       it != s_nativeMolotovOverlays.end();) {
    const bool seen =
        std::find(seenInfernos.begin(), seenInfernos.end(), it->first) !=
        seenInfernos.end();
    if (!seen || now - it->second.lastSeenAt > 0.35f) {
      DestroyNativeMolotovOverlay(manager, it->second);
      it = s_nativeMolotovOverlays.erase(it);
    } else {
      ++it;
    }
  }
}
} // namespace

void Misc::EngineParticleUpdate() {
  const bool needsNativeParticleManager =
      Globals::world_snow_enabled ||
      Globals::particle_color_enabled ||
      Globals::native_kill_lightning_enabled ||
      !s_nativeTimedParticles.empty() ||
      !s_nativeMolotovOverlays.empty();
  void *manager =
      needsNativeParticleManager ? GetNativeParticleManager() : nullptr;
  C_CSPlayerPawn *localPawn = EntityManager::Get().GetLocalPawn();

  // Handles belong to a specific particle manager instance. Never carry them
  // across disconnects or map transitions.
  if (manager != s_lastNativeParticleManager) {
    s_nativeTimedParticles.clear();
    s_nativeKillEnemies.clear();
    s_pendingConfirmedLightningKills.clear();
    s_snowHandles.fill(-1);
    s_nativeMolotovOverlays.clear();
    s_nextSnowSpawnAttempt = 0.0f;
    s_nextSnowControlUpdate = 0.0f;
    s_snowFailureCount = 0;
    s_lightningFailureCount = 0;
    s_snowCircuitOpen = false;
    s_lightningCircuitOpen = false;
    s_killCounterController = 0;
    s_previousLocalRoundKills = 0;
    s_killCounterInitialized = false;
    s_lastKillCounterAvailableAt = -1000.0f;
    s_lastNativeParticleManager = manager;
    if (manager)
      NativeParticleLog("particle manager changed; runtime state reset");
  }

  const float now = Misc_GetTime();
  UpdateNativeSnow(manager, localPawn);
  UpdateNativeMolotovColor(manager, now);
  for (auto it = s_nativeTimedParticles.begin();
       it != s_nativeTimedParticles.end();) {
    if (now >= it->destroyAt) {
      DestroyNativeParticle(manager, it->handle);
      it = s_nativeTimedParticles.erase(it);
    } else {
      ++it;
    }
  }

  const bool anyKillEffect =
      Globals::native_kill_lightning_enabled ||
      Globals::kill_dust_dissolve_enabled;
  if (!anyKillEffect) {
    s_nativeKillEnemies.clear();
    s_pendingConfirmedLightningKills.clear();
    s_lightningFailureCount = 0;
    s_lightningCircuitOpen = false;
    s_lightningWasEnabled = false;
    s_killCounterController = 0;
    s_previousLocalRoundKills = 0;
    s_killCounterInitialized = false;
    s_lastKillCounterAvailableAt = -1000.0f;
    return;
  }

  if (!s_lightningWasEnabled) {
    s_lightningWasEnabled = true;
    s_lightningFailureCount = 0;
    s_lightningCircuitOpen = false;
    NativeParticleLog("confirmed kill effects armed on the game thread");
  }

  // Do not require the local pawn to be alive: a trade kill still raises the
  // server-confirmed counter. The overlay silhouette also remains available
  // when the native particle manager is temporarily absent.
  if (!localPawn ||
      (s_lightningCircuitOpen &&
       !Globals::kill_dust_dissolve_enabled)) {
    return;
  }

  const auto entities = EntityManager::Get().GetEntities();
  UpdateNativeKillEnemySnapshots(entities, now);

  int localRoundKills = 0;
  uintptr_t localController = 0;
  const bool killCounterAvailable =
      ReadLocalRoundKills(localRoundKills, localController);

  if (killCounterAvailable) {
    s_lastKillCounterAvailableAt = now;
    if (!s_killCounterInitialized ||
        localController != s_killCounterController) {
      s_killCounterController = localController;
      s_previousLocalRoundKills = localRoundKills;
      s_killCounterInitialized = true;
      s_pendingConfirmedLightningKills.clear();
    } else if (localRoundKills < s_previousLocalRoundKills) {
      // Round reset.
      s_previousLocalRoundKills = localRoundKills;
      s_pendingConfirmedLightningKills.clear();
      for (auto &[_, state] : s_nativeKillEnemies) {
        state.pendingDeath = false;
        state.consumedForDeath = false;
        state.fallbackQueued = false;
      }
    } else if (localRoundKills > s_previousLocalRoundKills) {
      QueueConfirmedLightningKills(
          localRoundKills - s_previousLocalRoundKills, now);
      s_previousLocalRoundKills = localRoundKills;
    }
  } else if ((now - s_lastKillCounterAvailableAt) > 1.0f) {
    // Compatibility fallback for modes that do not expose action-tracking
    // services. It is delayed so a transient null pointer cannot duplicate a
    // kill that the server counter will report on the next frame.
    for (auto &[_, state] : s_nativeKillEnemies) {
      if (!state.pendingDeath || state.fallbackQueued ||
          state.consumedForDeath)
        continue;
      if ((now - s_lastLocalHitTime) <= 2.0f) {
        state.fallbackQueued = true;
        QueueConfirmedLightningKills(1, now);
      }
    }
  }

  UpdateConfirmedLightningQueue(manager, now);

  // Keep snapshots long enough to survive corpse removal and delayed network
  // kill-stat replication, then prune stale controller identities.
  for (auto it = s_nativeKillEnemies.begin();
       it != s_nativeKillEnemies.end();) {
    if ((now - it->second.lastSeenAt) > 8.0f &&
        !it->second.pendingDeath)
      it = s_nativeKillEnemies.erase(it);
    else
      ++it;
  }
}

void Misc::DeathDustDissolveRender() {
  std::lock_guard<std::mutex> lock(s_deathDustDissolveMutex);

  if (!Globals::kill_dust_dissolve_enabled) {
    s_deathDustDissolves.clear();
    return;
  }

  const float now = Misc_GetTime();
  s_deathDustDissolves.erase(
      std::remove_if(
          s_deathDustDissolves.begin(), s_deathDustDissolves.end(),
          [now](const DeathDustDissolve &effect) {
            return now - effect.spawnTime >= effect.duration;
          }),
      s_deathDustDissolves.end());
  if (s_deathDustDissolves.empty())
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;

  Vector localOrigin{};
  bool hasLocalOrigin = false;
  const uintptr_t localPawn = Memory::Globals::LocalPawn();
  if (Utils::IsValidPtr(localPawn)) {
    const uintptr_t sceneNode = Utils::SafeRead<uintptr_t>(
        localPawn + Offsets::m_pGameSceneNode);
    if (Utils::IsValidPtr(sceneNode)) {
      localOrigin =
          Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
      hasLocalOrigin = IsUsableLightningPosition(localOrigin);
    }
  }

  const float screenWidth =
      static_cast<float>(Globals::GameViewportWidth);
  const float screenHeight =
      static_cast<float>(Globals::GameViewportHeight);

  for (const DeathDustDissolve &effect : s_deathDustDissolves) {
    const float age = now - effect.spawnTime;
    if (age < 0.0f || age >= effect.duration)
      continue;

    const float normalizedAge = age / effect.duration;
    const float fadeIn = std::clamp(age / 0.09f, 0.0f, 1.0f);

    float distanceScale = 1.0f;
    if (hasLocalOrigin) {
      const Vector delta = effect.center - localOrigin;
      const float distance = delta.Length();
      distanceScale =
          std::clamp(700.0f / (distance + 190.0f), 0.62f, 1.38f);
    }

    const int red =
        std::clamp(static_cast<int>(effect.color[0] * 255.0f), 0, 255);
    const int green =
        std::clamp(static_cast<int>(effect.color[1] * 255.0f), 0, 255);
    const int blue =
        std::clamp(static_cast<int>(effect.color[2] * 255.0f), 0, 255);

    for (size_t index = 0; index < effect.points.size(); ++index) {
      const DeathDustPoint &point = effect.points[index];
      float pointFade = 1.0f;
      if (normalizedAge > point.dissolveStart) {
        pointFade = std::clamp(
            (1.0f - normalizedAge) /
                std::max(0.05f, 1.0f - point.dissolveStart),
            0.0f, 1.0f);
        pointFade *= pointFade;
      }
      if (pointFade <= 0.002f)
        continue;

      Vector world = point.position + point.velocity * age;
      const float drift = age * age * 0.55f;
      world.x += std::sin(point.phase + age * 1.7f) * drift;
      world.y += std::cos(point.phase + age * 1.4f) * drift;

      Vector screen{};
      if (!Utils::WorldToScreen(world, screen, Globals::ViewMatrix,
                                screenWidth, screenHeight))
        continue;

      const float shimmer =
          0.76f + 0.16f * std::sin(age * 8.0f + point.phase);
      const float alphaFloat =
          fadeIn * pointFade * shimmer * effect.color[3];
      const int alpha =
          std::clamp(static_cast<int>(alphaFloat * 255.0f), 0, 255);
      if (alpha <= 2)
        continue;

      const float baseSize =
          (1.18f + static_cast<float>(effect.density) * 0.12f) *
          distanceScale * point.size *
          (0.92f + 0.08f * std::sin(age * 6.0f + point.phase));
      if (baseSize < 0.42f)
        continue;

      const ImVec2 center(screen.x, screen.y);
      const int glowAlpha = std::clamp(alpha / 10, 0, 18);
      if (glowAlpha > 0) {
        draw->AddCircleFilled(
            center, baseSize * 2.35f,
            IM_COL32(red, green, blue, glowAlpha), 8);
      }

      draw->AddCircleFilled(center, baseSize,
                            IM_COL32(red, green, blue, alpha), 6);
      if ((index % 13u) == 0u && baseSize > 0.7f) {
        draw->AddCircleFilled(
            center, baseSize * 0.36f,
            IM_COL32(238, 244, 255,
                     std::clamp(static_cast<int>(alpha * 0.72f), 0, 255)),
            6);
      }
    }
  }
}

struct HitParticle {
  Vector pos;
  Vector velocity;
  float baseSize;
  float life;
  float age;
  float drag;
  float gravity;
  float rotation;
  float rotSpeed;
  int layer;
};

struct HitParticleBurst {
  Vector origin;
  float spawnTime;
  int style;
  float lifetime;
  float sizeMul;
  float intensity;
  float glowMul;
  float colorR, colorG, colorB, colorA;
  std::vector<HitParticle> particles;
};

struct HitDistortion {
  Vector origin;
  float spawnTime;
  float maxRadius;
  float life;
};

static std::vector<HitParticleBurst> s_hp_bursts;
static std::vector<HitDistortion> s_hp_distortions;
static std::mutex s_hp_mutex;
static int s_hp_lastTotalHits = 0;
static bool s_hp_initialized = false;

static float HP_PseudoRand(float seed, int idx) {
  uint32_t h = (uint32_t)(seed * 1000.0f) ^ (uint32_t)(idx * 2654435761u);
  h ^= h >> 16;
  h *= 0x45d9f3b;
  h ^= h >> 16;
  return (float)(h & 0xFFFF) / 65535.0f;
}

static void HP_GetPresetColor(int style, int idx, float rand01, float &outR,
                              float &outG, float &outB, float &outA) {
  switch (style) {
  case 0: {
    float r = 0.72f + rand01 * 0.28f;
    float g = 0.02f + rand01 * 0.08f;
    float b = 0.01f + rand01 * 0.04f;
    outR = r;
    outG = g;
    outB = b;
    outA = 0.92f;
    break;
  }
  case 1: {
    float t = (float)(idx % 3) / 2.0f;
    outR = 0.55f + t * 0.35f;
    outG = 0.02f + t * 0.06f + rand01 * 0.04f;
    outB = 0.01f + rand01 * 0.02f;
    outA = 0.95f;

    if ((idx % 7) == 0) {
      outR = 0.95f;
      outG = 0.15f;
      outB = 0.05f;
    }
    break;
  }
  case 2: {
    float pulse = sinf(rand01 * 6.283f) * 0.5f + 0.5f;
    outR = 0.3f + pulse * 0.4f;
    outG = 0.7f + pulse * 0.3f;
    outB = 1.0f;
    outA = 0.88f;
    if ((idx % 4) == 0) {
      outR = 1.0f;
      outG = 1.0f;
      outB = 1.0f;
      outA = 0.95f;
    }
    break;
  }
  case 3: {
    outR = 0.75f + rand01 * 0.25f;
    outG = 0.82f + rand01 * 0.18f;
    outB = 1.0f;
    outA = 0.55f + rand01 * 0.2f;
    break;
  }
  case 4: {
    float t = rand01;
    outR = 1.0f;
    outG = 0.65f + t * 0.35f;
    outB = 0.15f + t * 0.25f;
    outA = 0.9f + t * 0.1f;
    if ((idx % 5) == 0) {
      outR = 1.0f;
      outG = 1.0f;
      outB = 0.9f;
    }
    break;
  }
  default:
    outR = 1.0f;
    outG = 1.0f;
    outB = 1.0f;
    outA = 1.0f;
    break;
  }
}

static void HP_SpawnBurst(const Vector &hitPos, int style) {
  float now = Misc_GetTime();
  int maxParts = Globals::hitparticle_max_per_hit;
  if (maxParts < 8)
    maxParts = 8;
  if (maxParts > 64)
    maxParts = 64;

  HitParticleBurst burst{};
  burst.origin = hitPos;
  burst.spawnTime = now;
  burst.style = style;
  burst.lifetime = Globals::hitparticle_lifetime;
  burst.sizeMul = Globals::hitparticle_size;
  burst.intensity = Globals::hitparticle_intensity;
  burst.glowMul = Globals::hitparticle_glow;

  burst.colorR = Globals::hitparticle_color[0];
  burst.colorG = Globals::hitparticle_color[1];
  burst.colorB = Globals::hitparticle_color[2];
  burst.colorA = Globals::hitparticle_color[3];

  burst.particles.reserve(maxParts);

  constexpr float PI = 3.14159265358979323846f;
  float seed =
      hitPos.x * 0.017f + hitPos.y * 0.013f + hitPos.z * 0.009f + now * 7.3f;
  bool randomize = Globals::hitparticle_randomize;

  for (int i = 0; i < maxParts; ++i) {
    HitParticle p{};
    p.pos = hitPos;
    p.age = 0.0f;

    float r0 = HP_PseudoRand(seed, i * 3);
    float r1 = HP_PseudoRand(seed, i * 3 + 1);
    float r2 = HP_PseudoRand(seed, i * 3 + 2);

    float angle = (2.0f * PI * (float)i / (float)maxParts);
    if (randomize)
      angle += (r0 - 0.5f) * 0.8f;

    float elevation = (r1 - 0.5f) * PI * 0.7f;
    float cosEl = cosf(elevation);

    switch (style) {
    case 0: {
      float speed = 60.0f + r0 * 110.0f;
      p.velocity.x = cosf(angle) * cosEl * speed;
      p.velocity.y = sinf(angle) * cosEl * speed;
      p.velocity.z = sinf(elevation) * speed * 0.6f + 40.0f + r2 * 30.0f;
      p.baseSize = 1.8f + r1 * 2.5f;
      p.life = 0.5f + r2 * 0.4f;
      p.drag = 0.92f + r0 * 0.04f;
      p.gravity = 180.0f + r1 * 60.0f;
      p.layer = i % 3;
      break;
    }
    case 1: {
      float speed = 40.0f + r0 * 80.0f;
      p.velocity.x = cosf(angle) * cosEl * speed;
      p.velocity.y = sinf(angle) * cosEl * speed;
      p.velocity.z = 30.0f + r2 * 60.0f;
      p.baseSize = 2.2f + r1 * 3.5f;
      p.life = 0.6f + r2 * 0.5f;
      p.drag = 0.88f + r0 * 0.06f;
      p.gravity = 220.0f + r1 * 80.0f;
      p.layer = i % 4;

      if ((i % 5) == 0) {
        p.baseSize *= 1.8f;
        p.velocity.x *= 0.5f;
        p.velocity.y *= 0.5f;
        p.velocity.z += 40.0f;
        p.gravity *= 1.4f;
      }
      break;
    }
    case 2: {
      float speed = 100.0f + r0 * 160.0f;
      p.velocity.x = cosf(angle) * cosEl * speed;
      p.velocity.y = sinf(angle) * cosEl * speed;
      p.velocity.z = sinf(elevation) * speed * 0.4f;
      p.baseSize = 1.4f + r1 * 2.0f;
      p.life = 0.35f + r2 * 0.3f;
      p.drag = 0.95f;
      p.gravity = -15.0f;
      p.layer = i % 3;
      break;
    }
    case 3: {
      float ringSpeed = 140.0f + r0 * 60.0f;
      p.velocity.x = cosf(angle) * ringSpeed;
      p.velocity.y = sinf(angle) * ringSpeed;
      p.velocity.z = (r1 - 0.5f) * 20.0f;
      p.baseSize = 2.0f + r1 * 1.5f;
      p.life = 0.3f + r2 * 0.25f;
      p.drag = 0.97f;
      p.gravity = 0.0f;
      p.layer = 0;
      break;
    }
    case 4: {
      float speed = 150.0f + r0 * 250.0f;
      p.velocity.x = cosf(angle) * cosEl * speed;
      p.velocity.y = sinf(angle) * cosEl * speed;
      p.velocity.z = sinf(elevation) * speed * 0.5f + 20.0f;
      p.baseSize = 0.8f + r1 * 1.2f;
      p.life = 0.15f + r2 * 0.25f;
      p.drag = 0.98f;
      p.gravity = 60.0f;
      p.layer = i % 2;
      break;
    }
    }

    p.rotation = r0 * 6.283f;
    p.rotSpeed = (r1 - 0.5f) * 12.0f;

    burst.particles.push_back(p);
  }

  std::lock_guard<std::mutex> lock(s_hp_mutex);

  while (s_hp_bursts.size() >= 8)
    s_hp_bursts.erase(s_hp_bursts.begin());

  s_hp_bursts.push_back(std::move(burst));

  if (Globals::hitparticle_distortion) {
    HitDistortion d{};
    d.origin = hitPos;
    d.spawnTime = now;
    d.maxRadius = 60.0f + (style == 3 ? 40.0f : 0.0f);
    d.life = 0.35f;
    s_hp_distortions.push_back(d);
  }
}

void Misc::HitParticleUpdate() {
  if (!Globals::hitparticle_enabled) {
    std::lock_guard<std::mutex> lock(s_hp_mutex);
    s_hp_bursts.clear();
    s_hp_distortions.clear();
    s_hp_lastTotalHits = 0;
    s_hp_initialized = false;
    return;
  }

  int totalHits = 0;
  if (!ReadHitsoundData(totalHits))
    return;

  if (!s_hp_initialized) {
    s_hp_lastTotalHits = totalHits;
    s_hp_initialized = true;
    return;
  }

  if (totalHits > s_hp_lastTotalHits && s_hp_lastTotalHits >= 0) {

    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (client) {
      uintptr_t localPawnAddr =
          Memory::Globals::LocalPawn();
      if (Utils::IsValidPtr(localPawnAddr) && localPawnAddr > 0x1000000) {

        uintptr_t localScene = Utils::SafeRead<uintptr_t>(
            localPawnAddr + Offsets::m_pGameSceneNode);
        Vector localOrigin = {0, 0, 0};
        if (Utils::IsValidPtr(localScene))
          localOrigin =
              Utils::SafeRead<Vector>(localScene + Offsets::m_vecAbsOrigin);
        Vector viewOff =
            Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecViewOffset);
        Vector eyePos = {localOrigin.x + viewOff.x, localOrigin.y + viewOff.y,
                         localOrigin.z + viewOff.z};

        uintptr_t viewAnglesAddr = client + Offsets::dwViewAngles;
        float pitch = Utils::SafeRead<float>(viewAnglesAddr);
        float yaw = Utils::SafeRead<float>(viewAnglesAddr + 4);
        constexpr float D2R = 3.14159265f / 180.0f;
        float cp = cosf(pitch * D2R), sp = sinf(pitch * D2R);
        float cy = cosf(yaw * D2R), sy = sinf(yaw * D2R);
        Vector fwd = {cp * cy, cp * sy, -sp};

        auto entities = EntityManager::Get().GetEntities();
        float bestDot = -999.0f;
        Vector bestHitPos = eyePos;
        bestHitPos.z += 40.0f;

        for (const auto &ent : entities) {
          if (!ent.isEnemy || !ent.pawn)
            continue;
          uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
          if (!Utils::IsValidPtr(pawnAddr))
            continue;

          bool alive = Utils::SafeAlive(ent.pawn);
          if (!alive)
            continue;

          uintptr_t sceneNode =
              Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
          if (!Utils::IsValidPtr(sceneNode))
            continue;

          Vector entPos =
              Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
          entPos.z += 40.0f;

          float dx = entPos.x - eyePos.x;
          float dy = entPos.y - eyePos.y;
          float dz = entPos.z - eyePos.z;
          float dist = sqrtf(dx * dx + dy * dy + dz * dz);
          if (dist < 1.0f)
            continue;

          float ndx = dx / dist, ndy = dy / dist, ndz = dz / dist;
          float dot = fwd.x * ndx + fwd.y * ndy + fwd.z * ndz;

          if (dot > bestDot) {
            bestDot = dot;
            bestHitPos = entPos;
          }
        }

        if (bestDot > 0.5f) {
          HP_SpawnBurst(bestHitPos, Globals::hitparticle_style);
        }
      }
    }
  } else if (totalHits == 0 && s_hp_lastTotalHits != 0) {
  }

  s_hp_lastTotalHits = totalHits;

  float now = Misc_GetTime();
  {
    std::lock_guard<std::mutex> lock(s_hp_mutex);
    s_hp_bursts.erase(std::remove_if(s_hp_bursts.begin(), s_hp_bursts.end(),
                                     [now](const HitParticleBurst &b) {
                                       return (now - b.spawnTime) >
                                              (b.lifetime * 1.5f + 0.1f);
                                     }),
                      s_hp_bursts.end());
    s_hp_distortions.erase(std::remove_if(s_hp_distortions.begin(),
                                          s_hp_distortions.end(),
                                          [now](const HitDistortion &d) {
                                            return (now - d.spawnTime) > d.life;
                                          }),
                           s_hp_distortions.end());
  }
}

void Misc::HitParticleRender() {
  if (!Globals::hitparticle_enabled)
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;

  float now = Misc_GetTime();
  float screenW = Globals::GameViewportWidth;
  float screenH = Globals::GameViewportHeight;

  std::lock_guard<std::mutex> lock(s_hp_mutex);

  for (const auto &dist : s_hp_distortions) {
    float age = now - dist.spawnTime;
    if (age < 0.0f || age > dist.life)
      continue;

    float t = age / dist.life;
    float fade = 1.0f - t * t;

    Vector screen;
    if (!Utils::WorldToScreen(dist.origin, screen, Globals::ViewMatrix, screenW,
                              screenH))
      continue;

    float radius = dist.maxRadius * t * 0.8f;

    Vector offsetWorld = dist.origin;
    offsetWorld.x += radius;
    Vector screenOff;
    if (!Utils::WorldToScreen(offsetWorld, screenOff, Globals::ViewMatrix,
                              screenW, screenH))
      continue;

    float screenRadius = fabsf(screenOff.x - screen.x);
    if (screenRadius < 2.0f)
      continue;

    int alpha = (int)(fade * 45.0f);
    if (alpha <= 0)
      continue;

    draw->AddCircle(ImVec2(screen.x, screen.y), screenRadius,
                    IM_COL32(200, 220, 255, alpha), 48, 2.5f);
    draw->AddCircle(ImVec2(screen.x, screen.y), screenRadius * 0.85f,
                    IM_COL32(255, 255, 255, alpha / 2), 48, 1.2f);
  }

  for (auto &burst : s_hp_bursts) {
    float burstAge = now - burst.spawnTime;
    if (burstAge < 0.0f)
      continue;

    bool useOverride = Globals::hitparticle_color_override;

    for (auto &p : burst.particles) {
      p.age = burstAge;

      float lifeT = p.age / (p.life * burst.lifetime / 0.65f);
      if (lifeT <= 0.0f || lifeT >= 1.0f)
        continue;

      float dt = p.age;
      Vector world;
      world.x = p.pos.x + p.velocity.x * dt * powf(p.drag, dt * 60.0f);
      world.y = p.pos.y + p.velocity.y * dt * powf(p.drag, dt * 60.0f);
      world.z = p.pos.z + p.velocity.z * dt * powf(p.drag, dt * 60.0f) -
                0.5f * p.gravity * dt * dt;

      Vector screen;
      if (!Utils::WorldToScreen(world, screen, Globals::ViewMatrix, screenW,
                                screenH))
        continue;

      Vector camPos;

      uintptr_t client = Memory::GetModuleBase("client.dll");
      float worldDist = 500.0f;
      if (client) {
        uintptr_t lpAddr =
            Memory::Globals::LocalPawn();
        if (Utils::IsValidPtr(lpAddr) && lpAddr > 0x1000000) {
          uintptr_t sn =
              Utils::SafeRead<uintptr_t>(lpAddr + Offsets::m_pGameSceneNode);
          if (Utils::IsValidPtr(sn)) {
            Vector lp = Utils::SafeRead<Vector>(sn + Offsets::m_vecAbsOrigin);
            float dx2 = world.x - lp.x, dy2 = world.y - lp.y,
                  dz2 = world.z - lp.z;
            worldDist = sqrtf(dx2 * dx2 + dy2 * dy2 + dz2 * dz2);
          }
        }
      }

      float distScale = 500.0f / (worldDist + 50.0f);
      if (distScale > 3.0f)
        distScale = 3.0f;
      if (distScale < 0.15f)
        continue;

      float fadeIn = (lifeT < 0.05f) ? lifeT / 0.05f : 1.0f;
      float fadeOut = 1.0f - lifeT;
      fadeOut = fadeOut * fadeOut;
      float alpha = fadeIn * fadeOut * burst.intensity;

      float sizeAnim = 1.0f;
      if (burst.style == 0 || burst.style == 1) {

        sizeAnim = (lifeT < 0.15f) ? (lifeT / 0.15f) : (0.6f + 0.4f * fadeOut);
      } else if (burst.style == 2) {

        sizeAnim = (lifeT < 0.1f) ? (lifeT / 0.1f) * 1.3f : fadeOut * 1.3f;
      } else if (burst.style == 3) {

        sizeAnim = 0.5f + lifeT * 2.0f;
      } else if (burst.style == 4) {

        sizeAnim = fadeOut;
      }

      float size = p.baseSize * burst.sizeMul * distScale * sizeAnim;
      if (size < 0.3f)
        continue;

      float cr, cg, cb, ca;
      if (useOverride) {
        cr = burst.colorR;
        cg = burst.colorG;
        cb = burst.colorB;
        ca = burst.colorA;
      } else {
        float rand01 =
            HP_PseudoRand(burst.spawnTime, (int)(p.rotation * 100.0f));
        HP_GetPresetColor(burst.style, p.layer, rand01, cr, cg, cb, ca);
      }

      int r = (int)(cr * 255.0f);
      int g = (int)(cg * 255.0f);
      int b = (int)(cb * 255.0f);
      int a = (int)(alpha * ca * 255.0f);
      if (a <= 0)
        continue;
      if (a > 255)
        a = 255;

      ImVec2 center(screen.x, screen.y);

      if (burst.glowMul > 0.01f) {
        float glowSize = size * (2.0f + burst.glowMul);
        int glowAlpha = (int)(a * 0.12f * burst.glowMul);
        if (glowAlpha > 0 && glowAlpha <= 255) {
          draw->AddCircleFilled(center, glowSize, IM_COL32(r, g, b, glowAlpha),
                                16);
        }
      }

      {
        float midSize = size * 1.4f;
        int midAlpha = (int)(a * 0.35f);
        if (midAlpha > 0 && midAlpha <= 255) {
          draw->AddCircleFilled(center, midSize, IM_COL32(r, g, b, midAlpha),
                                12);
        }
      }

      {
        draw->AddCircleFilled(center, size, IM_COL32(r, g, b, a), 12);
      }

      if (burst.style != 3) {
        float coreSize = size * 0.35f;
        int coreAlpha = (int)(a * 0.6f * fadeOut);
        if (coreAlpha > 0 && coreAlpha <= 255 && coreSize > 0.3f) {

          int wR = r + (int)((255 - r) * 0.6f);
          int wG = g + (int)((255 - g) * 0.5f);
          int wB = b + (int)((255 - b) * 0.3f);
          draw->AddCircleFilled(center, coreSize,
                                IM_COL32(wR, wG, wB, coreAlpha), 8);
        }
      }

      if (burst.style == 3 && size > 2.0f) {
        float ringAlpha = alpha * ca;
        int rA = (int)(ringAlpha * 180.0f);
        if (rA > 0 && rA <= 255) {
          draw->AddCircle(center, size * 1.2f, IM_COL32(r, g, b, rA), 32,
                          2.0f * distScale);
          draw->AddCircle(center, size * 0.8f, IM_COL32(r, g, b, rA / 2), 32,
                          1.2f * distScale);
        }
      }

      if (burst.style == 4 && lifeT < 0.6f && size > 1.0f) {
        float trailLen = size * 1.8f;
        float vLen =
            sqrtf(p.velocity.x * p.velocity.x + p.velocity.y * p.velocity.y +
                  p.velocity.z * p.velocity.z);
        if (vLen > 10.0f) {
          Vector trailEnd;
          float trailDt = dt - 0.008f;
          if (trailDt < 0.0f)
            trailDt = 0.0f;
          trailEnd.x =
              p.pos.x + p.velocity.x * trailDt * powf(p.drag, trailDt * 60.0f);
          trailEnd.y =
              p.pos.y + p.velocity.y * trailDt * powf(p.drag, trailDt * 60.0f);
          trailEnd.z = p.pos.z +
                       p.velocity.z * trailDt * powf(p.drag, trailDt * 60.0f) -
                       0.5f * p.gravity * trailDt * trailDt;
          Vector trailScreen;
          if (Utils::WorldToScreen(trailEnd, trailScreen, Globals::ViewMatrix,
                                   screenW, screenH)) {
            int tA = (int)(a * 0.5f);
            if (tA > 0) {
              draw->AddLine(ImVec2(trailScreen.x, trailScreen.y), center,
                            IM_COL32(r, g, b, tA), 1.2f * distScale);
            }
          }
        }
      }
    }
  }
}

struct HitLogEntry {
  std::string hitboxName;
  std::string playerName;
  int damage;
  float spawnTime;
  float yOffset;
  float targetYOffset;
  float alpha;
  bool isKill;
};

struct WorldDamageNumber {
  Vector worldPos;
  int damage;
  float spawnTime;
  float scale;
  float alpha;
  bool isKill;
};

static std::vector<HitLogEntry> s_hitlog_entries;
static std::vector<WorldDamageNumber> s_world_damage_numbers;
static std::mutex s_hitlog_mutex;
static int s_hitlog_lastTotalHits = 0;
static bool s_hitlog_initialized = false;
static std::unordered_map<uintptr_t, int> s_hitlog_lastHealth;

static const char *GetHitboxName(int hitgroup) {
  switch (hitgroup) {
  case 1:
    return "HEAD";
  case 2:
    return "CHEST";
  case 3:
    return "STOMACH";
  case 4:
    return "LEFT ARM";
  case 5:
    return "RIGHT ARM";
  case 6:
    return "LEFT LEG";
  case 7:
    return "RIGHT LEG";
  case 0:
    return "BODY";
  default:
    return "BODY";
  }
}

static int TraceHitbox(const Vector &eyePos, const Vector &direction,
                       C_CSPlayerPawn *targetPawn) {
  if (!Utils::IsValidPtr(reinterpret_cast<uintptr_t>(targetPawn)))
    return 0;

  Vector headPos = Utils::GetBonePos(targetPawn, BoneID::Head);
  Vector neckPos = Utils::GetBonePos(targetPawn, BoneID::Neck);
  Vector spinePos = Utils::GetBonePos(targetPawn, BoneID::Spine);
  Vector pelvisPos = Utils::GetBonePos(targetPawn, BoneID::Pelvis);
  Vector leftArmPos = Utils::GetBonePos(targetPawn, BoneID::LeftArm);
  Vector rightArmPos = Utils::GetBonePos(targetPawn, BoneID::RightArm);
  Vector leftKneePos = Utils::GetBonePos(targetPawn, BoneID::LeftKnee);
  Vector rightKneePos = Utils::GetBonePos(targetPawn, BoneID::RightKnee);
  Vector leftFootPos = Utils::GetBonePos(targetPawn, BoneID::LeftFoot);
  Vector rightFootPos = Utils::GetBonePos(targetPawn, BoneID::RightFoot);

  auto DistanceToRay = [&](const Vector &bonePos) -> float {
    if (bonePos.IsZero())
      return 9999.0f;

    Vector toPoint = bonePos - eyePos;
    float proj = toPoint.Dot(direction);
    if (proj < 0.0f)
      return 9999.0f;

    Vector closest = eyePos + direction * proj;
    return (bonePos - closest).Length();
  };

  struct BoneHit {
    int hitgroup;
    float distance;
  };

  std::vector<BoneHit> hits;

  float headDist = DistanceToRay(headPos);
  if (headDist < 12.0f)
    hits.push_back({1, headDist});

  float neckDist = DistanceToRay(neckPos);
  if (neckDist < 15.0f)
    hits.push_back({2, neckDist});

  float spineDist = DistanceToRay(spinePos);
  if (spineDist < 15.0f)
    hits.push_back({2, spineDist});

  float pelvisDist = DistanceToRay(pelvisPos);
  if (pelvisDist < 15.0f)
    hits.push_back({3, pelvisDist});

  float leftArmDist = DistanceToRay(leftArmPos);
  if (leftArmDist < 8.0f)
    hits.push_back({4, leftArmDist});

  float rightArmDist = DistanceToRay(rightArmPos);
  if (rightArmDist < 8.0f)
    hits.push_back({5, rightArmDist});

  float leftKneeDist = DistanceToRay(leftKneePos);
  if (leftKneeDist < 10.0f)
    hits.push_back({6, leftKneeDist});

  float leftFootDist = DistanceToRay(leftFootPos);
  if (leftFootDist < 10.0f)
    hits.push_back({6, leftFootDist});

  float rightKneeDist = DistanceToRay(rightKneePos);
  if (rightKneeDist < 10.0f)
    hits.push_back({7, rightKneeDist});

  float rightFootDist = DistanceToRay(rightFootPos);
  if (rightFootDist < 10.0f)
    hits.push_back({7, rightFootDist});

  if (hits.empty())
    return 0;

  BoneHit closest = hits[0];
  for (const auto &hit : hits) {
    if (hit.distance < closest.distance)
      closest = hit;
  }

  return closest.hitgroup;
}

void Misc::HitLogUpdate() {
  if (!Globals::hitlog_enabled && !Globals::hitlog_draw_damage) {
    std::lock_guard<std::mutex> lock(s_hitlog_mutex);
    s_hitlog_entries.clear();
    s_world_damage_numbers.clear();
    s_hitlog_lastHealth.clear();
    s_hitlog_lastTotalHits = 0;
    s_hitlog_initialized = false;
    return;
  }

  int totalHits = 0;
  if (!ReadHitsoundData(totalHits))
    return;

  if (!s_hitlog_initialized) {
    s_hitlog_lastTotalHits = totalHits;
    s_hitlog_initialized = true;
    return;
  }

  if (totalHits > s_hitlog_lastTotalHits && s_hitlog_lastTotalHits >= 0) {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (client) {
      uintptr_t localPawnAddr =
          Memory::Globals::LocalPawn();
      if (Utils::IsValidPtr(localPawnAddr) && localPawnAddr > 0x1000000) {

        uintptr_t localScene = Utils::SafeRead<uintptr_t>(
            localPawnAddr + Offsets::m_pGameSceneNode);
        Vector localOrigin = {0, 0, 0};
        if (Utils::IsValidPtr(localScene))
          localOrigin =
              Utils::SafeRead<Vector>(localScene + Offsets::m_vecAbsOrigin);
        Vector viewOff =
            Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecViewOffset);
        Vector eyePos = {localOrigin.x + viewOff.x, localOrigin.y + viewOff.y,
                         localOrigin.z + viewOff.z};

        uintptr_t viewAnglesAddr = client + Offsets::dwViewAngles;
        float pitch = Utils::SafeRead<float>(viewAnglesAddr);
        float yaw = Utils::SafeRead<float>(viewAnglesAddr + 4);
        constexpr float D2R = 3.14159265f / 180.0f;
        float cp = cosf(pitch * D2R), sp = sinf(pitch * D2R);
        float cy = cosf(yaw * D2R), sy = sinf(yaw * D2R);
        Vector fwd = {cp * cy, cp * sy, -sp};

        auto entities = EntityManager::Get().GetEntities();
        float bestDot = -999.0f;
        uintptr_t bestPawn = 0;
        int bestPrevHealth = 100;
        int bestCurrentHealth = 100;

        for (const auto &ent : entities) {
          if (!ent.isEnemy || !ent.pawn)
            continue;
          uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
          if (!Utils::IsValidPtr(pawnAddr))
            continue;

          uintptr_t sceneNode =
              Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
          if (!Utils::IsValidPtr(sceneNode))
            continue;

          Vector entPos =
              Utils::SafeRead<Vector>(sceneNode + Offsets::m_vecAbsOrigin);
          entPos.z += 40.0f;

          float dx = entPos.x - eyePos.x;
          float dy = entPos.y - eyePos.y;
          float dz = entPos.z - eyePos.z;
          float dist = sqrtf(dx * dx + dy * dy + dz * dz);
          if (dist < 1.0f)
            continue;

          float ndx = dx / dist, ndy = dy / dist, ndz = dz / dist;
          float dot = fwd.x * ndx + fwd.y * ndy + fwd.z * ndz;

          if (dot > bestDot) {
            bestDot = dot;
            bestPawn = pawnAddr;
            bestCurrentHealth =
                Utils::SafeRead<int>(pawnAddr + Offsets::m_iHealth);

            auto it = s_hitlog_lastHealth.find(pawnAddr);
            if (it != s_hitlog_lastHealth.end())
              bestPrevHealth = it->second;
            else
              bestPrevHealth = bestCurrentHealth;
          }
        }

        if (bestDot > 0.5f && bestPawn != 0) {

          int damage = bestPrevHealth - bestCurrentHealth;
          bool isKill = false;

          if (damage <= 0) {

            bool alive =
                Utils::SafeAlive(reinterpret_cast<C_CSPlayerPawn *>(bestPawn));
            if (!alive || bestCurrentHealth <= 0) {
              damage = 0;
              isKill = true;
            } else {

              s_hitlog_lastTotalHits = totalHits;
              return;
            }
          }

          if (damage > 100)
            damage = 100;

          C_CSPlayerPawn *pawn = reinterpret_cast<C_CSPlayerPawn *>(bestPawn);
          int hitgroup = TraceHitbox(eyePos, fwd, pawn);
          const char *hitboxName = GetHitboxName(hitgroup);

          std::string playerName = "Enemy";
          for (const auto &ent : entities) {
            if (reinterpret_cast<uintptr_t>(ent.pawn) == bestPawn &&
                ent.controller) {
              uintptr_t ctrlAddr = reinterpret_cast<uintptr_t>(ent.controller);
              char nameBuf[128] = {};
              if (ReadPlayerName(ctrlAddr, nameBuf, sizeof(nameBuf)) &&
                  nameBuf[0] != '\0') {
                playerName = std::string(nameBuf);
              }
              break;
            }
          }

          HitLogEntry entry;
          entry.hitboxName = hitboxName;
          entry.playerName = playerName;
          entry.damage = damage;
          entry.spawnTime = Misc_GetTime();
          entry.yOffset = -80.0f;
          entry.targetYOffset = 0.0f;
          entry.alpha = 0.0f;
          entry.isKill = isKill;

          std::lock_guard<std::mutex> lock(s_hitlog_mutex);

          for (auto &e : s_hitlog_entries) {
            e.targetYOffset += 70.0f;
          }

          s_hitlog_entries.push_back(entry);

          while (s_hitlog_entries.size() >
                 (size_t)Globals::hitlog_max_visible) {
            s_hitlog_entries.erase(s_hitlog_entries.begin());
          }

          if (Globals::hitlog_draw_damage) {

            Vector hitPos = {0, 0, 0};

            switch (hitgroup) {
            case 1:
              hitPos = Utils::GetBonePos(pawn, BoneID::Head);
              break;
            case 2:
              hitPos = Utils::GetBonePos(pawn, BoneID::Spine);
              break;
            case 3:
              hitPos = Utils::GetBonePos(pawn, BoneID::Pelvis);
              break;
            case 4:
              hitPos = Utils::GetBonePos(pawn, BoneID::LeftArm);
              break;
            case 5:
              hitPos = Utils::GetBonePos(pawn, BoneID::RightArm);
              break;
            case 6:
              hitPos = Utils::GetBonePos(pawn, BoneID::LeftKnee);
              break;
            case 7:
              hitPos = Utils::GetBonePos(pawn, BoneID::RightKnee);
              break;
            default:
              hitPos = Utils::GetBonePos(pawn, BoneID::Spine);
              break;
            }

            if (!hitPos.IsZero()) {
              WorldDamageNumber worldDmg;
              worldDmg.worldPos = hitPos;
              worldDmg.damage = damage;
              worldDmg.spawnTime = Misc_GetTime();
              worldDmg.scale = 0.5f;
              worldDmg.alpha = 1.0f;
              worldDmg.isKill = isKill;
              s_world_damage_numbers.push_back(worldDmg);
            }
          }

          s_hitlog_lastHealth[bestPawn] = bestCurrentHealth;
        }
      }
    }
  } else if (totalHits == 0 && s_hitlog_lastTotalHits != 0) {

    s_hitlog_lastHealth.clear();
  }

  s_hitlog_lastTotalHits = totalHits;

  auto entities = EntityManager::Get().GetEntities();
  std::unordered_set<uintptr_t> seenPawns;

  for (const auto &ent : entities) {
    if (!ent.isEnemy || !ent.pawn)
      continue;
    uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
    if (!Utils::IsValidPtr(pawnAddr))
      continue;

    seenPawns.insert(pawnAddr);

    int health = Utils::SafeRead<int>(pawnAddr + Offsets::m_iHealth);
    s_hitlog_lastHealth[pawnAddr] = health;
  }

  for (auto it = s_hitlog_lastHealth.begin();
       it != s_hitlog_lastHealth.end();) {
    if (seenPawns.find(it->first) == seenPawns.end())
      it = s_hitlog_lastHealth.erase(it);
    else
      ++it;
  }

  float now = Misc_GetTime();
  {
    std::lock_guard<std::mutex> lock(s_hitlog_mutex);
    s_hitlog_entries.erase(
        std::remove_if(s_hitlog_entries.begin(), s_hitlog_entries.end(),
                       [now](const HitLogEntry &e) {
                         return (now - e.spawnTime) > Globals::hitlog_duration;
                       }),
        s_hitlog_entries.end());

    s_world_damage_numbers.erase(
        std::remove_if(s_world_damage_numbers.begin(),
                       s_world_damage_numbers.end(),
                       [now](const WorldDamageNumber &d) {
                         return (now - d.spawnTime) > 2.0f;
                       }),
        s_world_damage_numbers.end());
  }
}

void Misc::HitLogRender() {
  if (!Globals::hitlog_enabled && !Globals::hitlog_draw_damage)
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;

  float now = Misc_GetTime();
  float slideSpeed = Globals::hitlog_slide_speed;
  float fadeSpeed = Globals::hitlog_fade_speed;

  std::lock_guard<std::mutex> lock(s_hitlog_mutex);

  if (Globals::hitlog_draw_damage) {
    float screenW = Globals::GameViewportWidth;
    float screenH = Globals::GameViewportHeight;

    for (auto &dmg : s_world_damage_numbers) {
      float age = now - dmg.spawnTime;
      if (age < 0.0f || age > 2.0f)
        continue;

      float growPhase = 0.3f;
      float holdPhase = 0.5f;
      float fadePhase = 1.2f;

      if (age < growPhase) {
        float t = age / growPhase;
        t = t * t * (3.0f - 2.0f * t);
        dmg.scale = 0.5f + t * 1.0f;
      } else if (age < growPhase + holdPhase) {
        dmg.scale = 1.5f;
      } else {
        float t = (age - growPhase - holdPhase) / fadePhase;
        dmg.scale = 1.5f + t * 0.3f;
      }

      if (age < growPhase + holdPhase) {
        dmg.alpha = 1.0f;
      } else {
        float t = (age - growPhase - holdPhase) / fadePhase;
        dmg.alpha = 1.0f - t;
      }

      if (dmg.alpha <= 0.0f)
        continue;

      Vector worldPos = dmg.worldPos;
      worldPos.z += age * 30.0f;

      Vector screenPos;
      if (!Utils::WorldToScreen(worldPos, screenPos, Globals::ViewMatrix,
                                screenW, screenH))
        continue;

      char damageText[32];
      if (dmg.isKill) {
        snprintf(damageText, sizeof(damageText), "KILL");
      } else {
        snprintf(damageText, sizeof(damageText), "-%d", dmg.damage);
      }

      ImU32 textColor;
      if (dmg.isKill) {
        textColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.2f, 0.2f, dmg.alpha));
      } else {
        textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            Globals::hitlog_damage_color[0], Globals::hitlog_damage_color[1],
            Globals::hitlog_damage_color[2], dmg.alpha));
      }

      ImFont *font = ImGui::GetFont();
      float baseFontSize = font->FontSize;
      float fontSize = baseFontSize * dmg.scale;

      ImVec2 textSize =
          font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, damageText);
      ImVec2 textPos(screenPos.x - textSize.x * 0.5f,
                     screenPos.y - textSize.y * 0.5f);

      ImU32 shadowColor = IM_COL32(0, 0, 0, (int)(dmg.alpha * 200.0f));
      for (int ox = -1; ox <= 1; ox++) {
        for (int oy = -1; oy <= 1; oy++) {
          if (ox == 0 && oy == 0)
            continue;
          draw->AddText(font, fontSize,
                        ImVec2(textPos.x + ox * 2, textPos.y + oy * 2),
                        shadowColor, damageText);
        }
      }

      draw->AddText(font, fontSize, textPos, textColor, damageText);
    }
  }

  if (!Globals::hitlog_enabled)
    return;

  float startX = Globals::GameViewportX + 20.0f;
  float startY = Globals::GameViewportY + 20.0f;

  for (auto &entry : s_hitlog_entries) {
    float age = now - entry.spawnTime;
    float lifetime = Globals::hitlog_duration;

    if (age < slideSpeed) {
      float t = age / slideSpeed;
      t = t * t * (3.0f - 2.0f * t);
      entry.yOffset = -80.0f + t * (entry.targetYOffset + 80.0f);
      entry.alpha = t;
    } else {

      float diff = entry.targetYOffset - entry.yOffset;
      entry.yOffset += diff * 0.15f;
      entry.alpha = 1.0f;
    }

    if (age > lifetime - fadeSpeed) {
      float fadeT = (lifetime - age) / fadeSpeed;
      entry.alpha = fadeT;
    }

    if (entry.alpha <= 0.0f)
      continue;

    float posX = startX;
    float posY = startY + entry.yOffset;

    float panelW = entry.isKill ? 320.0f : 280.0f;
    float panelH = 60.0f;
    float rounding = 8.0f;

    int bgAlpha = (int)(entry.alpha * 220.0f);
    draw->AddRectFilled(ImVec2(posX, posY),
                        ImVec2(posX + panelW, posY + panelH),
                        IM_COL32(18, 18, 18, bgAlpha), rounding);

    int borderAlpha = (int)(entry.alpha * 100.0f);
    draw->AddRect(ImVec2(posX, posY), ImVec2(posX + panelW, posY + panelH),
                  IM_COL32(60, 60, 60, borderAlpha), rounding, 0, 1.5f);

    ImU32 accentColor;
    if (entry.isKill) {
      accentColor =
          ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.2f, 0.2f, entry.alpha));
    } else {
      accentColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
          Globals::hitlog_damage_color[0], Globals::hitlog_damage_color[1],
          Globals::hitlog_damage_color[2], entry.alpha));
    }
    draw->AddRectFilled(ImVec2(posX + 1, posY + 1),
                        ImVec2(posX + panelW - 1, posY + 3), accentColor);

    float textY = posY + 15.0f;
    int textAlpha = (int)(entry.alpha * 255.0f);

    if (entry.isKill) {

      ImVec2 skullCenter(posX + 22.0f, textY + 8.0f);
      float skullSize = 10.0f;

      draw->AddCircleFilled(skullCenter, skullSize,
                            IM_COL32(255, 50, 50, textAlpha), 16);
      draw->AddCircleFilled(
          ImVec2(skullCenter.x, skullCenter.y + skullSize * 0.6f),
          skullSize * 0.7f, IM_COL32(255, 50, 50, textAlpha), 16);

      draw->AddCircleFilled(ImVec2(skullCenter.x - 4.0f, skullCenter.y - 2.0f),
                            2.5f, IM_COL32(0, 0, 0, textAlpha), 8);
      draw->AddCircleFilled(ImVec2(skullCenter.x + 4.0f, skullCenter.y - 2.0f),
                            2.5f, IM_COL32(0, 0, 0, textAlpha), 8);

      draw->AddTriangleFilled(
          ImVec2(skullCenter.x, skullCenter.y + 2.0f),
          ImVec2(skullCenter.x - 2.0f, skullCenter.y + 5.0f),
          ImVec2(skullCenter.x + 2.0f, skullCenter.y + 5.0f),
          IM_COL32(0, 0, 0, textAlpha));

      char killText[256];
      snprintf(killText, sizeof(killText), "%s OLDU", entry.playerName.c_str());

      ImU32 killColor =
          ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.3f, 0.3f, entry.alpha));

      ImFont *currentFont = ImGui::GetFont();
      float fontSize = currentFont->FontSize * 1.2f;

      draw->AddText(currentFont, fontSize, ImVec2(posX + 50.0f + 1, textY + 1),
                    IM_COL32(0, 0, 0, textAlpha / 2), killText);
      draw->AddText(currentFont, fontSize, ImVec2(posX + 50.0f, textY),
                    killColor, killText);
    } else {

      draw->AddText(ImVec2(posX + 15.0f, textY),
                    IM_COL32(200, 200, 200, textAlpha), "HIT");

      ImU32 hitboxColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
          Globals::hitlog_hitbox_color[0], Globals::hitlog_hitbox_color[1],
          Globals::hitlog_hitbox_color[2], entry.alpha));

      draw->AddText(ImVec2(posX + 55.0f, textY), hitboxColor,
                    entry.hitboxName.c_str());

      char damageText[32];
      snprintf(damageText, sizeof(damageText), "-%d HP", entry.damage);

      ImU32 damageColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
          Globals::hitlog_damage_color[0], Globals::hitlog_damage_color[1],
          Globals::hitlog_damage_color[2], entry.alpha));

      ImFont *currentFont = ImGui::GetFont();
      float fontSize = currentFont->FontSize * 1.3f;

      ImVec2 damageSize = ImGui::CalcTextSize(damageText);
      float damageX = posX + panelW - damageSize.x - 15.0f;

      draw->AddText(currentFont, fontSize, ImVec2(damageX + 1, textY + 1),
                    IM_COL32(0, 0, 0, textAlpha / 2), damageText);
      draw->AddText(currentFont, fontSize, ImVec2(damageX, textY), damageColor,
                    damageText);
    }
  }
}

static std::chrono::steady_clock::time_point s_chatspam_lastSend;
static bool s_chatspam_initialized = false;

void Misc::ChatSpamUpdate() {
  if (!Globals::chatspam_enabled) {
    s_chatspam_initialized = false;
    return;
  }

  if (!Interfaces::m_pEngine)
    return;

  if (!s_chatspam_initialized) {
    s_chatspam_lastSend = std::chrono::steady_clock::now();
    s_chatspam_initialized = true;
    return;
  }

  auto now = std::chrono::steady_clock::now();
  float elapsed =
      std::chrono::duration<float>(now - s_chatspam_lastSend).count();

  if (elapsed < Globals::chatspam_interval)
    return;
  if (Globals::chatspam_message[0] == '\0')
    return;

  s_chatspam_lastSend = now;

  char cmd[300] = {};
  snprintf(cmd, sizeof(cmd), "say %s", Globals::chatspam_message);

  __try {
    Interfaces::m_pEngine->ExecuteClientCMD(cmd);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static ImU32 RainbowColor(float timeOffset, float speed, float alpha = 1.0f) {
  float t = static_cast<float>(GetTickCount64()) / 1000.0f * speed + timeOffset;
  float r = std::sin(t) * 0.5f + 0.5f;
  float g = std::sin(t + 2.094f) * 0.5f + 0.5f;
  float b = std::sin(t + 4.189f) * 0.5f + 0.5f;
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

void Misc::WatermarkRender() {
  if (!Globals::watermark_enabled)
    return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  if (!dl)
    return;

  ImFont *titleFont = widget_font_big ? widget_font_big : ImGui::GetFont();
  ImFont *bodyFont  = widget_font     ? widget_font     : ImGui::GetFont();
  const float scale = HudScale();

  // -- live data --------------------------------------------------------
  // FPS is jittery frame-to-frame. We keep an exponentially-smoothed
  // running average and only refresh the displayed integer twice a
  // second — long enough to stop the number flickering, short enough
  // to still feel "live".
  static float s_fpsAvg     = 0.f;
  static float s_fpsDisplay = 0.f;
  static double s_fpsLastShown = 0.0;

  float dt = ImGui::GetIO().DeltaTime;
  if (dt > 1e-5f) {
      float inst = 1.0f / dt;
      if (s_fpsAvg <= 1.f)            // first frame
          s_fpsAvg = inst;
      else
          s_fpsAvg += (inst - s_fpsAvg) * 0.06f;   // ~0.5 s time-const
  }
  double tNow = ImGui::GetTime();
  if (tNow - s_fpsLastShown >= 0.5 || s_fpsDisplay == 0.f) {
      s_fpsDisplay   = s_fpsAvg;
      s_fpsLastShown = tNow;
  }

  time_t now = time(nullptr);
  struct tm ts; localtime_s(&ts, &now);
  char timeBuf[16]; strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ts);
  char fpsBuf[24];  snprintf(fpsBuf,  sizeof(fpsBuf),  "%.0f fps", s_fpsDisplay);

  // -- layout -----------------------------------------------------------
  const float kPadX     = 14.f * scale;
  const float kPadY     = 7.f * scale;
  const float kGap      = 12.f * scale; // gap between segments
  const float kSepW     = 1.f;
  const float kBrandSz  = 15.f * scale;
  const float kBodySz   = 13.f * scale;

  // measure each piece
  const char *brandA = "RAVEN";
  const char *brandB = ".CASH";
  ImVec2 brandASz = titleFont->CalcTextSizeA(kBrandSz, FLT_MAX, 0.f, brandA);
  ImVec2 brandBSz = titleFont->CalcTextSizeA(kBrandSz, FLT_MAX, 0.f, brandB);
  ImVec2 fpsSz    = bodyFont->CalcTextSizeA (kBodySz,  FLT_MAX, 0.f, fpsBuf);
  ImVec2 timeSz   = bodyFont->CalcTextSizeA (kBodySz,  FLT_MAX, 0.f, timeBuf);

  float dotW   = 14.f * scale;    // pulsing accent dot + its padding
  float lineH  = (std::max)(brandASz.y,
                            (std::max)(fpsSz.y, timeSz.y));
  float totalW = dotW + brandASz.x + brandBSz.x + kGap + kSepW + kGap +
                 fpsSz.x + kGap + kSepW + kGap + timeSz.x;
  float boxW   = totalW + kPadX * 2.f;
  float boxH   = lineH  + kPadY * 2.f;

  // top-right anchor with a small inset.
  float viewportRight = HudViewportRight();
  ImVec2 boxMin = ImVec2(viewportRight - boxW - 14.f * scale,
                         HudViewportTop() + 14.f * scale);
  ImVec2 boxMax = ImVec2(boxMin.x + boxW, boxMin.y + boxH);

  // -- shell ------------------------------------------------------------
  const float rounding = 6.f * scale;
  // Soft outer drop-shadow.
  dl->AddRectFilled(
      ImVec2(boxMin.x - 1.f, boxMin.y + 2.f),
      ImVec2(boxMax.x + 1.f, boxMax.y + 3.f),
      HudColor(0, 0, 0, 80), rounding + 1.f);
  // Dark glass body.
  dl->AddRectFilled(boxMin, boxMax, HudColor(14, 15, 18, 230), rounding);
  // Subtle gray hairline border.
  dl->AddRect(boxMin, boxMax, HudColor(50, 52, 60, 220), rounding, 0, 1.0f);
  // Top 1-px crimson rail — the only red surface element.
  dl->AddRectFilled(
      ImVec2(boxMin.x + rounding * 0.5f, boxMin.y),
      ImVec2(boxMax.x - rounding * 0.5f, boxMin.y + 1.5f),
      HudAccent(0.94f));

  // -- contents ---------------------------------------------------------
  float cy = boxMin.y + (boxH - lineH) * 0.5f;
  float cx = boxMin.x + kPadX;

  // Pulsing crimson dot (pulses on the second tick).
  {
    float t = (float)ImGui::GetTime();
    float pulse = 0.55f + 0.45f * (0.5f + 0.5f * sinf(t * 2.4f));
    ImVec2 dotC = ImVec2(cx + 4.f * scale, boxMin.y + boxH * 0.5f);
    // Halo
    dl->AddCircleFilled(
        dotC, 6.f * scale, HudAccent(0.27f * pulse), 18);
    // Core
    dl->AddCircleFilled(
        dotC, 3.2f * scale, HudAccent(), 16);
    cx += dotW;
  }

  // Brand "RAVEN" + ".CASH"
  dl->AddText(titleFont, kBrandSz, ImVec2(cx, cy),
              IM_COL32(245, 246, 250, 255), brandA);
  cx += brandASz.x;
  dl->AddText(titleFont, kBrandSz, ImVec2(cx, cy),
              HudAccent(), brandB);
  cx += brandBSz.x + kGap;

  auto sep = [&](float x) {
    dl->AddRectFilled(
        ImVec2(x, boxMin.y + 8.f),
        ImVec2(x + kSepW, boxMax.y - 8.f),
        HudColor(55, 58, 66, 220));
  };

  sep(cx); cx += kSepW + kGap;
  dl->AddText(bodyFont, kBodySz, ImVec2(cx, cy + (lineH - fpsSz.y) * 0.5f),
              IM_COL32(195, 198, 210, 255), fpsBuf);
  cx += fpsSz.x + kGap;

  sep(cx); cx += kSepW + kGap;
  dl->AddText(bodyFont, kBodySz, ImVec2(cx, cy + (lineH - timeSz.y) * 0.5f),
              IM_COL32(195, 198, 210, 255), timeBuf);
}

void Misc::FeatureStatusRender() {
  if (!Globals::hud_status_enabled)
    return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (!dl)
    return;

  struct StatusEntry {
    const char* label;
    bool enabled;
  };

  const std::array<StatusEntry, 5> entries = {{
      {"SILENT", Globals::silent_aim_enabled},
      {"NO SPREAD", Globals::silent_aim_enabled && Globals::silent_aim_nospread},
      {"ANTI-AIM", Globals::antiaim_enabled},
      {"BHOP", Globals::bunnyhop_enabled},
      {"3RD PERSON", Globals::thirdperson_enabled},
  }};

  std::vector<const char*> activeLabels;
  activeLabels.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.enabled)
      activeLabels.push_back(entry.label);
  }

  const bool hasActiveFeature = !activeLabels.empty();
  if (!hasActiveFeature)
    activeLabels.push_back("READY");

  ImFont* font = widget_font ? widget_font : ImGui::GetFont();
  const float scale = HudScale();
  const float fontSize = 11.5f * scale;
  const float padX = 10.f * scale;
  const float gap = 5.f * scale;
  const float chipPad = 7.f * scale;
  const float boxH = 30.f * scale;
  const float rounding = 6.f * scale;
  const char* heading = "ACTIVE";
  const ImVec2 headingSize =
      font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, heading);

  float boxW = padX * 2.f + headingSize.x + 10.f * scale;
  for (const char* label : activeLabels) {
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
    boxW += textSize.x + chipPad * 2.f + gap;
  }
  boxW -= gap;

  const float viewportRight = HudViewportRight();
  float y = HudViewportTop() + 14.f * scale;
  if (Globals::watermark_enabled)
    y += 37.f * scale;
  const ImVec2 boxMin(viewportRight - boxW - 14.f * scale, y);
  const ImVec2 boxMax(boxMin.x + boxW, boxMin.y + boxH);

  dl->AddRectFilled(ImVec2(boxMin.x - 1.f, boxMin.y + 2.f),
                    ImVec2(boxMax.x + 1.f, boxMax.y + 3.f),
                    HudColor(0, 0, 0, 74), rounding + 1.f);
  dl->AddRectFilled(boxMin, boxMax, HudColor(14, 15, 18, 230), rounding);
  dl->AddRect(boxMin, boxMax, HudColor(50, 52, 60, 220), rounding, 0, 1.f);
  dl->AddRectFilled(boxMin,
                    ImVec2(boxMin.x + 2.f * scale, boxMax.y),
                    HudAccent(0.94f), rounding);

  float x = boxMin.x + padX;
  const float headingY = boxMin.y + (boxH - headingSize.y) * 0.5f;
  dl->AddText(font, fontSize, ImVec2(x, headingY), HudAccent(), heading);
  x += headingSize.x + 10.f * scale;

  for (const char* label : activeLabels) {
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
    const float chipW = textSize.x + chipPad * 2.f;
    const float chipH = 20.f * scale;
    const float chipY = boxMin.y + (boxH - chipH) * 0.5f;
    const ImVec2 chipMin(x, chipY);
    const ImVec2 chipMax(x + chipW, chipY + chipH);

    dl->AddRectFilled(chipMin, chipMax,
                      hasActiveFeature ? HudAccent(0.13f)
                                       : HudColor(40, 42, 50, 180),
                      chipH * 0.5f);
    dl->AddRect(chipMin, chipMax,
                hasActiveFeature ? HudAccent(0.34f)
                                 : HudColor(70, 73, 82, 170),
                chipH * 0.5f, 0, 1.f);
    dl->AddText(font, fontSize,
                ImVec2(chipMin.x + chipPad,
                       chipMin.y + (chipH - textSize.y) * 0.5f),
                hasActiveFeature ? HudColor(238, 240, 247, 255)
                                 : HudColor(150, 154, 165, 240),
                label);
    x += chipW + gap;
  }
}

void Misc::SmokeTimerRender() {
  if (!Globals::smoketimer_enabled) {
    s_smokeTimers.clear();
    return;
  }

  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
      return;

    uintptr_t entityList =
        Utils::SafeRead<uintptr_t>(client + Offsets::dwEntityList);
    if (!Utils::IsValidPtr(entityList))
      return;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (!dl)
      return;

    float screenW = (float)Globals::ScreenWidth;
    float screenH = (float)Globals::ScreenHeight;
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 1024; i++) {
      uintptr_t entry = Utils::SafeRead<uintptr_t>(
          entityList + (8 * ((i & 0x7FFF) >> 9) + 16));
      if (!Utils::IsValidPtr(entry))
        continue;

      uintptr_t entity =
          Utils::SafeRead<uintptr_t>(entry + (112 * (i & 0x1FF)));
      if (!Utils::IsValidPtr(entity) || entity < 0x1000000)
        continue;

      uintptr_t entityIdentity = Utils::SafeRead<uintptr_t>(entity + 0x10);
      if (!Utils::IsValidPtr(entityIdentity) || entityIdentity < 0x1000000)
        continue;

      uintptr_t designerNamePtr =
          Utils::SafeRead<uintptr_t>(entityIdentity + 0x20);
      if (!Utils::IsValidPtr(designerNamePtr))
        continue;

      char className[64] = {};
      if (!Utils::SafeReadString(designerNamePtr, className, sizeof(className)))
        continue;

      // We moved the tracking to SmokeColorChanger
      // so it can catch the tick before nosmoke zeros it.
    }

    for (auto it = s_smokeTimers.begin(); it != s_smokeTimers.end();) {
      float elapsed =
          std::chrono::duration<float>(now - it->second.startTime).count();
      const float SMOKE_DURATION = 21.5f;
      float remaining = SMOKE_DURATION - elapsed;

      if (remaining <= 0.0f) {
        it = s_smokeTimers.erase(it);
      } else {
        Vector screen;
        if (Utils::WorldToScreen(it->second.position, screen,
                                 Globals::ViewMatrix, screenW, screenH)) {
          float frac = std::clamp(remaining / SMOKE_DURATION, 0.f, 1.f);

          ImU32 timerColor = ImGui::ColorConvertFloat4ToU32(
              ImVec4(1.0f - frac, frac, 0.0f, 1.0f));
          ImU32 bgColor = IM_COL32(15, 15, 15, 200);

          float barW = 80.f;
          float barH = 8.f;
          float barX = screen.x - barW * 0.5f;
          float barY = screen.y + 15.f;

          dl->AddRectFilled(ImVec2(barX - 2, barY - 2),
                            ImVec2(barX + barW + 2, barY + barH + 2), bgColor,
                            4.f);
          dl->AddRectFilled(ImVec2(barX, barY),
                            ImVec2(barX + barW * frac, barY + barH), timerColor,
                            2.f);

          char text[32];
          snprintf(text, sizeof(text), "SMOKE: %.1fs", remaining);
          ImVec2 textSize = ImGui::CalcTextSize(text);
          dl->AddText(ImVec2(screen.x - textSize.x * 0.5f, barY - 18.f),
                      IM_COL32(255, 255, 255, 255), text);
        }
        ++it;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void Misc::Render() {

  CinematicPostProcessRender();
  HitmarkerRender();

  SpectatorListRender();

  BombTimerRender();
  SmokeTimerRender();
  BulletTracerRender();
  HitParticleRender();
  DeathDustDissolveRender();
  HitLogRender();
  CrosshairIndicatorRender();
  WatermarkRender();
  FeatureStatusRender();
  MovementKeysRender();
  VelocityGraphRender();
}

void Misc::Run() {

  SyncThirdPersonRequest();

  HitManagerUpdate();

  SpectatorListUpdate();

  BombTimerUpdate();

  BulletTracerUpdate();
  HitParticleUpdate();
  HitLogUpdate();
  DeathSoundUpdate();
  AntiFlash();
  WorldManipulationUpdate();
  SmokeColorChanger();
  // Bhop / auto-strafe / long-jump are now handled in Movement::Run(cmd)
  // from the CreateMove hook. The old force-jump memory-write approach
  // here is left as dead code (functions still exist for backward compat
  // but are not called from the main loop).
  ChatSpamUpdate();
  NameChangerUpdate();
}

void Misc::NameChangerUpdate() {
  if (!Globals::misc_name_changer) return;

  if (Interfaces::m_pEngine) {
    static auto last_setinfo_time = std::chrono::steady_clock::now();
    const auto now_time = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now_time - last_setinfo_time).count() > 400) {
      Interfaces::m_pEngine->ExecuteClientCMD("setinfo name x");
      last_setinfo_time = now_time;
    }
  }
}

void Misc::run_bunnyhop(c_user_cmd *cmd) {
  if (!Globals::bunnyhop_enabled)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  const uint8_t moveType =
      Utils::SafeRead<uint8_t>(localPawnAddr + Offsets::m_MoveType);
  if (moveType == MOVETYPE_NOCLIP || moveType == MOVETYPE_LADDER)
    return;

  const uint32_t flags =
      Utils::SafeRead<uint32_t>(localPawnAddr + Offsets::m_fFlags);
  const bool onGround = (flags & 1U) != 0;

  constexpr uintptr_t dwForceJump = 0x2094490;

  const bool spaceHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
  if (!spaceHeld)
    return;

  // Speed cap check (0 = no cap)
  if (Globals::bunnyhop_speed_cap > 1.0f) {
    Vector vel = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
    float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);
    if (speed2d > Globals::bunnyhop_speed_cap) {
      // suppress jump to bleed speed naturally
      Utils::SafeWrite<int>(client + dwForceJump, 256);
      return;
    }
  }

  if (onGround) {
    Utils::SafeWrite<int>(client + dwForceJump, 65537); // +jump
  } else {
    Utils::SafeWrite<int>(client + dwForceJump, 256);   // -jump
  }
}

void Misc::run_auto_strafe(c_user_cmd *cmd) {
  if (!Globals::bunnyhop_enabled || !cmd)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  const uint8_t moveType =
      Utils::SafeRead<uint8_t>(localPawnAddr + Offsets::m_MoveType);
  if (moveType == MOVETYPE_NOCLIP || moveType == MOVETYPE_LADDER)
    return;

  const uint32_t flags =
      Utils::SafeRead<uint32_t>(localPawnAddr + Offsets::m_fFlags);
  const bool onGround = (flags & 1U) != 0;
  if (onGround)
    return;

  __try {
    struct FakePBBase {
      char pad[0x50];
      float flForwardMove;
      float flSideMove;
      float flUpMove;
    };
    
    uintptr_t baseCmdAddr = *reinterpret_cast<uintptr_t *>(
        reinterpret_cast<uintptr_t>(cmd) + 0x18 + 0x28);
    if (!baseCmdAddr)
      return;

    auto *baseCmd = reinterpret_cast<FakePBBase *>(baseCmdAddr);

    // Get current velocity vectors
    Vector velocity = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
    float speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);

    // Determine target strafe direction based on mouse movement/wasd input keys
    const bool keyA = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool keyD = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool keyW = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool keyS = (GetAsyncKeyState('S') & 0x8000) != 0;

    // View angles for strafe alignment
    const QAngle viewAngles =
        Utils::SafeRead<QAngle>(client + Offsets::dwViewAngles);

    float yaw = viewAngles.yaw;

    // Auto strafer core logic:
    // Determine the optimal delta angle to maximize acceleration
    float velocityDirection = atan2f(velocity.y, velocity.x) * (180.0f / 3.14159265f);
    float velocityYawDelta = remainderf(velocityDirection - yaw, 360.0f);

    float strafeAngle = 0.0f;
    if (speed > 5.0f) {
      float perfectDelta = std::clamp(RAD2DEG(asinf(std::clamp(30.0f / speed, -0.99f, 0.99f))), 0.0f, 90.0f);
      if (keyA) {
        strafeAngle = perfectDelta;
      } else if (keyD) {
        strafeAngle = -perfectDelta;
      } else if (keyW) {
        strafeAngle = perfectDelta * 0.5f;
      } else if (keyS) {
        strafeAngle = -perfectDelta * 0.5f;
      } else {
        // No keys pressed: dynamic air-strafe based on velocity direction
        if (velocityYawDelta > 0.0f) {
          strafeAngle = perfectDelta;
        } else {
          strafeAngle = -perfectDelta;
        }
      }
    } else {
      // Starting from standstill, alternate left/right to gain initial speed
      static bool flip = false;
      flip = !flip;
      strafeAngle = flip ? 45.0f : -45.0f;
    }

    baseCmd->flForwardMove = 0.0f;
    baseCmd->flSideMove = (strafeAngle > 0.0f ? -1.0f : 1.0f) * 450.0f;

    // Correct movement command according to modern subtick input delta adjustments
    float wishYaw = remainderf(yaw + strafeAngle, 360.0f);
    float rad = DEG2RAD(yaw - wishYaw);
    
    float originalSideMove = baseCmd->flSideMove;
    baseCmd->flSideMove = originalSideMove * cosf(rad);
    baseCmd->flForwardMove = originalSideMove * sinf(rad);

  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
}

// ======================= LONG JUMP =======================

namespace {
  static bool  s_lj_active       = false;
  static bool  s_lj_was_on_ground = true;
  static float s_lj_peak_speed   = 0.0f;
  static float s_lj_current_speed = 0.0f;
  static auto  s_lj_jump_time    = std::chrono::steady_clock::now();
  static int   s_lj_tick_counter = 0;
  static bool  s_lj_boosted_this_jump = false;
}

void Misc::run_longjump() {
  if (!Globals::longjump_enabled)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  const uint8_t moveType =
      Utils::SafeRead<uint8_t>(localPawnAddr + Offsets::m_MoveType);
  if (moveType == MOVETYPE_NOCLIP || moveType == MOVETYPE_LADDER)
    return;

  const uint32_t flags =
      Utils::SafeRead<uint32_t>(localPawnAddr + Offsets::m_fFlags);
  const bool onGround = (flags & 1U) != 0;

  constexpr uintptr_t dwForceJump = 0x2094490;

  // Read current velocity
  Vector vel = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
  float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);
  s_lj_current_speed = speed2d;

  // Track peak speed during jump
  if (!onGround && speed2d > s_lj_peak_speed)
    s_lj_peak_speed = speed2d;

  // Check if longjump key is held
  const bool ljKeyHeld = (GetAsyncKeyState(Globals::longjump_key) & 0x8000) != 0;
  const bool spaceHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

  // Detect ground→air transition (jump moment)
  if (s_lj_was_on_ground && !onGround && (ljKeyHeld || (spaceHeld && ljKeyHeld))) {
    s_lj_active = true;
    s_lj_jump_time = std::chrono::steady_clock::now();
    s_lj_peak_speed = speed2d;
    s_lj_tick_counter = 0;
    s_lj_boosted_this_jump = false;
  }

  // When player lands, deactivate
  if (!s_lj_was_on_ground && onGround) {
    s_lj_active = false;
    s_lj_boosted_this_jump = false;
  }

  s_lj_was_on_ground = onGround;

  if (!s_lj_active || !ljKeyHeld)
    return;

  s_lj_tick_counter++;

  // === VELOCITY BOOST on jump ===
  if (!s_lj_boosted_this_jump && s_lj_tick_counter <= 3) {
    float boostMultiplier = Globals::longjump_boost;

    // Mode adjustments
    switch (Globals::longjump_mode) {
    case 0: // Velocity Boost - clean single boost
      break;
    case 1: // Turbo - stronger boost
      boostMultiplier *= 1.5f;
      break;
    case 2: // Mega - maximum boost
      boostMultiplier *= 2.5f;
      break;
    }

    // Calculate boosted velocity: maintain direction, increase magnitude
    if (speed2d > 10.0f) {
      float dirX = vel.x / speed2d;
      float dirY = vel.y / speed2d;
      float newSpeed = speed2d * boostMultiplier;

      // Clamp to prevent insane values that crash the engine
      if (newSpeed > 3500.0f)
        newSpeed = 3500.0f;

      Vector newVel;
      newVel.x = dirX * newSpeed;
      newVel.y = dirY * newSpeed;
      newVel.z = vel.z + 50.0f; // slight upward kick for distance

      Utils::SafeWrite<Vector>(localPawnAddr + Offsets::m_vecVelocity, newVel);
      s_lj_boosted_this_jump = true;
    }
  }

  // === DUCK BOOST (crouch mid-air for more distance) ===
  if (Globals::longjump_duck_boost && s_lj_tick_counter > 5) {
    // Force crouch in air to reduce hitbox and gain glide
    constexpr uintptr_t dwForceCrouch = 0x2094520;
    Utils::SafeWrite<int>(client + dwForceCrouch, 65537); // +duck
  }

  // === AUTO-JUMP at landing for chained longjumps ===
  if (spaceHeld && onGround) {
    Utils::SafeWrite<int>(client + dwForceJump, 65537);
  }
}

void Misc::run_longjump_render() {
  if (!Globals::longjump_enabled || !Globals::longjump_hud)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  // Read velocity for live display
  Vector vel = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
  float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);

  const uint32_t flags =
      Utils::SafeRead<uint32_t>(localPawnAddr + Offsets::m_fFlags);
  const bool onGround = (flags & 1U) != 0;
  const bool ljKeyHeld = (GetAsyncKeyState(Globals::longjump_key) & 0x8000) != 0;

  // Only show HUD when longjump key is held or during active jump
  if (!ljKeyHeld && !s_lj_active)
    return;

  auto* drawList = ImGui::GetBackgroundDrawList();
  float cx = Globals::GameViewportX + Globals::ScreenWidth / 2.0f;
  float baseY = Globals::GameViewportY + Globals::ScreenHeight * 0.72f;

  // === SPEED BAR ===
  float barWidth = 300.0f;
  float barHeight = 8.0f;
  float barX = cx - barWidth / 2.0f;
  float barY = baseY;

  // Background
  drawList->AddRectFilled(
      ImVec2(barX - 2, barY - 2),
      ImVec2(barX + barWidth + 2, barY + barHeight + 2),
      IM_COL32(0, 0, 0, 180), 4.0f);

  // Speed fill (normalized to 0-800 units max for visual)
  float speedNorm = (std::min)(speed2d / 800.0f, 1.0f);

  // Color gradient: green -> yellow -> red based on speed
  uint8_t r, g, b;
  if (speedNorm < 0.5f) {
    float t = speedNorm * 2.0f;
    r = (uint8_t)(t * 255);
    g = 255;
    b = 0;
  } else {
    float t = (speedNorm - 0.5f) * 2.0f;
    r = 255;
    g = (uint8_t)((1.0f - t) * 255);
    b = 0;
  }

  drawList->AddRectFilled(
      ImVec2(barX, barY),
      ImVec2(barX + barWidth * speedNorm, barY + barHeight),
      IM_COL32(r, g, b, 220), 3.0f);

  // Border
  drawList->AddRect(
      ImVec2(barX - 1, barY - 1),
      ImVec2(barX + barWidth + 1, barY + barHeight + 1),
      IM_COL32(255, 255, 255, 80), 4.0f);

  // === SPEED TEXT ===
  char speedText[64];
  snprintf(speedText, sizeof(speedText), "%.0f u/s", speed2d);
  ImVec2 textSize = ImGui::CalcTextSize(speedText);
  float textX = cx - textSize.x / 2.0f;
  float textY = barY - textSize.y - 4.0f;

  // Shadow
  drawList->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0, 0, 0, 200), speedText);
  // Main text
  drawList->AddText(ImVec2(textX, textY), IM_COL32(r, g, b, 255), speedText);

  // === PEAK SPEED (during jump) ===
  if (s_lj_active || (!onGround && s_lj_peak_speed > 0.0f)) {
    char peakText[64];
    snprintf(peakText, sizeof(peakText), "PEAK: %.0f u/s", s_lj_peak_speed);
    ImVec2 peakSize = ImGui::CalcTextSize(peakText);
    float peakX = cx - peakSize.x / 2.0f;
    float peakY = barY + barHeight + 6.0f;

    drawList->AddText(ImVec2(peakX + 1, peakY + 1), IM_COL32(0, 0, 0, 200), peakText);
    drawList->AddText(ImVec2(peakX, peakY), IM_COL32(255, 200, 50, 255), peakText);
  }

  // === MODE INDICATOR ===
  const char* modeNames[] = { "VELOCITY", "TURBO", "MEGA" };
  int modeIdx = (std::clamp)(Globals::longjump_mode, 0, 2);
  char modeText[32];
  snprintf(modeText, sizeof(modeText), "LJ: %s", modeNames[modeIdx]);
  ImVec2 modeSize = ImGui::CalcTextSize(modeText);
  float modeX = cx - modeSize.x / 2.0f;
  float modeY = textY - modeSize.y - 2.0f;

  ImU32 modeColor = s_lj_active ? IM_COL32(50, 255, 50, 255) : IM_COL32(200, 200, 200, 180);
  drawList->AddText(ImVec2(modeX + 1, modeY + 1), IM_COL32(0, 0, 0, 200), modeText);
  drawList->AddText(ImVec2(modeX, modeY), modeColor, modeText);
}

// ======================= MOVEMENT KEYS DISPLAY =======================

#include "../../menu/Menu.h"

namespace {
  static bool s_mk_dragging = false;
  static float s_mk_drag_ox = 0, s_mk_drag_oy = 0;

  static bool s_vg_dragging = false;
  static float s_vg_drag_ox = 0, s_vg_drag_oy = 0;
  static std::vector<float> s_vg_history;

  // Generic drag helper: returns true if widget is being dragged
  static bool HandleWidgetDrag(float& posX, float& posY, float w, float h,
                               bool& dragging, float& dragOX, float& dragOY) {
    if (!Menu::IsOpen)
      return false;

    ImVec2 mousePos = ImGui::GetMousePos();
    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (mouseDown && !dragging) {
      // Check if click is inside widget
      if (mousePos.x >= posX && mousePos.x <= posX + w &&
          mousePos.y >= posY && mousePos.y <= posY + h) {
        dragging = true;
        dragOX = mousePos.x - posX;
        dragOY = mousePos.y - posY;
      }
    }

    if (!mouseDown) {
      dragging = false;
    }

    if (dragging) {
      posX = mousePos.x - dragOX;
      posY = mousePos.y - dragOY;
      return true;
    }
    return false;
  }
}

void Misc::MovementKeysRender() {
  if (!Globals::movkeys_enabled)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  auto* drawList = ImGui::GetBackgroundDrawList();
  const float alpha = std::clamp(Globals::movkeys_opacity * HudOpacity(),
                                 0.05f, 1.0f);
  const uint8_t a = (uint8_t)(alpha * 255.0f);
  const uint8_t aHalf = (uint8_t)(alpha * 128.0f);
  const uint8_t aDim = (uint8_t)(alpha * 60.0f);

  const float* ac = Globals::movkeys_accent_color;
  uint8_t acR = (uint8_t)(ac[0] * 255), acG = (uint8_t)(ac[1] * 255), acB = (uint8_t)(ac[2] * 255);

  // Key states
  const bool kW = (GetAsyncKeyState('W') & 0x8000) != 0;
  const bool kA = (GetAsyncKeyState('A') & 0x8000) != 0;
  const bool kS = (GetAsyncKeyState('S') & 0x8000) != 0;
  const bool kD = (GetAsyncKeyState('D') & 0x8000) != 0;
  const bool kSpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
  const bool kShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool kCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

  // Widget dimensions
  float keySize = 32.0f;
  float gap = 4.0f;
  float totalW = keySize * 3 + gap * 2;
  float panelPad = 10.0f;
  float panelW = totalW + panelPad * 2;
  float panelH = keySize * 2 + gap + panelPad * 2 + keySize * 0.65f + gap + 8.0f;
  if (Globals::movkeys_show_velocity)
    panelH += 20.0f;

  // Default position: bottom center
  if (Globals::movkeys_pos_x < 0)
    Globals::movkeys_pos_x = Globals::GameViewportX + Globals::ScreenWidth / 2.0f - panelW / 2.0f;
  if (Globals::movkeys_pos_y < 0)
    Globals::movkeys_pos_y = Globals::GameViewportY + Globals::ScreenHeight * 0.82f;

  // Handle dragging
  HandleWidgetDrag(Globals::movkeys_pos_x, Globals::movkeys_pos_y,
                   panelW, panelH, s_mk_dragging, s_mk_drag_ox, s_mk_drag_oy);

  float panelX = Globals::movkeys_pos_x;
  float panelY = Globals::movkeys_pos_y;
  float startX = panelX + panelPad;
  float startY = panelY + panelPad;

  int style = Globals::movkeys_style;

  // Draw drag indicator when menu is open
  if (Menu::IsOpen) {
    drawList->AddRect(
        ImVec2(panelX - 1, panelY - 1),
        ImVec2(panelX + panelW + 1, panelY + panelH + 1),
        IM_COL32(acR, acG, acB, 120), 8.0f, 0, 1.5f);
    // Small drag icon
    const char* dragLabel = s_mk_dragging ? "Moving..." : "Drag";
    ImVec2 dlSize = ImGui::CalcTextSize(dragLabel);
    drawList->AddText(ImVec2(panelX + panelW / 2 - dlSize.x / 2, panelY - dlSize.y - 2),
                      IM_COL32(acR, acG, acB, 200), dragLabel);
  }

  if (style == 0) {
    // ========== MODERN MECHANICAL-KEYCAP STYLE ==========
    //
    // Each key is rendered as a layered "keycap":
    //   - 1px outer shadow (gives separation from the background)
    //   - dark body fill with a subtle top-to-bottom shade
    //   - 1px thin border
    //   - inner top highlight (1px brighter line) → keycap look
    //   - centered label, white when pressed / dim when idle
    //
    // Press state smoothly fades in/out via a per-key animation
    // value so the highlight breathes instead of snapping on/off.

    // Background panel — semi-transparent shell with soft drop shadow.
    drawList->AddRectFilled(
        ImVec2(panelX - 1, panelY + 2),
        ImVec2(panelX + panelW + 1, panelY + panelH + 3),
        IM_COL32(0, 0, 0, (uint8_t)(alpha * 60)), 9.0f);
    drawList->AddRectFilled(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(12, 13, 16, (uint8_t)(alpha * 215)), 8.0f);
    drawList->AddRect(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(48, 50, 58, (uint8_t)(alpha * 220)), 8.0f, 0, 1.0f);

    // Per-key animation cache (held-time → 0..1 highlight). We key by
    // address so each call-site has its own slot without bookkeeping.
    auto AnimFor = [&](const void* slot, bool pressed) -> float {
      static std::unordered_map<const void*, float> s_anim;
      float& v = s_anim[slot];
      float target = pressed ? 1.0f : 0.0f;
      float dt = ImGui::GetIO().DeltaTime;
      v += (target - v) * std::clamp(dt * 18.f, 0.f, 1.f);
      if (fabsf(v - target) < 0.003f) v = target;
      return v;
    };

    auto DrawKey = [&](float x, float y, float w, float h,
                       const char* label, bool pressed, const void* animSlot) {
      float p = AnimFor(animSlot, pressed);

      // Lerp body color: dark neutral → crimson tint.
      auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
      uint8_t br = (uint8_t)lerp(28.f, (float)acR * 0.85f, p);
      uint8_t bg = (uint8_t)lerp(30.f, (float)acG * 0.85f, p);
      uint8_t bb = (uint8_t)lerp(36.f, (float)acB * 0.85f, p);

      // 1) outer drop shadow (1 px below the keycap)
      drawList->AddRectFilled(
          ImVec2(x, y + 1.5f), ImVec2(x + w, y + h + 1.5f),
          IM_COL32(0, 0, 0, (uint8_t)(alpha * 90)), 5.0f);

      // 2) body
      drawList->AddRectFilled(
          ImVec2(x, y), ImVec2(x + w, y + h),
          IM_COL32(br, bg, bb, (uint8_t)(alpha * (180.f + 60.f * p))),
          5.0f);

      // 3) top highlight (subtle 1-px brighter line just inside the
      //    top edge) — gives the keycap a "lit-from-above" feel.
      drawList->AddLine(
          ImVec2(x + 4.f, y + 1.5f),
          ImVec2(x + w - 4.f, y + 1.5f),
          IM_COL32(255, 255, 255, (uint8_t)(alpha * (45.f + 30.f * p))),
          1.0f);

      // 4) border
      uint8_t boR = (uint8_t)lerp(62.f, (float)acR, p);
      uint8_t boG = (uint8_t)lerp(64.f, (float)acG, p);
      uint8_t boB = (uint8_t)lerp(72.f, (float)acB, p);
      drawList->AddRect(
          ImVec2(x, y), ImVec2(x + w, y + h),
          IM_COL32(boR, boG, boB, (uint8_t)(alpha * (180.f + 60.f * p))),
          5.0f, 0, 1.0f);

      // 5) outer red glow when pressed
      if (p > 0.01f) {
        drawList->AddRect(
            ImVec2(x - 1.f, y - 1.f), ImVec2(x + w + 1.f, y + h + 1.f),
            IM_COL32(acR, acG, acB, (uint8_t)(alpha * 90.f * p)),
            6.0f, 0, 1.5f);
      }

      // 6) label — sharp white when pressed, dim gray when idle
      ImFont* lf = widget_font_big ? widget_font_big : ImGui::GetFont();
      float ls = 14.f;
      ImVec2 ts = lf->CalcTextSizeA(ls, FLT_MAX, 0.f, label);
      uint8_t tr = (uint8_t)lerp(150.f, 255.f, p);
      uint8_t tg = (uint8_t)lerp(152.f, 255.f, p);
      uint8_t tb = (uint8_t)lerp(160.f, 255.f, p);
      // 1-px text shadow for legibility on top of busy game scenes
      drawList->AddText(lf, ls,
          ImVec2(x + (w - ts.x) / 2.0f + 1.f,
                 y + (h - ts.y) / 2.0f + 1.f),
          IM_COL32(0, 0, 0, (uint8_t)(alpha * 160)),
          label);
      drawList->AddText(lf, ls,
          ImVec2(x + (w - ts.x) / 2.0f,
                 y + (h - ts.y) / 2.0f),
          IM_COL32(tr, tg, tb, a),
          label);
    };

    // Static slot pointers used as keys for the per-key animation map.
    static int slotW, slotA, slotS, slotD, slotSpace;
    // Row 1: W (centered above A-S-D)
    DrawKey(startX + keySize + gap, startY, keySize, keySize, "W", kW, &slotW);
    // Row 2: A S D
    float row2Y = startY + keySize + gap;
    DrawKey(startX,                         row2Y, keySize, keySize, "A", kA, &slotA);
    DrawKey(startX + keySize + gap,         row2Y, keySize, keySize, "S", kS, &slotS);
    DrawKey(startX + (keySize + gap) * 2,   row2Y, keySize, keySize, "D", kD, &slotD);
    // Row 3: SPACE — full-width bar, shorter height
    float row3Y = row2Y + keySize + gap + 4.0f;
    DrawKey(startX, row3Y, totalW, keySize * 0.65f, "SPACE", kSpace, &slotSpace);

  } else if (style == 1) {
    // ========== CLASSIC STYLE ==========
    drawList->AddRectFilled(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(10, 10, 15, aHalf), 6.0f);

    auto DrawClassicKey = [&](float x, float y, const char* label, bool pressed) {
      ImU32 col = pressed ? IM_COL32(acR, acG, acB, a) : IM_COL32(100, 100, 100, aDim);
      drawList->AddText(ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, aHalf), label);
      drawList->AddText(ImVec2(x, y), col, label);
    };

    float spacing = 16.0f;
    DrawClassicKey(startX + spacing + 4, startY, "W", kW);
    DrawClassicKey(startX, startY + spacing, "A", kA);
    DrawClassicKey(startX + spacing + 4, startY + spacing, "S", kS);
    DrawClassicKey(startX + (spacing + 4) * 2, startY + spacing, "D", kD);
    DrawClassicKey(startX, startY + spacing * 2 + 4, kShift ? "SHIFT" : "shift", kShift);
    DrawClassicKey(startX + 50, startY + spacing * 2 + 4, kSpace ? "JUMP" : "jump", kSpace);
    DrawClassicKey(startX + 90, startY + spacing * 2 + 4, kCtrl ? "DUCK" : "duck", kCtrl);

  } else {
    // ========== MINIMAL STYLE ==========
    drawList->AddRectFilled(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(10, 10, 15, aDim), 4.0f);

    float dotSize = 10.0f;
    float dotGap = 6.0f;
    struct KeyDot { bool pressed; };
    KeyDot keys[] = { {kW}, {kA}, {kS}, {kD}, {kSpace}, {kShift}, {kCtrl} };
    int count = 7;
    float dotsW = count * dotSize + (count - 1) * dotGap;
    float sx = panelX + (panelW - dotsW) / 2.0f;
    float sy = panelY + (panelH - dotSize) / 2.0f;

    for (int i = 0; i < count; i++) {
      float x = sx + i * (dotSize + dotGap);
      ImU32 col = keys[i].pressed ? IM_COL32(acR, acG, acB, a) : IM_COL32(50, 50, 60, aHalf);
      drawList->AddRectFilled(ImVec2(x, sy), ImVec2(x + dotSize, sy + dotSize), col, 2.0f);
    }
  }

  // Velocity text
  if (Globals::movkeys_show_velocity) {
    Vector vel = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
    float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);

    char velText[32];
    snprintf(velText, sizeof(velText), "%.0f u/s", speed2d);
    ImVec2 ts = ImGui::CalcTextSize(velText);
    float vx = panelX + panelW / 2.0f - ts.x / 2.0f;
    float vy = panelY + panelH - 18.0f;

    drawList->AddText(ImVec2(vx + 1, vy + 1), IM_COL32(0, 0, 0, aHalf), velText);

    float norm = (std::min)(speed2d / 300.0f, 1.0f);
    uint8_t sr = norm < 0.5f ? (uint8_t)(norm * 2.0f * 255) : 255;
    uint8_t sg = norm < 0.5f ? 255 : (uint8_t)((1.0f - (norm - 0.5f) * 2.0f) * 255);
    drawList->AddText(ImVec2(vx, vy), IM_COL32(sr, sg, 0, a), velText);
  }
}

// ======================= VELOCITY GRAPH =======================

void Misc::VelocityGraphRender() {
  if (!Globals::velgraph_enabled)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  uintptr_t localPawnAddr =
      Memory::Globals::LocalPawn();
  if (!localPawnAddr)
    return;

  auto *localPawn = reinterpret_cast<C_CSPlayerPawn *>(localPawnAddr);
  if (!localPawn->IsAlive())
    return;

  // Read velocity
  Vector vel = Utils::SafeRead<Vector>(localPawnAddr + Offsets::m_vecVelocity);
  float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);

  // Update history
  int maxHistory = (std::clamp)(Globals::velgraph_history, 30, 600);
  s_vg_history.push_back(speed2d);
  while ((int)s_vg_history.size() > maxHistory)
    s_vg_history.erase(s_vg_history.begin());

  auto* drawList = ImGui::GetBackgroundDrawList();
  float alpha = std::clamp(Globals::velgraph_opacity * HudOpacity(),
                           0.05f, 1.0f);
  uint8_t a = (uint8_t)(alpha * 255);

  float gw = Globals::velgraph_width;
  float gh = Globals::velgraph_height;
  float totalH = gh + 30.0f; // extra for text

  // Default position
  if (Globals::velgraph_pos_x < 0)
    Globals::velgraph_pos_x = Globals::GameViewportX + Globals::ScreenWidth / 2.0f - gw / 2.0f;
  if (Globals::velgraph_pos_y < 0)
    Globals::velgraph_pos_y = Globals::GameViewportY + Globals::ScreenHeight * 0.60f;

  // Handle drag
  HandleWidgetDrag(Globals::velgraph_pos_x, Globals::velgraph_pos_y,
                   gw, totalH, s_vg_dragging, s_vg_drag_ox, s_vg_drag_oy);

  float gx = Globals::velgraph_pos_x;
  float gy = Globals::velgraph_pos_y;

  // Drag indicator when menu open
  if (Menu::IsOpen) {
    const float* ac = Globals::velgraph_color;
    uint8_t acR = (uint8_t)(ac[0] * 255), acG = (uint8_t)(ac[1] * 255), acB = (uint8_t)(ac[2] * 255);
    drawList->AddRect(
        ImVec2(gx - 1, gy - 1),
        ImVec2(gx + gw + 1, gy + totalH + 1),
        IM_COL32(acR, acG, acB, 120), 6.0f, 0, 1.5f);
    const char* dragLabel = s_vg_dragging ? "Moving..." : "Drag";
    ImVec2 dlSize = ImGui::CalcTextSize(dragLabel);
    drawList->AddText(ImVec2(gx + gw / 2 - dlSize.x / 2, gy - dlSize.y - 2),
                      IM_COL32(acR, acG, acB, 200), dragLabel);
  }

  // Background
  const float* bgc = Globals::velgraph_bg_color;
  drawList->AddRectFilled(
      ImVec2(gx, gy), ImVec2(gx + gw, gy + totalH),
      IM_COL32((uint8_t)(bgc[0]*255), (uint8_t)(bgc[1]*255), (uint8_t)(bgc[2]*255), (uint8_t)(bgc[3]*255)),
      6.0f);
  drawList->AddRect(
      ImVec2(gx, gy), ImVec2(gx + gw, gy + totalH),
      IM_COL32(255, 255, 255, (uint8_t)(alpha * 40)), 6.0f);

  // Title
  const char* title = "VELOCITY";
  ImVec2 titleSize = ImGui::CalcTextSize(title);
  drawList->AddText(ImVec2(gx + 6, gy + 3), IM_COL32(200, 200, 200, a), title);

  // Current speed value top-right
  char speedStr[32];
  snprintf(speedStr, sizeof(speedStr), "%.0f", speed2d);
  ImVec2 speedSize = ImGui::CalcTextSize(speedStr);
  const float* lc = Globals::velgraph_color;
  uint8_t lcR = (uint8_t)(lc[0]*255), lcG = (uint8_t)(lc[1]*255), lcB = (uint8_t)(lc[2]*255);
  drawList->AddText(ImVec2(gx + gw - speedSize.x - 6, gy + 3), IM_COL32(lcR, lcG, lcB, a), speedStr);

  // Graph area
  float graphTop = gy + 20.0f;
  float graphBottom = gy + 20.0f + gh;
  float graphLeft = gx + 4.0f;
  float graphRight = gx + gw - 4.0f;
  float graphW = graphRight - graphLeft;

  // Find max for scaling
  float maxSpeed = 300.0f; // minimum scale
  float peakSpeed = 0.0f;
  for (float s : s_vg_history) {
    if (s > maxSpeed) maxSpeed = s;
    if (s > peakSpeed) peakSpeed = s;
  }
  maxSpeed *= 1.15f; // some headroom

  // Grid lines
  int gridLines = 3;
  for (int i = 1; i < gridLines; i++) {
    float t = (float)i / (float)gridLines;
    float lineY = graphBottom - t * (graphBottom - graphTop);
    drawList->AddLine(
        ImVec2(graphLeft, lineY), ImVec2(graphRight, lineY),
        IM_COL32(255, 255, 255, 15));
    char gridLabel[16];
    snprintf(gridLabel, sizeof(gridLabel), "%.0f", t * maxSpeed);
    drawList->AddText(ImVec2(graphLeft + 2, lineY - 10), IM_COL32(100, 100, 100, a), gridLabel);
  }

  // Draw graph line
  if (s_vg_history.size() >= 2) {
    int count = (int)s_vg_history.size();
    float stepX = graphW / (float)(maxHistory - 1);

    // Filled area under the curve
    for (int i = 1; i < count; i++) {
      float x1 = graphRight - (count - i) * stepX;
      float x2 = graphRight - (count - i - 1) * stepX;
      float y1 = graphBottom - (s_vg_history[i - 1] / maxSpeed) * (graphBottom - graphTop);
      float y2 = graphBottom - (s_vg_history[i] / maxSpeed) * (graphBottom - graphTop);

      y1 = (std::clamp)(y1, graphTop, graphBottom);
      y2 = (std::clamp)(y2, graphTop, graphBottom);

      if (x1 < graphLeft) x1 = graphLeft;
      if (x2 < graphLeft) continue;

      // Fill triangle
      drawList->AddTriangleFilled(
          ImVec2(x1, y1), ImVec2(x2, y2), ImVec2(x2, graphBottom),
          IM_COL32(lcR, lcG, lcB, (uint8_t)(alpha * 30)));
      drawList->AddTriangleFilled(
          ImVec2(x1, y1), ImVec2(x2, graphBottom), ImVec2(x1, graphBottom),
          IM_COL32(lcR, lcG, lcB, (uint8_t)(alpha * 30)));

      // Line
      drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
                        IM_COL32(lcR, lcG, lcB, a), 1.5f);
    }
  }

  // Peak indicator
  if (Globals::velgraph_show_peak && peakSpeed > 10.0f) {
    char peakStr[32];
    snprintf(peakStr, sizeof(peakStr), "PEAK: %.0f", peakSpeed);
    ImVec2 peakSize = ImGui::CalcTextSize(peakStr);
    drawList->AddText(
        ImVec2(gx + gw / 2 - peakSize.x / 2, graphBottom + 3),
        IM_COL32(255, 200, 50, a), peakStr);
  }
}

