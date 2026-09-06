#pragma once
#include <vector>
#include <shared_mutex>
#include "Classes.h"

struct Entity_t
{
    C_CSPlayerController* controller = nullptr;
    C_CSPlayerPawn* pawn = nullptr;
    int index = -1;
    bool isEnemy = false;
};

class EntityManager
{
private:
    std::vector<Entity_t> entities;
    mutable std::shared_mutex mutex;
    uintptr_t entityListAddress = 0;
    uintptr_t globalVarsAddress = 0;
    C_CSPlayerPawn* localPawn = nullptr;
    C_CSPlayerController* localController = nullptr;

    EntityManager();
    void UpdateInternal();

public:
    static EntityManager& Get();

    void Update();

    C_CSPlayerPawn* GetLocalPawn();
    C_CSPlayerPawn* GetLocalObservedPawn();
    C_CSPlayerController* GetLocalController();
    GlobalVars* GetGlobalVars();
    std::vector<Entity_t> GetEntities();
    C_BaseEntity* GetEntityFromHandle(uint32_t handle);

    
    
    
    bool IsKnownPlayerPawn(uintptr_t addr) const;
};
