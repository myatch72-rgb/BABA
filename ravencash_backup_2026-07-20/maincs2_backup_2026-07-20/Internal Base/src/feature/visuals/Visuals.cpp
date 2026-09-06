#include <string>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "Visuals.h"
#include "esp/Esp.h"
#include "chams/Chams.h"
#include "chamsv2/ChamsV2.h"
#include "enemycounter/EnemyCounter.h"
#include "grenadepredict/GrenadePrediction.h"
#include "../../ext/imgui/imgui.h"
#include "../../sdk/utils/Globals.h"
#include "../../sdk/memory/Offsets.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/entity/EntityManager.h"
#include "../../sdk/utils/Raycasting.h"
#include "../../sdk/utils/Utils.h"

namespace
{
    // -----------------------------------------------------------------
    // Grenade prediction has been extracted into its own self-healing
    // module — see src/feature/visuals/grenadepredict/GrenadePrediction.*.
    // The helpers below (GetActiveWeapon, GetActiveWeaponDefIndex,
    // GetLocalEyePosition) are still used by glow/ESP code further down,
    // so they stay in-file.
    // -----------------------------------------------------------------

    static uintptr_t GetActiveWeapon(C_CSPlayerPawn* localPawn)
    {
        uintptr_t pawn = reinterpret_cast<uintptr_t>(localPawn);
        uintptr_t weaponServices = Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pWeaponServices);
        if (!Utils::IsValidPtr(weaponServices))
            return 0;

        uint32_t activeHandle = Utils::SafeRead<uint32_t>(weaponServices + Offsets::m_hActiveWeapon);
        if ((activeHandle & 0x7FFF) == 0)
            return 0;

        auto* activeEntity = EntityManager::Get().GetEntityFromHandle(activeHandle);
        return reinterpret_cast<uintptr_t>(activeEntity);
    }

    static uint16_t GetActiveWeaponDefIndex(C_CSPlayerPawn* localPawn)
    {
        uintptr_t activeWeapon = GetActiveWeapon(localPawn);
        if (!Utils::IsValidPtr(activeWeapon))
            return 0;

        uintptr_t item = activeWeapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
        return Utils::SafeRead<uint16_t>(item + Offsets::sc_m_iItemDefinitionIndex);
    }

    static Vector GetLocalEyePosition(C_CSPlayerPawn* localPawn)
    {
        uintptr_t pawn = reinterpret_cast<uintptr_t>(localPawn);
        uintptr_t scene = Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pGameSceneNode);
        if (!Utils::IsValidPtr(scene))
            return {};

        Vector origin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
        Vector viewOffset = Utils::SafeRead<Vector>(pawn + Offsets::m_vecViewOffset);
        return origin + viewOffset;
    }

}

