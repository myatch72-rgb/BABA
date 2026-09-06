#pragma once
#include <Windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

// File logger for the SkinChanger. The old version was a no-op stub, which
// meant every diagnostic Log() call in SkinChanger.cpp silently vanished —
// making failures like the custom-model "ERROR" mesh impossible to debug.
// Writes to %APPDATA%\RavenCash\skinchanger.log, alongside the other live
// feature diagnostics. Header-only so `inline SCLogger g_Logger;` keeps
// working across translation units.
class SCLogger
{
public:
    void Init()
    {
        Log("==== SkinChanger logger initialized (pid=%lu) ====",
            GetCurrentProcessId());
    }

    void Log(const char* fmt, ...)
    {
        if (!fmt)
            return;

        char msg[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(msg, _TRUNCATE, fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);

        char line[1200];
        _snprintf_s(line, _TRUNCATE, "[%02u:%02u:%02u.%03u][%5lu] %s\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    GetCurrentThreadId(), msg);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureOpenLocked())
            return;
        fputs(line, m_file);
        fflush(m_file);
    }

private:
    // Caller must hold m_mutex.
    bool EnsureOpenLocked()
    {
        if (m_file)
            return true;
        if (m_failed)
            return false;

        char path[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
        {
            m_failed = true;
            return false;
        }
        strcat_s(path, "\\RavenCash");
        CreateDirectoryA(path, nullptr); // ok if it already exists
        strcat_s(path, "\\skinchanger.log");

        if (fopen_s(&m_file, path, "a") != 0 || !m_file)
        {
            m_file = nullptr;
            m_failed = true;
            return false;
        }
        return true;
    }

    std::mutex m_mutex;
    FILE* m_file = nullptr;
    bool m_failed = false;
};

inline SCLogger g_Logger;
