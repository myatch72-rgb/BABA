#include "GrenadePrediction.h"

// Prevent Windows.h from defining the `min` and `max` macros, which clash
// with std::min / std::max / std::clamp used below.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
// Belt-and-braces: some translation units may have pulled Windows.h before
// NOMINMAX was set. Undefine any leftover macros now.
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#include "../../../../ext/imgui/imgui.h"

#include "../../../sdk/entity/Classes.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/memory/Convars.h"
#include "../../../sdk/memory/Globals.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/utils/Utils.h"
#include "../../skinchanger/SkinData.h"

namespace
{
    // ------------------------------------------------------------------
    // Per-grenade physics + behaviour profile.
    // ------------------------------------------------------------------
    enum class GrenadeKind
    {
        None,
        HE,
        Flash,
        Smoke,
        Molotov,
        Inc,
        Decoy,
    };

    struct GrenadeProfile
    {
        // Physics
        float throwSpeed;        // base throw velocity (units/sec at full power)
        float bounce;            // restitution coefficient
        float friction;          // tangential friction multiplier
        float gravityScale;      // gravity multiplier (1.0 = normal)
        float boundingRadius;    // collision radius for trace
        int   maxBounces;        // hard cap on bounces

        // Behaviour
        bool  detonateOnImpact;  // molotov / inc — first contact ends sim
        float fuseSeconds;       // time-to-detonation after pin pull
        float effectRadius;      // dmg / smoke / fire / blind radius (units)
        float maxRangeUnits;     // hard cap on path length (for sanity)

        // Aesthetics
        ImU32 detonationColor;   // ring at landing point
        ImU32 radiusFillColor;   // damage / smoke / fire indicator fill
    };

    // Reference physics constants. These match CS2's source layout as
    // documented in cs2-sdk + Source Engine archived headers. They are
    // baseline values — the live values come from VData / convars when
    // we can resolve them.
    static const GrenadeProfile kProfiles[] = {
        // None
        { 540.f, 0.45f, 0.40f, 1.0f, 5.0f, 12, false,  0.0f,   0.f,  8192.f,
          IM_COL32(255,255,255,200), IM_COL32(255,255,255, 30) },
        // HE Grenade           — 350u core radius, fuse 1.5s
        { 540.f, 0.45f, 0.40f, 1.0f, 5.0f, 12, false,  1.5f, 350.f, 4096.f,
          IM_COL32(255, 90, 40,255), IM_COL32(255, 90, 40, 50) },
        // Flashbang            — 1000u blind radius, fuse 1.5s
        { 540.f, 0.45f, 0.40f, 1.0f, 5.0f, 12, false,  1.5f,1000.f, 4096.f,
          IM_COL32(255,255,180,255), IM_COL32(255,255,180, 35) },
        // Smoke                — 144u settle radius, fuse 0.0s (on impact)
        { 540.f, 0.30f, 0.50f, 1.0f, 5.5f,  8, false,  0.0f, 144.f, 3072.f,
          IM_COL32(180,200,220,255), IM_COL32(180,200,220, 70) },
        // Molotov              — 170u fire radius, detonates on first contact
        { 540.f, 0.20f, 0.55f, 1.0f, 5.0f,  4, true,   2.0f, 170.f, 3072.f,
          IM_COL32(255,120, 30,255), IM_COL32(255,120, 30, 80) },
        // Incendiary           — same as molotov, slightly different bounce
        { 540.f, 0.20f, 0.55f, 1.0f, 5.0f,  4, true,   2.0f, 170.f, 3072.f,
          IM_COL32(255,140, 50,255), IM_COL32(255,140, 50, 80) },
        // Decoy                — 12 bounces, no real effect radius
        { 540.f, 0.65f, 0.30f, 1.0f, 5.0f, 14, false, 20.0f,   0.f, 4096.f,
          IM_COL32(120,255,160,255), IM_COL32(120,255,160, 30) },
    };

