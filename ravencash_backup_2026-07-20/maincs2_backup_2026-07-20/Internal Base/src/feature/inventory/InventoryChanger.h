#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace InventoryChanger {

struct ItemRequest {
  std::uint16_t defIndex = 0;
  int paintKit = 0;
  float wear = 0.001f;
  int seed = 0;
  int statTrak = -1;
  int rarity = 1;
  bool unusual = false;
  bool legacy = false;
  std::string name;
  std::string model;
  int team = 0;
};

// Resolves the current client econ entry points and loads saved item metadata.
// Saved CEconItems are not bulk-injected during startup; no game object is
// touched from Setup's worker thread.
void Setup();

// Must be called after MinHook is initialized.
bool InstallHooks();

// Called from Present only. Explicitly queued items are startup-delayed and
// rate-limited so GC/SOCache notifications have a single thread owner.
void Run();

// Calls the native UnlockInventory function every Present frame when enabled.
// Removes the in-match inventory edit lock so skins can be changed live.
void RunUnlock();

// Forces sc_force_update so all weapon/glove skins are reapplied next tick.
void ForceReapplyAll();

// Thread-safe entry point used by the ImGui render thread.
bool QueueItem(const ItemRequest &request);

std::string GetStatus();
std::size_t GetPendingCount();
bool IsReady();
bool IsRuntimeItemId(std::uint64_t itemId);
// Returns the loadout slot/id most recently equipped through the runtime
// inventory hook for this definition and team.
bool GetEquippedRuntimeItem(std::uint16_t defIndex, int team, int &slot,
                            std::uint64_t &itemId);

} // namespace InventoryChanger
