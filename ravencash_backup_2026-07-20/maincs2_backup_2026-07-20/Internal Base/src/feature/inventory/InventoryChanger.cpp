#include "InventoryChanger.h"

#include "../../../ext/minhook/MinHook.h"
#include "../../core/CrashTelemetry.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/memory/Patterns.h"
#include "../../sdk/utils/Config.h"
#include "../../sdk/utils/Globals.h"
#include "../../sdk/utils/Utils.h"
#include "../skinchanger/SCLogger.h"
#include "../skinchanger/SkinChanger.h"
#include "../skinchanger/SkinDB.h"
#include "../skinchanger/SkinData.h"
#include "../../../ext/nlohmann/json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

extern SCLogger g_Logger;

namespace {

using Json = nlohmann::json;

const Json *FindJsonField(const Json &object, const char *key) noexcept {
  if (!key || !object.is_object())
    return nullptr;
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &(*found);
}

bool ReadJsonSigned(const Json &value, std::int64_t &output) noexcept {
  if (const auto *number = value.get_ptr<const Json::number_integer_t *>()) {
    output = static_cast<std::int64_t>(*number);
    return true;
  }
  if (const auto *number = value.get_ptr<const Json::number_unsigned_t *>()) {
    if (*number > static_cast<Json::number_unsigned_t>(
                      (std::numeric_limits<std::int64_t>::max)()))
      return false;
    output = static_cast<std::int64_t>(*number);
    return true;
  }
  return false;
}

int ReadJsonInt(const Json &object, const char *key, int fallback) noexcept {
  const auto *value = FindJsonField(object, key);
  std::int64_t parsed = 0;
  if (!value || !ReadJsonSigned(*value, parsed) ||
      parsed < (std::numeric_limits<int>::min)() ||
      parsed > (std::numeric_limits<int>::max)())
    return fallback;
  return static_cast<int>(parsed);
}

float ReadJsonFloat(const Json &object, const char *key,
                    float fallback) noexcept {
  const auto *value = FindJsonField(object, key);
  if (!value || value->is_null())
    return fallback;

  double parsed = 0.0;
  if (const auto *number = value->get_ptr<const Json::number_float_t *>()) {
    parsed = static_cast<double>(*number);
  } else {
    std::int64_t integral = 0;
    if (!ReadJsonSigned(*value, integral))
      return fallback;
    parsed = static_cast<double>(integral);
  }
  if (!std::isfinite(parsed) ||
      parsed < -(std::numeric_limits<float>::max)() ||
      parsed > (std::numeric_limits<float>::max)())
    return fallback;
  return static_cast<float>(parsed);
}

bool ReadJsonBool(const Json &object, const char *key,
                  bool fallback) noexcept {
  const auto *value = FindJsonField(object, key);
  if (!value)
    return fallback;
  const auto *boolean = value->get_ptr<const Json::boolean_t *>();
  return boolean ? static_cast<bool>(*boolean) : fallback;
}

std::string ReadJsonString(const Json &object, const char *key) {
  const auto *value = FindJsonField(object, key);
  if (!value)
    return {};
  const auto *text = value->get_ptr<const Json::string_t *>();
  return text ? *text : std::string{};
}

struct SOID_t {
  std::uint64_t id = 0;
  std::uint32_t type = 0;
  std::uint32_t padding = 0;
};

struct UtlVectorHeader {
  int size = 0;
  int allocationCount = 0;
  std::uintptr_t data = 0;
};

using CreateEconItemFn = void *(__cdecl *)();
using CreateBaseTypeCacheFn = void *(__fastcall *)(void *, unsigned int);
using GetInventoryManagerFn = void *(__fastcall *)();
using GetItemSchemaFn = void *(__fastcall *)();
using GetAttributeDefinitionFn = void *(__fastcall *)(void *, int);
using SetDynamicAttributeFn = void(__fastcall *)(void *, void *, const void *);
using EquipItemInLoadoutFn =
    bool(__fastcall *)(void *, int, int, std::uint64_t);

CreateEconItemFn g_CreateEconItem = nullptr;
CreateBaseTypeCacheFn g_CreateBaseTypeCache = nullptr;
GetInventoryManagerFn g_GetInventoryManager = nullptr;
GetItemSchemaFn g_GetItemSchema = nullptr;
GetAttributeDefinitionFn g_GetAttributeDefinition = nullptr;
SetDynamicAttributeFn g_SetDynamicAttribute = nullptr;
EquipItemInLoadoutFn g_EquipItemInLoadout = nullptr;
EquipItemInLoadoutFn g_OriginalEquipItemInLoadout = nullptr;

std::mutex g_StateMutex;
std::deque<InventoryChanger::ItemRequest> g_Pending;
std::vector<InventoryChanger::ItemRequest> g_Persisted;
struct RuntimeItem {
  std::uint64_t id = 0;
  InventoryChanger::ItemRequest request;
};
std::vector<RuntimeItem> g_RuntimeItems;
struct RuntimeEquip {
  int team = 0;
  int slot = 0;
  std::uint64_t id = 0;
  std::uint16_t defIndex = 0;
};
std::vector<RuntimeEquip> g_RuntimeEquips;
std::string g_Status = "Inventory is initializing";
std::atomic_bool g_Ready{false};
bool g_SetupDone = false;
bool g_EquipHookInstalled = false;
std::size_t g_RestoreRemaining = 0;
ULONGLONG g_SetupTick = 0;
ULONGLONG g_LastCommitAttemptTick = 0;

enum class CommitResult { Retry, Success, Failed };

constexpr std::size_t kMaxSavedItems = 512;
constexpr std::size_t kMaxPendingItems = 32;
constexpr std::size_t kMaxRuntimeItems = 64;
constexpr ULONGLONG kInventoryStartupGraceMs = 6000;
constexpr ULONGLONG kCommitIntervalMs = 500;
constexpr std::uintptr_t kLocalInventoryVtableIndex = 72;
constexpr std::uintptr_t kInventorySOCacheOffset = 0x68;
constexpr std::uintptr_t kInventoryOwnerOffset = 0x10;
constexpr std::uintptr_t kTypeCacheObjectsOffset = 0x8;

bool IsGloveDefinition(std::uint16_t defIndex, int *index = nullptr);

bool IsWeaponDefinition(std::uint16_t defIndex) {
  switch (static_cast<WeaponsEnum>(defIndex)) {
  case WEP_Deagle:
  case WEP_Elite:
  case WEP_FiveSeven:
  case WEP_Glock:
  case WEP_Ak47:
  case WEP_Aug:
  case WEP_Awp:
  case WEP_Famas:
  case WEP_G3Sg1:
  case WEP_Galil:
  case WEP_M249:
  case WEP_M4A4:
  case WEP_Mac10:
  case WEP_P90:
  case WEP_Mp5SD:
  case WEP_Ump45:
  case WEP_Xm1014:
  case WEP_Bizon:
  case WEP_Mag7:
  case WEP_Negev:
  case WEP_Sawedoff:
  case WEP_Tec9:
  case WEP_P2000:
  case WEP_Mp7:
  case WEP_Mp9:
  case WEP_Nova:
  case WEP_P250:
  case WEP_Scar20:
  case WEP_Sg556:
  case WEP_Ssg08:
  case WEP_M4A1S:
  case WEP_UspS:
  case WEP_Cz75A:
  case WEP_Revolver:
    return true;
  default:
    return false;
  }
}

bool IsSameItem(const InventoryChanger::ItemRequest &left,
                const InventoryChanger::ItemRequest &right) {
  return left.defIndex == right.defIndex &&
         left.paintKit == right.paintKit && left.seed == right.seed &&
         std::abs(left.wear - right.wear) < 0.000001f &&
         left.statTrak == right.statTrak && left.team == right.team &&
         left.model == right.model;
}

bool IsRequestSane(const InventoryChanger::ItemRequest &request) {
  if (request.defIndex == 0 || request.paintKit < 0 ||
      request.paintKit > 100000 || !std::isfinite(request.wear) ||
      request.wear < 0.00001f || request.wear > 1.0f ||
      request.seed < 0 || request.seed > 1000 ||
      request.statTrak < -1 || request.rarity < 0 ||
      request.rarity > 7 || request.model.size() >= MAX_PATH)
    return false;

  if (IsWeaponDefinition(request.defIndex) || IsKnife(request.defIndex))
    return request.paintKit > 0 && request.model.empty();

  if (IsGloveDefinition(request.defIndex))
    return request.paintKit > 0 && request.model.empty();

  // Agent definitions are not weapon enum values and carry their model path
  // directly. Restrict them to the two valid CS teams and to Source 2's agent
  // model namespace so a malformed persistence record can never become an
  // arbitrary engine model request.
  return request.paintKit == 0 && (request.team == 2 || request.team == 3) &&
         request.defIndex >= 4000 && !request.model.empty() &&
         (request.model.rfind("agents/models/", 0) == 0 ||
          request.model.rfind("characters/models/", 0) == 0);
}

bool IsAddressInClient(std::uintptr_t address) {
  const std::uintptr_t base = Memory::GetModuleBase("client.dll");
  if (!base || !address)
    return false;

  __try {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
      return false;
    return address >= base &&
           address < base + nt->OptionalHeader.SizeOfImage;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void SetStatus(const std::string &status) {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  g_Status = status;
}

void RegisterRuntimeItem(std::uint64_t id,
                         const InventoryChanger::ItemRequest &request) {
  if (id == 0)
    return;
  std::lock_guard<std::mutex> lock(g_StateMutex);
  auto existing = std::find_if(
      g_RuntimeItems.begin(), g_RuntimeItems.end(),
      [id](const RuntimeItem &item) { return item.id == id; });
  if (existing != g_RuntimeItems.end())
    existing->request = request;
  else
    g_RuntimeItems.push_back(RuntimeItem{id, request});
}

void RegisterRuntimeEquip(int team, int slot, std::uint64_t id,
                          std::uint16_t defIndex) {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  const auto existing = std::find_if(
      g_RuntimeEquips.begin(), g_RuntimeEquips.end(),
      [team, slot](const RuntimeEquip &equip) {
        return equip.team == team && equip.slot == slot;
      });
  const RuntimeEquip value{team, slot, id, defIndex};
  if (existing != g_RuntimeEquips.end())
    *existing = value;
  else
    g_RuntimeEquips.push_back(value);
}

bool FindRuntimeItem(std::uint64_t id,
                     InventoryChanger::ItemRequest &request) {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  const auto found = std::find_if(
      g_RuntimeItems.begin(), g_RuntimeItems.end(),
      [id](const RuntimeItem &item) { return item.id == id; });
  if (found == g_RuntimeItems.end())
    return false;
  request = found->request;
  return true;
}

bool IsGloveDefinition(std::uint16_t defIndex, int *index) {
  for (int i = 0; i < static_cast<int>(GloveTypes.size()); ++i) {
    if (GloveTypes[i].defIndex != defIndex)
      continue;
    if (index)
      *index = i;
    return true;
  }
  return false;
}

bool ResolveAgent(InventoryChanger::ItemRequest &request) {
  if (!request.model.empty())
    return true;
  if (!g_SkinDB || !g_SkinDB->IsDumped())
    return false;
  for (const auto &agent : g_SkinDB->GetAgents()) {
    if (agent.defIndex != request.defIndex)
      continue;
    request.model = agent.model;
    request.team = agent.team.find("Counter") != std::string::npos ? 3 : 2;
    if (request.name.empty())
      request.name = agent.name;
    return !request.model.empty();
  }
  return false;
}

bool ResolveLegacyPaintKit(const InventoryChanger::ItemRequest &request) {
  if (request.legacy || request.paintKit <= 0 || !g_SkinDB ||
      !g_SkinDB->IsDumped())
    return request.legacy;

  if (IsKnife(request.defIndex)) {
    for (const auto &skin : g_SkinDB->GetKnifeSkins())
      if (skin.paintKit == request.paintKit)
        return skin.legacy;
    return false;
  }

  for (const auto &skin : g_SkinDB->GetWeaponSkins(
           static_cast<WeaponsEnum>(request.defIndex)))
    if (skin.paintKit == request.paintKit)
      return skin.legacy;
  return false;
}

bool ApplyItemRequest(InventoryChanger::ItemRequest request, int equipTeam,
                      int slot, std::uint64_t itemId) {
  if (!g_SkinManager)
    return false;

  // A negative slot means the item was selected in our Inventory UI but has
  // not yet passed through the game's loadout function. Apply its fallback
  // visuals immediately, but do not advertise a fake loadout slot: the skin
  // runtime would otherwise call GetItemInLoadout(team, -1) while trying to
  // bind the synthetic SOC identity. The equip hook records the real slot as
  // soon as the user equips the item in the native loadout.
  if (slot >= 0)
    RegisterRuntimeEquip(equipTeam, slot, itemId, request.defIndex);

  std::lock_guard<std::recursive_mutex> skinLock(g_SkinStateMutex);
  Globals::sc_enabled = true;
  request.legacy = ResolveLegacyPaintKit(request);

  int gloveIndex = -1;
  if (IsKnife(request.defIndex)) {
    auto knife = std::find_if(
        Knives.begin(), Knives.end(), [&](const Knife_t &candidate) {
          return candidate.defIndex == request.defIndex;
        });
    if (knife == Knives.end())
      return false;

    Globals::sc_selected_knife =
        static_cast<int>(std::distance(Knives.begin(), knife)) + 1;
    SkinInfo_t skin;
    skin.weaponType = WEP_CtKnife;
    skin.paintKit = request.paintKit;
    skin.wear = request.wear;
    skin.seed = request.seed;
    skin.statTrak = request.statTrak;
    skin.rarity = request.rarity;
    skin.name = request.name;
    skin.legacy = request.legacy;
    g_SkinManager->AddSkin(skin);
    skin.weaponType = WEP_TKnife;
    g_SkinManager->AddSkin(skin);
  } else if (IsGloveDefinition(request.defIndex, &gloveIndex)) {
    Globals::sc_selected_glove_type = gloveIndex;
    g_SkinManager->Gloves.defIndex = request.defIndex;
    g_SkinManager->Gloves.paintKit = request.paintKit;
    g_SkinManager->Gloves.name = GloveTypes[gloveIndex].name;
    g_SkinManager->Gloves.wear = request.wear;
    g_SkinManager->Gloves.seed = request.seed;
  } else if (ResolveAgent(request)) {
    const int targetTeam = (equipTeam == 2 || equipTeam == 3)
                               ? equipTeam
                               : request.team;
    if (targetTeam == 3) {
      Globals::sc_inventory_agent_ct_model = request.model;
      Globals::sc_selected_agent_ct = -1;
    } else if (targetTeam == 2) {
      Globals::sc_inventory_agent_t_model = request.model;
      Globals::sc_selected_agent_t = -1;
    } else {
      return false;
    }
  } else {
    SkinInfo_t skin;
    skin.weaponType = static_cast<WeaponsEnum>(request.defIndex);
    skin.paintKit = request.paintKit;
    skin.wear = request.wear;
    skin.seed = request.seed;
    skin.statTrak = request.statTrak;
    skin.rarity = request.rarity;
    skin.name = request.name;
    skin.legacy = request.legacy;
    g_SkinManager->AddSkin(skin);
  }

  Globals::sc_force_update = 8;
  SkinChanger::g_MeshUpdateFrames = 30;
  SetStatus(request.name.empty() ? "Equipped item applied in game"
                                 : request.name + " equipped and applied");
  g_Logger.Log(
      "Inventory equip bridge: id=%llu team=%d slot=%d def=%u paint=%d "
      "model=%s",
      itemId, equipTeam, slot, request.defIndex, request.paintKit,
      request.model.c_str());
  CrashTelemetry::Trace(
      "inventory visual selection applied id=%llu team=%d slot=%d def=%u "
      "paint=%d enabled=%d skins=%zu gloveDef=%u glovePaint=%d",
      itemId, equipTeam, slot, request.defIndex, request.paintKit,
      Globals::sc_enabled ? 1 : 0, g_SkinManager->Skins.size(),
      g_SkinManager->Gloves.defIndex, g_SkinManager->Gloves.paintKit);
  return true;
}

void ApplyEquippedRuntimeItem(int equipTeam, int slot,
                              std::uint64_t itemId) {
  InventoryChanger::ItemRequest request;
  if (!FindRuntimeItem(itemId, request))
    return;
  ApplyItemRequest(std::move(request), equipTeam, slot, itemId);
}

bool __fastcall HookEquipItemInLoadout(void *inventoryManager, int team,
                                       int slot, std::uint64_t itemId) {
  const bool result = g_OriginalEquipItemInLoadout
                          ? g_OriginalEquipItemInLoadout(
                                inventoryManager, team, slot, itemId)
                          : false;
  // The game may return false for a locally-created SO even though Panorama
  // has accepted the selection. Runtime-ID filtering keeps this bridge scoped
  // exclusively to our own items.
  ApplyEquippedRuntimeItem(team, slot, itemId);
  return result;
}

std::filesystem::path GetInventoryPath() {
  const auto directory = Config::GetConfigPath();
  if (directory.empty())
    return {};
  return directory / "inventory_items.json";
}

Json ToJson(const InventoryChanger::ItemRequest &item) {
  return Json{{"def_index", item.defIndex},
              {"paint_kit", item.paintKit},
              {"wear", item.wear},
              {"seed", item.seed},
              {"stat_trak", item.statTrak},
              {"rarity", item.rarity},
              {"unusual", item.unusual},
              {"legacy", item.legacy},
              {"name", item.name},
              {"model", item.model},
              {"team", item.team}};
}

bool FromJson(const Json &value, InventoryChanger::ItemRequest &item) {
  if (!value.is_object())
    return false;
  const int definitionIndex = ReadJsonInt(value, "def_index", 0);
  if (definitionIndex <= 0 ||
      definitionIndex > (std::numeric_limits<std::uint16_t>::max)())
    return false;
  item.defIndex = static_cast<std::uint16_t>(definitionIndex);
  item.paintKit = ReadJsonInt(value, "paint_kit", 0);
  item.wear =
      std::clamp(ReadJsonFloat(value, "wear", 0.001f), 0.00001f, 1.0f);
  item.seed = std::clamp(ReadJsonInt(value, "seed", 0), 0, 1000);
  item.statTrak = std::max(ReadJsonInt(value, "stat_trak", -1), -1);
  item.rarity = std::clamp(ReadJsonInt(value, "rarity", 1), 0, 7);
  item.unusual = ReadJsonBool(value, "unusual", false);
  item.legacy = ReadJsonBool(value, "legacy", false);
  item.name = ReadJsonString(value, "name");
  item.model = ReadJsonString(value, "model");
  item.team = ReadJsonInt(value, "team", 0);
  return true;
}

void LoadSavedItems() {
  const auto path = GetInventoryPath();
  std::error_code filesystemError;
  if (path.empty() ||
      !std::filesystem::exists(path, filesystemError) || filesystemError)
    return;

  try {
    std::ifstream input(path);
    if (!input.is_open())
      return;
    Json root = Json::parse(input, nullptr, false);
    const Json *items = FindJsonField(root, "items");
    if (root.is_discarded() || !items || !items->is_array())
      return;

    std::size_t loaded = 0;
    std::size_t skipped = 0;
    std::lock_guard<std::mutex> lock(g_StateMutex);
    for (const auto &value : *items) {
      InventoryChanger::ItemRequest item;
      if (!FromJson(value, item) || !IsRequestSane(item) ||
          g_Persisted.size() >= kMaxSavedItems) {
        ++skipped;
        continue;
      }
      const bool duplicate =
          std::any_of(g_Persisted.begin(), g_Persisted.end(),
                      [&item](const InventoryChanger::ItemRequest &saved) {
                        return IsSameItem(saved, item);
                      });
      if (duplicate) {
        ++skipped;
        continue;
      }
      g_Persisted.push_back(item);
      ++loaded;
    }
    // Apply visual bridge for all persisted items so weapons/knives/gloves
    // immediately have skins in-game without requiring manual re-selection.
    for (const auto &item : g_Persisted) {
      ApplyItemRequest(item, item.team, -1, 0);
      if (g_Pending.size() < kMaxPendingItems)
        g_Pending.push_back(item);
    }
    g_RestoreRemaining = g_Pending.size();
    Globals::sc_enabled = true;
    Globals::sc_force_update = 8;

    if (loaded > 0)
      g_Status = "Saved inventory loaded (" + std::to_string(loaded) + " items active)";
    g_Logger.Log(
        "Inventory persistence loaded and applied: count=%zu queued=%zu",
        loaded, g_RestoreRemaining);
  } catch (...) {
    SetStatus("Inventory save file could not be read");
  }
}

bool SaveItems() {
  const auto path = GetInventoryPath();
  if (path.empty())
    return false;

  std::vector<InventoryChanger::ItemRequest> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    snapshot = g_Persisted;
  }

  try {
    Json root;
    root["schema_version"] = 1;
    root["items"] = Json::array();
    for (const auto &item : snapshot)
      root["items"].push_back(ToJson(item));

    auto temporary = path;
    temporary += ".tmp";
    {
      std::ofstream output(temporary, std::ios::trunc);
      if (!output.is_open())
        return false;
      output << root.dump(2);
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
  } catch (...) {
    return false;
  }
}

void *GetInventoryManager() {
  if (!g_GetInventoryManager)
    return nullptr;
  __try {
    void *manager = g_GetInventoryManager();
    if (!Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(manager)))
      return nullptr;
    const auto vtable = Utils::SafeRead<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(manager));
    return IsAddressInClient(vtable) ? manager : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void *GetLocalInventory() {
  void *manager = GetInventoryManager();
  if (!manager)
    return nullptr;

  const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
  const auto vtable = Utils::SafeRead<std::uintptr_t>(managerAddress);
  const auto function = Utils::SafeRead<std::uintptr_t>(
      vtable + kLocalInventoryVtableIndex * sizeof(std::uintptr_t));
  if (!IsAddressInClient(function))
    return nullptr;

  __try {
    auto getLocalInventory =
        reinterpret_cast<void *(__fastcall *)(void *)>(function);
    void *inventory = getLocalInventory(manager);
    return Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(inventory))
               ? inventory
               : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void *GetTypeCache(void *inventory) {
  if (!inventory || !g_CreateBaseTypeCache)
    return nullptr;
  const auto inventoryAddress = reinterpret_cast<std::uintptr_t>(inventory);
  void *soCache = reinterpret_cast<void *>(Utils::SafeRead<std::uintptr_t>(
      inventoryAddress + kInventorySOCacheOffset));
  if (!Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(soCache)))
    return nullptr;
  __try {
    void *typeCache = g_CreateBaseTypeCache(soCache, 1);
    return Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(typeCache))
               ? typeCache
               : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

std::pair<std::uint64_t, std::uint32_t> GetHighestIDs(void *typeCache) {
  std::uint64_t maxItem = 0;
  std::uint32_t maxInventory = (1u << 30) - 1;
  if (!typeCache)
    return {maxItem, maxInventory};

  const auto vector = Utils::SafeRead<UtlVectorHeader>(
      reinterpret_cast<std::uintptr_t>(typeCache) + kTypeCacheObjectsOffset);
  if (vector.size < 0 || vector.size > 100000 ||
      (vector.size > 0 && !Utils::IsValidPtr(vector.data)))
    return {maxItem, maxInventory};

  for (int i = 0; i < vector.size; ++i) {
    const auto item = Utils::SafeRead<std::uintptr_t>(
        vector.data + static_cast<std::uintptr_t>(i) * sizeof(std::uintptr_t));
    if (!Utils::IsValidPtr(item))
      continue;
    const auto id = Utils::SafeRead<std::uint64_t>(item + 0x10);
    // Default/loadout placeholder objects use reserved 0xC... inventory
    // positions. They must be excluded from both maxima; otherwise synthetic
    // items are inserted into the reserved range and Panorama reuses another
    // knife's icon/composite entry.
    if ((id & 0xF000000000000000ULL) != 0)
      continue;
    maxItem = std::max(maxItem, id);
    maxInventory =
        std::max(maxInventory, Utils::SafeRead<std::uint32_t>(item + 0x2C));
  }
  return {maxItem, maxInventory};
}

bool SetAttribute(void *item, int index, const void *value) {
  if (!item || !value || !g_GetItemSchema || !g_GetAttributeDefinition ||
      !g_SetDynamicAttribute)
    return false;
  __try {
    void *schema = g_GetItemSchema();
    if (!Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(schema)))
      return false;
    void *definition = g_GetAttributeDefinition(schema, index);
    if (!Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(definition)))
      return false;
    g_SetDynamicAttribute(item, definition, value);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool AddObjectToCache(void *typeCache, void *item) {
  const auto cacheAddress = reinterpret_cast<std::uintptr_t>(typeCache);
  const auto vtable = Utils::SafeRead<std::uintptr_t>(cacheAddress);
  const auto function =
      Utils::SafeRead<std::uintptr_t>(vtable + sizeof(std::uintptr_t));
  if (!IsAddressInClient(function))
    return false;
  __try {
    auto addObject =
        reinterpret_cast<bool(__fastcall *)(void *, void *)>(function);
    return addObject(typeCache, item);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool NotifySOCreated(void *inventory, void *item) {
  const auto inventoryAddress = reinterpret_cast<std::uintptr_t>(inventory);
  const auto vtable = Utils::SafeRead<std::uintptr_t>(inventoryAddress);
  const auto function = Utils::SafeRead<std::uintptr_t>(vtable);
  if (!IsAddressInClient(function))
    return false;

  const SOID_t owner =
      Utils::SafeRead<SOID_t>(inventoryAddress + kInventoryOwnerOffset);
  if (owner.id == 0)
    return false;
  __try {
    auto soCreated = reinterpret_cast<void(__fastcall *)(void *, SOID_t,
                                                         void *, int)>(
        function);
    soCreated(inventory, owner, item, 4); // eSOCacheEvent_Incremental
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void *CreateEconItemSafe() {
  if (!g_CreateEconItem)
    return nullptr;
  __try {
    return g_CreateEconItem();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

CommitResult CommitItem(const InventoryChanger::ItemRequest &request) {
  if (!g_Ready || request.defIndex == 0)
    return CommitResult::Failed;

  void *inventory = GetLocalInventory();
  if (!inventory) {
    SetStatus("Waiting for the local inventory");
    return CommitResult::Retry;
  }
  void *typeCache = GetTypeCache(inventory);
  if (!typeCache) {
    SetStatus("Waiting for the inventory type cache");
    return CommitResult::Retry;
  }

  const SOID_t owner = Utils::SafeRead<SOID_t>(
      reinterpret_cast<std::uintptr_t>(inventory) + kInventoryOwnerOffset);
  if (owner.id == 0) {
    SetStatus("Inventory owner is not ready");
    return CommitResult::Retry;
  }

  void *item = CreateEconItemSafe();
  if (!Utils::IsValidPtr(reinterpret_cast<std::uintptr_t>(item))) {
    SetStatus("CEconItem creation failed");
    return CommitResult::Failed;
  }

  const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
  const auto [maxItemId, maxInventoryId] = GetHighestIDs(typeCache);

  const std::uint64_t newItemId = maxItemId + 1;
  const std::uint32_t newInventoryId =
      maxInventoryId == std::numeric_limits<std::uint32_t>::max()
          ? (1u << 30)
          : maxInventoryId + 1;
  Utils::SafeWrite<std::uint64_t>(itemAddress + 0x10, newItemId);
  // Newly-created SOC items do not have a backend/original item.  Current
  // client code uses a non-zero original ID as an alternate economy identity;
  // mirroring the synthetic ID here makes material lookup miss the attributes
  // that were attached to the new SOC object (most visibly on guns/gloves).
  Utils::SafeWrite<std::uint64_t>(itemAddress + 0x18, 0);
  Utils::SafeWrite<std::uint32_t>(itemAddress + 0x28,
                                  static_cast<std::uint32_t>(owner.id));
  Utils::SafeWrite<std::uint32_t>(itemAddress + 0x2C, newInventoryId);
  Utils::SafeWrite<std::uint16_t>(itemAddress + 0x30, request.defIndex);

  std::uint16_t packed = Utils::SafeRead<std::uint16_t>(itemAddress + 0x32);
  // CEconItem packs quality, level and rarity into this word.  Do not retain
  // constructor garbage: generated weapons are Unique, while knives/gloves
  // use Unusual, and all generated items use a valid level of one.
  packed &= static_cast<std::uint16_t>(
      ~((0xFu << 5) | (0x3u << 9) | (0xFu << 11)));
  const std::uint16_t quality =
      request.unusual ? 3u : (request.statTrak >= 0 ? 9u : 4u);
  packed |= static_cast<std::uint16_t>(quality << 5);
  packed |= static_cast<std::uint16_t>(1u << 9);
  packed |= static_cast<std::uint16_t>((request.rarity & 0xF) << 11);
  Utils::SafeWrite<std::uint16_t>(itemAddress + 0x32, packed);

  const float paintKit = static_cast<float>(request.paintKit);
  const float seed = static_cast<float>(request.seed);
  const float wear = std::clamp(request.wear, 0.00001f, 1.0f);
  if (request.paintKit > 0) {
    if (!SetAttribute(item, 6, &paintKit) ||
        !SetAttribute(item, 7, &seed) || !SetAttribute(item, 8, &wear)) {
      SetStatus("Item attributes could not be written");
      return CommitResult::Failed;
    }
  }
  if (request.statTrak >= 0) {
    const int statTrak = request.statTrak;
    const int statTrakType = 0;
    if (!SetAttribute(item, 80, &statTrak) ||
        !SetAttribute(item, 81, &statTrakType)) {
      SetStatus("StatTrak attributes could not be written");
      return CommitResult::Failed;
    }
  }

  if (!AddObjectToCache(typeCache, item)) {
    SetStatus("Inventory cache rejected the item");
    return CommitResult::Failed;
  }
  if (!NotifySOCreated(inventory, item)) {
    SetStatus("Item was cached but its inventory notification failed");
    return CommitResult::Failed;
  }

  const std::uint64_t runtimeItemId =
      Utils::SafeRead<std::uint64_t>(itemAddress + 0x10);
  RegisterRuntimeItem(runtimeItemId, request);

  g_Logger.Log("Inventory item added: id=%llu inventory=%u def=%u paint=%d "
               "wear=%.5f seed=%d",
               newItemId, newInventoryId, request.defIndex, request.paintKit,
               wear, request.seed);
  return CommitResult::Success;
}

} // namespace

namespace InventoryChanger {

struct PatternDiagnostic {
  std::string name;
  bool resolved = false;
};

std::vector<PatternDiagnostic> GetPatternDiagnostics();

void Setup() {
  if (g_SetupDone)
    return;
  g_SetupDone = true;
  g_SetupTick = GetTickCount64();
  g_LastCommitAttemptTick = 0;

  // The standalone Skins feature has been removed. Start from an empty visual
  // bridge so old profile state cannot overwrite items equipped through the
  // Inventory system. ApplyEquippedRuntimeItem repopulates only the selected
  // runtime weapon, knife, glove, or agent.
  {
    std::lock_guard<std::recursive_mutex> skinLock(g_SkinStateMutex);
    if (g_SkinManager) {
      g_SkinManager->Skins.clear();
      g_SkinManager->Gloves = Glove_t{};
      g_SkinManager->MusicKit = MusicKitInfo_t{};
    }
    Globals::sc_enabled = false;
    Globals::sc_selected_knife = 0;
    Globals::sc_selected_glove_type = -1;
    Globals::sc_selected_glove_skin = -1;
    Globals::sc_selected_music_kit = 0;
    Globals::sc_selected_agent_ct = -1;
    Globals::sc_selected_agent_t = -1;
    Globals::sc_inventory_agent_ct_model.clear();
    Globals::sc_inventory_agent_t_model.clear();
    Globals::sc_fix_skin_weapons.clear();
    Globals::sc_force_update = 0;
  }

  g_CreateEconItem = reinterpret_cast<CreateEconItemFn>(Memory::PatternScan(
      "client.dll", Patterns::Client::CreateEconItem));
  g_CreateBaseTypeCache =
      reinterpret_cast<CreateBaseTypeCacheFn>(Memory::PatternScan(
          "client.dll",
          Patterns::Client::CreateBaseTypeCache));
  // The current signature begins with E8 rel32, so PatternScan returns the
  // call instruction rather than GetInventoryManager itself. Calling that
  // instruction as a function corrupts the call frame and crashes when an
  // item is added. Resolve its rel32 target before storing the function ptr.
  g_GetInventoryManager = reinterpret_cast<GetInventoryManagerFn>(
      Memory::ResolveRipRelative(
          "client.dll",
          Patterns::Client::GetInventoryManager,
          1, 5));
  g_GetItemSchema = reinterpret_cast<GetItemSchemaFn>(Memory::PatternScan(
      "client.dll", Patterns::Client::GetItemSchema));
  g_GetAttributeDefinition =
      reinterpret_cast<GetAttributeDefinitionFn>(Memory::PatternScan(
          "client.dll",
          Patterns::Client::GetAttributeDefinition));
  g_SetDynamicAttribute =
      reinterpret_cast<SetDynamicAttributeFn>(Memory::PatternScan(
          "client.dll",
          Patterns::Client::SetDynamicAttributeValue));
  g_EquipItemInLoadout =
      reinterpret_cast<EquipItemInLoadoutFn>(Memory::PatternScan(
          "client.dll",
          Patterns::Client::EquipItemInLoadout));

  const bool ready = g_CreateEconItem && g_CreateBaseTypeCache &&
                     g_GetInventoryManager && g_GetItemSchema &&
                     g_GetAttributeDefinition && g_SetDynamicAttribute &&
                     g_EquipItemInLoadout;
  g_Ready = ready;
  const auto diagnostics = GetPatternDiagnostics();
  for (const auto &diagnostic : diagnostics) {
    g_Logger.Log("Inventory pattern %s: %s", diagnostic.name.c_str(),
                 diagnostic.resolved ? "resolved" : "MISSING");
  }
  g_Logger.Log(
      "Inventory patterns: create=%p cache=%p manager=%p schema=%p def=%p "
      "set=%p equip=%p ready=%d",
      g_CreateEconItem, g_CreateBaseTypeCache, g_GetInventoryManager,
      g_GetItemSchema, g_GetAttributeDefinition, g_SetDynamicAttribute,
      g_EquipItemInLoadout, ready);

  if (ready) {
    SetStatus("Inventory ready");
  } else {
    std::string missing = "Pattern error - missing:";
    for (const auto &diagnostic : diagnostics) {
      if (!diagnostic.resolved)
        missing += " " + diagnostic.name;
    }
    SetStatus(missing);
  }
  if (ready)
    LoadSavedItems();
}

bool InstallHooks() {
  if (g_EquipHookInstalled || !g_EquipItemInLoadout)
    return g_EquipHookInstalled;

  const MH_STATUS createStatus = MH_CreateHook(
      reinterpret_cast<void *>(g_EquipItemInLoadout),
      &HookEquipItemInLoadout,
      reinterpret_cast<void **>(&g_OriginalEquipItemInLoadout));
  if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
    g_Logger.Log("Inventory EquipItemInLoadout hook create failed: %d",
                 createStatus);
    SetStatus("Equip hook could not be created");
    return false;
  }

  const MH_STATUS enableStatus =
      MH_QueueEnableHook(reinterpret_cast<void *>(g_EquipItemInLoadout));
  if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
    g_Logger.Log("Inventory EquipItemInLoadout hook queue failed: %d",
                 enableStatus);
    SetStatus("Equip hook could not be queued");
    return false;
  }

  g_EquipHookInstalled = true;
  g_Logger.Log("Inventory EquipItemInLoadout hook queued: target=%p",
               g_EquipItemInLoadout);
  return true;
}

void Run() {
  if (!g_Ready)
    return;

  const ULONGLONG now = GetTickCount64();
  if (now - g_SetupTick < kInventoryStartupGraceMs)
    return;
  if (g_LastCommitAttemptTick != 0 &&
      now - g_LastCommitAttemptTick < kCommitIntervalMs)
    return;

  ItemRequest request;
  {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    if (g_Pending.empty())
      return;
    request = g_Pending.front();
  }

  g_LastCommitAttemptTick = now;
  const CommitResult result = CommitItem(request);
  if (result == CommitResult::Retry)
    return;

  {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    if (!g_Pending.empty())
      g_Pending.pop_front();
    if (g_RestoreRemaining > 0) {
      --g_RestoreRemaining;
    } else if (result == CommitResult::Success) {
      g_Persisted.push_back(request);
      if (g_Persisted.size() > kMaxSavedItems)
        g_Persisted.erase(g_Persisted.begin());
    }
    if (result == CommitResult::Success)
      g_Status = request.name.empty()
                     ? "Item added; equip it from the game loadout"
                     : request.name + " added; equip it from the loadout";
  }
  if (result == CommitResult::Success)
    SaveItems();
}

bool QueueItem(const ItemRequest &source) {
  if (!g_Ready || source.defIndex == 0)
    return false;

  ItemRequest request = source;
  request.wear = std::clamp(request.wear, 0.00001f, 1.0f);
  request.seed = std::clamp(request.seed, 0, 1000);
  request.statTrak = std::max(request.statTrak, -1);
  request.rarity = std::clamp(request.rarity, 0, 7);
  if (!IsRequestSane(request)) {
    SetStatus("Item data failed validation and was not queued");
    g_Logger.Log("Inventory queue rejected invalid item: def=%u paint=%d "
                 "wear=%.5f seed=%d team=%d model=%s",
                 request.defIndex, request.paintKit, request.wear,
                 request.seed, request.team, request.model.c_str());
    return false;
  }

  std::lock_guard<std::mutex> lock(g_StateMutex);
  if (g_Pending.size() >= kMaxPendingItems) {
    g_Status = "Inventory queue is full";
    return false;
  }
  if (g_RuntimeItems.size() + g_Pending.size() >= kMaxRuntimeItems) {
    g_Status = "Session item limit reached; restart before adding more";
    return false;
  }
  const bool alreadyQueued =
      std::any_of(g_Pending.begin(), g_Pending.end(),
                  [&request](const ItemRequest &queued) {
                    return IsSameItem(queued, request);
                  });
  if (alreadyQueued) {
    g_Status = "This item is already waiting to be added";
    return false;
  }
  g_Pending.push_back(std::move(request));
  const ULONGLONG now = GetTickCount64();
  g_Status = now - g_SetupTick < kInventoryStartupGraceMs
                 ? "Item queued; waiting for inventory startup grace period"
                 : "Item queued; waiting for the game thread";
  return true;
}

std::string GetStatus() {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  return g_Status;
}

std::size_t GetPendingCount() {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  return g_Pending.size();
}

bool IsReady() { return g_Ready.load(); }

std::vector<PatternDiagnostic> GetPatternDiagnostics() {
  return {{"CreateEconItem", g_CreateEconItem != nullptr},
          {"CreateBaseTypeCache", g_CreateBaseTypeCache != nullptr},
          {"GetInventoryManager", g_GetInventoryManager != nullptr},
          {"GetItemSchema", g_GetItemSchema != nullptr},
          {"GetAttributeDefinition", g_GetAttributeDefinition != nullptr},
          {"SetDynamicAttributeValue", g_SetDynamicAttribute != nullptr},
          {"EquipItemInLoadout", g_EquipItemInLoadout != nullptr}};
}
bool IsRuntimeItemId(std::uint64_t itemId) {
  ItemRequest ignored;
  return FindRuntimeItem(itemId, ignored);
}

bool GetEquippedRuntimeItem(std::uint16_t defIndex, int team, int &slot,
                            std::uint64_t &itemId) {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  const auto found = std::find_if(
      g_RuntimeEquips.rbegin(), g_RuntimeEquips.rend(),
      [defIndex, team](const RuntimeEquip &equip) {
        return equip.defIndex == defIndex &&
               (team == 0 || equip.team == 0 || equip.team == team);
      });
  if (found == g_RuntimeEquips.rend())
    return false;
  slot = found->slot;
  itemId = found->id;
  return true;
}

std::vector<ItemRequest> GetPersistedItems() {
  std::lock_guard<std::mutex> lock(g_StateMutex);
  return g_Persisted;
}

void SetPersistedItems(const std::vector<ItemRequest> &items) {
  {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_Persisted.clear();
    for (const auto &item : items) {
      if (IsRequestSane(item) && g_Persisted.size() < kMaxSavedItems)
        g_Persisted.push_back(item);
    }
  }
  SaveItems();
}

void ClearPersisted() {
  {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_Persisted.clear();
  }
  // Remove the file so the old catalog does not survive a config load
  const auto path = GetInventoryPath();
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

} // namespace InventoryChanger

// ---------------------------------------------------------------------------
// Inventory Unlock — native function pointer, resolved once lazily
// ---------------------------------------------------------------------------
namespace {
using UnlockInventoryFn = void(__fastcall *)(void *);
UnlockInventoryFn g_UnlockInventory = nullptr;
} // namespace

namespace InventoryChanger {

void RunUnlock() {
  if (!Globals::inventory_unlock_enabled)
    return;

  // Resolve once on first call — avoids blocking Setup()
  if (!g_UnlockInventory) {
    const uintptr_t addr = Memory::PatternScan(
        "client.dll", Patterns::Client::UnlockInventory);
    if (addr)
      g_UnlockInventory = reinterpret_cast<UnlockInventoryFn>(addr);
    else
      return;
  }

  __try {
    if (g_GetInventoryManager) {
      void *manager = g_GetInventoryManager();
      if (manager && Utils::IsValidPtr(reinterpret_cast<uintptr_t>(manager)))
        g_UnlockInventory(manager);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_Logger.Log("EXCEPTION in RunUnlock code=0x%08X", GetExceptionCode());
    // Sıfırla — bir sonraki frame'de yeniden resolve dener
    g_UnlockInventory = nullptr;
  }
}

void ForceReapplyAll() {
  // sc_force_update > 0 → SkinChanger::SC_RunLogic her silah/eldiven
  // attribute'unu önbellek durumundan bağımsız olarak yeniden yazar.
  // 5 frame, composite material rebuild için yeterli.
  Globals::sc_force_update = 5;
  g_Logger.Log("ForceReapplyAll: sc_force_update=5");
}

} // namespace InventoryChanger
