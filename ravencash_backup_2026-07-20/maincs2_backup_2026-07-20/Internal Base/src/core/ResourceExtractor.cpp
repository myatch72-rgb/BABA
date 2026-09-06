#include "ResourceExtractor.h"
#include "../../resource.h"
#include <string>
#include <filesystem>

namespace ResourceExtractor
{
    static const wchar_t* CS2_CSGO_PATH = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo";

    struct EmbeddedFile
    {
        int resourceId;
        const wchar_t* subFolder;   
        const wchar_t* fileName;
    };

    static const EmbeddedFile g_EmbeddedFiles[] =
    {
        
        { IDR_FONT_ARCADECLASSIC,   L"fonts", L"ARCADECLASSIC.TTF" },

        
        { IDR_WAV_XP_GAIN,          L"wav",   L"[minecraft] XP gain.wav" },
        { IDR_WAV_BOW_DING,         L"wav",   L"[minecraft] bow ding.wav" },
        { IDR_WAV_BUTTON,           L"wav",   L"[minecraft] button.wav" },
        { IDR_WAV_EGG_THROW,        L"wav",   L"[minecraft] egg throw.wav" },
        { IDR_WAV_HITSOUND,         L"wav",   L"[minecraft] hitsound.wav" },
        { IDR_WAV_OLD_HITSOUND,     L"wav",   L"[minecraft] old hitsound.wav" },
        { IDR_WAV_AIMBOOSTER,       L"wav",   L"aimbooster.wav" },
        { IDR_WAV_BEN,              L"wav",   L"ben.wav" },
        { IDR_WAV_BONK,             L"wav",   L"bonk.wav" },
        { IDR_WAV_BROTATO,          L"wav",   L"brotato.wav" },
        { IDR_WAV_CLICK,            L"wav",   L"click.wav" },
        { IDR_WAV_KICK,             L"wav",   L"kick.wav" },
        { IDR_WAV_MONEY_CLAIM,      L"wav",   L"money claim.wav" },
        { IDR_WAV_MOUTHSOUND,       L"wav",   L"mouthsound.wav" },
        { IDR_WAV_POP,              L"wav",   L"pop.wav" },
        { IDR_WAV_QUAVER,           L"wav",   L"quaver.wav" },
        { IDR_WAV_REGULUS,          L"wav",   L"regulus.wav" },
        { IDR_WAV_SATISFYING_CLICK, L"wav",   L"satisfying-click.wav" },
        { IDR_WAV_SPIRAL_KNIGHT,    L"wav",   L"spiral knight.wav" },
        { IDR_WAV_TAVERN_MISC1C,    L"wav",   L"tavern misc1c.wav" },
        { IDR_WAV_TING,             L"wav",   L"ting.wav" },
        { IDR_WAV_TRIDENT_PIERCE,   L"wav",   L"trident pierce.wav" },
        { IDR_WAV_WATER_DROP,       L"wav",   L"water drop.wav" },
        { IDR_WAV_ZELDA,            L"wav",   L"zelda.wav" },
        { IDR_MODEL_CTM_SAS,        L"models",L"ctm_sas.glb" },
        { IDR_MODEL_TM_PHOENIX,     L"models",L"tm_phoenix.glb" },

        
        { IDR_DEATH_AHRI2,          L"wav\\death", L"ahri2.wav" },
        { IDR_DEATH_AMOGUS,         L"wav\\death", L"amogus.wav" },
        { IDR_DEATH_BONEBREAK,      L"wav\\death", L"bonebreak.wav" },
        { IDR_DEATH_BONG,           L"wav\\death", L"death-bong.wav" },
        { IDR_DEATH_DRYBONES,       L"wav\\death", L"dry-bones-death.wav" },
        { IDR_DEATH_FORTNITE,       L"wav\\death", L"fortnite.wav" },
        { IDR_DEATH_MINECRAFT,      L"wav\\death", L"minecraft-death-sound-effect.wav" },
        { IDR_DEATH_NEGBEEPS,       L"wav\\death", L"negative_beeps-6008.wav" },
        { IDR_DEATH_SONICDED,       L"wav\\death", L"sonicded.wav" },
        { IDR_DEATH_VILLAGER,       L"wav\\death", L"villager.wav" },
    };

