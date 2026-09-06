#pragma once
#include <cstdint>
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/memory/Patterns.h"






namespace BhopV2Patterns
{
    inline const char* create_move        = Patterns::Client::CreateMove;
    constexpr auto get_command_context = "41 8B CC E8 ? ? ? ? 48 89 44 24 ? 48 8B F8 48 85 C0";
    constexpr auto get_entity_index   = "E8 ? ? ? ? 8B 8D ? ? ? ? 8D 51";
    constexpr auto construct_input    = "E8 ? ? ? ? 48 8B CF 4C 8B F8 44 8B B0 10 59 00 00";
    constexpr auto poo                = "48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CF 4C 8B F8 44 8B B0 10 59 00 00";
    constexpr auto automake_user_cmd  = "E8 ? ? ? ? 48 89 44 24 40 48 8D 4D F0";
}




namespace BhopV2
{
    inline uintptr_t Resolve(uintptr_t addr, int start, int end)
    {
        if (!addr) return 0;
        return addr + end + *reinterpret_cast<int32_t*>(addr + start);
    }
}


class c_user_cmd
{
public:
    void*    vfptr;              
    int      command_number;    
    char     pad_0008[0x50];    
    uint64_t m_button_state;    
    uint64_t m_button_state2;   
    uint64_t m_button_state3;   
    char     subtick_data[0x18];
    int      has_been_predicted;
    int      prediction_cmd_type;
    int      cmd_type;          
    int      cmd_flag;          
};


class i_csgo_input
{
public:
    inline c_user_cmd* get_user_cmd()
    {
        using get_command_context_t = void*(__fastcall*)(int);
        static get_command_context_t s_get_command_context =
            reinterpret_cast<get_command_context_t>(
                BhopV2::Resolve(
                    Memory::PatternScan("client.dll", BhopV2Patterns::get_command_context),
                    4, 8));

        
        using get_entity_index_t = void*(__fastcall*)(void*, int*);
        static get_entity_index_t s_get_entity_index =
            reinterpret_cast<get_entity_index_t>(
                BhopV2::Resolve(
                    Memory::PatternScan("client.dll", BhopV2Patterns::get_entity_index),
                    1, 5));

        static auto s_construct_input =
            reinterpret_cast<void*(__fastcall*)(void*, int)>(
                BhopV2::Resolve(
                    Memory::PatternScan("client.dll", BhopV2Patterns::construct_input),
                    1, 5));

        static uintptr_t s_poo_raw =
            BhopV2::Resolve(
                Memory::PatternScan("client.dll", BhopV2Patterns::poo),
                3, 7);

        static void* s_poo_ptr =
            s_poo_raw ? *reinterpret_cast<void**>(s_poo_raw) : nullptr;

        static auto s_automake_user_cmd =
            reinterpret_cast<c_user_cmd*(__fastcall*)(void*, int)>(
                BhopV2::Resolve(
                    Memory::PatternScan("client.dll", BhopV2Patterns::automake_user_cmd),
                    1, 5));

        
        if (!s_get_command_context || !s_get_entity_index || !s_construct_input ||
            !s_poo_ptr           || !s_automake_user_cmd)
            return nullptr;

        void* command_context = s_get_command_context(0);
        if (!command_context)
            return nullptr;

        
        int player_idx = 0;
        s_get_entity_index(command_context, &player_idx);

        int entity_index = (player_idx == -1) ? -1 : (player_idx - 1);

        
        void* array_inputs = s_construct_input(s_poo_ptr, entity_index);
        if (!array_inputs)
            return nullptr;

        int sequence_number =
            *reinterpret_cast<int*>(
                reinterpret_cast<uintptr_t>(array_inputs) + 0x5910);

        
        return s_automake_user_cmd(command_context, sequence_number);
    }
};
