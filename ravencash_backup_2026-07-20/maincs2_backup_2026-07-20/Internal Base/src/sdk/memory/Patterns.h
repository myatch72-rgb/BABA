#pragma once

namespace Patterns
{
    struct Sig {
        const char* pattern;
        int  dispOffset;
        int  instrLen;
    };

    namespace Data
    {
        inline constexpr Sig pGlobalVars      = { "48 89 15 ? ? ? ? 48 89 42", 3, 7 };
        inline constexpr Sig pGameRules       = { "48 8B 1D ? ? ? ? 48 8D 54 24 ? 0F 28 D0 48 8D 4C 24 ?", 3, 7 };
        inline constexpr Sig pEntityList      = { "48 89 0D ? ? ? ? E9 ? ? ? ? CC", 3, 7 };
        inline constexpr Sig pViewRender      = { "48 89 05 ? ? ? ? 48 8B C8 48 85 C0", 3, 7 };
        inline constexpr Sig pViewMatrix      = { "48 8D 0D ? ? ? ? 48 C1 E0 06", 3, 7 };
        inline constexpr Sig pPlantedC4       = { "0F 85 ? ? ? ? 39 3D ? ? ? ? 7E ? 49 8B 0E 48 8B 1D ? ? ? ? 48 8B D3 4C 8B 81 48 01 00 00", 3, 7 };
        inline constexpr Sig pGlowManager     = { "48 8B 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 41", 3, 7 };
        inline constexpr Sig pLocalController = { "48 8B 05 ? ? ? ? 41 89 BE", 3, 7 };
    }

    namespace Func
    {
        inline const char* GetLocalPawn =
            "48 83 EC ? 83 F9 ? 75 ? 48 8B 0D ? ? ? ? 48 8D 54 24 ? ? ? ? FF 90 ? ? ? ? ? ? 48 63 C1 4C 8D 05";
        inline const char* GetLocalController =
            "E8 ? ? ? ? 48 8B E8 48 85 C0 74 ? 33 DB 39 1D";
    }

    // ====================================================================
    // animationsystem.dll
    // ====================================================================
    namespace AnimationSystem
    {
        inline const char* FrameUpdate =
            "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 EB FF FF B8 38 15 00 00";
        inline const char* PAnimationSystemUtils =
            "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 48 83 EC 28 48 8B CA 48 8D 15";
        inline const char* ShouldUpdateSequences =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 49 8B 40 48";
    }

