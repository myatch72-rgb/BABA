#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace CustomModel
{
    struct ModelEntry
    {
        std::string displayName;
        std::string resourcePath;
    };

    extern std::mutex              g_ModelMutex;
    extern std::vector<ModelEntry> g_ModelList;
    extern int                     g_SelectedIdx;
    extern bool                    g_NeedApply;

    // Scan csgo/characters/models for .vmdl_c files and populate g_ModelList.
    void Scan();

    // Called every frame from the frame stage hook — applies the selected model if flagged.
    void Apply();

    // Exposed for SkinChanger (safe to call from game thread hooks)
    void PrecacheModel(const char* path);
}

namespace CustomWeapon
{
    extern std::vector<CustomModel::ModelEntry> g_ModelList;
    extern int                                  g_SelectedIdx;
    extern bool                                 g_NeedApply;

    void Scan();
    void Apply();
}
