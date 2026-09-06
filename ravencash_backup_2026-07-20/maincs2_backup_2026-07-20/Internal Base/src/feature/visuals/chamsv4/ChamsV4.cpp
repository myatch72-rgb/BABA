#include "ChamsV4.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/memory/Patterns.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/entity/EntityManager.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#define _CRT_SECURE_NO_WARNINGS













namespace ChamsV4 {


static const char* SIG_DRAW_OBJECT = Patterns::SceneSystem::SIG_DRAW_OBJECT;


static constexpr const char* LOADKV3_PROC_ADDRESS = "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z";
static const char* CREATEMATERIAL_PATTERN = Patterns::MaterialSystem2::CreateMaterial;
static const char* SETTYPEKV3_PATTERN = Patterns::Client::SetTypeKV3;

struct KV3ID_t {
    const char* name;
    uint64_t unk0;
    uint64_t unk1;
};

struct ResourceBinding_t {
    void* m_pData;
};

template<typename T>
class CStrongHandle {
public:
    operator T* () const {
        if (!m_pBinding) return nullptr;
        return static_cast<T*>(m_pBinding->m_pData);
    }

    T* operator->() const {
        if (!m_pBinding) return nullptr;
        return static_cast<T*>(m_pBinding->m_pData);
    }

    const ResourceBinding_t* m_pBinding = nullptr;
};

class CMaterial2 {
public:
    const char* GetName() {
        using fn = const char* (__fastcall*)(void*);
        return (*reinterpret_cast<fn**>(this))[0](this);
    }
};

using LoadKV3_t = bool(__fastcall*)(void*, void*, const char*, const KV3ID_t*, const KV3ID_t*, unsigned int);
using CreateMaterial_t = void(__fastcall*)(void*, CStrongHandle<CMaterial2>*, const char*, void*, unsigned int, unsigned int);
using SetTypeKV3_t = void* (__fastcall*)(void*, unsigned int, unsigned int);

static LoadKV3_t g_fnLoadKV3 = nullptr;
static CreateMaterial_t g_fnCreateMaterial = nullptr;
static SetTypeKV3_t g_fnSetTypeKV3 = nullptr;
static void* g_pMaterialSystem = nullptr;

struct MaterialPair {
    CMaterial2* visible = nullptr;
    CMaterial2* hidden = nullptr;
};

static MaterialPair g_materials[CHAMSV4_MAT_MAX] = {};
static bool g_materialInitTried = false;
static bool g_tintCacheValid = false;
static float g_cachedVisibleTint[4] = {};
static float g_cachedHiddenTint[4] = {};



static constexpr const char* VMAT_FLAT_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    Shader = "csgo_unlitgeneric.vfx"
    F_IGNOREZ = 0
    F_DISABLE_Z_WRITE = 0
    F_DISABLE_Z_BUFFERING = 0
    F_BLEND_MODE = 1
    F_TRANSLUCENT = 1
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
    g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
    g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
    g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
    g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})";

static constexpr const char* VMAT_FLAT_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    Shader = "csgo_unlitgeneric.vfx"
    F_IGNOREZ = 1
    F_DISABLE_Z_WRITE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_BLEND_MODE = 1
    F_TRANSLUCENT = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 0.000000]
    g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
    g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
    g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
    g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
    g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})";

static constexpr const char* VMAT_ILLUMINATE_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_SELF_ILLUM = 1
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
    g_flSelfIllumScale = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
    g_flSelfIllumBrightness = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
    g_vSelfIllumTint = [ 10.000000, 10.000000, 10.000000, 10.000000 ]
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
    g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})";

static constexpr const char* VMAT_ILLUMINATE_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_SELF_ILLUM = 1
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_DISABLE_Z_BUFFERING = 1
    g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
    g_flSelfIllumScale = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
    g_flSelfIllumBrightness = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
    g_vSelfIllumTint = [ 10.000000, 10.000000, 10.000000, 10.000000 ]
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
    g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})";

