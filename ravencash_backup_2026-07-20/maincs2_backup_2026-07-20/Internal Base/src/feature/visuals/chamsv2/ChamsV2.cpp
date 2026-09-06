#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ChamsV2.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/entity/EntityManager.h"


#include "../../../../ext/nlohmann/json.hpp"
using json = nlohmann::json;

#include <algorithm>




namespace ChamsV2
{
    RenderStateGuard::RenderStateGuard(ID3D11DeviceContext* ctx)
        : m_ctx(ctx)
    {
        ctx->IAGetPrimitiveTopology(&m_topology);
        ctx->IAGetInputLayout(&m_input_layout);
        ctx->VSGetShader(&m_vs, nullptr, nullptr);
        ctx->PSGetShader(&m_ps, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 4, m_vs_cbs);
        ctx->PSGetConstantBuffers(0, 2, m_ps_cbs);

        ctx->IAGetVertexBuffers(0, 1, &m_vb, &m_vb_stride, &m_vb_offset);
        ctx->IAGetIndexBuffer(&m_ib, &m_ib_format, &m_ib_offset);

        ctx->RSGetState(&m_rs);
        m_num_viewports = 1;
        ctx->RSGetViewports(&m_num_viewports, &m_viewport);
        ctx->OMGetBlendState(&m_bs, m_blend_factor, &m_sample_mask);
        ctx->OMGetDepthStencilState(&m_dss, &m_stencil_ref);
        ctx->OMGetRenderTargets(1, &m_rtv, &m_dsv);
    }

    RenderStateGuard::~RenderStateGuard() {
        m_ctx->IASetPrimitiveTopology(m_topology);
        m_ctx->IASetInputLayout(m_input_layout);
        m_ctx->VSSetShader(m_vs, nullptr, 0);
        m_ctx->PSSetShader(m_ps, nullptr, 0);
        m_ctx->VSSetConstantBuffers(0, 4, m_vs_cbs);
        m_ctx->PSSetConstantBuffers(0, 2, m_ps_cbs);

        m_ctx->IASetVertexBuffers(0, 1, &m_vb, &m_vb_stride, &m_vb_offset);
        m_ctx->IASetIndexBuffer(m_ib, m_ib_format, m_ib_offset);

        m_ctx->RSSetState(m_rs);
        m_ctx->RSSetViewports(m_num_viewports, &m_viewport);
        m_ctx->OMSetBlendState(m_bs, m_blend_factor, m_sample_mask);
        m_ctx->OMSetDepthStencilState(m_dss, m_stencil_ref);
        m_ctx->OMSetRenderTargets(1, &m_rtv, m_dsv);

        if (m_input_layout) m_input_layout->Release();
        if (m_vs) m_vs->Release();
        if (m_ps) m_ps->Release();
        for (auto& cb : m_vs_cbs) if (cb) cb->Release();
        for (auto& cb : m_ps_cbs) if (cb) cb->Release();
        if (m_vb) m_vb->Release();
        if (m_ib) m_ib->Release();
        if (m_rs) m_rs->Release();
        if (m_bs) m_bs->Release();
        if (m_dss) m_dss->Release();
        if (m_rtv) m_rtv->Release();
        if (m_dsv) m_dsv->Release();
    }
}

static Vector GetLocalEyePositionV2()
{
    auto localPawn = EntityManager::Get().GetLocalPawn();
    if (!localPawn) return {};

    uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(localPawn);
    uintptr_t scene = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
    if (!Utils::IsValidPtr(scene)) return {};

    Vector origin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
    Vector viewOffset = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecViewOffset);
    return origin + viewOffset;
}

static bool IsPointInViewFrustumV2(const Vector& point, float ndc_margin)
{
    const float* m = Globals::ViewMatrix;

    const float clip_x = m[0] * point.x + m[1] * point.y + m[2] * point.z + m[3];
    const float clip_y = m[4] * point.x + m[5] * point.y + m[6] * point.z + m[7];
    const float clip_w = m[12] * point.x + m[13] * point.y + m[14] * point.z + m[15];

    if (clip_w <= 0.01f)
        return false;

    const float inv_w = 1.0f / clip_w;
    const float ndc_x = clip_x * inv_w;
    const float ndc_y = clip_y * inv_w;
    const float bound = 1.0f + ndc_margin;

    return ndc_x >= -bound && ndc_x <= bound && ndc_y >= -bound && ndc_y <= bound;
}

static bool IsPlayerInViewFrustumV2(const ChamsV2::BoneMatrix3x4* game_bones, int bone_count)
{
    if (!game_bones || bone_count <= 0)
        return false;

    const float ndc_margin = 0.10f;
    const int sample_step = (bone_count > 20) ? 2 : 1;

    for (int i = 0; i < bone_count; i += sample_step)
    {
        const Vector bone_pos = game_bones[i].GetOrigin();
        if (!std::isfinite(bone_pos.x) || !std::isfinite(bone_pos.y) || !std::isfinite(bone_pos.z))
            continue;

        if (IsPointInViewFrustumV2(bone_pos, ndc_margin))
            return true;
    }

    return false;
}


static void GetPerBoneVisibility(C_CSPlayerPawn* target, const ChamsV2::BoneMatrix3x4* game_bones, int bone_count, float* out_visibility)
{
    if (!target || bone_count <= 0) {
        memset(out_visibility, 0, sizeof(float) * ChamsV2::MAX_BONES);
        return;
    }
    
    if (!Raycasting::Get().IsLoaded()) {
        
        for (int i = 0; i < bone_count; i++) out_visibility[i] = 1.0f;
        for (int i = bone_count; i < ChamsV2::MAX_BONES; i++) out_visibility[i] = 0.0f;
        return;
    }

    Vector eyePos = GetLocalEyePositionV2();
    if (eyePos.IsZero()) {
        for (int i = 0; i < bone_count; i++) out_visibility[i] = 1.0f;
        for (int i = bone_count; i < ChamsV2::MAX_BONES; i++) out_visibility[i] = 0.0f;
        return;
    }

    
    for (int i = 0; i < bone_count; i++) {
        Vector bonePos = game_bones[i].GetOrigin();
        
        if (bonePos.IsZero() || std::isnan(bonePos.x) || std::isnan(bonePos.y) || std::isnan(bonePos.z)) {
            out_visibility[i] = 0.0f;
            continue;
        }
        
        out_visibility[i] = Raycasting::Get().IsVisible(eyePos, bonePos) ? 1.0f : 0.0f;
    }
    
    for (int i = bone_count; i < ChamsV2::MAX_BONES; i++) out_visibility[i] = 0.0f;
}

static float GetVisibilityRatioV2(C_CSPlayerPawn* target)
{
    if (!target)
        return 0.0f;

    if (!Raycasting::Get().IsLoaded())
        return 1.0f;

    Vector eyePos = GetLocalEyePositionV2();
    if (eyePos.IsZero())
        return 1.0f;

    static const BoneID sampleBones[] = {
        BoneID::Head, BoneID::Neck, BoneID::Spine, BoneID::Pelvis,
        BoneID::LeftShoulder, BoneID::LeftArm, BoneID::LeftHand,
        BoneID::RightShoulder, BoneID::RightArm, BoneID::RightHand,
        BoneID::LeftHip, BoneID::LeftKnee, BoneID::LeftFoot,
        BoneID::RightHip, BoneID::RightKnee, BoneID::RightFoot
    };

    int validCount = 0;
    int visibleCount = 0;
    for (BoneID bone : sampleBones)
    {
        Vector bonePos = Utils::GetBonePos(target, bone);
        if (bonePos.IsZero())
            continue;

        ++validCount;
        if (Raycasting::Get().IsVisible(eyePos, bonePos))
            ++visibleCount;
    }

    if (validCount == 0)
        return 0.0f;

    return static_cast<float>(visibleCount) / static_cast<float>(validCount);
}




namespace ChamsV2
{
    static const char* g_ShaderSource = R"(
        cbuffer CBViewProjection : register(b0) {
            row_major float4x4 g_ViewProjection;
            float g_ScreenW;
            float g_ScreenH;
            float2 _pad0;
        };

        cbuffer CBBoneMatrices : register(b1) {
            row_major float4x4 g_BoneMatrices[128];
        };

        cbuffer CBMaterial : register(b2) {
            float4 g_FillColor;      // Non-visible fill color
            float4 g_FillColorVis;   // Visible fill color
            float4 g_WireColor;      // Non-visible wire color
            float4 g_WireColorVis;   // Visible wire color
            int    g_RenderMode;
            float  g_Alpha;
            int    g_MaterialType;
            float  g_RimPower;
            float3 g_CamPos;
            float  g_Time;
            float  g_DissolveAmount;
            int    g_DissolveStyle;
            float  g_EdgeWidth;
            float  g_NoiseScale;
            float3 g_EdgeColor;
            float  g_GlowStrength;
        };

