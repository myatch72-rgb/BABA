#pragma once

struct ID3D11Device;

namespace Menu {
    inline bool IsOpen = true;
    void Initialize(ID3D11Device* device);
    void Render();
}