static constexpr const char* VMAT_GLOW_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_flFresnelExponent = 7.0
    g_flFresnelFalloff = 10.0
    g_flFresnelMax = 0.1
    g_flFresnelMin = 1.0
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_flToolsVisCubemapReflectionRoughness = 1.0
    g_flBeginMixingRoughness = 1.0
    g_vColorTint = [ 1.000000, 1.000000, 1.000000, 0 ]
    F_IGNOREZ = 0
    F_TRANSLUCENT = 1
    F_DISABLE_Z_WRITE = 0
    F_DISABLE_Z_BUFFERING = 1
    F_RENDER_BACKFACES = 0
})";

static constexpr const char* VMAT_GLOW_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_flFresnelExponent = 7.0
    g_flFresnelFalloff = 10.0
    g_flFresnelMax = 0.1
    g_flFresnelMin = 1.0
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_flToolsVisCubemapReflectionRoughness = 1.0
    g_flBeginMixingRoughness = 1.0
    g_vColorTint = [ 1.000000, 1.000000, 1.000000, 0 ]
    F_IGNOREZ = 1
    F_TRANSLUCENT = 1
    F_DISABLE_Z_WRITE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_RENDER_BACKFACES = 0
})";

static constexpr const char* VMAT_GHOST_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_flOpacityScale = 0.45
    g_flFresnelExponent = 0.75
    g_flFresnelFalloff = 1
    g_flFresnelMax = 0.0
    g_flFresnelMin = 1
    F_ADDITIVE_BLEND = 1
    F_BLEND_MODE = 0
    F_TRANSLUCENT = 1
    F_IGNOREZ = 0
    F_DISABLE_Z_WRITE = 0
    F_DISABLE_Z_BUFFERING = 0
    F_RENDER_BACKFACES = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_GHOST_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_flOpacityScale = 0.45
    g_flFresnelExponent = 0.75
    g_flFresnelFalloff = 1
    g_flFresnelMax = 0.0
    g_flFresnelMin = 1
    F_ADDITIVE_BLEND = 1
    F_BLEND_MODE = 0
    F_TRANSLUCENT = 1
    F_IGNOREZ = 1
    F_DISABLE_Z_WRITE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_RENDER_BACKFACES = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_TEXTURED_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_flMetalness = 0.000000
    g_flRoughness = 0.250000
    g_flRoughnessScaleFactor = 0.400000
    g_flSpecularScale = 3.000000
    g_flSpecularExponent = 128.000000
    g_flOpacityScale = 1.000000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7be61377.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";

static constexpr const char* VMAT_TEXTURED_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_PREPASS = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_flMetalness = 0.000000
    g_flRoughness = 0.250000
    g_flRoughnessScaleFactor = 0.400000
    g_flSpecularScale = 3.000000
    g_flSpecularExponent = 128.000000
    g_flOpacityScale = 1.000000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7be61377.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";

static constexpr const char* VMAT_METALLIC_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_bFogEnabled = 0
    g_flMetalness = 1.000
    g_flModelTintAmount = 1.000
    g_nScaleTexCoordUByModelScaleAxis = 0
    g_nScaleTexCoordVByModelScaleAxis = 0
    g_nTextureAddressModeU = 0
    g_nTextureAddressModeV = 0
    g_flTexCoordRotation = 0.000
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
})";

static constexpr const char* VMAT_METALLIC_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_PREPASS = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    F_BLEND_MODE = 1
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_bFogEnabled = 0
    g_flMetalness = 1.000
    g_flModelTintAmount = 1.000
    g_nScaleTexCoordUByModelScaleAxis = 0
    g_nScaleTexCoordVByModelScaleAxis = 0
    g_nTextureAddressModeU = 0
    g_nTextureAddressModeV = 0
    g_flTexCoordRotation = 0.000
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
})";

