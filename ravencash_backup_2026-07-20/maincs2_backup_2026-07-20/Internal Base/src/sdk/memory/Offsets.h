#pragma once
#include <cstddef>
#include <cstdint>
#include "Globals.h"


namespace Offsets {

// PATTERN-SCANNED at startup by Memory::Globals::Initialize() — literals
// are only fallbacks if the pattern fails to match.
// Updated on 2026-08-29 09:27:12 UTC from cs2-dumper output
inline uintptr_t dwEntityList = 0x2571220;
inline uintptr_t dwLocalPlayerController = 0x23A0F30;
inline uintptr_t dwViewMatrix = 0x23CB830;
inline uintptr_t dwViewRender = 0x23CB898;
inline uintptr_t dwGlobalVars = 0x20AF5F0;
inline uintptr_t dwGameRules = 0x23C5D28;
inline uintptr_t dwPlantedC4 = 0x2390A18;
inline uintptr_t dwGlowManager = 0x23C2A58;

// MANUAL — no usable RIP-relative pattern found that resolves to the same
// statics cs2-dumper exports. Update these two when the game patches change
// their layout (rare; check %APPDATA%\RavenCash\offset_resolver.log).
// Last synced on 2026-08-29 09:27:12 UTC from the current cs2-dumper output. 
// These are the ONLY two globals not pattern-scanned at runtime, so they MUST 
// be re-synced on every client.dll update. dwViewAngles is still consumed by 
// silent aim; dwCSGOInput remains available to legacy features, while third-person 
// now resolves the native input global independently from a build signature.
inline uintptr_t dwViewAngles = 0x23DC2F8;
inline uintptr_t dwCSGOInput  = 0x23DBC70;

// NEW OFFSETS - Added from cs2-dumper 2026-08-29 09:27:12 UTC
inline uintptr_t dwLocalPlayerPawn = 0x23C6268;
inline uintptr_t dwPrediction = 0x23C6170;
inline uintptr_t dwSensitivity = 0x23C3578;
inline uintptr_t dwWeaponC4 = 0x233EF10;
inline uintptr_t dwGameEntitySystem = 0x2571220;

// REMOVED: dwLocalPlayerPawn — use Memory::Globals::LocalPawn() instead.
constexpr uintptr_t m_flCurTime = 0xC;
constexpr uintptr_t m_iFrameCount = 0x20;
constexpr std::ptrdiff_t m_pItemServices = 0x1210;
constexpr uintptr_t m_gamePhase = 0x84;
constexpr uintptr_t m_nRoundsPlayedThisPhase = 0x8C;

// C_CSGameRules schema offsets — used by SkipTeamIntro etc. Value below is
// a recent observation (2024-Q4 schema dumps). If CS2 patches it, look in
// the public cs2-dumper output under `client.dll/schemas/C_CSGameRules`
// and update this constant. SEH around every read/write keeps a stale
// value from crashing — at worst the feature no-ops silently.
constexpr uintptr_t m_bTeamIntroPeriod = 0xF04;

constexpr uintptr_t m_iHealth = 0x34C;
constexpr uintptr_t m_iTeamNum = 0x3E7;
constexpr uintptr_t m_lifeState = 0x354;

// C_BaseEntity gravity fields — used by the no-gravity-ragdoll feature.
// Live players use the movement-services path so writing these on a player
// pawn has no observable effect; only physics-simulated entities (ragdolls,
// debris, weapon drops on the floor) actually consult them per simulate tick.
constexpr uintptr_t m_flGravityScale         = 0x540;  // float32
constexpr uintptr_t m_bGravityDisabled       = 0x549;  // bool
constexpr uintptr_t m_flActualGravityScale   = 0x564;  // float32 (cached post-multiply)
constexpr uintptr_t m_bGravityActuallyDisabled = 0x568; // bool

// C_RagdollProp / C_ClientRagdoll specific — overrides the per-entity
// gravity multiplier that the ragdoll-solver actually reads.
constexpr uintptr_t m_gravityScale_ragdoll   = 0x1078; // float32
constexpr uintptr_t m_bClientSideRagdoll     = 0x3E6;  // bool on C_BasePlayerPawn
constexpr uintptr_t m_pClientsideRagdoll     = 0x10B0; // CBaseAnimGraph* on C_CSPlayerPawn

// C_CSPlayerPawnBase / C_CSPlayerPawn — networked eye angles. Anti-aim
// pins this to the player's REAL yaw every tick so client-side prediction
// can't drag the local first-person camera toward the fake yaw we shipped
// in pBaseCmd.
constexpr uintptr_t m_angEyeAngles = 0x3350; // QAngle
constexpr uintptr_t m_vOldOrigin = 0x13B8;
constexpr uintptr_t m_pGameSceneNode = 0x330;
constexpr uintptr_t m_pCollision = 0x340;
constexpr uintptr_t m_entitySpottedState = 0x1C60;
constexpr uintptr_t m_bSpotted = 0x8;
constexpr uintptr_t m_bSpottedByMask = 0xC;

constexpr uintptr_t m_vecAbsOrigin = 0xC8;
constexpr uintptr_t m_modelState = 0x140;

// C_Inferno (current cs2-dumper schema, 2026-08-29). VectorWS entries are
// already world-space, so no scene-node transform is applied.
constexpr uintptr_t m_infernoFirePositions = 0x1020; // VectorWS[64]
constexpr uintptr_t m_infernoFireIsBurning = 0x1620; // bool[64]
constexpr uintptr_t m_infernoFireCount = 0x1960;     // int32
constexpr uintptr_t m_infernoType = 0x1964;          // int32
constexpr uintptr_t m_infernoFireLifetime = 0x1968;  // float32

constexpr uintptr_t m_hModel = 0xA0;
constexpr uintptr_t m_ModelName = 0xA8;

constexpr uintptr_t m_vecMins = 0x40;
constexpr uintptr_t m_vecMaxs = 0x4C;

constexpr uintptr_t m_pCameraServices = 0x1240;
constexpr uintptr_t m_pAimPunchServices = 0x14B8;
constexpr uintptr_t m_pWeaponServices = 0x1208;
constexpr uintptr_t m_vecViewOffset = 0xE78;
constexpr uintptr_t m_vViewPunchAngle = 0x48;
constexpr uintptr_t m_vAimPunchAngle = 0x50;
constexpr uintptr_t m_vAimPunchAngleVel = 0x5C;
constexpr uintptr_t m_iShotsFired = 0x1C8C;
constexpr uintptr_t m_fFlags = 0x3F4;
constexpr uintptr_t m_vecVelocity = 0x430;
constexpr uintptr_t m_MoveType = 0x525;
constexpr uintptr_t m_nActualMoveType = 0x526;
constexpr uintptr_t m_flFOVSensitivityAdjust = 0x13B0;
constexpr uintptr_t m_flFOV = 0x80;
constexpr uintptr_t m_bVerticalFOV = 0x609;
constexpr uintptr_t m_flViewmodelFOV = 0x1BA8;
constexpr uintptr_t m_bResumeZoom = 0x1C79;
constexpr uintptr_t m_bIsScoped = 0x1C78;
constexpr uintptr_t m_iIDEntIndex = 0x342C;
constexpr uintptr_t m_bIsBuyMenuOpen = 0x150A;
constexpr uintptr_t m_hActiveWeapon = 0x60;
constexpr uintptr_t m_zoomLevel = 0x1CE0;

constexpr uintptr_t m_iFOV = 0x290;
constexpr uintptr_t m_iFOVStart = 0x294;
constexpr uintptr_t m_flFOVTime = 0x298;
constexpr uintptr_t m_flFOVRate = 0x29C;
constexpr uintptr_t m_hZoomOwner = 0x2A0;
constexpr uintptr_t m_flLastShotFOV = 0x2A4;

constexpr uintptr_t m_iDesiredFOV = 0x78C;
constexpr uintptr_t m_iszPlayerName = 0x6F4;
constexpr uintptr_t m_hPlayerPawn = 0x914;
constexpr uintptr_t m_hObserverPawn = 0x918;
constexpr uintptr_t m_bPawnIsAlive = 0x91C;
// CCSPlayerController -> CCSPlayerController_ActionTrackingServices.
// m_iNumRoundKills is a networked, server-confirmed local kill counter and is
// more reliable than inferring a kill from a pawn disappearing from the list.
constexpr uintptr_t m_pActionTrackingServices = 0x820;
constexpr uintptr_t m_iNumRoundKills = 0x128;
constexpr uintptr_t m_steamID =
    0x780; // CCSPlayerController::m_steamID (uint64)

constexpr uintptr_t m_VData = 0x388;
constexpr uintptr_t m_flZoomTime0 = 0x808;
constexpr uintptr_t m_flZoomTime1 = 0x80C;
constexpr uintptr_t m_flZoomTime2 = 0x810;

constexpr uintptr_t m_bBombTicking = 0x11A0;
constexpr uintptr_t m_nBombSite = 0x11A4;
constexpr uintptr_t m_flC4Blow = 0x11D0;
constexpr uintptr_t m_bBeingDefused = 0x11DC;
constexpr uintptr_t m_flDefuseLength = 0x11EC;
constexpr uintptr_t m_flDefuseCountDown = 0x11F0;
constexpr uintptr_t m_bBombDefused = 0x11F4;
constexpr uintptr_t m_hBombDefuser = 0x11F8;
constexpr uintptr_t m_flSimulationTime = 0x3B8;

constexpr uintptr_t m_bHasDefuser = 0x48;

constexpr uintptr_t m_Glow = 0xDE0;
constexpr uintptr_t m_glowColorOverride = 0x40;
constexpr uintptr_t m_iGlowType = 0x30;
constexpr uintptr_t m_bGlowing = 0x51;

constexpr uintptr_t m_unWeaponHash = 0x14FC;

constexpr uintptr_t m_flEmitSoundTime = 0x1C80;

constexpr uintptr_t m_pBulletServices = 0x1490;
constexpr uintptr_t m_totalHitsOnServer = 0x48;

constexpr uintptr_t m_pObserverServices = 0x1220;

// CCSPlayerController → in-game money services
//   m_pInGameMoneyServices points to CCSPlayerController_InGameMoneyServices
//   inside which m_iAccount holds the player's current cash (int32).
constexpr uintptr_t m_pInGameMoneyServices = 0x810;
constexpr uintptr_t m_iAccount             = 0x40;
constexpr uintptr_t m_hObserverTarget = 0x4C;
constexpr uintptr_t m_hPawn = 0x6BC;

constexpr uintptr_t sc_m_nFallbackPaintKit = 0x1680;
constexpr uintptr_t sc_m_nFallbackStatTrak = 0x168C;
constexpr uintptr_t sc_m_flFallbackWear = 0x1688;
constexpr uintptr_t sc_m_nFallbackSeed = 0x1684;
constexpr uintptr_t sc_m_OriginalOwnerXuidLow = 0x1678;
constexpr uintptr_t sc_m_AttributeManager = 0x11A8;

constexpr uintptr_t sc_m_Item = 0x50;

constexpr uintptr_t sc_m_iItemDefinitionIndex = 0x1BA;
constexpr uintptr_t sc_m_iItemID = 0x1C8;
constexpr uintptr_t sc_m_iItemIDHigh = 0x1D0;
constexpr uintptr_t sc_m_iItemIDLow = 0x1D4;
constexpr uintptr_t sc_m_iAccountID = 0x1D8;
constexpr uintptr_t sc_m_iEntityQuality = 0x1BC;
constexpr uintptr_t sc_m_bInitialized = 0x1E8;
constexpr uintptr_t sc_m_bDisallowSOC = 0x1E9;
constexpr uintptr_t sc_m_szCustomNameOverride = 0x399;
constexpr uintptr_t sc_m_AttributeList = 0x208;
constexpr uintptr_t sc_m_NetworkedDynamicAttributes = 0x280;
constexpr uintptr_t sc_m_bRestoreCustomMaterialAfterPrecache = 0x1B8;

constexpr uintptr_t sc_m_Attributes = 0x8;

constexpr uintptr_t sc_m_hMyWeapons = 0x48;
constexpr uintptr_t sc_m_hActiveWeapon_ws = 0x60;

constexpr uintptr_t sc_m_pChild = 0x40;
constexpr uintptr_t sc_m_pNextSibling = 0x48;
constexpr uintptr_t sc_m_pOwner = 0x30;

constexpr uintptr_t sc_m_hOwnerEntity = 0x520;

constexpr uintptr_t sc_m_MeshGroupMask = 0x208;

constexpr uintptr_t sc_m_flLastSpawnTimeIndex = 0x1404;

constexpr uintptr_t sc_m_bNeedToReApplyGloves = 0x168D;
constexpr uintptr_t sc_m_EconGloves = 0x1690;
constexpr uintptr_t sc_m_nEconGlovesChanged = 0x1B00;
constexpr uintptr_t sc_m_hHudModelArms = 0x1B84;
constexpr uintptr_t sc_m_hViewmodelAttachment = 0x16B0;

// Non-networked client fields verified against the current client.dll.
constexpr uintptr_t sc_m_nSubclassID = 0x380;
constexpr uintptr_t sc_m_GloveComponent = 0x1518;
constexpr uintptr_t sc_m_GloveComponentOwner = 0xA8;

constexpr uintptr_t sc_m_pInventoryServices = 0x818;

constexpr uintptr_t sc_m_unMusicID = 0x58;

constexpr uintptr_t sc_m_pEntity = 0x10;

constexpr uintptr_t sc_m_entityFlags = 0x30;

constexpr uintptr_t sc_m_pDirtyModelData = 0xD8;
constexpr uintptr_t sc_m_DirtyMeshGroupMask = 0x10;

constexpr uintptr_t m_flFlashDuration = 0x1428;
constexpr uintptr_t m_flFlashMaxAlpha = 0x1424;
constexpr uintptr_t m_flFlashScreenshotAlpha = 0x1418;
constexpr uintptr_t m_flFlashOverlayAlpha = 0x141C;
constexpr uintptr_t m_bFlashBuildUp = 0x1420;

constexpr uintptr_t m_vSmokeColor = 0x1284;
constexpr uintptr_t m_nExplodeEffectTickBegin = 0x11F0;
constexpr uintptr_t m_bDidSmokeEffect = 0x127C;
constexpr uintptr_t m_nSmokeEffectTickBegin = 0x1278;

// Tonemap schema fields (cs2-dumper, 2026-08-29). Runtime use is guarded by
// an exact C_TonemapController2 schema-class check.
constexpr uintptr_t world_tonemap_exposure_min = 0x600;
constexpr uintptr_t world_tonemap_exposure_max = 0x604;
constexpr uintptr_t world_tonemap_speed_up = 0x608;
constexpr uintptr_t world_tonemap_speed_down = 0x60C;
constexpr uintptr_t world_tonemap_smoothing = 0x610;
} // namespace Offsets
