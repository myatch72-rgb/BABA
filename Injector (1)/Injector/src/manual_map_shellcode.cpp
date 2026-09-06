#include "manual_map.h"

#include <fstream>
#include <iostream>
#include <vector>

// ============================================================================
// Shellcode icin veri yapisi - hedef surece kopyalanacak
// ============================================================================
struct MANUAL_MAP_DATA {
    BYTE*     pBase;                                                        // DLL base adresi (hedef surec)
    HMODULE   (WINAPI* pLoadLibraryA)(LPCSTR);                              // LoadLibraryA fonksiyon pointeri
    FARPROC   (WINAPI* pGetProcAddress)(HMODULE, LPCSTR);                   // GetProcAddress fonksiyon pointeri
    BOOL      (WINAPI* pRtlAddFunctionTable)(PRUNTIME_FUNCTION, DWORD, DWORD64); // x64 exception table kaydi
    BOOL      bSuccess;                                                     // Basari flagi
};

// DllMain fonksiyon tanimlamasi
using f_DLL_ENTRY_POINT = BOOL(WINAPI*)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);

// ============================================================================
// SHELLCODE - Bu fonksiyon hedef surece kopyalanip orada calistirilir
// KURALLAR:
//   - Hicbir global degisken kullanilamaz
//   - Hicbir string literal kullanilamaz
//   - Hicbir CRT fonksiyonu kullanilamaz
//   - Sadece MANUAL_MAP_DATA icindeki fonksiyon pointerleri kullanilabilir
// ============================================================================
#pragma optimize("", off)
#pragma runtime_checks("", off)

