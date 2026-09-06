# CS2 CHAMS & MATERYAL ENTEGRASYON PAKETI

Bu paket, **Execution** projesindeki gelişmiş materyal sistemi ve KeyValues3 shader yapılandırmalarını başka herhangi bir CS2 projesine (örneğin `ravencash` veya sıfırdan bir internal base) entegre etmek için gereken tüm kaynak kodları, pattern'leri, vtable indekslerini ve KV3 materyallerini içerir.

---

## 📁 1. Klasör İçeriği

1. **`ChamsV4.cpp` & `ChamsV4.h`**:
   - `ravencash` (Internal Base) projesi için modernize edilmiş, 12 materyalin tamamını (Metallic, Glow, Ghost, Textured, Flat, Overlay, Latex, Wireframe, Pearlescent, Glass, Gold, Illuminate) destekleyen hazır drop-in dosyalar.
   - `DrawObject` hook'u ile çalışır, oyuncular (düşman/takım), eller, eldivenler ve silahlar için bağımsız materyal ve renk seçimi sunar.

2. **`execution_chams.cpp` & `execution_chams.hpp`**:
   - `Execution` projesinin `GeneratePrimitives` hook tabanlı chams sistemi.
   - `AnimatableSceneObjectDesc` sanal tablosunun (vtable) 4. indeksini hook'lar.
   - Mesh parçalanması (flicker) yapmaz, `c_mesh_primitive_output_buffer` üzerinde iki geçişli (occluded + visible XQZ) çizim yapar.

3. **`execution_chams_material.cpp` & `execution_chams_material.hpp`**:
   - `tier0.dll` (`CUtlBuffer`, `LoadKV3`) ve `client.dll` (`SetTypeKV3`), `materialsystem2.dll` (`CreateMaterial`) üzerinden KV3 metinlerinden dinamik motor materyali (`c_material_2*`) üreten altyapı.
   - Ayrıca 10 adet gökyüzü (Skybox) materyalini dinamik yükleyen fonksiyonları içerir.

4. **`raven.dll`**:
   - `ravencash` projesinin 12 materyalli güncel derlenmiş Release x64 binary'si.

---

## 🔍 2. Gerekli Pattern & İmzalar (CS2 Güncel)

### A. MaterialSystem2 & KeyValues3 (Materyal Üretimi)
```cpp
// materialsystem2.dll -> CreateMaterial
// İmzası:
"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 10 01 00 00 48 8B 05 ? ? ? ? 4C 8B F2"

// client.dll -> SetTypeKV3
// İmzası:
"40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16 00 00 00 48 8B D9 44 0F 45 C8"

// tier0.dll -> Export İsimleri:
// "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"
// "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z"
// "?PutString@CUtlBuffer@@QEAAXPEBD@Z"

// VMaterialSystem2_001 Arayüzü:
// GetProcAddress(GetModuleHandleA("materialsystem2.dll"), "CreateInterface")("VMaterialSystem2_001", nullptr)
```

### B. Hook Yöntemleri (DrawObject vs GeneratePrimitives)

#### Yöntem 1: DrawObject (SceneSystem)
```cpp
// scenesystem.dll -> DrawObject
"48 8B C4 53 57 41 54"
// Hook Prototipi:
void* __fastcall hkDrawObject(__int64 a1, __int64 a2, MeshDrawData* meshArray, int count, __int64 a5, __int64 a6, __int64 a7);
```

#### Yöntem 2: GeneratePrimitives (Flicker-Free, En Stabil)
```cpp
// AnimatableSceneObjectDesc vtable index 4
void* animatable_desc = g_interfaces->m_scene_system->get_scene_object_desc("AnimatableSceneObjectDesc");
void* generate_target = vmt::get_v_method(animatable_desc, 4);

// scenesystem.dll Raw Pattern (Alternatif):
"48 8B C4 4C 89 48 ? 4C 89 40 ? 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70"
```

---

## 🎨 3. Desteklenen 12 Adet Materyal Listesi

| İndeks | Materyal Adı | Shader | Açıklama |
|---|---|---|---|
| **0** | Flat | `csgo_unlitgeneric.vfx` | Düz, ışıksız, tek renk unlit materyal |
| **1** | Illuminate | `csgo_complex.vfx` | Kendi kendine aydınlanan (Self-Illuminated) parlak materyal |
| **2** | Glow | `csgo_effects.vfx` | Fresnel dış kenar parlamalı glow materyali |
| **3** | Ghost | `csgo_effects.vfx` | Yarı saydam, additive blend fresnel hayalet efekti |
| **4** | Textured | `csgo_complex.vfx` | Sahne ışıklarını ve gölgelerini koruyan PBR dokulu materyal |
| **5** | Metallic | `csgo_complex.vfx` | Haritanın cubemap yansımasını kullanan parlak krom/metal |
| **6** | Overlay | `csgo_effects.vfx` | Parlaklık ve renk boostlu additive dış çizgi kaplaması |
| **7** | Latex | `csgo_character.vfx` | PBR pürüzsüz kauçuk / plastik yüzey görünümü |
| **8** | Wireframe | `csgo_unlitgeneric.vfx` | `F_WIREFRAME = 1` bayraklı tel kafes görünümü |
| **9** | Pearlescent | `csgo_complex.vfx` | Bakış açısına göre ton değiştiren yanardöner (ince film) kaplama |
| **10** | Glass | `csgo_effects.vfx` | Kırılmalı cam ve kenar odaklı saydamlık efekti |
| **11** | Gold | `csgo_complex.vfx` | Yüksek metalik ve özel renk tonuyla fırçalanmış altın PBR |

---

## 🚀 4. Yeni Bir Projeye Entegre Etme Adımları

1. Yeni projenizin `visuals` klasörüne `ChamsV4.h` ve `ChamsV4.cpp` dosyalarını ekleyin.
2. `Hooks.cpp` içerisinde:
   ```cpp
   ChamsV4::Initialize();
   MH_CreateHook((void*)ChamsV4::g_DrawObjectAddr, &ChamsV4::hkDrawObject_Trampoline, (void**)&ChamsV4::oDrawObject);
   MH_QueueEnableHook((void*)ChamsV4::g_DrawObjectAddr);
   ```
3. Menü arayüzünüzdeki combo kutularına 12 seçeneği tanımlayın:
   ```cpp
   const char* cv4MatAll[] = {
       "Flat", "Illuminate", "Glow", "Ghost", "Textured", "Metallic", "Overlay", "Latex", "Wireframe", "Pearlescent", "Glass", "Gold"
   };
   ```
4. Projenizi derleyin.