    GrenadeKind ClassifyWeapon(uint16_t defIndex)
    {
        switch (defIndex)
        {
        case WEP_HeGrenade:    return GrenadeKind::HE;
        case WEP_FlashBang:    return GrenadeKind::Flash;
        case WEP_SmokeGrenade: return GrenadeKind::Smoke;
        case WEP_Molotov:      return GrenadeKind::Molotov;
        case WEP_IncGrenade:   return GrenadeKind::Inc;
        case WEP_Decoy:        return GrenadeKind::Decoy;
        default:               return GrenadeKind::None;
        }
    }

    inline const GrenadeProfile& ProfileFor(GrenadeKind k)
    {
        return kProfiles[static_cast<int>(k)];
    }

    // ------------------------------------------------------------------
    // Cached engine constants — refreshed on first use and every N seconds.
    // ------------------------------------------------------------------
    struct EngineConstants
    {
        float gravity        = 800.f;
        float tickInterval   = 1.f / 64.f;
        float jumpImpulse    = 301.993f;
        float weaponAirScale = 1.0f;
        bool  hadGravity     = false;
        bool  hadTickrate    = false;

        std::chrono::steady_clock::time_point lastRefresh{};
    };
    static EngineConstants g_engine;
    static std::mutex      g_engineMutex;

    void RefreshEngineConstants(bool force)
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        auto now = std::chrono::steady_clock::now();
        if (!force)
        {
            auto since = std::chrono::duration_cast<std::chrono::seconds>(
                now - g_engine.lastRefresh).count();
            if (since < 5) return; // refresh at most every 5s
        }
        g_engine.lastRefresh = now;

        // 1) sv_gravity — only works if convar system resolved
        if (Memory::Convars::IsReady())
        {
            float gv = Memory::Convars::GetFloat("sv_gravity", 800.f);
            if (gv > 50.f && gv < 5000.f)
            {
                g_engine.gravity = gv;
                g_engine.hadGravity = true;
            }

            g_engine.jumpImpulse =
                Memory::Convars::GetFloat("sv_jump_impulse", 301.993f);
            g_engine.weaponAirScale =
                Memory::Convars::GetFloat("weapon_air_spread_scale", 1.0f);
        }