static constexpr const char* VMAT_OVERLAY_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tRoughness = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_tMetalness = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_flColorBoost = 30
    g_flOpacityScale = 245.55
    g_flFresnelExponent = 7.75
    g_flFresnelFalloff = 5
    g_flFresnelMax = 0.0
    g_flFresnelMin = 9
    F_ADDITIVE_BLEND = 1
    F_BLEND_MODE = 1
    F_TRANSLUCENT = 1
    F_IGNOREZ = 0
    F_DISABLE_Z_WRITE = 0
    F_DISABLE_Z_BUFFERING = 0
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_OVERLAY_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tRoughness = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_tMetalness = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_normal_tga_b3f4ec4c.vtex"
    g_flColorBoost = 30
    g_flOpacityScale = 245.55
    g_flFresnelExponent = 7.75
    g_flFresnelFalloff = 5
    g_flFresnelMax = 0.0
    g_flFresnelMin = 9
    F_ADDITIVE_BLEND = 1
    F_BLEND_MODE = 1
    F_TRANSLUCENT = 1
    F_IGNOREZ = 1
    F_DISABLE_Z_WRITE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_LATEX_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_character.vfx"
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_bFogEnabled = 0
    g_flMetalness = 0.350
    g_flRoughness = 0.450
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_79a2e0d0.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    g_tMetalness = resource:"materials/default/default_metal_tga_8fbc2820.vtex"
})";

static constexpr const char* VMAT_LATEX_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_character.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_PREPASS = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_bFogEnabled = 0
    g_flMetalness = 0.350
    g_flRoughness = 0.450
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_79a2e0d0.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    g_tMetalness = resource:"materials/default/default_metal_tga_8fbc2820.vtex"
})";

static constexpr const char* VMAT_WIREFRAME_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_unlitgeneric.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_WIREFRAME = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})";

static constexpr const char* VMAT_WIREFRAME_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_unlitgeneric.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_WIREFRAME = 1
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})";

static constexpr const char* VMAT_PEARLESCENT_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_flMetalness = 0.800000
    g_flRoughness = 0.050000
    g_flRoughnessScaleFactor = 0.100000
    g_flSpecularScale = 8.000000
    g_flSpecularExponent = 256.000000
    g_flOpacityScale = 1.000000
    g_flAnisotropy = 0.800000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";

static constexpr const char* VMAT_PEARLESCENT_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_PREPASS = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
    g_flMetalness = 0.800000
    g_flRoughness = 0.050000
    g_flRoughnessScaleFactor = 0.100000
    g_flSpecularScale = 8.000000
    g_flSpecularExponent = 256.000000
    g_flOpacityScale = 1.000000
    g_flAnisotropy = 0.800000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";

static constexpr const char* VMAT_GLASS_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_flOpacityScale = 0.15
    g_flFresnelExponent = 4.0
    g_flFresnelFalloff = 2
    g_flFresnelMax = 0.7
    g_flFresnelMin = 0.0
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 0
    F_IGNOREZ = 0
    F_DISABLE_Z_WRITE = 0
    F_DISABLE_Z_BUFFERING = 0
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_GLASS_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_effects.vfx"
    g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
    g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
    g_flOpacityScale = 0.15
    g_flFresnelExponent = 4.0
    g_flFresnelFalloff = 2
    g_flFresnelMax = 0.7
    g_flFresnelMin = 0.0
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 0
    F_IGNOREZ = 1
    F_DISABLE_Z_WRITE = 1
    F_DISABLE_Z_BUFFERING = 1
    F_RENDER_BACKFACES = 0
    g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
})";

static constexpr const char* VMAT_GOLD_VISIBLE =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    g_vColorTint = [1.000000, 0.764706, 0.321569, 1.000000]
    g_flMetalness = 1.000000
    g_flRoughness = 0.300000
    g_flRoughnessScaleFactor = 0.500000
    g_flSpecularScale = 4.000000
    g_flSpecularExponent = 128.000000
    g_flOpacityScale = 1.000000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";

static constexpr const char* VMAT_GOLD_HIDDEN =
R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_complex.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_SPECULAR = 1
    F_METALNESS_TEXTURE = 0
    F_DISABLE_Z_BUFFERING = 1
    F_DISABLE_Z_PREPASS = 1
    F_DISABLE_Z_WRITE = 1
    F_IGNOREZ = 1
    g_vColorTint = [1.000000, 0.764706, 0.321569, 1.000000]
    g_flMetalness = 1.000000
    g_flRoughness = 0.300000
    g_flRoughnessScaleFactor = 0.500000
    g_flSpecularScale = 4.000000
    g_flSpecularExponent = 128.000000
    g_flOpacityScale = 1.000000
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
    TextureAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})";


static float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static bool ColorArrayEqual(const float a[4], const float b[4])
{
    constexpr float eps = 0.0005f;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > eps)
            return false;
    }
    return true;
}

