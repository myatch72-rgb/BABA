#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <atomic>
#include "SkinData.h"
#include "../../ext/nlohmann/json.hpp"

#pragma comment(lib, "winhttp.lib")

class CSkinDB
{
private:
    std::vector<SkinInfo_t> knifeSkins;
    std::vector<SkinInfo_t> gloveSkins;
    std::vector<SkinInfo_t> weaponSkins;
    std::vector<AgentInfo_t> agentItems;
    std::atomic_bool m_bDumped{false};
    std::atomic_bool m_bLoading{false};

    std::vector<std::string> knifeTypes = {
        "Bayonet", "Classic Knife", "Flip Knife", "Gut Knife",
        "Karambit", "M9 Bayonet", "Huntsman Knife", "Falchion Knife",
        "Bowie Knife", "Butterfly Knife", "Shadow Daggers", "Paracord Knife",
        "Survival Knife", "Ursus Knife", "Navaja Knife", "Nomad Knife",
        "Stiletto Knife", "Talon Knife", "Skeleton Knife", "Kukri Knife"
    };

    std::vector<std::string> gloveTypeNames = {
        "Bloodhound Gloves", "Broken Fang Gloves",
        "Driver Gloves", "Hand Wraps",
        "Hydra Gloves", "Moto Gloves",
        "Specialist Gloves", "Sport Gloves"
    };

    WeaponsEnum GetDefPerString(const std::string& name)
    {
        static const std::unordered_map<std::string, WeaponsEnum> weaponMap = {
            {"AK-47", WEP_Ak47},
            {"AUG", WEP_Aug},
            {"AWP", WEP_Awp},
            {"PP-Bizon", WEP_Bizon},
            {"CZ75-Auto", WEP_Cz75A},
            {"Desert Eagle", WEP_Deagle},
            {"Dual Berettas", WEP_Elite},
            {"FAMAS", WEP_Famas},
            {"Five-SeveN", WEP_FiveSeven},
            {"G3SG1", WEP_G3Sg1},
            {"Galil AR", WEP_Galil},
            {"Glock-18", WEP_Glock},
            {"P2000", WEP_P2000},
            {"M249", WEP_M249},
            {"M4A1-S", WEP_M4A1S},
            {"M4A4", WEP_M4A4},
            {"MAC-10", WEP_Mac10},
            {"MAG-7", WEP_Mag7},
            {"MP5-SD", WEP_Mp5SD},
            {"MP7", WEP_Mp7},
            {"MP9", WEP_Mp9},
            {"Negev", WEP_Negev},
            {"Nova", WEP_Nova},
            {"XM1014", WEP_Xm1014},
            {"USP-S", WEP_UspS},
            {"Tec-9", WEP_Tec9},
            {"SSG 08", WEP_Ssg08},
            {"SG 553", WEP_Sg556},
            {"SCAR-20", WEP_Scar20},
            {"Sawed-Off", WEP_Sawedoff},
            {"R8 Revolver", WEP_Revolver},
            {"P90", WEP_P90},
            {"P250", WEP_P250},
            {"UMP-45", WEP_Ump45},
        };

        for (const auto& [key, value] : weaponMap)
        {
            if (name.find(key) != std::string::npos)
                return value;
        }
        return WEP_NONE;
    }

    
    std::string DownloadString(const wchar_t* host, const wchar_t* path)
    {
        std::string result;
        HINTERNET hSession = WinHttpOpen(L"SkinChanger/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return result;
        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

        HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL))
        {
            DWORD dwSize = 0;
            DWORD dwDownloaded = 0;
            do
            {
                dwSize = 0;
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;

                char* buffer = new char[dwSize + 1];
                ZeroMemory(buffer, dwSize + 1);
                WinHttpReadData(hRequest, buffer, dwSize, &dwDownloaded);
                if (result.size() + static_cast<std::size_t>(dwDownloaded) >
                    8u * 1024u * 1024u) {
                    delete[] buffer;
                    result.clear();
                    break;
                }
                result.append(buffer, dwDownloaded);
                delete[] buffer;
            } while (dwSize > 0);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    std::string LoadResourceString(HMODULE module, int resourceId)
    {
        if (!module || resourceId <= 0)
            return {};

        HRSRC resource =
            FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource)
            return {};
        HGLOBAL loaded = LoadResource(module, resource);
        if (!loaded)
            return {};
        const void* data = LockResource(loaded);
        const DWORD size = SizeofResource(module, resource);
        if (!data || size == 0 || size > 8u * 1024u * 1024u)
            return {};
        return std::string(static_cast<const char*>(data),
                           static_cast<std::size_t>(size));
    }

