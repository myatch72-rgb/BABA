#include "Interfaces.h"
#include "../memory/PatternScan.h"
#include "../memory/Patterns.h"
#include "../utils/Utils.h"
#include <Windows.h>

void Interfaces::IVEngine::ExecuteClientCMD(const char* cmd)
{
    if (!this || !cmd || !*cmd)
        return;

    using RunCommandFn = void(__fastcall*)(void*, std::uint32_t, const char*,
                                            unsigned char, std::int64_t,
                                            std::int64_t);
    static RunCommandFn runCommand = nullptr;
    static bool attempted = false;

    if (!attempted) {
        attempted = true;
        const uintptr_t address = Memory::PatternScan(
            "engine2.dll",
            Patterns::Engine2::RunCommand);
        runCommand = reinterpret_cast<RunCommandFn>(address);
    }

    if (!runCommand)
        return;

    __try {
        runCommand(this, 0, cmd, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool Interfaces::SetupClientOnly()
{
    if (m_pIClient)
        return true;

    __try {
        HMODULE hClient = GetModuleHandleA("client.dll");
        if (!hClient)
            return false;

        using CreateInterfaceFn = void* (*)(const char*, int*);
        auto createInterface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(hClient, "CreateInterface"));
        if (!createInterface)
            return false;

        m_pIClient = createInterface("Source2Client002", nullptr);
        if (!m_pIClient)
            m_pIClient = createInterface("Source2Client001", nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        m_pIClient = nullptr;
    }

    return m_pIClient != nullptr;
}

void Interfaces::Setup()
{
    __try {
        uintptr_t client = Memory::GetModuleBase("client.dll");

        if (client) {
            fnTraceShape = reinterpret_cast<void*>(Memory::PatternScan("client.dll", Patterns::Client::TraceShape));
            fnInitEntitiesOnly = reinterpret_cast<void*>(Memory::PatternScan("client.dll", Patterns::Client::InitFilter));
            fnInitStandard = reinterpret_cast<void*>(Memory::PatternScan("client.dll", Patterns::Client::TraceInitData));
            
            
            SetupClientOnly();

            HMODULE hClient = GetModuleHandleA("client.dll");
            if (hClient) {
                typedef void* (*CreateInterfaceFn)(const char*, int*);
                auto pCreateInterface = (CreateInterfaceFn)GetProcAddress(hClient, "CreateInterface");
                if (pCreateInterface) {
                    m_pLegacyGameUI = reinterpret_cast<ILegacyGameUI*>(
                        pCreateInterface("LegacyGameUI001", nullptr));
                }
            }
        }

        // The previous trace-manager and scene-system pointer signatures were
        // ambiguous (thousands/two matches respectively) and neither pointer
        // has a consumer. Keep them null instead of caching arbitrary globals.

        {
            HMODULE hParticles = GetModuleHandleA("particles.dll");
            if (hParticles) {
                typedef void* (*CreateInterfaceFn)(const char*, int*);
                auto pCreateInterface = (CreateInterfaceFn)GetProcAddress(hParticles, "CreateInterface");
                if (pCreateInterface) {
                    m_pParticleSystemMgr = pCreateInterface("ParticleSystemMgr003", nullptr);
                }
            }
        }

        
        {
            HMODULE hEngine = GetModuleHandleA("engine2.dll");
            if (hEngine) {
                typedef void* (*CreateInterfaceFn)(const char*, int*);
                auto pCreateInterface = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
                if (pCreateInterface) {
                    m_pEngine = reinterpret_cast<IVEngine*>(
                        pCreateInterface("Source2EngineToClient001", nullptr));
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
