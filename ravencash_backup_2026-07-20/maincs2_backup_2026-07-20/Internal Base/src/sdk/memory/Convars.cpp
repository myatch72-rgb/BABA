#include "Convars.h"
#include "PatternScan.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>

// ============================================================================
//  CS2 ConVar resolver — V2 (modern interface-based)
//
//  Background
//  ----------
//  The original V1 resolver pattern-scanned for "FindConVar" and called it as
//      void* (__fastcall*)(void* this, const char* name);
//  expecting a raw `ConVarData*` back. This was Source-1 era and is COMPLETELY
//  WRONG for modern CS2 — confirmed by diagnostic log:
//
//      [sanity] sv_gravity : FindRaw=0000000000000000  GetFloat=-12345.000
//      [sanity] sv_cheats  : FindRaw=0000000000000000  GetFloat=-12345.000
//
//  Both `sv_gravity` and `sv_cheats` exist in literally every CS2 build but
//  the V1 resolver returned nullptr. Every GrenadePrediction "live gravity
//  read" had been silently using the 800.0 fallback.
//
//  Modern CS2 ICvar (tier0.dll, exported as `VEngineCvar007`)
//  ---------------------------------------------------------
//  The interface is acquired via the standard Source CreateInterface export
//  rather than by pattern scan — far less fragile across patches.
//
//      auto fn = (CreateInterfaceFn)GetProcAddress(tier0, "CreateInterface");
//      void* iCvar = fn("VEngineCvar007", nullptr);
//
//  FindConVar no longer returns a raw data pointer. It returns a 16-bit
//  HANDLE that has to be dereferenced via a separate `GetConVar(handle)`
//  vtable call:
//
//      virtual ConVarHandle FindConVar(const char* name, bool unk);   // ~slot 13
//      virtual CConVarData* GetConVar (ConVarHandle handle);          // ~slot 3
//
//  Vtable slot indexes vary subtly across CS2 patches, so we probe a small
//  range and pick the slot that produces a non-zero, sane handle for the
//  guaranteed-to-exist `sv_gravity` convar. Whichever combination works is
//  cached for the rest of the session.
//
//  Everything is heavily SEH-bracketed and logs to
//      %APPDATA%\RavenCash\convar_resolver.log
//  so we can iterate from the log alone if a future CS2 patch shifts the
//  vtable layout.
// ============================================================================

namespace
{
    // ----------------------------------------------------------------------
    //  Logging
    // ----------------------------------------------------------------------
    void WriteLog(const char* line)
    {
        char appdata[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) return;
        std::filesystem::path dir = std::filesystem::path(appdata) / "RavenCash";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        FILE* f = nullptr;
        fopen_s(&f, (dir / "convar_resolver.log").string().c_str(), "a");
        if (!f) return;
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }

    // ----------------------------------------------------------------------
    //  Interface & vtable state
    // ----------------------------------------------------------------------
    using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

    std::atomic<bool> g_initialized{ false };
    std::atomic<bool> g_initOk{ false };
    void*             g_pCVarInterface = nullptr;
    void**            g_pCVarVTable    = nullptr;

    // Resolved slot indexes (set during sanity probing).
    int               g_slotFindConVar = -1;
    int               g_slotGetConVar  = -1;

