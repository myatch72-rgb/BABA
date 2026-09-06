#include "process.h"
#include <TlHelp32.h>
#include <Psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment(lib, "Psapi.lib")

namespace ProcessUtils
{
    bool IsElevated()
    {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
            return false;

        TOKEN_ELEVATION elevation = {};
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        bool isElevated = false;

        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize))
        {
            isElevated = (elevation.TokenIsElevated != 0);
        }

        CloseHandle(hToken);
        return isElevated;
    }

    bool EnableDebugPrivilege()
    {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
            return false;

        LUID luid = {};
        if (!LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid))
        {
            CloseHandle(hToken);
            return false;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        BOOL result = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
        DWORD error = GetLastError();
        CloseHandle(hToken);

        return (result && error == ERROR_SUCCESS);
    }

    DWORD FindProcessId(const std::string& processName)
    {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32 pe = {};
        pe.dwSize = sizeof(PROCESSENTRY32);

        DWORD targetPid = 0;
        if (Process32First(hSnapshot, &pe))
        {
            do
            {
                char exeName[MAX_PATH] = {};
                #ifdef UNICODE
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exeName, MAX_PATH, nullptr, nullptr);
                #else
                strncpy_s(exeName, sizeof(exeName), pe.szExeFile, _TRUNCATE);
                #endif

                if (_stricmp(exeName, processName.c_str()) == 0)
                {
                    targetPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(hSnapshot, &pe));
        }

        CloseHandle(hSnapshot);
        return targetPid;
    }

    bool IsProcess64Bit(HANDLE hProcess)
    {
        BOOL isWow64 = FALSE;
        if (IsWow64Process(hProcess, &isWow64))
        {
            // In 64-bit Windows: FALSE means 64-bit native application
            return !isWow64;
        }
        return false;
    }

    HANDLE OpenTargetProcess(DWORD pid, DWORD desiredAccess)
    {
        return OpenProcess(desiredAccess, FALSE, pid);
    }

    // ========== MODULE SYNCHRONIZATION IMPLEMENTATION ==========

    bool IsModuleLoaded(HANDLE hProcess, const std::string& moduleName)
    {
        HMODULE hMods[1024];
        DWORD cbNeeded = 0;

        // Enumerate all modules in target process
        if (!EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL))
            return false;

        DWORD moduleCount = cbNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < moduleCount; i++)
        {
            char szModName[MAX_PATH] = {};
            if (GetModuleBaseNameA(hProcess, hMods[i], szModName, sizeof(szModName)))
            {
                if (_stricmp(szModName, moduleName.c_str()) == 0)
                    return true;
            }
        }

        return false;
    }

    uintptr_t GetModuleBaseAddress(HANDLE hProcess, const std::string& moduleName)
    {
        HMODULE hMods[1024];
        DWORD cbNeeded = 0;

        if (!EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL))
            return 0;

        DWORD moduleCount = cbNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < moduleCount; i++)
        {
            char szModName[MAX_PATH] = {};
            if (GetModuleBaseNameA(hProcess, hMods[i], szModName, sizeof(szModName)))
            {
                if (_stricmp(szModName, moduleName.c_str()) == 0)
                    return reinterpret_cast<uintptr_t>(hMods[i]);
            }
        }

        return 0;
    }

    std::vector<std::string> EnumerateModules(HANDLE hProcess)
    {
        std::vector<std::string> modules;
        HMODULE hMods[1024];
        DWORD cbNeeded = 0;

        if (!EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL))
            return modules;

        DWORD moduleCount = cbNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < moduleCount; i++)
        {
            char szModName[MAX_PATH] = {};
            if (GetModuleBaseNameA(hProcess, hMods[i], szModName, sizeof(szModName)))
            {
                modules.push_back(std::string(szModName));
            }
        }

        return modules;
    }

    bool WaitForCriticalModules(HANDLE hProcess, DWORD timeoutMs)
    {
        // Critical CS2 modules that MUST be loaded before injection
        const std::vector<std::string> criticalModules = {
            "client.dll",          // Main CS2 game logic
            "engine2.dll",         // Source 2 engine core
            "tier0.dll",           // Low-level utilities
            "inputsystem.dll",     // Input handling
            "scenesystem.dll",     // Scene management
            "rendersystemdx11.dll" // DirectX 11 renderer (critical for Present hook)
        };

        std::cout << "[*] Kritik CS2 modulleri bekleniyor (Race Condition Onleme)..." << std::endl;
        std::cout << "    Hedef Moduller: client.dll, engine2.dll, rendersystemdx11.dll..." << std::endl;

        auto startTime = std::chrono::steady_clock::now();
        size_t loadedCount = 0;

        while (true)
        {
            loadedCount = 0;

            // Check each critical module
            for (const auto& moduleName : criticalModules)
            {
                if (IsModuleLoaded(hProcess, moduleName))
                {
                    loadedCount++;
                }
            }

            // All critical modules loaded
            if (loadedCount == criticalModules.size())
            {
                std::cout << "[+] Tum kritik moduller yuklendi! (" << loadedCount << "/" << criticalModules.size() << ")" << std::endl;
                
                // Additional safety delay for module initialization
                std::cout << "[*] Modul ilklendirme guvenligi icin 2 saniye bekleniyor..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                
                return true;
            }

            // Check timeout
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime
            ).count();

            if (elapsed >= timeoutMs)
            {
                std::cout << "[!] Zaman asimi! Sadece " << loadedCount << "/" << criticalModules.size() 
                          << " modul yuklenebildi." << std::endl;
                std::cout << "[!] Enjeksiyon devam ediyor ama karsiliksizlik riski var!" << std::endl;
                return false;
            }

            // Progress indicator
            if (static_cast<int>(elapsed / 1000) % 3 == 0)
            {
                std::cout << "    [" << loadedCount << "/" << criticalModules.size() << " yuklendi, "
                          << (timeoutMs - elapsed) / 1000 << "s kaldi]" << std::endl;
            }

            // Poll every 500ms
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}
