#include <imgui.h>
#include <d3d11.h>


ImFont* poppins = nullptr;
ImFont* font_icon = nullptr;


ID3D11ShaderResourceView* logo = nullptr;
ID3D11ShaderResourceView* logotwo = nullptr;
ID3D11ShaderResourceView* foto_user = nullptr;


char discord_username[64] = "bouy";
char expiry_date[64] = "25.09.2023";


float accent_colour[4] = { 1.f, 1.f, 1.f, 1.f };
bool particles = true;
int old_tab = 0;
float content_animation = 0.0f;
float dpi_scale = 1.0f;
const char* games_list[4] = { "Counter-Strike: GO", "Dying Light", "Apex Legends", "Warface" };
ID3D11ShaderResourceView* logo_png = nullptr;

// -------------------------------------------------------------------------
// Custom Model Changer
// -------------------------------------------------------------------------
#include <filesystem>
#include <Windows.h>
#include "Globals.h"
namespace Globals {
    void ScanCustomModels() {
        sc_custom_models.clear();
        sc_custom_models.push_back({ "[ OFF ]", "" });
        sc_custom_model_idx = 0;

        char cwd[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, cwd);
        std::string root = cwd;
       
        auto pos = root.find("bin\\win64");
        if (pos != std::string::npos) {
            root.replace(pos, 9, "csgo\\characters\\models");
        }

        if (std::filesystem::exists(root)) {
            for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
                if (p.path().extension() == ".vmdl_c") {
                    std::string full = p.path().string();
                    std::string rel = full.substr(full.find("characters\\"));
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    rel = rel.substr(0, rel.find(".vmdl_c")) + ".vmdl";
                   
                    std::string fname = p.path().stem().string();
                    sc_custom_models.push_back({ fname, rel });
                }
            }
        }
    }
}
