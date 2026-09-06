#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <unordered_map>




enum WeaponsEnum : uint16_t
{
    WEP_NONE = 0,
    WEP_Deagle = 1,
    WEP_Elite = 2,
    WEP_FiveSeven = 3,
    WEP_Glock = 4,
    WEP_Ak47 = 7,
    WEP_Aug = 8,
    WEP_Awp = 9,
    WEP_Famas = 10,
    WEP_G3Sg1 = 11,
    WEP_Galil = 13,
    WEP_M249 = 14,
    WEP_M4A4 = 16,
    WEP_Mac10 = 17,
    WEP_P90 = 19,
    WEP_Mp5SD = 23,
    WEP_Ump45 = 24,
    WEP_Xm1014 = 25,
    WEP_Bizon = 26,
    WEP_Mag7 = 27,
    WEP_Negev = 28,
    WEP_Sawedoff = 29,
    WEP_Tec9 = 30,
    WEP_Zeus = 31,
    WEP_P2000 = 32,
    WEP_Mp7 = 33,
    WEP_Mp9 = 34,
    WEP_Nova = 35,
    WEP_P250 = 36,
    WEP_Scar20 = 38,
    WEP_Sg556 = 39,
    WEP_Ssg08 = 40,
    WEP_CtKnife = 42,
    WEP_FlashBang = 43,
    WEP_HeGrenade = 44,
    WEP_SmokeGrenade = 45,
    WEP_Molotov = 46,
    WEP_Decoy = 47,
    WEP_IncGrenade = 48,
    WEP_C4 = 49,
    WEP_TKnife = 59,
    WEP_M4A1S = 60,
    WEP_UspS = 61,
    WEP_Cz75A = 63,
    WEP_Revolver = 64,
};




struct SkinInfo_t
{
    int paintKit = 0;
    WeaponsEnum weaponType = WEP_NONE;
    std::string name = "";
    float wear = 0.001f;
    int seed = 0;
    int statTrak = -1;
    int rarity = 1;
    std::string image_url = "";
    // Source 2 ships two weapon mesh groups. Paint kits marked as legacy
    // must render on group 2 or their UVs/materials appear shifted.
    bool legacy = false;
    std::string description = "";
    std::string collection = "";
    std::string source = "";
    std::string weaponName = "";
    std::string patternName = "";
    std::string rarityName = "";
    float minFloat = 0.0f;
    float maxFloat = 1.0f;
};




inline std::unordered_map<uint16_t, std::string> KnifeModels = {
    {500, "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl"},
    {503, "weapons/models/knife/knife_css/weapon_knife_css.vmdl"},
    {505, "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl"},
    {506, "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl"},
    {507, "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl"},
    {508, "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl"},
    {509, "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl"},
    {512, "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl"},
    {514, "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl"},
    {515, "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl"},
    {516, "weapons/models/knife/knife_push/weapon_knife_push.vmdl"},
    {517, "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl"},
    {518, "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl"},
    {519, "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl"},
    {520, "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl"},
    {521, "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl"},
    {522, "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl"},
    {523, "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl"},
    {525, "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl"},
    {526, "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl"}
};

inline std::map<uint16_t, std::string> KnifeNames = {
    {500, "Bayonet"},
    {503, "Classic"},
    {505, "Flip"},
    {506, "Gut"},
    {507, "Karambit"},
    {508, "M9 Bayonet"},
    {509, "Huntsman"},
    {512, "Falchion"},
    {514, "Bowie"},
    {515, "Butterfly"},
    {516, "Daggers"},
    {517, "Paracord"},
    {518, "Survival"},
    {519, "Ursus"},
    {520, "Navaja"},
    {521, "Nomad"},
    {522, "Stiletto"},
    {523, "Talon"},
    {525, "Skeleton"},
    {526, "Kukri"}
};

struct Knife_t
{
    uint16_t defIndex = 0;
    std::string name = "";
    std::string model = "";

    Knife_t(uint16_t def = 0) : defIndex(def)
    {
        if (def && KnifeNames.count(def) && KnifeModels.count(def))
        {
            name = KnifeNames.at(def);
            model = KnifeModels.at(def);
        }
    }
};

inline std::vector<Knife_t> Knives = {
    Knife_t(500), Knife_t(503), Knife_t(505), Knife_t(506), Knife_t(507),
    Knife_t(508), Knife_t(509), Knife_t(512), Knife_t(514), Knife_t(515),
    Knife_t(516), Knife_t(517), Knife_t(518), Knife_t(519), Knife_t(520),
    Knife_t(521), Knife_t(522), Knife_t(523), Knife_t(525), Knife_t(526)
};




