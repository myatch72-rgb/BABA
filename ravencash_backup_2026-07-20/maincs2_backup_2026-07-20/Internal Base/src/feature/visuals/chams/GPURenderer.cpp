#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GPURenderer.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "../../../sdk/utils/Globals.h"

#pragma comment(lib, "d3dcompiler.lib")





GPUChamsRenderer& GPUChamsRenderer::Get()
{
    static GPUChamsRenderer instance;
    return instance;
}

bool GPUChamsRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    if (m_initialized) return true;
    if (!device || !ctx) return false;

    m_device = device;
    m_context = ctx;

    
    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    cbd.ByteWidth = sizeof(CBFrame);
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_cbFrame))) return false;

    cbd.ByteWidth = sizeof(CBObject);
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_cbObject))) return false;

    
    D3D11_BUFFER_DESC bdBox = {};
    bdBox.Usage = D3D11_USAGE_DYNAMIC;
    bdBox.ByteWidth = sizeof(ChamsVertex) * 8 * MaxBoxes;
    bdBox.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bdBox.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bdBox, nullptr, &m_vbBox))) return false;

    
    std::vector<uint16_t> indices;
    indices.reserve(MaxBoxes * 36);
    for (int i = 0; i < MaxBoxes; i++) {
        uint16_t base = (uint16_t)(i * 8);
        uint16_t idx[36] = {
            0,2,1, 1,2,3, 4,5,6, 5,7,6, 0,1,4, 1,5,4,
            2,6,3, 3,6,7, 0,4,2, 2,4,6, 1,3,5, 3,7,5
        };
        for (int j = 0; j < 36; j++) idx[j] += base;
        indices.insert(indices.end(), idx, idx + 36);
    }
    D3D11_BUFFER_DESC ibd = {};
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.ByteWidth = (UINT)(sizeof(uint16_t) * indices.size());
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { indices.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&ibd, &ibData, &m_ibBox))) return false;

    
    D3D11_BUFFER_DESC bdLine = {};
    bdLine.Usage = D3D11_USAGE_DYNAMIC;
    bdLine.ByteWidth = sizeof(ChamsVertex) * MaxLines;
    bdLine.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bdLine.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bdLine, nullptr, &m_vbLine))) return false;

    
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    const char* vsCode =
        "cbuffer CBObject : register(b0) { matrix World; };"
        "cbuffer CBFrame : register(b1) { matrix View; matrix Proj; };"
        "struct VS_INPUT { float3 Pos:POSITION; float4 Color:COLOR; };"
        "struct PS_INPUT { float4 Pos:SV_POSITION; float4 Color:COLOR; };"
        "PS_INPUT main(VS_INPUT input) { PS_INPUT o; o.Pos=mul(float4(input.Pos,1),World);"
        "o.Pos=mul(o.Pos,View); o.Pos=mul(o.Pos,Proj); o.Color=input.Color; return o; }";

    if (FAILED(D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob))) {
        if (errorBlob) errorBlob->Release();
        return false;
    }
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs))) {
        vsBlob->Release();
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    if (FAILED(device->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_layout))) {
        vsBlob->Release();
        return false;
    }
    vsBlob->Release();

    const char* psCode =
        "struct PS_INPUT { float4 Pos:SV_POSITION; float4 Color:COLOR; };"
        "float4 main(PS_INPUT i):SV_TARGET { return i.Color; }";

    if (FAILED(D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob))) {
        if (errorBlob) errorBlob->Release();
        return false;
    }
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps))) {
        psBlob->Release();
        return false;
    }
    psBlob->Release();

    
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &m_rasterState))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &m_blendState))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dd, &m_depthState))) return false;

    m_initialized = true;
    return true;
}

void GPUChamsRenderer::Shutdown()
{
    if (m_vs) { m_vs->Release(); m_vs = nullptr; }
    if (m_ps) { m_ps->Release(); m_ps = nullptr; }
    if (m_layout) { m_layout->Release(); m_layout = nullptr; }
    if (m_cbFrame) { m_cbFrame->Release(); m_cbFrame = nullptr; }
    if (m_cbObject) { m_cbObject->Release(); m_cbObject = nullptr; }
    if (m_vbBox) { m_vbBox->Release(); m_vbBox = nullptr; }
    if (m_ibBox) { m_ibBox->Release(); m_ibBox = nullptr; }
    if (m_vbLine) { m_vbLine->Release(); m_vbLine = nullptr; }
    if (m_rasterState) { m_rasterState->Release(); m_rasterState = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_depthState) { m_depthState->Release(); m_depthState = nullptr; }
    m_initialized = false;
}




