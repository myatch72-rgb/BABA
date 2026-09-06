#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

class CViewSetup;
struct c_aggregate_object_array;
struct c_mesh_primitive_output_buffer;

namespace Hooks {
    void Setup();
    bool IsRenderReady();
    bool ActivateInventoryHooks();
    bool ActivateFullRuntimeHooks();
    bool IsFullRuntimeActive();
    void Destroy();

    inline HRESULT(__stdcall* oPresent)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) = nullptr;
    HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    void PresentInternal(IDXGISwapChain* swapChain, UINT sync, UINT flags);

    inline HRESULT(__stdcall* oResizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) = nullptr;
    HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    
    inline void(__stdcall* oDrawIndexed)(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) = nullptr;
    void __stdcall hkDrawIndexed(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
    
    inline void(__stdcall* oDrawIndexedInstanced)(ID3D11DeviceContext* pContext, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) = nullptr;
    void __stdcall hkDrawIndexedInstanced(ID3D11DeviceContext* pContext, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

    inline WNDPROC oWndProc = nullptr;
    LRESULT __stdcall hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    inline void(__fastcall* oCreateMove)(void*, unsigned int, __int64) = nullptr;
    void __fastcall hkCreateMove(void* pInput, unsigned int nSlot, __int64 a3);

    inline __int64(__fastcall* oDrawAggregateSceneObject)(void*, void*, c_aggregate_object_array*) = nullptr;
    __int64 __fastcall hkDrawAggregateSceneObject(void* a1, void* a2, c_aggregate_object_array* a3);

    inline void(__fastcall* oGeneratePrimitives)(void*, void*, void*, c_mesh_primitive_output_buffer*) = nullptr;
    void __fastcall hkGeneratePrimitives(void* __this, void* object, void* a3, c_mesh_primitive_output_buffer* render_buf);
    
    inline void(__fastcall* oSkyBoxObjectDrawArray)(std::int64_t, std::int64_t, std::int64_t, int, int, std::int64_t, std::int64_t) = nullptr;
    void __fastcall hkSkyBoxObjectDrawArray(std::int64_t this_ptr, std::int64_t render, std::int64_t primitive, int nCount, int renderFlags, std::int64_t viewInfo, std::int64_t renderStats);

    
    
    inline void(__fastcall* oFrameStageNotify)(void*, int) = nullptr;
    void __fastcall hkFrameStageNotify(void* _this, int curStage);

    inline void(__fastcall* oGetViewmodel)(void*, float*, float*) = nullptr;
    void __fastcall hkGetViewmodel(void* unk, float* viewmodelOffsets, float* viewmodelFov);

    inline __int64(__fastcall* oDrawScopeOverlay)(void*, void*) = nullptr;
    __int64 __fastcall hkDrawScopeOverlay(void* _this, void* a2);

    
    inline void*(__fastcall* oLightScene)(void*, void*, void*) = nullptr;
    void* __fastcall hkLightScene(void* rcx, void* object, void* r8);

    inline bool(__fastcall* SetPlayerReady)(void* thisptr, const char* szReadyType) = nullptr;

    inline void(__fastcall* oMatchFoundHandler)() = nullptr;
    void __fastcall hkMatchFoundHandler();

    inline __int64(__fastcall* oPanoramaEvent)(void* pUnk, const char* szEventName, void* pUnk1, float flUnk) = nullptr;
    __int64 __fastcall hkPanoramaEvent(void* pUnk, const char* szEventName, void* pUnk1, float flUnk);

    inline char(__fastcall* oSetInfo)(void* rcx, void* a2) = nullptr;
    char __fastcall hkSetInfo(void* rcx, void* a2);

    // No Gravity Ragdoll hooks the shared ConVar-value getter and filters it
    // to the exact ragdoll_gravity_scale physics-solver return site.

    // DrawTeamIntro (client.dll) — called every frame during the round
    // intro / team-select cinematic. We hook it to force-end the intro
    // period (`m_bTeamIntroPeriod = true`) so the user can move and shoot
    // immediately without waiting for the cinematic to finish.
    inline void(__fastcall* oDrawTeamIntro)(void* rules, __int64 a2, char* a3) = nullptr;
    void __fastcall hkDrawTeamIntro(void* rules, __int64 a2, char* a3);
}
