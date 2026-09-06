#include "chams_material.hpp"

#include "../../core/main.hpp"
#include "../../sdk/valve/signatures/signatures.hpp"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {
	// --- KeyValues3 machinery (layouts lifted from a known-working reference) ---
	struct kv3_id_t {
		const char*   m_name;
		std::uint64_t m_unk0;
		std::uint64_t m_unk1;
	};

	class utl_buffer_t {
	public:
		char _pad[0x80];
	};

	class c_key_values_3 {
	public:
		char          pad_0000[0x100];
		std::uint64_t m_key;
		void*         m_value;
		char          pad_0110[0x8];
	};

	using ctor_utlbuffer_fn  = utl_buffer_t* (__fastcall*)(utl_buffer_t*, int, int, int);
	using putstring_fn       = void          (__fastcall*)(utl_buffer_t*, const char*);
	using loadkv3_fn         = bool          (__fastcall*)(c_key_values_3*, void*, utl_buffer_t*, const kv3_id_t*, const char*);
	using setkv3type_fn      = c_key_values_3*(__fastcall*)(c_key_values_3*, unsigned int, unsigned int);
	using create_material_fn = std::int64_t  (__fastcall*)(void*, void*, const char*, void*, unsigned int, unsigned int);

	struct natives_t {
		ctor_utlbuffer_fn  ctor_utlbuffer  = nullptr;
		putstring_fn       put_string      = nullptr;
		loadkv3_fn         load_kv3        = nullptr;
		setkv3type_fn      set_kv3_type    = nullptr;
		create_material_fn create_material = nullptr;
		bool               tried           = false;
		bool               ok              = false;
	};

	// Resolve every native entry point once. tier0 helpers are exported by name;
	// SetTypeKV3 / CreateMaterial come from the signature database.
	natives_t& natives() {
		static natives_t n;
		if (n.tried)
			return n;
		n.tried = true;

		if (const HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
			n.ctor_utlbuffer = reinterpret_cast<ctor_utlbuffer_fn>(GetProcAddress(tier0, "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z"));
			n.put_string     = reinterpret_cast<putstring_fn>(GetProcAddress(tier0, "?PutString@CUtlBuffer@@QEAAXPEBD@Z"));
			n.load_kv3       = reinterpret_cast<loadkv3_fn>(GetProcAddress(tier0, "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEAVCUtlBuffer@@AEBUKV3ID_t@@PEBDI@Z"));
			if (!n.load_kv3)
				n.load_kv3   = reinterpret_cast<loadkv3_fn>(GetProcAddress(tier0, "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"));
		}

		n.set_kv3_type    = reinterpret_cast<setkv3type_fn>(SIG("KV3_SetType"));
		n.create_material = reinterpret_cast<create_material_fn>(SIG("CreateMaterial"));

		n.ok = n.ctor_utlbuffer && n.put_string && n.load_kv3 && n.set_kv3_type && n.create_material;
		return n;
	}

	// Allocate a KV3 material-resource block, set its type and load the .vmat text.
	c_key_values_3* build_material_kv(const natives_t& n, const char* buffer) {
		c_key_values_3* kv = new c_key_values_3[0x10];
		n.set_kv3_type(kv, 1U, 6U);   // (type = generic material resource)

		const int len = static_cast<int>(std::strlen(buffer));
		utl_buffer_t buf;
		n.ctor_utlbuffer(&buf, 0, len + 1, 1);
		n.put_string(&buf, buffer);

		kv3_id_t id{ "generic", 0x41B818518343427Eull, 0xB5F447C23C0CDF8Cull };
		n.load_kv3(kv, nullptr, &buf, &id, nullptr);
		return kv;
	}
}