static bool HasTintChanged()
{
    if (!g_tintCacheValid)
        return true;

    return !ColorArrayEqual(g_cachedVisibleTint, Globals::chamsv4_visible_color) ||
        !ColorArrayEqual(g_cachedHiddenTint, Globals::chamsv4_hidden_color);
}

static void CacheCurrentTints()
{
    for (int i = 0; i < 4; ++i) {
        g_cachedVisibleTint[i] = Globals::chamsv4_visible_color[i];
        g_cachedHiddenTint[i] = Globals::chamsv4_hidden_color[i];
    }
    g_tintCacheValid = true;
}

static std::string BuildTintVector(const float color[4])
{
    char buffer[96] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "[%.6f, %.6f, %.6f, %.6f]",
        Clamp01(color[0]),
        Clamp01(color[1]),
        Clamp01(color[2]),
        Clamp01(color[3])
    );
    return std::string(buffer);
}

static std::string ApplyColorTintToVmat(const char* baseVmat, const float color[4])
{
    if (!baseVmat)
        return {};

    std::string content(baseVmat);
    const std::string tint = BuildTintVector(color);

    
    const char* colorKeys[] = { "g_vColorTint", "g_vOverrideColor" };
    for (const char* key : colorKeys) {
        size_t searchPos = 0;
        while (true) {
            const size_t keyPos = content.find(key, searchPos);
            if (keyPos == std::string::npos)
                break;

            const size_t bracketOpen = content.find('[', keyPos);
            if (bracketOpen == std::string::npos)
                break;

            const size_t bracketClose = content.find(']', bracketOpen);
            if (bracketClose == std::string::npos)
                break;

            content.replace(bracketOpen, bracketClose - bracketOpen + 1, tint);
            searchPos = bracketOpen + tint.size();
        }
    }

    return content;
}

static std::string BuildMaterialName(const char* baseName, const float color[4])
{
    unsigned int r = static_cast<unsigned int>(Clamp01(color[0]) * 255.0f);
    unsigned int g = static_cast<unsigned int>(Clamp01(color[1]) * 255.0f);
    unsigned int b = static_cast<unsigned int>(Clamp01(color[2]) * 255.0f);
    unsigned int a = static_cast<unsigned int>(Clamp01(color[3]) * 255.0f);

    char buffer[192] = {};
    std::snprintf(buffer, sizeof(buffer), "%s_%02X%02X%02X%02X.vmat", baseName, r, g, b, a);
    return std::string(buffer);
}



static CMaterial2* CreateMaterialFromKV3(const char* name, const char* finalContent)
{
    if (!g_fnLoadKV3 || !g_fnCreateMaterial || !g_fnSetTypeKV3 || !g_pMaterialSystem || !name || !finalContent)
        return nullptr;

    void* pKV3Raw = std::malloc(0x200);
    if (!pKV3Raw)
        return nullptr;
    std::memset(pKV3Raw, 0, 0x200);

    void* pKV3 = g_fnSetTypeKV3(pKV3Raw, 1U, 6U);
    if (!pKV3) {
        std::free(pKV3Raw);
        return nullptr;
    }

    KV3ID_t kv3_id = { "generic", 0x41B818518343427EULL, 0xB5F447C23C0CDF8CULL };
    if (!g_fnLoadKV3(pKV3, nullptr, finalContent, &kv3_id, nullptr, 0U)) {
        std::free(pKV3Raw);
        return nullptr;
    }

    CStrongHandle<CMaterial2> matHandle = {};
    g_fnCreateMaterial(g_pMaterialSystem, &matHandle, name, pKV3, 0U, 1U);

    std::free(pKV3Raw);

    if (matHandle.m_pBinding && matHandle.m_pBinding->m_pData) {
        return static_cast<CMaterial2*>(matHandle.m_pBinding->m_pData);
    }

    return nullptr;
}

