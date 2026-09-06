#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

class c_material_2;
class c_animatable_scene_object_desc;
class c_scene_animatable_object;
class c_mesh_primitive_output_buffer;

// Chams: overrides the material of player/local meshes inside the scene-system
// GeneratePrimitives hook. Two independent passes -- "occluded" (through walls,
// depth disabled) and "visible" (depth tested) -- each with its own material style
// and colour. Applied to four independent targets (enemies, teammates, local arms,
// local weapon viewmodel). The custom materials are built once from embedded KV3
// (.vmat) buffers and shared across targets; only the colour differs per draw.
//
// GeneratePrimitives fires once per animatable scene object (a whole pawn / arms /
// weapon), with the object -- and therefore its owner -- resolved at natural
// granularity. This is what fixes the flicker: unlike the old DrawObject hook, which
// fired per low-level mesh part and had to reconstruct a churny owner pointer for
// each, here the owner is stable per object, so no TTL hysteresis / subtree walking
// is needed to bridge animation-driven scene-graph churn.
class c_chams {
public:
	// The original GeneratePrimitives function type (see
	// hooks::generate_primitives::hk_generate_primitives).
	using generate_primitives_fn = void*(__fastcall*)(c_animatable_scene_object_desc*, c_scene_animatable_object*, void*, c_mesh_primitive_output_buffer*);

	// Style index -- matches the menu material dropdown.
	enum { style_flat = 0, style_glow = 1, style_ghost = 2, style_textured = 3, style_metallic = 4, style_overlay = 5, style_latex = 6, style_wireframe = 7, style_pearlescent = 8, style_glass = 9, style_gold = 10, style_count = 11 };

	// Target index -- matches c_config::m_chams_targets ordering and the menu target dropdown.
	enum { target_enemy = 0, target_teammate = 1, target_arms = 2, target_viewmodel = 3, target_count = 4 };

	// Build the custom materials once. Must be called from a frame-safe context
	// (present hook), NOT from inside the hook. No-op after the first success.
	void ensure_init();

	// Refresh the map of target entity pointers -> target index. Call once per frame
	// from the present hook. on_generate_primitives only overrides an object whose
	// owner is in this map -- the schema class checks are unreliable inside the render
	// hook (get_class_name is broken in execution), so we classify targets here (in a
	// frame-safe context, via the controller scan) instead.
	void refresh_targets();

	// Clear cached target entity pointers and handles (called on round end / map shutdown)
	void clear_targets();

	// Called from the GeneratePrimitives hook for every animatable object. If the
	// object is a target, calls `original` to populate render_buf, swaps the material
	// and colour on the newly-added primitives, and returns true (the hook must NOT
	// call original again). Returns false to let the hook generate normally.
	bool on_generate_primitives(c_animatable_scene_object_desc* desc, c_scene_animatable_object* object,
	                            void* a3, c_mesh_primitive_output_buffer* render_buf, generate_primitives_fn original, void*& out_result);

private:
	// Look up the target index for an object owner, or -1 if it is not a target.
	// Matches by entity pointer first, then by handle slot (entry_index). Kept in its
	// own method so its std::lock_guard never shares a frame with the SEH in
	// on_generate_primitives.
	int classify(const void* entity, int entry_index);

	bool          m_initialized = false;
	c_material_2* m_visible[style_count]  = {};  // depth-tested variants
	c_material_2* m_occluded[style_count] = {};  // through-wall variants

	// Frames an entry survives without being re-resolved before it is aged out. Kept
	// as a small safety net for single-frame resolution misses; per-object owner
	// stability means this no longer has to bridge animation churn as before.
	static constexpr int k_target_ttl = 32;

	struct target_entry {
		int target;   // target index (target_enemy .. target_viewmodel)
		int ttl;      // frames left before eviction; reset to k_target_ttl on resolve
	};

	std::mutex                                     m_targets_mutex;
	std::unordered_map<const void*, target_entry>  m_targets;         // entity ptr   -> {target, ttl} (guarded)
	std::unordered_map<int, target_entry>          m_target_indices;  // handle slot  -> {target, ttl} (guarded)
};

inline const auto g_chams = std::make_unique<c_chams>();
