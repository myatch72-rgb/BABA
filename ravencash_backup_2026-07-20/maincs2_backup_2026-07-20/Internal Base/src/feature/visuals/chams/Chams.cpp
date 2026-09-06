#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Chams.h"
#include "GPURenderer.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/memory/Offsets.h"
#include <cmath>
#include <algorithm>


extern ID3D11RenderTargetView* g_ChamsRTV;


static Vector GetLocalEyePosition()
{
    auto localPawn = EntityManager::Get().GetLocalPawn();
    if (!localPawn) return {};

    uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(localPawn);
    uintptr_t scene = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
    if (!Utils::IsValidPtr(scene)) return {};

    Vector origin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
    Vector viewOffset = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecViewOffset);
    return origin + viewOffset;
}


static bool IsAnyBoneVisible(C_CSPlayerPawn* pawn)
{
    if (!Raycasting::Get().IsLoaded())
        return true; 

    Vector eyePos = GetLocalEyePosition();
    if (eyePos.IsZero())
        return true;

    static const BoneID majorBones[] = {
        BoneID::Head, BoneID::Neck, BoneID::Spine, BoneID::Pelvis,
        BoneID::LeftShoulder, BoneID::RightShoulder,
        BoneID::LeftHip, BoneID::RightHip
    };

    for (auto bone : majorBones)
    {
        Vector bonePos = Utils::GetBonePos(pawn, bone);
        if (!bonePos.IsZero() && Raycasting::Get().IsVisible(eyePos, bonePos))
            return true;
    }

    return false;
}







void Chams::Render()
{
    if (!Globals::chams_enabled && !Globals::chams_enabled_team)
        return;

    auto& gpu = GPUChamsRenderer::Get();
    if (!gpu.IsInitialized())
        return;

    
    gpu.BeginFrame((float)Globals::ScreenWidth, (float)Globals::ScreenHeight);

    
    gpu.SetViewMatrix(Globals::ViewMatrix);

    auto entities = EntityManager::Get().GetEntities();
    C_CSPlayerPawn* localPawn = EntityManager::Get().GetLocalPawn();
    if (!localPawn) return;
    C_CSPlayerPawn* observedPawn = EntityManager::Get().GetLocalObservedPawn();

    for (const auto& ent : entities)
    {
        if (!ent.pawn) continue;
        if (ent.pawn == localPawn) continue;
        if (observedPawn && ent.pawn == observedPawn) continue;
        uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
        if (!Utils::IsValidPtr(pawnAddr)) continue;
        if (!Utils::SafeAlive(ent.pawn)) continue;

        int team = Utils::SafeTeam(ent.pawn);
        int localTeam = Utils::SafeTeam(localPawn);
        const bool isEnemy = (team != localTeam);
        const bool chamsEnabled = isEnemy ? Globals::chams_enabled : Globals::chams_enabled_team;
        if (!chamsEnabled) continue;
        const bool chamsFilledEnabled = isEnemy ? Globals::chams_filled : Globals::chams_filled_team;
        const bool chamsWireEnabled = isEnemy ? Globals::chams_wireframe : Globals::chams_wireframe_team;
        if (!chamsFilledEnabled && !chamsWireEnabled) continue;

        
        Vector origin = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vOldOrigin);
        Vector headPos = Utils::GetBonePos(ent.pawn, BoneID::Head);
        
        if (headPos.IsZero()) {
            headPos = origin;
            headPos.z += 72.0f; 
        } else {
            headPos.z += 7.0f; 
        }

        Vector sHead, sFoot;
        if (!Utils::WorldToScreen(headPos, sHead, (float*)Globals::ViewMatrix, (float)Globals::ScreenWidth, (float)Globals::ScreenHeight) ||
            !Utils::WorldToScreen(origin, sFoot, (float*)Globals::ViewMatrix, (float)Globals::ScreenWidth, (float)Globals::ScreenHeight))
            continue;

        
        if (std::isnan(origin.x) || std::isnan(origin.y) || std::isnan(origin.z)) continue;
        if (std::isnan(headPos.x) || std::isnan(headPos.y) || std::isnan(headPos.z)) continue;
        if (origin.x == 0 && origin.y == 0 && origin.z == 0) continue;

        
        float height = headPos.z - origin.z;
        if (height < 10.0f) height = 72.0f; 

        
        Vector boxCenter;
        boxCenter.x = (headPos.x + origin.x) * 0.5f;
        boxCenter.y = (headPos.y + origin.y) * 0.5f;
        boxCenter.z = (headPos.z + origin.z) * 0.5f + 2.0f; 

        
        Vector boxSize;
        boxSize.x = 30.0f;  
        boxSize.y = 30.0f;  
        boxSize.z = height + 12.0f; 

        
        bool isVis = IsAnyBoneVisible(ent.pawn);

        float* fillHidden = isEnemy ? Globals::chams_fill_color : Globals::chams_fill_color_team;
        float* fillVisible = isEnemy ? Globals::chams_fill_color_vis : Globals::chams_fill_color_vis_team;
        float* wireHidden = isEnemy ? Globals::chams_wire_color : Globals::chams_wire_color_team;
        float* wireVisible = isEnemy ? Globals::chams_wire_color_vis : Globals::chams_wire_color_vis_team;

        if (chamsFilledEnabled) {
            gpu.DrawBox(boxCenter, boxSize, isVis ? fillVisible : fillHidden);
        }

        if (chamsWireEnabled) {
            gpu.DrawWireBox(boxCenter, boxSize, isVis ? wireVisible : wireHidden);
        }
    }
}

void Chams::Flush()
{
    if (!Globals::chams_enabled && !Globals::chams_enabled_team) return;

    auto& gpu = GPUChamsRenderer::Get();
    if (!gpu.IsInitialized()) return;

    if (g_ChamsRTV) {
        gpu.EndFrame(g_ChamsRTV);
    }
}
