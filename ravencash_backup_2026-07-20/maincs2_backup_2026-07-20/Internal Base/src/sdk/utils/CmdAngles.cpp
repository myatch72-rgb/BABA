#include "CmdAngles.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <cmath>

#include "CCSGOInput.h"   // CUserCmd / CCSGOUserCmdPB / CCSGOInputHistoryEntryPB
#include "Utils.h"

namespace CmdAngles
{
    // Stamp the legacy `pBaseCmd->pViewAngles` slot — kept inside its own
    // SEH-protected helper so the caller can still hold std::vector etc.
    static void WriteBaseCmd(c_user_cmd* cmd, float pitch, float yaw)
    {
        if (!cmd) return;
        __try {
            // The lean c_user_cmd from BhopV2.h shares a base address with
            // the full CUserCmd layout in CCSGOInput.h; reinterpret to
            // gain access to the protobuf chain.
            CUserCmd* full = reinterpret_cast<CUserCmd*>(cmd);
            CBaseUserCmdPB* base = full->csgoUserCmd.pBaseCmd;
            if (!base) return;
            CMsgQAngle* va = base->pViewAngles;
            if (!va) return;
            va->angValue.pitch = pitch;
            va->angValue.yaw   = yaw;
            va->angValue.roll  = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Protobuf layout drift — silently skip rather than crash.
        }
    }