        cbuffer CBBoneVisibility : register(b3) {
            float4 g_BoneVisibility[32]; // 128 floats packed as 32 float4s
        };

        struct VS_INPUT {
            float3 Position    : POSITION;
            float3 Normal      : NORMAL;
            uint4  BoneIndices : BLENDINDICES;
            float4 BoneWeights : BLENDWEIGHT;
        };

        struct PS_INPUT {
            float4 Position   : SV_POSITION;
            float3 WorldPos   : TEXCOORD0;
            float3 WorldNormal: TEXCOORD1;
            float  Visibility : TEXCOORD2;  // Per-vertex visibility
        };

        // Helper to get bone visibility from packed array
        float GetBoneVis(uint bone_idx) {
            uint vec_idx = bone_idx / 4;
            uint comp_idx = bone_idx % 4;
            return g_BoneVisibility[vec_idx][comp_idx];
        }

        PS_INPUT VS_Skinning(VS_INPUT input) {
            PS_INPUT output = (PS_INPUT)0;

            float4 skinned_pos = float4(0, 0, 0, 0);
            float3 skinned_normal = float3(0, 0, 0);
            float vertex_visibility = 0.0f;

            [unroll]
            for (int i = 0; i < 4; i++) {
                float weight = input.BoneWeights[i];
                if (weight > 0.0001f) {
                    uint bone_idx = input.BoneIndices[i];
                    float4 bone_pos = mul(g_BoneMatrices[bone_idx], float4(input.Position, 1.0f));
                    skinned_pos += bone_pos * weight;

                    float3 bone_normal = mul((float3x3)g_BoneMatrices[bone_idx], input.Normal);
                    skinned_normal += bone_normal * weight;

                    // Accumulate per-bone visibility weighted by bone weight
                    vertex_visibility += GetBoneVis(bone_idx) * weight;
                }
            }

            output.WorldPos = skinned_pos.xyz;
            output.WorldNormal = normalize(skinned_normal);
            output.Visibility = saturate(vertex_visibility);

            float4 clip;
            clip.x = dot(g_ViewProjection[0], skinned_pos);
            clip.y = dot(g_ViewProjection[1], skinned_pos);
            clip.z = dot(g_ViewProjection[2], skinned_pos);
            clip.w = dot(g_ViewProjection[3], skinned_pos);

            if (clip.w < 0.01f) {
                output.Position = float4(0, 0, -1, 1);
                return output;
            }

            float inv_w = 1.0f / clip.w;
            float ndc_x = clip.x * inv_w;
            float ndc_y = clip.y * inv_w;

            float screen_x = g_ScreenW * 0.5f + ndc_x * g_ScreenW * 0.5f + 0.5f;
            float screen_y = g_ScreenH * 0.5f - ndc_y * g_ScreenH * 0.5f + 0.5f;

            output.Position.x = (screen_x / g_ScreenW) * 2.0f - 1.0f;
            output.Position.y = -((screen_y / g_ScreenH) * 2.0f - 1.0f);
            output.Position.z = 0.5f;
            output.Position.w = 1.0f;

            return output;
        }

        float4 PS_Fill(PS_INPUT input) : SV_TARGET {
            float3 N = normalize(input.WorldNormal);
            float3 V = normalize(g_CamPos - input.WorldPos);
            
            // Interpolate colors based on per-vertex visibility
            float vis = input.Visibility;
            float3 base_color = lerp(g_FillColor.rgb, g_FillColorVis.rgb, vis);
            float base_alpha = lerp(g_FillColor.a, g_FillColorVis.a, vis) * g_Alpha;
            float3 accent_color = lerp(g_WireColor.rgb, g_WireColorVis.rgb, vis);

            float4 result = float4(base_color, base_alpha);

            // 0: Flat / Solid  (result already set)
            // 1: Rim Light / Fresnel
            if (g_MaterialType == 1) {
                float rim = 1.0 - saturate(dot(N, V));
                rim = pow(rim, g_RimPower);
                float3 color = lerp(base_color, accent_color, rim);
                result = float4(color, base_alpha + rim * 0.3);
            }
            // 2: Gradient (height-based animated)
            else if (g_MaterialType == 2) {
                float height = input.WorldPos.z;
                float t = frac(height * 0.02 + g_Time * 0.3);
                float3 color = lerp(base_color, accent_color, t);
                result = float4(color, base_alpha);
            }
            // 3: Pulse (breathing effect)
            else if (g_MaterialType == 3) {
                float pulse = sin(g_Time * 3.0) * 0.5 + 0.5;
                float3 color = base_color * (0.3 + pulse * 0.7);
                float rim = 1.0 - saturate(dot(N, V));
                color += accent_color * rim * pulse * 0.5;
                result = float4(color, base_alpha * (0.5 + pulse * 0.5));
            }
            // 4: Stars
            else if (g_MaterialType == 4) {
                float2 uv = input.WorldPos.xy * 0.15;
                float2 grid = floor(uv);
                float2 f = frac(uv);
                float rnd = frac(sin(dot(grid, float2(12.9898, 78.233))) * 43758.5453);
                float rnd2 = frac(sin(dot(grid, float2(93.989, 67.345))) * 28461.6432);
                float2 star_pos = float2(rnd, rnd2);
                float dist = length(f - star_pos);
                float twinkle = sin(g_Time * (2.0 + rnd * 4.0) + rnd * 6.28) * 0.5 + 0.5;
                float star = smoothstep(0.18, 0.0, dist) * twinkle;
                float3 star_color = float3(0.9 + rnd * 0.1, 0.85 + rnd2 * 0.15, 1.0);
                float3 color = base_color * 0.4 + star * star_color;
                result = float4(color, base_alpha);
            }
            // 5: Holographic / Rainbow
            else if (g_MaterialType == 5) {
                float rim = 1.0 - saturate(dot(N, V));
                float hue = frac(rim * 2.0 + g_Time * 0.5 + input.WorldPos.z * 0.01);
                float r = abs(hue * 6.0 - 3.0) - 1.0;
                float g_val = 2.0 - abs(hue * 6.0 - 2.0);
                float b = 2.0 - abs(hue * 6.0 - 4.0);
                float3 rgb = saturate(float3(r, g_val, b));
                float3 color = lerp(base_color * 0.3, rgb, rim * 0.8 + 0.2);
                result = float4(color, base_alpha);
            }
            // 6: Crystal / Faceted
            else if (g_MaterialType == 6) {
                float3 R = reflect(-V, N);
                float facet = abs(dot(N, float3(0.577, 0.577, 0.577)));
                float3 color = base_color * (0.3 + facet * 0.7);
                float spec = pow(saturate(dot(R, float3(0, 0, 1))), 32.0);
                color += spec * float3(1, 1, 1) * 0.6;
                float edge = 1.0 - saturate(dot(N, V));
                edge = pow(edge, 3.0);
                color += edge * accent_color * 0.4;
                result = float4(color, base_alpha);
            }
            // 7: Textured (view-space lighting + specular + rim)
            else if (g_MaterialType == 7) {
                float3 lightMain = normalize(float3(0.3, 0.8, 0.6));
                float3 lightFill = normalize(float3(-0.2, -0.5, 0.4));
                float  diffMain  = max(0.0, dot(N, lightMain));
                float  diffFill  = max(0.0, dot(N, lightFill)) * 0.35;
                float  ambient   = 0.35;
                float3 halfVec   = normalize(lightMain + V);
                float  spec      = pow(saturate(dot(N, halfVec)), 24.0) * 0.4;
                float  lighting  = ambient + diffMain + diffFill + spec;
                float  rim       = pow(1.0 - saturate(dot(N, V)), 2.0);
                float3 rimColor  = base_color * 1.4;
                float3 litColor  = base_color * lighting;
                float3 texResult = lerp(litColor, rimColor, rim * 0.4);
                result = float4(saturate(texResult), base_alpha);
            }
            // 8: Metallic (reflection + fresnel + specular)
            else if (g_MaterialType == 8) {
                float  absVD     = saturate(abs(dot(N, V)));
                float3 darkColor = base_color * 0.3;
                float  reflection= 0.5 * N.y + 0.5;
                float  fresnel   = pow(1.0 - absVD, 2.0);
                float  specular  = pow(absVD, 15.0);
                float3 metalColor= lerp(darkColor, base_color, reflection);
                metalColor += float3(1.0, 1.0, 1.0) * specular * 1.5;
                metalColor += base_color * fresnel * 0.8;
                result = float4(saturate(metalColor), base_alpha);
            }
            // 9: Glow Blend (fresnel-based Color <-> Accent blend)
            else if (g_MaterialType == 9) {
                float  absVD        = saturate(abs(dot(N, V)));
                float  fresnel      = pow(1.0 - absVD, 2.0);
                float3 blendedColor = lerp(base_color, accent_color, fresnel);
                float  blendedAlpha = lerp(base_alpha, 1.0, fresnel);
                result = float4(blendedColor * (1.0 + fresnel * 1.5), blendedAlpha);
            }
            // 10: CS2 Glow (multi-layer bloom effect)
            else if (g_MaterialType == 10) {
                float  absVD     = saturate(abs(dot(N, V)));
                float  aaWidth   = max(0.015, fwidth(absVD) * 4.0);
                float  edgeAA    = smoothstep(0.0, aaWidth, absVD);
                float  silAA     = 0.35 + 0.65 * edgeAA;
                float  rimGlow   = pow(1.0 - absVD, 2.5);
                float  rimSharp  = pow(1.0 - absVD, 6.0);
                float  rimSoft   = pow(1.0 - absVD, 1.2);
                float  core      = saturate(rimSharp * 4.0);
                float  mid       = saturate(rimGlow * 2.2);
                float  outer     = saturate(rimSoft * 0.55);
                float  bloom     = core * 0.85 + mid * 0.55 + outer * 0.25;
                float  alpha     = saturate(bloom) * base_alpha * silAA;
                float  intensity = g_GlowStrength > 0.01 ? g_GlowStrength : 2.0;
                float3 glowRGB   = accent_color * (1.0 + core * intensity * 2.5 + mid * intensity);
                result = float4(glowRGB, alpha);
            }

            // ---- DISSOLVE EFFECT ----
            if (g_DissolveAmount > 0.001) {
                // 3D noise function (hash-based value noise)
                float3 nc = input.WorldPos * g_NoiseScale;
                float3 ni = floor(nc);
                float3 nf = frac(nc);
                nf = nf * nf * (3.0 - 2.0 * nf);
                // Hash helper
                #define H31(p) frac(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453)
                float n000 = H31(ni);
                float n100 = H31(ni + float3(1,0,0));
                float n010 = H31(ni + float3(0,1,0));
                float n110 = H31(ni + float3(1,1,0));
                float n001 = H31(ni + float3(0,0,1));
                float n101 = H31(ni + float3(1,0,1));
                float n011 = H31(ni + float3(0,1,1));
                float n111 = H31(ni + float3(1,1,1));
                float noiseVal = lerp(
                    lerp(lerp(n000, n100, nf.x), lerp(n010, n110, nf.x), nf.y),
                    lerp(lerp(n001, n101, nf.x), lerp(n011, n111, nf.x), nf.y),
                    nf.z);
                #undef H31

                float dThresh = g_DissolveAmount;

                // Style 0: Vaporize (upward gradient bias)
                if (g_DissolveStyle == 0) {
                    float heightBias = saturate(input.WorldPos.z * 0.008 + 0.3);
                    dThresh = g_DissolveAmount * 1.3 - heightBias * 0.35;
                }
                // Style 1: Energy Disintegration (electric flicker)
                else if (g_DissolveStyle == 1) {
                    float flicker = sin(g_Time * 35.0 + input.WorldPos.x * 7.0) * 0.12;
                    noiseVal += flicker;
                }
                // Style 2: Dust Collapse (multi-octave noise)
                else if (g_DissolveStyle == 2) {
                    float3 nc2 = input.WorldPos * g_NoiseScale * 0.4;
                    float3 ni2 = floor(nc2); float3 nf2 = frac(nc2);
                    nf2 = nf2 * nf2 * (3.0 - 2.0 * nf2);
                    #define H31b(p) frac(sin(dot(p, float3(269.5, 183.3, 421.1))) * 29471.7)
                    float lo = lerp(
                        lerp(lerp(H31b(ni2), H31b(ni2+float3(1,0,0)), nf2.x), lerp(H31b(ni2+float3(0,1,0)), H31b(ni2+float3(1,1,0)), nf2.x), nf2.y),
                        lerp(lerp(H31b(ni2+float3(0,0,1)), H31b(ni2+float3(1,0,1)), nf2.x), lerp(H31b(ni2+float3(0,1,1)), H31b(ni2+float3(1,1,1)), nf2.x), nf2.y),
                        nf2.z);
                    #undef H31b
                    noiseVal = noiseVal * 0.6 + lo * 0.4;
                }
                // Style 3: Digital Glitch (horizontal strips + flicker)
                else if (g_DissolveStyle == 3) {
                    float strips = step(0.5, frac(input.WorldPos.z * 0.25 + g_Time * 1.5));
                    noiseVal = noiseVal * 0.4 + strips * 0.6;
                    float glitch = step(0.85, frac(sin(g_Time * 60.0 + input.WorldPos.y * 3.0) * 43758.5453));
                    dThresh += glitch * 0.12;
                }

                // Clip dissolved pixels
                if (noiseVal < dThresh) discard;

                // Edge glow
                float edgeDist = noiseVal - dThresh;
                if (edgeDist < g_EdgeWidth) {
                    float edgeT = 1.0 - (edgeDist / g_EdgeWidth);
                    edgeT = edgeT * edgeT;
                    result.rgb = lerp(result.rgb, g_EdgeColor, edgeT * g_GlowStrength);
                    result.a = max(result.a, edgeT * g_GlowStrength * 0.8);
                }

                // Fade overall alpha near end of dissolve
                result.a *= saturate(1.5 - g_DissolveAmount);
            }

            return result;
        }

