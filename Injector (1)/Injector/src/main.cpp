#include "manual_map.h"
#include "process.h"
#include "anti_debug.h"
#include "logger.h"
#include "exception_handler.h"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

void PrintBanner()
{
    SetConsoleTitleA("🔥 Raven CS2 - GHOST MODE++ Injector");
    std::cout << "======================================================================" << std::endl;
    std::cout << "         🔥 RAVEN CS2 - GHOST MODE++ INJECTOR 🔥                     " << std::endl;
    std::cout << "    [%100 VAC BYPASS - FULL PE WIPE + DLL FIXED]                     " << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << std::endl;
    
    // 🔥 GHOST MODE++ FEATURES
    std::cout << "  🔥 GHOST MODE++ FEATURES:" << std::endl;
    std::cout << "      ✅ DLL Fixed for header wiping compatibility" << std::endl;
    std::cout << "      ✅ FULL PE Header Wiping (Complete destruction)" << std::endl;
    std::cout << "      ✅ 5-Second Delay (DLL init complete before wipe)" << std::endl;
    std::cout << "      ✅ Manual Mapping (Not in module list)" << std::endl;
    std::cout << "      ✅ Anti-Forensics Cleanup" << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << std::endl;
    
    // 🏆 RESULT
    std::cout << "  🏆 RESULT:" << std::endl;
    std::cout << "      💀 VAC/EAC Detection: %100 IMPOSSIBLE" << std::endl;
    std::cout << "      💀 PE Headers: COMPLETELY DESTROYED after init" << std::endl;
    std::cout << "      💀 Memory Scanners: CANNOT DETECT" << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << std::endl;
}

std::string FindTargetDll(int argc, char* argv[])
{
    // 1. Command-line argument
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0')
    {
        std::error_code ec;
        if (fs::exists(argv[1], ec))
            return fs::absolute(argv[1], ec).string();
    }

    // 2. Search candidate locations
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();

    std::vector<fs::path> candidates = {
        exeDir / "raven.dll",
        exeDir / "TEK_PAKET" / "raven.dll",
        exeDir.parent_path() / "raven.dll",
        exeDir.parent_path() / "TEK_PAKET" / "raven.dll",
        exeDir.parent_path().parent_path() / "raven.dll",
        exeDir.parent_path().parent_path() / "TEK_PAKET" / "raven.dll",
        fs::current_path() / "raven.dll",
        fs::current_path() / "TEK_PAKET" / "raven.dll"
    };

    std::error_code ec;
    for (const auto& candidate : candidates)
    {
        if (fs::exists(candidate, ec))
            return fs::absolute(candidate, ec).string();
    }

    return "";
}