static bool ResolveMaterialFunctions()
{
    if (g_fnLoadKV3 && g_fnCreateMaterial && g_fnSetTypeKV3 && g_pMaterialSystem) {
        return true;
    }

    HMODULE hTier0 = GetModuleHandleA("tier0.dll");
    if (!hTier0) {
        return false;
    }

    g_fnLoadKV3 = reinterpret_cast<LoadKV3_t>(GetProcAddress(hTier0, LOADKV3_PROC_ADDRESS));
    if (!g_fnLoadKV3) {
        return false;
    }

    const uintptr_t createMatAddr = Memory::PatternScan("materialsystem2.dll", CREATEMATERIAL_PATTERN);
    if (!createMatAddr) {
        return false;
    }
    g_fnCreateMaterial = reinterpret_cast<CreateMaterial_t>(createMatAddr);

    const uintptr_t setTypeAddr = Memory::PatternScan("client.dll", SETTYPEKV3_PATTERN);
    if (!setTypeAddr) {
        return false;
    }
    g_fnSetTypeKV3 = reinterpret_cast<SetTypeKV3_t>(setTypeAddr);

    if (!g_pMaterialSystem) {
        HMODULE hMat = GetModuleHandleA("materialsystem2.dll");
        if (!hMat) {
            return false;
        }

        using CreateInterface_t = void* (__cdecl*)(const char*, int*);
        auto fnCreateInterface = reinterpret_cast<CreateInterface_t>(GetProcAddress(hMat, "CreateInterface"));
        if (!fnCreateInterface) {
            return false;
        }

        g_pMaterialSystem = fnCreateInterface("VMaterialSystem2_001", nullptr);
    }

    return g_pMaterialSystem != nullptr;
}

static void EnsureMaterialsInitialized()
{
    bool hasAnyMaterials = false;
    for (int i = 0; i < CHAMSV4_MAT_MAX; ++i) {
        if (g_materials[i].visible || g_materials[i].hidden) {
            hasAnyMaterials = true;
            break;
        }
    }

    if (hasAnyMaterials && !HasTintChanged())
        return;

    if (!hasAnyMaterials && g_materialInitTried)
        return;

    if (!ResolveMaterialFunctions())
        return;

    struct MatDef {
        const char* name;
        const char* visibleVmat;
        const char* hiddenVmat;
    };

    const MatDef defs[CHAMSV4_MAT_MAX] = {
        { "flat",        VMAT_FLAT_VISIBLE,        VMAT_FLAT_HIDDEN },
        { "illuminate",  VMAT_ILLUMINATE_VISIBLE,  VMAT_ILLUMINATE_HIDDEN },
        { "glow",        VMAT_GLOW_VISIBLE,        VMAT_GLOW_HIDDEN },
        { "ghost",       VMAT_GHOST_VISIBLE,       VMAT_GHOST_HIDDEN },
        { "textured",    VMAT_TEXTURED_VISIBLE,    VMAT_TEXTURED_HIDDEN },
        { "metallic",    VMAT_METALLIC_VISIBLE,    VMAT_METALLIC_HIDDEN },
        { "overlay",     VMAT_OVERLAY_VISIBLE,     VMAT_OVERLAY_HIDDEN },
        { "latex",       VMAT_LATEX_VISIBLE,       VMAT_LATEX_HIDDEN },
        { "wireframe",   VMAT_WIREFRAME_VISIBLE,   VMAT_WIREFRAME_HIDDEN },
        { "pearlescent", VMAT_PEARLESCENT_VISIBLE, VMAT_PEARLESCENT_HIDDEN },
        { "glass",       VMAT_GLASS_VISIBLE,       VMAT_GLASS_HIDDEN },
        { "gold",        VMAT_GOLD_VISIBLE,        VMAT_GOLD_HIDDEN },
    };

    for (int i = 0; i < CHAMSV4_MAT_MAX; ++i) {
        char visName[128], hidName[128];
        std::snprintf(visName, sizeof(visName), "materials/dev/chamsv4_%s_visible", defs[i].name);
        std::snprintf(hidName, sizeof(hidName), "materials/dev/chamsv4_%s_hidden", defs[i].name);

        const std::string visContent = ApplyColorTintToVmat(defs[i].visibleVmat, Globals::chamsv4_visible_color);
        const std::string hidContent = ApplyColorTintToVmat(defs[i].hiddenVmat, Globals::chamsv4_hidden_color);

        g_materials[i].visible = CreateMaterialFromKV3(
            BuildMaterialName(visName, Globals::chamsv4_visible_color).c_str(),
            visContent.c_str()
        );
        g_materials[i].hidden = CreateMaterialFromKV3(
            BuildMaterialName(hidName, Globals::chamsv4_hidden_color).c_str(),
            hidContent.c_str()
        );
    }

    CacheCurrentTints();
}