void __stdcall Shellcode(MANUAL_MAP_DATA* pData) {
    if (!pData)
        return;

    BYTE* pBase = pData->pBase;
    if (!pBase)
        return;

    // DOS Header dogrula
    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(pBase);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) // 'MZ'
        return;

    // NT Headers dogrula
    auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(pBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE) // 'PE\0\0'
        return;

    auto* pOpt = &pNt->OptionalHeader;

    // ========================================================================
    // ADIM 1: Base Relocation Duzeltmeleri
    // DLL farkli bir base adrese yuklendiyse tum absolute adresleri duzelt
    // ========================================================================
    UINT_PTR locationDelta = reinterpret_cast<UINT_PTR>(pBase) - pOpt->ImageBase;

    if (locationDelta) {
        if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
            auto* pRelocBlock = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

            while (pRelocBlock->VirtualAddress) {
                UINT numEntries = (pRelocBlock->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* pEntries = reinterpret_cast<WORD*>(pRelocBlock + 1);

                for (UINT i = 0; i < numEntries; i++, pEntries++) {
                    int type   = *pEntries >> 12;
                    int offset = *pEntries & 0xFFF;

                    if (type == IMAGE_REL_BASED_DIR64) {
                        // x64: 8 byte pointer relocation
                        *reinterpret_cast<UINT_PTR*>(pBase + pRelocBlock->VirtualAddress + offset) += locationDelta;
                    }
                    else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        // x86 uyumu: 4 byte relocation
                        *reinterpret_cast<DWORD*>(pBase + pRelocBlock->VirtualAddress + offset) += static_cast<DWORD>(locationDelta);
                    }
                    else if (type == IMAGE_REL_BASED_HIGH) {
                        *reinterpret_cast<WORD*>(pBase + pRelocBlock->VirtualAddress + offset) += HIWORD(locationDelta);
                    }
                    else if (type == IMAGE_REL_BASED_LOW) {
                        *reinterpret_cast<WORD*>(pBase + pRelocBlock->VirtualAddress + offset) += LOWORD(locationDelta);
                    }
                    // IMAGE_REL_BASED_ABSOLUTE (type 0) -> atla
                }

                // Sonraki relocation bloguna gec
                pRelocBlock = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<BYTE*>(pRelocBlock) + pRelocBlock->SizeOfBlock);
            }
        }
    }

    // ========================================================================
    // ADIM 2: Import Address Table (IAT) Cozumlemesi
    // Her import edilen DLL yuklenip fonksiyon adresleri cozumlenir
    // ========================================================================
    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto* pImportDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        while (pImportDesc->Name) {
            char* szModule = reinterpret_cast<char*>(pBase + pImportDesc->Name);
            HMODULE hModule = pData->pLoadLibraryA(szModule);

            if (hModule) {
                UINT_PTR* pThunkRef;
                UINT_PTR* pFuncRef;

                if (pImportDesc->OriginalFirstThunk) {
                    pThunkRef = reinterpret_cast<UINT_PTR*>(pBase + pImportDesc->OriginalFirstThunk);
                    pFuncRef  = reinterpret_cast<UINT_PTR*>(pBase + pImportDesc->FirstThunk);
                }
                else {
                    // Bazi DLL'lerde OriginalFirstThunk olmayabilir
                    pThunkRef = reinterpret_cast<UINT_PTR*>(pBase + pImportDesc->FirstThunk);
                    pFuncRef  = pThunkRef;
                }

                for (; *pThunkRef; pThunkRef++, pFuncRef++) {
                    if (IMAGE_SNAP_BY_ORDINAL(*pThunkRef)) {
                        // Ordinal ile import
                        *pFuncRef = reinterpret_cast<UINT_PTR>(
                            pData->pGetProcAddress(hModule,
                                reinterpret_cast<LPCSTR>(*pThunkRef & 0xFFFF)));
                    }
                    else {
                        // Isim ile import
                        auto* pImportByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(pBase + *pThunkRef);
                        *pFuncRef = reinterpret_cast<UINT_PTR>(
                            pData->pGetProcAddress(hModule, pImportByName->Name));
                    }
                }
            }

            pImportDesc++;
        }
    }

    // ========================================================================
    // ADIM 3: TLS (Thread Local Storage) Callback'leri
    // ========================================================================
    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* pTls = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);

        auto* pCallback = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(pTls->AddressOfCallBacks);
        if (pCallback) {
            while (*pCallback) {
                (*pCallback)(reinterpret_cast<PVOID>(pBase), DLL_PROCESS_ATTACH, nullptr);
                pCallback++;
            }
        }
    }

    // ========================================================================
    // ADIM 4: x64 Exception Handler Tablosu Kaydi
    // SEH (__try/__except) kullanan DLL'ler icin gerekli
    // ========================================================================
    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size && pData->pRtlAddFunctionTable) {
        auto* pFuncEntry = reinterpret_cast<PRUNTIME_FUNCTION>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress);
        DWORD numEntries = pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(RUNTIME_FUNCTION);
        pData->pRtlAddFunctionTable(pFuncEntry, numEntries, reinterpret_cast<DWORD64>(pBase));
    }

    // ========================================================================
    // ADIM 5: DllMain Cagrisi
    // ========================================================================
    if (pOpt->AddressOfEntryPoint) {
        auto pDllMain = reinterpret_cast<f_DLL_ENTRY_POINT>(pBase + pOpt->AddressOfEntryPoint);
        pDllMain(reinterpret_cast<HINSTANCE>(pBase), DLL_PROCESS_ATTACH, nullptr);
    }

    pData->bSuccess = TRUE;
}

// Shellcode boyutunu hesaplamak icin isaretci fonksiyon - ASLA TASIMAYIN
void __stdcall ShellcodeStub() { }

#pragma optimize("", on)

// ============================================================================
// MANUAL MAPPER ANA MOTORU
// ============================================================================
namespace ManualMapper {

