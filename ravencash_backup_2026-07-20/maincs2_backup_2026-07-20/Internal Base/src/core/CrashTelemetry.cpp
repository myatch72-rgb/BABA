#include "CrashTelemetry.h"

#include <DbgHelp.h>
#include <Psapi.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Psapi.lib")

namespace {

constexpr LONG kThreadSlotCount = 64;
constexpr LONG kMaximumLoggedExceptions = 32;
constexpr LONG kMaximumAutomaticDumps = 3;

struct ThreadStageSlot {
  volatile LONG threadId = 0;
  volatile LONG stage = static_cast<LONG>(CrashTelemetry::Stage::Unknown);
};

HMODULE g_selfModule = nullptr;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
PVOID g_vectoredHandler = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;
ThreadStageSlot g_threadStages[kThreadSlotCount]{};
wchar_t g_logDirectory[MAX_PATH]{};
wchar_t g_logPath[MAX_PATH]{};
ULONGLONG g_startTick = 0;
volatile LONG g_exceptionSequence = 0;
volatile LONG g_dumpSequence = 0;
volatile LONG g_exceptionWriterActive = 0;
volatile LONG64 g_lastHeartbeatTick = 0;

const char *StageName(CrashTelemetry::Stage stage) {
  using Stage = CrashTelemetry::Stage;
  switch (stage) {
  case Stage::DllAttach:
    return "dll-attach";
  case Stage::WorkerEnter:
    return "worker-enter";
  case Stage::RuntimeDiagnostics:
    return "runtime-diagnostics";
  case Stage::ResourceExtraction:
    return "resource-extraction";
  case Stage::OffsetResolution:
    return "offset-resolution";
  case Stage::ConvarInitialization:
    return "convar-initialization";
  case Stage::GrenadePredictionInitialization:
    return "grenade-prediction-initialization";
  case Stage::SkinChangerSetup:
    return "skinchanger-setup";
  case Stage::InventoryChangerSetup:
    return "inventorychanger-setup";
  case Stage::HooksSetup:
    return "hooks-setup";
  case Stage::HooksMinHook:
    return "hooks-minhook";
  case Stage::HooksFeatureInstall:
    return "hooks-feature-install";
  case Stage::HooksD3DBootstrap:
    return "hooks-d3d-bootstrap";
  case Stage::HooksInterfaces:
    return "hooks-interfaces";
  case Stage::HooksScene:
    return "hooks-scene";
  case Stage::HooksGameplay:
    return "hooks-gameplay";
  case Stage::HooksFrameStage:
    return "hooks-frame-stage";
  case Stage::HooksComplete:
    return "hooks-complete";
  case Stage::StartupComplete:
    return "startup-complete";
  case Stage::WorkerLoop:
    return "worker-loop";
  case Stage::WorkerEntityUpdate:
    return "worker-entity-update";
  case Stage::WorkerAimbot:
    return "worker-aimbot";
  case Stage::WorkerTriggerbot:
    return "worker-triggerbot";
  case Stage::WorkerRCS:
    return "worker-rcs";
  case Stage::PresentEnter:
    return "present-enter";
  case Stage::PresentDeviceInitialization:
    return "present-device-initialization";
  case Stage::PresentNewFrame:
    return "present-new-frame";
  case Stage::PresentMenu:
    return "present-menu";
  case Stage::PresentRaycasting:
    return "present-raycasting";
  case Stage::PresentVisuals:
    return "present-visuals";
  case Stage::PresentMisc:
    return "present-misc";
  case Stage::PresentAspectRatio:
    return "present-aspect-ratio";
  case Stage::PresentImGuiRender:
    return "present-imgui-render";
  case Stage::PresentOriginal:
    return "present-original";
  case Stage::ResizeBuffers:
    return "resize-buffers";
  case Stage::Shutdown:
    return "shutdown";
  case Stage::ProcessDetach:
    return "process-detach";
  default:
    return "unknown";
  }
}

ThreadStageSlot *FindThreadSlot(DWORD threadId, bool create) {
  for (LONG i = 0; i < kThreadSlotCount; ++i) {
    if (static_cast<DWORD>(
            InterlockedCompareExchange(&g_threadStages[i].threadId, 0, 0)) ==
        threadId)
      return &g_threadStages[i];
  }
  if (!create)
    return nullptr;

  for (LONG i = 0; i < kThreadSlotCount; ++i) {
    if (InterlockedCompareExchange(&g_threadStages[i].threadId,
                                   static_cast<LONG>(threadId), 0) == 0)
      return &g_threadStages[i];
  }
  return nullptr;
}

CrashTelemetry::Stage CurrentThreadStage(DWORD threadId) {
  ThreadStageSlot *slot = FindThreadSlot(threadId, false);
  if (!slot)
    return CrashTelemetry::Stage::Unknown;
  return static_cast<CrashTelemetry::Stage>(
      InterlockedCompareExchange(&slot->stage, 0, 0));
}

void WriteHandle(HANDLE file, const char *text, DWORD length, bool flush) {
  if (!file || file == INVALID_HANDLE_VALUE || !text || length == 0)
    return;
  DWORD written = 0;
  WriteFile(file, text, length, &written, nullptr);
  if (flush)
    FlushFileBuffers(file);
}

void AppendEmergency(const char *text, DWORD length) {
  if (!g_logPath[0] || !text || length == 0)
    return;
  HANDLE file =
      CreateFileW(g_logPath, FILE_APPEND_DATA,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  WriteHandle(file, text, length, true);
  CloseHandle(file);
}

void WriteRegular(const char *text, DWORD length) {
  AcquireSRWLockExclusive(&g_logLock);
  // Regular trace calls are used from Present and FrameStage. Forcing a disk
  // cache flush for every line stalls Source 2's render/game threads and can
  // trigger an external watchdog. Emergency exception records and shutdown
  // still flush synchronously.
  WriteHandle(g_logFile, text, length, false);
  ReleaseSRWLockExclusive(&g_logLock);
}

void FormatPrefix(char (&output)[256], const char *kind,
                  CrashTelemetry::Stage stage, DWORD threadId) {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  const unsigned long long elapsed =
      static_cast<unsigned long long>(GetTickCount64() - g_startTick);
  _snprintf_s(output, sizeof(output), _TRUNCATE,
              "[%04u-%02u-%02u %02u:%02u:%02u.%03u]"
              "[+%llums][pid=%lu][tid=%lu][%s][%s] ",
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
              time.wSecond, time.wMilliseconds, elapsed,
              GetCurrentProcessId(), threadId, kind ? kind : "trace",
              StageName(stage));
}

void TraceV(const char *kind, const char *format, va_list args,
            bool emergency) {
  if (!format)
    return;
  const DWORD threadId = GetCurrentThreadId();
  const auto stage = CurrentThreadStage(threadId);

  char prefix[256]{};
  char body[3072]{};
  char line[3584]{};
  FormatPrefix(prefix, kind, stage, threadId);
  _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
  _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s\r\n", prefix, body);
  const DWORD length = static_cast<DWORD>(strnlen_s(line, sizeof(line)));
  if (emergency)
    AppendEmergency(line, length);
  else
    WriteRegular(line, length);

  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hConsole && hConsole != INVALID_HANDLE_VALUE) {
    if (kind && (strcmp(kind, "exception") == 0 || strcmp(kind, "error") == 0)) {
      SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
    } else if (kind && strcmp(kind, "warn") == 0) {
      SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    } else {
      SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    }
    DWORD written = 0;
    WriteFile(hConsole, line, length, &written, nullptr);
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
  }
}

void EmergencyTrace(const char *format, ...) {
  va_list args;
  va_start(args, format);
  TraceV("exception", format, args, true);
  va_end(args);
}

void DescribeAddress(std::uintptr_t address, char (&moduleName)[MAX_PATH],
                     std::uintptr_t &moduleBase, DWORD &protect,
                     DWORD &type) {
  moduleName[0] = '\0';
  moduleBase = 0;
  protect = 0;
  type = 0;
  if (!address)
    return;

  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<const void *>(address), &memory,
                   sizeof(memory)) != sizeof(memory))
    return;
  moduleBase = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
  protect = memory.Protect;
  type = memory.Type;
  if (memory.Type == MEM_IMAGE && memory.AllocationBase)
    GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), moduleName,
                       MAX_PATH);
}

