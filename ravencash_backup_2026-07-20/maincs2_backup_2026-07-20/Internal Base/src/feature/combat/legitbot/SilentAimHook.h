#pragma once

namespace SilentAimHook {
    bool Install();
    void Shutdown();

    // In-game gate. Aimbot worker calls this every tick with whether the
    // local pawn is valid + alive + on a team. When false, the hook's
    // anti-aim write path becomes a no-op so we don't poke at pointers
    // that just became invalid (map load, disconnect, spectator).
    void SetLocalValid(bool v);

    // Live local view (pitch, yaw) from pawn->v_angle. The hook uses
    // these as the BASE for anti-aim's yaw flip / pitch override so the
    // written angle is `base + offset` — stable per real-camera frame,
    // not a runaway accumulation from the previous overwrite.
    // Compatibility entry point; no-spread no longer stores camera angles.
    void SetLocalView(float pitch, float yaw);

    // Anti-Aim: Provides the target fake pitch and yaw to the hook.
    void SetAntiAimTarget(float fakePitch, float fakeYaw);
    
    // Anti-Aim: Tells the hook whether Anti-Aim is actively overriding the view.
    void SetAntiAimActive(bool active);

    // Armed only for a tick where SilentAim actually acquired a target.
    // This prevents the accuracy hooks from affecting normal/manual shots.
    void SetNoSpreadActive(bool active);

    // Lazy hook gate. Aimbot worker calls this every tick with whether
    // ANY feature wants the CMsgQAngleCpy hook live (silent / rage /
    // anti-aim). On false the patch is reverted, on true re-applied.
    // Stops community-server map-load crashes that came from leaving
    // the hook permanently armed across map transitions.
    // Enabled only while the setting is requested and the local pawn is valid.
    void SetWantsHook(bool wants);

}
