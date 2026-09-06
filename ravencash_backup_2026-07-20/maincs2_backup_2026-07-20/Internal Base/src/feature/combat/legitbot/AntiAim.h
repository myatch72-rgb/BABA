#pragma once

class c_user_cmd;

namespace AntiAim {
    // Called from hkCreateMove every tick (~64Hz).
    void Run(c_user_cmd* cmd);
}
