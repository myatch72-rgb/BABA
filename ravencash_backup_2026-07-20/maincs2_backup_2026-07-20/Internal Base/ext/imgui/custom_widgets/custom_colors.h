#include "../imgui.h"

class c_colors
{
public:
    ImVec4 white_color = ImColor(255, 255, 255);
    ImVec4 black_color = ImColor(0, 0, 0);

    ImVec4 accent_color = ImColor(140, 155, 253);
    ImVec4 window_background = ImColor(9, 10, 11);

    ImVec4 selection_active = ImColor(14, 15, 17);

    ImVec4 child_top_bar = ImColor(17, 19, 23);
    ImVec4 child_bottom_bar = ImColor(11, 12, 14);
    ImVec4 child_stroke_bar = ImColor(20, 23, 28);

    ImVec4 element_background = ImColor(27, 29, 34);
    ImVec4 keybind_background = ImColor(20, 21, 25);

    ImVec4 combo_color = ImColor(15, 16, 19);
    ImVec4 separator = ImColor(19, 20, 22);

    ImVec4 text_active = ImColor(255, 255, 255);
    ImVec4 text = ImColor(42, 45, 55);

};

inline c_colors* clr = new c_colors();