struct Glove_t
{
    uint16_t defIndex = 0;
    int paintKit = 0;
    std::string name = "";
    float wear = 0.001f;
    int seed = 0;

    Glove_t(uint16_t _def = 0, int _paint = 0, std::string _name = "",
            float _wear = 0.001f, int _seed = 0)
        : defIndex(_def), paintKit(_paint), name(_name), wear(_wear),
          seed(_seed) {}
};

inline std::vector<Glove_t> GloveTypes = {
    Glove_t(5027, 0, "Bloodhound Gloves"),
    Glove_t(5030, 0, "Sport Gloves"),
    Glove_t(5031, 0, "Driver Gloves"),
    Glove_t(5032, 0, "Hand Wraps"),
    Glove_t(5033, 0, "Moto Gloves"),
    Glove_t(5034, 0, "Specialist Gloves"),
    Glove_t(5035, 0, "Hydra Gloves"),
    Glove_t(4725, 0, "Broken Fang Gloves")
};




struct MusicKitInfo_t
{
    uint16_t id;
    const char* name;
};

inline std::vector<MusicKitInfo_t> MusicKits = {
    {1,  "Counter-Strike 2"}, {3,  "Crimson Assault"}, {4,  "Sharpened"},
    {5,  "Insurgency"}, {6,  "A|D|8"}, {7,  "High Noon"},
    {8,  "Death's Head Demolition"}, {9,  "Desert Fire"}, {10, "LNOE"},
    {11, "Metal"}, {12, "All I Want For Christmas"}, {13, "Iso Rhythm"},
    {14, "For No Mankind"}, {15, "Hotline Miami"}, {16, "The 8-Bit Kit"},
    {17, "The Talos Principle"}, {18, "Battlepack"}, {19, "Molotov"},
    {20, "Uber Blasto Phone"}, {21, "Hazardous Environments"},
    {22, "Headshot"}, {23, "Total Domination"}, {24, "I Am"},
    {25, "Diamonds"}, {26, "Invasion!"}, {27, "Lion's Mouth"},
    {28, "Sponge Fingerz"}, {29, "Aggressive"}, {30, "Java Havana Funkaloo"},
    {31, "Moments CSGO"}, {32, "Disgusting"}, {33, "The Good Youth"},
    {34, "FREE"}, {35, "Life's Not Out To Get You"}, {36, "Backbone"},
    {37, "GLA"}, {38, "Arena"}, {39, "EZ4ENCE"}, {40, "Halo"},
    {41, "King, Scar"}, {42, "Anti-Citizen"}, {43, "Bachram"},
    {44, "Taco Truck"}, {45, "Eye of the Dragon"}, {46, "M.U.D.D. FORCE"},
    {47, "Neo Noir"}, {48, "Bodacious"}, {49, "Drifter"},
    {50, "All For Dust"}, {51, "Hades"}, {52, "The Lowfile Pack"},
    {53, "CHAINSAW Lxadxut"}, {54, "Mocha Petal"}, {55, "Yellow Magic"},
    {56, "VICI"}, {57, "Atro Bellum"}, {58, "Work Hard, Play Hard"},
    {59, "Kolibri"}, {60, "U Mad!"}, {61, "Flashbang Dance"},
    {62, "Heading For The Source"}, {63, "Void"}, {64, "Shooters"},
    {65, "Dashstar*"}, {66, "Gothic Luxury"}, {67, "Lock Me Up"},
    {68, "Hua Lian"}, {69, "Ultimate"}, {75, "CS:GO"}, {76, "Under Bright Lights"},
};




struct Agent_t
{
    uint16_t defIndex = 0;
    std::string name = "";
    std::string model = "";
    int team = 0; 

    Agent_t(uint16_t _def = 0, std::string _name = "", std::string _model = "", int _team = 0)
        : defIndex(_def), name(_name), model(_model), team(_team) {}
};

struct AgentInfo_t
{
    uint16_t defIndex = 0;
    std::string name = "";
    std::string model = "";
    std::string team = "";
    int rarity = 1;
    std::string image_url = "";
};