    // ----------------------------------------------------------------------
    //  ConVar layout (verified against modern cs2-sdk + nerv-internal)
    //
    //  CConVarData layout in CS2 2024/2025:
    //      +0x00  const char*       m_pszName
    //      +0x08  const char*       m_pszDescription   (or vtable+desc)
    //      +0x10  EConVarType       m_eType            (uint16)
    //      +0x12  uint16            m_nTimesChanged
    //      +0x18  uint32            m_nFlags           <-- FCVAR_CHEAT etc.
    //      +0x1C  uint32            m_nCallbackIdx
    //      +0x20-0x38              default / min / max
    //      +0x40  union value (float / int / bool / ...)
    //
    //  (The +0x40 value offset was verifiable against the original code path
    //  succeeding for users who *had* a working resolver; flags at +0x18
    //  matches modern source dumps.)
    // ----------------------------------------------------------------------
    constexpr ptrdiff_t kFlagsOffset = 0x18;
    constexpr ptrdiff_t kValueOffset = 0x40;
    constexpr uint32_t  kRestrictiveFlagsMask =
          (1u <<  1)    // DEVELOPMENTONLY
        | (1u <<  4)    // HIDDEN
        | (1u <<  5)    // PROTECTED
        | (1u <<  6)    // SPONLY
        | (1u << 14)    // CHEAT
        | (1u << 19)    // RELEASE
        | (1u << 22);   // PER_USER

    // ----------------------------------------------------------------------
    //  Modern signature wrappers
    //
    //  FindConVar's modern prototype packs the return into RAX as a small
    //  integer "handle". We model that as uint64 and inspect its low bits.
    //  Note: passing the third `bool` argument is harmless on builds that
    //  only take two args — x64 calling convention will just ignore R8.
    // ----------------------------------------------------------------------
    using FindConVar_t  = uint64_t (*)(void* thisPtr, const char* name, bool unk);
    using GetConVar_t   = void*    (*)(void* thisPtr, uint64_t handle);