    bool MapDll(HANDLE hProcess, const std::string& dllPath) {

        // ====================================================================
        // 1. DLL dosyasini diskten oku
        // ====================================================================
        std::ifstream file(dllPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "  [-] DLL dosyasi acilamadi: " << dllPath << std::endl;
            return false;
        }

        auto fileSize = file.tellg();
        if (fileSize < sizeof(IMAGE_DOS_HEADER)) {
            std::cerr << "  [-] DLL dosyasi cok kucuk" << std::endl;
            file.close();
            return false;
        }

        file.seekg(0, std::ios::beg);
        std::vector<BYTE> rawDll(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(rawDll.data()), fileSize);
        file.close();

        std::cout << "  [+] DLL okundu: " << fileSize << " bytes" << std::endl;

        // ====================================================================
        // 2. PE Basliklarini Parse Et
        // ====================================================================
        auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(rawDll.data());
        if (pDos->e_magic != IMAGE_DOS_SIGNATURE) {
            std::cerr << "  [-] Gecersiz DOS imzasi (MZ bekleniyor)" << std::endl;
            return false;
        }

        auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(rawDll.data() + pDos->e_lfanew);
        if (pNt->Signature != IMAGE_NT_SIGNATURE) {
            std::cerr << "  [-] Gecersiz NT imzasi (PE bekleniyor)" << std::endl;
            return false;
        }

        if (pNt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            std::cerr << "  [-] DLL x64 degil! (IMAGE_FILE_MACHINE_AMD64 bekleniyor)" << std::endl;
            return false;
        }

        auto* pOpt = &pNt->OptionalHeader;

        std::cout << "  [+] PE baslik dogrulandi" << std::endl;
        std::cout << "      Image Size : 0x" << std::hex << pOpt->SizeOfImage << std::dec << std::endl;
        std::cout << "      Entry Point: 0x" << std::hex << pOpt->AddressOfEntryPoint << std::dec << std::endl;
        std::cout << "      Sections   : " << pNt->FileHeader.NumberOfSections << std::endl;

        // ====================================================================
        // 3. Hedef Surece Bellek Tahsis Et
        // ====================================================================
        BYTE* pTargetBase = reinterpret_cast<BYTE*>(
            VirtualAllocEx(hProcess, nullptr, pOpt->SizeOfImage,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        if (!pTargetBase) {
            std::cerr << "  [-] Hedef surece bellek tahsis edilemedi. Hata: " << GetLastError() << std::endl;
            return false;
        }

        std::cout << "  [+] Bellek tahsis edildi: 0x" << std::hex
                  << reinterpret_cast<UINT_PTR>(pTargetBase) << std::dec << std::endl;

        // ====================================================================
        // 4. PE Basliklarini Yaz
        // ====================================================================
        if (!WriteProcessMemory(hProcess, pTargetBase, rawDll.data(), pOpt->SizeOfHeaders, nullptr)) {
            std::cerr << "  [-] PE basliklari yazilamadi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        // ====================================================================
        // 5. Section'lari Haritala
        // ====================================================================
        auto* pSectionHeader = IMAGE_FIRST_SECTION(pNt);
        for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++, pSectionHeader++) {
            if (pSectionHeader->SizeOfRawData == 0)
                continue;

            if (!WriteProcessMemory(hProcess,
                    pTargetBase + pSectionHeader->VirtualAddress,
                    rawDll.data() + pSectionHeader->PointerToRawData,
                    pSectionHeader->SizeOfRawData,
                    nullptr))
            {
                char sectionName[9] = {};
                memcpy(sectionName, pSectionHeader->Name, 8);
                std::cerr << "  [-] Section yazilamadi: " << sectionName
                          << " Hata: " << GetLastError() << std::endl;
                VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
                return false;
            }
        }

        std::cout << "  [+] " << pNt->FileHeader.NumberOfSections << " section haritalandi" << std::endl;

        // ====================================================================
        // 6. Shellcode Veri Yapisini Hazirla
        // ====================================================================
        MANUAL_MAP_DATA mapData{};
        mapData.pBase            = pTargetBase;
        mapData.pLoadLibraryA    = LoadLibraryA;
        mapData.pGetProcAddress  = GetProcAddress;
        mapData.bSuccess         = FALSE;

        // RtlAddFunctionTable - x64 exception handling icin gerekli
        // raven.dll __try/__except kullaniyor, bu olmadan crash olur
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            mapData.pRtlAddFunctionTable = reinterpret_cast<decltype(mapData.pRtlAddFunctionTable)>(
                GetProcAddress(hKernel32, "RtlAddFunctionTable"));
        }

        // ====================================================================
        // 7. Map Data'yi Hedef Surece Yaz
        // ====================================================================
        BYTE* pMapDataRemote = reinterpret_cast<BYTE*>(
            VirtualAllocEx(hProcess, nullptr, sizeof(MANUAL_MAP_DATA),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        if (!pMapDataRemote) {
            std::cerr << "  [-] Map data icin bellek tahsis edilemedi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        if (!WriteProcessMemory(hProcess, pMapDataRemote, &mapData, sizeof(MANUAL_MAP_DATA), nullptr)) {
            std::cerr << "  [-] Map data yazilamadi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pMapDataRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        // ====================================================================
        // 8. Shellcode'u Hedef Surece Yaz
        // ====================================================================
        // Shellcode boyutunu hesapla (fonksiyon pointer farki)
        // Guvenlik: eger hesaplama hatali sonuc verirse sabit boyut kullan
        SIZE_T shellcodeSize = reinterpret_cast<BYTE*>(ShellcodeStub) - reinterpret_cast<BYTE*>(Shellcode);
        if (shellcodeSize == 0 || shellcodeSize > 0x10000) {
            shellcodeSize = 0x1000; // 4KB guvenli fallback
        }

        BYTE* pShellcodeRemote = reinterpret_cast<BYTE*>(
            VirtualAllocEx(hProcess, nullptr, shellcodeSize,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        if (!pShellcodeRemote) {
            std::cerr << "  [-] Shellcode icin bellek tahsis edilemedi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pMapDataRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        if (!WriteProcessMemory(hProcess, pShellcodeRemote, Shellcode, shellcodeSize, nullptr)) {
            std::cerr << "  [-] Shellcode yazilamadi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pShellcodeRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pMapDataRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        std::cout << "  [+] Shellcode yazildi (" << shellcodeSize << " bytes)" << std::endl;

        // ====================================================================
        // 9. Shellcode'u Hedef Surece Calistir (Remote Thread)
        // ====================================================================
        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(pShellcodeRemote),
            pMapDataRemote, 0, nullptr);

        if (!hThread) {
            std::cerr << "  [-] Remote thread olusturulamadi. Hata: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, pShellcodeRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pMapDataRemote, 0, MEM_RELEASE);
            VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
            return false;
        }

        std::cout << "  [+] Remote thread olusturuldu, shellcode calisiyor..." << std::endl;

        // Shellcode tamamlanana kadar bekle
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);

        // ====================================================================
        // 10. Sonucu Kontrol Et
        // ====================================================================
        MANUAL_MAP_DATA resultData{};
        ReadProcessMemory(hProcess, pMapDataRemote, &resultData, sizeof(MANUAL_MAP_DATA), nullptr);

        // Shellcode ve map data bellegini temizle (artik gerekli degil)
        VirtualFreeEx(hProcess, pShellcodeRemote, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, pMapDataRemote, 0, MEM_RELEASE);

        // ====================================================================
        // 11. PE Basliklarini Sil (Anti-Detection)
        // IPTAL EDILDI: raven.dll icindeki ResourceExtractor ve diger 
        // internal fonksiyonlarin PE basligina ihtiyaci olabilir!
        // ====================================================================
        /*
        BYTE zeroBuffer[0x1000]{};
        DWORD headerSize = pOpt->SizeOfHeaders;
        if (headerSize > sizeof(zeroBuffer)) {
            headerSize = sizeof(zeroBuffer);
        }
        WriteProcessMemory(hProcess, pTargetBase, zeroBuffer, headerSize, nullptr);
        */

        if (!resultData.bSuccess) {
            std::cerr << "  [-] Shellcode basarisiz oldu! DllMain hata dondurmus olabilir." << std::endl;
            return false;
        }

        std::cout << "  [+] Manual mapping tamamlandi!" << std::endl;
        return true;
    }

}