bool IsExecutableProtection(DWORD protection) {
  const DWORD base = protection & 0xFFu;
  return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
         base == PAGE_EXECUTE_READWRITE ||
         base == PAGE_EXECUTE_WRITECOPY;
}

void LogRegisters(const CONTEXT *context) {
  if (!context)
    return;
#ifdef _M_X64
  EmergencyTrace(
      "registers RIP=%016llX RSP=%016llX RBP=%016llX EFLAGS=%08lX",
      static_cast<unsigned long long>(context->Rip),
      static_cast<unsigned long long>(context->Rsp),
      static_cast<unsigned long long>(context->Rbp), context->EFlags);
  EmergencyTrace(
      "registers RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
      static_cast<unsigned long long>(context->Rax),
      static_cast<unsigned long long>(context->Rbx),
      static_cast<unsigned long long>(context->Rcx),
      static_cast<unsigned long long>(context->Rdx));
  EmergencyTrace(
      "registers RSI=%016llX RDI=%016llX R8=%016llX R9=%016llX",
      static_cast<unsigned long long>(context->Rsi),
      static_cast<unsigned long long>(context->Rdi),
      static_cast<unsigned long long>(context->R8),
      static_cast<unsigned long long>(context->R9));
  EmergencyTrace(
      "registers R10=%016llX R11=%016llX R12=%016llX R13=%016llX",
      static_cast<unsigned long long>(context->R10),
      static_cast<unsigned long long>(context->R11),
      static_cast<unsigned long long>(context->R12),
      static_cast<unsigned long long>(context->R13));
  EmergencyTrace("registers R14=%016llX R15=%016llX",
                 static_cast<unsigned long long>(context->R14),
                 static_cast<unsigned long long>(context->R15));
#else
  EmergencyTrace("registers EIP=%08lX ESP=%08lX EBP=%08lX EFLAGS=%08lX",
                 context->Eip, context->Esp, context->Ebp, context->EFlags);
#endif
}