        float4 PS_Wire(PS_INPUT input) : SV_TARGET {
            float vis = input.Visibility;
            float3 wire_color = lerp(g_WireColor.rgb, g_WireColorVis.rgb, vis);
            float wire_alpha = lerp(g_WireColor.a, g_WireColorVis.a, vis) * g_Alpha;

            // Dissolve clipping for wireframe too
            if (g_DissolveAmount > 0.001) {
                float3 nc = input.WorldPos * g_NoiseScale;
                float3 ni = floor(nc); float3 nf = frac(nc);
                nf = nf * nf * (3.0 - 2.0 * nf);
                #define H31w(p) frac(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453)
                float noiseVal = lerp(
                    lerp(lerp(H31w(ni), H31w(ni+float3(1,0,0)), nf.x), lerp(H31w(ni+float3(0,1,0)), H31w(ni+float3(1,1,0)), nf.x), nf.y),
                    lerp(lerp(H31w(ni+float3(0,0,1)), H31w(ni+float3(1,0,1)), nf.x), lerp(H31w(ni+float3(0,1,1)), H31w(ni+float3(1,1,1)), nf.x), nf.y),
                    nf.z);
                #undef H31w
                float dThresh = g_DissolveAmount;
                if (g_DissolveStyle == 0) { dThresh = g_DissolveAmount * 1.3 - saturate(input.WorldPos.z * 0.008 + 0.3) * 0.35; }
                else if (g_DissolveStyle == 3) { float strips = step(0.5, frac(input.WorldPos.z * 0.25 + g_Time * 1.5)); noiseVal = noiseVal * 0.4 + strips * 0.6; }
                if (noiseVal < dThresh) discard;
                wire_alpha *= saturate(1.5 - g_DissolveAmount);
            }

            return float4(wire_color, wire_alpha);
        }
    )";
}




namespace ChamsV2
{
    
    static bool IsValidBoneMatrix(const BoneMatrix3x4& mat) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                if (!std::isfinite(mat.m[r][c])) return false;
            }
        }
        
        if (std::abs(mat.m[0][3]) > 100000.f || std::abs(mat.m[1][3]) > 100000.f || std::abs(mat.m[2][3]) > 100000.f)
            return false;
        return true;
    }

    
    static bool IsValidSkinMatrix(const BoneMatrix3x4& mat) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                if (!std::isfinite(mat.m[r][c])) return false;
                if (std::abs(mat.m[r][c]) > 1e6f) return false;
            }
        }
        return true;
    }

    
    static BoneMatrix3x4 BuildWorldBoneMatrix(const Vector& pos, float qx, float qy, float qz, float qw) {
        
        float ql2 = qx * qx + qy * qy + qz * qz + qw * qw;
        if (ql2 < 1e-12f) { qx = 0; qy = 0; qz = 0; qw = 1; }
        else { float inv = 1.f / sqrtf(ql2); qx *= inv; qy *= inv; qz *= inv; qw *= inv; }

        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;

        BoneMatrix3x4 mat{};
        mat.m[0][0] = 1.f - 2.f * (yy + zz);
        mat.m[0][1] = 2.f * (xy - wz);
        mat.m[0][2] = 2.f * (xz + wy);
        mat.m[0][3] = pos.x;

        mat.m[1][0] = 2.f * (xy + wz);
        mat.m[1][1] = 1.f - 2.f * (xx + zz);
        mat.m[1][2] = 2.f * (yz - wx);
        mat.m[1][3] = pos.y;

        mat.m[2][0] = 2.f * (xz - wy);
        mat.m[2][1] = 2.f * (yz + wx);
        mat.m[2][2] = 1.f - 2.f * (xx + yy);
        mat.m[2][3] = pos.z;

        return mat;
    }
}




