#pragma once
#include <cstdint>

namespace OffsetResolver
{
    struct Result {
        int total;
        int resolved;
        int failed;
    };

    // Scans Patterns::Data signatures against loaded modules and overwrites
    // Offsets::dw* with resolved values. Falls back to hard-coded literal in
    // Offsets.h when a pattern fails to match. Safe to call once at startup.
    Result Resolve();
}
