#pragma once
#include <cstdint>

// ----------------------------------------------------------------------------
// Convar system accessor — pattern-scans the engine's CCVar interface and the
// CCVar::FindConVar method so we can fetch live engine convar values
// (sv_gravity, sv_jump_impulse, weapon_air_spread_scale ...) without ever
// hard-coding their static addresses.
//
// All public functions are safe to call before initialization — they return
// the supplied fallback value if the convar system did not resolve.
// ----------------------------------------------------------------------------
namespace Memory::Convars
{
    // Called once during startup, after Memory::Globals::Initialize().
    // Pattern-scans tier0.dll / engine2.dll for the convar interface and the
    // FindConVar function. Safe to call multiple times — idempotent.
    bool Initialize();

    // True if both g_pCVar and the FindConVar function were resolved.
    bool IsReady();

    // Returns the convar's current value, or `fallback` if the convar system
    // did not initialize / the name is unknown.
    float GetFloat(const char* name, float fallback);
    int   GetInt  (const char* name, int   fallback);
    bool  GetBool (const char* name, bool  fallback);

    // Returns the raw ConVarData pointer (CS2's internal convar struct), or
    // nullptr if not found. Useful when you need to read a value that lives
    // at a non-standard offset inside the struct.
    void* FindRaw (const char* name);

    // ------------------------------------------------------------------
    //  Setters
    //
    //  Write the supplied value into ConVarData + 0x40 (slot 0). All
    //  setters silently no-op if the convar is unknown or the convar
    //  system did not initialize. SEH-protected against layout drift.
    //
    //  Each setter ALSO calls StripRestrictiveFlags() on the convar
    //  first so cheat-protected / dev-only convars (e.g.
    //  cl_grenadethrowing_trajectory_time) can be written from a
    //  non-cheats server.
    // ------------------------------------------------------------------
    bool SetFloat(const char* name, float value);
    bool SetInt  (const char* name, int   value);
    bool SetBool (const char* name, bool  value);

    // Clears FCVAR_CHEAT / FCVAR_DEVELOPMENTONLY / FCVAR_HIDDEN /
    // FCVAR_PROTECTED bits at ConVarData + 0x10 so the engine accepts
    // writes from outside a cheats-enabled server. Returns true if the
    // convar was found and flags were modified.
    bool StripRestrictiveFlags(const char* name);
}
