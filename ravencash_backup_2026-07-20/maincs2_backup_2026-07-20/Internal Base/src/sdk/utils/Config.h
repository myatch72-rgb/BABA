#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace Config
{
    inline constexpr int schema_version = 2;

    void Save(std::string name);
    void Load(std::string name);
    void Delete(std::string name);
    void Refresh();
    
    inline std::vector<std::string> configs;
    inline std::string current_config = "";
    inline std::string last_status = "Ready";
    
    std::filesystem::path GetConfigPath();
}