extern ID3D11RenderTargetView* g_ChamsRTV;

namespace ChamsV2
{
    bool GPURenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        m_device = device;
        m_context = context;

        if (!CreateShaders()) return false;
        if (!CreateConstantBuffers()) return false;
        if (!CreatePipelineStates()) return false;

        m_initialized = true;
        return true;
    }

    void GPURenderer::Shutdown() {
        if (m_vertex_shader)     { m_vertex_shader->Release();     m_vertex_shader = nullptr; }
        if (m_pixel_shader_fill) { m_pixel_shader_fill->Release(); m_pixel_shader_fill = nullptr; }
        if (m_pixel_shader_wire) { m_pixel_shader_wire->Release(); m_pixel_shader_wire = nullptr; }
        if (m_input_layout)      { m_input_layout->Release();      m_input_layout = nullptr; }
        if (m_cb_view_projection){ m_cb_view_projection->Release();m_cb_view_projection = nullptr; }
        if (m_cb_bones)          { m_cb_bones->Release();          m_cb_bones = nullptr; }
        if (m_cb_bone_visibility){ m_cb_bone_visibility->Release();m_cb_bone_visibility = nullptr; }
        if (m_cb_material)       { m_cb_material->Release();       m_cb_material = nullptr; }
        if (m_rs_fill)           { m_rs_fill->Release();           m_rs_fill = nullptr; }
        if (m_rs_wireframe)      { m_rs_wireframe->Release();      m_rs_wireframe = nullptr; }
        if (m_blend_state)       { m_blend_state->Release();       m_blend_state = nullptr; }
        if (m_depth_state)       { m_depth_state->Release();       m_depth_state = nullptr; }
        m_initialized = false;
    }

    bool GPURenderer::CreateShaders() {
        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_fill_blob = nullptr;
        ID3DBlob* ps_wire_blob = nullptr;
        ID3DBlob* error_blob = nullptr;

        HRESULT hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr,
            "VS_Skinning", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &error_blob);
        if (FAILED(hr)) {
            if (error_blob) error_blob->Release();
            return false;
        }

        hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr,
            "PS_Fill", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_fill_blob, &error_blob);
        if (FAILED(hr)) {
            if (error_blob) error_blob->Release();
            vs_blob->Release();
            return false;
        }

        hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr,
            "PS_Wire", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_wire_blob, &error_blob);
        if (FAILED(hr)) {
            if (error_blob) error_blob->Release();
            vs_blob->Release();
            ps_fill_blob->Release();
            return false;
        }

        m_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
        m_device->CreatePixelShader(ps_fill_blob->GetBufferPointer(), ps_fill_blob->GetBufferSize(), nullptr, &m_pixel_shader_fill);
        m_device->CreatePixelShader(ps_wire_blob->GetBufferPointer(), ps_wire_blob->GetBufferSize(), nullptr, &m_pixel_shader_wire);

        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        hr = m_device->CreateInputLayout(layout, 4, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);

        vs_blob->Release();
        ps_fill_blob->Release();
        ps_wire_blob->Release();

        return SUCCEEDED(hr);
    }

    bool GPURenderer::CreateConstantBuffers() {
        D3D11_BUFFER_DESC desc{};
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        desc.ByteWidth = sizeof(CBViewProjection);
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_cb_view_projection))) return false;

        desc.ByteWidth = sizeof(CBBoneMatrices);
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_cb_bones))) return false;

        desc.ByteWidth = sizeof(CBBoneVisibility);
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_cb_bone_visibility))) return false;

        desc.ByteWidth = sizeof(CBMaterial);
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_cb_material))) return false;

        return true;
    }

    bool GPURenderer::CreatePipelineStates() {
        D3D11_RASTERIZER_DESC rs_desc{};
        rs_desc.FillMode = D3D11_FILL_SOLID;
        rs_desc.CullMode = D3D11_CULL_NONE;
        rs_desc.DepthClipEnable = FALSE;
        if (FAILED(m_device->CreateRasterizerState(&rs_desc, &m_rs_fill))) return false;

        rs_desc.FillMode = D3D11_FILL_WIREFRAME;
        if (FAILED(m_device->CreateRasterizerState(&rs_desc, &m_rs_wireframe))) return false;

        D3D11_BLEND_DESC blend_desc{};
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateBlendState(&blend_desc, &m_blend_state))) return false;

        D3D11_DEPTH_STENCIL_DESC ds_desc{};
        ds_desc.DepthEnable = FALSE;
        ds_desc.StencilEnable = FALSE;
        if (FAILED(m_device->CreateDepthStencilState(&ds_desc, &m_depth_state))) return false;

        return true;
    }

    bool GPURenderer::UploadMesh(SkinnedMesh& mesh) {
        if (!m_device || mesh.vertices.empty()) return false;

        std::vector<GPUVertex> gpu_verts(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); i++) {
            auto& src = mesh.vertices[i];
            auto& dst = gpu_verts[i];

            dst.position[0] = src.position.x;
            dst.position[1] = src.position.y;
            dst.position[2] = src.position.z;

            dst.normal[0] = src.normal.x;
            dst.normal[1] = src.normal.y;
            dst.normal[2] = src.normal.z;

            dst.bone_indices =
                (static_cast<uint32_t>(src.joint_indices[0]) & 0xFF) |
                ((static_cast<uint32_t>(src.joint_indices[1]) & 0xFF) << 8) |
                ((static_cast<uint32_t>(src.joint_indices[2]) & 0xFF) << 16) |
                ((static_cast<uint32_t>(src.joint_indices[3]) & 0xFF) << 24);

            dst.weights[0] = src.weights[0];
            dst.weights[1] = src.weights[1];
            dst.weights[2] = src.weights[2];
            dst.weights[3] = src.weights[3];
        }

        D3D11_BUFFER_DESC vb_desc{};
        vb_desc.ByteWidth = static_cast<UINT>(gpu_verts.size() * sizeof(GPUVertex));
        vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vb_data{};
        vb_data.pSysMem = gpu_verts.data();

        if (FAILED(m_device->CreateBuffer(&vb_desc, &vb_data, &mesh.gpu_vertex_buffer))) return false;

        D3D11_BUFFER_DESC ib_desc{};
        ib_desc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
        ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
        ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ib_data{};
        ib_data.pSysMem = mesh.indices.data();

        if (FAILED(m_device->CreateBuffer(&ib_desc, &ib_data, &mesh.gpu_index_buffer))) return false;

        return true;
    }

    void GPURenderer::QueueDraw(SkinnedMesh* mesh,
        const BoneMatrix3x4* game_bones, int game_bone_count,
        const float* bone_visibility,
        float fill_color[4], float fill_color_vis[4],
        float wire_color[4], float wire_color_vis[4],
        float alpha, int render_mode, int material_type,
        float dissolve_amount, int dissolve_style,
        float edge_width, float noise_scale,
        const float* edge_color_rgb, float glow_strength)
    {
        if (!m_initialized || !mesh || !mesh->gpu_vertex_buffer) return;
        if (game_bone_count <= 0 || !game_bones[0].Valid()) return;
        if (m_draw_count >= MAX_DRAW_COMMANDS) return;

        DrawCommand& cmd = m_pending_draws[m_draw_count];
        cmd.mesh = mesh;
        memcpy(cmd.fill_color, fill_color, sizeof(float) * 4);
        memcpy(cmd.fill_color_vis, fill_color_vis, sizeof(float) * 4);
        memcpy(cmd.wire_color, wire_color, sizeof(float) * 4);
        memcpy(cmd.wire_color_vis, wire_color_vis, sizeof(float) * 4);
        cmd.alpha = alpha;
        cmd.render_mode = render_mode;
        cmd.material_type = material_type;

        
        cmd.dissolve_amount = dissolve_amount;
        cmd.dissolve_style = dissolve_style;
        cmd.edge_width = edge_width;
        cmd.noise_scale = noise_scale;
        if (edge_color_rgb) {
            cmd.edge_color[0] = edge_color_rgb[0];
            cmd.edge_color[1] = edge_color_rgb[1];
            cmd.edge_color[2] = edge_color_rgb[2];
        } else {
            cmd.edge_color[0] = 1.0f; cmd.edge_color[1] = 0.6f; cmd.edge_color[2] = 0.2f;
        }
        cmd.glow_strength = glow_strength;

        
        memcpy(cmd.bone_visibility, bone_visibility, sizeof(float) * MAX_BONES);

        size_t bone_count = (std::min)(mesh->inverse_bind_matrices.size(), static_cast<size_t>(MAX_BONES));
        cmd.combined_count = static_cast<int>(bone_count);

        
        BoneMatrix3x4 pelvis_ibm_fallback = game_bones[0].Multiply(
            bone_count > 0 ? mesh->inverse_bind_matrices[0] : BoneMatrix3x4::Identity());

        for (size_t i = 0; i < bone_count; i++) {
            int game_idx = (i < mesh->gltf_to_game_bone_map.size())
                ? mesh->gltf_to_game_bone_map[i]
                : 0;

            if (game_idx >= 0 && game_idx < game_bone_count) {
                BoneMatrix3x4 skin = game_bones[game_idx].Multiply(mesh->inverse_bind_matrices[i]);
                
                if (IsValidSkinMatrix(skin)) {
                    cmd.combined_matrices[i] = skin;
                } else {
                    
                    cmd.combined_matrices[i] = game_bones[0].Multiply(mesh->inverse_bind_matrices[i]);
                }
            }
            else {
                cmd.combined_matrices[i] = pelvis_ibm_fallback;
            }
        }

        m_draw_count++;
    }

    void GPURenderer::Flush() {
        if (!m_initialized || m_draw_count == 0) return;

        RenderStateGuard guard(m_context);

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(Globals::ScreenWidth);
        vp.Height = static_cast<float>(Globals::ScreenHeight);
        vp.MinDepth = 0.f;
        vp.MaxDepth = 1.f;
        vp.TopLeftX = Globals::GameViewportX;
        vp.TopLeftY = Globals::GameViewportY;
        m_context->RSSetViewports(1, &vp);

        if (g_ChamsRTV) {
            m_context->OMSetRenderTargets(1, &g_ChamsRTV, nullptr);
        }

        m_context->IASetInputLayout(m_input_layout);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertex_shader, nullptr, 0);

        float blend_factor[4] = { 0, 0, 0, 0 };
        m_context->OMSetBlendState(m_blend_state, blend_factor, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_depth_state, 0);

        UpdateViewProjection();

        
        ID3D11Buffer* vs_cbs[4] = { m_cb_view_projection, m_cb_bones, m_cb_material, m_cb_bone_visibility };
        m_context->VSSetConstantBuffers(0, 4, vs_cbs);
        
        ID3D11Buffer* ps_cbs[4] = { nullptr, nullptr, m_cb_material, m_cb_bone_visibility };
        m_context->PSSetConstantBuffers(0, 4, ps_cbs);

        for (int d = 0; d < m_draw_count; d++) {
            auto& cmd = m_pending_draws[d];
            UpdateBoneMatrices(cmd.combined_matrices, cmd.combined_count);
            UpdateBoneVisibility(cmd.bone_visibility);

            UINT stride = sizeof(GPUVertex);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, &cmd.mesh->gpu_vertex_buffer, &stride, &offset);
            m_context->IASetIndexBuffer(cmd.mesh->gpu_index_buffer, DXGI_FORMAT_R32_UINT, 0);

            
            if (cmd.render_mode == 0 || cmd.render_mode == 2) {
                UpdateMaterial(cmd.fill_color, cmd.fill_color_vis, cmd.wire_color, cmd.wire_color_vis,
                    cmd.alpha, 0, cmd.material_type,
                    cmd.dissolve_amount, cmd.dissolve_style, cmd.edge_width, cmd.noise_scale,
                    cmd.edge_color, cmd.glow_strength);
                m_context->RSSetState(m_rs_fill);
                m_context->PSSetShader(m_pixel_shader_fill, nullptr, 0);
                m_context->DrawIndexed(cmd.mesh->index_count, 0, 0);
            }

            
            if (cmd.render_mode == 1 || cmd.render_mode == 2) {
                UpdateMaterial(cmd.fill_color, cmd.fill_color_vis, cmd.wire_color, cmd.wire_color_vis,
                    cmd.alpha, 1, cmd.material_type,
                    cmd.dissolve_amount, cmd.dissolve_style, cmd.edge_width, cmd.noise_scale,
                    cmd.edge_color, cmd.glow_strength);
                m_context->RSSetState(m_rs_wireframe);
                m_context->PSSetShader(m_pixel_shader_wire, nullptr, 0);
                m_context->DrawIndexed(cmd.mesh->index_count, 0, 0);
            }
        }

        m_draw_count = 0;
    }

    void GPURenderer::UpdateViewProjection() {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_cb_view_projection, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

        auto* data = static_cast<CBViewProjection*>(mapped.pData);
        memcpy(data->vp, Globals::ViewMatrix, sizeof(float) * 16);

        data->screen_w = static_cast<float>(Globals::ScreenWidth);
        data->screen_h = static_cast<float>(Globals::ScreenHeight);
        data->pad[0] = 0.f;
        data->pad[1] = 0.f;

        m_context->Unmap(m_cb_view_projection, 0);
    }

    void GPURenderer::UpdateBoneMatrices(const BoneMatrix3x4* combined, int count) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_cb_bones, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

        auto* data = static_cast<CBBoneMatrices*>(mapped.pData);
        memset(data, 0, sizeof(CBBoneMatrices));

        int n = (std::min)(count, MAX_BONES);
        for (int i = 0; i < n; i++) {
            combined[i].To4x4(data->bones[i]);
        }

        m_context->Unmap(m_cb_bones, 0);
    }

    void GPURenderer::UpdateBoneVisibility(const float* visibility) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_cb_bone_visibility, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

        auto* data = static_cast<CBBoneVisibility*>(mapped.pData);
        
        memcpy(data->visibility, visibility, sizeof(float) * MAX_BONES);

        m_context->Unmap(m_cb_bone_visibility, 0);
    }

    void GPURenderer::UpdateMaterial(float fill[4], float fill_vis[4], float wire[4], float wire_vis[4], 
                                     float alpha, int mode, int material_type,
                                     float dissolve_amount, int dissolve_style,
                                     float edge_width, float noise_scale,
                                     const float* edge_color_rgb, float glow_strength) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_cb_material, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

        auto* data = static_cast<CBMaterial*>(mapped.pData);
        memcpy(data->color, fill, sizeof(float) * 4);
        memcpy(data->color_vis, fill_vis, sizeof(float) * 4);
        memcpy(data->wire_color, wire, sizeof(float) * 4);
        memcpy(data->wire_color_vis, wire_vis, sizeof(float) * 4);

        data->render_mode = mode;
        data->alpha = alpha;
        data->material_type = material_type;
        data->rim_power = 2.5f;

        auto& vm = Globals::ViewMatrix;
        float tx = vm[3], ty = vm[7], tz = vm[11];
        data->cam_pos[0] = -(vm[0] * tx + vm[4] * ty + vm[8] * tz);
        data->cam_pos[1] = -(vm[1] * tx + vm[5] * ty + vm[9] * tz);
        data->cam_pos[2] = -(vm[2] * tx + vm[6] * ty + vm[10] * tz);
        data->time = static_cast<float>(GetTickCount64()) / 1000.0f;

        
        data->dissolve_amount = dissolve_amount;
        data->dissolve_style = dissolve_style;
        data->edge_width = edge_width;
        data->noise_scale = noise_scale;
        if (edge_color_rgb) {
            data->edge_color[0] = edge_color_rgb[0];
            data->edge_color[1] = edge_color_rgb[1];
            data->edge_color[2] = edge_color_rgb[2];
        } else {
            data->edge_color[0] = 1.0f; data->edge_color[1] = 0.6f; data->edge_color[2] = 0.2f;
        }
        data->glow_strength = glow_strength;

        m_context->Unmap(m_cb_material, 0);
    }
}




