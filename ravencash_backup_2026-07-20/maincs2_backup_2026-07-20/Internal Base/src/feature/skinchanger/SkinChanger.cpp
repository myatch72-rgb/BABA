#include "SkinChanger.h"
#include "../inventory/InventoryChanger.h"
#include "../../../ext/minhook/MinHook.h"
#include "../../sdk/entity/EntityManager.h"
#include "../../sdk/memory/Offsets.h"
#include "../../sdk/memory/PatternScan.h"
#include "../../sdk/memory/Patterns.h"
#include "../../sdk/utils/Globals.h"
#include "../../sdk/utils/Utils.h"
#include "SCLogger.h"
#include "SkinDB.h"
#include "SkinData.h"
#include "../../../resource.h"
#include "../misc/CustomModel.h"
#include <Windows.h>
#include <climits>
#include <map>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <shlobj.h>
#include <unordered_map>
#include <atomic>
#include <cstring>
#include <exception>


static int logThrottle = 0;
static std::atomic_bool g_SkinDatabaseLoaded{false};

#include <mutex>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

static std::vector<void *> g_AllocatedAttributeMemory;
static std::mutex g_MemoryMutex;
static const size_t MAX_TRACKED_ALLOCATIONS = 100;

struct SC_CompositeRefreshState {
  int paintKit = 0;
  uint16_t definitionIndex = 0;
  int delayFrames = 12;
  int cooldownFrames = 0;
  int attemptsRemaining = 24;
  int successfulCalls = 0;
  bool preserveRuntimeIdentity = false;
  bool loggedBindingFailure = false;
};

// A newly networked weapon can expose its fallback fields several frames
// before its composite-material owner is ready. Keep refreshes bounded and
// process only the active weapon from FRAME_RENDER_END.
static std::unordered_map<uintptr_t, SC_CompositeRefreshState>
    g_CompositeRefreshQueue;
static std::unordered_map<uintptr_t, int> g_LoggedNormalMeshPaint;

static uintptr_t g_LastValidPawn = 0;
static int g_FramesSinceLastValidPawn = 0;

