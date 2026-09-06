#include "Globals.h"
#include "Offsets.h"
#include "PatternScan.h"
#include "Patterns.h"

#include <Windows.h>
#include <cstdio>
#include <filesystem>

namespace
{
    void WriteLog(const char* line)
    {
        char appdata[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
            return;
        std::filesystem::path dir = std::filesystem::path(appdata) / "RavenCash";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        FILE* f = nullptr;
        fopen_s(&f, (dir / "offset_resolver.log").string().c_str(), "a");
        if (!f) return;
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }

    void LogOk(const char* name, uintptr_t addr)
    {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[ok]   %s -> 0x%llX", name, (unsigned long long)addr);
        WriteLog(buf);
    }

    void LogFail(const char* name)
    {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[fail] %s : pattern not found", name);
        WriteLog(buf);
    }

    bool ResolvePointer(const char* name, const char* module,
                        const Patterns::Sig& sig, void*& outPtr)
    {
        uintptr_t addr = Memory::ResolveRipRelative(
            module, sig.pattern, sig.dispOffset, sig.instrLen);
        if (!addr) { LogFail(name); return false; }
        outPtr = reinterpret_cast<void*>(addr);
        LogOk(name, addr);
        return true;
    }

    bool ResolveFunction(const char* name, const char* module,
                         const char* pattern, Memory::Globals::GetLocalFn& outFn)
    {
        uintptr_t addr = Memory::PatternScan(module, pattern);
        if (!addr) { LogFail(name); return false; }
        outFn = reinterpret_cast<Memory::Globals::GetLocalFn>(addr);
        LogOk(name, addr);
        return true;
    }
}

uintptr_t Memory::Globals::LocalPawn()
{
    if (!pfnGetLocalPawn) return 0;
    __try {
        return reinterpret_cast<uintptr_t>(pfnGetLocalPawn(-1));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t Memory::Globals::LocalController()
{
    if (!pfnGetLocalController) return 0;
    __try {
        return reinterpret_cast<uintptr_t>(pfnGetLocalController(-1));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

Memory::Globals::Result Memory::Globals::InitializeLocalPlayerAccessors()
{
    WriteLog("===== Memory::Globals::InitializeLocalPlayerAccessors() =====");

    Result result{ 2, 0, 0 };
    if (pfnGetLocalPawn ||
        ResolveFunction("pfnGetLocalPawn", "client.dll",
                        Patterns::Func::GetLocalPawn, pfnGetLocalPawn))
        ++result.resolved;
    else
        ++result.failed;

    if (pfnGetLocalController ||
        ResolveFunction("pfnGetLocalController", "client.dll",
                        Patterns::Func::GetLocalController,
                        pfnGetLocalController))
        ++result.resolved;
    else
        ++result.failed;

    char buffer[160];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "===== local accessors: %d/%d resolved, %d failed =====",
                result.resolved, result.total, result.failed);
    WriteLog(buffer);
    return result;
}

Memory::Globals::Result Memory::Globals::Initialize()
{
    WriteLog("===== Memory::Globals::Initialize() =====");

    Result r{ 0, 0, 0 };

    auto tryPtr = [&](const char* name, const Patterns::Sig& sig, void*& slot) {
        ++r.total;
        if (ResolvePointer(name, "client.dll", sig, slot)) ++r.resolved;
        else                                                ++r.failed;
    };
    auto tryFn = [&](const char* name, const char* pat, GetLocalFn& slot) {
        ++r.total;
        if (ResolveFunction(name, "client.dll", pat, slot)) ++r.resolved;
        else                                                 ++r.failed;
    };

    tryPtr("pGlobalVars",      Patterns::Data::pGlobalVars,      pGlobalVars);
    tryPtr("pEntityList",      Patterns::Data::pEntityList,      pEntityList);
    tryPtr("pViewMatrix",      Patterns::Data::pViewMatrix,      pViewMatrix);
    tryPtr("pViewRender",      Patterns::Data::pViewRender,      pViewRender);
    tryPtr("pGameRules",       Patterns::Data::pGameRules,       pGameRules);
    tryPtr("pPlantedC4",       Patterns::Data::pPlantedC4,       pPlantedC4);
    tryPtr("pGlowManager",     Patterns::Data::pGlowManager,     pGlowManager);
    tryPtr("pLocalController", Patterns::Data::pLocalController, pLocalController);

    // pCSGOInput and pViewAngles intentionally excluded — neither pattern2.txt
    // nor cs2-sdk.com expose RIP-relative signatures that resolve to the same
    // statics cs2-dumper labels dwCSGOInput / dwViewAngles. The patterns I
    // tried match different statics (off by ~0xA00000+). Keeping these two as
    // hardcoded fallbacks in Offsets.h is more reliable than a wrong pattern.

    tryFn("pfnGetLocalPawn",       Patterns::Func::GetLocalPawn,       pfnGetLocalPawn);
    tryFn("pfnGetLocalController", Patterns::Func::GetLocalController, pfnGetLocalController);

    // Sync remaining Offsets::dwX (offsets-from-base) with resolved pointers so
    // existing `client + Offsets::dwX` reads keep working unchanged.
    uintptr_t clientBase = Memory::GetModuleBase("client.dll");
    if (clientBase) {
        auto sync = [&](const char* name, void* p, uintptr_t& slot) {
            if (!p) return;
            slot = reinterpret_cast<uintptr_t>(p) - clientBase;
            char buf[256];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[sync] Offsets::%s = 0x%llX",
                        name, (unsigned long long)slot);
            WriteLog(buf);
        };
        sync("dwGlobalVars",  pGlobalVars,  Offsets::dwGlobalVars);
        sync("dwEntityList",  pEntityList,  Offsets::dwEntityList);
        sync("dwViewMatrix",  pViewMatrix,  Offsets::dwViewMatrix);
        sync("dwViewRender",  pViewRender,  Offsets::dwViewRender);
        sync("dwGameRules",   pGameRules,   Offsets::dwGameRules);
        sync("dwPlantedC4",   pPlantedC4,   Offsets::dwPlantedC4);
        sync("dwGlowManager", pGlowManager, Offsets::dwGlowManager);
        sync("dwLocalPlayerController", pLocalController, Offsets::dwLocalPlayerController);
        // dwCSGOInput, dwViewAngles intentionally NOT synced — they stay as
        // the literal fallback in Offsets.h. See note above.
    }

    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "===== done: %d/%d resolved, %d failed =====",
                r.resolved, r.total, r.failed);
    WriteLog(buf);
    return r;
}