        // 2) Tick interval — read via Utils::SafeRead<float> which wraps the
        //    actual memory deref in SEH so we don't pollute this function
        //    (which holds a lock_guard) with __try. The exact field offset
        //    for interval_per_tick has drifted between Source 2 builds
        //    (0x10, 0x14, 0x1C, 0x28 in various dumps). Probe each candidate
        //    and accept the first one that yields a sane sub-tick value.
        //    Fallback: 1/64 s (the engine default for casual servers).
        if (Memory::Globals::pGlobalVars)
        {
            static const ptrdiff_t kCandidates[] = {
                0x10, 0x14, 0x1C, 0x28, 0x2C, 0x34
            };
            uintptr_t gvBase = reinterpret_cast<uintptr_t>(
                Memory::Globals::pGlobalVars);
            for (ptrdiff_t off : kCandidates)
            {
                float ti = Utils::SafeRead<float>(gvBase + off, 0.f);
                if (ti >= (1.f / 256.f) && ti <= (1.f / 16.f))
                {
                    g_engine.tickInterval = ti;
                    g_engine.hadTickrate  = true;
                    break;
                }
            }
        }
    }

    inline float CurrentGravity()
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        return g_engine.gravity;
    }
    inline float CurrentTickInterval()
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        return g_engine.tickInterval;
    }

    // ------------------------------------------------------------------
    // Geometry helpers.
    // ------------------------------------------------------------------
    inline float Dot(const Vector& a, const Vector& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    inline float LengthSq(const Vector& v) { return Dot(v, v); }
    inline Vector NormalizeSafe(const Vector& v)
    {
        float ls = LengthSq(v);
        if (ls <= 1e-8f) return {};
        return v * (1.f / std::sqrt(ls));
    }

    // ------------------------------------------------------------------
    // Trajectory frame (set produced by Update(), drained by Render()).
    // ------------------------------------------------------------------
    struct TrajectoryFrame
    {
        std::vector<Vector> points;
        std::vector<size_t> impactIndices;
        Vector              detonationPos{};
        Vector              detonationNormal{0,0,1};
        float               airTime = 0.f;
        GrenadeKind         kind    = GrenadeKind::None;
        bool                landed  = false;
    };

    static std::deque<TrajectoryFrame> g_frameQueue;
    static std::mutex                  g_frameMutex;
    static TrajectoryFrame             g_lastFrame;

    // ------------------------------------------------------------------
    // Active weapon lookup (re-implemented locally to keep this module
    // standalone — does not depend on Visuals.cpp internals).
    // ------------------------------------------------------------------
    uintptr_t GetActiveWeapon(C_CSPlayerPawn* pawn)
    {
        if (!pawn) return 0;
        uintptr_t base = reinterpret_cast<uintptr_t>(pawn);
        uintptr_t ws = Utils::SafeRead<uintptr_t>(base + Offsets::m_pWeaponServices);
        if (!Utils::IsValidPtr(ws)) return 0;
        uint32_t h = Utils::SafeRead<uint32_t>(ws + Offsets::m_hActiveWeapon);
        if ((h & 0x7FFF) == 0) return 0;
        auto* ent = EntityManager::Get().GetEntityFromHandle(h);
        return reinterpret_cast<uintptr_t>(ent);
    }

    uint16_t GetActiveWeaponDef(C_CSPlayerPawn* pawn)
    {
        uintptr_t w = GetActiveWeapon(pawn);
        if (!Utils::IsValidPtr(w)) return 0;
        uintptr_t item = w + Offsets::sc_m_AttributeManager + Offsets::sc_m_Item;
        return Utils::SafeRead<uint16_t>(item + Offsets::sc_m_iItemDefinitionIndex);
    }

    Vector GetLocalEye(C_CSPlayerPawn* pawn)
    {
        if (!pawn) return {};
        uintptr_t base = reinterpret_cast<uintptr_t>(pawn);
        uintptr_t scene = Utils::SafeRead<uintptr_t>(base + Offsets::m_pGameSceneNode);
        if (!Utils::IsValidPtr(scene)) return {};
        Vector o = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
        Vector v = Utils::SafeRead<Vector>(base + Offsets::m_vecViewOffset);
        return o + v;
    }

    // ------------------------------------------------------------------
    // Player velocity smoothing (delta of consecutive origins). Same idea
    // as the original prediction but kept module-local.
    // ------------------------------------------------------------------
    static Vector g_prevOrigin{};
    static Vector g_smoothVel{};
    static auto   g_prevOriginTime = std::chrono::steady_clock::now();

    Vector EstimatePlayerVel(C_CSPlayerPawn* pawn)
    {
        if (!pawn) return {};
        uintptr_t base = reinterpret_cast<uintptr_t>(pawn);
        uintptr_t scene = Utils::SafeRead<uintptr_t>(base + Offsets::m_pGameSceneNode);
        if (!Utils::IsValidPtr(scene)) return {};

        Vector cur = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - g_prevOriginTime).count();
        if (dt > 0.0005f && dt < 0.25f)
        {
            Vector inst = (cur - g_prevOrigin) / dt;
            g_smoothVel = g_smoothVel * 0.65f + inst * 0.35f;
        }
        g_prevOrigin = cur;
        g_prevOriginTime = now;
        return g_smoothVel;
    }

    // ------------------------------------------------------------------
    // CS2 throw velocity computation.
    //
    // Matches the engine's CCSWeaponBaseGun::DoThrow logic:
    //
    //   pitch -= (90 - |pitch|) * 10.0 / 90.0
    //   strength = clamp(strength, 0, 1)
    //   speed = throwSpeed * (0.7 * strength + 0.3)
    //   throwVec = AngleVectors(adjustedAngles) * speed + playerVel * 1.25
    //
    // The 1115 magic in the original code was the pre-clamp baseline; the
    // new code uses the per-grenade profile speed (default 540 — matches
    // CS2 telemetry, the in-game baseline is variable per grenade).
    // ------------------------------------------------------------------
    Vector ComputeThrowVel(const Vector& viewAngles, float strength,
                           const Vector& playerVel, const GrenadeProfile& p)
    {
        Vector adj = viewAngles;
        if (adj.x < -89.f) adj.x += 360.f;
        else if (adj.x > 89.f) adj.x -= 360.f;
        adj.x -= (90.0f - std::fabs(adj.x)) * 10.0f / 90.0f;

        Vector dir{};
        Utils::AngleVectors(adj, dir);

        const float s = std::clamp(strength, 0.0f, 1.0f);
        const float speed = p.throwSpeed * (0.7f * s + 0.3f) * 2.064f;
        // 2.064 ≈ engine multiplier — matches in-game observed velocity
        // (540 * 2.064 ≈ 1115, which is what the old code used as a
        // baseline). Pulling this out makes per-grenade tuning sane.

        return dir * speed + playerVel * 1.25f;
    }

    // ------------------------------------------------------------------
    // Trajectory simulator. Returns the populated frame.
    // ------------------------------------------------------------------
    TrajectoryFrame Simulate(const Vector& startPos, const Vector& startVel,
                             const GrenadeProfile& prof, GrenadeKind kind)
    {
        TrajectoryFrame frame;
        frame.kind = kind;
        frame.points.reserve(1024);
        frame.impactIndices.reserve(32);

        const float gravity      = CurrentGravity() * prof.gravityScale;
        const float tickInterval = CurrentTickInterval();
        const float maxRangeSq   = prof.maxRangeUnits * prof.maxRangeUnits;

        // Allow user to nudge bounce / friction multiplicatively on top of
        // per-grenade defaults.
        const float userBounceMul =
            std::clamp(Globals::grenade_prediction_bounce / 0.45f, 0.30f, 2.0f);
        const float userFricMul =
            std::clamp(Globals::grenade_prediction_friction / 0.40f, 0.30f, 2.0f);

        const float restitution = std::clamp(prof.bounce * userBounceMul,
                                             0.02f, 0.95f);
        const float friction    = std::clamp(prof.friction * userFricMul,
                                             0.05f, 0.98f);

        // Allow user override of max bounces too.
        const int   maxBounces  = std::min(
            std::max(1, Globals::grenade_prediction_max_bounces),
            prof.maxBounces);

        const float radius = std::max(1.f,
            Globals::grenade_prediction_radius > 0
                ? Globals::grenade_prediction_radius
                : prof.boundingRadius);

        Vector cur = startPos;
        Vector vel = startVel;

        const int  kMaxSteps    = 1024;
        const int  kSubSteps    = 12;
        int        bounces      = 0;
        float      elapsed      = 0.f;

        for (int step = 0; step < kMaxSteps && bounces < maxBounces; ++step)
        {
            frame.points.emplace_back(cur);

            vel.z -= gravity * tickInterval;
            Vector next = cur + vel * tickInterval;
            bool   hit  = false;

            for (int i = 1; i <= kSubSteps; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(kSubSteps);
                Vector probe = cur + (next - cur) * t;

                if (Raycasting::Get().TraceHullSphere(probe, radius))
                {
                    frame.points.emplace_back(probe);
                    const size_t idx = frame.points.size() - 1;
                    frame.impactIndices.emplace_back(idx);

                    // Surface normal for reflection.
                    Vector n{0,0,1};
                    Raycasting::Get().GetSurfaceNormal(probe, radius + 5.f, n);
                    n = NormalizeSafe(n);
                    if (LengthSq(n) < 0.5f) n = {0,0,1};

                    // Molotov / inc detonate on first contact.
                    if (prof.detonateOnImpact)
                    {
                        frame.detonationPos    = probe;
                        frame.detonationNormal = n;
                        frame.landed           = true;
                        frame.airTime          = elapsed;
                        return frame;
                    }

                    float vn = Dot(vel, n);
                    Vector vNorm = n * vn;
                    Vector vTan  = vel - vNorm;
                    vel = (vNorm * -restitution) + (vTan * friction);
                    cur = probe + n * (radius * 0.30f + 0.05f);
                    hit = true;
                    ++bounces;
                    break;
                }
            }

            if (!hit) cur = next;
            elapsed += tickInterval;

            // Stop if grenade is at rest.
            if (LengthSq(vel) < 16.f)
            {
                frame.detonationPos = cur;
                frame.landed        = true;
                break;
            }

            // Range / fuse cap (fuse for HE/flash/smoke ends sim).
            if (LengthSq(cur - startPos) > maxRangeSq) break;
            if (prof.fuseSeconds > 0.f && elapsed >= prof.fuseSeconds)
            {
                frame.detonationPos = cur;
                frame.landed        = true;
                break;
            }
        }

        if (!frame.landed && !frame.points.empty())
            frame.detonationPos = frame.points.back();
        frame.airTime = elapsed;
        return frame;
    }

    void ClearFrames()
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        g_frameQueue.clear();
        g_lastFrame = {};
    }

    // ------------------------------------------------------------------
    // Throw-strength reading. CS2 distinguishes:
    //   LMB only        — full throw    (strength = 1.0)
    //   RMB only        — soft underhand (strength = 0.5)
    //   LMB + RMB held  — drop (strength = 0.0)
    // ------------------------------------------------------------------
    float ReadThrowStrength(bool& outActive)
    {
        const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        outActive = lmb || rmb || Globals::grenade_prediction_always_show;
        if (lmb && rmb) return 0.0f;
        if (rmb)        return 0.5f;
        if (lmb)        return 1.0f;
        // Passive-show mode: assume full-power throw so user can aim.
        return Globals::grenade_prediction_passive_strength;
    }

    // ------------------------------------------------------------------
    // World-to-screen helper bound to current viewport.
    // ------------------------------------------------------------------
    bool ToScreen(const Vector& w, ImVec2& s)
    {
        Vector tmp{};
        if (!Utils::WorldToScreen(w, tmp, (float*)Globals::ViewMatrix,
                                  (float)Globals::ScreenWidth,
                                  (float)Globals::ScreenHeight))
            return false;
        s = ImVec2(tmp.x, tmp.y);
        return true;
    }

    // ------------------------------------------------------------------
    // Draw a 3D-ish flat disc on the impact surface (oriented to surface
    // normal). Uses N sample points on the disc rim projected through
    // WorldToScreen.
    // ------------------------------------------------------------------
    void DrawDisc(ImDrawList* dl, const Vector& center, const Vector& normal,
                  float radius, ImU32 col, float thickness, int segments = 48)
    {
        // Build two perpendicular tangent vectors.
        Vector n = NormalizeSafe(normal);
        if (LengthSq(n) < 0.5f) n = {0,0,1};
        Vector up   = (std::fabs(n.z) > 0.95f) ? Vector{1,0,0} : Vector{0,0,1};
        Vector tanA = NormalizeSafe(Vector{
            up.y*n.z - up.z*n.y,
            up.z*n.x - up.x*n.z,
            up.x*n.y - up.y*n.x });
        Vector tanB = NormalizeSafe(Vector{
            n.y*tanA.z - n.z*tanA.y,
            n.z*tanA.x - n.x*tanA.z,
            n.x*tanA.y - n.y*tanA.x });

        ImVec2 prev{};
        bool   havePrev = false;
        ImVec2 first{};
        bool   haveFirst = false;
        for (int i = 0; i <= segments; ++i)
        {
            float a = (float)i / (float)segments * 6.2831853f;
            Vector p = center +
                tanA * (std::cos(a) * radius) +
                tanB * (std::sin(a) * radius);
            ImVec2 s;
            if (!ToScreen(p, s)) { havePrev = false; continue; }
            if (havePrev)
                dl->AddLine(prev, s, col, thickness);
            else if (!haveFirst) { first = s; haveFirst = true; }
            prev = s;
            havePrev = true;
        }
    }

    // ------------------------------------------------------------------
    // Color gradient helper — interpolate between two ImU32 colors.
    // ------------------------------------------------------------------
    ImU32 LerpCol(ImU32 a, ImU32 b, float t)
    {
        t = std::clamp(t, 0.f, 1.f);
        int ar=(a>>IM_COL32_R_SHIFT)&0xFF, ag=(a>>IM_COL32_G_SHIFT)&0xFF,
            ab=(a>>IM_COL32_B_SHIFT)&0xFF, aa=(a>>IM_COL32_A_SHIFT)&0xFF;
        int br=(b>>IM_COL32_R_SHIFT)&0xFF, bg=(b>>IM_COL32_G_SHIFT)&0xFF,
            bb=(b>>IM_COL32_B_SHIFT)&0xFF, ba=(b>>IM_COL32_A_SHIFT)&0xFF;
        int r = (int)(ar + (br-ar) * t);
        int g = (int)(ag + (bg-ag) * t);
        int bl= (int)(ab + (bb-ab) * t);
        int al= (int)(aa + (ba-aa) * t);
        return IM_COL32(r, g, bl, al);
    }
}

