#include "EntityManager.h"
#include "../memory/PatternScan.h"
#include "../memory/Offsets.h"
#include "../utils/Utils.h"
#include <chrono>


static bool SafeIsEnemy(uintptr_t pawnAddr, uintptr_t localAddr) {
    if (!pawnAddr || !localAddr) return false;
    int pawnTeam = Utils::SafeRead<int>(pawnAddr + Offsets::m_iTeamNum);
    int localTeam = Utils::SafeRead<int>(localAddr + Offsets::m_iTeamNum);
    return (localTeam != 0 && pawnTeam != localTeam);
}

static int SafeGetTeam(uintptr_t pawnAddr) {
    return Utils::SafeRead<int>(pawnAddr + Offsets::m_iTeamNum);
}

EntityManager::EntityManager()
{
    uintptr_t client = Memory::GetModuleBase("client.dll");
    entityListAddress = client ? client + Offsets::dwEntityList : 0;
    globalVarsAddress = client ? client + Offsets::dwGlobalVars : 0;
}

EntityManager& EntityManager::Get()
{
    static EntityManager instance;
    return instance;
}

void EntityManager::UpdateInternal()
{
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) return;

    if (!entityListAddress) entityListAddress = client + Offsets::dwEntityList;
    if (!globalVarsAddress) globalVarsAddress = client + Offsets::dwGlobalVars;

    
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    uintptr_t listPtr = Utils::SafeRead<uintptr_t>(entityListAddress);
    if (!Utils::IsValidPtr(listPtr))
        return;

    
    uintptr_t localPawnAddr = Memory::Globals::LocalPawn();
    uintptr_t localCtrlAddr = Utils::SafeRead<uintptr_t>(client + Offsets::dwLocalPlayerController);

    auto currentLocalPawn = reinterpret_cast<C_CSPlayerPawn*>(localPawnAddr);
    auto currentLocalCtrl = reinterpret_cast<C_CSPlayerController*>(localCtrlAddr);

    std::vector<Entity_t> temp;

    // First 64 for players
    for (int i = 0; i < 64; i++)
    {
        uintptr_t listEntry = Utils::SafeRead<uintptr_t>(listPtr + ((8 * (i & 0x7FFF) >> 9) + 16));
        if (!Utils::IsValidPtr(listEntry))
            continue;

        uintptr_t controllerPtr = Utils::SafeRead<uintptr_t>(listEntry + 112 * (i & 0x1FF));
        if (!Utils::IsValidPtr(controllerPtr))
            continue;

        auto controller = reinterpret_cast<C_CSPlayerController*>(controllerPtr);
        
        uint32_t pawnHandle = Utils::SafeRead<uint32_t>(controllerPtr + Offsets::m_hPlayerPawn);
        if (pawnHandle == 0xFFFFFFFF)
            continue;

        uintptr_t pawnListPtr = Utils::SafeRead<uintptr_t>(entityListAddress);
        if (!Utils::IsValidPtr(pawnListPtr))
            continue;

        uintptr_t pawnListEntry = Utils::SafeRead<uintptr_t>(pawnListPtr + (8 * ((pawnHandle & 0x7FFF) >> 9) + 16));
        if (!Utils::IsValidPtr(pawnListEntry))
            continue;

        uintptr_t pawnPtr = Utils::SafeRead<uintptr_t>(pawnListEntry + 112 * (pawnHandle & 0x1FF));
        if (!Utils::IsValidPtr(pawnPtr))
            continue;

        auto pawn = reinterpret_cast<C_CSPlayerPawn*>(pawnPtr);
        if (!pawn) continue;

        uint8_t pawnTeam = Utils::SafeRead<uint8_t>(pawnPtr + Offsets::m_iTeamNum);
        if (pawnTeam < 2 || pawnTeam > 3) continue; 

        Entity_t ent{};
        ent.controller = controller;
        ent.pawn = pawn;
        ent.index = i;
        
        uint8_t localTeam = 0;
        if (currentLocalPawn) localTeam = Utils::SafeRead<uint8_t>(localPawnAddr + Offsets::m_iTeamNum);
        ent.isEnemy = (localTeam != 0 && pawnTeam != localTeam);

        temp.push_back(ent);
    }
    entities = std::move(temp);
    localPawn = currentLocalPawn;
    localController = currentLocalCtrl;
}

void EntityManager::Update()
{
    UpdateInternal();
}

C_BaseEntity* EntityManager::GetEntityFromHandle(uint32_t handle)
{
    if (!handle || handle == 0xFFFFFFFF || !entityListAddress)
        return nullptr;

    __try {
        uintptr_t listPtr = Utils::SafeRead<uintptr_t>(entityListAddress);
        if (!Utils::IsValidPtr(listPtr))
            return nullptr;

        
        uintptr_t entry = Utils::SafeRead<uintptr_t>(listPtr + (8 * ((handle & 0x7FFF) >> 9) + 16));
        if (!Utils::IsValidPtr(entry))
            return nullptr;

        
        uintptr_t entPtr = Utils::SafeRead<uintptr_t>(entry + (112 * (handle & 0x1FF)));
        if (!Utils::IsValidPtr(entPtr))
            return nullptr;

        return reinterpret_cast<C_BaseEntity*>(entPtr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

GlobalVars* EntityManager::GetGlobalVars()
{
    if (!globalVarsAddress) return nullptr;
    uintptr_t addr = Utils::SafeRead<uintptr_t>(globalVarsAddress);
    return reinterpret_cast<GlobalVars*>(addr);
}

C_CSPlayerPawn* EntityManager::GetLocalPawn()
{
    std::shared_lock lock(mutex);
    return localPawn;
}

C_CSPlayerPawn* EntityManager::GetLocalObservedPawn()
{
    C_CSPlayerPawn* local = GetLocalPawn();
    if (!local)
        return nullptr;

    if (Utils::SafeAlive(local))
        return nullptr;

    uintptr_t localAddr = reinterpret_cast<uintptr_t>(local);
    uintptr_t observerServices = Utils::SafeRead<uintptr_t>(localAddr + Offsets::m_pObserverServices);
    if (!Utils::IsValidPtr(observerServices))
        return nullptr;

    uint32_t targetHandle = Utils::SafeRead<uint32_t>(observerServices + Offsets::m_hObserverTarget);
    if (!targetHandle || targetHandle == 0xFFFFFFFF)
        return nullptr;

    C_BaseEntity* observed = GetEntityFromHandle(targetHandle);
    if (!observed)
        return nullptr;

    return reinterpret_cast<C_CSPlayerPawn*>(observed);
}

C_CSPlayerController* EntityManager::GetLocalController()
{
    std::shared_lock lock(mutex);
    return localController;
}

std::vector<Entity_t> EntityManager::GetEntities()
{
    std::shared_lock lock(mutex);
    return entities; 
}

bool EntityManager::IsKnownPlayerPawn(uintptr_t addr) const
{
    if (!addr) return false;

    
    
    
    std::shared_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    for (const auto& e : entities)
    {
        if (reinterpret_cast<uintptr_t>(e.pawn) == addr)
            return true;
    }
    return false;
}
