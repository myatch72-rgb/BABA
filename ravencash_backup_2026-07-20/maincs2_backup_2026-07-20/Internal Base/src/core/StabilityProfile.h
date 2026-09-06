#pragma once

namespace StabilityProfile {

// Keep injection itself minimal. Inventory is activated only after the first
// completed Present cycles; the remaining runtime hooks are installed in a
// second, post-render stage after Inventory has proven ready.
inline constexpr bool EnableInventory = true;
inline constexpr bool EnableFullRuntime = true;
inline constexpr bool MenuOnly = !EnableInventory && !EnableFullRuntime;

} // namespace StabilityProfile