static EChamsV4EntityType SafeClassifyMaterial(CMaterial2* pMat)
{
    if (!pMat)
        return CV4_ENTITY_UNKNOWN;

    __try {
        
        using fn_getName = const char* (__fastcall*)(void*);
        const char* name = (*reinterpret_cast<fn_getName**>(pMat))[0](pMat);
        if (!name || !name[0])
            return CV4_ENTITY_UNKNOWN;

        
        
        if (strstr(name, "glove") && !strstr(name, "v_glove"))
            return CV4_ENTITY_GLOVES;

        
        if (strstr(name, "sleeve") || strstr(name, "bare_arm") || strstr(name, "v_glove"))
            return CV4_ENTITY_HANDS;
        if (strstr(name, "v_model") && strstr(name, "arm"))
            return CV4_ENTITY_HANDS;

        
        if (strstr(name, "knife") || strstr(name, "bayonet") || strstr(name, "karambit") ||
            strstr(name, "butterfly") || strstr(name, "falchion") || strstr(name, "huntsman") ||
            strstr(name, "bowie") || strstr(name, "navaja") || strstr(name, "stiletto") ||
            strstr(name, "talon") || strstr(name, "ursus") || strstr(name, "skeleton") ||
            strstr(name, "paracord") || strstr(name, "survival") || strstr(name, "nomad") ||
            strstr(name, "kukri"))
            return CV4_ENTITY_KNIFE;

        
        if (strstr(name, "chicken"))
            return CV4_ENTITY_CHICKEN;

        
        if (strstr(name, "character") || strstr(name, "agent") ||
            strstr(name, "models/player"))
            return CV4_ENTITY_PLAYER;

        
        if (strstr(name, "weapon"))
            return CV4_ENTITY_WEAPON;

        return CV4_ENTITY_WORLD;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return CV4_ENTITY_UNKNOWN;
    }
}




















static uintptr_t SafeGetEntityPtrFromSceneObj(uintptr_t sceneObjAddr) noexcept
{
    if (sceneObjAddr < 0x10000) return 0;

    
    static const int kOffsets[] = { 0xC0, 0xB8, 0xC8, 0xB0, 0xD0, 0xA8, 0xA0, 0xD8, 0xE0 };
    for (int off : kOffsets) {
        uint32_t handle = Utils::SafeRead<uint32_t>(sceneObjAddr + off);
        if (handle == 0 || handle == 0xFFFFFFFF) continue;
        int idx = static_cast<int>(handle & 0x7FFF);
        if (idx <= 0 || idx >= 16384) continue;
        C_BaseEntity* ent = reinterpret_cast<C_BaseEntity*>(
            EntityManager::Get().GetEntityFromHandle(handle));
        if (ent && reinterpret_cast<uintptr_t>(ent) > 0x10000)
            return reinterpret_cast<uintptr_t>(ent);
    }
    return 0;
}


static bool IsAimbotTargetingActive() noexcept
{
    if (!Globals::aim_enabled) return false;
    return (Globals::aim_key == 0)
        || ((GetAsyncKeyState(Globals::aim_key) & 0x8000) != 0);
}



