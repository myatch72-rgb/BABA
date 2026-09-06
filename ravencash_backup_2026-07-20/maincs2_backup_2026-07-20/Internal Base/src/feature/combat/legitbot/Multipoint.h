#pragma once
#include <vector>
#include "../../../sdk/entity/Classes.h"
#include "../../../sdk/utils/Vector.h"

#define HITBOX_MAX 64

namespace Multipoint
{
    struct HitboxPointSet {
        int arrayIndex;
        int hitboxId;
        int shapeType;
        const char* name;
        std::vector<Vector> points;
    };

    // Generates multipoint positions for a specific pawn.
    // pointscale: percentage of hitbox radius to shrink points (0.0 to 1.0)
    // targetHitbox: default 6 (Head). If -1, generates for all hitboxes.
    std::vector<HitboxPointSet> GetHitboxWorldPoints(C_CSPlayerPawn* pawn, float pointscale, int targetHitbox = 6);
}
