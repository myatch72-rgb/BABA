#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

namespace AntiDebug
{
    // Checks if a debugger is attached or hardware breakpoints are present
    bool IsDebuggerPresentExtended();

    // Verifies hardware debug registers DR0..DR3
    bool CheckHardwareBreakpoints();

    // Verifies system environment safety
    bool PerformEnvironmentChecks();
}