namespace ChamsV2
{
    BoneMatrix3x4 MeshLoader::ConvertIBM(const float* col4x4) {
        BoneMatrix3x4 out{};
        out.m[0][0] = col4x4[0]; out.m[0][1] = col4x4[4]; out.m[0][2] = col4x4[8];  out.m[0][3] = col4x4[12];
        out.m[1][0] = col4x4[1]; out.m[1][1] = col4x4[5]; out.m[1][2] = col4x4[9];  out.m[1][3] = col4x4[13];
        out.m[2][0] = col4x4[2]; out.m[2][1] = col4x4[6]; out.m[2][2] = col4x4[10]; out.m[2][3] = col4x4[14];
        return out;
    }

    void MeshLoader::BuildBoneMapping(const std::vector<std::string>& joint_names, std::vector<int16_t>& mapping) {
        mapping.resize(joint_names.size());
        for (size_t i = 0; i < joint_names.size(); i++) {
            mapping[i] = static_cast<int16_t>(i);
        }
    }

    bool MeshLoader::LoadGLB(const std::string& path, SkinnedMesh& mesh) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        size_t file_size = static_cast<size_t>(file.tellg());
        file.seekg(0);

        std::vector<uint8_t> file_data(file_size);
        file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        file.close();

        if (file_size < 12) return false;