void LogStackCandidates(const CONTEXT *context) {
  if (!context)
    return;
#ifdef _M_X64
  const std::uintptr_t stackPointer =
      static_cast<std::uintptr_t>(context->Rsp);
#else
  const std::uintptr_t stackPointer =
      static_cast<std::uintptr_t>(context->Esp);
#endif
  MEMORY_BASIC_INFORMATION stackMemory{};
  if (!stackPointer ||
      VirtualQuery(reinterpret_cast<const void *>(stackPointer), &stackMemory,
                   sizeof(stackMemory)) != sizeof(stackMemory) ||
      stackMemory.State != MEM_COMMIT ||
      (stackMemory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    EmergencyTrace("stack scan unavailable rsp=%p",
                   reinterpret_cast<void *>(stackPointer));
    return;
  }

  const std::uintptr_t regionEnd =
      reinterpret_cast<std::uintptr_t>(stackMemory.BaseAddress) +
      stackMemory.RegionSize;
  const std::size_t available =
      regionEnd > stackPointer ? (regionEnd - stackPointer) / sizeof(void *) : 0;
  const std::size_t count = available < 192 ? available : 192;
  const auto *values = reinterpret_cast<const std::uintptr_t *>(stackPointer);
  int emitted = 0;

  __try {
    for (std::size_t i = 0; i < count && emitted < 64; ++i) {
      const std::uintptr_t candidate = values[i];
      MEMORY_BASIC_INFORMATION codeMemory{};
      if (candidate < 0x10000 ||
          VirtualQuery(reinterpret_cast<const void *>(candidate), &codeMemory,
                       sizeof(codeMemory)) != sizeof(codeMemory) ||
          codeMemory.State != MEM_COMMIT || codeMemory.Type != MEM_IMAGE ||
          !IsExecutableProtection(codeMemory.Protect))
        continue;

      char moduleName[MAX_PATH]{};
      const auto moduleBase =
          reinterpret_cast<std::uintptr_t>(codeMemory.AllocationBase);
      GetModuleFileNameA(static_cast<HMODULE>(codeMemory.AllocationBase),
                         moduleName, MAX_PATH);
      EmergencyTrace("stack[%03zu] value=%016llX module=%s+0x%llX", i,
                     static_cast<unsigned long long>(candidate),
                     moduleName[0] ? moduleName : "<image>",
                     static_cast<unsigned long long>(candidate - moduleBase));
      ++emitted;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    EmergencyTrace("stack scan raised an exception after %d candidates",
                   emitted);
  }
  if (emitted == 0)
    EmergencyTrace("stack scan found no executable image candidates");
}

bool WriteMiniDump(EXCEPTION_POINTERS *exceptionInfo, LONG sequence,
                   const wchar_t *tag) {
  if (!exceptionInfo || !g_logDirectory[0])
    return false;

  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t path[MAX_PATH]{};
  _snwprintf_s(
      path, _countof(path), _TRUNCATE,
      L"%s\\raven_%s_%04u%02u%02u_%02u%02u%02u_pid%lu_%02ld.dmp",
      g_logDirectory, tag ? tag : L"exception", time.wYear, time.wMonth,
      time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId(),
      sequence);

  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    EmergencyTrace("minidump create failed error=%lu", GetLastError());
    return false;
  }

  MINIDUMP_EXCEPTION_INFORMATION information{};
  information.ThreadId = GetCurrentThreadId();
  information.ExceptionPointers = exceptionInfo;
  information.ClientPointers = FALSE;
  const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
      MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithHandleData |
      MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
      MiniDumpWithFullMemoryInfo | MiniDumpScanMemory |
      MiniDumpWithIndirectlyReferencedMemory);
  const BOOL result =
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                        &information, nullptr, nullptr);
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);

  char narrowPath[MAX_PATH]{};
  WideCharToMultiByte(CP_UTF8, 0, path, -1, narrowPath, MAX_PATH, nullptr,
                      nullptr);
  EmergencyTrace("minidump result=%d error=%lu path=%s", result ? 1 : 0,
                 error, narrowPath);
  return result == TRUE;
}

