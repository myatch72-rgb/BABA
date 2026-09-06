# 🔨 RAVEN CS2 INJECTOR - DERLEME TALİMATLARI

## 📋 Gereksinimler

### Yazılım Gereksinimleri:
- **Visual Studio 2019** veya daha yeni (Community Edition yeterli)
- **Windows 10 SDK** (10.0.19041.0 veya daha yeni)
- **C++17 veya üstü** desteği
- **Git** (BlackBone klonlamak için)

### Donanım Gereksinimleri:
- **x64 işlemci** (zorunlu)
- En az **4GB RAM**
- **2GB boş disk alanı**

---

## 🚀 Adım Adım Derleme

### 1. BlackBone Kütüphanesini Hazırlama

BlackBone bu projede zaten `include/BlackBone` ve `lib/` klasörlerinde bulunuyor. Eğer eksikse:

```cmd
cd "C:\Users\myatc\OneDrive\Desktop\BABA-main\Injector (1)\Injector"
git clone https://github.com/DarthTon/Blackbone.git temp_blackbone
```

Sonra `temp_blackbone/src` içindeki dosyaları `include/BlackBone` klasörüne kopyalayın.

---

### 2. Visual Studio Proje Ayarları

#### Solution'u açın:
```
Injector.sln dosyasını Visual Studio ile açın
```

#### Configuration ayarları:
1. **Solution Explorer** > **Injector** projesine sağ tık > **Properties**
2. **Configuration:** Release
3. **Platform:** x64

#### C/C++ Ayarları:

**General:**
- C++ Language Standard: `ISO C++17 Standard (/std:c++17)` veya üstü
- SDL checks: `No (/sdl-)`

**Preprocessor > Preprocessor Definitions:**
```
_CRT_SECURE_NO_WARNINGS
WIN32_LEAN_AND_MEAN
NOMINMAX
NDEBUG
_CONSOLE
```

**Code Generation:**
```
Runtime Library: Multi-threaded (/MT)  ← ÖNEMLİ!
Security Check: Disable Security Check (/GS-)
```

**Optimization:**
```
Optimization: Maximize Speed (/O2)
Inline Function Expansion: Any Suitable (/Ob2)
Favor Size Or Speed: Favor fast code (/Ot)
```

**Language:**
```
C++ Language Standard: ISO C++17
Conformance mode: No
```

#### Linker Ayarları:

**General:**
```
Enable Incremental Linking: No (/INCREMENTAL:NO)
```

**Debugging:**
```
Generate Debug Info: No
```

**System:**
```
SubSystem: Console (/SUBSYSTEM:CONSOLE)
```

**Advanced:**
```
Randomized Base Address: Yes (/DYNAMICBASE)
Data Execution Prevention: Yes (/NXCOMPAT)
```

**Input > Additional Dependencies:**
```
kernel32.lib
user32.lib
advapi32.lib
psapi.lib
%(AdditionalDependencies)
```

---

### 3. Derleme İşlemi

#### Visual Studio IDE ile:
1. **Build > Configuration Manager**
   - Active solution configuration: **Release**
   - Active solution platform: **x64**
2. **Build > Build Solution** (Ctrl+Shift+B)
3. Başarılı derleme sonrası:
   ```
   x64\Release\Injector.exe
   ```

#### Command Line ile (Developer Command Prompt):
```cmd
cd "C:\Users\myatc\OneDrive\Desktop\BABA-main\Injector (1)\Injector"
msbuild Injector.sln /p:Configuration=Release /p:Platform=x64 /m
```

---

## 📦 Çıktı Dosyaları

Başarılı derleme sonrası şu dosyalar oluşur:

```
x64\Release\
├── Injector.exe          <- Ana çalıştırılabilir dosya
├── Injector.pdb          <- Debug sembolleri (opsiyonel)
└── Injector.iobj/.ipdb   <- Incremental link dosyaları (silinebilir)
```

**Gerekli dosyalar dağıtım için:**
- ✅ `Injector.exe` (zorunlu)
- ✅ `raven.dll` (enjekte edilecek DLL)
- ❌ `.pdb`, `.iobj`, `.ipdb` dosyaları gereksiz

---

## 🧪 Test ve Doğrulama

### 1. Basit Test:
```cmd
# Yönetici olarak çalıştır
cd x64\Release
Injector.exe
```

**Beklenen çıktı:**
```
======================================================================
              RAVEN CS2 - BLACKBONE MANUAL MAP INJECTOR               
       [Stealth Native Manual Mapper - PE Header Wipe & VAD Hide]     
======================================================================

[+] Log sistemi baslatildi.
[+] Yonetici yetkileri dogrulandi.
[+] SeDebugPrivilege basariyla etkinlestirildi.
...
```

### 2. Dependency Check:
```cmd
# Dependencies.exe ile kontrol (opsiyonel)
dumpbin /dependents Injector.exe
```

**Beklenen bağımlılıklar (minimal):**
- KERNEL32.dll
- USER32.dll
- ADVAPI32.dll
- PSAPI.dll
- (VCRUNTIME140.dll OLMAMALI - /MT kullanıldıysa)

---

## ⚠️ Olası Derleme Hataları ve Çözümleri

### Hata: `LNK1104: cannot open file 'BlackBone.lib'`

**Çözüm:**
BlackBone statik kütüphanesini derleyip `lib/` klasörüne koyun:
```cmd
cd temp_blackbone\build
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
copy Release\BlackBone.lib ..\..\lib\
```

---

