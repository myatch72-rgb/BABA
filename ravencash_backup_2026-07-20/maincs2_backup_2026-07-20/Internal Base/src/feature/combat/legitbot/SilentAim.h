// SilentAim.h — invisible aimbot that writes the OUTGOING CUserCmd's
// view angles but never touches the local crosshair.
//
// Why this is different from `Aimbot::`:
//   * The legit aimbot writes `client + dwViewAngles`, which is where the
//     game reads the *visible* mouse-look from. Hits register, but anyone
//     watching your screen (or a demo) sees the cursor snap.
//   * SilentAim writes `cmd->pBaseCmd->pViewAngles->angValue` — the field
//     the engine serialises into the network packet. The server uses this
//     for hit-reg, but the local view angles you see on screen are
//     unaffected. From spectator POV your aim looks completely natural.
//
// Why it lives next to the rage logic that already does the same write:
//   The Ragebot has identical mechanics but uses rage-style filters
//   (hold-to-rage key, smoothing=0.5, multipoint, no vis-check). Legit
//   players want the same trick with tighter filters (low FOV, vis-check
//   on, reaction delay, distance cap). Keeping the two as separate modules
//   means you can run legit silent aim while leaving rage off, and the
//   menu can expose each with its own knobs without merge conflicts.
//
// Call from the CreateMove hook AFTER Ragebot::Run — they share the
// underlying write target but the priority is "rage wins if both fire on
// the same tick".

#pragma once

class c_user_cmd;

namespace SilentAim {
    // Reads enemies + the legit-aim filters, writes a silent correction
    // into `cmd` if it finds a suitable target. No-op when disabled or
    // when the key isn't held.
    void Run(c_user_cmd* cmd);
}
