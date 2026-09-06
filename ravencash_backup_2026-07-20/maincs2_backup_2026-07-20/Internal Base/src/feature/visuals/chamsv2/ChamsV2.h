#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <d3dcompiler.h>
#include <vector>
#include <array>
#include <string>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")


#include "../../../sdk/utils/Vector.h"
#include "../../../sdk/entity/Classes.h"







namespace ChamsV2
{
    
    struct BoneMatrix3x4
    {
        float m[3][4];

        Vector TransformPoint(const Vector& p) const {
            return {
                p.x * m[0][0] + p.y * m[0][1] + p.z * m[0][2] + m[0][3],
                p.x * m[1][0] + p.y * m[1][1] + p.z * m[1][2] + m[1][3],
                p.x * m[2][0] + p.y * m[2][1] + p.z * m[2][2] + m[2][3]
            };
        }

        Vector GetOrigin() const {
            return { m[0][3], m[1][3], m[2][3] };
        }

        BoneMatrix3x4 Multiply(const BoneMatrix3x4& other) const {
            BoneMatrix3x4 out{};
            for (int i = 0; i < 3; i++) {
                out.m[i][0] = m[i][0] * other.m[0][0] + m[i][1] * other.m[1][0] + m[i][2] * other.m[2][0];
                out.m[i][1] = m[i][0] * other.m[0][1] + m[i][1] * other.m[1][1] + m[i][2] * other.m[2][1];
                out.m[i][2] = m[i][0] * other.m[0][2] + m[i][1] * other.m[1][2] + m[i][2] * other.m[2][2];
                out.m[i][3] = m[i][0] * other.m[0][3] + m[i][1] * other.m[1][3] + m[i][2] * other.m[2][3] + m[i][3];
            }
            return out;
        }

        void To4x4(float out[16]) const {
            out[0]  = m[0][0]; out[1]  = m[0][1]; out[2]  = m[0][2]; out[3]  = m[0][3];
            out[4]  = m[1][0]; out[5]  = m[1][1]; out[6]  = m[1][2]; out[7]  = m[1][3];
            out[8]  = m[2][0]; out[9]  = m[2][1]; out[10] = m[2][2]; out[11] = m[2][3];
            out[12] = 0.f;     out[13] = 0.f;     out[14] = 0.f;     out[15] = 1.f;
        }

        bool Valid() const {
            return std::isfinite(m[0][3]) && std::isfinite(m[1][3]) && std::isfinite(m[2][3])
                && std::abs(m[0][3]) < 50000.f && std::abs(m[1][3]) < 50000.f && std::abs(m[2][3]) < 50000.f;
        }

        static BoneMatrix3x4 Identity() {
            BoneMatrix3x4 out{};
            out.m[0][0] = 1.f; out.m[1][1] = 1.f; out.m[2][2] = 1.f;
            return out;
        }
    };

    
    struct MeshVertex
    {
        Vector position;
        Vector normal;
        std::array<uint16_t, 4> joint_indices;
        std::array<float, 4> weights;
    };

    
    struct SkinnedMesh
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<BoneMatrix3x4> inverse_bind_matrices;
        std::vector<int16_t> gltf_to_game_bone_map;

        ID3D11Buffer* gpu_vertex_buffer{ nullptr };
        ID3D11Buffer* gpu_index_buffer{ nullptr };
        uint32_t index_count{ 0 };
        uint32_t vertex_count{ 0 };

        bool Loaded() const { return !vertices.empty() && !indices.empty(); }

