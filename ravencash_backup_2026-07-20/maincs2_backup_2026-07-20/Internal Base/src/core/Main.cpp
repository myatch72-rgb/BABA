#include "../../src/feature/combat/legitbot/Aimbot.h"
#include "../../src/feature/combat/legitbot/RCS.h"
#include "../../src/feature/combat/legitbot/Triggerbot.h"
#include "../../src/feature/inventory/InventoryChanger.h"
#include "../../src/feature/skinchanger/SkinChanger.h"
#include "../../src/feature/visuals/grenadepredict/GrenadePrediction.h"
#include "../../src/sdk/entity/EntityManager.h"
#include "../../src/sdk/interfaces/Interfaces.h"
#include "../../src/sdk/memory/Convars.h"
#include "../../src/sdk/memory/Globals.h"
#include "../../src/sdk/memory/OffsetResolver.h"
#include "../../src/feature/misc/CustomModel.h"
#include "CrashTelemetry.h"
#include "Hooks.h"
#include "ResourceExtractor.h"
#include "StabilityProfile.h"

#include <Windows.h>
#include <exception>

static HANDLE g_MainThread = nullptr;

static void LoadDeferredInventoryCatalogue(void* module) {
  try {
    SkinChanger::SetupDatabase(module);
    CrashTelemetry::Trace("deferred SkinChanger::SetupDatabase end");
  } catch (const std::exception& error) {
    CrashTelemetry::Trace(
        "deferred SkinChanger::SetupDatabase caught std::exception: %s",
        error.what());
  } catch (...) {
    CrashTelemetry::Trace(
        "deferred SkinChanger::SetupDatabase caught unknown C++ exception");
  }
}

static bool PrepareDeferredSkinRuntime() {
  bool initialized = false;
  __try {
    const auto localAccess =
        Memory::Globals::InitializeLocalPlayerAccessors();
    const bool localPawnReady =
        Memory::Globals::pfnGetLocalPawn != nullptr;
    CrashTelemetry::Trace(
        "inventory local-player accessors resolved=%d/%d failed=%d "
        "pawnReady=%d pawnFn=%p controllerFn=%p",
        localAccess.resolved, localAccess.total, localAccess.failed,
        localPawnReady ? 1 : 0, Memory::Globals::pfnGetLocalPawn,
        Memory::Globals::pfnGetLocalController);

    CrashTelemetry::Trace("deferred SkinChanger::Setup begin");
    SkinChanger::Setup();
    initialized = localPawnReady && SkinChanger::g_Initialized;
    CrashTelemetry::Trace(
        "deferred SkinChanger::Setup end initialized=%d",
        initialized ? 1 : 0);
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "deferred SkinChanger::Setup")) {
    initialized = false;
  }
  return initialized;
}

static bool PrepareDeferredFullRuntime() {
  bool initialized = false;
  __try {
    CrashTelemetry::Mark(CrashTelemetry::Stage::OffsetResolution);
    CrashTelemetry::Trace("deferred OffsetResolver::Resolve begin");
    const auto offsets = OffsetResolver::Resolve();
    CrashTelemetry::Trace(
        "deferred OffsetResolver::Resolve end resolved=%d/%d failed=%d",
        offsets.resolved, offsets.total, offsets.failed);

    CrashTelemetry::Mark(CrashTelemetry::Stage::ConvarInitialization);
    CrashTelemetry::Trace("deferred Memory::Convars::Initialize begin");
    const bool convarsReady = Memory::Convars::Initialize();
    CrashTelemetry::Trace(
        "deferred Memory::Convars::Initialize end ready=%d",
        convarsReady ? 1 : 0);

    CrashTelemetry::Mark(
        CrashTelemetry::Stage::GrenadePredictionInitialization);
    CrashTelemetry::Trace("deferred GrenadePrediction::Initialize begin");
    GrenadePrediction::Initialize();
    CrashTelemetry::Trace("deferred GrenadePrediction::Initialize end");

    // Offset resolution is allowed to be partially fail-closed: individual
    // features already validate their own pointer/pattern dependencies. The
    // renderer and validated Inventory runtime are the activation prerequisites.
    initialized = Hooks::IsRenderReady() && SkinChanger::g_Initialized;
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "deferred full-runtime preparation")) {
    initialized = false;
  }
  return initialized;
}