    // ====================================================================
    // client.dll
    // ====================================================================
    namespace Client
    {
        inline const char* ConCommand_thirdperson =
            "48 83 EC 38 48 8B 0D ? ? ? ? 48 8D 54 24 ? 48 8B 01 FF 90 10 03 00 00 83 7C 24 ? 00 0F 85 ? ? ? ? 4C 8B 05 ? ? ? ? 41 8B 80 50 0B 00 00";
        inline const char* ConCommand_firstperson =
            "48 83 EC 28 48 8B 0D ? ? ? ? 48 8D 54 24 30 48 8B 01 FF 90 ? ? ? ? 83 7C 24 30 00 75 ? 48 8B 05 ? ? ? ? C6 80 29 02 00 00 00 C7 80 A8 06 00 00 00";
        inline const char* ThirdPersonOffHandler =
            "48 83 EC 28 48 8B 0D ? ? ? ? 48 8D 54 24 ? 48 8B 01 FF 90 ? ? ? ? 83 7C 24 ? 00 75 ? 48 8B 05 ? ? ? ? C6 80 29 02 00 00 00 C7 80";
        inline const char* ThirdPersonOnHandler =
            "48 83 EC 38 48 8B 0D ? ? ? ? 48 8D 54 24 ? 48 8B 01 FF 90 10 03 00 00 83 7C 24 ? 00 0F 85 ? ? ? ? 4C 8B 05 ? ? ? ? 41 8B 80 50 0B";
        inline const char* ThirdPersonReset =
            "48 8B 40 08 44 38 ? 75 10 44 88 ? 01";
        inline const char* ChangeModel =
            "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24";
        inline const char* SetModel =
            "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24";
        inline const char* ApplyEconCustomization =
            "48 89 5C 24 ? 57 48 83 EC ? 8B FA 48 8B D9 E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 48 85 C0 74";
        // Updated: build 14175 — extra registers pushed before 55 53
        inline const char* CreateMove =
            "48 8B C4 4C 89 40 18 48 89 48 08 55 53 41 54 41 55";
        inline const char* FrameStageNotify =
            "48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED";
        inline const char* OverrideView =
            "40 57 48 83 EC ? 48 8B FA E8 ? ? ? ? BA";
        inline const char* DrawScopeOverlay =
            "48 8B C4 53 57 48 83 EC ? 48 8B FA";
        inline const char* GetViewModelOffsets =
            "40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D 8B F8 4C 8B F2 48 8B F1 E8";
        inline const char* DrawTeamIntro =
            "48 83 EC ? ? ? ? ? 44 38 89";
        inline const char* HandleTeamIntro_fallback =
            "48 83 EC 28 45 0F B6 08";
        inline const char* SetPlayerReady =
            "40 53 48 83 EC 20 48 8B DA 48 8D 15 ? ? ? ? 48 8B CB FF 15 ? ? ? ? 85 C0 75 14 BA";
        inline const char* MatchFoundHandler =
            "48 85 D2 0F 84 ? ? ? ? 48 8B C4 55 53 56 57 48 8D A8";
        inline const char* PanoramaEvent =
            "40 56 57 41 57 48 83 EC ? 48 8B 3D ? ? ? ? 4D 85 C0";
        inline const char* GetViewmodel =
            "40 55 53 56 41 56 41 57 48 8B EC";
        // Updated: build 14175
        inline const char* RegenerateWeaponSkins =
            "48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10";
        inline const char* GetCustomPaintKitIndex =
            "48 89 5C 24 ? 57 48 83 EC ? 8B 15 ? ? ? ? 48 8B F9 65 48 8B 04 25 ? ? ? ? B9 ? ? ? ? 48 8B 04 D0 8B 04 01 39 05 ? ? ? ? 0F 8F ? ? ? ? E8 ? ? ? ? 8B 58 ? 39 1D ? ? ? ? 74 ? E8 ? ? ? ? 48 8B 15 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 89 05 ? ? ? ? 89 1D ? ? ? ? EB ? 48 8B 05 ? ? ? ? 48 85 C0 74";
        inline const char* GetItemSchema =
            "48 83 EC 28 E8 ? ? ? ? 48 8B 40 08 48 83 C4 28 C3";
        inline const char* GetAttributeDefinition =
            "85 D2 78 ? 3B 91 50 01 00 00 7D ? 48 8B 81 58 01 00 00 48 63 D2 48 8B 04 D0 C3 33 C0 C3";
        // Updated: build 14175 — SetDynamicAttributeValue_raw variant
        inline const char* SetDynamicAttributeValue =
            "48 89 6C 24 ? 57 41 56 41 57 48 81 EC ? ? ? ? 48 8B FA C7 44 24 ? ? ? ? ? 4D 8B F8";
        // Updated: build 14175
        inline const char* UpdateSubClass =
            "4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41 10 48 8B D9 8B 50 30 C1 EA 04";
        // UpdateCompositeMaterial — C_CSWeaponBase_UpdateCompositeMaterial (build 14175)
        inline const char* fnUpdateComposite =
            "E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24";
        // Updated: build 14175 — C_CSWeaponBase_UpdateCompositeMaterialSet
        inline const char* RegenerateWeaponSkin =
            "40 55 53 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 44 0F B6 FA 48 8B D9 BA ? ? ? ? 48 8D 0D ? ? ? ? E8";
        // Updated: build 14175 — third SetMeshGroupMask overload (with 48 8D 99)
        inline const char* SetMeshGroupMask =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71";
        inline const char* CopyEconItemView =
            "40 53 57 48 83 EC ? 48 89 6C 24 ? 48 8B FA 48 89 74 24 ? 48 8B D9 E8 ? ? ? ? 0F B7 87 BA 01 00 00 66 39 83 BA 01 00 00";
        inline const char* GloveApply_PerTick =
            "40 55 56 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B B9 ? 00 00 00 48 8B F1 48 85 FF 0F 84 ? ? ? ? 48 8D";
        // Updated: build 14175 — SetBodyGroup (not SetBodyGroup_inv / SetBodygroup)
        inline const char* SetBodyGroup =
            "85 D2 0F 88 ? ? ? ? 55 56 57 48 83 EC ? 41 8B E8 4C 89 B4 24 ? ? ? ? 8B FA 48 8B F1 E8";
        inline const char* CreateEconItem =
            "48 83 EC 28 B9 48 00 00 00 E8 ? ? ? ? 48 85";
        inline const char* CreateBaseTypeCache =
            "40 57 48 83 EC ? 4C 8B 49 ? 44 8B D2 4C 63 41 ? 48 8B F9 4F 8D 1C C1 49 8B C3";
        inline const char* GetInventoryManager =
            "E8 ? ? ? ? 48 8B D3 48 8B C8 4C 8B 00 41 FF 90 10 02 00 00 48 8B 0D ? ? ? ? 48 85 C9";
        inline const char* EquipItemInLoadout =
            "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 89 54 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 0F B7 FA";
        inline const char* PParticleManager =
            "48 8B 0D ? ? ? ? 41 B8 ? ? ? ? F3 0F 11 74 24 ? 48 C7 44 24 ? ? ? ? ?";
        inline const char* CacheParticleEffect =
            "4C 8B DC 53 48 81 EC ? ? ? ? F2 0F 10 05";
        inline const char* CreateParticleEffect =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? F3 0F 10 1D ? ? ? ? 41 8B F8 8B DA 4C 8D 05";
        inline const char* DestroyParticle =
            "83 FA ? 0F 84 ? ? ? ? 41 54";
        inline const char* C_BaseEntity_GetHitBoxSet =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 8B DA 48 8B F9 E8 ? ? ? ? 48 8B F0";
        inline const char* HitboxToWorldTransform =
            "48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 20 41";
        inline const char* TraceShape =
            "48 89 54 24 ? 48 89 4C 24 ? 55 53 56 57 41 56 41 57 48 8D AC 24 ? ? ? ? B8";
        inline const char* InitFilter =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F B6 41 ? 33 FF 24";
        inline const char* TraceInitData =
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8D 79 ? 33 F6 C7 47";
        inline const char* RagdollGravityScale =
            "72 61 67 64 6F 6C 6C 5F 67 72 61 76 69 74 79 5F 73 63 61 6C 65 00";
        inline const char* PMainMenuPanel =
            "EC ? 48 8B 05 ? ? ? ? 48 8D 15 ? ? ? ? 48";
        inline const char* PUiEngine =
            "48 89 78 ? 48 89 0D ? ? ? ?";
        inline const char* UpdateGlobalVars =
            "48 8B 0D ? ? ? ? 4C 8D 05 ? ? ? ? 48 85 D2";
        inline const char* SetTypeKV3 =
            "40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16 00 00 00 48 8B D9 44 0F 45 C8";
        // UnlockInventory — build 14175 (pattern.txt)
        inline const char* UnlockInventory =
            "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B E9 48 8B 0D ? ? ? ? ? ? ? FF 50";
    }

