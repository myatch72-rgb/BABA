#include "PatternScan.h"
#include <Windows.h>
#include <Psapi.h>
#include <vector>

uintptr_t Memory::GetModuleBase(const char* module)
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(module));
}

static std::vector<int> PatternToBytes(const char* pattern)
{
    std::vector<int> bytes;

    for (const char* c = pattern; *c; )
    {
        
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')
        {
            ++c;
            continue;
        }

        if (*c == '?')
        {
            ++c;
            if (*c == '?') ++c;
            bytes.push_back(-1);
            continue;
        }

        char* end = nullptr;
        unsigned long value = strtoul(c, &end, 16);
        if (end == c)
        {
            
            ++c;
            continue;
        }

        bytes.push_back(static_cast<int>(value & 0xFF));
        c = end;
    }

    return bytes;
}

uintptr_t Memory::PatternScan(const char* module, const char* signature)
{
    HMODULE mod = GetModuleHandleA(module);
    if (!mod) return 0;

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info)))
        return 0;

    auto base = reinterpret_cast<uint8_t*>(mod);
    auto size = info.SizeOfImage;

    auto pattern = PatternToBytes(signature);
    auto data = pattern.data();
    auto len = pattern.size();

    if (len == 0 || size < len)
        return 0;

    for (size_t i = 0; i <= size - len; ++i)
    {
        bool found = true;
        for (size_t j = 0; j < len; ++j)
        {
            if (data[j] != -1 && data[j] != base[i + j])
            {
                found = false;
                break;
            }
        }
        if (found)
            return reinterpret_cast<uintptr_t>(&base[i]);
    }

    return 0;
}

uintptr_t Memory::ResolveRipRelative(const char* module, const char* signature,
                                     int dispOffset, int instrLen)
{
    uintptr_t hit = PatternScan(module, signature);
    if (!hit) return 0;

    __try
    {
        int32_t disp = *reinterpret_cast<int32_t*>(hit + dispOffset);
        return hit + instrLen + static_cast<intptr_t>(disp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}