    // Iterate every populated sub-tick history entry and stamp the same
    // angle. Modern CS2 hit-reg samples one of these — having them all
    // identical guarantees the silent correction sticks regardless of
    // which subtick the server picks.
    static void WriteSubticks(c_user_cmd* cmd, float pitch, float yaw)
    {
        if (!cmd) return;
        __try {
            CUserCmd* full = reinterpret_cast<CUserCmd*>(cmd);
            auto& hist = full->csgoUserCmd.inputHistoryField;
            if (!hist.pRep) return;

            // Belt-and-braces clamp — protobuf RepeatedPtrField has been
            // observed reporting absurd sizes after a level transition.
            int count = hist.nCurrentSize;
            if (count <= 0 || count > 64) return;
            int allocated = hist.pRep->nAllocatedSize;
            if (count > allocated) count = allocated;
            if (count > 64) count = 64;

            for (int i = 0; i < count; ++i) {
                CCSGOInputHistoryEntryPB* entry = hist.pRep->tElements[i];
                if (!entry) continue;
                CMsgQAngle* va = entry->pViewAngles;
                if (!va) continue;
                va->angValue.pitch = pitch;
                va->angValue.yaw   = yaw;
                va->angValue.roll  = 0.f;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Same defensive bail.
        }
    }

    void WriteToHistory(c_user_cmd* cmd, float pitch, float yaw)
    {
        if (!cmd) return;
        if (pitch >  89.f) pitch =  89.f;
        if (pitch < -89.f) pitch = -89.f;

        // History-only — pBaseCmd left intact. Server's hit-reg path
        // reads history (so bullets land on bone) but client prediction
        // reads pBaseCmd (so the local camera stays glued to the mouse).
        // This is the "Silent Aim" flavour: invisible to the shooter,
        // bullets still land on target.
        WriteSubticks(cmd, pitch, yaw);
    }

    void WriteFull(c_user_cmd* cmd, float pitch, float yaw)
    {
        if (!cmd) return;
        if (pitch >  89.f) pitch =  89.f;
        if (pitch < -89.f) pitch = -89.f;

        // History + pBaseCmd — needed by Anti-Aim. Without the pBaseCmd
        // write, the server replicates the REAL angles into each
        // observer's copy of our pawn (history-only doesn't propagate
        // to networked m_angEyeAngles), so other players keep seeing us
        // pointed at the crosshair direction — defeating anti-aim.
        //
        // Crucially we still never touch dwViewAngles (the mouse-input
        // global that drives the local first-person camera). The
        // pBaseCmd write does affect client prediction, which updates
        // the LOCAL pawn's m_angEyeAngles in-place — but that field
        // only affects how the player MODEL is rendered, not where the
        // camera looks. In first person the user doesn't see their own
        // model, so the swap is invisible to them while remaining fully
        // visible to opponents in third-person / spectator / demo.
        WriteBaseCmd(cmd, pitch, yaw);
        WriteSubticks(cmd, pitch, yaw);
    }

    void WriteHybrid(c_user_cmd* cmd,
                     float realPitch, float realYaw,
                     float fakePitch, float fakeYaw)
    {
        if (!cmd) return;
        if (realPitch >  89.f) realPitch =  89.f;
        if (realPitch < -89.f) realPitch = -89.f;
        if (fakePitch >  89.f) fakePitch =  89.f;
        if (fakePitch < -89.f) fakePitch = -89.f;

        // pBaseCmd → FAKE.
        //   * The server's pawn-replication path reads pBaseCmd->pViewAngles
        //     and writes it into the networked m_angEyeAngles. That's the
        //     field every OTHER client reads to render our character's
        //     facing direction. Shipping FAKE here is what makes the
        //     anti-aim visibly turn our character to spin / face the
        //     floor from a spectator's POV.
        WriteBaseCmd(cmd, fakePitch, fakeYaw);

        // History → REAL.
        //   * Engine sub-tick movement processing samples view angles out
        //     of the input-history entry covering the subtick. Shipping
        //     REAL means W/A/S/D resolve onto the REAL forward vector
        //     (no more back-walking on Back/Spin modes).
        //   * Engine view-prediction sub-tick code also reads from the
        //     history; shipping REAL stops it from dragging dwViewAngles
        //     onto our fake yaw (no more "arada bir" camera kayma).
        //   * Server lag-comp & hit-reg read history for OUR shots, so
        //     bullets land where the crosshair points.
        WriteSubticks(cmd, realPitch, realYaw);
    }

    // -----------------------------------------------------------------
    //  AutoStop
    //
    //  Issues a 1-tick counter-strafe so the player's velocity hits
    //  zero RIGHT NOW — eliminating CS2's movement-spread component
    //  for the same tick we want to fire. The maths:
    //
    //      forward_basis = ( cos(yaw),  sin(yaw),  0 )
    //      right_basis   = ( sin(yaw), -cos(yaw),  0 )      (CS convention)
    //      velFwd   = velocity · forward_basis
    //      velRight = velocity · right_basis
    //
    //  We then write ±450 (max input) into the opposite move-floats —
    //  CS2's player_move accelerates the player against velocity and
    //  friction brings speed to ~0 the same tick.
    // -----------------------------------------------------------------
    void AutoStop(c_user_cmd* cmd, float velX, float velY, float yawDeg)
    {
        if (!cmd) return;

        const float speed2D = std::sqrt(velX * velX + velY * velY);
        if (speed2D < 1.0f) return;   // already stopped — nothing to do

        const float yawRad = yawDeg * 0.01745329252f;
        const float fwdX   =  std::cos(yawRad);
        const float fwdY   =  std::sin(yawRad);
        const float rgtX   =  std::sin(yawRad);
        const float rgtY   = -std::cos(yawRad);

        const float velFwd   = velX * fwdX + velY * fwdY;
        const float velRight = velX * rgtX + velY * rgtY;

        __try {
            uintptr_t baseCmd = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(cmd) + 0x40);
            if (!baseCmd) return;

            float* pFwd  = reinterpret_cast<float*>(baseCmd + 0x50);
            float* pSide = reinterpret_cast<float*>(baseCmd + 0x54);

            // Sign-only counter input. Magnitude of 450 == max W/S/A/D.
            // Only override the axis if the player has meaningful
            // velocity along it (tiny drift due to landing wobble etc.
            // shouldn't yank an axis to ±450).
            if (velFwd >  2.f) *pFwd  = -450.f;
            else if (velFwd < -2.f) *pFwd  =  450.f;

            if (velRight >  2.f) *pSide = -450.f;
            else if (velRight < -2.f) *pSide =  450.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Layout drift — silently skip.
        }
    }

