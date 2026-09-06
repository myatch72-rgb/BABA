#pragma once
#include <cstdint>

struct Vector;

namespace Memory::Globals
{
    // ------------------------------------------------------------------
    // RIP-relative data globals (client.dll). Set once at startup by
    // Initialize(). Each pointer holds the absolute address that the
    // RIP-relative instruction targets — i.e. what `client.dll + dwX`
    // used to evaluate to.
    // ------------------------------------------------------------------
    inline void* pGlobalVars      = nullptr;
    inline void* pEntityList      = nullptr;
    inline void* pViewMatrix      = nullptr;
    inline void* pViewRender      = nullptr;
    inline void* pViewAngles      = nullptr;
    inline void* pGameRules       = nullptr;
    inline void* pPlantedC4       = nullptr;
    inline void* pGlowManager     = nullptr;
    inline void* pCSGOInput       = nullptr;
    inline void* pLocalController = nullptr;

    // ------------------------------------------------------------------
    // Function-call accessors. No static offset is needed; the pattern
    // resolves the function address and we call it.
    // ------------------------------------------------------------------
    using GetLocalFn = void* (__fastcall*)(int slot);
    inline GetLocalFn pfnGetLocalPawn       = nullptr;
    inline GetLocalFn pfnGetLocalController = nullptr;

    // Convenience wrappers. Return 0 / nullptr if the underlying function
    // pointer didn't resolve.
    uintptr_t LocalPawn();
    uintptr_t LocalController();

    // ------------------------------------------------------------------
    // Resolve all patterns. Safe to call once early in MainThread, before
    // anything reads from these globals.
    // ------------------------------------------------------------------
    struct Result {
        int total;
        int resolved;
        int failed;
    };
    // Inventory/SkinChanger only needs the guarded local-player functions.
    // This narrow initializer avoids resolving and mutating unrelated global
    // offsets when the full runtime profile is disabled.
    Result InitializeLocalPlayerAccessors();
    Result Initialize();
}