    // ====================================================================
    // engine2.dll
    // ====================================================================
    namespace Engine2
    {
        inline const char* SetInfo =
            "40 55 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 45 33 FF";
        inline const char* RunCommand =
            "48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 41 56 41 57 48 81 EC ? ? ? ? 0F 29 70 ? 41 0F B6 E9";
    }

    // ====================================================================
    // materialsystem2.dll
    // ====================================================================
    namespace MaterialSystem2
    {
        inline const char* CreateMaterial =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 10 01 00 00 48 8B 05 ? ? ? ? 4C 8B F2";
    }

    // ====================================================================
    // rendersystemdx11.dll
    // ====================================================================
    namespace RenderSystemDX11
    {
        inline const char* GetResourceView =
            "48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 48 89 4C 24 ? 55 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 33 FF 4D 0F BE F8 89 7D ? 45 0F B6 E1 4C 8B 2D";
    }

    // ====================================================================
    // scenesystem.dll
    // ====================================================================
    namespace SceneSystem
    {
        inline const char* DrawAggregateSceneObject =
            "48 8B C4 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70";
        inline const char* GeneratePrimitives =
            "48 8B C4 4C 89 48 ? 4C 89 40 ? 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70";
        inline const char* DrawSkyboxArray =
            "45 85 C9 0F 8E ? ? ? ? 4C 8B DC 55 41 56 49 8D AB 58 FC FF FF 48 81 EC 98 04 00 00";
        inline const char* UpdateLightObject =
            "48 89 54 24 ? 55 57 41 56 48 83 EC";
        inline const char* SIG_DRAW_OBJECT =
            "48 8B C4 53 57 41 54";
    }

    // ====================================================================
    // panorama.dll
    // ====================================================================
    namespace Panorama
    {
        inline const char* RunScript =
            "48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ?";
    }

    // ====================================================================
    // resourcesystem.dll
    // ====================================================================
    namespace ResourceSystem
    {
        inline const char* Precache =
            "40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA";
    }

    // ====================================================================
    // tier0.dll
    // ====================================================================
    namespace Tier0
    {
        inline const char* CreateInterface =
            "4C 8B 0D ? ? ? ? 4C 8B D2 4C 8B D9 4D 85 C9 74 2E 49 8B 41 08 4D 8B C3 4C 2B C0";
    }

    // ====================================================================
    // inputsystem.dll
    // ====================================================================
    namespace InputSystem
    {
        inline const char* SDL_EventHandler =
            "53 48 81 EC ? ? ? ? 8B 02 48 8B DA 2D 00 04 00 00";
    }
}