    const nlohmann::json* FindField(const nlohmann::json& object,
                                    const char* key) const noexcept
    {
        if (!key || !object.is_object())
            return nullptr;
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &(*found);
    }

    bool ReadSigned(const nlohmann::json& value,
                    std::int64_t& output) const noexcept
    {
        if (const auto* signedValue =
                value.get_ptr<const nlohmann::json::number_integer_t*>()) {
            output = static_cast<std::int64_t>(*signedValue);
            return true;
        }
        if (const auto* unsignedValue =
                value.get_ptr<const nlohmann::json::number_unsigned_t*>()) {
            if (*unsignedValue >
                static_cast<nlohmann::json::number_unsigned_t>(
                    (std::numeric_limits<std::int64_t>::max)()))
                return false;
            output = static_cast<std::int64_t>(*unsignedValue);
            return true;
        }
        return false;
    }

    bool ParseSignedDecimal(const std::string& text,
                            std::int64_t& output) const noexcept
    {
        if (text.empty())
            return false;
        std::int64_t parsed = 0;
        const char* const begin = text.data();
        const char* const end = begin + text.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end)
            return false;
        output = parsed;
        return true;
    }

    int GetIntSafe(const nlohmann::json& object, const char* key,
                   int fallback) const noexcept
    {
        const auto* value = FindField(object, key);
        if (!value)
            return fallback;
        std::int64_t parsed = 0;
        if (!ReadSigned(*value, parsed)) {
            const auto* text =
                value->get_ptr<const nlohmann::json::string_t*>();
            if (!text || !ParseSignedDecimal(*text, parsed))
                return fallback;
        }
        if (parsed < (std::numeric_limits<int>::min)() ||
            parsed > (std::numeric_limits<int>::max)())
            return fallback;
        return static_cast<int>(parsed);
    }

    float GetFloatSafe(const nlohmann::json& object, const char* key,
                       float fallback) const noexcept
    {
        const auto* value = FindField(object, key);
        if (!value || value->is_null())
            return fallback;

        double parsed = 0.0;
        if (const auto* floating =
                value->get_ptr<const nlohmann::json::number_float_t*>()) {
            parsed = static_cast<double>(*floating);
        } else {
            std::int64_t integral = 0;
            if (!ReadSigned(*value, integral))
                return fallback;
            parsed = static_cast<double>(integral);
        }

        if (!std::isfinite(parsed) ||
            parsed < -(std::numeric_limits<float>::max)() ||
            parsed > (std::numeric_limits<float>::max)())
            return fallback;
        return static_cast<float>(parsed);
    }

    bool GetBoolSafe(const nlohmann::json& object, const char* key,
                     bool fallback) const noexcept
    {
        const auto* value = FindField(object, key);
        if (!value)
            return fallback;
        const auto* boolean =
            value->get_ptr<const nlohmann::json::boolean_t*>();
        return boolean ? static_cast<bool>(*boolean) : fallback;
    }

    int GetPaintIndexSafe(const nlohmann::json& skin) const noexcept
    {
        return GetIntSafe(skin, "paint_index", 0);
    }

    std::string GetStringSafe(const nlohmann::json& object,
                              const char* key) const
    {
        const auto* value = FindField(object, key);
        if (!value)
            return {};
        if (const auto* text =
                value->get_ptr<const nlohmann::json::string_t*>())
            return *text;
        if (!value->is_object())
            return {};
        const auto* localized = FindField(*value, "en");
        if (!localized)
            return {};
        const auto* text =
            localized->get_ptr<const nlohmann::json::string_t*>();
        return text ? *text : std::string{};
    }

    int GetRarityIdSafe(const nlohmann::json& skin) const noexcept
    {
        const auto* rarity = FindField(skin, "rarity");
        if (!rarity || !rarity->is_object())
            return 1;
        const auto* id = FindField(*rarity, "id");
        if (!id)
            return 1;
        if (const auto* idText =
                id->get_ptr<const nlohmann::json::string_t*>()) {
            if (idText->find("contraband") != std::string::npos) return 7;
            if (idText->find("ancient") != std::string::npos) return 6;
            if (idText->find("legendary") != std::string::npos) return 5;
            if (idText->find("mythical") != std::string::npos) return 4;
            if (idText->find("rare") != std::string::npos) return 3;
            if (idText->find("uncommon") != std::string::npos) return 2;
            return 1;
        }
        std::int64_t numericId = 0;
        if (!ReadSigned(*id, numericId) || numericId < 1 || numericId > 7)
            return 1;
        return static_cast<int>(numericId);
    }

