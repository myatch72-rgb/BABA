#include "SilentAim.h"
#include "SilentAimHook.h"

// Windows.h has `min`/`max` macros that collide with <algorithm>. We need
// `std::max` for the FOV clamp below so suppress the macros project-wide
// in this TU before any include pulls Windows.h in.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/utils/CCSGOInput.h"
#include "../../../sdk/utils/CmdAngles.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/utils/Utils.h"
#include "Multipoint.h"
#include "../../misc/bhop/BhopV2.h"  // c_user_cmd full definition for m_button_state

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace SilentAim {

// ---------------------------------------------------------------------------
// Local math helpers — duplicated from Aimbot.cpp to keep this module
// independent. Identical formulae so a target ranked best by one is ranked
// best by the other.
// ---------------------------------------------------------------------------

static void NormalizeDelta(Vector& d) {
    while (d.y >  180.f) d.y -= 360.f;
    while (d.y < -180.f) d.y += 360.f;
    while (d.x >   89.f) d.x -= 180.f;
    while (d.x <  -89.f) d.x += 180.f;
}

static Vector CalcAngle(const Vector& src, const Vector& dst) {
    Vector d = dst - src;
    const float h = std::sqrt(d.x * d.x + d.y * d.y);
    Vector a;
    a.x = std::atan2(-d.z, h) * (180.0f / 3.14159265358979323846f);
    a.y = std::atan2( d.y, d.x) * (180.0f / 3.14159265358979323846f);
    a.z = 0.0f;
    return a;
}

static float Fov(const Vector& view, const Vector& target) {
    Vector d = target - view;
    NormalizeDelta(d);
    return std::sqrt(d.x * d.x + d.y * d.y);
}

// Vis check — loose-by-default when raycaster isn't loaded so the user
// doesn't lose all aim on maps without a .tri. `aim_vis_strict_fallback`
// flips this to strict (refuse aim entirely) for users who want zero
// wallhack chance at the cost of aim uptime.
static bool VisCheck(const Vector& eye, const Vector& bone,
                     C_CSPlayerPawn* /*enemyPawn*/, int /*localTeam*/) {
    if (!Globals::aim_vis_check) return true;
    if (!Raycasting::Get().IsLoaded())
        return !Globals::aim_vis_strict_fallback;
    return Raycasting::Get().IsVisible(eye, bone);
}

// ---------------------------------------------------------------------------
// Helper: resolve the cmd's view-angle pointer. Lives in its own function
// because MSVC refuses to mix __try with functions that have destructible
// objects on the stack (the main Run() loop holds an std::vector of
// entities — putting __try in there is a compile error C2712).
// ---------------------------------------------------------------------------

static QAngle* ResolveCmdViewAngles(c_user_cmd* cmd) {
    __try {
        const uintptr_t baseCmd =
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(cmd) + 0x18 + 0x28);
        if (!baseCmd) return nullptr;
        const uintptr_t pViewAnglesAddr = Utils::SafeRead<uintptr_t>(baseCmd + 0x38);
        if (!pViewAnglesAddr) return nullptr;
        return &reinterpret_cast<CMsgQAngle*>(pViewAnglesAddr)->angValue;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Cmd-side angle propagation lives in sdk/utils/CmdAngles.h.

// Push the IN_ATTACK bit into the cmd's button-state fields. Isolated
// in its own function so the SEH around it doesn't tangle with the
// std::vector in Run() (MSVC C2712).
static void PushFireButton(c_user_cmd* cmd) {
    __try {
        cmd->m_button_state  |= 1ULL << 0; // IN_ATTACK
        cmd->m_button_state2 |= 1ULL << 0;
        cmd->m_button_state3 |= 1ULL << 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // c_user_cmd layout drifted - silently skip the fire.
    }
}

// Pure write — also isolated so the __try has its own scope free of
// std::vector destructors.
static bool WriteCmdAngles(QAngle* pCmd, const Vector& targetAng, float smoothing) {
    __try {
        if (smoothing > 0.001f) {
            Vector cmdAng = { pCmd->pitch, pCmd->yaw, pCmd->roll };
            Vector delta  = targetAng - cmdAng;
            while (delta.y >  180.f) delta.y -= 360.f;
            while (delta.y < -180.f) delta.y += 360.f;
            const float k = (std::clamp)(1.0f - smoothing, 0.02f, 1.0f);
            cmdAng.x += delta.x * k;
            cmdAng.y += delta.y * k;
            pCmd->pitch = (std::clamp)(cmdAng.x, -89.f, 89.f);
            pCmd->yaw   = cmdAng.y;
            pCmd->roll  = 0.f;
        } else {
            pCmd->pitch = targetAng.x;
            pCmd->yaw   = targetAng.y;
            pCmd->roll  = 0.f;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SetNoSpreadSeed(c_user_cmd* cmd) {
    __try {
        CBaseUserCmdPB* base =
            reinterpret_cast<CUserCmd*>(cmd)->csgoUserCmd.pBaseCmd;
        if (base) {
            base->nRandomSeed = 0;
            base->nHasBits |= (1u << 9);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Main entry — called from the CreateMove hook every tick. Cheap when the
// feature is disabled (single bool compare and bail).
// ---------------------------------------------------------------------------

void Run(c_user_cmd* cmd) {
    if (!Globals::silent_aim_enabled || !cmd) return;

    // Key gate — same convention as Aimbot: key==0 means "always".
    if (Globals::silent_aim_key != 0 &&
        Globals::silent_aim_key > 0 && Globals::silent_aim_key < 256) {
        if ((GetAsyncKeyState(Globals::silent_aim_key) & 0x8000) == 0)
            return;
    }

    // Optional "only fire" gate, shared with the regular aimbot.
    if (Globals::aim_only_when_shooting) {
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
            return;
    }

    const uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) return;

    auto* localPawn = EntityManager::Get().GetLocalPawn();
    if (!localPawn || !Utils::SafeAlive(localPawn)) return;

    const uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(localPawn);
    const uintptr_t scene = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
    if (!Utils::IsValidPtr(scene)) return;

    const Vector localOrigin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
    const Vector viewOffset  = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecViewOffset);
    const Vector localEye    = localOrigin + viewOffset;

    const Vector currentView = Utils::SafeRead<Vector>(client + Offsets::dwViewAngles);

    // Accuracy gates (Airborne/Jump)
    const uint32_t flags    = Utils::SafeRead<uint32_t>(pawnAddr + Offsets::m_fFlags);
    const bool     onGround = (flags & 0x1u) != 0;
    const Vector   velocity = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecVelocity);
    const float    speed2D  = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);

    static bool s_wasOnGround = true;
    const bool justLanded     = onGround && !s_wasOnGround;
    s_wasOnGround             = onGround;

    // No Spread explicitly authorizes airborne acquisition. Without this
    // override the default Skip In Air gate exits before a target/seed is set,
    // making the No Spread option appear completely non-functional.
    if (Globals::silent_aim_skip_air && !onGround &&
        !Globals::silent_aim_nospread) return;
    if (Globals::silent_aim_skip_jump && justLanded) return;

    QAngle* pCmdViewAngles = ResolveCmdViewAngles(cmd);
    if (!pCmdViewAngles) return;

    // -----------------------------------------------------------------
    // Pick the best enemy bone using the shared legit filters.
    // -----------------------------------------------------------------
    const float baseFov = Globals::silent_aim_fov < 0.1f ? 0.1f : Globals::silent_aim_fov;

    auto entities = EntityManager::Get().GetEntities();
    Vector bestBone{};
    float  bestFovScore = baseFov;
    bool   found = false;
    const int localTeam = Utils::SafeTeam(localPawn);

    for (const auto& e : entities) {
        if (!e.pawn || e.pawn == localPawn) continue;
        if (!Utils::SafeAlive(e.pawn))       continue;
        if (!(e.isEnemy || Globals::target_teammates)) continue;

        BoneID priority[11];
        int    nBones = 0;
        if (Globals::aim_hitbox_head)    priority[nBones++] = BoneID::Head;
        if (Globals::aim_hitbox_neck)    priority[nBones++] = BoneID::Neck;
        if (Globals::aim_hitbox_chest)   priority[nBones++] = BoneID::Chest;
        if (Globals::aim_hitbox_stomach) priority[nBones++] = BoneID::Spine;
        if (Globals::aim_hitbox_pelvis)  priority[nBones++] = BoneID::Pelvis;
        if (Globals::aim_hitbox_arms)  { priority[nBones++] = BoneID::ShoulderL;
                                         priority[nBones++] = BoneID::ShoulderR; }
        if (Globals::aim_hitbox_legs)  { priority[nBones++] = BoneID::KneeL;
                                         priority[nBones++] = BoneID::KneeR; }
        if (Globals::aim_hitbox_feet)  { priority[nBones++] = BoneID::FootHeelL;
                                         priority[nBones++] = BoneID::FootHeelR; }
        if (nBones == 0) priority[nBones++] = BoneID::Head;

        for (int i = 0; i < nBones; ++i) {
            std::vector<Vector> pointsToScan;
            
            if (Globals::silent_aim_multipoint) {
                int targetHitbox = -1;
                if (priority[i] == BoneID::Head) targetHitbox = 6;
                else if (priority[i] == BoneID::Chest || priority[i] == BoneID::Spine) targetHitbox = 5;
                
                if (targetHitbox != -1) {
                    auto sets = Multipoint::GetHitboxWorldPoints(e.pawn, Globals::silent_aim_multipoint_scale, targetHitbox);
                    for (const auto& s : sets) {
                        for (const auto& pt : s.points) {
                            pointsToScan.push_back(pt);
                        }
                    }
                }
            }
            
            // Fallback or addition of the center bone
            const Vector centerBone = Utils::GetBonePos(e.pawn, priority[i]);
            if (pointsToScan.empty() && !centerBone.IsZero()) {
                pointsToScan.push_back(centerBone);
            }
            
            bool hit_in_this_bone = false;
            for (const auto& bone : pointsToScan) {
                if (bone.IsZero()) continue;

                // Distance gate.
                if (Globals::aim_max_distance > 1.0f) {
                    const float dx = bone.x - localEye.x;
                    const float dy = bone.y - localEye.y;
                    const float dz = bone.z - localEye.z;
                    const float r  = Globals::aim_max_distance;
                    if (dx*dx + dy*dy + dz*dz > r*r) continue;
                }

                if (!VisCheck(localEye, bone, e.pawn, localTeam)) continue;

                const Vector ang = CalcAngle(localEye, bone);
                const float  fov = Fov(currentView, ang);
                if (fov < bestFovScore) {
                    bestFovScore = fov;
                    bestBone     = bone;
                    found        = true;
                    hit_in_this_bone = true;
                }
            }
            if (hit_in_this_bone && Globals::aim_smart_bone) break;
        }
    }

    if (!found) return;

    Vector targetAng = CalcAngle(localEye, bestBone);

    if (Globals::silent_aim_nospread) {
        // Keep the protobuf seed deterministic and mark field #10 present.
        // The paired accuracy hooks are armed only for this acquired shot.
        SetNoSpreadSeed(cmd);
        SilentAimHook::SetNoSpreadActive(true);
    }

    // RCS (Recoil Control System) - Compensate for aim punch
    const uintptr_t aimPunchServices = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pAimPunchServices);
    if (Utils::IsValidPtr(aimPunchServices)) {
        const Vector aimPunch = Utils::SafeRead<Vector>(aimPunchServices + Offsets::m_vAimPunchAngle);
        targetAng.x -= aimPunch.x * 2.0f;
        targetAng.y -= aimPunch.y * 2.0f;
    }

    targetAng.x = (std::clamp)(targetAng.x, -89.f, 89.f);
    NormalizeDelta(targetAng);
    targetAng.z = 0.f;

    // Auto Stop
    if (Globals::silent_aim_auto_stop &&
        onGround &&
        speed2D > Globals::silent_aim_stop_speed)
    {
        CmdAngles::AutoStop(cmd, velocity.x, velocity.y, currentView.y);
    }

    WriteCmdAngles(pCmdViewAngles, targetAng, Globals::silent_aim_smoothing);

    // Propagate the final (possibly smoothed) aim angle into every subtick
    // history entry so the server's hit-reg path actually sees it.
    CmdAngles::WriteToHistory(cmd, pCmdViewAngles->pitch, pCmdViewAngles->yaw);

    // Optional auto-fire: when a valid target is locked, also push the
    // IN_ATTACK bit into the outgoing cmd's button state.
    if (Globals::silent_aim_autofire) {
        PushFireButton(cmd);
    }
}

} // namespace SilentAim
