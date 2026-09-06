#pragma once
#include <cstdint>
#include <excpt.h>
#include "../classes/SceneSystem.h"


class ILegacyGameUI {
public:
    void show_message_box(const char* title, const char* message,
        bool show_ok = true, bool show_cancel = false,
        const char* ok_command = nullptr, const char* cancel_command = nullptr,
        const char* closed_command = nullptr, const char* legend = nullptr,
        const char* unknown = nullptr)
    {
        
        using fn = void(__fastcall*)(void*, const char*, const char*,
            bool, bool, const char*, const char*, const char*, const char*, const char*);
        void** vtable = *reinterpret_cast<void***>(this);
        reinterpret_cast<fn>(vtable[28])(this, title, message,
            show_ok, show_cancel, ok_command, cancel_command,
            closed_command, legend, unknown);
    }
};

namespace Interfaces
{
    inline void* m_pTraceManager = nullptr;
    inline void* fnTraceShape = nullptr;
    inline void* fnInitEntitiesOnly = nullptr;
    inline void* fnInitStandard = nullptr;
    inline ISceneSystem* m_pSceneSystem = nullptr;
    inline void* m_pIClient = nullptr; 
    inline ILegacyGameUI* m_pLegacyGameUI = nullptr;
    inline void* m_pParticleSystemMgr = nullptr;

    
    class IVEngine {
    public:
        // Resolves engine2's current RunCommand implementation by signature.
        // The old hard-coded vtable[50] now points at an unrelated function.
        void ExecuteClientCMD(const char* cmd);
    };

    inline IVEngine* m_pEngine = nullptr;

    // Captures only Source2Client. Inventory runtime activation uses this
    // narrow path after Present is stable instead of resolving every optional
    // engine/particle/trace interface during startup.
    bool SetupClientOnly();
    void Setup();
}
