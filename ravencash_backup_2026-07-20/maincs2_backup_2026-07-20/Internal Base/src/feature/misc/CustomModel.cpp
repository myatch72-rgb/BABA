#include "CustomModel.h"

#include "../../sdk/memory/Globals.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/memory/Patterns.h"
#include "../../sdk/utils/Globals.h"

#include <Windows.h>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include "../../core/CrashTelemetry.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// CBufferString shim
// ---------------------------------------------------------------------------
struct CBufferString
{
    int  m_nLength        = 0;
    int  m_nAllocatedSize = (int)(0x80000000u | 0x40000000u | 8u);
    union { char* m_pString; char m_szInline[8]; };
    CBufferString() { m_pString = nullptr; }
};

// ---------------------------------------------------------------------------
// Resolved function pointers (filled once on first Apply())
// ---------------------------------------------------------------------------
using SetModelFn  = void*(__fastcall*)(void*, const char*);
using PrecacheFn  = void*(__fastcall*)(void*, CBufferString*, const char*);
using InsertFn    = const char*(__fastcall*)(CBufferString*, int, const char*, int, bool);

static SetModelFn  s_fnSetModel  = nullptr;
static PrecacheFn  s_fnPrecache  = nullptr;
static InsertFn    s_fnInsert    = nullptr;
static void*       s_pIRS        = nullptr;
static bool        s_resolved    = false;

static void ResolveOnce()
{
    if (s_resolved) return;
    s_resolved = true;

    // SetModel — same sig as in SkinChanger
    uintptr_t addr = Memory::PatternScan(
        "client.dll",
        Patterns::Client::SetModel);
    if (addr) s_fnSetModel = reinterpret_cast<SetModelFn>(addr);

    // ResourceSystem013 via CreateInterface exported from resourcesystem.dll
    HMODULE hRS = GetModuleHandleA("resourcesystem.dll");
    if (hRS)
    {
        using CreateInterfaceFn = void*(*)(const char*, int*);
        auto pCI = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(hRS, "CreateInterface"));
        if (pCI)
            s_pIRS = pCI("ResourceSystem013", nullptr);
    }

    // Precache (block-load) inside resourcesystem.dll
    uintptr_t bloadAddr = Memory::PatternScan(
        "resourcesystem.dll",
        Patterns::ResourceSystem::Precache);
    if (bloadAddr) s_fnPrecache = reinterpret_cast<PrecacheFn>(bloadAddr);

    // CBufferString::Insert exported from tier0.dll
    HMODULE hTier0 = GetModuleHandleA("tier0.dll");
    if (hTier0)
        s_fnInsert = reinterpret_cast<InsertFn>(
            GetProcAddress(hTier0, "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z"));
}