    static const EmbeddedFile g_GameParticleFiles[] =
    {
        { IDR_PARTICLE_SNOW,       L"particles\\rain_fx", L"snow.vpcf_c" },
        { IDR_PARTICLE_SNOW_OUTER, L"particles\\rain_fx", L"snow_outer.vpcf_c" },
        // Unique aliases prevent Source 2's resource cache from folding
        // multiple density layers back into the same loaded definition.
        { IDR_PARTICLE_SNOW,       L"particles\\rain_fx", L"snow_dense_a.vpcf_c" },
        { IDR_PARTICLE_SNOW_OUTER, L"particles\\rain_fx", L"snow_outer_dense_a.vpcf_c" },
        { IDR_PARTICLE_SNOW,       L"particles\\rain_fx", L"snow_blizzard_b.vpcf_c" },
        { IDR_PARTICLE_SNOW_OUTER, L"particles\\rain_fx", L"snow_outer_blizzard_b.vpcf_c" },
        { IDR_PARTICLE_LIGHTNING_BLUE, L"particles\\raven_fx", L"lightning_kill_blue.vpcf_c" },
        { IDR_PARTICLE_LIGHTNING_GREEN, L"particles\\raven_fx", L"lightning_kill_green.vpcf_c" },
        { IDR_PARTICLE_LIGHTNING_PURPLE, L"particles\\raven_fx", L"lightning_kill_purple.vpcf_c" },
        { IDR_PARTICLE_MOLOTOV_BLUE, L"particles\\raven_fx", L"molotov_blue.vpcf_c" },
        { IDR_PARTICLE_MOLOTOV_GREEN, L"particles\\raven_fx", L"molotov_green.vpcf_c" },
        { IDR_PARTICLE_MOLOTOV_PURPLE, L"particles\\raven_fx", L"molotov_purple.vpcf_c" },
    };

    static bool ExtractResource(HMODULE hModule, int resourceId,
                                const std::wstring& outputPath,
                                bool overwrite = false)
    {
        
        if (!overwrite && std::filesystem::exists(outputPath))
            return true;

        HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!hRes)
            return false;

        HGLOBAL hData = LoadResource(hModule, hRes);
        if (!hData)
            return false;

        void* pData = LockResource(hData);
        DWORD dataSize = SizeofResource(hModule, hRes);
        if (!pData || dataSize == 0)
            return false;

        HANDLE hFile = CreateFileW(
            outputPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        DWORD bytesWritten = 0;
        BOOL success = WriteFile(hFile, pData, dataSize, &bytesWritten, nullptr);
        CloseHandle(hFile);

        return success && (bytesWritten == dataSize);
    }

    void ExtractAll(HMODULE hModule)
    {
        std::wstring basePath = std::wstring(CS2_CSGO_PATH) + L"\\raven";

        
        CreateDirectoryW(basePath.c_str(), nullptr);

        
        std::wstring fontsDir = basePath + L"\\fonts";
        std::wstring wavDir = basePath + L"\\wav";
        std::wstring modelsDir = basePath + L"\\models";

        CreateDirectoryW(fontsDir.c_str(), nullptr);
        CreateDirectoryW(wavDir.c_str(), nullptr);
        CreateDirectoryW(modelsDir.c_str(), nullptr);

        
        std::wstring deathDir = wavDir + L"\\death";
        CreateDirectoryW(deathDir.c_str(), nullptr);

        
        for (const auto& file : g_EmbeddedFiles)
        {
            std::wstring outputPath = basePath + L"\\" + file.subFolder + L"\\" + file.fileName;
            std::wstring fname = file.fileName;
            
            if (fname.size() >= 4 && fname.substr(fname.size() - 4) == L".glb") {
                if (std::filesystem::exists(outputPath)) {
                    std::filesystem::remove(outputPath);
                }
            }
        }

        
        for (const auto& file : g_EmbeddedFiles)
        {
            std::wstring outputPath = basePath + L"\\" + file.subFolder + L"\\" + file.fileName;
            ExtractResource(hModule, file.resourceId, outputPath);
        }

        // Source 2 resolves these from the game search path, not raven/.
        // Embed and deploy them so the snow toggle is self-contained.
        for (const auto& file : g_GameParticleFiles)
        {
            const std::filesystem::path folder =
                std::filesystem::path(CS2_CSGO_PATH) / file.subFolder;
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            ExtractResource(hModule, file.resourceId,
                            (folder / file.fileName).wstring(), true);
        }
    }
}