inline std::vector<Agent_t> AgentsCT = {
    Agent_t(4680, "Default", "agents/models/ctm_sas/ctm_sas.vmdl", 3),
    Agent_t(4619, "SEAL Team 6 Soldier", "agents/models/ctm_st6/ctm_st6_variante.vmdl", 3),
    Agent_t(4711, "Special Agent Ava", "agents/models/ctm_fbi/ctm_fbi_variantb.vmdl", 3),
    Agent_t(4730, "John 'Van Healen' Kask", "agents/models/ctm_swat/ctm_swat_variantg.vmdl", 3),
    Agent_t(4816, "Cmdr. Davida 'Goggles' Fernandez | SEAL Frogman", "agents/models/ctm_diver/ctm_diver_varianta.vmdl", 3),
    Agent_t(4817, "Nazi Kar", "agents/models/ctm_gendarmerie/ctm_gendarmerie_variantc.vmdl", 3),
};

inline std::vector<Agent_t> AgentsT = {
    Agent_t(4619, "Default", "agents/models/tm_phoenix/tm_phoenix.vmdl", 2),
    Agent_t(4680, "Prof. Shahmat (Abuzer Kmrc) ", "agents/models/tm_leet/tm_leet_variantj.vmdl", 2),
    Agent_t(4711, "Jungle Rebel", "agents/models/tm_leet/tm_leet_variantk.vmdl", 2),
    Agent_t(4726, "Elite Trapper Solman", "agents/models/tm_jungle_raider/tm_jungle_raider_varianta.vmdl", 2),
    Agent_t(4816, "Komik Adam", "agents/models/tm_professional/tm_professional_vari.vmdl", 2),
    Agent_t(4817, "Profesyonel Kar", "agents/models/tm_professional/tm_professional_varg.vmdl", 2),
    Agent_t(4818, "Profesyonel", "agents/models/tm_professional/tm_professional_varf.vmdl", 2),
};




inline std::recursive_mutex g_SkinStateMutex;

class SkinManager
{
public:
    std::vector<SkinInfo_t> Skins;
    Glove_t Gloves = Glove_t();
    Knife_t Knife = Knife_t();
    MusicKitInfo_t MusicKit = { 1, "Counter-Strike 2" };

    void AddSkin(SkinInfo_t addedSkin)
    {
        std::lock_guard<std::recursive_mutex> lock(g_SkinStateMutex);
        for (SkinInfo_t& skin : Skins)
        {
            if (skin.weaponType == addedSkin.weaponType)
            {
                if (skin.paintKit == addedSkin.paintKit) return;
                skin = addedSkin;
                return;
            }
        }
        Skins.push_back(addedSkin);
    }

    SkinInfo_t GetSkin(WeaponsEnum def)
    {
        std::lock_guard<std::recursive_mutex> lock(g_SkinStateMutex);
        for (const SkinInfo_t& skin : Skins)
            if (skin.weaponType == def)
                return skin;
        return SkinInfo_t{ 0, WEP_NONE, "", 0.001f, 0, -1, 0 };
    }

    void RemoveSkin(WeaponsEnum def)
    {
        std::lock_guard<std::recursive_mutex> lock(g_SkinStateMutex);
        for (auto it = Skins.begin(); it != Skins.end(); ++it)
        {
            if (it->weaponType == def) { Skins.erase(it); return; }
        }
    }
};

inline SkinManager* g_SkinManager = nullptr;




inline const char* WeaponIdToName(uint16_t id)
{
    switch (id)
    {
    case 1: return "Desert Eagle"; case 2: return "Dual Berettas";
    case 3: return "Five-SeveN"; case 4: return "Glock-18";
    case 7: return "AK-47"; case 8: return "AUG"; case 9: return "AWP";
    case 10: return "FAMAS"; case 11: return "G3SG1"; case 13: return "Galil AR";
    case 14: return "M249"; case 16: return "M4A4"; case 17: return "MAC-10";
    case 19: return "P90"; case 23: return "MP5-SD"; case 24: return "UMP-45";
    case 25: return "XM1014"; case 26: return "PP-Bizon"; case 27: return "MAG-7";
    case 28: return "Negev"; case 29: return "Sawed-Off"; case 30: return "Tec-9";
    case 31: return "Zeus x27"; case 32: return "P2000"; case 33: return "MP7";
    case 34: return "MP9"; case 35: return "Nova"; case 36: return "P250";
    case 38: return "SCAR-20"; case 39: return "SG 553"; case 40: return "SSG 08";
    case 42: return "CT Knife"; case 59: return "T Knife";
    case 60: return "M4A1-S"; case 61: return "USP-S";
    case 63: return "CZ75-Auto"; case 64: return "R8 Revolver";
    default: return "Unknown";
    }
}

inline bool IsKnife(uint16_t defIndex)
{
    return (defIndex == WEP_CtKnife || defIndex == WEP_TKnife ||
            (defIndex >= 500 && defIndex <= 526));
}