    // ----------------------------------------------------------------------
    //  Probe — call each candidate slot with sv_gravity to see which slot is
    //  the real FindConVar.
    //
    //  Strategy: a real FindConVar for sv_gravity returns a SMALL, non-zero
    //  handle (sv_gravity registers early so its handle index is tiny — < a
    //  few hundred). A non-FindConVar slot will typically AV (caught by
    //  SEH) or return either nullptr or a huge garbage value.
    // ----------------------------------------------------------------------
    uint64_t SafeCallFindConVar(void* iface, void* fn, const char* name)
    {
        if (!iface || !fn) return UINT64_MAX;
        __try {
            auto p = reinterpret_cast<FindConVar_t>(fn);
            return p(iface, name, false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return UINT64_MAX;
        }
    }

    void* SafeCallGetConVar(void* iface, void* fn, uint64_t handle)
    {
        if (!iface || !fn) return nullptr;
        __try {
            auto p = reinterpret_cast<GetConVar_t>(fn);
            return p(iface, handle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Look like a plausible registered-convar handle? Real handles in CS2
    // are small unsigned integers (a few thousand at most), and the high
    // bits encode a type tag — but the FULL uint64 is never an arbitrary
    // user-space pointer like 0x00007FF8xxxxxxxx.
    bool LooksLikeHandle(uint64_t v)
    {
        if (v == 0 || v == UINT64_MAX) return false;
        // Reject anything that looks like a pointer (high 32 bits set).
        if ((v >> 32) > 0xFFFF) return false;
        // Reject values too large to be a sensible registry index.
        if ((v & 0xFFFF) > 0xFF00) return false;
        return true;
    }

    // Resolved name-offset (where m_pszName lives inside the ConVarData
    // struct). Modern CS2 builds have varied between +0x00 (legacy
    // layout) and +0x08 (post-2024 layout, vtable lives at +0x00). We
    // detect this at probe time and stash it here.
    ptrdiff_t g_nameOffset = -1;

    // Tests whether `p` looks like a ConVarData by checking each candidate
    // name offset and seeing whether the string at that offset starts with
    // a sensible ASCII letter/underscore. Returns the matching offset on
    // success, -1 on failure.
    ptrdiff_t DetectNameOffset(void* p, const char* expectedPrefix = nullptr)
    {
        if (!p) return -1;
        if (reinterpret_cast<uintptr_t>(p) < 0x10000) return -1;

        // Candidate offsets — most common first.
        static const ptrdiff_t kCandidates[] = { 0x00, 0x08, 0x10, 0x18 };
        for (ptrdiff_t off : kCandidates) {
            __try {
                const char* name = *reinterpret_cast<const char**>(
                    reinterpret_cast<uint8_t*>(p) + off);
                if (!name) continue;
                // Pointer should look like a valid string pointer.
                if (reinterpret_cast<uintptr_t>(name) < 0x10000) continue;
                char first = name[0];
                bool looksOk =
                    (first >= 'a' && first <= 'z') ||
                    (first >= 'A' && first <= 'Z') ||
                     first == '_';
                if (!looksOk) continue;

                // If caller gave us an expected prefix (e.g. "sv_"), match it.
                if (expectedPrefix) {
                    size_t plen = 0;
                    while (expectedPrefix[plen]) ++plen;
                    bool match = true;
                    for (size_t k = 0; k < plen; ++k) {
                        if (name[k] != expectedPrefix[k]) { match = false; break; }
                    }
                    if (!match) continue;
                }
                return off;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
        return -1;
    }

    bool LooksLikeConVarData(void* p)
    {
        if (g_nameOffset < 0) {
            // Not yet probed — discover the layout on the fly.
            g_nameOffset = DetectNameOffset(p);
        }
        return g_nameOffset >= 0;
    }

    // Dump the first 64 bytes of the returned pointer so a future layout
    // change is obvious from the log alone.
    void DumpFirstBytes(void* p)
    {
        if (!p) return;
        __try {
            char buf[300];
            char* cur = buf;
            size_t rem = sizeof(buf);
            int n = _snprintf_s(cur, rem, _TRUNCATE,
                                "  raw bytes @ 0x%016llX:",
                                (unsigned long long)p);
            cur += n; rem -= n;
            uint8_t* b = reinterpret_cast<uint8_t*>(p);
            for (int i = 0; i < 64 && rem > 4; ++i) {
                if (i % 8 == 0) {
                    int m = _snprintf_s(cur, rem, _TRUNCATE, "\n    %02X:", i);
                    cur += m; rem -= m;
                }
                int m = _snprintf_s(cur, rem, _TRUNCATE, " %02X", b[i]);
                cur += m; rem -= m;
            }
            WriteLog(buf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            WriteLog("  raw bytes dump AVed");
        }
    }

    // SAFE vtable inspection.
    //
    //  Earlier we tried to BRUTE-FORCE-probe every vtable slot by calling it
    //  with arbitrary args ("sv_gravity"). That crashed CS2 on inject —
    //  slots 1/2 are RegisterConVar/UnregisterConVar and corrupted the
    //  registry the moment they ran with garbage parameters, even though
    //  the immediate call didn't AV.
    //
    //  New strategy: NEVER blindly call a vtable slot. We only call slots
    //  that match what cs2-sdk documents for the current CS2 era:
    //
    //      slot  3 : GetConVar  (ConVarHandle handle) -> CConVarData*
    //      slot 13 : FindConVar (const char* name, bool unk) -> ConVarHandle
    //
    //  These are the well-known slot indexes for modern CS2's ICvar
    //  (VEngineCvar007). We only call slot 13 once with "sv_gravity"
    //  during init to verify the layout still holds; if that single test
    //  call returns a handle that GetConVar (slot 3) can dereference back
    //  to a CConVarData whose name starts with "sv_", we commit. Otherwise
    //  the resolver stays disabled — IsReady() returns false and every
    //  consumer falls back to its hard-coded default. Safe by default.
    //
    //  We still DUMP the first 20 vtable slot addresses to the log so that
    //  if Valve renumbers them in a future patch we can spot the shift
    //  without re-injecting.
    void ProbeVTable()
    {
        if (!g_pCVarInterface || !g_pCVarVTable) return;

        char buf[256];
        WriteLog("===== ICvar vtable probe (passive) =====");
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "interface=0x%016llX  vtable=0x%016llX",
                    (unsigned long long)g_pCVarInterface,
                    (unsigned long long)g_pCVarVTable);
        WriteLog(buf);

        // Passive dump only — never call these.
        for (int i = 0; i < 20; ++i) {
            void* fn = nullptr;
            __try { fn = g_pCVarVTable[i]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { fn = nullptr; }
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "  vtable[%2d] = 0x%016llX", i,
                        (unsigned long long)fn);
            WriteLog(buf);
        }

        // Single SAFE verification — slot 13 (FindConVar) + slot 3 (GetConVar)
        // with the canonical universally-existing convar "sv_gravity".
        constexpr int kFindConVarSlot = 13;
        constexpr int kGetConVarSlot  = 3;

        void* findFn = nullptr;
        void* getFn  = nullptr;
        __try {
            findFn = g_pCVarVTable[kFindConVarSlot];
            getFn  = g_pCVarVTable[kGetConVarSlot];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (!findFn || !getFn) {
            WriteLog("[fail] expected slots 13/3 are null — vtable layout changed");
            return;
        }

        uint64_t h = SafeCallFindConVar(g_pCVarInterface, findFn, "sv_gravity");
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  test FindConVar(slot 13, \"sv_gravity\") -> 0x%016llX",
                    (unsigned long long)h);
        WriteLog(buf);
        if (h == 0 || h == UINT64_MAX) {
            WriteLog("[fail] slot 13 returned nothing — abandon resolver");
            return;
        }

        // In this CS2 build slot 13 returns a raw `CConVarData*` directly,
        // NOT an opaque ConVarHandle. Dump the first 64 bytes so we can
        // SEE the layout in the log file.
        void* data = reinterpret_cast<void*>(h);
        DumpFirstBytes(data);

        // Probe candidate name-offsets against the known string "sv_".
        ptrdiff_t off = DetectNameOffset(data, "sv_");
        if (off >= 0) {
            g_nameOffset = off;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "  direct ConVarData* OK — name at +0x%02X starts with \"sv_\"",
                (int)off);
            WriteLog(buf);
            g_slotGetConVar = -2;   // direct mode — no GetConVar dereference
        } else {
            // Old-style: FindConVar returned a handle, dereference via slot 3.
            data = SafeCallGetConVar(g_pCVarInterface, getFn, h);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "  fallback GetConVar(slot 3, handle) -> 0x%016llX",
                (unsigned long long)data);
            WriteLog(buf);
            DumpFirstBytes(data);

            ptrdiff_t off2 = DetectNameOffset(data, "sv_");
            if (off2 < 0) {
                WriteLog("[fail] neither direct nor handle path produced "
                         "a ConVarData with a recognizable sv_-prefix name");
                return;
            }
            g_nameOffset = off2;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "  handle-path OK — name at +0x%02X starts with \"sv_\"",
                (int)off2);
            WriteLog(buf);
        }

        // Log the resolved name + a float read to confirm the layout.
        const char* name = "?";
        float       v40  = -999.f;
        __try {
            name = *reinterpret_cast<const char**>(
                reinterpret_cast<uint8_t*>(data) + g_nameOffset);
            v40  = *reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(data) + kValueOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  resolved sv_gravity: name=\"%s\"  val@40=%.3f",
                    name, v40);
        WriteLog(buf);

        g_slotFindConVar = kFindConVarSlot;
        g_slotGetConVar  = kGetConVarSlot;
        WriteLog("==> resolver READY using slots 13/3");
    }
}

// ============================================================================
//  Public API
// ============================================================================

bool Memory::Convars::Initialize()
{
    if (g_initialized.load()) return g_initOk.load();
    g_initialized.store(true);

    WriteLog("===== Memory::Convars::Initialize() V2 =====");

    // 1. Get tier0.dll's CreateInterface export.
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0) {
        WriteLog("[fail] tier0.dll not loaded yet");
        g_initOk.store(false);
        return false;
    }
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(
        GetProcAddress(tier0, "CreateInterface"));
    if (!createInterface) {
        WriteLog("[fail] tier0.dll!CreateInterface export missing");
        g_initOk.store(false);
        return false;
    }

    // 2. Acquire VEngineCvar007.
    int returnCode = 0;
    g_pCVarInterface = createInterface("VEngineCvar007", &returnCode);
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[%s]  CreateInterface(VEngineCvar007) -> 0x%016llX  rc=%d",
                g_pCVarInterface ? "ok" : "fail",
                (unsigned long long)g_pCVarInterface, returnCode);
    WriteLog(buf);
    if (!g_pCVarInterface) {
        g_initOk.store(false);
        return false;
    }

    // 3. Read the vtable.
    __try {
        g_pCVarVTable = *reinterpret_cast<void***>(g_pCVarInterface);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pCVarVTable = nullptr;
    }
    if (!g_pCVarVTable) {
        WriteLog("[fail] vtable read AVed");
        g_initOk.store(false);
        return false;
    }

    // 4. Probe slots to figure out FindConVar / GetConVar.
    ProbeVTable();
    const bool ok = (g_slotFindConVar >= 0 && g_slotGetConVar >= 0);
    g_initOk.store(ok);
    WriteLog(ok ? "===== convar system READY ====="
                : "===== convar system DEGRADED — probe failed =====");
    return ok;
}

bool Memory::Convars::IsReady()
{
    return g_initOk.load();
}

void* Memory::Convars::FindRaw(const char* name)
{
    if (!g_initOk.load() || !name) return nullptr;
    if (!g_pCVarInterface || !g_pCVarVTable) return nullptr;
    if (g_slotFindConVar < 0) return nullptr;

    __try {
        void* findFn = g_pCVarVTable[g_slotFindConVar];
        uint64_t h = SafeCallFindConVar(g_pCVarInterface, findFn, name);
        if (h == 0 || h == UINT64_MAX) return nullptr;

        // g_slotGetConVar = -2 is the sentinel for "FindConVar already
        // returns ConVarData* directly in this build, no dereference call
        // needed" (set in ProbeVTable() after a positive direct-pointer test).
        if (g_slotGetConVar == -2) {
            return reinterpret_cast<void*>(h);
        }

        if (g_slotGetConVar < 0) return nullptr;
        void* getFn = g_pCVarVTable[g_slotGetConVar];
        return SafeCallGetConVar(g_pCVarInterface, getFn, h);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

float Memory::Convars::GetFloat(const char* name, float fallback)
{
    void* cv = FindRaw(name);
    if (!cv) return fallback;
    __try {
        return *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

int Memory::Convars::GetInt(const char* name, int fallback)
{
    void* cv = FindRaw(name);
    if (!cv) return fallback;
    __try {
        return *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

bool Memory::Convars::GetBool(const char* name, bool fallback)
{
    void* cv = FindRaw(name);
    if (!cv) return fallback;
    __try {
        return *reinterpret_cast<bool*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

bool Memory::Convars::StripRestrictiveFlags(const char* name)
{
    void* cv = FindRaw(name);
    if (!cv) return false;
    __try {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kFlagsOffset);
        *pFlags &= ~kRestrictiveFlagsMask;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Memory::Convars::SetFloat(const char* name, float value)
{
    void* cv = FindRaw(name);
    if (!cv) return false;
    __try {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kFlagsOffset);
        *pFlags &= ~kRestrictiveFlagsMask;

        float* pVal = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
        *pVal = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Memory::Convars::SetInt(const char* name, int value)
{
    void* cv = FindRaw(name);
    if (!cv) return false;
    __try {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kFlagsOffset);
        *pFlags &= ~kRestrictiveFlagsMask;

        int32_t* pVal = reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
        *pVal = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Memory::Convars::SetBool(const char* name, bool value)
{
    void* cv = FindRaw(name);
    if (!cv) return false;
    __try {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(cv) + kFlagsOffset);
        *pFlags &= ~kRestrictiveFlagsMask;

        bool* pVal = reinterpret_cast<bool*>(
            reinterpret_cast<uint8_t*>(cv) + kValueOffset);
        *pVal = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
