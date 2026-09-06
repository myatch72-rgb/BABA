#pragma once
#include <cstdint>

class c_user_cmd;

namespace Movement
{
    // Button bit flags (CS2 IN_* constants)
    constexpr uint64_t IN_ATTACK    = 1ULL << 0;
    constexpr uint64_t IN_JUMP      = 1ULL << 1;
    constexpr uint64_t IN_DUCK      = 1ULL << 2;
    constexpr uint64_t IN_FORWARD   = 1ULL << 3;
    constexpr uint64_t IN_BACK      = 1ULL << 4;
    constexpr uint64_t IN_USE       = 1ULL << 5;
    constexpr uint64_t IN_MOVELEFT  = 1ULL << 9;
    constexpr uint64_t IN_MOVERIGHT = 1ULL << 10;

    // Called per CreateMove tick. Synchronises jump state and applies
    // mouse/A-D driven air strafe without camera or velocity writes.
    void Run(c_user_cmd* cmd);
}
