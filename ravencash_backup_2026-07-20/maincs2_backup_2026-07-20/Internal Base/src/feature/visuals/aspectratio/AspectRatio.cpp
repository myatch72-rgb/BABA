#include "AspectRatio.h"

#include "../../../../ext/minhook/MinHook.h"
#include "../../../sdk/classes/CViewSetup.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/memory/Patterns.h"
#include "../../../sdk/utils/Globals.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace AspectRatio {
namespace {

using OverrideViewFn = void(__fastcall*)(void*, CViewSetup*);
OverrideViewFn g_originalOverrideView = nullptr;
uintptr_t g_overrideViewAddress = 0;
std::atomic_bool g_overrideEnabled{false};
std::atomic<float> g_overrideValue{16.0f / 9.0f};
std::atomic_bool g_ownedAspectFlag{false};
std::atomic_bool g_callbackLogged{false};
std::atomic_int g_loggedOverrideState{-1};

void Log(const char* line) {
    char appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
        return;
    std::filesystem::path dir = std::filesystem::path(appdata) / "RavenCash";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    FILE* file = nullptr;
    fopen_s(&file, (dir / "features.log").string().c_str(), "a");
    if (!file)
        return;
    std::fprintf(file, "[AspectRatio] %s\n", line);
    std::fclose(file);
}

void __fastcall HookedOverrideView(void* self, CViewSetup* setup) {
    if (g_originalOverrideView)
        g_originalOverrideView(self, setup);

    if (!setup)
        return;

    __try {
        if (!g_callbackLogged.exchange(true, std::memory_order_acq_rel)) {
            char status[192] = {};
            std::snprintf(status, sizeof(status),
                          "OverrideView callback setup=%p native=%.4f flags=0x%02X",
                          static_cast<void*>(setup), setup->flAspectRatio,
                          static_cast<unsigned>(setup->nSomeFlags));
            Log(status);
        }

        const bool enabled =
            g_overrideEnabled.load(std::memory_order_acquire);
        const int state = enabled ? 1 : 0;
        if (g_loggedOverrideState.exchange(state, std::memory_order_acq_rel) !=
            state) {
            Log(enabled ? "CViewSetup override enabled"
                        : "CViewSetup override disabled");
        }

        if (enabled) {
            const float value = g_overrideValue.load(std::memory_order_relaxed);
            if (std::isfinite(value)) {
                setup->flAspectRatio = std::clamp(value, 0.5f, 3.0f);
                setup->nSomeFlags |= 0x02;
                g_ownedAspectFlag.store(true, std::memory_order_release);
            }
        } else if (g_ownedAspectFlag.exchange(false,
                                               std::memory_order_acq_rel)) {
            // We only clear the bit after an override owned by us. The view
            // builder then recalculates the native window aspect this frame.
            setup->nSomeFlags &= static_cast<uint8_t>(~0x02u);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ownedAspectFlag.store(false, std::memory_order_release);
    }
}

} // namespace

void Install() {
    if (g_overrideViewAddress)
        return;

    // ClientModeCSNormal::OverrideView. In build 14169 RDX is CViewSetup*;
    // the full signature is unique at client.dll+0xC987F0.
    g_overrideViewAddress = Memory::PatternScan(
        "client.dll",
        Patterns::Client::OverrideView);
    if (!g_overrideViewAddress) {
        Log("OverrideView pattern not found; override disabled safely");
        return;
    }

    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(g_overrideViewAddress), &HookedOverrideView,
        reinterpret_cast<void**>(&g_originalOverrideView));
    if (createStatus != MH_OK) {
        g_overrideViewAddress = 0;
        g_originalOverrideView = nullptr;
        Log("MH_CreateHook(OverrideView) failed");
        return;
    }

    const MH_STATUS enableStatus =
        MH_EnableHook(reinterpret_cast<void*>(g_overrideViewAddress));
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        MH_RemoveHook(reinterpret_cast<void*>(g_overrideViewAddress));
        g_overrideViewAddress = 0;
        g_originalOverrideView = nullptr;
        Log("MH_EnableHook(OverrideView) failed");
        return;
    }

    Log("OverrideView hook installed (aspect=0x4D4 flags=0x551)");
}

void Run() {
    if (!std::isfinite(Globals::aspect_ratio_value))
        Globals::aspect_ratio_value = 16.0f / 9.0f;
    Globals::aspect_ratio_value =
        std::clamp(Globals::aspect_ratio_value, 0.5f, 3.0f);
    g_overrideValue.store(Globals::aspect_ratio_value,
                          std::memory_order_relaxed);
    g_overrideEnabled.store(Globals::aspect_ratio_enabled,
                            std::memory_order_release);
}

} // namespace AspectRatio
