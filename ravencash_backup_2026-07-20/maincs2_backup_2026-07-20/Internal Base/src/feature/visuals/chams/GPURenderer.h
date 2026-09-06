#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include "../../../sdk/utils/Vector.h"

using namespace DirectX;




struct ChamsVertex {
    XMFLOAT3 pos;
    XMFLOAT4 color;
};




struct CBFrame { XMMATRIX view; XMMATRIX proj; };
struct CBObject { XMMATRIX world; };





class GPUChamsRenderer
{
private:
    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    
    ID3D11VertexShader*  m_vs = nullptr;
    ID3D11PixelShader*   m_ps = nullptr;
    ID3D11InputLayout*   m_layout = nullptr;

    
    ID3D11Buffer* m_cbFrame = nullptr;
    ID3D11Buffer* m_cbObject = nullptr;

    
    ID3D11Buffer* m_vbBox = nullptr;    
    ID3D11Buffer* m_ibBox = nullptr;    
    ID3D11Buffer* m_vbLine = nullptr;   

    
    ID3D11RasterizerState*   m_rasterState = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11DepthStencilState* m_depthState = nullptr;

    
    std::vector<ChamsVertex> m_boxVertices;
    std::vector<ChamsVertex> m_lineVertices;

    bool m_initialized = false;
    float m_width = 0, m_height = 0;

    static constexpr int MaxBoxes = 512;
    static constexpr int MaxLines = 8192;

    
    struct SavedState {
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11InputLayout* layout;
        ID3D11VertexShader* vs;
        ID3D11PixelShader* ps;
        ID3D11Buffer* vsConstBuf[2];
        ID3D11RasterizerState* rasterizer;
        ID3D11BlendState* blend;
        float blendFactor[4];
        UINT blendMask;
        ID3D11DepthStencilState* depth;
        UINT stencilRef;
        ID3D11Buffer* vb;
        UINT vbStride, vbOffset;
        ID3D11Buffer* ib;
        DXGI_FORMAT ibFormat;
        UINT ibOffset;
        ID3D11RenderTargetView* rtv;
        ID3D11DepthStencilView* dsv;
        D3D11_VIEWPORT viewport;
        UINT numViewports;
    } m_saved{};

    void SaveState();
    void RestoreState();

    GPUChamsRenderer() = default;

public:
    static GPUChamsRenderer& Get();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Shutdown();

    
    void BeginFrame(float screenW, float screenH);

    
    void SetViewMatrix(const float gameViewMatrix[16]);

    
    void DrawBox(Vector pos, Vector size, float color[4]);
    void DrawWireBox(Vector pos, Vector size, float color[4]);

    
    void EndFrame(ID3D11RenderTargetView* rtv);

    bool IsInitialized() const { return m_initialized; }
};
