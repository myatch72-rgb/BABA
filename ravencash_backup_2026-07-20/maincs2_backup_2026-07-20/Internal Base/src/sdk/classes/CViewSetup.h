#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
//  CViewSetup — engine-side per-frame view configuration struct.
//
//  Layout reverse-engineered from CS2 builds (mid-2024 → present). The
//  pad ranges are deliberate: only the fields we read/write are named, so
//  the struct survives minor engine reshuffles around them.
//
//  Used by the AspectRatio hook: the engine passes a CViewSetup* in RDX
//  to its view-setup function each frame. We write our user value into
//  flAspectRatio and set bit 1 of nSomeFlags so the engine actually
//  consumes the override rather than recomputing from the window size.
// ============================================================================
// Note: we intentionally inline the vector / angle members as three
// raw floats each instead of pulling in the SDK's Vector / QAngle —
// this header is included from low-level hook code that we don't want
// to drag heavyweight headers into.
#pragma pack(push, 1)
class CViewSetup {
public:
    unsigned char pad_0000[0x450];   // 0x000
    float         flOrthoLeft;       // 0x450
    float         flOrthoTop;        // 0x454
    float         flOrthoRight;      // 0x458
    float         flOrthoBottom;     // 0x45C
    unsigned char pad_0460[0x38];    // 0x460
    float         flFov;             // 0x498
    float         flFovViewmodel;    // 0x49C
    float         vecOrigin[3];      // 0x4A0
    unsigned char pad_04AC[0xC];     // 0x4AC
    float         angView[3];        // 0x4B8
    unsigned char pad_04C4[0x10];    // 0x4C4
    float         flAspectRatio;     // 0x4D4
    unsigned char pad_04D8[0x79];    // 0x4D8
    uint8_t       nSomeFlags;        // 0x551
};
#pragma pack(pop)

static_assert(offsetof(CViewSetup, flOrthoLeft)   == 0x450, "CViewSetup::flOrthoLeft offset mismatch");
static_assert(offsetof(CViewSetup, flFov)         == 0x498, "CViewSetup::flFov offset mismatch");
static_assert(offsetof(CViewSetup, flAspectRatio) == 0x4D4, "CViewSetup::flAspectRatio offset mismatch");
static_assert(offsetof(CViewSetup, nSomeFlags)    == 0x551, "CViewSetup::nSomeFlags offset mismatch");