        void Release() {
            if (gpu_vertex_buffer) { gpu_vertex_buffer->Release(); gpu_vertex_buffer = nullptr; }
            if (gpu_index_buffer) { gpu_index_buffer->Release(); gpu_index_buffer = nullptr; }
        }
    };

    
    struct GPUVertex
    {
        float position[3];
        float normal[3];
        uint32_t bone_indices;
        float weights[4];
    };

    
    constexpr int MAX_BONES = 128;
    constexpr int MAX_DRAW_COMMANDS = 64; 

    struct CBViewProjection {
        float vp[4][4];
        float screen_w;
        float screen_h;
        float pad[2];
    };

    struct CBBoneMatrices {
        float bones[MAX_BONES][16];
    };

    
    struct CBBoneVisibility {
        float visibility[MAX_BONES];  
    };

    struct CBMaterial {
        float color[4];           
        float color_vis[4];       
        float wire_color[4];      
        float wire_color_vis[4];  
        int   render_mode;
        float alpha;
        int   material_type;
        float rim_power;
        float cam_pos[3];
        float time;
        
        float dissolve_amount;    
        int   dissolve_style;     
        float edge_width;
        float noise_scale;
        float edge_color[3];
        float glow_strength;
    };

    
    struct DrawCommand {
        SkinnedMesh* mesh;
        BoneMatrix3x4 combined_matrices[MAX_BONES]; 
        float bone_visibility[MAX_BONES];            
        int combined_count;                          
        float fill_color[4];
        float fill_color_vis[4];
        float wire_color[4];
        float wire_color_vis[4];
        float alpha;
        int render_mode;
        int material_type;
        
        float dissolve_amount;
        int   dissolve_style;
        float edge_width;
        float noise_scale;
        float edge_color[3];
        float glow_strength;
    };

    
    class RenderStateGuard {
    public:
        explicit RenderStateGuard(ID3D11DeviceContext* ctx);
        ~RenderStateGuard();
    private:
        ID3D11DeviceContext* m_ctx;
        D3D11_PRIMITIVE_TOPOLOGY m_topology;
        ID3D11InputLayout* m_input_layout;
        ID3D11VertexShader* m_vs;
        ID3D11PixelShader* m_ps;
        ID3D11Buffer* m_vs_cbs[4];  
        ID3D11Buffer* m_ps_cbs[2];  
        ID3D11Buffer* m_vb;
        UINT m_vb_stride, m_vb_offset;
        ID3D11Buffer* m_ib;
        DXGI_FORMAT m_ib_format;
        UINT m_ib_offset;
        ID3D11RasterizerState* m_rs;
        D3D11_VIEWPORT m_viewport;
        UINT m_num_viewports;
        ID3D11BlendState* m_bs;
        float m_blend_factor[4];
        UINT m_sample_mask;
        ID3D11DepthStencilState* m_dss;
        UINT m_stencil_ref;
        ID3D11RenderTargetView* m_rtv;
        ID3D11DepthStencilView* m_dsv;
    };

    
    class MeshLoader {
    public:
        static bool LoadGLB(const std::string& path, SkinnedMesh& mesh);
    private:
        static BoneMatrix3x4 ConvertIBM(const float* col_major_4x4);
        static void BuildBoneMapping(const std::vector<std::string>& joint_names, std::vector<int16_t>& mapping);
    };

    
    class GPURenderer {
    public:
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        bool UploadMesh(SkinnedMesh& mesh);
        void QueueDraw(SkinnedMesh* mesh, const BoneMatrix3x4* game_bones, int game_bone_count,
                       const float* bone_visibility,
                       float fill_color[4], float fill_color_vis[4],
                       float wire_color[4], float wire_color_vis[4],
                       float alpha, int render_mode, int material_type,
                       float dissolve_amount = 0.0f, int dissolve_style = 0,
                       float edge_width = 0.08f, float noise_scale = 0.15f,
                       const float* edge_color_rgb = nullptr, float glow_strength = 1.0f);
        void Flush();

    private:
        bool CreateShaders();
        bool CreateConstantBuffers();
        bool CreatePipelineStates();

        void UpdateViewProjection();
        void UpdateBoneMatrices(const BoneMatrix3x4* combined, int count);
        void UpdateBoneVisibility(const float* visibility);
        void UpdateMaterial(float fill[4], float fill_vis[4], float wire[4], float wire_vis[4], 
                           float alpha, int mode, int material_type,
                           float dissolve_amount = 0.0f, int dissolve_style = 0,
                           float edge_width = 0.08f, float noise_scale = 0.15f,
                           const float* edge_color_rgb = nullptr, float glow_strength = 1.0f);

        ID3D11Device* m_device{ nullptr };
        ID3D11DeviceContext* m_context{ nullptr };

        ID3D11VertexShader* m_vertex_shader{ nullptr };
        ID3D11PixelShader* m_pixel_shader_fill{ nullptr };
        ID3D11PixelShader* m_pixel_shader_wire{ nullptr };
        ID3D11InputLayout* m_input_layout{ nullptr };

        ID3D11Buffer* m_cb_view_projection{ nullptr };
        ID3D11Buffer* m_cb_bones{ nullptr };
        ID3D11Buffer* m_cb_bone_visibility{ nullptr };
        ID3D11Buffer* m_cb_material{ nullptr };

        ID3D11RasterizerState* m_rs_fill{ nullptr };
        ID3D11RasterizerState* m_rs_wireframe{ nullptr };
        ID3D11BlendState* m_blend_state{ nullptr };
        ID3D11DepthStencilState* m_depth_state{ nullptr };

        bool m_initialized{ false };
    public:
        DrawCommand m_pending_draws[MAX_DRAW_COMMANDS]; 
        int m_draw_count{ 0 };
    };

    
    struct PawnCache {
        BoneMatrix3x4 prev_bones[MAX_BONES];  
        float cached_visibility[MAX_BONES];   
        int bone_count{ 0 };
        int8_t cached_team{ -1 };  
        int cached_game_team{ -1 };
        bool alive_initialized{ false };
        bool was_alive{ false };
        Vector last_good_pelvis{};
        bool has_last_good_pelvis{ false };
        bool visibility_valid{ false };
        std::chrono::steady_clock::time_point last_visibility_update{};
        std::chrono::steady_clock::time_point last_model_check{};
        std::chrono::steady_clock::time_point last_seen{}; 
    };

    
    struct DissolveSnapshot {
        BoneMatrix3x4 bones[MAX_BONES];
        float bone_visibility[MAX_BONES];
        int bone_count;
        bool is_terrorist;
        bool is_enemy;
        int material_type;
        float fill_color[4];
        float fill_color_vis[4];
        float wire_color[4];
        float wire_color_vis[4];
    };

    
    class MeshRenderer {
    public:
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                        const std::string& ct_mesh_path, const std::string& t_mesh_path);
        void Shutdown();

        void RenderPlayer(C_CSPlayerController* controller, C_CSPlayerPawn* pawn, float esp_alpha, bool isEnemy);
        void Flush();
        bool IsReady() const { return m_ready; }

        
        bool CreateSnapshot(C_CSPlayerPawn* pawn, bool isEnemy, DissolveSnapshot& out);
        void RenderSnapshot(const DissolveSnapshot& snap, float dissolve_amount, int dissolve_style,
                           float edge_width, float noise_scale, const float* edge_color, float glow_strength);

    private:
        int ReadGameBones(C_CSPlayerPawn* pawn, const PawnCache& cache, int bone_count, BoneMatrix3x4* out_bones);
        bool IsModelTerrorist(C_CSPlayerPawn* pawn) const;
        void CleanupStaleEntries();
        void UpdateGamePhaseModelSwapState();
        int GetDetectedMeshTeam(bool modelIsTerrorist, int gameTeamNow) const;

        SkinnedMesh m_mesh_ct;
        SkinnedMesh m_mesh_t;
        GPURenderer m_gpu_renderer;
        bool m_ready{ false };

        
        std::unordered_map<uintptr_t, PawnCache> m_pawn_cache;
        std::chrono::steady_clock::time_point m_last_cleanup{};
        int m_last_game_phase{ -1 };
        int m_last_rounds_played_this_phase{ -1 };
        bool m_halftime_model_swap{ false };

        
        static constexpr int VISIBILITY_UPDATE_INTERVAL_MS = 100;
        
        static constexpr int CACHE_CLEANUP_INTERVAL_MS = 5000;
        
        static constexpr int CACHE_STALE_THRESHOLD_MS = 10000;
        
        static constexpr int MODEL_RECHECK_INTERVAL_MS = 1500;
    };

    
    inline MeshRenderer g_MeshRenderer;
}