### Hata: `C2039: 'vector': is not a member of 'std'`

**Çözüm:**
`#include <vector>` ekleyin ve C++ standardını kontrol edin:
```cpp
Project Properties > C/C++ > Language > C++ Language Standard: /std:c++17
```

---

### Hata: `LNK2019: unresolved external symbol`

**Çözüm:**
1. Tüm `.cpp` dosyalarının projeye ekli olduğunu kontrol edin
2. `psapi.lib` eklenmişse kontrol edin:
   ```
   Project Properties > Linker > Input > Additional Dependencies
   ```

---

### Hata: Runtime Error (MSVCP140.dll eksik)

**Çözüm:**
Runtime Library ayarını değiştirin:
```
Properties > C/C++ > Code Generation > Runtime Library: /MT
```

**NOT:** `/MT` kullanırsanız C++ Runtime statik olarak bağlanır (önerilir)

---

## 🎨 Build Varyantları

### Debug Build (Geliştirme):
```
Configuration: Debug
Runtime Library: /MTd
Optimization: Disabled (/Od)
```

**Avantajlar:**
- Hata ayıklama kolay
- Breakpoint kullanılabilir
- PDB dosyaları tam bilgi içerir

**Dezavantajlar:**
- Yavaş çalışır
- Dosya boyutu büyük

---

### Release Build (Production):
```
Configuration: Release
Runtime Library: /MT
Optimization: /O2
```

**Avantajlar:**
- Maksimum hız
- Küçük dosya boyutu
- Bağımsız çalışır (external DLL gerektirmez)

**Dezavantajlar:**
- Debug zor
- Compiler optimizasyonları kod akışını değiştirebilir

---

## 📂 Dosya Yapısı

```
Injector (1)/
├── Injector/
│   ├── include/
│   │   ├── BlackBone/         <- BlackBone header dosyaları
│   │   ├── 3rd_party/         <- Üçüncü parti kütüphaneler
│   │   └── BlackBoneDrv/      <- Driver headers
│   ├── lib/
│   │   ├── BlackBone.lib      <- BlackBone statik library (x64)
│   │   └── ...
│   ├── src/
│   │   ├── main.cpp           <- Ana giriş noktası
│   │   ├── manual_map.cpp/h   <- BlackBone wrapper
│   │   ├── process.cpp/h      <- Process utilities
│   │   ├── logger.cpp/h       <- Log sistemi
│   │   ├── anti_debug.cpp/h   <- Anti-debug kontroller
│   │   └── exception_handler.h <- SEH handler
│   ├── x64/
│   │   └── Release/
│   │       └── Injector.exe   <- Derlenen executable
│   ├── Injector.sln           <- Visual Studio solution
│   ├── Injector.vcxproj       <- Proje dosyası
│   └── BUILD_INSTRUCTIONS.md  <- Bu dosya
└── IMPROVEMENTS.md             <- İyileştirmeler dökümantasyonu
```

---

## 🔐 Güvenlik Notları

### Code Signing (Opsiyonel):
Eğer dijital imza eklemek isterseniz:
```cmd
signtool sign /f "certificate.pfx" /p "password" /t http://timestamp.digicert.com Injector.exe
```

### Obfuscation (Opsiyonel):
String encryption ve control flow obfuscation için:
- VMProtect
- Themida
- Enigma Protector

**NOT:** Obfuscation anti-virus false positive oranını artırabilir!

---

## 📋 Checklist: Production Build

Dağıtım öncesi kontrol listesi:

- [ ] Configuration: **Release x64**
- [ ] Runtime Library: **/MT** (statik)
- [ ] Optimization: **Enabled (/O2)**
- [ ] Debug info: **Removed**
- [ ] Dependencies: **Minimal** (dumpbin ile kontrol)
- [ ] Dosya boyutu: **< 500KB** (kabul edilebilir)
- [ ] Test: **Yönetici olarak çalışıyor**
- [ ] Test: **cs2.exe bulunca enjekte ediyor**
- [ ] Test: **Log dosyası oluşturuluyor**
- [ ] Test: **Modül bekleme çalışıyor**
- [ ] Antivirus test: **False positive kontrol**

---

## 🆘 Destek

### Hata Raporlama:
Derleme hatası yaşarsanız lütfen şunları paylaşın:
1. Visual Studio sürümü
2. Windows SDK sürümü
3. Tam hata mesajı (Build Output)
4. `Injector.vcxproj` dosya içeriği

### Faydalı Kaynaklar:
- [Visual Studio Documentation](https://docs.microsoft.com/en-us/visualstudio/)
- [CMake Tutorial](https://cmake.org/cmake/help/latest/guide/tutorial/)
- [BlackBone GitHub](https://github.com/DarthTon/Blackbone)
- [Windows SDK Documentation](https://docs.microsoft.com/en-us/windows/win32/)

---

## 📜 Lisans ve Yasal

Bu proje **eğitim ve araştırma amaçlıdır**. 

- ⚠️ Online oyunlarda kullanımı **ToS ihlalidir**
- ⚠️ VAC ban riski vardır
- ⚠️ Yalnızca **test ortamında** kullanın
- ⚠️ Geliştiricinin hiçbir sorumluluğu yoktur

**BlackBone Library:**
- Lisans: MIT License
- Geliştirici: DarthTon
- Repository: https://github.com/DarthTon/Blackbone

---

**Son Güncelleme:** 2024-01-15  
**Versiyon:** 2.0.0  
**Minimum Visual Studio:** 2019 (v142 toolset)