// ---------------------------------------------------------------------------
// Raw SEH helpers — no C++ objects with destructors inside __try
// ---------------------------------------------------------------------------
static void RawPrecache(const char* path)
{
    if (!s_fnPrecache || !s_pIRS || !s_fnInsert) return;

    __try {
        CBufferString names;
        s_fnInsert(&names, 0, path, -1, false);

        void*      pIRS = s_pIRS;
        PrecacheFn fn   = s_fnPrecache;

        fn(pIRS, &names, "");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void RawSetModel(uintptr_t pawnAddr, const char* path)
{
    SetModelFn fn   = s_fnSetModel;
    void*      pawn = reinterpret_cast<void*>(pawnAddr);

    __try { fn(pawn, path); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
static std::string GetModelsRoot()
{
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string root = cwd;

    auto pos = root.find("bin\\win64");
    if (pos != std::string::npos)
    {
        root.replace(pos, 9, "csgo\\characters\\models");
        return root;
    }

    pos = root.find("game\\csgo");
    if (pos != std::string::npos)
        return root.substr(0, pos) + "game\\csgo\\characters\\models";

    return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
           "Counter-Strike Global Offensive\\game\\csgo\\characters\\models";
}

// ---------------------------------------------------------------------------
// Public state
// ---------------------------------------------------------------------------
namespace CustomModel
{
    std::mutex              g_ModelMutex;
    std::vector<ModelEntry> g_ModelList;
    int                     g_SelectedIdx = 0;
    bool                    g_NeedApply   = false;

    void Scan()
    {
        std::lock_guard<std::mutex> lock(g_ModelMutex);
        g_ModelList.clear();
        g_ModelList.push_back({ "[ OFF ]", "" });

        // Preset models included in MODELLER folder (paths match game/csgo/ on disk)
        static const std::vector<ModelEntry> presets = {
            { "2B Nier Automata", "characters/models/nozb1/2b_nier_automata_player_model/2b_nier_player_model.vmdl" },
            { "Adult Neptune", "characters/models/nozb1/adult_neptune_player_model/adult_neptune_player_model.vmdl" },
            { "Adult Jailer", "characters/models/nozb1/adult_neptune_player_model/adult_jailer.vmdl" },
            { "Lego Batman", "characters/models/nozb1/lego_pack_player_model/lego_batman_player_model/lego_batman.vmdl" },
            { "Muscular Gura", "characters/models/nozb1/muscular_gura_player_model/muscular_gura_player_model.vmdl" },
            { "Spider-Man (NWH)", "characters/models/nozb1/spider_no_way_home_player_model/spydernw_player_model.vmdl" },
            { "T-Rex Noun", "characters/models/nozb1/trex_noun_player_model/trex_noun_player_model.vmdl" },
            { "Yoshino", "characters/models/nozb1/yoshino_player_model/yoshino_player_model.vmdl" }
        };

        for (const auto& preset : presets) {
            g_ModelList.push_back(preset);
        }

        std::string root = GetModelsRoot();

        std::error_code ec;
        if (fs::exists(root, ec))
        {
            for (auto& entry : fs::recursive_directory_iterator(root, ec))
            {
                if (ec) { ec.clear(); continue; }
                auto ext = entry.path().extension().string();
                if (ext != ".vmdl_c" && ext != ".vmdl") continue;

                std::string stem = entry.path().stem().string();
                if (stem == "no_hitbox") continue;

                std::string full = entry.path().string();
                size_t charPos = full.find("characters\\");
                if (charPos == std::string::npos) continue;

                std::string rel = full.substr(charPos);
                std::replace(rel.begin(), rel.end(), '\\', '/');

                if (rel.size() > 7 && rel.substr(rel.size() - 7) == ".vmdl_c")
                    rel = rel.substr(0, rel.size() - 7) + ".vmdl";

                bool exists = false;
                for (const auto& item : g_ModelList)
                {
                    if (item.resourcePath == rel) { exists = true; break; }
                }
                if (!exists) {
                    g_ModelList.push_back({ stem, rel });
                }
            }
        }

        if (g_SelectedIdx >= (int)g_ModelList.size())
            g_SelectedIdx = 0;
    }

    void Apply()
    {
        ResolveOnce();

        if (!g_NeedApply) return;

        if (!Globals::custommodel_enabled)
        {
            g_NeedApply = false;
            return;
        }

        std::string pathStr;
        {
            std::lock_guard<std::mutex> lock(g_ModelMutex);
            if (g_SelectedIdx <= 0 || g_SelectedIdx >= (int)g_ModelList.size())
            {
                g_NeedApply = false;
                return;
            }
            pathStr = g_ModelList[g_SelectedIdx].resourcePath;
        }

        if (pathStr.empty()) { g_NeedApply = false; return; }

        uintptr_t localPawn = Memory::Globals::LocalPawn();
        if (!localPawn || localPawn < 0x100000) return; // pawn not ready yet, retry next frame

        if (!s_fnSetModel) { g_NeedApply = false; return; }

        // Validate model file exists on disk to prevent SetModel from nulling pawn model
        std::string testPath = pathStr;
        if (testPath.size() > 5 && testPath.substr(testPath.size() - 5) == ".vmdl")
            testPath += "_c";

        std::string root = GetModelsRoot();
        std::string fullPath = root + "/" + testPath;
        std::string directCsgo = "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/" + testPath;

        std::error_code ec;
        bool exists = fs::exists(fullPath, ec) || fs::exists(directCsgo, ec) || fs::exists(testPath, ec);
        if (!exists)
        {
            CrashTelemetry::Trace("CustomModel file not found on disk: %s", testPath.c_str());
            g_NeedApply = false;
            return;
        }

        RawPrecache(pathStr.c_str());
        RawSetModel(localPawn, pathStr.c_str());
        CrashTelemetry::Trace("CustomModel applied successfully: %s", pathStr.c_str());

        g_NeedApply = false;
    }

    void PrecacheModel(const char* path)
    {
        ResolveOnce();
        RawPrecache(path);
    }

} // namespace CustomModel

namespace CustomWeapon
{
    std::vector<CustomModel::ModelEntry> g_ModelList;
    int                                  g_SelectedIdx = 0;
    bool                                 g_NeedApply   = false;

    static std::string GetWeaponsRoot()
    {
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        std::string root = cwd;

        auto pos = root.find("bin\\win64");
        if (pos != std::string::npos)
        {
            root.replace(pos, 9, "csgo\\weapons");
            return root;
        }

        pos = root.find("game\\csgo");
        if (pos != std::string::npos)
            return root.substr(0, pos) + "game\\csgo\\weapons";

        return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
               "Counter-Strike Global Offensive\\game\\csgo\\weapons";
    }

    void Scan()
    {
        g_ModelList.clear();
        g_ModelList.push_back({ "[ OFF ]", "" });

        std::string root = GetWeaponsRoot();

        std::error_code ec;
        if (!fs::exists(root, ec)) return;

        for (auto& entry : fs::recursive_directory_iterator(root, ec))
        {
            if (ec) { ec.clear(); continue; }
            auto ext = entry.path().extension().string();
            if (ext != ".vmdl_c" && ext != ".vmdl") continue;

            std::string full = entry.path().string();

            size_t charPos = full.find("weapons\\");
            if (charPos == std::string::npos) continue;

            std::string rel = full.substr(charPos);
            std::replace(rel.begin(), rel.end(), '\\', '/');

            if (rel.size() > 7 && rel.substr(rel.size() - 7) == ".vmdl_c")
                rel = rel.substr(0, rel.size() - 7) + ".vmdl";

            g_ModelList.push_back({ entry.path().stem().string(), rel });
        }

        if (g_SelectedIdx >= (int)g_ModelList.size())
            g_SelectedIdx = 0;
    }

    void Apply()
    {
        if (!g_NeedApply) return;
        g_NeedApply = false;
        
        Globals::sc_force_update = 1;
    }
} // namespace CustomWeapon
