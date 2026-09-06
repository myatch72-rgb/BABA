#pragma once

namespace Features {

// Drives all gameplay feature work every rendered frame. Called from the
// Present path once per frame; this is the single, clean entry point the rest
// of the runtime uses instead of scattering per-feature calls across the hook.
//
// FrameStageNotify is intentionally not hooked on this build: its direct
// original call executes the receiver as a callback and faults at
// client+0x23C0A20. All frame-driven features (including skin logic) run from
// here instead, which sidesteps that fault entirely.
void RunFrame();

} // namespace Features