// ============================================================================
//   Public API
// ============================================================================
void GrenadePrediction::Initialize()
{
    RefreshEngineConstants(/*force=*/true);
}

void GrenadePrediction::Update()
{
    if (!Globals::grenade_prediction_enabled)
    {
        ClearFrames();
        return;
    }
    if (!Raycasting::Get().IsLoaded())
    {
        ClearFrames();
        return;
    }

    C_CSPlayerPawn* pawn = EntityManager::Get().GetLocalPawn();
    if (!pawn || !Utils::SafeAlive(pawn))
    {
        ClearFrames();
        return;
    }

    uint16_t defIdx = GetActiveWeaponDef(pawn);
    GrenadeKind kind = ClassifyWeapon(defIdx);
    if (kind == GrenadeKind::None)
    {
        ClearFrames();
        return;
    }

    bool active = false;
    float strength = ReadThrowStrength(active);
    if (!active)
    {
        ClearFrames();
        return;
    }

    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client) return;

    // Refresh convars periodically so live sv_gravity changes propagate.
    RefreshEngineConstants(/*force=*/false);

    const GrenadeProfile& prof = ProfileFor(kind);
    Vector viewAngles = Utils::SafeRead<Vector>(client + Offsets::dwViewAngles);
    Vector playerVel  = EstimatePlayerVel(pawn);
    Vector startPos   = GetLocalEye(pawn);
    if (startPos.IsZero()) return;

    Vector throwVel = ComputeThrowVel(viewAngles, strength, playerVel, prof);
    TrajectoryFrame frame = Simulate(startPos, throwVel, prof, kind);
    if (frame.points.size() < 2) return;

    std::lock_guard<std::mutex> lock(g_frameMutex);
    g_frameQueue.emplace_back(std::move(frame));
    while (g_frameQueue.size() > 4) g_frameQueue.pop_front();
}

