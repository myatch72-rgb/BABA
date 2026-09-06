#pragma once
#include <cstdint>
#include "../memory/Offsets.h"
#include "../utils/Vector.h"
#include "../utils/SafeMemory.h"
#include "Hitbox.h"
#include "../memory/PatternScan.h"
#include "../memory/Patterns.h"
#define SCHEMA(type, name, offset) \
    type name() const { \
        if (!this) return type{}; \
        return *reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(this) + offset); \
    }

class GlobalVars {
public:
    SCHEMA(float, m_flCurTime, Offsets::m_flCurTime);
    SCHEMA(int, m_iFrameCount, Offsets::m_iFrameCount);
};

class CGameSceneNode
{
public:
    SCHEMA(uintptr_t, m_modelState, Offsets::m_modelState);
    SCHEMA(Vector, m_vecAbsOrigin, Offsets::m_vecAbsOrigin);
};

class CCollisionProperty {
public:
    SCHEMA(Vector, m_vecMins, Offsets::m_vecMins);
    SCHEMA(Vector, m_vecMaxs, Offsets::m_vecMaxs);
};

class C_BaseEntity
{
public:
    SCHEMA(int, m_iHealth, Offsets::m_iHealth);
    SCHEMA(uint8_t, m_iTeamNum, Offsets::m_iTeamNum);
    SCHEMA(uint8_t, m_lifeState, Offsets::m_lifeState);
    SCHEMA(Vector, m_vOldOrigin, Offsets::m_vOldOrigin);
    SCHEMA(uintptr_t, m_pGameSceneNode, Offsets::m_pGameSceneNode);
    SCHEMA(uintptr_t, m_pCollision, Offsets::m_pCollision);

    bool IsVisible() const {
        if (!this) return false;
        uintptr_t spottedStateAddr = reinterpret_cast<uintptr_t>(this) + Offsets::m_entitySpottedState;
        if (!spottedStateAddr) return false;

        bool bSpotted = *reinterpret_cast<bool*>(spottedStateAddr + Offsets::m_bSpotted);
        uint32_t mask = *reinterpret_cast<uint32_t*>(spottedStateAddr + Offsets::m_bSpottedByMask);
        
        return bSpotted || (mask != 0);
    }

    void SetSpotted(bool state) {
        if (!this) return;
        uintptr_t spottedStateAddr = reinterpret_cast<uintptr_t>(this) + Offsets::m_entitySpottedState;
        if (!spottedStateAddr) return;
        *reinterpret_cast<bool*>(spottedStateAddr + Offsets::m_bSpotted) = state;
    }

    bool IsAlive() const { return m_iHealth() > 0; }
};

class C_CSPlayerPawn : public C_BaseEntity
{
public:
    SCHEMA(Vector, m_vecViewOffset, Offsets::m_vecViewOffset);
    SCHEMA(int, m_iShotsFired, Offsets::m_iShotsFired);
    SCHEMA(uintptr_t, m_pWeaponServices, Offsets::m_pWeaponServices);

    Vector m_aimPunchAngle() const {
        if (!this) return Vector{0, 0, 0};
        uintptr_t services = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(this) + Offsets::m_pAimPunchServices);
        if (!services) return Vector{0, 0, 0};
        return *reinterpret_cast<Vector*>(services + Offsets::m_vAimPunchAngle);
    }

    bool IsVisible() const {
        return C_BaseEntity::IsVisible();
    }

    CHitBoxSet* GetHitboxSet(int i) {
        if (!this) return nullptr;
        using fn = CHitBoxSet* (__thiscall*)(void*, int);
        static auto addr = Memory::PatternScan(
            "client.dll",
            Patterns::Client::C_BaseEntity_GetHitBoxSet);
        if (!addr)
            return nullptr;
     
        const auto get_hitbox_set = reinterpret_cast<fn>(addr);
        return get_hitbox_set(this, i);
    }

    int HitboxToWorldTransform(CHitBoxSet* hitbox_set, CTransform* out_transform) {
        if (!this) return -1;
        using fn = int(__thiscall*)(void*, CHitBoxSet*, CTransform*, int max_studio_bones);
        static auto addr = Memory::PatternScan(
            "client.dll",
            Patterns::Client::HitboxToWorldTransform);
        if (!addr)
            return -1;
     
        const auto hitbox_to_world_transform = reinterpret_cast<fn>(addr);
        return hitbox_to_world_transform(this, hitbox_set, out_transform, 1024);
    }
};

class CPlayer_WeaponServices {
public:
    SCHEMA(uint32_t, m_hActiveWeapon, Offsets::m_hActiveWeapon);
};

class CWeaponVData {
public:
    SCHEMA(uintptr_t, m_szName, 0x640);
};
class C_CSWeaponBase : public C_BaseEntity {
public:
    SCHEMA(uintptr_t, m_VData, Offsets::m_VData);
};

class C_CSPlayerController : public C_BaseEntity
{
public:
    SCHEMA(uint32_t, m_hPlayerPawn, Offsets::m_hPlayerPawn);
    uintptr_t m_iszPlayerNameAddr() const {
        return reinterpret_cast<uintptr_t>(this) + Offsets::m_iszPlayerName;
    }
    SCHEMA(bool, m_bPawnIsAlive, Offsets::m_bPawnIsAlive);
};