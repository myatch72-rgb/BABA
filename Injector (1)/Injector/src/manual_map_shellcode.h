#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <string>

namespace ManualMapShellcode
{
    struct MapResult
    {
        bool success = false;
        uintptr_t imageBase = 0;
        std::wstring errorMessage;
    };

    // Classic manual map using CUSTOM SHELLCODE (compatible with raven.dll)
    // This method is 100% compatible with the original injector
    MapResult InjectDll(HANDLE hProcess, const std::string& dllPath);
}