void GrenadePrediction::Render()
{
    if (!Globals::grenade_prediction_enabled) return;

    TrajectoryFrame f;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        if (!g_frameQueue.empty())
        {
            f = std::move(g_frameQueue.front());
            g_frameQueue.pop_front();
            g_lastFrame = f;
        }
        else f = g_lastFrame;
    }
    if (f.points.size() < 2) return;

    auto* dl = ImGui::GetBackgroundDrawList();

    const ImU32 baseCol = ImGui::ColorConvertFloat4ToU32(
        *(ImVec4*)Globals::grenade_prediction_color);
    const ImU32 hitCol  = ImGui::ColorConvertFloat4ToU32(
        *(ImVec4*)Globals::grenade_prediction_hit_color);
    const float thick   = std::max(1.f, Globals::grenade_prediction_thickness);
    const GrenadeProfile& prof = ProfileFor(f.kind);

    // --- Path with optional gradient -----------------------------------
    size_t impactCursor = 0;
    const size_t N = f.points.size();
    for (size_t i = 1; i < N; ++i)
    {
        while (impactCursor < f.impactIndices.size() &&
               f.impactIndices[impactCursor] < i) ++impactCursor;
        const bool isImpact =
            impactCursor < f.impactIndices.size() &&
            f.impactIndices[impactCursor] == i;

        ImVec2 a{}, b{};
        if (!ToScreen(f.points[i - 1], a)) continue;
        if (!ToScreen(f.points[i],     b)) continue;

        ImU32 col;
        if (isImpact) col = hitCol;
        else if (Globals::grenade_prediction_gradient)
            col = LerpCol(baseCol, prof.detonationColor, (float)i / (float)N);
        else
            col = baseCol;

        dl->AddLine(a, b, col, thick);

        if (isImpact && Globals::grenade_prediction_draw_points)
            dl->AddCircleFilled(b, thick + 1.5f, hitCol, 12);
    }

    // --- Detonation 3D disc on landing surface -------------------------
    if (Globals::grenade_prediction_show_landing && f.landed)
    {
        // Outline disc — small, just marks the spot.
        DrawDisc(dl, f.detonationPos, f.detonationNormal,
                 std::max(8.f, prof.boundingRadius * 1.5f),
                 prof.detonationColor, thick + 0.5f, 32);

        // Crosshair marker
        ImVec2 cs;
        if (ToScreen(f.detonationPos, cs))
        {
            const float k = 6.f;
            dl->AddLine({cs.x - k, cs.y}, {cs.x + k, cs.y},
                        prof.detonationColor, thick);
            dl->AddLine({cs.x, cs.y - k}, {cs.x, cs.y + k},
                        prof.detonationColor, thick);
        }
    }

    // --- Damage / smoke / fire / flash radius indicator ----------------
    if (Globals::grenade_prediction_show_radius &&
        f.landed && prof.effectRadius > 0.f)
    {
        // Use radius fill (semi-transparent rim).
        DrawDisc(dl, f.detonationPos, f.detonationNormal,
                 prof.effectRadius, prof.radiusFillColor, thick, 64);

        // HE: also draw inner "guaranteed-kill" ring.
        if (f.kind == GrenadeKind::HE)
        {
            DrawDisc(dl, f.detonationPos, f.detonationNormal,
                     prof.effectRadius * 0.40f,
                     IM_COL32(255, 50, 30, 130),
                     thick, 48);
        }
    }

    // --- Text overlay near impact (fuse, distance, kind) ---------------
    if (Globals::grenade_prediction_show_text && f.landed)
    {
        ImVec2 cs;
        if (ToScreen(f.detonationPos, cs))
        {
            char buf[128];
            const char* name = "";
            switch (f.kind)
            {
            case GrenadeKind::HE:      name = "HE";      break;
            case GrenadeKind::Flash:   name = "FLASH";   break;
            case GrenadeKind::Smoke:   name = "SMOKE";   break;
            case GrenadeKind::Molotov: name = "MOLOTOV"; break;
            case GrenadeKind::Inc:     name = "INC";     break;
            case GrenadeKind::Decoy:   name = "DECOY";   break;
            default:                   name = "NADE";    break;
            }

            // Distance from local pawn to detonation
            float dist = 0.f;
            {
                C_CSPlayerPawn* pawn = EntityManager::Get().GetLocalPawn();
                if (pawn)
                {
                    Vector eye = GetLocalEye(pawn);
                    dist = (f.detonationPos - eye).Length();
                }
            }

            if (prof.fuseSeconds > 0.f && !prof.detonateOnImpact)
            {
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "%s  flight %.2fs  fuse %.2fs  %.0fu",
                            name, f.airTime,
                            std::max(0.f, prof.fuseSeconds - f.airTime),
                            dist);
            }
            else if (prof.detonateOnImpact)
            {
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "%s  IGNITE on land  flight %.2fs  %.0fu",
                            name, f.airTime, dist);
            }
            else
            {
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "%s  flight %.2fs  %.0fu",
                            name, f.airTime, dist);
            }

            ImVec2 sz = ImGui::CalcTextSize(buf);
            ImVec2 tl{ cs.x + 8.f,           cs.y - sz.y - 4.f };
            ImVec2 br{ tl.x + sz.x + 6.f,    tl.y + sz.y + 4.f };
            dl->AddRectFilled(tl, br, IM_COL32(0,0,0,170), 3.f);
            dl->AddText({ tl.x + 3.f, tl.y + 2.f },
                        IM_COL32(255,255,255,235), buf);
        }
    }
}
