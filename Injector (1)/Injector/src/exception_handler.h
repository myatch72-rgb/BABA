#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <string>
#include "logger.h"

namespace ExceptionHandler
{
    // Windows Structured Exception Handler (SEH) callback
    // Catches crashes and logs them before the process terminates
    inline LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo)
    {
        if (pExceptionInfo && pExceptionInfo->ExceptionRecord)
        {
            DWORD exceptionCode = pExceptionInfo->ExceptionRecord->ExceptionCode;
            PVOID exceptionAddress = pExceptionInfo->ExceptionRecord->ExceptionAddress;

            // Log the exception with full details
            InjectorLogger::g_Logger.LogException(
                exceptionCode,
                exceptionAddress,
                "Unhandled exception in injector process"
            );

            // Additional context if available
            if (pExceptionInfo->ContextRecord)
            {
                auto ctx = pExceptionInfo->ContextRecord;
                char contextInfo[512];
                sprintf_s(contextInfo, sizeof(contextInfo),
                    "CPU Context: RIP=0x%llx, RSP=0x%llx, RBP=0x%llx, RAX=0x%llx",
                    ctx->Rip, ctx->Rsp, ctx->Rbp, ctx->Rax
                );
                InjectorLogger::g_Logger.Debug(contextInfo);
            }

            // Log exception parameters
            for (DWORD i = 0; i < pExceptionInfo->ExceptionRecord->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
            {
                char paramInfo[128];
                sprintf_s(paramInfo, sizeof(paramInfo),
                    "Exception Parameter[%u]: 0x%llx",
                    i, pExceptionInfo->ExceptionRecord->ExceptionInformation[i]
                );
                InjectorLogger::g_Logger.Debug(paramInfo);
            }

            InjectorLogger::g_Logger.Shutdown();
        }

        // Pass to default handler (will show Windows error dialog and terminate)
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Install the SEH handler for the injector process
    inline void InstallExceptionHandler()
    {
        SetUnhandledExceptionFilter(UnhandledExceptionFilter);
        InjectorLogger::g_Logger.Debug("SEH (Structured Exception Handler) installed for crash reporting.");
    }

    // Remove the SEH handler (called before clean exit)
    inline void RemoveExceptionHandler()
    {
        SetUnhandledExceptionFilter(nullptr);
        InjectorLogger::g_Logger.Debug("SEH handler removed.");
    }
}