        uint32_t magic = *reinterpret_cast<uint32_t*>(&file_data[0]);
        uint32_t version = *reinterpret_cast<uint32_t*>(&file_data[4]);

        if (magic != 0x46546C67) return false;  
        if (version != 2) return false;

        size_t offset = 12;

        uint32_t json_length = *reinterpret_cast<uint32_t*>(&file_data[offset]);
        uint32_t json_type = *reinterpret_cast<uint32_t*>(&file_data[offset + 4]);
        offset += 8;

        if (json_type != 0x4E4F534A) return false;  

        std::string json_str(reinterpret_cast<char*>(&file_data[offset]), json_length);
        offset += json_length;

        if (offset + 8 > file_size) return false;

        uint32_t bin_length = *reinterpret_cast<uint32_t*>(&file_data[offset]);
        uint32_t bin_type = *reinterpret_cast<uint32_t*>(&file_data[offset + 4]);
        offset += 8;

        if (bin_type != 0x004E4942) return false;  

        const uint8_t* bin_data = &file_data[offset];

        json gltf;
        try {
            gltf = json::parse(json_str);
        }
        catch (...) {
            return false;
        }

        
        auto& bv_array = gltf["bufferViews"];
        struct BufferViewInfo { int byte_offset; int byte_length; int byte_stride; };
        std::vector<BufferViewInfo> buffer_views;

        for (auto& bv : bv_array) {
            buffer_views.push_back({
                bv.value("byteOffset", 0),
                bv["byteLength"].get<int>(),
                bv.value("byteStride", 0)
            });
        }

        
        struct AccessorInfo {
            int buffer_view; int byte_offset; int component_type; int count; std::string type;
        };
        std::vector<AccessorInfo> accessors;

        for (auto& acc : gltf["accessors"]) {
            accessors.push_back({
                acc["bufferView"].get<int>(),
                acc.value("byteOffset", 0),
                acc["componentType"].get<int>(),
                acc["count"].get<int>(),
                acc["type"].get<std::string>()
            });
        }

        auto get_accessor_ptr = [&](int acc_idx) -> const uint8_t* {
            auto& acc = accessors[acc_idx];
            auto& bv = buffer_views[acc.buffer_view];
            return bin_data + bv.byte_offset + acc.byte_offset;
        };

        
        std::vector<std::string> node_names;
        for (auto& node : gltf["nodes"]) {
            node_names.push_back(node.value("name", ""));
        }

        if (!gltf.contains("skins") || gltf["skins"].empty()) return false;

        auto& skin = gltf["skins"][0];
        auto& joints = skin["joints"];
        int ibm_accessor_idx = skin["inverseBindMatrices"].get<int>();

        
        auto& ibm_acc = accessors[ibm_accessor_idx];
        const float* ibm_ptr = reinterpret_cast<const float*>(get_accessor_ptr(ibm_accessor_idx));

        mesh.inverse_bind_matrices.resize(ibm_acc.count);
        for (int i = 0; i < ibm_acc.count; i++) {
            mesh.inverse_bind_matrices[i] = ConvertIBM(&ibm_ptr[i * 16]);
        }

        std::vector<std::string> joint_names;
        for (auto& j : joints) {
            int node_idx = j.get<int>();
            joint_names.push_back(node_names[node_idx]);
        }

        BuildBoneMapping(joint_names, mesh.gltf_to_game_bone_map);

        
        mesh.vertices.clear();
        mesh.indices.clear();

        for (auto& node : gltf["nodes"]) {
            if (!node.contains("mesh") || !node.contains("skin"))
                continue;

            int mesh_idx = node["mesh"].get<int>();
            auto& gltf_mesh = gltf["meshes"][mesh_idx];

            for (auto& prim : gltf_mesh["primitives"]) {
                auto& attrs = prim["attributes"];

                if (!attrs.contains("POSITION") || !attrs.contains("JOINTS_0") ||
                    !attrs.contains("WEIGHTS_0") || !prim.contains("indices"))
                    continue;

                int pos_acc_idx = attrs["POSITION"].get<int>();
                int joints_acc_idx = attrs["JOINTS_0"].get<int>();
                int weights_acc_idx = attrs["WEIGHTS_0"].get<int>();
                int indices_acc_idx = prim["indices"].get<int>();

                auto& pos_acc = accessors[pos_acc_idx];
                auto& joints_acc = accessors[joints_acc_idx];
                auto& weights_acc = accessors[weights_acc_idx];
                auto& indices_acc = accessors[indices_acc_idx];

                uint32_t base_vertex = static_cast<uint32_t>(mesh.vertices.size());
                const float* pos_ptr = reinterpret_cast<const float*>(get_accessor_ptr(pos_acc_idx));
                const uint16_t* joints_ptr = reinterpret_cast<const uint16_t*>(get_accessor_ptr(joints_acc_idx));
                const float* weights_ptr = reinterpret_cast<const float*>(get_accessor_ptr(weights_acc_idx));

                
                bool joints_are_bytes = (joints_acc.component_type == 5121);
                const uint8_t* joints_byte_ptr = reinterpret_cast<const uint8_t*>(get_accessor_ptr(joints_acc_idx));

                for (int v = 0; v < pos_acc.count; v++) {
                    MeshVertex vert{};
                    vert.position.x = pos_ptr[v * 3 + 0];
                    vert.position.y = pos_ptr[v * 3 + 1];
                    vert.position.z = pos_ptr[v * 3 + 2];

                    if (joints_are_bytes) {
                        vert.joint_indices[0] = joints_byte_ptr[v * 4 + 0];
                        vert.joint_indices[1] = joints_byte_ptr[v * 4 + 1];
                        vert.joint_indices[2] = joints_byte_ptr[v * 4 + 2];
                        vert.joint_indices[3] = joints_byte_ptr[v * 4 + 3];
                    } else {
                        vert.joint_indices[0] = joints_ptr[v * 4 + 0];
                        vert.joint_indices[1] = joints_ptr[v * 4 + 1];
                        vert.joint_indices[2] = joints_ptr[v * 4 + 2];
                        vert.joint_indices[3] = joints_ptr[v * 4 + 3];
                    }

                    vert.weights[0] = weights_ptr[v * 4 + 0];
                    vert.weights[1] = weights_ptr[v * 4 + 1];
                    vert.weights[2] = weights_ptr[v * 4 + 2];
                    vert.weights[3] = weights_ptr[v * 4 + 3];

                    mesh.vertices.push_back(vert);
                }

                
                if (indices_acc.component_type == 5123) {
                    const uint16_t* idx16_ptr = reinterpret_cast<const uint16_t*>(get_accessor_ptr(indices_acc_idx));
                    for (int i = 0; i < indices_acc.count; i++) {
                        mesh.indices.push_back(static_cast<uint32_t>(idx16_ptr[i]) + base_vertex);
                    }
                } else {
                    const uint32_t* idx_ptr = reinterpret_cast<const uint32_t*>(get_accessor_ptr(indices_acc_idx));
                    for (int i = 0; i < indices_acc.count; i++) {
                        mesh.indices.push_back(idx_ptr[i] + base_vertex);
                    }
                }
            }
        }

        mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size());
        mesh.index_count = static_cast<uint32_t>(mesh.indices.size());

        
        for (auto& v : mesh.vertices)
            v.normal = { 0.f, 0.f, 0.f };

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            auto& v0 = mesh.vertices[mesh.indices[i]];
            auto& v1 = mesh.vertices[mesh.indices[i + 1]];
            auto& v2 = mesh.vertices[mesh.indices[i + 2]];

            Vector e1 = v1.position - v0.position;
            Vector e2 = v2.position - v0.position;

            
            Vector n;
            n.x = e1.y * e2.z - e1.z * e2.y;
            n.y = e1.z * e2.x - e1.x * e2.z;
            n.z = e1.x * e2.y - e1.y * e2.x;

            v0.normal += n;
            v1.normal += n;
            v2.normal += n;
        }

        
        for (auto& v : mesh.vertices) {
            float len = v.normal.Length();
            if (len > 0.0001f) {
                v.normal.x /= len;
                v.normal.y /= len;
                v.normal.z /= len;
            }
        }

        return mesh.vertex_count > 0 && mesh.index_count > 0;
    }
}




