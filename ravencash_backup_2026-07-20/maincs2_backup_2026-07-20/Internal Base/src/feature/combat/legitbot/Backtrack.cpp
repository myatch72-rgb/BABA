#include "Backtrack.h"
#include "../../sdk/memory/PatternScan.h"
#include <Windows.h>

namespace Backtrack
{
    std::unique_ptr<CSubtickTracker> g_Tracker = std::make_unique<CSubtickTracker>();

    void CSubtickTracker::Init() {
        void* cv = Memory::Convars::FindRaw("sv_maxunlag");
        if (!cv) return;
        
        // Exploit from UC: "float goes into byte at +23. write 0xFF and the compare wraps"
        *(reinterpret_cast<uint8_t*>(cv) + 23) = 0xFF;
    }

    SubtickRecord* CSubtickTracker::Find(C_CSPlayerPawn* tgt) {
        if (!tgt) return nullptr;

        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint64_t now = q.QuadPart * 100;

        for (size_t i = 0; i < recs.size(); ++i) {
            auto& r = recs[(head - i - 1) & (recs.size() - 1)];
            
            // If the record is older than ~15.6 seconds, break
            if (now - r.ns > 15625000000ULL) break;
            
            if (r.qs != Observe(tgt)) continue;
            return &r;
        }
        return nullptr;
    }

    uint8_t CSubtickTracker::Observe(C_CSPlayerPawn* p) {
        (void)p;
        return 0;
    }
}
