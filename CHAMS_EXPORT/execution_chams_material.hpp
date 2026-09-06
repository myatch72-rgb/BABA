#pragma once

#include "../../sdk/valve/classes/c_scene_object.hpp"   // c_scene_animatable_object, c_material_2

#include <cstdint>

// Colour as stored inline in the DrawObject mesh descriptor.
struct chams_mesh_color_t {
	std::uint8_t r, g, b, a;
};

// The mesh descriptor passed as the 3rd argument to the scene-system DrawObject
// function. Layout verified against a known-working reference. NOTE: the colour
// here is stored INLINE (4 bytes) -- this is intentionally different from
// c_mesh_primitive, which models the same slot as a pointer; for DrawObject the
// inline interpretation is the correct one.
class c_mesh_data {
public:
	char                       pad_0[24];               // 0x00
	c_scene_animatable_object* scene_animatable_object; // 0x18
	c_material_2*              material;                // 0x20
	c_material_2*              material2;               // 0x28
	char                       pad_1[32];               // 0x30
	chams_mesh_color_t         color;                   // 0x50
	char                       pad_2[16];               // 0x54
};

// Build a custom material from a KV3 (.vmat) text buffer via the native material
// system. Returns nullptr on any failure (unresolved signatures/exports, resource
// system not ready). Every native call is wrapped in SEH. Must NOT be called from
// inside the DrawObject hook -- it may load resources; call it from a frame-safe
// context (e.g. the present hook) instead.
c_material_2* create_custom_material(const char* material_name, const char* kv3_buffer);

// Load an already-compiled material resource by path (e.g.
// "materials/skybox/aurora.vmat") via IMaterialSystem2::FindOrCreateMaterialFromResource.
// Unlike create_custom_material (which assembles a material from a KV3 text buffer and
// leaves sky-shader sub-blocks unbound -> crashes the sky pass), this returns a real,
// fully-built engine material. A trailing "_c" is stripped. Returns nullptr on failure.
// May load resources -- call only from a frame-safe context (present hook).
c_material_2* find_material_by_path(const char* path);

// ---- Sky override -----------------------------------------------------------
// A fixed table of selectable skyboxes for the "override sky" feature. Index 0
// is "none" (colour-only, no material swap); indices 1..k_sky_override_count-1
// select a skybox. sky_override_names() returns k_sky_override_count menu labels
// (parallel to the table, "none" first).
//
// get_override_sky_material() lazily builds each skybox material once (via
// create_custom_material) and caches it, returning the cached pointer on later
// calls. Because it may load resources, call it ONLY from a frame-safe context
// (the present hook) -- never from inside a draw hook. Returns nullptr for index
// 0, an out-of-range index, or a material that failed to build.
inline constexpr int k_sky_override_count = 11;   // "none" + 10 skyboxes
const char* const* sky_override_names();
c_material_2*      get_override_sky_material(int index);
