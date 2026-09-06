#include "AntiAim.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <chrono>
#include <cmath>

#include "../../../sdk/memory/Globals.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/utils/CCSGOInput.h"
#include "../../../sdk/utils/CmdAngles.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/utils/Vector.h"
#include "../../misc/bhop/BhopV2.h"
#include "SilentAimHook.h"

namespace AntiAim {

// ===========================================================================
//  Camera-safe hybrid command path
//
//  The observer-facing base command receives the selected fake angle while
//  every sub-tick history entry retains the real local angle. Nothing writes
//  dwViewAngles, movement floats, movement buttons or pawn velocity here.
// ===========================================================================

static inline float NormalizeYaw(float y) {
    while (y >  180.f) y -= 360.f;
    while (y < -180.f) y += 360.f;
    return y;
}

static inline bool HasButton(c_user_cmd* cmd, uint64_t button) {
    if (!cmd) return false;
    __try {
        if ((cmd->m_button_state & button) != 0) return true;
        CUserCmd* full = reinterpret_cast<CUserCmd*>(cmd);
        CBaseUserCmdPB* base = full->csgoUserCmd.pBaseCmd;
        return base && base->pInButtonState &&
               ((base->pInButtonState->nValue & button) != 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static inline bool IsFiringNow(c_user_cmd* cmd) {
    constexpr uint64_t IN_ATTACK = 1ULL << 0;
    return HasButton(cmd, IN_ATTACK) ||
           ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
}

// Continuous spin uses steady_clock so its rate is decoupled from tick
// rate. Wrapped to [-180,180] via NormalizeYaw on the way out.
static float SpinYawAt(float baseYaw)
{
    static const auto t0 = std::chrono::steady_clock::now();
    const float t = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - t0).count();
    return NormalizeYaw(baseYaw + t * Globals::antiaim_spin_speed);
}

static float ComputeFakeYaw(float realYaw)
{
    switch (Globals::antiaim_yaw_mode)
    {
    default:
    case 1:  // Backwards — flip 180° + user offset
        return NormalizeYaw(realYaw + 180.f + Globals::antiaim_yaw_offset);

    case 2:  // Sideways Left
        return NormalizeYaw(realYaw + 90.f + Globals::antiaim_yaw_offset);

    case 3:  // Sideways Right
        return NormalizeYaw(realYaw - 90.f + Globals::antiaim_yaw_offset);

    case 4: { // Jitter — alternates ± jitter on each tick on top of base
        static bool toggle = false;
        toggle = !toggle;
        const float side = toggle ? 1.f : -1.f;
        return NormalizeYaw(realYaw + 180.f
                             + Globals::antiaim_yaw_offset
                             + side * Globals::antiaim_yaw_jitter);
    }

    case 5:  // Spin — continuous rotation independent of mouse
        return SpinYawAt(realYaw);
    }
}

static float ComputeFakePitch()
{
    switch (Globals::antiaim_pitch_mode)
    {
    case 1: return  89.f;   // Down (looks at ground)
    case 2: return -89.f;   // Up   (looks at sky)
    case 3: return   0.f;   // Zero (horizontal)
    default: return  0.f;   // 0 = Off (caller filters this out)
    }
}

// ===========================================================================
//  Entry — called from hkCreateMove on every tick. Cheap when disabled.
// ===========================================================================
void Run(c_user_cmd* cmd)
{
    // Varsayılan olarak kancayı kapatıyoruz ki, aimbot veya normal atışlarda sahte açılar araya girmesin.
    SilentAimHook::SetAntiAimActive(false);

    if (!cmd) return;
    if (!Globals::antiaim_enabled) return;

    // Never alter interaction commands or view-dependent movement modes.
    // These paths legitimately consume the real command angle locally.
    constexpr uint64_t IN_USE = 1ULL << 5;
    if (HasButton(cmd, IN_USE)) return;

    const uintptr_t pawn = Memory::Globals::LocalPawn();
    if (!pawn) return;
    const uint8_t moveType = Utils::SafeRead<uint8_t>(pawn + Offsets::m_MoveType);
    if (moveType == 7 || moveType == 9) return; // noclip / ladder

    // Key gate — shared convention with Aimbot / Silent Aim.
    if (Globals::antiaim_key != 0 &&
        Globals::antiaim_key > 0 && Globals::antiaim_key < 256)
    {
        if ((GetAsyncKeyState(Globals::antiaim_key) & 0x8000) == 0)
            return;
    }

    // Snapshot REAL angles from the mouse-input global FIRST. Every
    // pin/restore below uses this exact triple, so even if the engine
    // mutates dwViewAngles mid-tick we land back where the user was.
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) return;
    const Vector realAngles =
        Utils::SafeRead<Vector>(client + Offsets::dwViewAngles);
    if (!std::isfinite(realAngles.x) || !std::isfinite(realAngles.y))
        return;

    // While firing we DON'T touch the cmd — real angles ship so bullets
    // land where the crosshair points. Silent Aim, if enabled, drives
    // its own write on the same tick after this short-circuit.
    if (Globals::antiaim_disable_firing && IsFiringNow(cmd))
        return;

    const bool wantYaw   = Globals::antiaim_yaw_mode   != 0;
    const bool wantPitch = Globals::antiaim_pitch_mode != 0;
    if (!wantYaw && !wantPitch) return;

    const float realYaw   = realAngles.y;
    const float fakeYaw   = wantYaw   ? ComputeFakeYaw(realYaw)
                                      : realYaw;
    const float fakePitch = wantPitch ? ComputeFakePitch()
                                      : realAngles.x;

    // Açıları oyun motorunun QAngleCpy ağ paketleme sistemine yolluyoruz.
    // Bu sayede lokal kameramız (pBaseCmd tahmini nedeniyle) etkilenmemiş oluyor.
    SilentAimHook::SetAntiAimTarget(fakePitch, fakeYaw);
    SilentAimHook::SetAntiAimActive(true);

    // Hareket Vektörü Düzeltmesi (Movement Fix):
    // Sahte bir açıya doğru baktığımız için (örneğin geri geri), W/A/S/D tuşlarının 
    // gerçek fare açımıza göre gitmesini sağlamak adına vektörleri tersine döndürüyoruz.
    if (wantYaw) {
        CmdAngles::FixMovement(cmd, realYaw - fakeYaw);
    }
}




} // namespace AntiAim
