#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <string>
#include <vector>

namespace ProcessUtils
{
    // Checks if current injector process is running with elevated (Administrator) rights
    bool IsElevated();

    // Acquires SeDebugPrivilege to access protected/elevated processes
    bool EnableDebugPrivilege();

    // Finds Process ID by executable name (case-insensitive)
    DWORD FindProcessId(const std::string& processName);

    // Checks whether target process is 64-bit
    bool IsProcess64Bit(HANDLE hProcess);

    // Opens target process with required access rights
    HANDLE OpenTargetProcess(DWORD pid, DWORD desiredAccess = PROCESS_ALL_ACCESS);

    // ========== NEW: MODULE SYNCHRONIZATION SYSTEM ==========

    // Waits for critical CS2 modules to be loaded in memory before injection
    // This prevents race conditions where DLL tries to hook functions before they exist
    // Returns true if all modules are loaded, false on timeout
    bool WaitForCriticalModules(HANDLE hProcess, DWORD timeoutMs = 30000);

    // Checks if a specific module is loaded in the target process
    bool IsModuleLoaded(HANDLE hProcess, const std::string& moduleName);

    // Gets the base address of a loaded module (returns 0 if not found)
    uintptr_t GetModuleBaseAddress(HANDLE hProcess, const std::string& moduleName);

    // Enumerates all loaded modules in target process (for diagnostic purposes)
    std::vector<std::string> EnumerateModules(HANDLE hProcess);
}