void GPUChamsRenderer::SaveState()
{
    m_context->IAGetPrimitiveTopology(&m_saved.topology);
    m_context->IAGetInputLayout(&m_saved.layout);
    m_context->VSGetShader(&m_saved.vs, nullptr, nullptr);
    m_context->PSGetShader(&m_saved.ps, nullptr, nullptr);
    m_context->VSGetConstantBuffers(0, 2, m_saved.vsConstBuf);
    m_context->RSGetState(&m_saved.rasterizer);
    m_context->OMGetBlendState(&m_saved.blend, m_saved.blendFactor, &m_saved.blendMask);
    m_context->OMGetDepthStencilState(&m_saved.depth, &m_saved.stencilRef);
    m_context->IAGetVertexBuffers(0, 1, &m_saved.vb, &m_saved.vbStride, &m_saved.vbOffset);
    m_context->IAGetIndexBuffer(&m_saved.ib, &m_saved.ibFormat, &m_saved.ibOffset);
    m_saved.numViewports = 1;
    m_context->RSGetViewports(&m_saved.numViewports, &m_saved.viewport);
    m_context->OMGetRenderTargets(1, &m_saved.rtv, &m_saved.dsv);
}

void GPUChamsRenderer::RestoreState()
{
    m_context->IASetPrimitiveTopology(m_saved.topology);
    m_context->IASetInputLayout(m_saved.layout);
    m_context->VSSetShader(m_saved.vs, nullptr, 0);
    m_context->PSSetShader(m_saved.ps, nullptr, 0);
    m_context->VSSetConstantBuffers(0, 2, m_saved.vsConstBuf);
    m_context->RSSetState(m_saved.rasterizer);
    m_context->OMSetBlendState(m_saved.blend, m_saved.blendFactor, m_saved.blendMask);
    m_context->OMSetDepthStencilState(m_saved.depth, m_saved.stencilRef);
    UINT stride = m_saved.vbStride, offset = m_saved.vbOffset;
    m_context->IASetVertexBuffers(0, 1, &m_saved.vb, &stride, &offset);
    m_context->IASetIndexBuffer(m_saved.ib, m_saved.ibFormat, m_saved.ibOffset);
    m_context->RSSetViewports(m_saved.numViewports, &m_saved.viewport);
    m_context->OMSetRenderTargets(1, &m_saved.rtv, m_saved.dsv);

    
    if (m_saved.layout) m_saved.layout->Release();
    if (m_saved.vs) m_saved.vs->Release();
    if (m_saved.ps) m_saved.ps->Release();
    for (auto& b : m_saved.vsConstBuf) if (b) b->Release();
    if (m_saved.rasterizer) m_saved.rasterizer->Release();
    if (m_saved.blend) m_saved.blend->Release();
    if (m_saved.depth) m_saved.depth->Release();
    if (m_saved.vb) m_saved.vb->Release();
    if (m_saved.ib) m_saved.ib->Release();
    if (m_saved.rtv) m_saved.rtv->Release();
    if (m_saved.dsv) m_saved.dsv->Release();
    memset(&m_saved, 0, sizeof(m_saved));
}




void GPUChamsRenderer::BeginFrame(float screenW, float screenH)
{
    m_boxVertices.clear();
    m_lineVertices.clear();
    m_boxVertices.reserve(MaxBoxes * 8);
    m_lineVertices.reserve(MaxLines);
    m_width = screenW;
    m_height = screenH;
}

void GPUChamsRenderer::SetViewMatrix(const float gameViewMatrix[16])
{
    
    
    
    
    
    
    XMMATRIX viewProj = XMMATRIX(
        gameViewMatrix[0], gameViewMatrix[4], gameViewMatrix[8],  gameViewMatrix[12],
        gameViewMatrix[1], gameViewMatrix[5], gameViewMatrix[9],  gameViewMatrix[13],
        gameViewMatrix[2], gameViewMatrix[6], gameViewMatrix[10], gameViewMatrix[14],
        gameViewMatrix[3], gameViewMatrix[7], gameViewMatrix[11], gameViewMatrix[15]
    );

    
    XMMATRIX transposedView = XMMatrixTranspose(viewProj);
    XMMATRIX transposedProj = XMMatrixTranspose(XMMatrixIdentity());

    CBFrame cbf;
    cbf.view = transposedView;
    cbf.proj = transposedProj;
    m_context->UpdateSubresource(m_cbFrame, 0, nullptr, &cbf, 0, 0);
}