static void SC_SafeVirtualFree(void *mem) {
  if (!mem)
    return;
  __try {
    VirtualFree(mem, 0, MEM_RELEASE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static void SC_FreeOldAttributeMemory_NoLock() {

  while (g_AllocatedAttributeMemory.size() > MAX_TRACKED_ALLOCATIONS) {
    void *oldMem = g_AllocatedAttributeMemory.front();
    SC_SafeVirtualFree(oldMem);
    g_AllocatedAttributeMemory.erase(g_AllocatedAttributeMemory.begin());
  }
}

static void SC_FreeOldAttributeMemory() {
  std::lock_guard<std::mutex> lock(g_MemoryMutex);
  SC_FreeOldAttributeMemory_NoLock();
}

static void SC_CleanupAllMemory() {
  std::lock_guard<std::mutex> lock(g_MemoryMutex);

  g_Logger.Log("SC_CleanupAllMemory: Freeing %d allocations",
               (int)g_AllocatedAttributeMemory.size());

  for (void *mem : g_AllocatedAttributeMemory) {
    SC_SafeVirtualFree(mem);
  }
  g_AllocatedAttributeMemory.clear();
  g_CompositeRefreshQueue.clear();

}

static void SC_TrackAllocation(void *mem) {
  if (!mem)
    return;
  std::lock_guard<std::mutex> lock(g_MemoryMutex);
  g_AllocatedAttributeMemory.push_back(mem);

  if (g_AllocatedAttributeMemory.size() > MAX_TRACKED_ALLOCATIONS) {
    SC_FreeOldAttributeMemory_NoLock();
  }
}

static const std::map<uint16_t, uint32_t> m_subclassIdMap = {
    {500, 3933374535}, {503, 3787235507}, {505, 4046390180}, {506, 2047704618},
    {507, 1731408398}, {508, 1638561588}, {509, 2282479884}, {512, 3412259219},
    {514, 2511498851}, {515, 1353709123}, {516, 4269888884}, {517, 1105782941},
    {518, 275962944},  {519, 1338637359}, {520, 3230445913}, {521, 3206681373},
    {522, 2595277776}, {523, 4029975521}, {524, 2463111489}, {525, 365028728},
    {526, 3845286452}};

static uintptr_t fnRegenerateWeaponSkins = 0;
static uintptr_t fnRegenerateWeaponSkin = 0;
static uintptr_t fnApplyEconCustomization = 0;
static uintptr_t fnGetCustomPaintKitIndex = 0;
static uintptr_t fnGetItemSchema = 0;
static uintptr_t fnGetAttributeDefinition = 0;
static uintptr_t fnSetDynamicAttributeValue = 0;
static uintptr_t fnCopyEconItemView = 0;
static uintptr_t fnGloveApplyPerTick = 0;
static uintptr_t fnSetBodyGroup = 0;
static uintptr_t fnUpdateSubClass = 0;
static uintptr_t fnUpdateComposite = 0;
static uintptr_t fnSetMeshGroupMask = 0;
static uintptr_t g_InventoryManager = 0;
static bool g_SetModelHookInstalled = false;
static SkinChanger::fnSetModel_t oSetModel = nullptr;

static bool SC_ShouldOverrideKnifeModel(const char *currentModel) {
  if (!currentModel || !Globals::sc_enabled)
    return false;
  int knifeIdx = Globals::sc_selected_knife;
  if (knifeIdx <= 0 || knifeIdx > (int)Knives.size())
    return false;

  return strstr(currentModel, "weapon_knife") != nullptr ||
         strstr(currentModel, "knife_default") != nullptr;
}

static const char *SC_GetSelectedKnifeModel() {
  int knifeIdx = Globals::sc_selected_knife;
  if (knifeIdx <= 0 || knifeIdx > (int)Knives.size())
    return nullptr;
  const char *model = Knives[knifeIdx - 1].model.c_str();
  return (model && model[0]) ? model : nullptr;
}

static const char *SC_GetSelectedAgentModel(void *entity) {
  if (!entity || !Globals::sc_enabled)
    return nullptr;

  const uintptr_t localPawn = Memory::Globals::LocalPawn();
  if (!localPawn || reinterpret_cast<uintptr_t>(entity) != localPawn)
    return nullptr;

  const int team = Utils::SafeRead<int>(localPawn + Offsets::m_iTeamNum);
  const std::string *model = nullptr;
  if (team == 3)
    model = &Globals::sc_inventory_agent_ct_model;
  else if (team == 2)
    model = &Globals::sc_inventory_agent_t_model;

  return model && !model->empty() ? model->c_str() : nullptr;
}

static void __fastcall hkSetModel(void *entity, const char *modelPath) {
  if (!oSetModel || !entity)
    return;

  const char *finalModel = modelPath;
  if (const char *forcedAgentModel = SC_GetSelectedAgentModel(entity)) {
    finalModel = forcedAgentModel;
  } else if (SC_ShouldOverrideKnifeModel(modelPath)) {
    const char *forcedKnifeModel = SC_GetSelectedKnifeModel();
    if (forcedKnifeModel)
      finalModel = forcedKnifeModel;
  }

  oSetModel(entity, finalModel);
}

static bool SC_IsAddressInClient(uintptr_t address) {
  const uintptr_t base = Memory::GetModuleBase("client.dll");
  if (!base || !address)
    return false;

  __try {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        base + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
      return false;
    return address >= base &&
           address < base + nt->OptionalHeader.SizeOfImage;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// CCSInventoryManager is a static game-system object, not a pointer-to-pointer.
// Its registration prologue is shared by many systems, so every candidate must
// be filtered by the RIP-relative class-name string before it is used.
static uintptr_t SC_FindInventoryManager() {
  const uintptr_t base = Memory::GetModuleBase("client.dll");
  if (!base)
    return 0;

  __try {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        base + static_cast<uintptr_t>(dos->e_lfanew));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        nt->Signature != IMAGE_NT_SIGNATURE)
      return 0;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto *data = reinterpret_cast<const uint8_t *>(base);
    constexpr size_t kPatternSize = 87;

    for (size_t i = 0; i + kPatternSize <= imageSize; ++i) {
      const uint8_t *p = data + i;
      if (p[0] != 0x48 || p[1] != 0x83 || p[2] != 0xEC || p[3] != 0x28 ||
          p[4] != 0x48 || p[5] != 0x8D || p[6] != 0x15 ||
          p[11] != 0x48 || p[12] != 0x8D || p[13] != 0x0D ||
          p[18] != 0xE8 || p[23] != 0x48 || p[24] != 0x8D ||
          p[25] != 0x05 || p[30] != 0x48 || p[31] != 0xC7 ||
          p[32] != 0x05 || p[37] != 0 || p[38] != 0 || p[39] != 0 ||
          p[40] != 0 || p[41] != 0x48 || p[42] != 0x89 ||
          p[43] != 0x05 || p[48] != 0x48 || p[49] != 0x8D ||
          p[50] != 0x0D || p[55] != 0x48 || p[56] != 0x8B ||
          p[57] != 0x05 || p[62] != 0x48 || p[63] != 0x8D ||
          p[64] != 0x15 || p[69] != 0x48 || p[70] != 0x89 ||
          p[71] != 0x0D || p[76] != 0x48 || p[77] != 0x83 ||
          p[78] != 0xC4 || p[79] != 0x28 || p[80] != 0x48 ||
          p[81] != 0xFF || p[82] != 0xA0 || p[83] != 0xE8 ||
          p[84] != 0x01 || p[85] != 0 || p[86] != 0)
        continue;

      const int32_t nameDisp =
          *reinterpret_cast<const int32_t *>(p + 7);
      const uintptr_t nameAddress =
          reinterpret_cast<uintptr_t>(p + 11) + nameDisp;
      char systemName[64]{};
      if (!Utils::SafeReadString(nameAddress, systemName,
                                 sizeof(systemName)) ||
          strcmp(systemName, "CCSInventoryManager") != 0)
        continue;

      const int32_t managerDisp =
          *reinterpret_cast<const int32_t *>(p + 51);
      const uintptr_t manager =
          reinterpret_cast<uintptr_t>(p + 55) + managerDisp;
      const uintptr_t vtable = Utils::SafeRead<uintptr_t>(manager);
      if (Utils::IsValidPtr(manager) && SC_IsAddressInClient(vtable))
        return manager;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  return 0;
}

static bool SafeSetupInternal() {
  __try {
    uintptr_t setModelAddr = Memory::PatternScan(
        "client.dll",
        Patterns::Client::SetModel);
    if (setModelAddr) {
      SkinChanger::fnSetModel =
          reinterpret_cast<SkinChanger::fnSetModel_t>(setModelAddr);
      g_Logger.Log("SetModel found at: 0x%llX", setModelAddr);
    } else {
      g_Logger.Log("ERROR: SetModel pattern NOT FOUND!");
    }

    uintptr_t regenAddr = Memory::PatternScan(
        "client.dll", Patterns::Client::RegenerateWeaponSkins);
    if (regenAddr) {
      fnRegenerateWeaponSkins = regenAddr;
      g_Logger.Log("RegenerateWeaponSkins found at: 0x%llX", regenAddr);
    } else {
      g_Logger.Log("ERROR: RegenerateWeaponSkins pattern NOT FOUND!");
    }

    fnApplyEconCustomization = Memory::PatternScan(
        "client.dll",
        Patterns::Client::ApplyEconCustomization);
    fnGetCustomPaintKitIndex = Memory::PatternScan(
        "client.dll",
        Patterns::Client::GetCustomPaintKitIndex);
    fnGetItemSchema = Memory::PatternScan(
        "client.dll",
        Patterns::Client::GetItemSchema);
    fnGetAttributeDefinition = Memory::PatternScan(
        "client.dll",
        Patterns::Client::GetAttributeDefinition);
    fnSetDynamicAttributeValue = Memory::PatternScan(
        "client.dll",
        Patterns::Client::SetDynamicAttributeValue);
    g_Logger.Log("Econ functions: apply=0x%llX livePaint=0x%llX "
                 "schema=0x%llX attrDef=0x%llX setAttr=0x%llX",
                 fnApplyEconCustomization, fnGetCustomPaintKitIndex,
                 fnGetItemSchema,
                 fnGetAttributeDefinition, fnSetDynamicAttributeValue);

    uintptr_t updateSubClassAddr = Memory::PatternScan(
        "client.dll", Patterns::Client::UpdateSubClass);
    if (updateSubClassAddr) {
      fnUpdateSubClass = updateSubClassAddr;
      g_Logger.Log("UpdateSubClass found at: 0x%llX", updateSubClassAddr);
    } else {
      g_Logger.Log("ERROR: UpdateSubClass pattern NOT FOUND!");
    }

    fnUpdateComposite = Memory::ResolveRipRelative(
        "client.dll", Patterns::Client::fnUpdateComposite, 1, 5);
    g_Logger.Log("UpdateComposite found at: 0x%llX", fnUpdateComposite);

    fnRegenerateWeaponSkin = Memory::PatternScan(
        "client.dll",
        Patterns::Client::RegenerateWeaponSkin);
    g_Logger.Log("RegenerateWeaponSkin(this,bool) found at: 0x%llX",
                 fnRegenerateWeaponSkin);

    uintptr_t setMeshGroupMaskAddr = Memory::PatternScan(
        "client.dll",
        Patterns::Client::SetMeshGroupMask);
    if (setMeshGroupMaskAddr) {
      fnSetMeshGroupMask = setMeshGroupMaskAddr;
      g_Logger.Log("SetMeshGroupMask found at: 0x%llX", setMeshGroupMaskAddr);
    } else {
      g_Logger.Log("ERROR: SetMeshGroupMask pattern NOT FOUND!");
    }

    fnCopyEconItemView = Memory::PatternScan(
        "client.dll",
        Patterns::Client::CopyEconItemView);
    fnGloveApplyPerTick = Memory::PatternScan(
        "client.dll",
        Patterns::Client::GloveApply_PerTick);
    // The shorter 53 55 signature is SetBodyGroup_inv(this, index, name).
    // Passing an integer as its third argument never updates the pawn's glove
    // bodygroup. Use the actual (this, group, value) overload.
    fnSetBodyGroup = Memory::PatternScan(
        "client.dll",
        Patterns::Client::SetBodyGroup);
    g_InventoryManager = SC_FindInventoryManager();
    g_Logger.Log("Glove functions: copyItemView=0x%llX apply=0x%llX "
                 "bodyGroup=0x%llX inventoryManager=0x%llX",
                 fnCopyEconItemView, fnGloveApplyPerTick, fnSetBodyGroup,
                 g_InventoryManager);

    SkinChanger::g_Initialized = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_Logger.Log("EXCEPTION in SafeSetupInternal!");
    SkinChanger::g_Initialized = false;
    return false;
  }
}

static void SafeCallSetModel(void *entity, const char *model) {
  if (!SkinChanger::fnSetModel || !entity || !model)
    return;
  __try {
    auto pfn =
        (void(__fastcall *)(void *, const char *))SkinChanger::fnSetModel;
    pfn(entity, model);
  } __except (
      (g_Logger.Log("CRASH in fnSetModel! code=0x%08X entity=0x%llX model=%s",
                    GetExceptionCode(), (uintptr_t)entity, model),
       EXCEPTION_EXECUTE_HANDLER)) {
  }
}

static bool SafeCallUpdateSubClass(void *entity) {
  if (!fnUpdateSubClass || !entity)
    return false;
  __try {
    auto pfn = (void(__fastcall *)(void *))fnUpdateSubClass;
    pfn(entity);
    return true;
  } __except (
      (g_Logger.Log("CRASH in fnUpdateSubClass! code=0x%08X entity=0x%llX",
                    GetExceptionCode(), (uintptr_t)entity),
       EXCEPTION_EXECUTE_HANDLER)) {
  }
  return false;
}

static void SafeCallSetMeshGroupMask(void *sceneNode, uint64_t mask) {
  if (!fnSetMeshGroupMask || !sceneNode)
    return;
  __try {
    auto pfn = (void(__fastcall *)(void *, uint64_t))fnSetMeshGroupMask;
    pfn(sceneNode, mask);
  } __except (
      (g_Logger.Log(
           "CRASH in fnSetMeshGroupMask! code=0x%08X node=0x%llX mask=%llu",
           GetExceptionCode(), (uintptr_t)sceneNode, mask),
       EXCEPTION_EXECUTE_HANDLER)) {
  }
}

static bool SafeCallUpdateComposite(uintptr_t weapon,
                                    bool runSecondary = false) {
  if (!weapon || !Utils::IsValidPtr(weapon) ||
      (!fnUpdateComposite && !fnRegenerateWeaponSkin))
    return false;

  bool primaryCalled = false;
  bool materialSetCalled = false;

  if (fnUpdateComposite) {
    __try {
      // Call sites in the current client pass C_CSWeaponBase + 0x608. This is
      // the embedded composite-material owner; the function immediately reads
      // its vectors at +0x4A0/+0x4B8/+0x4C0. Passing C_CSWeaponBase itself
      // shifts those reads into unrelated entity state and produces the
      // repeatable client.dll+0x1420590 access violation seen in telemetry.
      constexpr uintptr_t kCompositeOwnerOffset = 0x608;
      reinterpret_cast<void(__fastcall *)(void *, bool)>(fnUpdateComposite)(
          reinterpret_cast<void *>(weapon + kCompositeOwnerOffset),
          true);
      primaryCalled = true;
    } __except ((g_Logger.Log(
                     "EXCEPTION in UpdateCompositeMaterial code=0x%08X "
                     "weapon=0x%llX owner=0x%llX",
                     GetExceptionCode(), weapon, weapon + 0x608),
                 EXCEPTION_EXECUTE_HANDLER)) {
    }
  }

  // Keep this in its own exception boundary. A transient composite-owner
  // failure must not prevent the weapon-level material-set pass.
  if (fnRegenerateWeaponSkin) {
    __try {
      reinterpret_cast<void(__fastcall *)(void *, bool)>(
          fnRegenerateWeaponSkin)(reinterpret_cast<void *>(weapon),
                                  runSecondary);
      materialSetCalled = true;
    } __except ((g_Logger.Log(
                     "EXCEPTION in UpdateCompositeMaterialSet code=0x%08X "
                     "weapon=0x%llX",
                     GetExceptionCode(), weapon),
                 EXCEPTION_EXECUTE_HANDLER)) {
    }
  }

  return primaryCalled || materialSetCalled;
}

static bool SafeCallApplyEconCustomization(void *weapon) {
  if (!fnApplyEconCustomization || !weapon)
    return false;
  __try {
    auto pfn = reinterpret_cast<void(__fastcall *)(void *, int)>(
        fnApplyEconCustomization);
    // Mode 1 consumes m_nFallback* and schedules the modern composite
    // material rebuild. Mode 0 does not commit the paint override.
    pfn(weapon, 1);
    return true;
  } __except (
      (g_Logger.Log("EXCEPTION in ApplyEconCustomization! code=0x%08X "
                    "weapon=0x%llX",
                    GetExceptionCode(), (uintptr_t)weapon),
       EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static int SC_GetResolvedPaintKit(uintptr_t itemView) {
  if (!fnGetCustomPaintKitIndex || !itemView ||
      !Utils::IsValidPtr(itemView))
    return -1;

  __try {
    const auto pfn = reinterpret_cast<__int64(__fastcall *)(void *)>(
        fnGetCustomPaintKitIndex);
    const __int64 result = pfn(reinterpret_cast<void *>(itemView));
    if (result < 0 || result > INT_MAX)
      return -1;
    return static_cast<int>(result);
  } __except (
      (g_Logger.Log("EXCEPTION in GetCustomPaintKitIndex! code=0x%08X "
                    "item=0x%llX",
                    GetExceptionCode(), itemView),
       EXCEPTION_EXECUTE_HANDLER)) {
    return -1;
  }
}

static bool SafeCallGloveApply(uintptr_t pawn) {
  if (!fnGloveApplyPerTick || !pawn)
    return false;

  // Native callers pass the glove component embedded in C_CSPlayerPawn. The
  // component's owner must point back to the same pawn.
  const uintptr_t component = pawn + Offsets::sc_m_GloveComponent;
  if (!Utils::IsValidPtr(component) ||
      Utils::SafeRead<uintptr_t>(component +
                                 Offsets::sc_m_GloveComponentOwner) != pawn)
    return false;

  __try {
    auto pfn = reinterpret_cast<void(__fastcall *)(void *)>(
        fnGloveApplyPerTick);
    pfn(reinterpret_cast<void *>(component));
    return true;
  } __except (
      (g_Logger.Log("EXCEPTION in GloveApply_PerTick! code=0x%08X "
                    "pawn=0x%llX component=0x%llX",
                    GetExceptionCode(), pawn, component),
       EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static bool SafeCallSetBodyGroup(uintptr_t pawn) {
  if (!fnSetBodyGroup || !pawn || !Utils::IsValidPtr(pawn))
    return false;
  __try {
    reinterpret_cast<void(__fastcall *)(void *, int, int)>(
        fnSetBodyGroup)(reinterpret_cast<void *>(pawn), 0, 1);
    return true;
  } __except ((g_Logger.Log("EXCEPTION in SetBodyGroup code=0x%08X",
                            GetExceptionCode()),
               EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static uint64_t SC_GetMeshGroupMask(int paintKit, int weaponDefIdx = 0,
                                    bool legacyPaintKit = false) {
  if (paintKit <= 0)
    return 1;

  // Mesh-group mask is a BITMASK: bit0 (=1) enables the modern/default mesh
  // group, bit1 (=2) enables the LEGACY mesh group. Most weapons only have a
  // modern mesh group, so forcing legacy (2) draws wrong/missing geometry and
  // the skin's UVs end up shifted/broken. Default to the modern mesh (1) for
  // every weapon. The item database exposes the same legacy_model flag as the
  // live paint-kit schema; keep the manual override for old saved configs.
  uint64_t mask = legacyPaintKit ? 2 : 1;

  if (weaponDefIdx != 0 && Globals::sc_fix_skin_weapons.count(weaponDefIdx)) {
    mask = 2;
    if (logThrottle % 300 == 0) {
      g_Logger.Log("  [FIX SKIN] Legacy mesh forced for weapon def %d, "
                   "paintKit=%d -> mask=%llu",
                   weaponDefIdx, paintKit, mask);
    }
  }

  return mask;
}

static int SC_GetPaintKitForWeapon(WeaponsEnum wepType) {
  if (!g_SkinManager)
    return 0;
  SkinInfo_t skin = g_SkinManager->GetSkin(wepType);
  return skin.paintKit;
}

static int SC_GetKnifePaintKit() {
  if (!g_SkinManager)
    return 0;
  SkinInfo_t skin = g_SkinManager->GetSkin(WEP_CtKnife);
  if (skin.paintKit == 0)
    skin = g_SkinManager->GetSkin(WEP_TKnife);
  return skin.paintKit;
}

static uintptr_t SC_GetEntityFromHandle(uint32_t handle) {
  if (!handle || handle == 0xFFFFFFFF)
    return 0;
  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return 0;
  uintptr_t entityList =
      Utils::SafeRead<uintptr_t>(client + Offsets::dwEntityList);
  if (!entityList)
    return 0;
  uintptr_t entry = Utils::SafeRead<uintptr_t>(
      entityList + 0x8 * ((handle & 0x7FFF) >> 9) + 0x10);
  if (!Utils::IsValidPtr(entry))
    return 0;
  return Utils::SafeRead<uintptr_t>(entry + 0x70 * (handle & 0x1FF));
}

static bool SC_GetEntityDesignerName(uintptr_t entity, char *buffer,
                                     size_t bufferSize) {
  if (!entity || !buffer || bufferSize < 2 || !Utils::IsValidPtr(entity))
    return false;
  buffer[0] = '\0';
  const uintptr_t identity =
      Utils::SafeRead<uintptr_t>(entity + Offsets::sc_m_pEntity);
  if (!identity || !Utils::IsValidPtr(identity))
    return false;
  const uintptr_t designerName =
      Utils::SafeRead<uintptr_t>(identity + 0x20);
  if (!designerName || !Utils::IsValidPtr(designerName))
    return false;
  return Utils::SafeReadString(designerName, buffer, bufferSize) &&
         buffer[0] != '\0';
}

static bool SC_GetEntityClassName(uintptr_t entity, char *buffer,
                                  size_t bufferSize) {
  if (!entity || !buffer || bufferSize < 2 || !Utils::IsValidPtr(entity))
    return false;
  buffer[0] = '\0';

  // CEntityInstance::GetSchemaClassInfo is vfunc 44 in the current client.
  // EntityIdentity::m_designerName describes the map/entity symbol (and is
  // frequently empty for HUD-only objects), so it cannot be used to identify
  // C_CS2HudModelWeapon reliably.
  __try {
    const uintptr_t vtable = Utils::SafeRead<uintptr_t>(entity);
    if (!vtable || !Utils::IsValidPtr(vtable))
      return false;
    const uintptr_t getSchemaClassInfo =
        Utils::SafeRead<uintptr_t>(vtable + 44 * sizeof(uintptr_t));
    if (!getSchemaClassInfo || !Utils::IsValidPtr(getSchemaClassInfo))
      return false;

    void *classInfo = nullptr;
    reinterpret_cast<void(__fastcall *)(void *, void **)>(
        getSchemaClassInfo)(reinterpret_cast<void *>(entity), &classInfo);
    if (!classInfo ||
        !Utils::IsValidPtr(reinterpret_cast<uintptr_t>(classInfo)))
      return false;

    const uintptr_t className = Utils::SafeRead<uintptr_t>(
        reinterpret_cast<uintptr_t>(classInfo) + 0x8);
    if (!className || !Utils::IsValidPtr(className))
      return false;
    return Utils::SafeReadString(className, buffer, bufferSize) &&
           buffer[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    buffer[0] = '\0';
    return false;
  }
}

static bool SC_IsHudWeaponName(const char *name) {
  if (!name || !name[0])
    return false;
  const bool looksLikeViewModel =
      strcmp(name, "C_CS2HudModelWeapon") == 0 ||
      strstr(name, "hud_model_weapon") != nullptr ||
      strstr(name, "viewmodel") != nullptr;
  return looksLikeViewModel && strstr(name, "arms") == nullptr &&
         strstr(name, "addon") == nullptr;
}

static int SC_GetWeaponsRaw(uintptr_t pawn, uintptr_t *outBuffer,
                            int maxCount) {
  if (!pawn)
    return 0;
  uintptr_t weaponServices =
      Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pWeaponServices);
  if (!weaponServices) {
    g_Logger.Log("WeaponServices is NULL (pawn=0x%llX, offset=0x%X)", pawn,
                 Offsets::m_pWeaponServices);
    return 0;
  }

  // The live client layout is { int m_Size; 4 bytes padding;
  // CHandle<T>* m_pElements; ... }.  This is also visible in the runtime
  // values: +0x0 is a small weapon count while +0x8 is a canonical 64-bit
  // address.  The generated SDK helper used the fields in the opposite order,
  // making every valid list look like it contained hundreds of millions of
  // weapons and preventing every paint kit from reaching SC_ApplyWeaponSkin.
  const uintptr_t weaponVector =
      weaponServices + Offsets::sc_m_hMyWeapons;
  int weaponCount = Utils::SafeRead<int>(weaponVector + 0x0);
  const uintptr_t weaponEntry =
      Utils::SafeRead<uintptr_t>(weaponVector + 0x8);

  if (!Utils::IsValidPtr(weaponEntry) || weaponCount <= 0 ||
      weaponCount > 64) {
    if (logThrottle % 300 == 0) {
      g_Logger.Log(
          "Bad weapon list: count=%d entry=0x%llX (ws=0x%llX, offset=0x%X)",
          weaponCount, weaponEntry, weaponServices, Offsets::sc_m_hMyWeapons);
    }
    return 0;
  }
  if (weaponCount > maxCount)
    weaponCount = maxCount;
  int count = 0;
  for (int i = 0; i < weaponCount; i++) {
    uint32_t handle =
        Utils::SafeRead<uint32_t>(weaponEntry + i * sizeof(uint32_t));
    if (!handle || handle == 0xFFFFFFFF)
      continue;
    uintptr_t weapon = SC_GetEntityFromHandle(handle);
    if (weapon && Utils::IsValidPtr(weapon))
      outBuffer[count++] = weapon;
  }
  if (logThrottle % 300 == 0) {
    g_Logger.Log("GetWeapons: count=%d resolved=%d entry=0x%llX", weaponCount,
                 count, weaponEntry);
  }
  return count;
}

static uintptr_t SC_GetHudModelWeaponEntity(uintptr_t pawn,
                                            uintptr_t activeWeapon) {
  if (!pawn || !activeWeapon)
    return 0;

  // Current C_EconEntity exposes the first-person attachment directly. This
  // is more reliable than walking the arms hierarchy while a weapon switch is
  // still re-parenting C_CS2HudModelWeapon nodes.
  const uint32_t attachmentHandle = Utils::SafeRead<uint32_t>(
      activeWeapon + Offsets::sc_m_hViewmodelAttachment);
  if (attachmentHandle && attachmentHandle != 0xFFFFFFFF) {
    const uintptr_t attachment = SC_GetEntityFromHandle(attachmentHandle);
    char attachmentName[96] = {};
    char attachmentClass[96] = {};
    const bool attachmentIsHudWeapon =
        attachment && Utils::IsValidPtr(attachment) &&
        ((SC_GetEntityClassName(attachment, attachmentClass,
                                sizeof(attachmentClass)) &&
          SC_IsHudWeaponName(attachmentClass)) ||
         (SC_GetEntityDesignerName(attachment, attachmentName,
                                   sizeof(attachmentName)) &&
          SC_IsHudWeaponName(attachmentName)));
    if (attachmentIsHudWeapon) {
      const uintptr_t attachmentNode = Utils::SafeRead<uintptr_t>(
          attachment + Offsets::m_pGameSceneNode);
      if (attachmentNode && Utils::IsValidPtr(attachmentNode))
        return attachment;
    }
  }

  uint32_t armsHandle =
      Utils::SafeRead<uint32_t>(pawn + Offsets::sc_m_hHudModelArms);
  if (!armsHandle || armsHandle == 0xFFFFFFFF)
    return 0;

  uintptr_t arms = SC_GetEntityFromHandle(armsHandle);
  if (!arms || !Utils::IsValidPtr(arms))
    return 0;

  uintptr_t armsSceneNode =
      Utils::SafeRead<uintptr_t>(arms + Offsets::m_pGameSceneNode);
  if (!armsSceneNode || !Utils::IsValidPtr(armsSceneNode))
    return 0;

  uintptr_t fallbackHudWeapon = 0;
  uintptr_t exactOwnerFallback = 0;
  int hudWeaponCount = 0;
  int visited = 0;
  struct SceneSearchEntry {
    uintptr_t node;
    int depth;
  };
  SceneSearchEntry pending[64] = {};
  int pendingCount = 0;
  const uintptr_t firstChild =
      Utils::SafeRead<uintptr_t>(armsSceneNode + Offsets::sc_m_pChild);
  if (firstChild && Utils::IsValidPtr(firstChild))
    pending[pendingCount++] = {firstChild, 1};

  while (pendingCount > 0 && visited < 64) {
    const SceneSearchEntry current = pending[--pendingCount];
    const uintptr_t childNode = current.node;
    if (!childNode || !Utils::IsValidPtr(childNode))
      continue;
    ++visited;

    const uintptr_t nextSibling =
        Utils::SafeRead<uintptr_t>(childNode + Offsets::sc_m_pNextSibling);
    if (nextSibling && Utils::IsValidPtr(nextSibling) && pendingCount < 64)
      pending[pendingCount++] = {nextSibling, current.depth};
    if (current.depth < 4) {
      const uintptr_t nestedChild =
          Utils::SafeRead<uintptr_t>(childNode + Offsets::sc_m_pChild);
      if (nestedChild && Utils::IsValidPtr(nestedChild) && pendingCount < 64)
        pending[pendingCount++] = {nestedChild, current.depth + 1};
    }

    const uintptr_t childOwner =
        Utils::SafeRead<uintptr_t>(childNode + Offsets::sc_m_pOwner);
    if (childOwner && Utils::IsValidPtr(childOwner)) {
      char designerName[96] = {};
      char className[96] = {};
      const bool hasDesignerName = SC_GetEntityDesignerName(
          childOwner, designerName, sizeof(designerName));
      const bool hasClassName =
          SC_GetEntityClassName(childOwner, className, sizeof(className));
      const bool isHudWeapon =
          (hasClassName && SC_IsHudWeaponName(className)) ||
          (hasDesignerName && SC_IsHudWeaponName(designerName));
      if (isHudWeapon) {
        ++hudWeaponCount;
        if (!fallbackHudWeapon)
          fallbackHudWeapon = childOwner;
      }

      uint32_t ownerHandle =
          Utils::SafeRead<uint32_t>(childOwner + Offsets::sc_m_hOwnerEntity);
      if (ownerHandle && ownerHandle != 0xFFFFFFFF) {
        uintptr_t resolvedOwner = SC_GetEntityFromHandle(ownerHandle);
        if (resolvedOwner == activeWeapon) {
          if (isHudWeapon)
            return childOwner;
          // During a model switch the schema class can briefly be unavailable
          // even though m_hOwnerEntity already points to the active weapon.
          // The exact owner relationship is authoritative and safer than a
          // name-only candidate from a different hierarchy branch.
          if (!exactOwnerFallback)
            exactOwnerFallback = childOwner;
        }
      }
    }
  }

  if (exactOwnerFallback)
    return exactOwnerFallback;

  // Owner handles can lag behind the arms hierarchy for one render stage
  // during a weapon switch. With exactly one weapon HUD child, that child is
  // the model currently being rendered in first person.
  if (hudWeaponCount == 1)
    return fallbackHudWeapon;

  static std::unordered_map<uintptr_t, bool> s_loggedLookupFailure;
  if (!s_loggedLookupFailure[activeWeapon]) {
    s_loggedLookupFailure[activeWeapon] = true;
    g_Logger.Log("HUD weapon lookup failed: weapon=0x%llX attachment=%08X "
                 "arms=%08X nodes=%d hudCandidates=%d",
                 activeWeapon, attachmentHandle, armsHandle, visited,
                 hudWeaponCount);
  }
  return 0;
}

static bool SC_IsLegacyPaintKitForWeapon(WeaponsEnum wepType) {
  if (!g_SkinManager)
    return false;
  return g_SkinManager->GetSkin(wepType).legacy;
}

static uintptr_t SC_GetHudModelWeaponSceneNode(uintptr_t pawn,
                                               uintptr_t activeWeapon) {
  const uintptr_t hudWeapon =
      SC_GetHudModelWeaponEntity(pawn, activeWeapon);
  if (!hudWeapon || !Utils::IsValidPtr(hudWeapon))
    return 0;

  const uintptr_t sceneNode = Utils::SafeRead<uintptr_t>(
      hudWeapon + Offsets::m_pGameSceneNode);
  return Utils::IsValidPtr(sceneNode) ? sceneNode : 0;
}

static uintptr_t SC_GetActiveWeapon(uintptr_t pawn) {
  if (!pawn)
    return 0;
  uintptr_t weaponServices =
      Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pWeaponServices);
  if (!weaponServices)
    return 0;
  uint32_t activeHandle =
      Utils::SafeRead<uint32_t>(weaponServices + Offsets::m_hActiveWeapon);
  if (!activeHandle || activeHandle == 0xFFFFFFFF)
    return 0;
  return SC_GetEntityFromHandle(activeHandle);
}

enum EItemAttributeDef : int {
  ATTR_Paint = 6,
  ATTR_Pattern = 7,
  ATTR_Wear = 8,
};

static bool SC_SetDynamicAttribute(uintptr_t item, int attributeIndex,
                                   float value) {
  if (!item || !fnGetItemSchema || !fnGetAttributeDefinition ||
      !fnSetDynamicAttributeValue)
    return false;

  __try {
    auto getSchema =
        reinterpret_cast<void *(__fastcall *)()>(fnGetItemSchema);
    auto getDefinition = reinterpret_cast<void *(__fastcall *)(void *, int)>(
        fnGetAttributeDefinition);
    auto setValue = reinterpret_cast<void(__fastcall *)(void *, void *,
                                                        const void *)>(
        fnSetDynamicAttributeValue);

    void *schema = getSchema();
    if (!schema || !Utils::IsValidPtr(reinterpret_cast<uintptr_t>(schema)))
      return false;
    void *definition = getDefinition(schema, attributeIndex);
    if (!definition ||
        !Utils::IsValidPtr(reinterpret_cast<uintptr_t>(definition)))
      return false;

    void *attributeList = reinterpret_cast<void *>(
        item + Offsets::sc_m_AttributeList);
    if (!Utils::IsValidPtr(reinterpret_cast<uintptr_t>(attributeList)))
      return false;

    setValue(attributeList, definition, &value);
    return true;
  } __except (
      (g_Logger.Log("EXCEPTION in SetDynamicAttributeValue! code=0x%08X "
                    "item=0x%llX attribute=%d",
                    GetExceptionCode(), item, attributeIndex),
       EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static bool SC_WriteAttributes(uintptr_t item, int paintKit, float wear,
                               int seed) {
  const bool paintOk =
      SC_SetDynamicAttribute(item, ATTR_Paint, static_cast<float>(paintKit));
  const bool seedOk =
      SC_SetDynamicAttribute(item, ATTR_Pattern, static_cast<float>(seed));
  const bool wearOk = SC_SetDynamicAttribute(item, ATTR_Wear, wear);
  if (!paintOk || !seedOk || !wearOk) {
    g_Logger.Log("SetDynamicAttributeValue failed: item=0x%llX paint=%d "
                 "seed=%d wear=%.4f results=%d/%d/%d",
                 item, paintKit, seed, wear, paintOk, seedOk, wearOk);
  }
  return paintOk && seedOk && wearOk;
}

static uintptr_t SC_GetLocalInventory() {
  if (!g_InventoryManager || !Utils::IsValidPtr(g_InventoryManager))
    g_InventoryManager = SC_FindInventoryManager();
  if (!g_InventoryManager)
    return 0;

  const uintptr_t vtable =
      Utils::SafeRead<uintptr_t>(g_InventoryManager);
  const uintptr_t function =
      Utils::SafeRead<uintptr_t>(vtable + 72 * sizeof(uintptr_t));
  if (!SC_IsAddressInClient(vtable) || !SC_IsAddressInClient(function))
    return 0;

  __try {
    auto getLocalInventory =
        reinterpret_cast<void *(__fastcall *)(void *)>(function);
    return reinterpret_cast<uintptr_t>(
        getLocalInventory(reinterpret_cast<void *>(g_InventoryManager)));
  } __except (
      (g_Logger.Log("EXCEPTION in CCSInventoryManager::GetLocalInventory "
                    "code=0x%08X",
                    GetExceptionCode()),
       EXCEPTION_EXECUTE_HANDLER)) {
    return 0;
  }
}

static uintptr_t SC_GetLoadoutGlove(uintptr_t pawn) {
  const int team = Utils::SafeRead<int>(pawn + Offsets::m_iTeamNum);
  if (team != 2 && team != 3)
    return 0;

  const uintptr_t inventory = SC_GetLocalInventory();
  if (!inventory || !Utils::IsValidPtr(inventory))
    return 0;
  const uintptr_t vtable = Utils::SafeRead<uintptr_t>(inventory);
  const uintptr_t function =
      Utils::SafeRead<uintptr_t>(vtable + 8 * sizeof(uintptr_t));
  if (!SC_IsAddressInClient(vtable) || !SC_IsAddressInClient(function))
    return 0;

  __try {
    auto getItemInLoadout = reinterpret_cast<void *(__fastcall *)(void *, int,
                                                                  int)>(
        function);
    const uintptr_t item = reinterpret_cast<uintptr_t>(getItemInLoadout(
        reinterpret_cast<void *>(inventory), team, 41));
    if (!item || !Utils::IsValidPtr(item) ||
        !Utils::SafeRead<bool>(item + Offsets::sc_m_bInitialized))
      return 0;
    return item;
  } __except (
      (g_Logger.Log("EXCEPTION in GetItemInLoadout(team=%d, slot=41) "
                    "code=0x%08X",
                    team, GetExceptionCode()),
       EXCEPTION_EXECUTE_HANDLER)) {
    return 0;
  }
}

static uintptr_t SC_GetRuntimeLoadoutItem(uintptr_t pawn,
                                          uint16_t targetDefIndex) {
  const int team = Utils::SafeRead<int>(pawn + Offsets::m_iTeamNum);
  if (team != 2 && team != 3)
    return 0;

  int slot = 0;
  uint64_t expectedItemId = 0;
  int loadoutTeam = team;
  if (!InventoryChanger::GetEquippedRuntimeItem(targetDefIndex, loadoutTeam,
                                                 slot, expectedItemId)) {
    // Picked-up enemy weapons retain their original loadout definition. Use
    // the other team's runtime entry when the current team has no matching
    // slot instead of silently falling back to a disconnected paint source.
    loadoutTeam = (team == 2) ? 3 : 2;
    if (!InventoryChanger::GetEquippedRuntimeItem(
            targetDefIndex, loadoutTeam, slot, expectedItemId))
      return 0;
  }

  const uintptr_t inventory = SC_GetLocalInventory();
  if (!inventory || !Utils::IsValidPtr(inventory))
    return 0;
  const uintptr_t vtable = Utils::SafeRead<uintptr_t>(inventory);
  const uintptr_t function =
      Utils::SafeRead<uintptr_t>(vtable + 8 * sizeof(uintptr_t));
  if (!SC_IsAddressInClient(vtable) || !SC_IsAddressInClient(function))
    return 0;

  __try {
    auto getItemInLoadout = reinterpret_cast<void *(__fastcall *)(void *, int,
                                                                  int)>(
        function);
    const uintptr_t item = reinterpret_cast<uintptr_t>(getItemInLoadout(
        reinterpret_cast<void *>(inventory), loadoutTeam, slot));
    if (!item || !Utils::IsValidPtr(item) ||
        !Utils::SafeRead<bool>(item + Offsets::sc_m_bInitialized))
      return 0;
    const uint64_t itemId =
        Utils::SafeRead<uint64_t>(item + Offsets::sc_m_iItemID);
    if (itemId != expectedItemId ||
        !InventoryChanger::IsRuntimeItemId(itemId))
      return 0;
    return item;
  } __except ((g_Logger.Log(
                  "EXCEPTION in runtime GetItemInLoadout(team=%d slot=%d)",
                  loadoutTeam, slot),
               EXCEPTION_EXECUTE_HANDLER)) {
    return 0;
  }
}

static bool SC_BindRuntimeItemToWeapon(uintptr_t pawn, uintptr_t weapon,
                                       uint16_t targetDefIndex,
                                       bool updateKnifeSubclass) {
  if (!pawn || !weapon || !targetDefIndex)
    return false;
  const uintptr_t source = SC_GetRuntimeLoadoutItem(pawn, targetDefIndex);
  if (!source)
    return false;

  const uintptr_t destination =
      weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
  const uint16_t sourceDefinition = Utils::SafeRead<uint16_t>(
      source + Offsets::sc_m_iItemDefinitionIndex);
  if (sourceDefinition != targetDefIndex)
    return false;

  const uint64_t itemId =
      Utils::SafeRead<uint64_t>(source + Offsets::sc_m_iItemID);
  const uint64_t previousItemId =
      Utils::SafeRead<uint64_t>(destination + Offsets::sc_m_iItemID);
  const bool previousDisallowSOC =
      Utils::SafeRead<bool>(destination + Offsets::sc_m_bDisallowSOC);
  Utils::SafeWrite<uint64_t>(destination + Offsets::sc_m_iItemID, itemId);
  Utils::SafeWrite<uint32_t>(
      destination + Offsets::sc_m_iItemIDHigh,
      Utils::SafeRead<uint32_t>(source + Offsets::sc_m_iItemIDHigh));
  Utils::SafeWrite<uint32_t>(
      destination + Offsets::sc_m_iItemIDLow,
      Utils::SafeRead<uint32_t>(source + Offsets::sc_m_iItemIDLow));
  Utils::SafeWrite<uint32_t>(
      destination + Offsets::sc_m_iAccountID,
      Utils::SafeRead<uint32_t>(source + Offsets::sc_m_iAccountID));
  Utils::SafeWrite<int32_t>(
      destination + Offsets::sc_m_iEntityQuality,
      Utils::SafeRead<int32_t>(source + Offsets::sc_m_iEntityQuality));
  Utils::SafeWrite<bool>(destination + Offsets::sc_m_bInitialized, true);
  Utils::SafeWrite<bool>(destination + Offsets::sc_m_bDisallowSOC, false);
  Utils::SafeWrite<bool>(destination +
                             Offsets::sc_m_bRestoreCustomMaterialAfterPrecache,
                         true);

  if (!updateKnifeSubclass &&
      (previousItemId != itemId || previousDisallowSOC)) {
    g_Logger.Log("Normal weapon SOC identity bound: weapon=0x%llX id=%llu "
                 "def=%u sourcePaint=%d",
                 weapon, itemId, targetDefIndex,
                 SC_GetResolvedPaintKit(source));
  }

  if (updateKnifeSubclass) {
    static std::unordered_map<uintptr_t, uint16_t> s_boundKnifeDefinitions;
    const auto subclass = m_subclassIdMap.find(targetDefIndex);
    if (subclass == m_subclassIdMap.end())
      return false;
    if (s_boundKnifeDefinitions[weapon] != targetDefIndex) {
      Utils::SafeWrite<uint16_t>(
          destination + Offsets::sc_m_iItemDefinitionIndex, targetDefIndex);
      Utils::SafeWrite<uint32_t>(weapon + Offsets::sc_m_nSubclassID,
                                 subclass->second);
      if (!SafeCallUpdateSubClass(reinterpret_cast<void *>(weapon)))
        return false;
      s_boundKnifeDefinitions[weapon] = targetDefIndex;
      g_Logger.Log("Knife runtime identity bound: weapon=0x%llX id=%llu "
                   "def=%u subclass=%u",
                   weapon, itemId, targetDefIndex, subclass->second);
    }
  }
  return true;
}

static bool SC_QueueCompositeRefresh(uintptr_t weapon, int paintKit,
                                     uint16_t definitionIndex,
                                     bool preserveRuntimeIdentity) {
  if (!weapon || paintKit <= 0 || definitionIndex == 0)
    return false;

  // sc_force_update deliberately lives for several game ticks so knives,
  // gloves and agents can all observe it. Do not restart an already-running
  // weapon composite job on every one of those ticks; doing so repeatedly
  // re-queues clientside_reload_custom_econ before the first material build
  // has had a chance to finish.
  const auto existing = g_CompositeRefreshQueue.find(weapon);
  if (existing != g_CompositeRefreshQueue.end() &&
      existing->second.paintKit == paintKit &&
      existing->second.definitionIndex == definitionIndex &&
      existing->second.preserveRuntimeIdentity == preserveRuntimeIdentity &&
      existing->second.attemptsRemaining > 0) {
    return false;
  }

  // Weapon inventories are small. Bound the queue defensively in case stale
  // entity addresses survive a map transition long enough to be observed.
  if (g_CompositeRefreshQueue.size() >= 64 &&
      g_CompositeRefreshQueue.find(weapon) ==
          g_CompositeRefreshQueue.end()) {
    g_CompositeRefreshQueue.erase(g_CompositeRefreshQueue.begin());
  }

  SC_CompositeRefreshState &state = g_CompositeRefreshQueue[weapon];
  state.paintKit = paintKit;
  state.definitionIndex = definitionIndex;
  state.delayFrames = 12;
  state.cooldownFrames = 0;
  state.attemptsRemaining = 24;
  state.successfulCalls = 0;
  state.preserveRuntimeIdentity = preserveRuntimeIdentity;
  state.loggedBindingFailure = false;

  // A short mesh-group pulse is enough to expose the newly-created material.
  // The previous 180-frame pulse continuously switched the node through mask
  // zero while the asynchronous composite was being compiled.
  if (SkinChanger::g_MeshUpdateFrames < 42)
    SkinChanger::g_MeshUpdateFrames = 42;

  g_Logger.Log("Queued composite refresh: weapon=0x%llX def=%u paint=%d "
               "runtimeIdentity=%d",
               weapon, definitionIndex, paintKit, preserveRuntimeIdentity);
  return true;
}

static void SC_ProcessCompositeRefresh(uintptr_t pawn,
                                       uintptr_t activeWeapon) {
  for (auto it = g_CompositeRefreshQueue.begin();
       it != g_CompositeRefreshQueue.end();) {
    if (!Utils::IsValidPtr(it->first))
      it = g_CompositeRefreshQueue.erase(it);
    else
      ++it;
  }

  const auto queued = g_CompositeRefreshQueue.find(activeWeapon);
  if (queued == g_CompositeRefreshQueue.end())
    return;

  SC_CompositeRefreshState &state = queued->second;
  const uintptr_t item = activeWeapon + Offsets::sc_m_AttributeManager +
                         Offsets::sc_m_Item;
  const uint16_t currentDefinition = Utils::SafeRead<uint16_t>(
      item + Offsets::sc_m_iItemDefinitionIndex);
  const int currentPaint = Utils::SafeRead<int32_t>(
      activeWeapon + Offsets::sc_m_nFallbackPaintKit);

  // Entity addresses may be reused. Never refresh a different item that later
  // occupies the same address.
  if (IsKnife(currentDefinition) ||
      (!state.preserveRuntimeIdentity && currentPaint != state.paintKit) ||
      (currentDefinition != state.definitionIndex &&
       !state.preserveRuntimeIdentity)) {
    g_CompositeRefreshQueue.erase(queued);
    return;
  }

  if (state.delayFrames > 0) {
    --state.delayFrames;
    return;
  }
  if (state.cooldownFrames > 0) {
    --state.cooldownFrames;
    return;
  }
  if (state.attemptsRemaining <= 0) {
    const int resolvedPaint = SC_GetResolvedPaintKit(item);
    g_Logger.Log("Composite refresh exhausted: weapon=0x%llX def=%u "
                 "paint=%d resolvedPaint=%d successes=%d",
                 activeWeapon, state.definitionIndex, state.paintKit,
                 resolvedPaint, state.successfulCalls);
    g_CompositeRefreshQueue.erase(queued);
    return;
  }

  if (state.preserveRuntimeIdentity &&
      !SC_BindRuntimeItemToWeapon(pawn, activeWeapon,
                                  state.definitionIndex, false)) {
    --state.attemptsRemaining;
    state.cooldownFrames = 6;
    if (!state.loggedBindingFailure) {
      g_Logger.Log("Composite refresh waiting for runtime identity: "
                   "weapon=0x%llX def=%u paint=%d",
                   activeWeapon, state.definitionIndex, state.paintKit);
      state.loggedBindingFailure = true;
    }
    return;
  }

  const int resolvedPaint = SC_GetResolvedPaintKit(item);
  const bool paintResolved = (resolvedPaint == state.paintKit);

  // GetCustomPaintKitIndex verifies only the logical EconItemView value. It
  // can report the requested paint before the asynchronous composite texture
  // exists. SOC-backed weapons normally resolve from their cached CEconItem
  // and do not need the fallback apply entry; use it only if SOC resolution is
  // rejected. Fallback-backed weapons still issue one spaced native apply.
  if (!paintResolved ||
      (!state.preserveRuntimeIdentity && state.successfulCalls == 0)) {
    SafeCallApplyEconCustomization(reinterpret_cast<void *>(activeWeapon));
  }

  --state.attemptsRemaining;
  // The current regeneration entry has primary and secondary passes. Running
  // only the secondary=true branch left legacy-model finishes (for example
  // Printstream/Decimator) with a valid name and paint index but a vanilla
  // material. Run the primary pass first, then spaced secondary passes.
  const bool runSecondary = state.successfulCalls > 0;
  if (SafeCallUpdateComposite(activeWeapon, runSecondary)) {
    ++state.successfulCalls;
    state.cooldownFrames = 7;
    // Two successful composite passes are sufficient for the material system
    // to finalize the paint texture. Waiting for 3 passes caused some skins
    // (e.g. Printstream, Fade) to stay black when paintResolved lagged behind
    // the async texture compilation. Remove the paintResolved gate here —
    // SC_ProcessCompositeRefresh already validates the definition index and
    // paint kit at entry so stale entity reuse is already blocked.
    if (state.successfulCalls >= 2) {
      g_Logger.Log("Composite material finalized: weapon=0x%llX def=%u "
                   "paint=%d resolvedPaint=%d passes=%d attemptsUsed=%d",
                   activeWeapon, state.definitionIndex, state.paintKit,
                   resolvedPaint, state.successfulCalls,
                   24 - state.attemptsRemaining);
      g_CompositeRefreshQueue.erase(queued);
    }
  } else {
    // The composite owner commonly becomes ready a few frames after the
    // weapon entity. SEH contains that transient failure; a spaced retry then
    // succeeds without blocking the game thread.
    state.cooldownFrames = 7;
  }
}

static bool SC_CopyLoadoutGlove(uintptr_t pawn, uintptr_t destination) {
  if (!fnCopyEconItemView || !destination ||
      !Utils::IsValidPtr(destination))
    return false;
  const uintptr_t source = SC_GetLoadoutGlove(pawn);
  if (!source || source == destination)
    return false;

  // A locally-created CEconItem is sufficient for Panorama inventory cards,
  // but its generated C_EconItemView must never be copy-assigned into the
  // pawn's engine-owned embedded glove view. The copy constructor assumes
  // fully-populated GC allocator/SOC metadata and corrupts the spawning pawn
  // when that metadata belongs to a synthetic item.
  const uint64_t sourceItemId =
      Utils::SafeRead<uint64_t>(source + Offsets::sc_m_iItemID);
  if (InventoryChanger::IsRuntimeItemId(sourceItemId)) {
    g_Logger.Log("Glove item-view copy skipped for runtime item id=%llu",
                 sourceItemId);
    return false;
  }

  __try {
    auto copyItemView = reinterpret_cast<void *(__fastcall *)(void *,
                                                               const void *)>(
        fnCopyEconItemView);
    void *result = copyItemView(reinterpret_cast<void *>(destination),
                                reinterpret_cast<const void *>(source));
    const bool initialized =
        Utils::SafeRead<bool>(destination + Offsets::sc_m_bInitialized);
    g_Logger.Log("Glove item-view copy: src=0x%llX dst=0x%llX result=%p "
                 "initialized=%d sourceDef=%u",
                 source, destination, result, initialized,
                 Utils::SafeRead<uint16_t>(
                     source + Offsets::sc_m_iItemDefinitionIndex));
    // CopyAssign returns the destination in current builds, but the copied
    // initialized bit is the stable success signal we actually depend on.
    return initialized;
  } __except (
      (g_Logger.Log("EXCEPTION in C_EconItemView::CopyAssign code=0x%08X "
                    "dst=0x%llX src=0x%llX",
                    GetExceptionCode(), destination, source),
       EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static bool SC_IsConstructedEconItemView(uintptr_t itemView) {
  if (!itemView || !Utils::IsValidPtr(itemView))
    return false;
  const uintptr_t vtable = Utils::SafeRead<uintptr_t>(itemView);
  return SC_IsAddressInClient(vtable);
}

static void SC_MarkGlovesChanged(uintptr_t pawn) {
  uint8_t serial = Utils::SafeRead<uint8_t>(
      pawn + Offsets::sc_m_nEconGlovesChanged);
  serial = static_cast<uint8_t>(serial + 1);
  if (serial == 0)
    serial = 1;
  Utils::SafeWrite<uint8_t>(pawn + Offsets::sc_m_nEconGlovesChanged, serial);
  Utils::SafeWrite<bool>(pawn + Offsets::sc_m_bNeedToReApplyGloves, true);
}

// C_CSPlayerPawn always constructs its embedded C_EconItemView, even when the
// Steam inventory has no item in loadout slot 41. This fallback initializes
// that already-constructed view locally instead of requiring the user to own
// a real glove first.
static bool SC_PrepareEmbeddedGloveView(uintptr_t itemView) {
  if (!SC_IsConstructedEconItemView(itemView))
    return false;

  Utils::SafeWrite<bool>(itemView + Offsets::sc_m_bInitialized, true);
  Utils::SafeWrite<bool>(
      itemView + Offsets::sc_m_bRestoreCustomMaterialAfterPrecache, true);
  return true;
}

static bool SC_RestoreEmbeddedGloveView(uintptr_t pawn,
                                        uintptr_t itemView) {
  // A synthetic inventory item is a Panorama/SOC façade, not a fully-owned
  // engine CEconItemView.  Never use it as the source of a restore operation
  // and never turn the pawn's embedded glove view into "bare hands" while the
  // engine still has that synthetic item selected in slot 41.  Either action
  // leaves the native glove component observing two contradictory states.
  const uintptr_t loadoutGlove = SC_GetLoadoutGlove(pawn);
  if (loadoutGlove && loadoutGlove != itemView) {
    const uint64_t loadoutItemId =
        Utils::SafeRead<uint64_t>(loadoutGlove + Offsets::sc_m_iItemID);
    if (InventoryChanger::IsRuntimeItemId(loadoutItemId)) {
      g_Logger.Log("Glove restore skipped for runtime item id=%llu",
                   loadoutItemId);
      return false;
    }
  }

  if (SC_CopyLoadoutGlove(pawn, itemView)) {
    Utils::SafeWrite<bool>(itemView + Offsets::sc_m_bDisallowSOC, false);
    SC_MarkGlovesChanged(pawn);
    return true;
  }

  if (!SC_IsConstructedEconItemView(itemView))
    return false;

  // No genuine slot-41 item means the native state is bare hands.
  Utils::SafeWrite<uint16_t>(itemView + Offsets::sc_m_iItemDefinitionIndex,
                             0);
  Utils::SafeWrite<uint64_t>(itemView + Offsets::sc_m_iItemID, 0);
  Utils::SafeWrite<uint32_t>(itemView + Offsets::sc_m_iItemIDHigh, 0);
  Utils::SafeWrite<uint32_t>(itemView + Offsets::sc_m_iItemIDLow, 0);
  Utils::SafeWrite<bool>(itemView + Offsets::sc_m_bDisallowSOC, false);
  Utils::SafeWrite<bool>(itemView + Offsets::sc_m_bInitialized, false);
  SC_MarkGlovesChanged(pawn);
  return true;
}

static void SC_ForceUpdateHud(uintptr_t weapon) {

  uintptr_t identity =
      Utils::SafeRead<uintptr_t>(weapon + Offsets::sc_m_pEntity);
  if (!identity || !Utils::IsValidPtr(identity))
    return;

  uint32_t oldFlags =
      Utils::SafeRead<uint32_t>(identity + Offsets::sc_m_entityFlags);
  Utils::SafeWrite<uint32_t>(identity + Offsets::sc_m_entityFlags, 128);
  Sleep(1);
  Utils::SafeWrite<uint32_t>(identity + Offsets::sc_m_entityFlags, oldFlags);

  g_Logger.Log("  ForceUpdateHud: identity=0x%llX flags %u->128->%u", identity,
               oldFlags, oldFlags);
}

static void SC_ApplyWeaponSkin(uintptr_t weapon, int paintKit, float wear,
                               int seed, int statTrak,
                               bool preserveRuntimeIdentity) {
  if (!weapon || paintKit == 0)
    return;
  uintptr_t item = weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;

  g_Logger.Log("ApplyWeaponSkin: weapon=0x%llX paintKit=%d item=0x%llX "
               "(AM=0x%X + Item=0x%X)",
               weapon, paintKit, item, Offsets::sc_m_AttributeManager,
               Offsets::sc_m_Item);

  const uint16_t definitionIndex = Utils::SafeRead<uint16_t>(
      item + Offsets::sc_m_iItemDefinitionIndex);

  // Inventory-created guns already own a complete CEconItem in the local
  // SOCache. Keep that identity on the live C_EconItemView so the material
  // system can retrieve its dynamic paint/seed/wear attributes. The fallback
  // path is retained only for weapons that do not have a runtime loadout item.
  if (preserveRuntimeIdentity) {
    Utils::SafeWrite<bool>(item + Offsets::sc_m_bDisallowSOC, false);
  } else {
    Utils::SafeWrite<uint32_t>(item + Offsets::sc_m_iItemIDHigh,
                               UINT32_MAX);
    Utils::SafeWrite<uint32_t>(item + Offsets::sc_m_iItemIDLow,
                               UINT32_MAX);
    Utils::SafeWrite<bool>(item + Offsets::sc_m_bDisallowSOC, true);
    Utils::SafeWrite<int32_t>(item + Offsets::sc_m_iEntityQuality,
                              statTrak >= 0 ? 9 : 0);
  }
  Utils::SafeWrite<bool>(item + Offsets::sc_m_bInitialized, true);
  Utils::SafeWrite<bool>(
      item + Offsets::sc_m_bRestoreCustomMaterialAfterPrecache, true);

  Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackPaintKit, paintKit);
  Utils::SafeWrite<float>(weapon + Offsets::sc_m_flFallbackWear, wear);
  Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackSeed, seed);
  Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackStatTrak,
                            statTrak >= 0 ? statTrak : -1);

  const bool newCompositeJob =
      SC_QueueCompositeRefresh(weapon, paintKit, definitionIndex,
                               preserveRuntimeIdentity);
  const int resolvedBeforeApply = SC_GetResolvedPaintKit(item);
  // Always call ApplyEconCustomization when the resolved paint does not match
  // the requested kit — regardless of whether a new composite job was opened.
  // The previous gate (newCompositeJob &&) meant that if a job already existed
  // for this weapon the engine never got the econ apply call, leaving the skin
  // black on repeated force-update ticks.
  const bool needsApply =
      !preserveRuntimeIdentity
          ? (resolvedBeforeApply != paintKit)
          : (resolvedBeforeApply != paintKit);
  const bool applyResult =
      needsApply &&
      SafeCallApplyEconCustomization(reinterpret_cast<void *>(weapon));

  const uint32_t readHigh =
      Utils::SafeRead<uint32_t>(item + Offsets::sc_m_iItemIDHigh);
  const uint32_t readLow =
      Utils::SafeRead<uint32_t>(item + Offsets::sc_m_iItemIDLow);
  const int32_t readPaint =
      Utils::SafeRead<int32_t>(weapon + Offsets::sc_m_nFallbackPaintKit);
  const int resolvedPaint = SC_GetResolvedPaintKit(item);
  const uint64_t liveItemId =
      Utils::SafeRead<uint64_t>(item + Offsets::sc_m_iItemID);
  g_Logger.Log("  Weapon paint source: SOC=%d liveId=%llu queued=%d apply=%d "
               "IDs=%08X/%08X fallback=%d resolved=%d expect=%d",
               preserveRuntimeIdentity, liveItemId, newCompositeJob,
               applyResult,
               readHigh, readLow, readPaint, resolvedPaint, paintKit);
}

static void SC_ApplyKnifeModel(uintptr_t pWeaponEntity, int knifeIdx) {
  if (!SkinChanger::fnSetModel || !pWeaponEntity) {
    g_Logger.Log("ApplyKnifeModel SKIP: fnSetModel=%p entity=0x%llX",
                 SkinChanger::fnSetModel, pWeaponEntity);
    return;
  }

  if (knifeIdx <= 0 || knifeIdx > (int)Knives.size()) {
    g_Logger.Log("ApplyKnifeModel SKIP: bad index %d (max=%d)", knifeIdx,
                 (int)Knives.size());
    return;
  }
  const char *model = Knives[knifeIdx - 1].model.c_str();

  if (Globals::customweapon_enabled && CustomWeapon::g_SelectedIdx > 0 && 
      CustomWeapon::g_SelectedIdx < (int)CustomWeapon::g_ModelList.size()) {
      model = CustomWeapon::g_ModelList[CustomWeapon::g_SelectedIdx].resourcePath.c_str();
      CustomModel::PrecacheModel(model);
  }

  if (!model || model[0] == '\0') {
    g_Logger.Log("ApplyKnifeModel SKIP: empty model for index %d", knifeIdx);
    return;
  }
  g_Logger.Log("ApplyKnifeModel: entity=0x%llX knife=%s model=%s",
               pWeaponEntity, Knives[knifeIdx - 1].name.c_str(), model);
  SafeCallSetModel(reinterpret_cast<void *>(pWeaponEntity), model);
}

static void SC_ApplyKnifeDefinition(uintptr_t weapon) {
  if (!weapon || !g_SkinManager)
    return;
  int knifeIdx = Globals::sc_selected_knife;
  if (knifeIdx <= 0 || knifeIdx > (int)Knives.size())
    return;

  uintptr_t item = weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;

  g_Logger.Log("ApplyKnifeVisuals: weapon=0x%llX knife=%s", weapon,
               Knives[knifeIdx - 1].name.c_str());

  // Keep the real entity class, definition and subclass intact.  Rewriting a
  // live weapon into another subclass through the stale native updater left
  // client.dll calling a heap address as a vtable function (NX fault).
  SkinInfo_t knifeSkin = g_SkinManager->GetSkin(WEP_CtKnife);
  if (knifeSkin.paintKit == 0)
    knifeSkin = g_SkinManager->GetSkin(WEP_TKnife);

  g_Logger.Log("  KnifeSkin paintKit=%d", knifeSkin.paintKit);

  if (knifeSkin.paintKit > 0) {
    Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackPaintKit,
                              knifeSkin.paintKit);
    Utils::SafeWrite<float>(weapon + Offsets::sc_m_flFallbackWear,
                            knifeSkin.wear);
    Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackSeed,
                              knifeSkin.seed);

  }

  SkinChanger::g_PendingKnifeWeapon = weapon;
  SkinChanger::g_PendingKnifeIndex = knifeIdx;
  g_Logger.Log("  [KNIFE] Stored pending visuals: weapon=0x%llX knifeIdx=%d",
               weapon, knifeIdx);
}

static void SC_ResetGloveState() {
  const bool hadState = SkinChanger::g_PendingGlovePawn != 0 ||
                        SkinChanger::g_PendingGloveApply ||
                        SkinChanger::g_PendingGloveWeaponCount != 0;
  SkinChanger::g_PendingGlovePawn = 0;
  SkinChanger::g_PendingGloveWeaponCount = 0;
  SkinChanger::g_PendingGloveApply = false;
  memset(SkinChanger::g_PendingGloveWeapons, 0,
         sizeof(SkinChanger::g_PendingGloveWeapons));

  if (hadState)
    g_Logger.Log("SC_ResetGloveState: all state cleared");
}

static void SC_ApplyGloves(uintptr_t localPawn, uintptr_t *, int) {
  if (!localPawn || !g_SkinManager)
    return;

  int health = Utils::SafeRead<int>(localPawn + Offsets::m_iHealth);
  uint8_t lifeState =
      Utils::SafeRead<uint8_t>(localPawn + Offsets::m_lifeState);
  if (health <= 0 || lifeState != 0) {
    return;
  }

  const uint16_t gloveDef = g_SkinManager->Gloves.defIndex;
  const int glovePaintKit = g_SkinManager->Gloves.paintKit;
  const float gloveWear = g_SkinManager->Gloves.wear;
  const int gloveSeed = g_SkinManager->Gloves.seed;
  const uintptr_t econGloves = localPawn + Offsets::sc_m_EconGloves;

  if (!Utils::IsValidPtr(econGloves)) {
    g_Logger.Log("ApplyGloves SKIP: econGloves ptr (0x%llX) is invalid",
                 econGloves);
    return;
  }

  // A runtime inventory item owns a CEconItem attribute container, while the
  // pawn embeds a C_EconItemView. The current SetDynamicAttribute routine is a
  // CEconItem method; invoking it on either CAttributeList inside the embedded
  // view corrupts that view even when SEH catches the immediate AV. Keep the
  // stable SOC identity path here and never call that routine on econGloves.
  const uintptr_t loadoutGlove = SC_GetLoadoutGlove(localPawn);
  if (loadoutGlove && loadoutGlove != econGloves) {
    const uint64_t loadoutItemId =
        Utils::SafeRead<uint64_t>(loadoutGlove + Offsets::sc_m_iItemID);
    if (InventoryChanger::IsRuntimeItemId(loadoutItemId)) {
      static uintptr_t s_runtimePawn = 0;
      static uint64_t s_runtimeItemId = 0;
      static float s_runtimeSpawnTime = -1.f;
      static uint8_t s_runtimeUpdateFrames = 0;
      const uint64_t currentItemId = Utils::SafeRead<uint64_t>(
          econGloves + Offsets::sc_m_iItemID);
      const float currentSpawnTime = Utils::SafeRead<float>(
          localPawn + Offsets::sc_m_flLastSpawnTimeIndex);
      const bool changed = s_runtimePawn != localPawn ||
                           s_runtimeItemId != loadoutItemId ||
                           currentItemId != loadoutItemId ||
                           s_runtimeSpawnTime != currentSpawnTime ||
                           Globals::sc_force_update > 0;
      if (changed) {
        Utils::SafeWrite<uint16_t>(
            econGloves + Offsets::sc_m_iItemDefinitionIndex,
            Utils::SafeRead<uint16_t>(
                loadoutGlove + Offsets::sc_m_iItemDefinitionIndex));
        Utils::SafeWrite<uint64_t>(econGloves + Offsets::sc_m_iItemID,
                                   loadoutItemId);
        Utils::SafeWrite<uint32_t>(
            econGloves + Offsets::sc_m_iItemIDHigh,
            Utils::SafeRead<uint32_t>(
                loadoutGlove + Offsets::sc_m_iItemIDHigh));
        Utils::SafeWrite<uint32_t>(
            econGloves + Offsets::sc_m_iItemIDLow,
            Utils::SafeRead<uint32_t>(
                loadoutGlove + Offsets::sc_m_iItemIDLow));
        Utils::SafeWrite<uint32_t>(
            econGloves + Offsets::sc_m_iAccountID,
            Utils::SafeRead<uint32_t>(
                loadoutGlove + Offsets::sc_m_iAccountID));
        Utils::SafeWrite<bool>(econGloves + Offsets::sc_m_bDisallowSOC,
                               false);
        Utils::SafeWrite<bool>(econGloves + Offsets::sc_m_bInitialized,
                               true);
        Utils::SafeWrite<bool>(
            econGloves + Offsets::sc_m_bRestoreCustomMaterialAfterPrecache,
            true);
        // Notify the native glove component once for this identity/spawn.
        // The following frames only keep the reapply flag alive while the
        // corrected SetBodyGroup call makes the hands mesh visible.
        SC_MarkGlovesChanged(localPawn);
        s_runtimePawn = localPawn;
        s_runtimeItemId = loadoutItemId;
        s_runtimeSpawnTime = currentSpawnTime;
        s_runtimeUpdateFrames = 3;
        g_Logger.Log("ApplyGloves safe runtime identity: id=%llu def=%u "
                     "paint=%d spawn=%.2f sourceInitialized=%d",
                     loadoutItemId,
                     Utils::SafeRead<uint16_t>(econGloves +
                                               Offsets::sc_m_iItemDefinitionIndex),
                     glovePaintKit, currentSpawnTime,
                     Utils::SafeRead<bool>(loadoutGlove +
                                           Offsets::sc_m_bInitialized));
      }

      if (s_runtimeUpdateFrames > 0) {
        Utils::SafeWrite<bool>(econGloves + Offsets::sc_m_bInitialized, true);
        const bool bodyGroupApplied = SafeCallSetBodyGroup(localPawn);
        // Let the game's own glove component consume the new SOC identity on
        // its normal tick. Calling GloveApply_PerTick directly here races that
        // component and can make it rebuild the just-updated view as default.
        Utils::SafeWrite<bool>(
            localPawn + Offsets::sc_m_bNeedToReApplyGloves, true);
        if (s_runtimeUpdateFrames == 3 || s_runtimeUpdateFrames == 1) {
          g_Logger.Log(
              "  Runtime glove deferred refresh frame=%u bodyGroup=%d "
              "needFlag=%d",
              s_runtimeUpdateFrames, bodyGroupApplied,
              Utils::SafeRead<bool>(
                  localPawn + Offsets::sc_m_bNeedToReApplyGloves));
        }
        --s_runtimeUpdateFrames;
      }
      return;
    }
  }

  const uint16_t currentDef = Utils::SafeRead<uint16_t>(
      econGloves + Offsets::sc_m_iItemDefinitionIndex);
  const float lastSpawnTime =
      Utils::SafeRead<float>(localPawn + Offsets::sc_m_flLastSpawnTimeIndex);

  static float s_lastSpawnTime = 0.f;
  static uint16_t s_lastGloveDef = 0;
  static int s_lastGlovePaintKit = 0;
  static uint8_t s_updateFrames = 0;
  static uintptr_t s_lastPawn = 0;

  if (s_lastPawn != 0 && s_lastPawn != localPawn) {
    g_Logger.Log(
        "ApplyGloves: Pawn changed (0x%llX -> 0x%llX), resetting glove state",
        s_lastPawn, localPawn);
    s_lastSpawnTime = 0.f;
    s_lastGloveDef = 0;
    s_lastGlovePaintKit = 0;
    s_updateFrames = 0;
  }
  s_lastPawn = localPawn;

  // Restore the genuine slot-41 item when the changer is switched off.
  if (gloveDef == 0) {
    if (s_lastGloveDef != 0) {
      const bool restored =
          SC_RestoreEmbeddedGloveView(localPawn, econGloves);
      if (restored) {
        s_updateFrames = 4;
      }
      g_Logger.Log("ApplyGloves restore: copied=%d pawn=0x%llX",
                   restored, localPawn);
    }
    s_lastGloveDef = 0;
    s_lastGlovePaintKit = 0;
    s_lastSpawnTime = lastSpawnTime;
  } else {
    const bool needsFullUpdate =
        (Globals::sc_force_update > 0) || (currentDef != gloveDef) ||
                         (lastSpawnTime != s_lastSpawnTime) ||
                         (s_lastGloveDef != gloveDef) ||
                         (s_lastGlovePaintKit != glovePaintKit);

    if (needsFullUpdate) {
      g_Logger.Log("ApplyGloves: def=%d paint=%d pawn=0x%llX "
                   "spawnTime=%.2f currentDef=%d",
                   gloveDef, glovePaintKit, localPawn, lastSpawnTime,
                   currentDef);

      // Only an engine-owned loadout view is a valid CopyAssign source. There
      // is no safe local fallback through CEconItem::SetDynamicAttribute for a
      // C_EconItemView; fail closed instead of corrupting the pawn.
      const bool copiedLoadout =
          SC_CopyLoadoutGlove(localPawn, econGloves);
      if (!copiedLoadout) {
        g_Logger.Log("ApplyGloves FAILED CLOSED: no safe engine-owned item "
                     "view source");
        return;
      }

      Utils::SafeWrite<uint16_t>(
          econGloves + Offsets::sc_m_iItemDefinitionIndex, gloveDef);
      Utils::SafeWrite<int32_t>(
          econGloves + Offsets::sc_m_iEntityQuality, 4);
      Utils::SafeWrite<uint64_t>(econGloves + Offsets::sc_m_iItemID,
                                 UINT64_MAX);
      Utils::SafeWrite<uint32_t>(econGloves + Offsets::sc_m_iItemIDHigh,
                                 UINT32_MAX);
      Utils::SafeWrite<uint32_t>(econGloves + Offsets::sc_m_iItemIDLow,
                                 UINT32_MAX);
      Utils::SafeWrite<bool>(econGloves + Offsets::sc_m_bDisallowSOC, true);
      Utils::SafeWrite<bool>(econGloves + Offsets::sc_m_bInitialized, true);

      SC_MarkGlovesChanged(localPawn);
      s_updateFrames = 6;
      s_lastSpawnTime = lastSpawnTime;
      s_lastGloveDef = gloveDef;
      s_lastGlovePaintKit = glovePaintKit;
      g_Logger.Log("ApplyGloves prepared: source=%s initialized=%d "
                   "changedSerial=%u",
                   copiedLoadout ? "loadout" : "embedded-fallback",
                   Utils::SafeRead<bool>(econGloves +
                                         Offsets::sc_m_bInitialized),
                   Utils::SafeRead<uint8_t>(
                       localPawn + Offsets::sc_m_nEconGlovesChanged));
    }
  }

  if (s_updateFrames > 0) {
    SC_MarkGlovesChanged(localPawn);
    const bool nativeApplied = SafeCallGloveApply(localPawn);
    g_Logger.Log("  ApplyGloves frame %d: nativeApply=%d needFlag=%d",
                 s_updateFrames, nativeApplied,
                 Utils::SafeRead<bool>(
                     localPawn + Offsets::sc_m_bNeedToReApplyGloves));
    s_updateFrames--;
  }
}

static void SC_ApplyMusicKit(uintptr_t localController) {
  if (!localController || !g_SkinManager)
    return;
  uintptr_t inventoryServices = Utils::SafeRead<uintptr_t>(
      localController + Offsets::sc_m_pInventoryServices);

  if (!inventoryServices)
    inventoryServices = Utils::SafeRead<uintptr_t>(
        localController + Offsets::sc_m_pInventoryServices + 0x8);
  if (!inventoryServices) {
    if (logThrottle % 300 == 0) {
      g_Logger.Log("MusicKit: inventoryServices is NULL (controller=0x%llX, "
                   "offset=0x%X, fallback=0x%X)",
                   localController, Offsets::sc_m_pInventoryServices,
                   Offsets::sc_m_pInventoryServices + 0x8);
    }
    return;
  }
  Utils::SafeWrite<uint16_t>(inventoryServices + Offsets::sc_m_unMusicID,
                             g_SkinManager->MusicKit.id);
}

static void SC_UpdateActiveWeaponDef(uintptr_t localPawn) {
  if (!localPawn) {
    Globals::sc_current_weapon_def = WEP_NONE;
    return;
  }

  uintptr_t activeWeapon = SC_GetActiveWeapon(localPawn);
  if (logThrottle % 300 == 0) {
    g_Logger.Log(
        "ActiveWeapon: pawn=0x%llX, resolved via WeaponServices=0x%llX",
        localPawn, activeWeapon);
  }

  if (!activeWeapon || !Utils::IsValidPtr(activeWeapon)) {
    Globals::sc_current_weapon_def = WEP_NONE;
    if (logThrottle % 300 == 0) {
      g_Logger.Log("  Active weapon pointer is invalid, setting WEP_NONE");
    }
    return;
  }

  uintptr_t activeItem =
      activeWeapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
  uint16_t rawDefIdx = Utils::SafeRead<uint16_t>(
      activeItem + Offsets::sc_m_iItemDefinitionIndex);
  Globals::sc_current_weapon_def = static_cast<WeaponsEnum>(rawDefIdx);

  if (logThrottle % 300 == 0) {
    g_Logger.Log("  activeItem=0x%llX (weapon + 0x%X + 0x%X), defIdx=%d (%s)",
                 activeItem, Offsets::sc_m_AttributeManager, Offsets::sc_m_Item,
                 rawDefIdx, WeaponIdToName(rawDefIdx));
  }
}

static bool SafeCallRegenerateWeaponSkins() {
  if (!fnRegenerateWeaponSkins)
    return false;
  __try {
    ((void (*)())fnRegenerateWeaponSkins)();
    return true;
  } __except ((
      g_Logger.Log("EXCEPTION in fnRegenerateWeaponSkins code=0x%08X fn=0x%llX",
                   GetExceptionCode(), fnRegenerateWeaponSkins),
      EXCEPTION_EXECUTE_HANDLER)) {
    return false;
  }
}

static void SC_RunLogic(uintptr_t client, uintptr_t localPawn,
                        uintptr_t localController) {
  if (!g_SkinManager)
    return;

  logThrottle++;

  SC_UpdateActiveWeaponDef(localPawn);

  static uintptr_t weaponBuffer[64];
  int weaponCount = SC_GetWeaponsRaw(localPawn, weaponBuffer, 64);

  // A skin must still be applied to the weapon in hand if a future client
  // update temporarily changes the network-vector layout.  ActiveWeapon uses
  // an independent handle and is already validated by the entity system.
  if (weaponCount == 0) {
    const uintptr_t activeWeapon = SC_GetActiveWeapon(localPawn);
    if (activeWeapon && Utils::IsValidPtr(activeWeapon)) {
      weaponBuffer[0] = activeWeapon;
      weaponCount = 1;
      if (logThrottle % 300 == 0)
        g_Logger.Log("Weapon list fallback: using active weapon 0x%llX",
                     activeWeapon);
    }
  }

  if (logThrottle % 300 == 0) {
    g_Logger.Log(
        "RunLogic: pawn=0x%llX controller=0x%llX weaponCount=%d forceUpdate=%d",
        localPawn, localController, weaponCount, Globals::sc_force_update);
    g_Logger.Log(
        "  SkinManager: %d skins, knife=%d, glove def=%d paint=%d, music=%d",
        (int)g_SkinManager->Skins.size(), Globals::sc_selected_knife,
        g_SkinManager->Gloves.defIndex, g_SkinManager->Gloves.paintKit,
        g_SkinManager->MusicKit.id);
  }

  bool shouldUpdate = false;
  bool shouldRegenerateWeaponSkins = false;

  for (int w = 0; w < weaponCount; w++) {
    uintptr_t weapon = weaponBuffer[w];
    uintptr_t item =
        weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
    uint16_t defIndex =
        Utils::SafeRead<uint16_t>(item + Offsets::sc_m_iItemDefinitionIndex);

    if (IsKnife(defIndex)) {
      if (Globals::sc_selected_knife > 0 &&
          Globals::sc_selected_knife <= (int)Knives.size()) {
        const uint16_t targetKnifeDef =
            Knives[Globals::sc_selected_knife - 1].defIndex;
        const bool runtimeKnifeBound = SC_BindRuntimeItemToWeapon(
            localPawn, weapon, targetKnifeDef, false);
        const bool needsKnifeRefresh =
            (Globals::sc_force_update > 0) ||
            (SkinChanger::g_PendingKnifeWeapon != weapon) ||
            (SkinChanger::g_PendingKnifeIndex != Globals::sc_selected_knife);
        if (needsKnifeRefresh) {
          g_Logger.Log(
              "Applying safe knife visuals to weapon[%d] (defIdx=%d)", w,
              defIndex);
          SC_ApplyKnifeDefinition(weapon);
          shouldUpdate = true;
        }

        if (!runtimeKnifeBound)
          Utils::SafeWrite<uint32_t>(item + Offsets::sc_m_iItemIDHigh,
                                     (uint32_t)-1);

        SkinInfo_t knifeSkin = g_SkinManager->GetSkin(WEP_CtKnife);
        if (knifeSkin.paintKit == 0)
          knifeSkin = g_SkinManager->GetSkin(WEP_TKnife);

        if (knifeSkin.paintKit > 0) {
          Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackPaintKit,
                                    knifeSkin.paintKit);
          Utils::SafeWrite<float>(weapon + Offsets::sc_m_flFallbackWear,
                                  knifeSkin.wear);
          Utils::SafeWrite<int32_t>(weapon + Offsets::sc_m_nFallbackSeed,
                                    knifeSkin.seed);
        }

        SkinChanger::g_PendingKnifeWeapon = weapon;
        SkinChanger::g_PendingKnifeIndex = Globals::sc_selected_knife;
      }
      continue;
    }

    WeaponsEnum weaponType = static_cast<WeaponsEnum>(defIndex);
    SkinInfo_t skin = g_SkinManager->GetSkin(weaponType);
    if (skin.paintKit == 0)
      continue;

    int runtimeSlot = 0;
    uint64_t runtimeItemId = 0;
    const int team = Utils::SafeRead<int>(localPawn + Offsets::m_iTeamNum);
    bool runtimeItemSelected = InventoryChanger::GetEquippedRuntimeItem(
        defIndex, team, runtimeSlot, runtimeItemId);
    if (!runtimeItemSelected) {
      // Picked-up enemy weapon: use the corresponding item from the other
      // loadout. This mirrors SC_GetRuntimeLoadoutItem's selection order.
      runtimeItemSelected = InventoryChanger::GetEquippedRuntimeItem(
          defIndex, team == 2 ? 3 : 2, runtimeSlot, runtimeItemId);
    }

    const uint64_t itemIdBefore =
        Utils::SafeRead<uint64_t>(item + Offsets::sc_m_iItemID);
    const bool disallowSOCBefore =
        Utils::SafeRead<bool>(item + Offsets::sc_m_bDisallowSOC);
    const bool runtimeIdentityWasCurrent =
        runtimeItemSelected && itemIdBefore == runtimeItemId &&
        !disallowSOCBefore;
    bool runtimeIdentityBound = false;
    if (runtimeItemSelected) {
      runtimeIdentityBound = runtimeIdentityWasCurrent ||
          SC_BindRuntimeItemToWeapon(localPawn, weapon, defIndex, false);
    }

    const uint64_t currentItemId =
        Utils::SafeRead<uint64_t>(item + Offsets::sc_m_iItemID);
    const uint32_t currentIDHigh =
        Utils::SafeRead<uint32_t>(item + Offsets::sc_m_iItemIDHigh);
    const uint32_t currentIDLow =
        Utils::SafeRead<uint32_t>(item + Offsets::sc_m_iItemIDLow);
    const int32_t currentFallbackPK =
        Utils::SafeRead<int32_t>(weapon + Offsets::sc_m_nFallbackPaintKit);
    const int resolvedPaint = SC_GetResolvedPaintKit(item);

    const bool needsPaintApply = runtimeIdentityBound
        ? ((Globals::sc_force_update > 0) ||
           !runtimeIdentityWasCurrent || resolvedPaint != skin.paintKit)
        : ((Globals::sc_force_update > 0) ||
           currentIDHigh != UINT32_MAX || currentIDLow != UINT32_MAX ||
           currentFallbackPK != skin.paintKit ||
           resolvedPaint != skin.paintKit);

    if (needsPaintApply) {
      if (logThrottle % 300 == 0 || (Globals::sc_force_update > 0)) {
        g_Logger.Log("Applying weapon skin: weapon[%d] defIdx=%d paintKit=%d "
                     "(sourceSOC=%d selectedSOC=%d runtimeId=%llu slot=%d "
                     "liveId=%llu resolved=%d currentPK=%d IDs=%08X/%08X)",
                     w, defIndex, skin.paintKit, runtimeIdentityBound,
                     runtimeItemSelected, runtimeItemId, runtimeSlot,
                     currentItemId, resolvedPaint, currentFallbackPK,
                     currentIDHigh, currentIDLow);
      }
      SC_ApplyWeaponSkin(weapon, skin.paintKit, skin.wear, skin.seed,
                         skin.statTrak, runtimeIdentityBound);
      shouldUpdate = true;
      shouldRegenerateWeaponSkins = true;
    }
  }
  if (shouldRegenerateWeaponSkins) {
    // Call the global weapon skin regeneration function if available. This
    // triggers the engine to rebuild composite materials for all networked
    // weapons, fixing the black-skin issue when paint attributes are written
    // but the material system hasn't refreshed yet.
    SafeCallRegenerateWeaponSkins();
    if (SkinChanger::g_MeshUpdateFrames < 30)
      SkinChanger::g_MeshUpdateFrames = 30;
  }

  if (shouldUpdate) {
    for (int w = 0; w < weaponCount; w++) {
      uintptr_t weapon = weaponBuffer[w];
      uintptr_t item =
          weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
      uint16_t defIndex2 =
          Utils::SafeRead<uint16_t>(item + Offsets::sc_m_iItemDefinitionIndex);

      if (IsKnife(defIndex2) && Globals::sc_selected_knife > 0) {
        SkinChanger::g_PendingKnifeWeapon = weapon;
        SkinChanger::g_PendingKnifeIndex = Globals::sc_selected_knife;
      }
    }
  }

  const int pawnHealth =
      Utils::SafeRead<int>(localPawn + Offsets::m_iHealth);
  const uint8_t pawnLifeState =
      Utils::SafeRead<uint8_t>(localPawn + Offsets::m_lifeState);
  if (pawnHealth > 0 && pawnLifeState == 0) {
    // Keep the stage-6 task alive when selection becomes zero so the genuine
    // slot-41 item can be copied back exactly once.
    SkinChanger::g_PendingGlovePawn = localPawn;
    SkinChanger::g_PendingGloveWeaponCount = weaponCount;
    for (int i = 0; i < weaponCount && i < 64; i++)
      SkinChanger::g_PendingGloveWeapons[i] = weaponBuffer[i];
    SkinChanger::g_PendingGloveApply = true;
  } else {
    SC_ResetGloveState();
  }

  SC_ApplyMusicKit(localController);

  {
    int playerTeam = Utils::SafeRead<int>(localPawn + Offsets::m_iTeamNum);
    int agentIdx = -1;
    const char *agentModel = nullptr;

    if (playerTeam == 3 && !Globals::sc_inventory_agent_ct_model.empty()) {
      agentModel = Globals::sc_inventory_agent_ct_model.c_str();
    } else if (playerTeam == 2 &&
               !Globals::sc_inventory_agent_t_model.empty()) {
      agentModel = Globals::sc_inventory_agent_t_model.c_str();
    } else if (playerTeam == 3 && Globals::sc_selected_agent_ct >= 0 &&
        Globals::sc_selected_agent_ct < (int)AgentsCT.size()) {
      agentIdx = Globals::sc_selected_agent_ct;
      agentModel = AgentsCT[agentIdx].model.c_str();
    } else if (playerTeam == 2 && Globals::sc_selected_agent_t >= 0 &&
               Globals::sc_selected_agent_t < (int)AgentsT.size()) {
      agentIdx = Globals::sc_selected_agent_t;
      agentModel = AgentsT[agentIdx].model.c_str();
    }

    if (agentModel && agentModel[0] != '\0') {
      SkinChanger::g_PendingAgentPawn = localPawn;
      strncpy_s(SkinChanger::g_PendingAgentModel, agentModel, _TRUNCATE);
      SkinChanger::g_PendingAgentApply = true;
    } else {
      SkinChanger::g_PendingAgentApply = false;
      SkinChanger::g_PendingAgentModel[0] = '\0';
    }
  }

  if (Globals::sc_force_update > 0)
    Globals::sc_force_update--;
}

static bool SafeRunOuter() {
  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) {
      SC_ResetGloveState();
      return false;
    }
    uintptr_t localPawn = Memory::Globals::LocalPawn();
    uintptr_t localController =
        Utils::SafeRead<uintptr_t>(client + Offsets::dwLocalPlayerController);
    if (!localPawn || !Utils::IsValidPtr(localPawn)) {
      SC_ResetGloveState();
      return false;
    }
    if (!localController || !Utils::IsValidPtr(localController)) {
      SC_ResetGloveState();
      return false;
    }
    int health = Utils::SafeRead<int>(localPawn + Offsets::m_iHealth);
    uint8_t lifeState =
        Utils::SafeRead<uint8_t>(localPawn + Offsets::m_lifeState);
    if (health <= 0 || lifeState != 0) {
      SC_ResetGloveState();
      return false;
    }

    static int cleanupCounter = 0;
    if (++cleanupCounter > 600) {
      SC_FreeOldAttributeMemory();
      cleanupCounter = 0;
    }

    SC_RunLogic(client, localPawn, localController);
    return true;
  } __except ((g_Logger.Log("EXCEPTION in SafeRunOuter! code=0x%08X",
                            GetExceptionCode()),
               EXCEPTION_EXECUTE_HANDLER)) {
    SC_ResetGloveState();
    SC_CleanupAllMemory();
    return false;
  }
}

namespace SkinChanger {
void PrepareDatabase() {
  try {
    g_Logger.Init();

    if (!g_SkinManager) {
      g_SkinManager = new SkinManager();
      g_Logger.Log("SkinManager created (database-only profile)");
    }
    if (!g_SkinDB) {
      g_SkinDB = new CSkinDB();
      g_Logger.Log("SkinDB created");
    }
  } catch (const std::exception& error) {
    g_Logger.Log("SkinDB container initialization std::exception: %s",
                 error.what());
  } catch (...) {
    g_Logger.Log("SkinDB container initialization unknown C++ exception");
  }
}

void SetupDatabase(void* module) {
  try {
    PrepareDatabase();
    if (!g_SkinDB) {
      g_Logger.Log("SkinDB embedded catalogue skipped: container unavailable");
      return;
    }

    bool expected = false;
    if (!g_SkinDatabaseLoaded.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
      return;

    const HMODULE selfModule =
        module ? static_cast<HMODULE>(module)
               : reinterpret_cast<HMODULE>(&__ImageBase);
    g_Logger.Log("SkinDB embedded catalogue load started: module=%p",
                 selfModule);
    if (g_SkinDB)
      g_SkinDB->DumpEmbedded(selfModule, IDR_INVENTORY_SKINS_JSON,
                             IDR_INVENTORY_AGENTS_JSON);
    const bool dumped = g_SkinDB && g_SkinDB->IsDumped();
    g_Logger.Log("SkinDB embedded catalogue load completed: dumped=%d",
                 dumped ? 1 : 0);
    if (!dumped)
      g_SkinDatabaseLoaded.store(false, std::memory_order_release);
  } catch (const std::exception& error) {
    g_SkinDatabaseLoaded.store(false, std::memory_order_release);
    g_Logger.Log("SkinDB embedded catalogue std::exception: %s",
                 error.what());
  } catch (...) {
    g_SkinDatabaseLoaded.store(false, std::memory_order_release);
    g_Logger.Log("SkinDB embedded catalogue unknown C++ exception");
  }
}

void Setup() {
  if (g_Initialized)
    return;

  SetupDatabase();
  g_Logger.Log("SkinChanger::Setup() called");

  if (!g_SkinManager) {
    g_SkinManager = new SkinManager();
    g_Logger.Log("SkinManager created");

    // NOTE: previously seeded "default" skins (paintKit 1171 on AK/M4A4/Deagle/
    // Glock/UspS) as a RegenerateWeaponSkins trigger. That forced an unwanted
    // finish onto weapons the user never configured (a plain pistol would show
    // a skin), so it is removed. Regen is already triggered whenever a real
    // user-selected skin is applied (shouldRegenerateWeaponSkins in SC_RunLogic),
    // so nothing is lost — unconfigured weapons now stay vanilla.
  }
  if (!g_SkinDB) {
    g_SkinDB = new CSkinDB();
    g_Logger.Log("SkinDB created, starting download thread...");
  }

  g_Logger.Log("Running pattern scan for SetModel...");
  SafeSetupInternal();

  g_Logger.Log("Setup complete. fnSetModel=%p, g_Initialized=%d", fnSetModel,
               g_Initialized);

  g_Logger.Log("=== OFFSET DUMP ===");
  g_Logger.Log("sc_m_AttributeManager = 0x%X", Offsets::sc_m_AttributeManager);
  g_Logger.Log("sc_m_Item = 0x%X", Offsets::sc_m_Item);
  g_Logger.Log("sc_m_iItemDefinitionIndex = 0x%X",
               Offsets::sc_m_iItemDefinitionIndex);
  g_Logger.Log("sc_m_iItemIDHigh = 0x%X", Offsets::sc_m_iItemIDHigh);
  g_Logger.Log("sc_m_nFallbackPaintKit = 0x%X",
               Offsets::sc_m_nFallbackPaintKit);
  g_Logger.Log("sc_m_flFallbackWear = 0x%X", Offsets::sc_m_flFallbackWear);
  g_Logger.Log("sc_m_nFallbackSeed = 0x%X", Offsets::sc_m_nFallbackSeed);
  g_Logger.Log("sc_m_EconGloves = 0x%X", Offsets::sc_m_EconGloves);
  g_Logger.Log("sc_m_bNeedToReApplyGloves = 0x%X",
               Offsets::sc_m_bNeedToReApplyGloves);
  g_Logger.Log("sc_m_pInventoryServices = 0x%X",
               Offsets::sc_m_pInventoryServices);
  g_Logger.Log("sc_m_unMusicID = 0x%X", Offsets::sc_m_unMusicID);
  g_Logger.Log("sc_m_hMyWeapons = 0x%X", Offsets::sc_m_hMyWeapons);
  g_Logger.Log("m_pWeaponServices = 0x%X", Offsets::m_pWeaponServices);
  g_Logger.Log("m_pGameSceneNode = 0x%X", Offsets::m_pGameSceneNode);
  g_Logger.Log("fnSetMeshGroupMask = 0x%llX", fnSetMeshGroupMask);
  g_Logger.Log("fnCopyEconItemView = 0x%llX", fnCopyEconItemView);
  g_Logger.Log("fnGloveApplyPerTick = 0x%llX", fnGloveApplyPerTick);
  g_Logger.Log("g_InventoryManager = 0x%llX", g_InventoryManager);
  g_Logger.Log("sc_m_flLastSpawnTimeIndex = 0x%X",
               Offsets::sc_m_flLastSpawnTimeIndex);
  g_Logger.Log("sc_m_iItemID = 0x%X", Offsets::sc_m_iItemID);
  g_Logger.Log("sc_m_iItemIDLow = 0x%X", Offsets::sc_m_iItemIDLow);
  g_Logger.Log("sc_m_hOwnerEntity = 0x%X", Offsets::sc_m_hOwnerEntity);
  g_Logger.Log("sc_m_hHudModelArms = 0x%X", Offsets::sc_m_hHudModelArms);
  if (Offsets::sc_m_hOwnerEntity != 0x520)
    g_Logger.Log(
        "WARNING: sc_m_hOwnerEntity mismatch! current=0x%X expected=0x520",
        Offsets::sc_m_hOwnerEntity);
  if (Offsets::sc_m_hHudModelArms != 0x1B7C)
    g_Logger.Log(
        "WARNING: sc_m_hHudModelArms mismatch! current=0x%X expected=0x1B7C",
        Offsets::sc_m_hHudModelArms);
  if (Offsets::sc_m_bNeedToReApplyGloves != 0x1685)
    g_Logger.Log("WARNING: sc_m_bNeedToReApplyGloves mismatch! current=0x%X "
                 "expected=0x1685",
                 Offsets::sc_m_bNeedToReApplyGloves);
  if (Offsets::sc_m_EconGloves != 0x1688)
    g_Logger.Log(
        "WARNING: sc_m_EconGloves mismatch! current=0x%X expected=0x1688",
        Offsets::sc_m_EconGloves);
  g_Logger.Log("=== END OFFSET DUMP ===");
}

static bool RestoreGlovesAfterMasterDisable() {
  const uintptr_t localPawn = Memory::Globals::LocalPawn();
  if (!localPawn || !Utils::IsValidPtr(localPawn))
    return false;

  const uintptr_t econGloves = localPawn + Offsets::sc_m_EconGloves;
  if (!SC_RestoreEmbeddedGloveView(localPawn, econGloves))
    return false;

  const bool nativeApplied = SafeCallGloveApply(localPawn);
  SC_ResetGloveState();
  g_Logger.Log("Master-disable glove restore: pawn=0x%llX nativeApply=%d",
               localPawn, nativeApplied);
  return nativeApplied;
}

void Run() {
  std::lock_guard<std::recursive_mutex> stateLock(g_SkinStateMutex);
  static bool wasEnabled = false;
  static bool restorePending = false;
  static ULONGLONG lastRestoreAttempt = 0;

  if (!Globals::sc_enabled) {
    if (wasEnabled)
      restorePending = true;
    wasEnabled = false;

    const ULONGLONG now = GetTickCount64();
    if (restorePending && now - lastRestoreAttempt >= 500) {
      lastRestoreAttempt = now;
      restorePending = !RestoreGlovesAfterMasterDisable();
    }
    return;
  }

  wasEnabled = true;
  restorePending = false;
  SafeRunOuter();
}

bool InstallHooks() {
  if (g_SetModelHookInstalled)
    return true;
  if (!fnSetModel) {
    g_Logger.Log("InstallHooks: SetModel pointer is null, skipping hook.");
    return false;
  }

  MH_STATUS createStatus =
      MH_CreateHook(reinterpret_cast<void *>(fnSetModel), &hkSetModel,
                    reinterpret_cast<void **>(&oSetModel));

  if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
    g_Logger.Log("InstallHooks: MH_CreateHook(SetModel) failed (%d)",
                 createStatus);
    return false;
  }

  MH_STATUS enableStatus =
      MH_QueueEnableHook(reinterpret_cast<void *>(fnSetModel));
  if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
    g_Logger.Log("InstallHooks: MH_QueueEnableHook(SetModel) failed (%d)",
                 enableStatus);
    return false;
  }

  g_SetModelHookInstalled = true;
  g_Logger.Log(
      "InstallHooks: SetModel hook queued. target=0x%llX original=%p",
      fnSetModel, oSetModel);
  return true;
}

static void RunVisualsLocked(int frameStage) {
  if (!Globals::sc_enabled || !g_Initialized)
    return;

  __try {
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) {

      SC_CleanupAllMemory();
      SC_ResetGloveState();
      g_PendingKnifeWeapon = 0;
      g_PendingAgentPawn = 0;
      g_PendingAgentApply = false;
      g_PendingAgentModel[0] = '\0';
      g_LastValidPawn = 0;
      g_FramesSinceLastValidPawn = 0;
      return;
    }

    uintptr_t localPawn = Memory::Globals::LocalPawn();
    if (!localPawn || !Utils::IsValidPtr(localPawn)) {

      g_FramesSinceLastValidPawn++;

      if (g_FramesSinceLastValidPawn > 300) {
        SC_CleanupAllMemory();
        SC_ResetGloveState();
        g_PendingKnifeWeapon = 0;
        g_PendingAgentPawn = 0;
        g_PendingAgentApply = false;
        g_PendingAgentModel[0] = '\0';
        g_LastValidPawn = 0;
        g_FramesSinceLastValidPawn = 0;
      }
      return;
    }

    g_FramesSinceLastValidPawn = 0;
    if (g_LastValidPawn != 0 && g_LastValidPawn != localPawn) {

      g_Logger.Log("Map change detected (old pawn=0x%llX, new=0x%llX), "
                   "cleaning up memory",
                   g_LastValidPawn, localPawn);
      SC_CleanupAllMemory();
      SC_ResetGloveState();
      g_PendingKnifeWeapon = 0;
      g_PendingAgentPawn = 0;
      g_PendingAgentApply = false;
      g_PendingAgentModel[0] = '\0';
    }
    g_LastValidPawn = localPawn;

    // A newly networked pawn can be visible through dwLocalPlayerPawn before
    // all of its client interfaces/vtables are finalized. Defer model and
    // embedded glove mutations until the same pawn + spawn generation has
    // remained stable for a short period.
    static uintptr_t s_stabilityPawn = 0;
    static float s_stabilitySpawnTime = -1.f;
    static int s_pawnStableFrames = 0;
    const float stabilitySpawnTime = Utils::SafeRead<float>(
        localPawn + Offsets::sc_m_flLastSpawnTimeIndex);
    if (s_stabilityPawn != localPawn ||
        s_stabilitySpawnTime != stabilitySpawnTime) {
      s_stabilityPawn = localPawn;
      s_stabilitySpawnTime = stabilitySpawnTime;
      s_pawnStableFrames = 0;
    } else if (s_pawnStableFrames < 300) {
      ++s_pawnStableFrames;
    }

    int health = Utils::SafeRead<int>(localPawn + Offsets::m_iHealth);
    uint8_t lifeState =
        Utils::SafeRead<uint8_t>(localPawn + Offsets::m_lifeState);
    if (health <= 0 || lifeState != 0) {

      g_PendingKnifeWeapon = 0;
      SC_ResetGloveState();
      return;
    }

    if (frameStage == 6) {

      if (s_pawnStableFrames >= 120 && g_PendingAgentApply &&
          g_PendingAgentPawn &&
          g_PendingAgentModel[0] != '\0') {
        static float s_lastAgentSpawnTime = -1.f;
        static int s_lastAgentCT = -1;
        static int s_lastAgentT = -1;
        static int s_agentRefreshFrames = 0;

        if (g_PendingAgentPawn == localPawn &&
            Utils::IsValidPtr(g_PendingAgentPawn)) {
          float spawnTime = Utils::SafeRead<float>(
              localPawn + Offsets::sc_m_flLastSpawnTimeIndex);
          int playerTeam =
              Utils::SafeRead<int>(localPawn + Offsets::m_iTeamNum);

          int currentAgentIdx = (playerTeam == 3)
                                    ? Globals::sc_selected_agent_ct
                                    : Globals::sc_selected_agent_t;
          int lastAgentIdx = (playerTeam == 3) ? s_lastAgentCT : s_lastAgentT;

          bool needsApply = (spawnTime != s_lastAgentSpawnTime) ||
                            (Globals::sc_force_update > 0) ||
                            (currentAgentIdx != lastAgentIdx);

          if (needsApply) {
            s_agentRefreshFrames = 6;
            s_lastAgentSpawnTime = spawnTime;
            if (playerTeam == 3)
              s_lastAgentCT = currentAgentIdx;
            else
              s_lastAgentT = currentAgentIdx;
          }

          if (s_agentRefreshFrames > 0) {
            SafeCallSetModel((void *)localPawn, g_PendingAgentModel);
            if (s_agentRefreshFrames == 6 || s_agentRefreshFrames == 1) {
              g_Logger.Log("[AGENT] Applied persistent model: %s "
                           "(team=%d spawnTime=%.2f frame=%d)",
                           g_PendingAgentModel, playerTeam, spawnTime,
                           s_agentRefreshFrames);
            }
            --s_agentRefreshFrames;
          }
        }
      }

      if (g_SkinManager) {
        uintptr_t activeWeaponForMesh = SC_GetActiveWeapon(localPawn);
        if (activeWeaponForMesh && Utils::IsValidPtr(activeWeaponForMesh)) {
          uintptr_t wepItem = activeWeaponForMesh +
                              Offsets::sc_m_AttributeManager +
                              Offsets::sc_m_Item;
          uint16_t wepDefIdx = Utils::SafeRead<uint16_t>(
              wepItem + Offsets::sc_m_iItemDefinitionIndex);

          if (!IsKnife(wepDefIdx)) {
            SC_ProcessCompositeRefresh(localPawn, activeWeaponForMesh);

            WeaponsEnum wepType = static_cast<WeaponsEnum>(wepDefIdx);
            int wepPaintKit = SC_GetPaintKitForWeapon(wepType);
            if (wepPaintKit > 0) {
              uint64_t wepMask = SC_GetMeshGroupMask(
                  wepPaintKit, (int)wepDefIdx,
                  SC_IsLegacyPaintKitForWeapon(wepType));

              bool needsForceReset = (g_MeshUpdateFrames >= 28);

              uintptr_t wepSceneNode = Utils::SafeRead<uintptr_t>(
                  activeWeaponForMesh + Offsets::m_pGameSceneNode);
              if (wepSceneNode && Utils::IsValidPtr(wepSceneNode)) {
                if (needsForceReset)
                  SafeCallSetMeshGroupMask((void *)wepSceneNode, 0);
                SafeCallSetMeshGroupMask((void *)wepSceneNode, wepMask);
              }

              uintptr_t hudWeaponNode =
                  SC_GetHudModelWeaponSceneNode(localPawn, activeWeaponForMesh);
              if (hudWeaponNode && Utils::IsValidPtr(hudWeaponNode)) {
                if (needsForceReset)
                  SafeCallSetMeshGroupMask((void *)hudWeaponNode, 0);
                SafeCallSetMeshGroupMask((void *)hudWeaponNode, wepMask);
              }

              if (g_LoggedNormalMeshPaint[activeWeaponForMesh] != wepPaintKit) {
                g_LoggedNormalMeshPaint[activeWeaponForMesh] = wepPaintKit;
                g_Logger.Log("Normal weapon mesh bound: weapon=0x%llX def=%u "
                             "paint=%d legacy=%d desired=%llu worldNode=0x%llX "
                             "hudNode=0x%llX",
                             activeWeaponForMesh, wepDefIdx, wepPaintKit,
                             SC_IsLegacyPaintKitForWeapon(wepType), wepMask,
                             wepSceneNode, hudWeaponNode);
              }
            }
          }
        }
      }

      if (s_pawnStableFrames >= 90 && g_PendingGloveApply &&
          g_PendingGlovePawn) {
        if (g_PendingGlovePawn != localPawn) {
          g_Logger.Log("RunVisuals (Stage 6): stale glove pawn "
                       "(pending=0x%llX current=0x%llX)",
                       g_PendingGlovePawn, localPawn);
          SC_ResetGloveState();
        } else if (Utils::IsValidPtr(g_PendingGlovePawn)) {
          SC_ApplyGloves(g_PendingGlovePawn, g_PendingGloveWeapons,
                         g_PendingGloveWeaponCount);
        }
      }

      if (g_MeshUpdateFrames > 0)
        g_MeshUpdateFrames--;
    }

    // FRAME_RENDER_END is stage 6 in the current client. The old stage-7
    // branch was unreachable, so knife models were never applied.
    if (frameStage == 6) {
      int knifeIdx = Globals::sc_selected_knife > 0 ? Globals::sc_selected_knife
                                                    : g_PendingKnifeIndex;
      if (knifeIdx > 0 && knifeIdx <= (int)Knives.size()) {
        const char *model = Knives[knifeIdx - 1].model.c_str();

        if (Globals::customweapon_enabled && CustomWeapon::g_SelectedIdx > 0 && 
            CustomWeapon::g_SelectedIdx < (int)CustomWeapon::g_ModelList.size()) {
            model = CustomWeapon::g_ModelList[CustomWeapon::g_SelectedIdx].resourcePath.c_str();
            CustomModel::PrecacheModel(model);
        }

        if (model && model[0] != '\0') {
          uintptr_t activeWeapon = SC_GetActiveWeapon(localPawn);
          uintptr_t weapon = (activeWeapon && Utils::IsValidPtr(activeWeapon))
                                 ? activeWeapon
                                 : g_PendingKnifeWeapon;

          if (weapon && Utils::IsValidPtr(weapon)) {
            uintptr_t item =
                weapon + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
            uint16_t activeDef = Utils::SafeRead<uint16_t>(
                item + Offsets::sc_m_iItemDefinitionIndex);

            if (IsKnife(activeDef)) {
              g_PendingKnifeWeapon = weapon;
              g_PendingKnifeIndex = knifeIdx;

              uintptr_t sceneNode = Utils::SafeRead<uintptr_t>(
                  weapon + Offsets::m_pGameSceneNode);
              if (sceneNode && Utils::IsValidPtr(sceneNode)) {
                void **sceneVtable = *(void ***)sceneNode;
                if (sceneVtable && Utils::IsValidPtr((uintptr_t)sceneVtable)) {
                  static int delayFrames = 0;
                  static uintptr_t lastAppliedWeapon = 0;
                  static int lastAppliedKnife = 0;

                  bool needsApply = (lastAppliedWeapon != weapon) ||
                                    (lastAppliedKnife != knifeIdx) ||
                                    (Globals::sc_force_update > 0);
                  if (needsApply) {
                    if (Globals::sc_force_update > 0)
                      delayFrames = 8;
                    else
                      delayFrames++;
                    if (delayFrames >= 8) {
                      static int visLogThrottle = 0;
                      if (visLogThrottle++ % 600 == 0 || needsApply) {
                        g_Logger.Log("RunVisuals KNIFE (Stage 6): "
                                     "weapon=0x%llX model=%s",
                                     weapon, model);
                      }

                      const uint16_t targetKnifeDef =
                          Knives[knifeIdx - 1].defIndex;
                      const bool runtimeKnifeBound =
                          SC_BindRuntimeItemToWeapon(
                              localPawn, weapon, targetKnifeDef, true);
                      if (!runtimeKnifeBound)
                        g_Logger.Log("  [KNIFE] Runtime identity unavailable; "
                                     "using visual model fallback");

                      SafeCallSetModel((void *)weapon, model);

                      const uintptr_t hudKnife =
                          SC_GetHudModelWeaponEntity(localPawn, weapon);
                      if (hudKnife && Utils::IsValidPtr(hudKnife))
                        SafeCallSetModel((void *)hudKnife, model);

                      if (runtimeKnifeBound)
                        g_Logger.Log("  [KNIFE] UpdateComposite result=%d",
                                     SafeCallUpdateComposite(weapon, true));

                      if (sceneNode && Utils::IsValidPtr(sceneNode)) {

                        int kPaintKit = SC_GetKnifePaintKit();
                        uint64_t knifeMask = SC_GetMeshGroupMask(
                            kPaintKit, (int)activeDef,
                            SC_IsLegacyPaintKitForWeapon(WEP_CtKnife));
                        SafeCallSetMeshGroupMask((void *)sceneNode, knifeMask);

                        uintptr_t hudKnifeNode =
                            SC_GetHudModelWeaponSceneNode(localPawn, weapon);
                        if (hudKnifeNode && Utils::IsValidPtr(hudKnifeNode)) {
                          SafeCallSetMeshGroupMask((void *)hudKnifeNode,
                                                   knifeMask);
                        }

                        g_Logger.Log(
                            "  [KNIFE] SetMeshGroupMask(%llu) paintKit=%d",
                            knifeMask, kPaintKit);
                      }

                      lastAppliedWeapon = weapon;
                      lastAppliedKnife = knifeIdx;
                      delayFrames = 0;
                    }
                  } else {
                    delayFrames = 0;
                  }
                }
              }
            }
          }
        }
      }

    }

  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_Logger.Log("EXCEPTION in RunVisuals! code=0x%08X", GetExceptionCode());

    SC_CleanupAllMemory();
    SC_ResetGloveState();
    g_PendingKnifeWeapon = 0;
    g_PendingAgentPawn = 0;
    g_PendingAgentApply = false;
    g_PendingAgentModel[0] = '\0';
    g_LastValidPawn = 0;
  }
}

void RunVisuals(int frameStage) {
  std::lock_guard<std::recursive_mutex> stateLock(g_SkinStateMutex);
  RunVisualsLocked(frameStage);
}

static void LogDebug(const std::string& msg) {
  try {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, path))) {
      std::filesystem::path p = path;
      p /= "Raven";
      if (!std::filesystem::exists(p)) {
        std::filesystem::create_directories(p);
      }
      p /= "skinchanger_debug.log";
      std::ofstream out(p, std::ios::app);
      if (out.is_open()) {
        out << msg << "\n";
      }
    }
  } catch (...) {}
}

} // namespace SkinChanger
