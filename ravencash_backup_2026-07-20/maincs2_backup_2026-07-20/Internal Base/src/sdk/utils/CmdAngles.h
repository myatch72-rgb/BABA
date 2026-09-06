#pragma once
#include <cstdint>

// =============================================================================
//  CmdAngles
//
//  CS2's CUserCmd carries view angles in TWO places:
//
//    1. The "base cmd" protobuf at  cmd->csgoUserCmd.pBaseCmd->pViewAngles
//       — historically what hit-reg used. Modern CS2 (2024-onwards) reads
//       this only as a fallback.
//
//    2. Per-subtick history entries at
//       cmd->csgoUserCmd.inputHistoryField[i]->pViewAngles
//       — what the server lag-comp pipeline now actually reads. There are
//       typically 1-3 of these per outgoing cmd.
//
//  WriteToHistory stamps EVERY history entry with the same (pitch, yaw, 0)
//  triple so the server picks up the silent-aim correction regardless of
//  which subtick it samples. Without this call the rewrite only affects
//  the legacy slot and the server quietly ignores it.
//
//  Implemented entirely inside __try / __except so a stale protobuf layout
//  (after a CS2 patch) can only AV-quietly, never crash the caller.
// =============================================================================
class c_user_cmd;

namespace CmdAngles
{
    // History-only write — used by Silent Aim. The local camera is NEVER
    // affected because the slot the client-side prediction reads
    // (`pBaseCmd->pViewAngles`) is left intact. Only the server's
    // hit-reg path picks up the change.
    void WriteToHistory(c_user_cmd* cmd, float pitch, float yaw);

    // Full write — history entries + the pBaseCmd slot. Used by Anti-Aim.
    // The pBaseCmd slot is what the server reads when it sets each
    // player's networked `m_angEyeAngles`, which is what OTHER clients
    // see when they look at us. The local first-person camera still
    // reads dwViewAngles (mouse), which we never touch, so this write
    // stays "silent" from the user's POV.
    void WriteFull(c_user_cmd* cmd, float pitch, float yaw);

    // Visible-anti-aim write — pBaseCmd gets the FAKE (pitch, yaw) so
    // the server's networked m_angEyeAngles update propagates the fake
    // direction to every other client (anti-aim's visible effect). The
    // sub-tick history entries get the REAL angles, so:
    //
    //   * The engine's sub-tick movement processing computes the
    //     forward-vector off the REAL yaw — W/A/S/D work normally.
    //   * The engine's view-prediction sub-tick reads of inputHistory
    //     don't drag dwViewAngles onto the fake yaw — camera stays put.
    //   * Server lag-comp / hit-reg reads inputHistory — REAL angles
    //     mean bullets land where the crosshair points (no anti-aim
    //     misalignment on our own shots).
    //
    //   pBaseCmd:    pitch = fakePitch, yaw = fakeYaw, roll = 0
    //   history[i]:  pitch = realPitch, yaw = realYaw, roll = 0
    //
    // Trade-off: this is a "visible" anti-aim only. Server lag-comp
    // sees our real angles so a perfect-aim attacker isn't actually
    // dragged onto a fake direction. The visible character rotation
    // (what other players SEE) still works because it's driven by the
    // networked m_angEyeAngles which the server updates off pBaseCmd.
    void WriteHybrid(c_user_cmd* cmd,
                     float realPitch, float realYaw,
                     float fakePitch, float fakeYaw);

    // Counter-rotates the cmd's forward / side move floats so the player
    // physically moves in their REAL look direction even though the cmd
    // ships a fake yaw. Without this, anti-aim's spin/jitter modes
    // rotate the move vector to wherever the fake yaw is pointing — the
    // player presses W but goes sideways or backwards.
    //
    //   deltaYawDeg = realYaw - fakeYaw   (degrees)
    //
    // Call AFTER writing the fake yaw into the cmd.
    void FixMovement(c_user_cmd* cmd, float deltaYawDeg);

    // Overrides the cmd's forward / side move floats so the engine
    // applies a counter-strafe burst — projects the player's current
    // 3-D velocity onto the supplied yaw basis and writes the OPPOSITE
    // direction at max-input magnitude (±450). CS2's friction zeroes
    // the velocity within a single tick, dropping movement-induced
    // spread to ground accuracy for the same shot.
    //
    //   velocity = pawn's current m_vecVelocity (world space)
    //   yawDeg   = the cmd yaw whose forward/right basis the move
    //              floats are projected onto (use REAL yaw — cmd's
    //              base yaw is unchanged by Silent Aim's history-only
    //              write)
    void AutoStop(c_user_cmd* cmd, float velX, float velY, float yawDeg);
}
