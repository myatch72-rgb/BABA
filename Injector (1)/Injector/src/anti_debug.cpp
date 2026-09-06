#include "anti_debug.h"
#include <iostream>
#include <winternl.h>

namespace AntiDebug
{
    bool IsDebuggerPresentExtended()
    {
        // 1. Basic Win32 API
        if (IsDebuggerPresent())
            return true;

        // 2. Remote debugger check
        BOOL isRemoteDebugger = FALSE;
        if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &isRemoteDebugger) && isRemoteDebugger)
            return true;

        // 3. PEB BeingDebugged check
        #if defined(_M_X64) || defined(__x86_64__)
        PPEB peb = reinterpret_cast<PPEB>(__readgsqword(0x60));
        if (peb && peb->BeingDebugged)
            return true;
        #endif

        return false;
    }

    bool CheckHardwareBreakpoints()
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        HANDLE hThread = GetCurrentThread();

        if (GetThreadContext(hThread, &ctx))
        {
            if (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3)
                return true;
        }

        return false;
    }

    bool PerformEnvironmentChecks()
    {
        if (IsDebuggerPresentExtended())
        {
            std::cout << "  [!] Uyari: Debugger algilandi!" << std::endl;
            return false;
        }

        if (CheckHardwareBreakpoints())
        {
            std::cout << "  [!] Uyari: Donanimsal kesme noktasi (DRx) algilandi!" << std::endl;
            return false;
        }

        return true;
    }
}