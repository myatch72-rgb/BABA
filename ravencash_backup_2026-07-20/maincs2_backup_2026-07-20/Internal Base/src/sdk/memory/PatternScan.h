#pragma once
#include <cstdint>

namespace Memory
{
    uintptr_t GetModuleBase(const char* module);
    uintptr_t PatternScan(const char* module, const char* signature);

    // Resolves a RIP-relative reference at a pattern hit.
    // dispOffset = position of the 4-byte displacement within the instruction (typically 3).
    // instrLen   = total length of the instruction containing the disp32 (typically 7).
    // Returns the absolute address that the instruction references, or 0 on failure.
    uintptr_t ResolveRipRelative(const char* module, const char* signature,
                                 int dispOffset = 3, int instrLen = 7);
}