void GPUChamsRenderer::DrawBox(Vector pos, Vector size, float color[4])
{
    if ((int)(m_boxVertices.size() / 8) >= MaxBoxes) return;

    XMFLOAT4 col = { color[0], color[1], color[2], color[3] };
    XMMATRIX world = XMMatrixTranslation(pos.x, pos.y, pos.z);

    for (int i = 0; i < 8; i++) {
        XMFLOAT3 v = {
            (i & 1 ? -0.5f : 0.5f) * size.x,
            (i & 2 ? -0.5f : 0.5f) * size.y,
            (i & 4 ? -0.5f : 0.5f) * size.z
        };
        XMVECTOR vec = XMVector3Transform(XMLoadFloat3(&v), world);
        XMFLOAT3 out;
        XMStoreFloat3(&out, vec);
        m_boxVertices.push_back({ out, col });
    }
}

void GPUChamsRenderer::DrawWireBox(Vector pos, Vector size, float color[4])
{
    if ((int)m_lineVertices.size() + 24 >= MaxLines) return;

    XMFLOAT4 col = { color[0], color[1], color[2], color[3] };
    XMMATRIX world = XMMatrixTranslation(pos.x, pos.y, pos.z);
    XMFLOAT3 corners[8];

    for (int i = 0; i < 8; i++) {
        XMFLOAT3 v = {
            (i & 1 ? -0.5f : 0.5f) * size.x,
            (i & 2 ? -0.5f : 0.5f) * size.y,
            (i & 4 ? -0.5f : 0.5f) * size.z
        };
        XMVECTOR vec = XMVector3Transform(XMLoadFloat3(&v), world);
        XMStoreFloat3(&corners[i], vec);
    }

    const int edges[12][2] = {
        {0,1}, {1,3}, {3,2}, {2,0},  
        {4,5}, {5,7}, {7,6}, {6,4},  
        {0,4}, {1,5}, {2,6}, {3,7}   
    };

    for (int i = 0; i < 12; i++) {
        m_lineVertices.push_back({ corners[edges[i][0]], col });
        m_lineVertices.push_back({ corners[edges[i][1]], col });
    }
}




void GPUChamsRenderer::EndFrame(ID3D11RenderTargetView* rtv)
{
    if (!m_initialized) return;
    if (m_boxVertices.empty() && m_lineVertices.empty()) return;

    SaveState();

    
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = m_width;
    vp.Height = m_height;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    vp.TopLeftX = Globals::GameViewportX;
    vp.TopLeftY = Globals::GameViewportY;
    m_context->RSSetViewports(1, &vp);

    
    m_context->RSSetState(m_rasterState);
    m_context->OMSetBlendState(m_blendState, nullptr, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_depthState, 0);

    
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(m_ps, nullptr, 0);
    m_context->IASetInputLayout(m_layout);

    
    CBObject obj;
    obj.world = XMMatrixTranspose(XMMatrixIdentity());
    m_context->UpdateSubresource(m_cbObject, 0, nullptr, &obj, 0, 0);
    m_context->VSSetConstantBuffers(0, 1, &m_cbObject);
    m_context->VSSetConstantBuffers(1, 1, &m_cbFrame);

    
    UINT numBoxes = (UINT)(m_boxVertices.size() / 8);
    if (numBoxes > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_context->Map(m_vbBox, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, m_boxVertices.data(), m_boxVertices.size() * sizeof(ChamsVertex));
            m_context->Unmap(m_vbBox, 0);
        }

        UINT stride = sizeof(ChamsVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, &m_vbBox, &stride, &offset);
        m_context->IASetIndexBuffer(m_ibBox, DXGI_FORMAT_R16_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->DrawIndexed(numBoxes * 36, 0, 0);
    }

    
    UINT numLineVerts = (UINT)m_lineVertices.size();
    if (numLineVerts > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_context->Map(m_vbLine, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, m_lineVertices.data(), m_lineVertices.size() * sizeof(ChamsVertex));
            m_context->Unmap(m_vbLine, 0);
        }

        UINT stride = sizeof(ChamsVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, &m_vbLine, &stride, &offset);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        m_context->Draw(numLineVerts, 0);
    }

    RestoreState();
}