static void RenderInternal()
{
    static auto lastGrenadeUpdate = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGrenadeUpdate).count() >= 8)
    {
        // New self-healing predictor — replaces the old in-file logic.
        GrenadePrediction::Update();
        lastGrenadeUpdate = now;
    }

    ESP::Render();
    Chams::Render();

    
    if ((Globals::chamsv2_enabled || Globals::chamsv2_enabled_team) && ChamsV2::g_MeshRenderer.IsReady())
    {
        
        struct DissolveEntry {
            ChamsV2::DissolveSnapshot snapshot;
            float startTime;
            float duration;
            int   style;
            float intensity;
            float edgeColor[3];
        };

        static std::unordered_map<uintptr_t, bool> s_chams_alive_state;
        static std::vector<DissolveEntry> s_dissolving;

        auto entities = EntityManager::Get().GetEntities();
        C_CSPlayerPawn* localPawn = EntityManager::Get().GetLocalPawn();
        C_CSPlayerPawn* observedPawn = EntityManager::Get().GetLocalObservedPawn();
        float curTime = static_cast<float>(GetTickCount64()) / 1000.0f;

        if (localPawn)
        {
            std::unordered_set<uintptr_t> seenPawns;
            seenPawns.reserve(entities.size());

            for (const auto& ent : entities)
            {
                if (!ent.pawn || !ent.controller) continue;
                if (ent.pawn == localPawn) continue;
                if (observedPawn && ent.pawn == observedPawn) continue;
                if (!Utils::IsValidPtr(reinterpret_cast<uintptr_t>(ent.pawn))) continue;

                uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);
                seenPawns.insert(pawnAddr);
                bool aliveNow = Utils::SafeAlive(ent.pawn);

                
                if (Globals::chams_kill_effect)
                {
                    const bool chamsv2On = ent.isEnemy ? Globals::chamsv2_enabled : Globals::chamsv2_enabled_team;
                    auto it = s_chams_alive_state.find(pawnAddr);
                    bool wasAlive = (it == s_chams_alive_state.end()) ? aliveNow : it->second;

                    if (wasAlive && !aliveNow && chamsv2On)
                    {
                        
                        if (s_dissolving.size() < 8) 
                        {
                            DissolveEntry entry{};
                            if (ChamsV2::g_MeshRenderer.CreateSnapshot(ent.pawn, ent.isEnemy, entry.snapshot))
                            {
                                entry.startTime = curTime;
                                entry.duration = Globals::chams_kill_effect_duration;
                                entry.style = Globals::chams_kill_effect_style;
                                entry.intensity = Globals::chams_kill_effect_intensity;
                                entry.edgeColor[0] = Globals::chams_kill_effect_edge_color[0];
                                entry.edgeColor[1] = Globals::chams_kill_effect_edge_color[1];
                                entry.edgeColor[2] = Globals::chams_kill_effect_edge_color[2];
                                s_dissolving.push_back(std::move(entry));
                            }
                        }
                    }
                    s_chams_alive_state[pawnAddr] = aliveNow;
                }

                
                const bool chamsv2Enabled = ent.isEnemy ? Globals::chamsv2_enabled : Globals::chamsv2_enabled_team;
                if (!chamsv2Enabled) continue;

                ChamsV2::g_MeshRenderer.RenderPlayer(ent.controller, ent.pawn, 1.0f, ent.isEnemy);
            }

            
            for (auto it = s_chams_alive_state.begin(); it != s_chams_alive_state.end(); )
            {
                if (seenPawns.find(it->first) == seenPawns.end())
                    it = s_chams_alive_state.erase(it);
                else
                    ++it;
            }

            
            for (int i = (int)s_dissolving.size() - 1; i >= 0; --i)
            {
                auto& entry = s_dissolving[i];
                float elapsed = curTime - entry.startTime;
                if (elapsed > entry.duration)
                {
                    s_dissolving.erase(s_dissolving.begin() + i);
                    continue;
                }

                float t = elapsed / entry.duration;
                
                float dissolveAmount = 1.0f - (1.0f - t) * (1.0f - t);
                dissolveAmount *= entry.intensity;
                if (dissolveAmount > 1.0f) dissolveAmount = 1.0f;

                float edgeWidth = 0.06f + (1.0f - t) * 0.06f; 
                float noiseScale = 0.12f + (entry.style == 2 ? 0.06f : 0.0f); 

                ChamsV2::g_MeshRenderer.RenderSnapshot(
                    entry.snapshot, dissolveAmount, entry.style,
                    edgeWidth, noiseScale, entry.edgeColor, entry.intensity);
            }

            
            if (!Globals::chams_kill_effect && !s_dissolving.empty())
            {
                s_dissolving.clear();
                s_chams_alive_state.clear();
            }
        }
    }

    if (Globals::aim_draw_fov && Globals::aim_enabled)
    {

        float referenceFov = 90.0f;
        ImGui::GetBackgroundDrawList()->AddCircle(
            ImVec2(Globals::GameViewportX + Globals::ScreenWidth / 2.f, Globals::GameViewportY + Globals::ScreenHeight / 2.f),
            Globals::aim_fov * (Globals::ScreenHeight / referenceFov),
            ImGui::ColorConvertFloat4ToU32(*(ImVec4*)Globals::aim_fov_color),
            64,
            1.0f
        );
    }

    if (Globals::silent_aim_draw_fov && Globals::silent_aim_enabled)
    {
        float referenceFov = 90.0f;
        ImGui::GetBackgroundDrawList()->AddCircle(
            ImVec2(Globals::GameViewportX + Globals::ScreenWidth / 2.f, Globals::GameViewportY + Globals::ScreenHeight / 2.f),
            Globals::silent_aim_fov * (Globals::ScreenHeight / referenceFov),
            ImGui::ColorConvertFloat4ToU32(*(ImVec4*)Globals::silent_aim_fov_color),
            64,
            1.0f
        );
    }


    if (Globals::trigger_draw_fov && Globals::trigger_enabled)
    {
        float referenceFov = 90.0f; 
        ImGui::GetBackgroundDrawList()->AddCircle(
            ImVec2(Globals::GameViewportX + Globals::ScreenWidth / 2.f, Globals::GameViewportY + Globals::ScreenHeight / 2.f),
            Globals::trigger_fov * (Globals::ScreenHeight / referenceFov), 
            ImGui::ColorConvertFloat4ToU32(*(ImVec4*)Globals::trigger_fov_color),
            64,
            1.0f
        );
    }

    GrenadePrediction::Render();

    C_CSPlayerPawn* localPawn = EntityManager::Get().GetLocalPawn();
    if (localPawn && Utils::SafeAlive(localPawn))
    {
        uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(localPawn);
        bool isScoped = Utils::SafeRead<bool>(pawnAddr + Offsets::m_bIsScoped);
        if (isScoped && Globals::remove_scope_overlay && Globals::custom_scope_crosshair)
        {
            uint16_t defIdx = GetActiveWeaponDefIndex(localPawn);
            if (defIdx == 9 || defIdx == 40 || defIdx == 38 || defIdx == 11)
            {
                float cx = Globals::GameViewportX + Globals::ScreenWidth / 2.0f;
                float cy = Globals::GameViewportY + Globals::ScreenHeight / 2.0f;
                ImU32 col = ImGui::ColorConvertFloat4ToU32(*(ImVec4*)Globals::scope_crosshair_color);
                float thick = Globals::scope_crosshair_thickness;
                
                auto* drawList = ImGui::GetBackgroundDrawList();
                // Tam ekran yatay çizgi
                drawList->AddLine(ImVec2(Globals::GameViewportX, cy), ImVec2(Globals::GameViewportX + Globals::ScreenWidth, cy), col, thick);
                // Tam ekran dikey çizgi
                drawList->AddLine(ImVec2(cx, Globals::GameViewportY), ImVec2(cx, Globals::GameViewportY + Globals::ScreenHeight), col, thick);
            }
        }
    }

    auto entities = EntityManager::Get().GetEntities();
    if (localPawn)
    {
        C_CSPlayerPawn* observedPawn = EntityManager::Get().GetLocalObservedPawn();
        const int localTeam = Utils::SafeTeam(localPawn);
        const bool glowEnemyEnabled = Globals::esp_glow || Globals::wireframe_enemy_enabled;
        const bool glowTeamEnabled = Globals::esp_glow_team;
        const Vector localEyePos = GetLocalEyePosition(localPawn);
        const bool canCheckVisibility = Raycasting::Get().IsLoaded();

        auto disableGlow = [](uintptr_t glowProperty)
        {
            Utils::SafeWrite<uint32_t>(glowProperty + Offsets::m_glowColorOverride, 0u);
            Utils::SafeWrite<int>(glowProperty + Offsets::m_iGlowType, 0);
            Utils::SafeWrite<bool>(glowProperty + 0x50, false);
            Utils::SafeWrite<bool>(glowProperty + Offsets::m_bGlowing, false);
        };

        for (const auto& entity : entities)
        {
            C_CSPlayerPawn* pawn = entity.pawn;
            if (!Utils::IsValidPtr(reinterpret_cast<uintptr_t>(pawn))) continue;
            if (pawn == localPawn) continue;
            if (observedPawn && pawn == observedPawn) continue;

            uintptr_t glowProperty = reinterpret_cast<uintptr_t>(pawn) + Offsets::m_Glow;
            if (!Utils::IsValidPtr(glowProperty))
                continue;

            const int pawnTeam = Utils::SafeTeam(pawn);
            const bool isEnemy = (localTeam > 0 && pawnTeam > 0) ? (pawnTeam != localTeam) : entity.isEnemy;
            const bool glowEnabled = isEnemy ? glowEnemyEnabled : glowTeamEnabled;
            const bool isAlive = Utils::SafeAlive(pawn);

            if (!glowEnabled || (!isAlive && !Globals::esp_glow_dead))
            {
                disableGlow(glowProperty);
                continue;
            }

            const float* glowHiddenColor = isEnemy ? Globals::esp_glow_color : Globals::esp_glow_color_team;
            const float* glowVisibleColor = isEnemy ? Globals::esp_glow_color_vis : Globals::esp_glow_color_vis_team;
            bool isVisible = false;
            if (canCheckVisibility && localEyePos.Length() > 0.0f)
            {
                Vector headPos = Utils::GetBonePos(pawn, BoneID::Head);
                if (headPos.Length() > 0.0f)
                    isVisible = Raycasting::Get().IsBoneVisible(localEyePos, headPos);
            }
            const float* glowColor = isVisible ? glowVisibleColor : glowHiddenColor;

            Utils::SafeWrite<float>(glowProperty + 0x8, glowColor[0]);
            Utils::SafeWrite<float>(glowProperty + 0xC, glowColor[1]);
            Utils::SafeWrite<float>(glowProperty + 0x10, glowColor[2]);

            uint8_t r = static_cast<uint8_t>((std::clamp)(glowColor[0], 0.0f, 1.0f) * 255.0f);
            uint8_t g = static_cast<uint8_t>((std::clamp)(glowColor[1], 0.0f, 1.0f) * 255.0f);
            uint8_t b = static_cast<uint8_t>((std::clamp)(glowColor[2], 0.0f, 1.0f) * 255.0f);
            uint8_t a = static_cast<uint8_t>((std::clamp)(glowColor[3], 0.0f, 1.0f) * 255.0f);
            uint32_t color = r | (g << 8) | (b << 16) | (a << 24);
            Utils::SafeWrite<uint32_t>(glowProperty + Offsets::m_glowColorOverride, color);

            Utils::SafeWrite<int>(glowProperty + 0x38, 10000);
            Utils::SafeWrite<int>(glowProperty + 0x3C, 0);
            Utils::SafeWrite<int>(glowProperty + Offsets::m_iGlowType, 3);
            Utils::SafeWrite<bool>(glowProperty + 0x50, true);
            Utils::SafeWrite<bool>(glowProperty + Offsets::m_bGlowing, true);
        }
    }
}

void Visuals::Render()
{
    __try
    {
        RenderInternal();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        
    }

    
    __try
    {
        Chams::Flush();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    
    __try
    {
        if (Globals::chamsv2_enabled || Globals::chamsv2_enabled_team)
            ChamsV2::g_MeshRenderer.Flush();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