void RecordException(EXCEPTION_POINTERS *exceptionInfo, const char *origin,
                     bool forceDump) {
  if (!exceptionInfo || !exceptionInfo->ExceptionRecord)
    return;

  const LONG sequence = InterlockedIncrement(&g_exceptionSequence);
  if (sequence > kMaximumLoggedExceptions)
    return;

  EXCEPTION_RECORD *record = exceptionInfo->ExceptionRecord;
  char moduleName[MAX_PATH]{};
  std::uintptr_t moduleBase = 0;
  DWORD protect = 0;
  DWORD type = 0;
  DescribeAddress(
      reinterpret_cast<std::uintptr_t>(record->ExceptionAddress), moduleName,
      moduleBase, protect, type);

  EmergencyTrace(
      "========== EXCEPTION #%ld origin=%s code=0x%08lX flags=0x%08lX "
      "address=%p module=%s+0x%llX protect=0x%lX type=0x%lX ==========",
      sequence, origin ? origin : "<unknown>", record->ExceptionCode,
      record->ExceptionFlags, record->ExceptionAddress,
      moduleName[0] ? moduleName : "<unknown>",
      moduleBase ? static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(
                           record->ExceptionAddress) -
                       moduleBase)
                 : 0ull,
      protect, type);

  if (record->NumberParameters > 0) {
    char parameters[1024]{};
    std::size_t used = 0;
    for (DWORD i = 0; i < record->NumberParameters && i < 15; ++i) {
      const int written = _snprintf_s(
          parameters + used, sizeof(parameters) - used, _TRUNCATE,
          "%s[%lu]=0x%llX", used ? " " : "", i,
          static_cast<unsigned long long>(record->ExceptionInformation[i]));
      if (written <= 0)
        break;
      used += static_cast<std::size_t>(written);
    }
    EmergencyTrace("exception parameters %s", parameters);
  }

  LogRegisters(exceptionInfo->ContextRecord);
  LogStackCandidates(exceptionInfo->ContextRecord);

  LONG dumpSequence = InterlockedIncrement(&g_dumpSequence);
  if (forceDump)
    WriteMiniDump(exceptionInfo, dumpSequence, L"boundary");
  EmergencyTrace("================ END EXCEPTION #%ld ================",
                 sequence);
}