c_material_2* create_custom_material(const char* material_name, const char* kv3_buffer) {
	const natives_t& n = natives();
	if (!n.ok || !material_name || !kv3_buffer)
		return nullptr;

	__try {
		c_key_values_3* kv = build_material_kv(n, kv3_buffer);
		if (!kv)
			return nullptr;

		// create_material(this = null (uses its own global), &binding_out, name, kv, 0, 1).
		// The out-parameter receives a resource-binding pointer; the material itself is
		// binding->data (the reference's strong-handle conversion resolves to the same).
		void* binding_out = nullptr;
		n.create_material(nullptr, &binding_out, material_name, kv, 0U, 1U);

		if (!binding_out)
			return nullptr;

		return *reinterpret_cast<c_material_2**>(binding_out);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// ---- Material-resource loader ----------------------------------------------
// IMaterialSystem2::FindOrCreateMaterialFromResource is a virtual method of the
// VMaterialSystem2_001 interface (materialsystem2.dll). Community gamedata pins it
// at vtable index 14. Signature (Windows / this-first ABI):
//   CMaterial*** __thiscall (IMaterialSystem2* this, CMaterial** out, const char* name)
// The returned CMaterial*** is dereferenced once to reach the CMaterial**
// (InfoForResourceTypeIMaterial2) that the draw primitive's m_material slot wants.
namespace {
	constexpr int k_find_or_create_material_index = 14;

	using find_or_create_material_fn = void** (__fastcall*)(void* material_system, void** out, const char* name);

	void* material_system_interface() {
		static void* iface = get_interface(&g_modules->m_modules.materialsystem2_dll, xorstr_("VMaterialSystem2_001"));
		return iface;
	}
}

c_material_2* find_material_by_path(const char* path) {
	if (!path)
		return nullptr;

	void* material_system = material_system_interface();
	if (!material_system)
		return nullptr;

	__try {
		// Strip a trailing "_c" (compiled suffix): the loader expects the source
		// path "materials/skybox/x.vmat", not "x.vmat_c".
		char name[256];
		std::strncpy(name, path, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		const std::size_t len = std::strlen(name);
		if (len >= 2 && name[len - 2] == '_' && name[len - 1] == 'c')
			name[len - 2] = '\0';

		auto fn = vmt::get_v_method<find_or_create_material_fn>(material_system, k_find_or_create_material_index);
		if (!fn)
			return nullptr;

		void* out = nullptr;
		void** result = fn(material_system, &out, name);
		if (!result)
			return nullptr;

		// CMaterial*** -> CMaterial** : one dereference yields the material handle.
		return *reinterpret_cast<c_material_2**>(result);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// ---- Sky override -----------------------------------------------------------
namespace {
	struct sky_def_t {
		const char* label;    // menu label (shown in the dropdown)
		const char* resource; // real compiled material path under materials/skybox/
	};

	// Real, already-compiled sky materials shipped with the game (verified present
	// under csgo/materials/skybox/*.vmat_c). These load as fully-built engine
	// materials -- the sky pass binds them without crashing, unlike a KV3-assembled
	// material. To add one: pick any materials/skybox/<name>.vmat_c and add a row.
	// Order must match sky_override_names() (which prepends "none").
	constexpr sky_def_t k_skies[] = {
		{ "lunacy (space/moon)", "materials/skybox/sky_lunacy.vmat"                 },
		{ "dust (sunny)",        "materials/skybox/sky_dust.vmat"                   },
		{ "vertigo (clouds)",    "materials/skybox/sky_vertigo.vmat"                },
		{ "aztec (overcast)",    "materials/skybox/sky_hr_aztec.vmat"                },
		{ "skydome 1",           "materials/models/props_structures/skydome.vmat"    },
		{ "skydome 2",           "materials/models/props_structures/skydome_2.vmat"  },
		{ "skydome 3",           "materials/models/props_structures/skydome_3.vmat"  },
		{ "skydome clouds",      "materials/models/props_structures/skydome_clouds.vmat" },
		{ "night city 1",        "materials/models/props_skyscrapers/skyscraper_top_01_night.vmat" },
		{ "night city 2",        "materials/models/props_skyscrapers/skyscraper_top_02_night.vmat" },
	};
	static_assert(std::size(k_skies) + 1 == k_sky_override_count, "sky table / count mismatch");
}

const char* const* sky_override_names() {
	static const char* names[k_sky_override_count] = { "none" };
	static bool built = false;
	if (!built) {
		for (int i = 0; i < static_cast<int>(std::size(k_skies)); ++i)
			names[i + 1] = k_skies[i].label;
		built = true;
	}
	return names;
}

c_material_2* get_override_sky_material(int index) {
	if (index <= 0 || index >= k_sky_override_count)
		return nullptr;

	static c_material_2* cache[k_sky_override_count] = {};
	if (cache[index])
		return cache[index];

	const sky_def_t& def = k_skies[index - 1];   // index 0 is "none"

	// Load the game's own compiled sky material by path -- a real, fully-built engine
	// material the sky pass can bind safely.
	cache[index] = find_material_by_path(def.resource);
	return cache[index];
}