DWORD WINAPI MainThread(LPVOID module) {
  // Real-time debug console
  AllocConsole();
  FILE *fDummy = nullptr;
  freopen_s(&fDummy, "CONOUT$", "w", stdout);
  freopen_s(&fDummy, "CONOUT$", "w", stderr);
  SetConsoleTitleA("RAVEN CS2 - CANLI HATA & LOG PANELI");

  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hConsole && hConsole != INVALID_HANDLE_VALUE) {
    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf("======================================================================\n");
    printf("        RAVEN CS2 - CANLI HATA & TELEMETRI LOG PANELI                \n");
    printf("======================================================================\n");
    printf("[+] Raven.dll basariyla cs2.exe icerisine enjekte edildi!\n");
    printf("[+] Tum olaylar ve hatalar anlik olarak bu pencereden izlenebilir.\n");
    printf("======================================================================\n\n");
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
  }

  CrashTelemetry::Mark(CrashTelemetry::Stage::WorkerEnter);
  CrashTelemetry::Trace("worker thread started module=%p", module);
  CrashTelemetry::RuntimeInitialize();

  bool startupCompleted = false;
  __try {
    CrashTelemetry::Mark(CrashTelemetry::Stage::ResourceExtraction);
    CrashTelemetry::Trace("ResourceExtractor::ExtractAll begin");
    ResourceExtractor::ExtractAll(static_cast<HMODULE>(module));
    CrashTelemetry::Trace("ResourceExtractor::ExtractAll end");

    if (StabilityProfile::EnableInventory) {
      CrashTelemetry::Mark(CrashTelemetry::Stage::SkinChangerSetup);
      CrashTelemetry::Trace("SkinChanger::PrepareDatabase begin");
      SkinChanger::PrepareDatabase();
      CrashTelemetry::Trace("SkinChanger::PrepareDatabase end");

      CrashTelemetry::Mark(CrashTelemetry::Stage::InventoryChangerSetup);
      CrashTelemetry::Trace("InventoryChanger::Setup begin");
      InventoryChanger::Setup();
      CustomModel::Scan();
      CustomWeapon::Scan();
      CrashTelemetry::Trace("InventoryChanger::Setup end");

      CrashTelemetry::Trace(
          "staged profile: catalogue and all runtime hooks deferred until "
          "after first Present");
    } else {
      CrashTelemetry::Trace(
          "menu-only recovery profile: offset/convar/grenade/skin/inventory "
          "startup disabled");
    }

    CrashTelemetry::Mark(CrashTelemetry::Stage::HooksSetup);
    CrashTelemetry::Trace("Hooks::Setup begin");
    Hooks::Setup();
    CrashTelemetry::Trace("Hooks::Setup end");
    startupCompleted = true;

    CrashTelemetry::Mark(CrashTelemetry::Stage::StartupComplete);
    CrashTelemetry::Trace("startup sequence completed; legacy UI notice "
                          "disabled");

    // LegacyGameUI001's vtable layout is not stable across Source 2 updates.
    // Calling the former vtable[28] notification method after the menu became
    // visible was the last recorded operation in the failing session. Keep
    // startup notifications inside the ImGui menu and never call this
    // unverified game interface from the worker thread.
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "MainThread startup sequence")) {
    CrashTelemetry::Trace("startup sequence aborted by SEH");
  }

  CrashTelemetry::Mark(CrashTelemetry::Stage::WorkerLoop);
  CrashTelemetry::Trace("worker loop entered startupCompleted=%d",
                        startupCompleted ? 1 : 0);
  unsigned long long workerPass = 0;
  bool stableHookProfileLogged = false;
  bool inventoryCataloguePending =
      startupCompleted && StabilityProfile::EnableInventory;
  bool inventoryRuntimePending = false;
  ULONGLONG inventoryRuntimeActivationTick = 0;
  bool fullRuntimePending = false;
  ULONGLONG fullRuntimeActivationTick = 0;
  while (!(GetAsyncKeyState(VK_END) & 1)) {
    if (startupCompleted && Hooks::IsRenderReady() &&
        !stableHookProfileLogged) {
      stableHookProfileLogged = true;
      // Present has proven sufficient for the menu. Inventory runtime setup is
      // still deferred until its catalogue/pattern preparation and a separate
      // render warm-up delay have completed.
      CrashTelemetry::Trace(
          "stable render profile active: inventory runtime still deferred");
    }

    // Do not run polling features while the real swap-chain and ImGui backend
    // are still being initialized. The previous 1 ms loop was active during
    // first Present and made a failure on either thread look like a render
    // crash.
    if (!startupCompleted || !Hooks::IsRenderReady()) {
      Sleep(10);
      continue;
    }

    // The inventory catalogue is deliberately loaded only after Present has
    // initialized the menu. A malformed/oversized catalogue must never prevent
    // the render hook from being installed and hiding the whole menu.
    if (inventoryCataloguePending) {
      inventoryCataloguePending = false;
      CrashTelemetry::Mark(CrashTelemetry::Stage::SkinChangerSetup);
      CrashTelemetry::Trace(
          "deferred SkinChanger::SetupDatabase begin (menu already ready)");
      LoadDeferredInventoryCatalogue(module);
      inventoryRuntimePending = PrepareDeferredSkinRuntime();
      inventoryRuntimeActivationTick = GetTickCount64() + 3000;
      CrashTelemetry::Trace(
          "inventory runtime activation scheduled ready=%d delayMs=3000",
          inventoryRuntimePending ? 1 : 0);
      CrashTelemetry::Mark(CrashTelemetry::Stage::WorkerLoop, false);
    }

    // Install only the validated loadout + FrameStage pair after the render
    // path and catalogue/runtime initialization have both been stable for
    // several seconds. SetModel remains unhooked; knife/agent model changes use
    // SkinChanger's guarded direct call from the game-thread callback.
    if (inventoryRuntimePending &&
        GetTickCount64() >= inventoryRuntimeActivationTick) {
      inventoryRuntimePending = false;
      const bool activated = Hooks::ActivateInventoryHooks();
      CrashTelemetry::Trace(
          "post-render inventory runtime activation result=%d",
          activated ? 1 : 0);

      if (activated && StabilityProfile::EnableFullRuntime) {
        const bool prepared = PrepareDeferredFullRuntime();
        fullRuntimePending = prepared;
        fullRuntimeActivationTick = GetTickCount64() + 2500;
        CrashTelemetry::Trace(
            "full runtime activation scheduled prepared=%d delayMs=2500",
            prepared ? 1 : 0);
        CrashTelemetry::Mark(CrashTelemetry::Stage::WorkerLoop, false);
      }
    }

    // Inventory remains the canary stage. Only after its live FrameStage and
    // loadout hooks have run without destabilizing the process do we install
    // the remaining command, visual and utility hooks in one queued batch.
    if (fullRuntimePending &&
        GetTickCount64() >= fullRuntimeActivationTick) {
      fullRuntimePending = false;
      const bool activated = Hooks::ActivateFullRuntimeHooks();
      CrashTelemetry::Trace(
          "post-render full runtime activation result=%d",
          activated ? 1 : 0);
    }

    // The worker remains lifecycle/telemetry only. Entity and command work is
    // owned by Present, FrameStage and CreateMove after staged activation.

    ++workerPass;
    CrashTelemetry::Mark(CrashTelemetry::Stage::WorkerLoop, false);
    if (workerPass == 1)
      CrashTelemetry::Trace("worker loop first pass completed");
    CrashTelemetry::Heartbeat("worker-loop");
    Sleep(10);
  }

  CrashTelemetry::Mark(CrashTelemetry::Stage::Shutdown);
  CrashTelemetry::Trace("VK_END observed; Hooks::Destroy begin");
  __try {
    Hooks::Destroy();
    CrashTelemetry::Trace("Hooks::Destroy end");
  } __except (CrashTelemetry::BoundaryFilter(
      GetExceptionInformation(), "Hooks::Destroy")) {
  }

  CrashTelemetry::Trace("worker thread exiting");
  ExitThread(0);
  return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    CrashTelemetry::EarlyInitialize(hModule);
    CrashTelemetry::Trace("DisableThreadLibraryCalls begin");
    DisableThreadLibraryCalls(hModule);
    CrashTelemetry::Trace("DisableThreadLibraryCalls end");

    DWORD workerThreadId = 0;
    g_MainThread =
        CreateThread(nullptr, 0, MainThread, hModule, 0, &workerThreadId);
    CrashTelemetry::Trace(
        "CreateThread result=%p workerTid=%lu error=%lu", g_MainThread,
        workerThreadId, g_MainThread ? ERROR_SUCCESS : GetLastError());
  } else if (reason == DLL_PROCESS_DETACH) {
    CrashTelemetry::Mark(CrashTelemetry::Stage::ProcessDetach, false);
    CrashTelemetry::Trace("DllMain PROCESS_DETACH reserved=%p", reserved);
  }

  return TRUE;
}
