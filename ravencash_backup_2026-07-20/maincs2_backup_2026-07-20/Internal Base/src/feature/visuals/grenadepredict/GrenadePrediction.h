#pragma once

// ----------------------------------------------------------------------------
//  GrenadePrediction
//
//  Self-healing, pattern-driven grenade trajectory predictor for CS2.
//
//  Design goals:
//    * No hard-coded engine constants. Gravity, tick interval and (where
//      possible) per-weapon throw velocity are read at runtime from convars
//      or from the live CGlobalVars/weapon entity.
//    * Per-grenade-type physics profiles (HE / Flash / Smoke / Molotov /
//      Inc / Decoy) — bounce, friction, fuse, post-impact behaviour all
//      differ per grenade.
//    * Rich on-screen overlays: detonation 3D disc, damage / smoke / fire /
//      flash radii, fuse-timer text, distance markers, gradient path.
//    * Survives game updates without offset patching — the only thing that
//      can break it is a complete change to convar naming or the weapon
//      class hierarchy, both of which are extremely rare.
//
//  Call once per frame:
//      GrenadePrediction::Update();   // simulate (cheap, mutex-protected)
//      GrenadePrediction::Render();   // draw the last cached frame
// ----------------------------------------------------------------------------
namespace GrenadePrediction
{
    // Called once during startup (Main.cpp), after Memory::Globals::Initialize
    // and Memory::Convars::Initialize. Caches engine convars / tick rate.
    void Initialize();

    // Cheap simulation tick. Bails immediately when the feature is disabled
    // or no grenade is held. Safe to call from the render thread.
    void Update();

    // Draws the last cached trajectory + overlays on the ImGui background
    // draw list. Must run on the present-callback thread.
    void Render();
}
