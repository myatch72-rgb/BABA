#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstdarg>

namespace fs = std::filesystem;

namespace InjectorLogger
{
    Logger g_Logger;

    Logger::Logger()
        : m_initialized(false),
          m_hConsole(INVALID_HANDLE_VALUE),
          m_defaultConsoleAttributes(7)
    {
    }

    Logger::~Logger()
    {
        Shutdown();
    }

    bool Logger::Initialize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) return true;

        m_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (m_hConsole != INVALID_HANDLE_VALUE)
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(m_hConsole, &csbi))
            {
                m_defaultConsoleAttributes = csbi.wAttributes;
            }

            DWORD dwMode = 0;
            if (GetConsoleMode(m_hConsole, &dwMode))
            {
                SetConsoleMode(m_hConsole, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }

        std::error_code ec;
        fs::create_directories("logs", ec);

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm timeInfo = {};
        localtime_s(&timeInfo, &in_time_t);

        std::ostringstream filename;
        filename << "logs/injector_"
                 << std::put_time(&timeInfo, "%Y%m%d_%H%M%S")
                 << ".log";

        m_logFilePath = filename.str();
        m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);

        m_initialized = true;
        return true;
    }

    void Logger::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
        {
            if (m_fileStream.is_open())
            {
                m_fileStream.flush();
                m_fileStream.close();
            }
            m_initialized = false;
        }
    }

    std::string Logger::GetTimestampString()
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm timeInfo = {};
        localtime_s(&timeInfo, &in_time_t);

        std::ostringstream ss;
        ss << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    void Logger::SetConsoleColor(LogLevel level)
    {
        if (m_hConsole == INVALID_HANDLE_VALUE) return;

        WORD color = m_defaultConsoleAttributes;
        switch (level)
        {
        case LogLevel::DEBUG:
            color = FOREGROUND_INTENSITY;
            break;
        case LogLevel::INFO:
            color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case LogLevel::WARNING:
            color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case LogLevel::ERROR:
            color = FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        case LogLevel::CRITICAL:
            color = BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        }

        SetConsoleTextAttribute(m_hConsole, color);
    }

    void Logger::ResetConsoleColor()
    {
        if (m_hConsole != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(m_hConsole, m_defaultConsoleAttributes);
        }
    }

    void Logger::Log(LogLevel level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string timestamp = GetTimestampString();
        std::string levelStr = "[INFO]    ";
        switch (level)
        {
        case LogLevel::DEBUG:    levelStr = "[DEBUG]   "; break;
        case LogLevel::INFO:     levelStr = "[INFO]    "; break;
        case LogLevel::WARNING:  levelStr = "[WARNING] "; break;
        case LogLevel::ERROR:    levelStr = "[ERROR]   "; break;
        case LogLevel::CRITICAL: levelStr = "[CRITICAL]"; break;
        }

        SetConsoleColor(level);
        std::cout << timestamp << " " << levelStr << " " << message << std::endl;
        ResetConsoleColor();

        if (m_fileStream.is_open())
        {
            m_fileStream << timestamp << " " << levelStr << " " << message << std::endl;
            m_fileStream.flush();
        }
    }

    void Logger::Log(LogLevel level, const std::wstring& message)
    {
        char buffer[2048] = {};
        WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, buffer, sizeof(buffer), nullptr, nullptr);
        Log(level, std::string(buffer));
    }

    void Logger::LogFormat(LogLevel level, const char* format, ...)
    {
        char buffer[2048] = {};
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        Log(level, std::string(buffer));
    }

    void Logger::Info(const std::string& message) { Log(LogLevel::INFO, message); }
    void Logger::Warning(const std::string& message) { Log(LogLevel::WARNING, message); }
    void Logger::Error(const std::string& message) { Log(LogLevel::ERROR, message); }
    void Logger::Critical(const std::string& message) { Log(LogLevel::CRITICAL, message); }
    void Logger::Debug(const std::string& message) { Log(LogLevel::DEBUG, message); }

    void Logger::LogSystemInfo()
    {
        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);

        std::ostringstream ss;
        ss << "System Arch: " << (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64 (AMD64)" : "x86")
           << " | Cores: " << si.dwNumberOfProcessors
           << " | PageSize: " << si.dwPageSize;

        Info(ss.str());
    }

    void Logger::LogWin32Error(const std::string& context, DWORD errorCode)
    {
        char errorMsg[512] = {};
        FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            errorMsg,
            sizeof(errorMsg),
            nullptr
        );

        size_t len = strlen(errorMsg);
        while (len > 0 && (errorMsg[len - 1] == '\r' || errorMsg[len - 1] == '\n' || errorMsg[len - 1] == ' '))
        {
            errorMsg[--len] = '\0';
        }

        std::ostringstream ss;
        ss << context << " - Win32 Hata Kodu (" << errorCode << "): "
           << (len > 0 ? errorMsg : "Bilinmeyen hata");

        Error(ss.str());
    }

    void Logger::LogException(DWORD exceptionCode, PVOID exceptionAddress, const std::string& message)
    {
        std::ostringstream ss;
        ss << "CRASH/EXCEPTION: " << message
           << " | Kod: 0x" << std::hex << exceptionCode
           << " | Adres: 0x" << exceptionAddress << std::dec;

        Critical(ss.str());
    }
}
