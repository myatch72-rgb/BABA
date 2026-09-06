# Backtrack.h

```cpp
#pragma once

#include "../../sdk/entity/C_CSPlayerPawn.h"
#include "../../sdk/memory/Convars.h"
#include <array>
#include <cstdint>
#include <memory>

namespace Backtrack
{
    struct SubtickRecord {
        uint64_t ns;
        Vector origin;
        Vector vel;
        float phase;
        uint8_t qs;
    };

    class CSubtickTracker {
    public:
        std::array<SubtickRecord, 16384> recs;
        size_t head = 0;

        void Init();
        SubtickRecord* Find(C_CSPlayerPawn* tgt);
        uint8_t Observe(C_CSPlayerPawn* p);
    };

    extern std::unique_ptr<CSubtickTracker> g_Tracker;
}
```