bool ShouldObserveException(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_STACK_OVERFLOW:
  case 0xC0000374: // STATUS_HEAP_CORRUPTION
  case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / fail-fast
    return true;
  default:
    return false;
  }
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *exceptionInfo) {
  if (!exceptionInfo || !exceptionInfo->ExceptionRecord ||
      !ShouldObserveException(
          exceptionInfo->ExceptionRecord->ExceptionCode))
    return EXCEPTION_CONTINUE_SEARCH;

  // ConVar discovery and resource probing deliberately probe candidate layouts/PE
  // directories behind local/API SEH guards. Those first-chance access violations
  // are recoverable; dumping full telemetry logs stalls injection and floods output.
  const auto stage = CurrentThreadStage(GetCurrentThreadId());
  if (stage == CrashTelemetry::Stage::ConvarInitialization ||
      stage == CrashTelemetry::Stage::ResourceExtraction)
    return EXCEPTION_CONTINUE_SEARCH;

  if (InterlockedCompareExchange(&g_exceptionWriterActive, 1, 0) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  __try {
    RecordException(exceptionInfo, "vectored-first-chance", false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static constexpr char failure[] =
        "[crash-telemetry] exception writer itself faulted\r\n";
    AppendEmergency(failure, static_cast<DWORD>(sizeof(failure) - 1));
  }
  InterlockedExchange(&g_exceptionWriterActive, 0);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledHandler(EXCEPTION_POINTERS *exceptionInfo) {
  if (InterlockedCompareExchange(&g_exceptionWriterActive, 1, 0) == 0) {
    __try {
      RecordException(exceptionInfo, "unhandled-filter", true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    InterlockedExchange(&g_exceptionWriterActive, 0);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void LogModule(const char *name) {
  HMODULE module = GetModuleHandleA(name);
  if (!module) {
    CrashTelemetry::Trace("module %-24s not-loaded", name);
    return;
  }

  MODULEINFO information{};
  GetModuleInformation(GetCurrentProcess(), module, &information,
                       sizeof(information));
  char path[MAX_PATH]{};
  GetModuleFileNameA(module, path, MAX_PATH);
  CrashTelemetry::Trace("module %-24s base=%p size=0x%lX path=%s", name,
                        module, information.SizeOfImage, path);
}

} // namespace

namespace CrashTelemetry {

void EarlyInitialize(HMODULE selfModule) {
  g_selfModule = selfModule;
  g_startTick = GetTickCount64();

  wchar_t appData[MAX_PATH]{};
  DWORD length =
      GetEnvironmentVariableW(L"APPDATA", appData, _countof(appData));
  if (length == 0 || length >= _countof(appData))
    GetTempPathW(_countof(appData), appData);

  _snwprintf_s(g_logDirectory, _countof(g_logDirectory), _TRUNCATE,
               L"%s\\RavenCash", appData);
  CreateDirectoryW(g_logDirectory, nullptr);
  _snwprintf_s(g_logPath, _countof(g_logPath), _TRUNCATE,
               L"%s\\crash_debug.log", g_logDirectory);

  g_logFile =
      CreateFileW(g_logPath, FILE_APPEND_DATA,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredHandler);
  g_previousUnhandledFilter = SetUnhandledExceptionFilter(UnhandledHandler);
  Mark(Stage::DllAttach, false);
  Trace("========== NEW DLL SESSION ==========");
  Trace("DllMain PROCESS_ATTACH module=%p veh=%p previousUnhandled=%p", 
        selfModule, g_vectoredHandler, g_previousUnhandledFilter);
}

void RuntimeInitialize() {
  Mark(Stage::RuntimeDiagnostics);
  char processPath[MAX_PATH]{};
  char modulePath[MAX_PATH]{};
  GetModuleFileNameA(nullptr, processPath, MAX_PATH);
  GetModuleFileNameA(g_selfModule, modulePath, MAX_PATH);
  Trace("process path=%s", processPath);
  Trace("command-line=%ls", GetCommandLineW());
  Trace("self module=%p path=%s", g_selfModule, modulePath);

  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  Trace("system processors=%lu pageSize=%lu allocationGranularity=%lu",
        system.dwNumberOfProcessors, system.dwPageSize,
        system.dwAllocationGranularity);

  static const char *modules[] = {
      "client.dll",       "engine2.dll",      "schemasystem.dll",
      "tier0.dll",        "rendersystemdx11.dll",
      "panorama.dll",     "scenesystem.dll",  "inputsystem.dll",
      "GameOverlayRenderer64.dll"};
  for (const char *module : modules)
    LogModule(module);
}

void Mark(Stage stage, bool emitLine) {
  const DWORD threadId = GetCurrentThreadId();
  if (ThreadStageSlot *slot = FindThreadSlot(threadId, true))
    InterlockedExchange(&slot->stage, static_cast<LONG>(stage));
  if (emitLine)
    Trace("stage-enter");
}

void Trace(const char *format, ...) {
  va_list args;
  va_start(args, format);
  TraceV("trace", format, args, false);
  va_end(args);
}

void Heartbeat(const char *name, unsigned intervalMs) {
  const LONG64 now = static_cast<LONG64>(GetTickCount64());
  const LONG64 previous = InterlockedCompareExchange64(
      &g_lastHeartbeatTick, 0, 0);
  if (previous != 0 && now - previous < static_cast<LONG64>(intervalMs))
    return;
  if (InterlockedCompareExchange64(&g_lastHeartbeatTick, now, previous) !=
      previous)
    return;
  Trace("heartbeat name=%s", name ? name : "<unnamed>");
}

int BoundaryFilter(EXCEPTION_POINTERS *exceptionInfo,
                   const char *boundaryName) {
  if (InterlockedCompareExchange(&g_exceptionWriterActive, 1, 0) == 0) {
    __try {
      RecordException(exceptionInfo,
                      boundaryName ? boundaryName : "seh-boundary", true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    InterlockedExchange(&g_exceptionWriterActive, 0);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

void Shutdown() {
  Mark(Stage::Shutdown);
  Trace("telemetry shutdown requested");
  if (g_vectoredHandler) {
    RemoveVectoredExceptionHandler(g_vectoredHandler);
    g_vectoredHandler = nullptr;
  }
  SetUnhandledExceptionFilter(g_previousUnhandledFilter);
  AcquireSRWLockExclusive(&g_logLock);
  if (g_logFile != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(g_logFile);
    CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
  }
  ReleaseSRWLockExclusive(&g_logLock);
}

const wchar_t *GetLogPath() { return g_logPath; }

} // namespace CrashTelemetry