public:
    bool IsDumped() const {
        return m_bDumped.load(std::memory_order_acquire);
    }

    void Dump()
    {
        DumpInternal(nullptr, nullptr);
    }

    void DumpEmbedded(HMODULE module, int skinsResourceId,
                      int agentsResourceId)
    {
        const std::string skins =
            LoadResourceString(module, skinsResourceId);
        const std::string agents =
            LoadResourceString(module, agentsResourceId);
        DumpInternal(&skins, &agents);
    }

    void DumpInternal(const std::string* embeddedSkins,
                      const std::string* embeddedAgents)
    {
        bool expected = false;
        if (!m_bLoading.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return;

        // Do not duplicate the embedded 5+ MB catalogue. The former value copy
        // happened before the JSON parser's exception boundary and could throw
        // std::bad_alloc during DLL startup.
        std::string downloadedSkins;
        const std::string* readBuffer = embeddedSkins;
        if (!readBuffer) {
            downloadedSkins =
                DownloadString(
                    L"raw.githubusercontent.com",
                    L"/ByMykel/CSGO-API/main/public/api/en/skins.json");
            readBuffer = &downloadedSkins;
        }

        if (!readBuffer || readBuffer->empty()) {
            m_bLoading.store(false, std::memory_order_release);
            return;
        }

        {
            // Manual-map injectors commonly do not register the image's x64
            // unwind table. Consequently a normal nlohmann type/parse
            // exception cannot be caught reliably inside this DLL. Parse in
            // non-throwing mode and validate every field before conversion.
            auto jsonData =
                nlohmann::json::parse(*readBuffer, nullptr, false);
            if (jsonData.is_discarded() || !jsonData.is_array()) {
                m_bLoading.store(false, std::memory_order_release);
                return;
            }

            for (const auto& skin : jsonData)
            {
                if (!skin.is_object())
                    continue;
                SkinInfo_t info;
                info.paintKit = GetPaintIndexSafe(skin);
                info.name = GetStringSafe(skin, "name");
                info.weaponType = GetDefPerString(info.name);
                info.rarity = GetRarityIdSafe(skin);
                info.image_url = GetStringSafe(skin, "image");
                info.legacy = GetBoolSafe(skin, "legacy_model", false);
                info.description = GetStringSafe(skin, "description");
                info.minFloat = GetFloatSafe(skin, "min_float", 0.0f);
                info.maxFloat = GetFloatSafe(skin, "max_float", 1.0f);

                if (skin.contains("weapon") && skin["weapon"].is_object())
                    info.weaponName = GetStringSafe(skin["weapon"], "name");
                if (skin.contains("pattern") && skin["pattern"].is_object())
                    info.patternName = GetStringSafe(skin["pattern"], "name");
                if (skin.contains("rarity") && skin["rarity"].is_object())
                    info.rarityName = GetStringSafe(skin["rarity"], "name");
                if (skin.contains("collections") &&
                    skin["collections"].is_array() &&
                    !skin["collections"].empty())
                    info.collection =
                        GetStringSafe(skin["collections"].front(), "name");
                if (skin.contains("crates") && skin["crates"].is_array() &&
                    !skin["crates"].empty())
                    info.source = GetStringSafe(skin["crates"].front(), "name");

                bool isKnife = false;
                bool isGlove = false;

                for (auto& k : knifeTypes)
                    if (info.name.find(k) != std::string::npos) { isKnife = true; break; }

                for (auto& g : gloveTypeNames)
                    if (info.name.find(g) != std::string::npos) { isGlove = true; break; }

                if (isKnife) { knifeSkins.push_back(info); continue; }
                if (isGlove) { gloveSkins.push_back(info); continue; }

                weaponSkins.push_back(info);
            }

            std::string downloadedAgents;
            const std::string* agentBuffer = embeddedAgents;
            if (!agentBuffer) {
                downloadedAgents =
                    DownloadString(
                        L"raw.githubusercontent.com",
                        L"/ByMykel/CSGO-API/main/public/api/en/agents.json");
                agentBuffer = &downloadedAgents;
            }
            if (agentBuffer && !agentBuffer->empty()) {
                auto agents =
                    nlohmann::json::parse(*agentBuffer, nullptr, false);
                if (!agents.is_discarded() && agents.is_array()) {
                    for (const auto& agent : agents) {
                        if (!agent.is_object())
                            continue;
                        AgentInfo_t info;
                        const int definitionIndex =
                            GetIntSafe(agent, "def_index", 0);
                        if (definitionIndex <= 0 ||
                            definitionIndex >
                                (std::numeric_limits<std::uint16_t>::max)())
                            continue;
                        info.defIndex =
                            static_cast<std::uint16_t>(definitionIndex);
                        info.name = GetStringSafe(agent, "name");
                        info.model = GetStringSafe(agent, "model_player");
                        info.image_url = GetStringSafe(agent, "image");
                        info.rarity = GetRarityIdSafe(agent);
                        if (agent.contains("team") && agent["team"].is_object())
                            info.team = GetStringSafe(agent["team"], "name");
                        if (info.defIndex != 0)
                            agentItems.push_back(std::move(info));
                    }
                }
            }

            m_bDumped.store(true, std::memory_order_release);
        }
        m_bLoading.store(false, std::memory_order_release);
    }

    std::vector<SkinInfo_t> GetWeaponSkins(WeaponsEnum type)
    {
        std::vector<SkinInfo_t> results;
        results.push_back(SkinInfo_t{ 0, WEP_NONE, "Vanilla", 0.001f, 0, -1, 1 });

        if (type == WEP_NONE) return results;

        for (const auto& skin : weaponSkins)
        {
            if (skin.weaponType != type) continue;
            results.push_back(skin);
        }

        
        std::sort(results.begin() + 1, results.end(), [](const SkinInfo_t& a, const SkinInfo_t& b) {
            return a.rarity > b.rarity;
        });

        return results;
    }

    std::vector<SkinInfo_t> GetGloveSkins(const std::string& gloveType)
    {
        std::vector<SkinInfo_t> results;
        if (gloveType.empty()) return results;

        for (const auto& skin : gloveSkins)
        {
            if (skin.name.find(gloveType) != std::string::npos)
                results.push_back(skin);
        }
        return results;
    }

    std::vector<SkinInfo_t>& GetKnifeSkins() { return knifeSkins; }
    const std::vector<AgentInfo_t>& GetAgents() const { return agentItems; }
};


inline CSkinDB* g_SkinDB = nullptr;