namespace ChamsV2
{
    bool MeshRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                   const std::string& ct_mesh_path, const std::string& t_mesh_path) {
        if (!m_gpu_renderer.Initialize(device, context)) {
            return false;
        }

        
        if (!MeshLoader::LoadGLB(ct_mesh_path, m_mesh_ct)) {
            return false;
        }
        if (!m_gpu_renderer.UploadMesh(m_mesh_ct)) {
            return false;
        }

        
        if (!MeshLoader::LoadGLB(t_mesh_path, m_mesh_t)) {
            return false;
        }
        if (!m_gpu_renderer.UploadMesh(m_mesh_t)) {
            return false;
        }

        m_ready = true;
        return true;
    }

    void MeshRenderer::Shutdown() {
        m_mesh_ct.Release();
        m_mesh_t.Release();
        m_gpu_renderer.Shutdown();
        m_ready = false;
    }

    
    struct GameBone {
        Vector position;
        float scale;
        float qx, qy, qz, qw;
    };

    
    static bool ReadModelNameSafe(C_CSPlayerPawn* pawn, char* out_name, int max_len) {
        __try {
            uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(pawn);
            uintptr_t game_scene = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
            if (!game_scene || !Utils::IsValidPtr(game_scene)) return false;

            uintptr_t model_name_ptr = Utils::SafeRead<uintptr_t>(game_scene + Offsets::m_modelState + Offsets::m_ModelName);
            if (!model_name_ptr || !Utils::IsValidPtr(model_name_ptr)) return false;

            for (int i = 0; i < max_len - 1; i++) {
                out_name[i] = Utils::SafeRead<char>(model_name_ptr + i);
                if (out_name[i] == '\0') return true;
            }
            out_name[max_len - 1] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            out_name[0] = '\0';
            return false;
        }
    }

    bool MeshRenderer::IsModelTerrorist(C_CSPlayerPawn* pawn) const {
        char model_name[260] = {};
        if (!ReadModelNameSafe(pawn, model_name, 260))
            return false;

        
        
        
        const char* p = model_name;
        bool has_tm = (strstr(p, "tm_") != nullptr);
        bool has_ctm = (strstr(p, "ctm_") != nullptr);

        return has_tm && !has_ctm;
    }

    int MeshRenderer::GetDetectedMeshTeam(bool modelIsTerrorist, int gameTeamNow) const {
        int detected = modelIsTerrorist ? 1 : 0;
        if (m_halftime_model_swap) {
            int gameTeamDetected = -1;
            if (gameTeamNow == 2) {
                gameTeamDetected = 1;
            } else if (gameTeamNow == 3) {
                gameTeamDetected = 0;
            }

            if (gameTeamDetected < 0 || gameTeamDetected != detected) {
                detected = 1 - detected;
            }
        }
        return detected;
    }

    void MeshRenderer::UpdateGamePhaseModelSwapState() {
        uintptr_t client = Memory::GetModuleBase("client.dll");
        if (!client) return;

        uintptr_t gameRules = Utils::SafeRead<uintptr_t>(client + Offsets::dwGameRules);
        if (!Utils::IsValidPtr(gameRules)) return;

        int gamePhaseNow = Utils::SafeRead<int>(gameRules + Offsets::m_gamePhase, -1);
        int roundsInPhaseNow = Utils::SafeRead<int>(gameRules + Offsets::m_nRoundsPlayedThisPhase, -1);
        if (gamePhaseNow < 0 || roundsInPhaseNow < 0) return;

        if (m_last_game_phase < 0) {
            m_last_game_phase = gamePhaseNow;
            m_last_rounds_played_this_phase = roundsInPhaseNow;
            return;
        }

        const bool gamePhaseChanged = (gamePhaseNow != m_last_game_phase);
        const bool phaseRoundCounterReset = (m_last_rounds_played_this_phase > 0 && roundsInPhaseNow == 0);

        if (gamePhaseChanged && phaseRoundCounterReset) {
            m_halftime_model_swap = !m_halftime_model_swap;
            for (auto& kv : m_pawn_cache) {
                kv.second.cached_team = -1;
                kv.second.cached_game_team = -1;
                kv.second.bone_count = 0;
                kv.second.visibility_valid = false;
                kv.second.has_last_good_pelvis = false;
            }
        } else if (gamePhaseChanged) {
            for (auto& kv : m_pawn_cache) {
                kv.second.cached_team = -1;
            }
        }

        m_last_game_phase = gamePhaseNow;
        m_last_rounds_played_this_phase = roundsInPhaseNow;
    }

    static bool IsFiniteBonePosition(const Vector& pos)
    {
        return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
    }

    
    
    int MeshRenderer::ReadGameBones(C_CSPlayerPawn* pawn, const PawnCache& cache, int bone_count, BoneMatrix3x4* out_bones) {
        uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(pawn);
        uintptr_t game_scene = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
        if (!game_scene || !Utils::IsValidPtr(game_scene)) return 0;

        
        uintptr_t bone_array = Utils::SafeRead<uintptr_t>(game_scene + Offsets::m_modelState + 0x80);
        if (!bone_array || !Utils::IsValidPtr(bone_array)) return 0;

        
        GameBone pelvis_bd = Utils::SafeRead<GameBone>(bone_array);
        if (!std::isfinite(pelvis_bd.position.x) || !std::isfinite(pelvis_bd.position.y) || !std::isfinite(pelvis_bd.position.z))
            return 0;
        if (pelvis_bd.position.x == 0.f && pelvis_bd.position.y == 0.f && pelvis_bd.position.z == 0.f)
            return 0;

        BoneMatrix3x4 pelvisWorld = BuildWorldBoneMatrix(pelvis_bd.position, pelvis_bd.qx, pelvis_bd.qy, pelvis_bd.qz, pelvis_bd.qw);

        Vector origin = Utils::SafeRead<Vector>(game_scene + Offsets::m_vecAbsOrigin);
        const bool is_alive = Utils::SafeAlive(pawn);

        
        Vector absMin, absMax;
        bool has_aabb = false;

        if (is_alive) {
            uintptr_t collision = Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pCollision);
            if (collision && Utils::IsValidPtr(collision)) {
                Vector colMins = Utils::SafeRead<Vector>(collision + Offsets::m_vecMins);
                Vector colMaxs = Utils::SafeRead<Vector>(collision + Offsets::m_vecMaxs);
                absMin = { origin.x + colMins.x - 10.f, origin.y + colMins.y - 10.f, origin.z + colMins.z - 10.f };
                absMax = { origin.x + colMaxs.x + 10.f, origin.y + colMaxs.y + 10.f, origin.z + colMaxs.z + 10.f };
                has_aabb = true;
            } else {
                
                absMin = { pelvis_bd.position.x - 200.f, pelvis_bd.position.y - 200.f, pelvis_bd.position.z - 200.f };
                absMax = { pelvis_bd.position.x + 200.f, pelvis_bd.position.y + 200.f, pelvis_bd.position.z + 200.f };
                has_aabb = true;
            }
        } else {
            
            const bool pelvis_near_origin = IsFiniteBonePosition(origin)
                && (origin - pelvis_bd.position).Length() <= 180.f;
            const bool pelvis_near_cache = cache.has_last_good_pelvis
                && (cache.last_good_pelvis - pelvis_bd.position).Length() <= 128.f;

            if (!pelvis_near_origin && !pelvis_near_cache) {
                return 0;
            }
        }

        int mappedCount = 0;

        for (int i = 0; i < bone_count; i++) {
            uintptr_t boneAddr = bone_array + i * sizeof(GameBone);
            if (!Utils::IsValidPtr(boneAddr)) {
                out_bones[i] = pelvisWorld; 
                continue;
            }
            GameBone bd = Utils::SafeRead<GameBone>(boneAddr);

            bool outsideBounds;
            if (is_alive && has_aabb) {
                
                bool posFinite = std::isfinite(bd.position.x) && std::isfinite(bd.position.y) && std::isfinite(bd.position.z);
                outsideBounds = !posFinite ||
                    (bd.position.x == 0.f && bd.position.y == 0.f && bd.position.z == 0.f) ||
                    bd.position.x < absMin.x || bd.position.x > absMax.x ||
                    bd.position.y < absMin.y || bd.position.y > absMax.y ||
                    bd.position.z < absMin.z || bd.position.z > absMax.z;
            } else {
                
                outsideBounds = !IsFiniteBonePosition(bd.position)
                    || (bd.position.x == 0.f && bd.position.y == 0.f && bd.position.z == 0.f)
                    || (pelvis_bd.position - bd.position).Length() >= 140.f;
            }

            if (!outsideBounds) {
                BoneMatrix3x4 candidate = BuildWorldBoneMatrix(bd.position, bd.qx, bd.qy, bd.qz, bd.qw);
                if (IsValidBoneMatrix(candidate)) {
                    out_bones[i] = candidate;
                    mappedCount++;
                    continue;
                }
            }

            
            out_bones[i] = pelvisWorld;
        }

        
        if (mappedCount == 0) return 0;

        return bone_count;
    }

    void MeshRenderer::CleanupStaleEntries() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_cleanup).count();
        if (elapsed < CACHE_CLEANUP_INTERVAL_MS) return;
        m_last_cleanup = now;

        
        for (auto it = m_pawn_cache.begin(); it != m_pawn_cache.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_seen).count();
            if (age > CACHE_STALE_THRESHOLD_MS) {
                it = m_pawn_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void MeshRenderer::RenderPlayer(C_CSPlayerController* controller, C_CSPlayerPawn* pawn, float esp_alpha, bool isEnemy) {
        if (!m_ready) return;
        const bool chamsEnabled = isEnemy ? Globals::chamsv2_enabled : Globals::chamsv2_enabled_team;
        if (!chamsEnabled) return;

        UpdateGamePhaseModelSwapState();

        uintptr_t pawn_addr = reinterpret_cast<uintptr_t>(pawn);
        auto now = std::chrono::steady_clock::now();

        
        CleanupStaleEntries();

        
        auto& cache = m_pawn_cache[pawn_addr];
        cache.last_seen = now;

        const bool aliveNow = Utils::SafeAlive(pawn);
        const int gameTeamNow = Utils::SafeTeam(pawn);

        bool shouldRefreshModel = (cache.cached_team < 0);
        auto model_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - cache.last_model_check).count();
        if (aliveNow && model_elapsed >= MODEL_RECHECK_INTERVAL_MS) {
            shouldRefreshModel = true;
        }

        if (gameTeamNow > 1) {
            if (cache.cached_game_team < 0) {
                cache.cached_game_team = gameTeamNow;
            } else if (cache.cached_game_team != gameTeamNow) {
                cache.cached_game_team = gameTeamNow;
                shouldRefreshModel = true;
            }
        }

        if (!cache.alive_initialized) {
            cache.alive_initialized = true;
            cache.was_alive = aliveNow;
        } else if (!cache.was_alive && aliveNow) {
            shouldRefreshModel = true;
        }
        cache.was_alive = aliveNow;

        if (shouldRefreshModel) {
            cache.last_model_check = now;
            const int detectedTeam = GetDetectedMeshTeam(IsModelTerrorist(pawn), gameTeamNow);
            if (cache.cached_team != detectedTeam) {
                cache.cached_team = static_cast<int8_t>(detectedTeam);
                cache.bone_count = 0;
                cache.visibility_valid = false;
                cache.has_last_good_pelvis = false;
            }
        }
        bool is_t = (cache.cached_team == 1);
        SkinnedMesh& mesh = is_t ? m_mesh_t : m_mesh_ct;

        int needed = static_cast<int>(mesh.gltf_to_game_bone_map.size());
        needed = (std::min)(needed, MAX_BONES);

        
        BoneMatrix3x4 game_bones[MAX_BONES];
        int bone_read = ReadGameBones(pawn, cache, needed, game_bones);
        if (bone_read == 0) {
            
            if (cache.bone_count > 0) {
                needed = cache.bone_count;
                memcpy(game_bones, cache.prev_bones, sizeof(BoneMatrix3x4) * needed);
            } else {
                return;
            }
        } else {
            
            memcpy(cache.prev_bones, game_bones, sizeof(BoneMatrix3x4) * needed);
            cache.bone_count = needed;
            cache.last_good_pelvis = game_bones[0].GetOrigin();
            cache.has_last_good_pelvis = true;
        }

        
        if (!IsPlayerInViewFrustumV2(game_bones, needed)) {
            return;
        }

        
        
        auto vis_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - cache.last_visibility_update).count();
        if (!cache.visibility_valid || vis_elapsed >= VISIBILITY_UPDATE_INTERVAL_MS) {
            GetPerBoneVisibility(pawn, game_bones, needed, cache.cached_visibility);
            cache.last_visibility_update = now;
            cache.visibility_valid = true;
        }

        
        int render_mode = 0;
        bool is_dead = !Utils::SafeAlive(pawn);

        float* fillHidden = isEnemy ? Globals::chamsv2_fill_color : Globals::chamsv2_fill_color_team;
        float* fillVisible = isEnemy ? Globals::chamsv2_fill_color_vis : Globals::chamsv2_fill_color_vis_team;
        float* wireHidden = isEnemy ? Globals::chamsv2_wire_color : Globals::chamsv2_wire_color_team;
        float* wireVisible = isEnemy ? Globals::chamsv2_wire_color_vis : Globals::chamsv2_wire_color_vis_team;
        int materialType = isEnemy ? Globals::chamsv2_material_type : Globals::chamsv2_material_type_team;

        
        float deadFill[4] = { 0.8f, 0.05f, 0.05f, 1.0f };
        float deadWire[4] = { 0.8f, 0.05f, 0.05f, 1.0f };
        if (is_dead && isEnemy) {
            fillHidden = deadFill;
            fillVisible = deadFill;
            wireHidden = deadWire;
            wireVisible = deadWire;
            materialType = 0; 
        }

        m_gpu_renderer.QueueDraw(&mesh, game_bones, needed, cache.cached_visibility,
            fillHidden, fillVisible, wireHidden, wireVisible,
            esp_alpha, render_mode, materialType);
    }

    void MeshRenderer::Flush() {
        if (!m_ready) return;
        m_gpu_renderer.Flush();
    }

    bool MeshRenderer::CreateSnapshot(C_CSPlayerPawn* pawn, bool isEnemy, DissolveSnapshot& out) {
        if (!m_ready || !pawn) return false;
        UpdateGamePhaseModelSwapState();
        uintptr_t pawn_addr = reinterpret_cast<uintptr_t>(pawn);
        if (!Utils::IsValidPtr(pawn_addr)) return false;

        
        const int gameTeamNow = Utils::SafeTeam(pawn);
        out.is_terrorist = (GetDetectedMeshTeam(IsModelTerrorist(pawn), gameTeamNow) == 1);
        out.is_enemy = isEnemy;
        SkinnedMesh& mesh = out.is_terrorist ? m_mesh_t : m_mesh_ct;

        int needed = static_cast<int>(mesh.gltf_to_game_bone_map.size());
        needed = (std::min)(needed, MAX_BONES);
        if (needed <= 0) return false;

        
        auto cacheIt = m_pawn_cache.find(pawn_addr);
        if (cacheIt != m_pawn_cache.end() && cacheIt->second.bone_count > 0) {
            out.bone_count = cacheIt->second.bone_count;
            memcpy(out.bones, cacheIt->second.prev_bones, sizeof(BoneMatrix3x4) * out.bone_count);
            memcpy(out.bone_visibility, cacheIt->second.cached_visibility, sizeof(float) * MAX_BONES);
        } else {
            
            PawnCache emptyCache{};
            int bone_read = ReadGameBones(pawn, emptyCache, needed, out.bones);
            if (bone_read == 0) return false;
            out.bone_count = needed;
            
            for (int i = 0; i < MAX_BONES; i++)
                out.bone_visibility[i] = (i < needed) ? 1.0f : 0.0f;
        }

        
        out.material_type = isEnemy ? Globals::chamsv2_material_type : Globals::chamsv2_material_type_team;
        float* fh = isEnemy ? Globals::chamsv2_fill_color : Globals::chamsv2_fill_color_team;
        float* fv = isEnemy ? Globals::chamsv2_fill_color_vis : Globals::chamsv2_fill_color_vis_team;
        float* wh = isEnemy ? Globals::chamsv2_wire_color : Globals::chamsv2_wire_color_team;
        float* wv = isEnemy ? Globals::chamsv2_wire_color_vis : Globals::chamsv2_wire_color_vis_team;
        memcpy(out.fill_color, fh, sizeof(float) * 4);
        memcpy(out.fill_color_vis, fv, sizeof(float) * 4);
        memcpy(out.wire_color, wh, sizeof(float) * 4);
        memcpy(out.wire_color_vis, wv, sizeof(float) * 4);

        return true;
    }

    void MeshRenderer::RenderSnapshot(const DissolveSnapshot& snap, float dissolve_amount, int dissolve_style,
                                      float edge_width, float noise_scale, const float* edge_color, float glow_strength) {
        if (!m_ready) return;
        if (snap.bone_count <= 0) return;

        SkinnedMesh& mesh = snap.is_terrorist ? m_mesh_t : m_mesh_ct;

        
        float fill[4], fill_vis[4], wire[4], wire_vis[4];
        memcpy(fill, snap.fill_color, sizeof(float) * 4);
        memcpy(fill_vis, snap.fill_color_vis, sizeof(float) * 4);
        memcpy(wire, snap.wire_color, sizeof(float) * 4);
        memcpy(wire_vis, snap.wire_color_vis, sizeof(float) * 4);

        m_gpu_renderer.QueueDraw(&mesh, snap.bones, snap.bone_count, snap.bone_visibility,
            fill, fill_vis, wire, wire_vis,
            1.0f, 0, snap.material_type,
            dissolve_amount, dissolve_style, edge_width, noise_scale, edge_color, glow_strength);
    }
}
