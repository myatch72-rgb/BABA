#include "RCS.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../menu/Menu.h"
#include <Windows.h>

namespace RCS {

static Vector lastPunch = { 0, 0, 0 };
static uint32_t lastWeaponHash = 0;

void Run()
{
    if (!Globals::rcs_enabled)
    {
        lastPunch = { 0, 0, 0 };
        return;
    }

    
    if (Menu::IsOpen) return;

    auto localPawn = EntityManager::Get().GetLocalPawn();
    if (!localPawn || !Utils::SafeAlive(localPawn))
    {
        lastPunch = { 0, 0, 0 };
        return;
    }

    
    uint32_t currentWeaponHash = Utils::SafeRead<uint32_t>(reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_unWeaponHash);
    if (currentWeaponHash != lastWeaponHash)
    {
        lastPunch = { 0, 0, 0 };
        lastWeaponHash = currentWeaponHash;
    }

    
    // Get Aim Punch Angle from Services
    uintptr_t aimPunchServices = Utils::SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_pAimPunchServices);
    if (!Utils::IsValidPtr(aimPunchServices)) return;

    Vector punchAngle = Utils::SafeRead<Vector>(aimPunchServices + Offsets::m_vAimPunchAngle);
    int shotsFired = Utils::SafeRead<int>(reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_iShotsFired);
    
    
    if (shotsFired > 1 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
    {
        
        Vector delta = punchAngle - lastPunch;

        
        if (delta.x > 0.0f) delta.x = 0.0f;

        
        float smartGain = 15.5f * Globals::rcs_calibration;

        
        float sensitivityScale = Utils::SafeRead<float>(reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_flFOVSensitivityAdjust, 1.0f);
        if (sensitivityScale <= 0.0f) sensitivityScale = 1.0f; 
        
        int moveX = (int)(delta.y * (2.0f * Globals::rcs_amount * smartGain * sensitivityScale));
        int moveY = (int)(-delta.x * (2.0f * Globals::rcs_amount * smartGain * sensitivityScale));

        if (moveX != 0 || moveY != 0)
        {
            mouse_event(MOUSEEVENTF_MOVE, moveX, moveY, 0, 0);
        }
    }
    
    lastPunch = punchAngle;
}

}
