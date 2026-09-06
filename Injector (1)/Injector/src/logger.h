#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <string>
#include <fstream>
#include <mutex>

#ifdef ERROR
#undef ERROR
#endif

namespace InjectorLogger
{
    enum class LogLevel
    {
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        CRITICAL
    };

    class Logger
    {
    public:
        Logger();
        ~Logger();

        bool Initialize();
        void Shutdown();

        void Log(LogLevel level, const std::string& message);
        void Log(LogLevel level, const std::wstring& message);
        void LogFormat(LogLevel level, const char* format, ...);
        
        void Info(const std::string& message);
        void Warning(const std::string& message);
        void Error(const std::string& message);
        void Critical(const std::string& message);
        void Debug(const std::string& message);

        void LogSystemInfo();
        void LogWin32Error(const std::string& context, DWORD errorCode);
        void LogException(DWORD exceptionCode, PVOID exceptionAddress, const std::string& message);

        std::string GetLogFilePath() const { return m_logFilePath; }

    private:
        std::string GetTimestampString();
        void SetConsoleColor(LogLevel level);
        void ResetConsoleColor();

        std::ofstream m_fileStream;
        std::string m_logFilePath;
        std::mutex m_mutex;
        bool m_initialized;
        HANDLE m_hConsole;
        WORD m_defaultConsoleAttributes;
    };

    extern Logger g_Logger;
}