static bool IsLocalViewmodelArmMat(CMaterial2* pMat) noexcept
{
    if (!pMat) return false;
    __try {
        using fn_getName = const char* (__fastcall*)(void*);
        const char* name = (*reinterpret_cast<fn_getName**>(pMat))[0](pMat);
        if (!name) return false;
        return (strstr(name, "v_model") != nullptr)
            && (strstr(name, "arm")     != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ShouldApplyChams(EChamsV4EntityType type)
{
    switch (type) {
    case CV4_ENTITY_PLAYER:  return Globals::chamsv4_players || Globals::chamsv4_team;
    case CV4_ENTITY_HANDS:   return Globals::chamsv4_hands;
    case CV4_ENTITY_GLOVES:  return Globals::chamsv4_gloves;
    case CV4_ENTITY_WEAPON:  return Globals::chamsv4_weapons && Globals::chamsv4_weapons_guns;
    case CV4_ENTITY_KNIFE:   return Globals::chamsv4_weapons && Globals::chamsv4_weapons_knives;
    case CV4_ENTITY_CHICKEN: return Globals::chamsv4_chicken;
    default: return false;
    }
}



static void* hkDrawObject_Internal(
    __int64 a1,
    __int64 a2,
    __int64* a3,
    int a4,
    __int64 a5,
    __int64 a6,
    __int64 a7)
{
    // The current scene-system batch count is small.  Reject an implausible
    // value before treating a3 as an array; a bad hook ABI/target must never
    // turn into an out-of-bounds material rewrite.
    if (!Globals::chamsv4_enabled || !a3 || a4 < 1 || a4 > 4096) {
        return oDrawObject(a1, a2, a3, a4, a5, a6, a7);
    }

    EnsureMaterialsInitialized();

    auto* meshArray = reinterpret_cast<MeshDrawData*>(a3);

    
    
    
    
    
    
    
    
    
    
    
    
    const uintptr_t meshEntityAddr =
        SafeGetEntityPtrFromSceneObj(
            static_cast<uintptr_t>(meshArray[0].sceneAnimObject));

    C_CSPlayerPawn* localPawn = EntityManager::Get().GetLocalPawn();
    const uintptr_t localAddr = reinterpret_cast<uintptr_t>(localPawn);

    const bool isLocalPlayer  = (meshEntityAddr != 0 && localAddr != 0
                                 && meshEntityAddr == localAddr);
    const uint8_t meshTeam    = meshEntityAddr
        ? Utils::SafeRead<uint8_t>(meshEntityAddr + Offsets::m_iTeamNum)
        : static_cast<uint8_t>(0);
    
    
    
    
    
    const bool isOtherPlayer  = !isLocalPlayer
                                && (meshTeam == 2 || meshTeam == 3)
                                && EntityManager::Get().IsKnownPlayerPawn(meshEntityAddr);
    auto* firstMat = reinterpret_cast<CMaterial2*>(meshArray[0].materialPtr);
    const EChamsV4EntityType materialType = SafeClassifyMaterial(firstMat);

    
    
    
    switch (materialType) {
    case CV4_ENTITY_UNKNOWN:
    case CV4_ENTITY_CHICKEN:
    case CV4_ENTITY_WORLD:
        return oDrawObject(a1, a2, a3, a4, a5, a6, a7);
    default:
        break;
    }

    
    
    
    
    static const int kMatMap[] = {
        CHAMSV4_MAT_FLAT, CHAMSV4_MAT_ILLUMINATE, CHAMSV4_MAT_GLOW
    };
    
    auto clampIdx    = [](int v) { return v < 0 ? 0 : (v > 2 ? 2 : v); };
    
    auto clampAllIdx = [](int v) { return v < 0 ? 0 : (v >= CHAMSV4_MAT_MAX ? CHAMSV4_MAT_MAX - 1 : v); };

    int matType = CHAMSV4_MAT_FLAT;
    bool allowHiddenPass = true;

    
    
    
    
    if (materialType == CV4_ENTITY_KNIFE) {
        if (!(Globals::chamsv4_weapons && Globals::chamsv4_weapons_knives))
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        
        matType = clampAllIdx(Globals::chamsv4_mat_knives);
        allowHiddenPass = false; 

    } else if (isOtherPlayer) {
        
        
        
        const uint8_t localTeam = Utils::SafeRead<uint8_t>(localAddr + Offsets::m_iTeamNum);
        const bool    isTeammate = (localTeam != 0 && meshTeam == localTeam);

        if ( isTeammate && !Globals::chamsv4_team)    return oDrawObject(a1, a2, a3, a4, a5, a6, a7);
        if (!isTeammate && !Globals::chamsv4_players) return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        matType = clampAllIdx(isTeammate ? Globals::chamsv4_mat_team
                                         : Globals::chamsv4_mat_players);

    } else if (materialType == CV4_ENTITY_HANDS) {
        
        if (!Globals::chamsv4_hands)
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        if (IsAimbotTargetingActive() && IsLocalViewmodelArmMat(firstMat))
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        
        matType = clampAllIdx(Globals::chamsv4_mat_hands);
        allowHiddenPass = false; 

    } else if (materialType == CV4_ENTITY_GLOVES) {
        
        if (!Globals::chamsv4_gloves)
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        
        matType = clampAllIdx(Globals::chamsv4_mat_gloves);
        allowHiddenPass = false; 

    } else if (materialType == CV4_ENTITY_WEAPON) {
        
        if (!(Globals::chamsv4_weapons && Globals::chamsv4_weapons_guns))
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        
        matType = clampAllIdx(Globals::chamsv4_mat_guns);
        allowHiddenPass = false; 

    } else {
        
        return oDrawObject(a1, a2, a3, a4, a5, a6, a7);
    }
    if (matType < 0 || matType >= CHAMSV4_MAT_MAX)
        matType = CHAMSV4_MAT_FLAT;

    const MaterialPair& selectedMat = g_materials[matType];
    const bool hasVisibleMat = (selectedMat.visible != nullptr);
    const bool hasHiddenMat  = (selectedMat.hidden  != nullptr);

    
    if (!hasVisibleMat && !hasHiddenMat)
        return oDrawObject(a1, a2, a3, a4, a5, a6, a7);

    
    struct SavedMeshState {
        MeshDrawData* mesh;
        __int64 material;
        __int64 material2;
    };
    std::vector<SavedMeshState> saved;
    saved.reserve(static_cast<size_t>(a4));
    for (int i = 0; i < a4; i++) {
        MeshDrawData* mesh = &meshArray[i];
        saved.push_back({ mesh, mesh->materialPtr, mesh->materialPtr2 });
    }

    
    if (allowHiddenPass && Globals::chamsv4_wallhack && hasHiddenMat) {
        for (const auto& state : saved) {
            state.mesh->materialPtr  = reinterpret_cast<__int64>(selectedMat.hidden);
            state.mesh->materialPtr2 = reinterpret_cast<__int64>(selectedMat.hidden);
        }
        oDrawObject(a1, a2, a3, a4, a5, a6, a7);

        
        
        
        
        for (const auto& state : saved) {
            state.mesh->materialPtr  = state.material;
            state.mesh->materialPtr2 = state.material2;
        }
    }

    
    
    
    if (hasVisibleMat) {
        for (const auto& state : saved) {
            state.mesh->materialPtr  = reinterpret_cast<__int64>(selectedMat.visible);
            state.mesh->materialPtr2 = reinterpret_cast<__int64>(selectedMat.visible);
        }
    }
    

    void* result = oDrawObject(a1, a2, a3, a4, a5, a6, a7);

    
    for (const auto& state : saved) {
        state.mesh->materialPtr  = state.material;
        state.mesh->materialPtr2 = state.material2;
    }

    return result;
}

void* __fastcall hkDrawObject_Trampoline(
    __int64 a1,
    __int64 a2,
    __int64* a3,
    int a4,
    __int64 a5,
    __int64 a6,
    __int64 a7)
{
    if (!oDrawObject) {
        return nullptr;
    }

    __try {
        return hkDrawObject_Internal(a1, a2, a3, a4, a5, a6, a7);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (oDrawObject)
            return oDrawObject(a1, a2, a3, a4, a5, a6, a7);
        return nullptr;
    }
}



void Initialize()
{
    
    g_DrawObjectAddr = Memory::PatternScan("scenesystem.dll", SIG_DRAW_OBJECT);

    EnsureMaterialsInitialized();
}

void Shutdown()
{
    g_DrawObjectAddr = 0;
    oDrawObject = nullptr;
    for (int i = 0; i < CHAMSV4_MAT_MAX; ++i) {
        g_materials[i].visible = nullptr;
        g_materials[i].hidden = nullptr;
    }
    g_fnLoadKV3 = nullptr;
    g_fnCreateMaterial = nullptr;
    g_fnSetTypeKV3 = nullptr;
    g_pMaterialSystem = nullptr;
    g_materialInitTried = false;
    g_tintCacheValid = false;
}

} 
