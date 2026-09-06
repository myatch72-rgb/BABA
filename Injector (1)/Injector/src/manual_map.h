#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <string>
#include <vector>

// BlackBone Library Headers - DISABLED FOR CLASSIC SHELLCODE
/*
#pragma warning(push, 0)
#include <BlackBone/Process/Process.h>
#include <BlackBone/ManualMap/MMap.h>
#include <BlackBone/PE/PEImage.h>
#include <BlackBone/Misc/Utils.h>
#pragma warning(pop)

#pragma comment(lib, "BlackBone.lib")
*/

struct DllLoaderData_t
{
    char DllPath[MAX_PATH] = { 0 };
};

namespace ManualMap
{
    struct MapOptions
    {
        bool wipeHeader = false;         // Keep PE headers intact for resource access and stability
        bool hideFromVAD = false;        // VAD hiding disabled for stability
        bool doubleWipeHeader = false;   // No secondary header wiping
        DWORD stabilityDelayMs = 1500;   // 1.5 seconds for hook race condition prevention
    };

    struct MapResult
    {
        bool success = false;
        uintptr_t imageBase = 0;
        std::wstring errorMessage;
    };

    // Performs manual map of a DLL into a target process
    MapResult InjectDll(
        HANDLE hProcess,
        const std::string& dllPath,
        const std::string& customDir = "",
        const MapOptions& options = MapOptions{}
    );
}