    // -----------------------------------------------------------------
    //  FixMovement
    //
    //  CS2's player_move code reads pBaseCmd->flForwardMove /
    //  flSideMove and projects them onto the basis formed by the cmd's
    //  view yaw. If we shipped a fake yaw without compensating, the
    //  player's input "W" gets routed into whatever direction the fake
    //  yaw points to (back-walks in Backwards mode, spins in Spin
    //  mode). Counter-rotate the move floats by the SAME delta we
    //  added to the yaw and the engine will end up pushing the player
    //  in the real direction.
    //
    //  The base cmd struct lives 0x40 inside the cmd. The move floats
    //  sit at base+0x50 (forward), base+0x54 (side). These match the
    //  layout Movement.cpp already relies on for bhop.
    //
    //  ALSO: modern CS2 uses sub-tick movement (subtickMovesField on
    //  pBaseCmd). Each subtick step carries flAnalogForwardDelta /
    //  flAnalogLeftDelta values that the engine sums into the per-tick
    //  movement vector — separately from flForwardMove. We rotate
    //  those too. Without this, Backwards/Sideways anti-aim modes
    //  visibly drag W-presses into the fake direction even when the
    //  legacy floats are correctly counter-rotated.
    // -----------------------------------------------------------------
    void FixMovement(c_user_cmd* cmd, float deltaYawDeg)
    {
        if (!cmd) return;
        // Cheap early-out — no rotation needed.
        if (deltaYawDeg > -0.001f && deltaYawDeg < 0.001f) return;

        const float rad = deltaYawDeg * 0.01745329252f;   // π/180
        const float c   = std::cos(rad);
        const float s   = std::sin(rad);

        __try {
            uintptr_t baseCmd = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(cmd) + 0x40);
            if (!baseCmd) return;

            // --- Legacy per-tick move floats ----------------------------
            float* pFwd  = reinterpret_cast<float*>(baseCmd + 0x50);
            float* pSide = reinterpret_cast<float*>(baseCmd + 0x54);

            float fwd  = *pFwd;
            float side = *pSide;
            if (!(fwd > -0.5f && fwd < 0.5f && side > -0.5f && side < 0.5f)) {
                // 2-D rotation of the (forward, side) vector by deltaYaw.
                *pFwd  = fwd * c - side * s;
                *pSide = fwd * s + side * c;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Layout drift — leave moves untouched rather than crash.
        }

        // --- Sub-tick analog deltas ------------------------------------
        // pBaseCmd->subtickMovesField is a RepeatedPtrField<CSubtickMoveStep>
        // starting at offset 0x18 inside the base cmd (after the inherited
        // CBasePB 24-byte header). Each CSubtickMoveStep has:
        //     +0x18  nButton          (8)
        //     +0x20  bPressed         (1, padded)
        //     +0x24  flWhen           (4)
        //     +0x28  flAnalogForwardDelta  (4)   <-- rotate
        //     +0x2C  flAnalogLeftDelta     (4)   <-- rotate
        __try {
            CUserCmd* full = reinterpret_cast<CUserCmd*>(cmd);
            CBaseUserCmdPB* base = full->csgoUserCmd.pBaseCmd;
            if (!base) return;

            auto& subticks = base->subtickMovesField;
            if (!subticks.pRep) return;

            int count = subticks.nCurrentSize;
            if (count <= 0 || count > 64) return;
            int allocated = subticks.pRep->nAllocatedSize;
            if (count > allocated) count = allocated;
            if (count > 64) count = 64;

            for (int i = 0; i < count; ++i) {
                CSubtickMoveStep* step = subticks.pRep->tElements[i];
                if (!step) continue;

                uintptr_t stepBase = reinterpret_cast<uintptr_t>(step);
                float* pFwd  = reinterpret_cast<float*>(stepBase + 0x28);
                float* pSide = reinterpret_cast<float*>(stepBase + 0x2C);

                float fwd  = *pFwd;
                float side = *pSide;
                if (fwd > -0.001f && fwd < 0.001f &&
                    side > -0.001f && side < 0.001f)
                    continue;

                *pFwd  = fwd * c - side * s;
                *pSide = fwd * s + side * c;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Same defensive bail.
        }
    }
}
