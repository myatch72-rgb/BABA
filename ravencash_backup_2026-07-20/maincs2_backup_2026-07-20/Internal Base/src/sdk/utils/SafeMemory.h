#pragma once
#include <Windows.h>
#include <cstdint>

namespace SafeMemory
{
    inline bool IsValidPtr(uintptr_t addr)
    {
        // CS2 is x64 and all live modules, heaps, entity chunks and manual-map
        // allocations used by this project reside above the 32-bit address
        // range. Entity chunks can contain transient poison/handle values such
        // as 0x47474700 while a map is being assembled; the former 0x10000
        // lower bound accepted those values and caused a first-chance AV before
        // SEH could fail the read closed.
        constexpr uintptr_t kLowestProcessPointer = 0x0000000100000000ULL;
        constexpr uintptr_t kHighestUserPointer = 0x00007FFFFFFFFFFFULL;
        return addr >= kLowestProcessPointer && addr < kHighestUserPointer;
    }

    template <typename T>
    inline T Read(uintptr_t addr, T defaultVal = T())
    {
        if (!IsValidPtr(addr))
            return defaultVal;

        __try
        {
            return *reinterpret_cast<T*>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return defaultVal;
        }
    }

    template <typename T>
    inline bool Write(uintptr_t addr, T val)
    {
        if (!IsValidPtr(addr))
            return false;

        __try
        {
            *reinterpret_cast<T*>(addr) = val;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    inline bool WriteProtected(uintptr_t addr, T val)
    {
        if (!IsValidPtr(addr))
            return false;

        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            __try
            {
                *reinterpret_cast<T*>(addr) = val;
                VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), oldProtect, &oldProtect);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), oldProtect, &oldProtect);
                return false;
            }
        }
        return false;
    }
}
