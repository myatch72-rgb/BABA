#pragma once

#include <Windows.h>

namespace CrashTelemetry {

enum class Stage : LONG {
  Unknown = 0,
  DllAttach,
  WorkerEnter,
  RuntimeDiagnostics,
  ResourceExtraction,
  OffsetResolution,
  ConvarInitialization,
  GrenadePredictionInitialization,
  SkinChangerSetup,
  InventoryChangerSetup,
  HooksSetup,
  HooksMinHook,
  HooksFeatureInstall,
  HooksD3DBootstrap,
  HooksInterfaces,
  HooksScene,
  HooksGameplay,
  HooksFrameStage,
  HooksComplete,
  StartupComplete,
  WorkerLoop,
  WorkerEntityUpdate,
  WorkerAimbot,
  WorkerTriggerbot,
  WorkerRCS,
  PresentEnter,
  PresentDeviceInitialization,
  PresentNewFrame,
  PresentMenu,
  PresentRaycasting,
  PresentVisuals,
  PresentMisc,
  PresentAspectRatio,
  PresentImGuiRender,
  PresentOriginal,
  ResizeBuffers,
  Shutdown,
  ProcessDetach,
};

// Loader-lock compatible initialization. Uses only Win32 calls and installs
// the vectored/unhandled exception observers before the worker thread starts.
void EarlyInitialize(HMODULE selfModule);

// Called from the worker thread after DllMain. Records process/module state.
void RuntimeInitialize();

void Mark(Stage stage, bool emitLine = true);
void Trace(const char *format, ...);
void Heartbeat(const char *name, unsigned intervalMs = 10000);

// Intended for use in an __except expression. It records the complete
// exception context and returns EXCEPTION_EXECUTE_HANDLER.
int BoundaryFilter(EXCEPTION_POINTERS *exceptionInfo,
                   const char *boundaryName);

void Shutdown();
const wchar_t *GetLogPath();

} // namespace CrashTelemetry