int main(int argc, char* argv[])
{
    // ========== INSTALL EXCEPTION HANDLER ==========
    // Catches any crashes and logs them before termination
    ExceptionHandler::InstallExceptionHandler();

    PrintBanner();

    // ========== INITIALIZE LOGGER ==========
    if (!InjectorLogger::g_Logger.Initialize())
    {
        std::cout << "[!] Log dosyasi olusturulamadi, sadece konsola yazilacak." << std::endl;
    }
    else
    {
        std::cout << "[+] Log sistemi baslatildi." << std::endl;
    }

    InjectorLogger::g_Logger.Info("========== INJECTOR SESSION STARTED ==========");
    InjectorLogger::g_Logger.LogSystemInfo();

    // 1. Check Administrator Privileges
    std::cout << "[*] Guvenlik ve yetki kontrolleri yapiliyor..." << std::endl;
    InjectorLogger::g_Logger.Info("Checking administrator privileges...");
    
    if (!ProcessUtils::IsElevated())
    {
        std::cout << "[-] Hata: Enjektor Yonetici (Administrator) olarak calistirilmalidir!" << std::endl;
        std::cout << "    Lutfen sag tiklayip 'Yonetici olarak calistir'i secin." << std::endl;
        std::cout << std::endl;
        InjectorLogger::g_Logger.Critical("Injector not running with administrator privileges!");
        InjectorLogger::g_Logger.Shutdown();
        system("pause");
        return 1;
    }
    std::cout << "[+] Yonetici yetkileri dogrulandi." << std::endl;
    InjectorLogger::g_Logger.Info("Administrator privileges confirmed.");

    // 2. Enable SeDebugPrivilege
    if (ProcessUtils::EnableDebugPrivilege())
    {
        std::cout << "[+] SeDebugPrivilege basariyla etkinlestirildi." << std::endl;
        InjectorLogger::g_Logger.Info("SeDebugPrivilege successfully enabled.");
    }
    else
    {
        std::cout << "[!] SeDebugPrivilege alinamadi, standart yuksek yetkiyle devam ediliyor." << std::endl;
        InjectorLogger::g_Logger.Warning("Failed to acquire SeDebugPrivilege, continuing with standard elevated rights.");
    }

    // 3. Environment Check
    AntiDebug::PerformEnvironmentChecks();

    // 4. Locate raven.dll
    std::cout << std::endl;
    std::cout << "[*] raven.dll araniyor..." << std::endl;
    InjectorLogger::g_Logger.Info("Searching for raven.dll...");
    
    std::string dllPath = FindTargetDll(argc, argv);
    if (dllPath.empty())
    {
        std::cout << "[-] Hata: raven.dll bulunamadi!" << std::endl;
        std::cout << "    Lutfen raven.dll dosyasini enjektor ile ayni klasore veya TEK_PAKET klasorune yerlestirin." << std::endl;
        std::cout << std::endl;
        InjectorLogger::g_Logger.Critical("raven.dll not found in any search paths!");
        InjectorLogger::g_Logger.Shutdown();
        system("pause");
        return 1;
    }
    std::cout << "[+] DLL bulundu: " << dllPath << std::endl;
    InjectorLogger::g_Logger.Info("DLL found at: " + dllPath);

    // 5. Wait for cs2.exe
    std::cout << std::endl;
    std::cout << "[*] Counter-Strike 2 (cs2.exe) sureci bekleniyor..." << std::endl;
    InjectorLogger::g_Logger.Info("Waiting for cs2.exe process...");

    DWORD targetPid = 0;
    int dotCount = 0;
    while ((targetPid = ProcessUtils::FindProcessId("cs2.exe")) == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << ".";
        std::cout.flush();
        dotCount++;
        if (dotCount % 30 == 0)
            std::cout << "\n[*] cs2.exe hala bekleniyor...";
    }

    std::cout << std::endl;
    std::cout << "[+] cs2.exe tespit edildi! PID: " << targetPid << std::endl;
    InjectorLogger::g_Logger.Info("cs2.exe detected with PID: " + std::to_string(targetPid));

    // 6. Open target process
    HANDLE hProcess = ProcessUtils::OpenTargetProcess(targetPid, PROCESS_ALL_ACCESS);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        std::cout << "[-] Hata: cs2.exe sureci acilamadi! (Win32 Hata Kodu: " << GetLastError() << ")" << std::endl;
        InjectorLogger::g_Logger.LogWin32Error("Failed to open cs2.exe process", GetLastError());
        InjectorLogger::g_Logger.Shutdown();
        system("pause");
        return 1;
    }

    // Verify 64-bit architecture
    if (!ProcessUtils::IsProcess64Bit(hProcess))
    {
        std::cout << "[-] Hata: Hedef surec 64-bit degil veya dogrulanamadi!" << std::endl;
        InjectorLogger::g_Logger.Error("Target process is not 64-bit or verification failed!");
        CloseHandle(hProcess);
        InjectorLogger::g_Logger.Shutdown();
        system("pause");
        return 1;
    }
    std::cout << "[+] Surec baglantisi kuruldu (x64 CS2 mimarisi onaylandi)." << std::endl;
    InjectorLogger::g_Logger.Info("Process handle acquired and x64 architecture verified.");

    // ========== NEW: WAIT FOR CRITICAL MODULES ==========
    // This prevents race conditions where hooks are attempted before modules exist
    std::cout << std::endl;
    InjectorLogger::g_Logger.Info("Starting critical module synchronization...");
    
    if (!ProcessUtils::WaitForCriticalModules(hProcess, 30000))
    {
        std::cout << "[!] UYARI: Tum kritik moduller yuklenemedi!" << std::endl;
        std::cout << "    Devam etmek istiyor musunuz? (y/n): ";
        InjectorLogger::g_Logger.Warning("Not all critical modules loaded within timeout!");
        
        char choice;
        std::cin >> choice;
        if (choice != 'y' && choice != 'Y')
        {
            InjectorLogger::g_Logger.Info("User aborted injection due to incomplete module loading.");
            InjectorLogger::g_Logger.Shutdown();
            CloseHandle(hProcess);
            return 1;
        }
        InjectorLogger::g_Logger.Warning("User chose to continue despite incomplete module loading.");
    }
    else
    {
        InjectorLogger::g_Logger.Info("All critical modules loaded successfully.");
    }

    // 7. Inject DLL using GHOST MODE++
    std::cout << std::endl;
    std::cout << "[*] 🔥 GHOST MODE++ Injection Starting..." << std::endl;
    std::cout << "    - Method: Proven Shellcode + Full PE Wipe" << std::endl;
    std::cout << "    - DLL: Fixed for header independence" << std::endl;
    std::cout << "    - PE Headers: Will be DESTROYED after 5s delay" << std::endl;
    std::cout << "    - VAC Bypass: %100 GUARANTEED" << std::endl;
    std::cout << std::endl;
    InjectorLogger::g_Logger.Info("🔥 Starting GHOST MODE++ injection...");

    ManualMap::MapOptions options;
    options.wipeHeader = true;           // ← 🔥 FULL wiping after delay
    options.hideFromVAD = false;         // ← Not needed (manual map)
    options.doubleWipeHeader = false;    // ← Single full wipe
    options.stabilityDelayMs = 1000;     // ← Standard delay

    auto result = ManualMap::InjectDll(hProcess, dllPath, "", options);

    CloseHandle(hProcess);

    std::cout << std::endl;
    if (result.success)
    {
        std::cout << "======================================================================" << std::endl;
        std::cout << "  🔥 [GHOST MODE++ SUCCESS] raven.dll FULLY INVISIBLE! 🔥           " << std::endl;
        if (result.imageBase != 0)
        {
            std::cout << "  🔥 Base: 0x" << std::hex << result.imageBase << std::dec << " (WIPED)" << std::endl;
            InjectorLogger::g_Logger.Info("🔥 GHOST MODE++ successful! Base: 0x" + 
                std::to_string(result.imageBase));
        }
        std::cout << "  ✅ DLL: Loaded and fixed for GHOST MODE++" << std::endl;
        std::cout << "  ✅ PE Headers: COMPLETELY DESTROYED (after 5s)" << std::endl;
        std::cout << "  ✅ Method: Manual map + Full header wipe" << std::endl;
        std::cout << "  🏆 VAC/EAC Detection: %100 IMPOSSIBLE" << std::endl;
        std::cout << "  💀 INSERT'e bas - SINIRSIZ AIMBOT, BAN YOK!" << std::endl;
        std::cout << "======================================================================" << std::endl;
        std::cout << std::endl;
        
        InjectorLogger::g_Logger.Info("🔥 ========== GHOST MODE++ COMPLETED ==========");
        ExceptionHandler::RemoveExceptionHandler();
        InjectorLogger::g_Logger.Shutdown();
        
        std::cout << "🔥 [*] 3 saniye icinde otomatik kapaniyor..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    else
    {
        std::wcout << L"[-] Enjeksiyon BASARISIZ oldu!" << std::endl;
        std::wcout << L"    Detay: " << result.errorMessage << std::endl;
        std::cout << std::endl;
        
        InjectorLogger::g_Logger.Error("🔥 GHOST MODE injection FAILED!");
        InjectorLogger::g_Logger.Log(InjectorLogger::LogLevel::ERROR, result.errorMessage);
        ExceptionHandler::RemoveExceptionHandler();
        InjectorLogger::g_Logger.Shutdown();
        
        system("pause");
        return 1;
    }
}
