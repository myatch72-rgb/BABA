#include "SilentAimHook.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../../ext/minhook/MinHook.h"

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "Psapi.lib")

// ---- Log ----------------------------------------------------------------
static void SLog(const char* m)
{
    char appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) return;
    char path[MAX_PATH] = {};
    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\RavenCash\\silentaimhook.log", appdata);

    HANDLE h = CreateFileA(path,
        FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, m, (DWORD)lstrlenA(m), &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

// ---- Pattern scanning ---------------------------------------------------
struct SPat { uint8_t b[64]; bool wild[64]; size_t len; };

static bool SParsePattern(const char* s, SPat& out)
{
    out.len = 0;
    const char* p = s;
    while (*p && out.len < 64) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?' && (p[1] == '?' || p[1] == ' ' || p[1] == 0)) {
            out.wild[out.len] = true;
            out.b[out.len] = 0;
            out.len++; p += (p[1] == '?' ? 2 : 1);
        } else {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(p[0]), lo = hex(p[1]);
            if (hi < 0 || lo < 0) return false;
            out.wild[out.len] = false;
            out.b[out.len] = (uint8_t)((hi << 4) | lo);
            out.len++; p += 2;
        }
    }
    return out.len > 0;
}

static int SScanAll(const wchar_t* mod, const char* patStr,
                    uintptr_t* out, int maxHits)
{
    HMODULE h = GetModuleHandleW(mod);
    if (!h) return 0;
    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return 0;
    SPat pat;
    if (!SParsePattern(patStr, pat)) return 0;
    auto* base = (uint8_t*)mi.lpBaseOfDll;
    if (mi.SizeOfImage < pat.len) return 0;
    size_t span = mi.SizeOfImage - pat.len;
    int hits = 0;
    __try {
        for (size_t i = 0; i < span && hits < maxHits; i++) {
            bool ok = true;
            for (size_t j = 0; j < pat.len; j++) {
                if (pat.wild[j]) continue;
                if (base[i + j] != pat.b[j]) { ok = false; break; }
            }
            if (ok) out[hits++] = (uintptr_t)(base + i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return hits;
}

// ---- GetInaccuracy direct hook (cspatterns.dev) -----------------------
// pattern: "48 89 5C 24 ? 55 56 57 48 81 EC ? ? ? ? 44 0F 29 84 24"
// ---- Target state -------------------------------------------------------
static std::atomic_bool g_localValid{false};
static std::atomic_bool g_noSpreadActive{false};

// Anti-Aim specific state
static std::atomic_bool g_antiAimActive{false};
static std::atomic<float> g_antiAimFakePitch{0.f};
static std::atomic<float> g_antiAimFakeYaw{0.f};
static std::atomic<float> g_localPitch{0.f};
static std::atomic<float> g_localYaw{0.f};

static bool WantsNoSpread()
{
    return g_localValid.load(std::memory_order_relaxed) &&
           g_noSpreadActive.load(std::memory_order_relaxed) &&
           Globals::silent_aim_enabled &&
           Globals::silent_aim_nospread;
}

static bool WantsAntiAim()
{
    return g_localValid.load(std::memory_order_relaxed) &&
           g_antiAimActive.load(std::memory_order_relaxed) &&
           Globals::antiaim_enabled;
}

namespace InacDirect {
    static uintptr_t g_addr  = 0;
    typedef float (__fastcall* Fn)(void*, float*, float*);
    static Fn g_oFn = nullptr;
    static float __fastcall hk(void* weapon, float* movementOut, float* airOut) {
        if (WantsNoSpread()) {
            __try {
                if (movementOut) *movementOut = 0.0f;
                if (airOut) *airOut = 0.0f;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0.0f;
        }
        return g_oFn ? g_oFn(weapon, movementOut, airOut) : 0.0f;
    }
}

// ---- GetSpread direct hook (cspatterns.dev) ----------------------------
// pattern: "48 83 EC ? 48 63 91"
namespace SpreadDirect {
    static uintptr_t g_addr  = 0;
    typedef float (__fastcall* Fn)(void*);
    static Fn g_oFn = nullptr;
    static float __fastcall hk(void* a1) { 
        if (WantsNoSpread()) return 0.0f;
        return g_oFn ? g_oFn(a1) : 0.0f;
    }
}

// ---- CalcSpread final cone hook ----------------------------------------
// Current ABI:
// weaponDef, bulletCount, scoped, seed, inaccuracy, spread, recoilIndex,
// spreadXOut, spreadYOut.
namespace CalcSpreadDirect {
    static uintptr_t g_addr = 0;
    typedef void (__fastcall* Fn)(
        std::int16_t, int, int, std::uint32_t,
        float, float, float, float*, float*);
    static Fn g_oFn = nullptr;

    static void __fastcall hk(
        std::int16_t weaponDef, int bulletCount, int scoped,
        std::uint32_t seed, float inaccuracy, float spread, float recoilIndex,
        float* spreadXOut, float* spreadYOut)
    {
        // Preserve native bookkeeping first, then clamp the final cone only
        // for the currently armed Silent-Aim command.
        if (g_oFn) {
            g_oFn(weaponDef, bulletCount, scoped, seed, inaccuracy, spread,
                  recoilIndex, spreadXOut, spreadYOut);
        }

        if (!WantsNoSpread())
            return;

        __try {
            if (spreadXOut) *spreadXOut = 0.0f;
            if (spreadYOut) *spreadYOut = 0.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- ComputeRandomSeed hook (server-validated no-spread) ---------------
namespace SeedHook {
    static uintptr_t g_addr    = 0;
    typedef uint32_t (__fastcall* Fn)(void*, void*, int);
    static Fn g_oFn = nullptr;

    static uint32_t __fastcall hk(void* a1, void* a2, int a3) {
        if (WantsNoSpread()) return 0;
        return g_oFn ? g_oFn(a1, a2, a3) : 0;
    }
}

static bool DirectHookSetup(const char* pat, uintptr_t& addr, void* hookFn, void** origOut, const char* tag)
{
    if (addr) return true;
    HMODULE h = GetModuleHandleW(L"client.dll");
    if (!h) return false;
    uintptr_t hits[8] = {};
    int n = SScanAll(L"client.dll", pat, hits, 8);
    if (n == 0) {
        char b[120];
        snprintf(b, sizeof(b), "[%s] pattern NOT found", tag);
        SLog(b);
        return false;
    }
    if (n != 1) {
        char b[120];
        snprintf(b, sizeof(b), "[%s] ambiguous pattern (hits=%d), refused",
                 tag, n);
        SLog(b);
        return false;
    }
    addr = hits[0];
    char b[160];
    snprintf(b, sizeof(b), "[%s] fn @ %p rva=0x%llX (hits=%d)",
             tag, (void*)addr,
             static_cast<unsigned long long>(
                 addr - reinterpret_cast<uintptr_t>(h)),
             n);
    SLog(b);
    const MH_STATUS status = MH_CreateHook((void*)addr, hookFn, origOut);
    if (status != MH_OK) {
        snprintf(b, sizeof(b), "[%s] MH_CreateHook failed status=%d",
                 tag, static_cast<int>(status));
        SLog(b);
        addr = 0;
        return false;
    }
    return true;
}

static void DirectHookApply(uintptr_t addr, const char* tag) {
    if (addr) {
        const MH_STATUS status = MH_EnableHook((void*)addr);
        char b[140];
        snprintf(b, sizeof(b), "[%s] MinHook enable status=%d @ %p",
                 tag, static_cast<int>(status), (void*)addr);
        SLog(b);
    }
}

static void DirectHookRevert(uintptr_t addr, const char* tag) {
    if (addr) {
        const MH_STATUS status = MH_DisableHook((void*)addr);
        char b[120];
        snprintf(b, sizeof(b), "[%s] MinHook disable status=%d",
                 tag, static_cast<int>(status));
        SLog(b);
    }
}

// ---- CMsgQAngleCpy Hook -------------------------------------------------
namespace QAngleCpyHook {
    static uintptr_t g_addr = 0;
    typedef void (__fastcall* Fn)(uintptr_t, void*);
    static Fn g_oFn = nullptr;
    static volatile bool g_fatal = false;
    static volatile bool g_writeFatal = false;

    static bool IsReadableWritable(uintptr_t p, size_t need)
    {
        if (p < 0x10000ULL || p > 0x7FFFFFFFFFFFULL) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
        if (mbi.Protect & bad) return false;
        DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & ok)) return false;
        if (((uintptr_t)mbi.BaseAddress + mbi.RegionSize) < p + need) return false;
        return true;
    }

    static void __fastcall hk(uintptr_t a1, void* a2)
    {
        if (g_fatal || !g_oFn) {
            if (g_oFn) g_oFn(a1, a2);
            return;
        }

        const bool wantAnti = WantsAntiAim();

        if (!g_writeFatal && a2 && wantAnti) {
            __try {
                if (IsReadableWritable((uintptr_t)a2 + 0x18, 12)) {
                    float* v = (float*)((uintptr_t)a2 + 0x18);
                    
                    // Sahte açıları yazıyoruz
                    v[0] = g_antiAimFakePitch.load(std::memory_order_relaxed);
                    v[1] = g_antiAimFakeYaw.load(std::memory_order_relaxed);
                    v[2] = 0.0f;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_writeFatal = true;
                SLog("[QAngleCpy] write SEH'd — disabled for session");
            }
        }

        __try { g_oFn(a1, a2); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_fatal = true;
            SLog("[QAngleCpy] original SEH'd — fatal");
        }
    }
}

// ---- Target state functions ---------------------------------------------

void SilentAimHook::SetLocalValid(bool v) {
    g_localValid.store(v, std::memory_order_relaxed);
}

void SilentAimHook::SetLocalView(float pitch, float yaw) {
    g_localPitch.store(pitch, std::memory_order_relaxed);
    g_localYaw.store(yaw, std::memory_order_relaxed);
}

void SilentAimHook::SetAntiAimTarget(float fakePitch, float fakeYaw) {
    g_antiAimFakePitch.store(fakePitch, std::memory_order_relaxed);
    g_antiAimFakeYaw.store(fakeYaw, std::memory_order_relaxed);
}

void SilentAimHook::SetAntiAimActive(bool active) {
    g_antiAimActive.store(active, std::memory_order_relaxed);
}

void SilentAimHook::SetNoSpreadActive(bool active) {
    g_noSpreadActive.store(active, std::memory_order_relaxed);
}

static bool g_currentWants = false;
static bool g_wantsInitialized = false;

void SilentAimHook::SetWantsHook(bool wants)
{
    if (g_wantsInitialized && wants == g_currentWants) return;
    g_currentWants = wants;
    g_wantsInitialized = true;

    if (wants) {
        DirectHookApply(SeedHook::g_addr, "Seed");
        DirectHookApply(InacDirect::g_addr, "Inac2");
        DirectHookApply(SpreadDirect::g_addr, "Spread");
        DirectHookApply(CalcSpreadDirect::g_addr, "CalcSpread");
        DirectHookApply(QAngleCpyHook::g_addr, "QAngleCpy");
    } else {
        DirectHookRevert(SeedHook::g_addr, "Seed");
        DirectHookRevert(InacDirect::g_addr, "Inac2");
        DirectHookRevert(SpreadDirect::g_addr, "Spread");
        DirectHookRevert(CalcSpreadDirect::g_addr, "CalcSpread");
        DirectHookRevert(QAngleCpyHook::g_addr, "QAngleCpy");
    }
}

// ---- Install / Shutdown -------------------------------------------------
bool SilentAimHook::Install()
{
    SLog("[SAim] Install: four-stage no-spread pipeline");
    bool installedAny = false;

    installedAny |= DirectHookSetup(
        "48 89 5C 24 ? 57 48 81 EC ? ? ? ? F3 0F 10 0A",
        SeedHook::g_addr, (void*)&SeedHook::hk,
        (void**)&SeedHook::g_oFn, "Seed");
    installedAny |= DirectHookSetup(
        "48 89 5C 24 ? 55 56 57 48 81 EC ? ? ? ? 44 0F 29 84 24",
        InacDirect::g_addr, (void*)&InacDirect::hk,
        (void**)&InacDirect::g_oFn, "Inac2");
    installedAny |= DirectHookSetup(
        "48 83 EC ? 48 63 91 ? ? ? ? 48 8B 81 ? ? ? ? 0F 29 74 24",
        SpreadDirect::g_addr, (void*)&SpreadDirect::hk,
        (void**)&SpreadDirect::g_oFn, "Spread");
    installedAny |= DirectHookSetup(
        "48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 63 EA",
        CalcSpreadDirect::g_addr, (void*)&CalcSpreadDirect::hk,
        (void**)&CalcSpreadDirect::g_oFn, "CalcSpread");
        
    // CMsgQAngleCpy from referans projesi.
    installedAny |= DirectHookSetup(
        "E8 ?? ?? ?? ?? 40 F6 C6 ?? 74 ?? 83 0F ?? 48 8B 43",
        QAngleCpyHook::g_addr, (void*)&QAngleCpyHook::hk,
        (void**)&QAngleCpyHook::g_oFn, "QAngleCpy");
        
    // Eğer E8 kalıbı (CALL) geldiyse mutlak adresi hesapla
    if (QAngleCpyHook::g_addr != 0) {
        HMODULE h = GetModuleHandleW(L"client.dll");
        auto* base = (uint8_t*)QAngleCpyHook::g_addr;
        if (base[0] == 0xE8) {
            int32_t rel = *(int32_t*)(base + 1);
            QAngleCpyHook::g_addr = QAngleCpyHook::g_addr + 5 + rel;
            // Yeni adresten Hook'u tekrar oluşturmamız gerekiyor:
            // MinHook oluşturulmuştu ancak yanlış adrese. Temizleyip doğrusuna oluşturalım:
            MH_RemoveHook((void*)base);
            MH_CreateHook((void*)QAngleCpyHook::g_addr, (void*)&QAngleCpyHook::hk, (void**)&QAngleCpyHook::g_oFn);
        }
    }

    return installedAny;
}

void SilentAimHook::Shutdown()
{
    DirectHookRevert(SeedHook::g_addr, "Seed");
    DirectHookRevert(InacDirect::g_addr, "Inac2");
    DirectHookRevert(SpreadDirect::g_addr, "Spread");
    DirectHookRevert(CalcSpreadDirect::g_addr, "CalcSpread");
    DirectHookRevert(QAngleCpyHook::g_addr, "QAngleCpy");
}
