#pragma once
#include <cstdint>

class i_network_game_client
{
public:
    bool should_predict( )
    {
        return *reinterpret_cast< bool* >( reinterpret_cast< uintptr_t >( this ) + 0x100 );
    }
 
    int delta_tick( )
    {
        return *reinterpret_cast< int* >( reinterpret_cast< uintptr_t >( this ) + 0x24C );
    }
 
    int server_tick( )
    {
        return *reinterpret_cast< int* >( reinterpret_cast< uintptr_t >( this ) + 0x37C );
    }
 
    bool& in_prediction( )
    {
        return *reinterpret_cast< bool* >( reinterpret_cast< uintptr_t >( this ) + 0xD0 );
    }
};
 
class i_network_client_service
{
public:
    i_network_game_client* network_game_client( )
    {
        return *( i_network_game_client** ) ( ( uintptr_t ) this + 0xA0 );
    }
};
