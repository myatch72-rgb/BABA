# 📝 RAVEN CS2 INJECTOR - CHANGELOG

## [2.0.1] - 2024-01-16 🔥 CRITICAL CRASH FIX

### 🐛 Kritik Hata Düzeltmeleri

#### ❌ OYUN CRASH SORUNU ÇÖZÜLDÜ
**Problem:** DLL başarıyla inject ediliyordu ama 8-12 saniye sonra CS2 çöküyordu.

**Kök Neden Analizi:**
1. `asImage=false` kullanılıyordu (RAW shellcode modu)
2. TLS (Thread Local Storage) callbacks çalışmıyordu
3. Static C++ constructor'lar çağrılmıyordu
4. Exception handling (SEH/VEH) düzgün kurulmuyordu
5. Import Address Table (IAT) bozuktu
6. BlackBone mapping flags eksikti (NoFlags kullanılmıştı)

**Uygulanan Çözümler:**

##### 1. `asImage=true` Aktif Edildi ✅
```cpp
// ÖNCEKİ (YANLIŞTI):
auto mapResult = targetProcess.mmap().MapImage(
    fileSize, buffer.data(), 
    false,  // ← RAW mode, DLL için yanlış!
    flags, nullptr, nullptr, &customArgs
);

// YENİ (DOĞRU):
auto mapResult = targetProcess.mmap().MapImage(
    fileSize, buffer.data(), 
    true,   // ← Native image mode, DLL için ŞART!
    flags, nullptr, nullptr, &customArgs
);
```

**Neden Gerekli:**
- TLS callbacks düzgün çalışır
- Static C++ global/static object constructor'ları çalışır
- Exception handling (SEH/VEH) doğru kurulur
- IAT (Import Address Table) düzgün map edilir
- ASLR relocations uygulanır
- Native PE format korunur

##### 2. Doğru Mapping Flags Eklendi ✅
```cpp
// ÖNCEKİ (YANLIŞTI):
blackbone::eLoadFlags flags = blackbone::NoFlags;  // 0x00, neredeyse hiçbir şey yapmaz

// YENİ (DOĞRU):
blackbone::eLoadFlags flags = blackbone::eLoadFlags::ManualImports 
                            | blackbone::eLoadFlags::CreateLdrRef
                            | blackbone::eLoadFlags::ResolveImports;
```

**Her Flag'in Önemi:**
- `ManualImports (0x01)`: Tüm import'ları manuel resolve eder (kernel32.dll, user32.dll, etc.)
- `CreateLdrRef (0x02)`: DLL'i Windows Loader Data Table'a kaydeder - **Exception handling için KRİTİK!**
- `ResolveImports (0x04)`: Delay-load import'ları çözer, bağımlılıkları yükler

##### 3. Stabilizasyon Süreleri Artırıldı ✅
```cpp
// ÖNCEKİ:
options.stabilityDelayMs = 1000;  // 1 saniye
std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // +2 saniye

// YENİ:
options.stabilityDelayMs = 1500;  // 1.5 saniye  
std::this_thread::sleep_for(std::chrono::milliseconds(3000)); // +3 saniye
// TOPLAM: 4.5 saniye (önceki: 3 saniye)
```

**Neden Daha Uzun Süre:**
- TLS callbacks'lerin tamamlanması
- Static C++ constructor'ların bitmesi
- DllMain PROCESS_ATTACH'ın çalışması
- Worker thread'lerin başlayıp stabilize olması
- MinHook initialization'ın tamamlanması
- Tüm hook'ların kurulması
- CS2 rendering thread'i ile senkronizasyon

### 📊 Etkilenen Dosyalar
- `src/manual_map.cpp` - Mapping flags ve asImage düzeltmesi
- `src/manual_map.h` - Varsayılan delay değerleri güncellendi
- `src/main.cpp` - Kullanıcı mesajları ve açıklamalar güncellendi
- `README.md` - v2.0.1 urgent fix bölümü eklendi
- `CRASH_FIX_NOTES.md` - Yeni detaylı açıklama dosyası

### 🎯 Beklenen Sonuç
✅ DLL başarıyla inject edilir  
✅ TLS callbacks çalışır  
✅ Static initialization tamamlanır  
✅ Exception handling düzgün çalışır  
✅ Tüm hook'lar kurulur  
✅ Present hook devreye girer  
✅ Menu açılır (INSERT tuşu)  
✅ **OYUN CRASH OLMAZ!** 🎯

### ⚠️ ÖNEMLİ NOTLAR
- `asImage=true` olmadan manuel map ile DLL injection **YAPILAMAZ**
- `CreateLdrRef` olmadan exception handling **ÇALIŞMAZ**
- `ManualImports` olmadan import'lar **RESOLVE EDİLMEZ**
- 3+ saniyelik stabilizasyon **KESİNLİKLE GEREKLİ**

---

## [2.0.0] - 2024-01-15

### 🎯 Büyük Yenilikler

#### ✨ Modül Senkronizasyonu Sistemi
- **Race Condition Önleme**: CS2'nin kritik modüllerinin tamamen yüklenmesini bekleyen mekanizma
- **Kontrol Edilen Modüller**: 
  - `client.dll` (Ana oyun mantığı)
  - `engine2.dll` (Motor çekirdeği)
  - `tier0.dll` (Sistem fonksiyonları)
  - `inputsystem.dll` (Giriş sistemi)
  - `scenesystem.dll` (Sahne yönetimi)
  - `rendersystemdx11.dll` (DirectX renderer)
- **Timeout Mekanizması**: 30 saniye içinde modüller yüklenmezse kullanıcıya seçenek sunma
- **Güvenlik Gecikmesi**: Modül yükleme sonrası 2 saniye ek initialization bekleme
- **Kullanıcı Geri Bildirimi**: Hangi modüllerin yüklendiğini gerçek zamanlı gösterme

**Yeni Fonksiyonlar:**
```cpp
bool WaitForCriticalModules(HANDLE hProcess, DWORD timeoutMs);
bool IsModuleLoaded(HANDLE hProcess, const std::string& moduleName);
uintptr_t GetModuleBaseAddress(HANDLE hProcess, const std::string& moduleName);
std::vector<std::string> EnumerateModules(HANDLE hProcess);
```

---

#### 📊 Kapsamlı Loglama Sistemi
- **Çift Katmanlı Log**: Hem konsola hem dosyaya yazma
- **Renkli Konsol**: Log seviyeleri için farklı renkler (INFO: Beyaz, WARNING: Sarı, ERROR: Kırmızı)
- **Thread-Safe**: Çoklu thread güvenliği ile mutex koruması
- **Zaman Damgası**: Milisaniye hassasiyetinde timestamp
- **Dosya Loglama**: `logs/injector_YYYYMMDD_HHMMSS.log` formatında otomatik dosya oluşturma
- **Sistem Bilgileri**: OS, CPU mimarisi, kullanıcı bilgileri kaydı
- **Win32 Hata Entegrasyonu**: GetLastError() otomatik açıklama
- **Exception Logging**: Crash detaylarını kaydetme

**Log Seviyeleri:**
- `INFO` - Normal bilgilendirme mesajları
- `WARNING` - Uyarılar (işlem devam eder)
- `ERROR` - Hatalar (işlem başarısız)
- `CRITICAL` - Kritik hatalar (çökme)
- `DEBUG` - Geliştirici debug bilgisi

**Yeni Dosyalar:**
- `src/logger.h` - Logger sınıfı tanımları
- `src/logger.cpp` - Logger implementasyonu

---

#### 🛡️ Structured Exception Handling (SEH)
- **Crash Yakalama**: İşlenmeyen exception'ları yakalama ve loglama
- **CPU Context Logging**: RIP, RSP, RBP, RAX register değerlerini kaydetme
- **Exception Parameters**: Tüm exception parametrelerini detaylı loglama
- **Graceful Shutdown**: Çökme öncesi log dosyasını kapatma ve flush etme

**Yeni Dosyalar:**
- `src/exception_handler.h` - SEH wrapper fonksiyonları

---

### 🔧 İyileştirmeler

#### Process Utils Genişletmeleri
**Dosya:** `src/process.cpp/h`
- Modül enumeration fonksiyonları
- Modül base address sorgulama
- Gelişmiş modül varlık kontrolü
- Psapi.lib entegrasyonu

#### Manuel Map Geliştirmeleri
**Dosya:** `src/manual_map.cpp`
- Her kritik adımda detaylı loglama
- Hata durumlarında Win32 error code loglama
- BlackBone işlem sonuçlarının kaydedilmesi
- Secondary header wipe başarı/başarısızlık raporlama

#### Main Loop İyileştirmeleri
**Dosya:** `src/main.cpp`
- Exception handler kurulumu (başlangıçta)
- Logger initialization ve sistem bilgileri kaydı
- Modül senkronizasyonu entegrasyonu
- Tüm kritik noktalarda detaylı loglama
- Graceful shutdown (exception handler kaldırma)

---

### 📝 Dokümantasyon

#### Yeni Belgeler:
1. **IMPROVEMENTS.md**
   - Yapılan tüm iyileştirmelerin detaylı açıklaması
   - Problem-çözüm senaryoları
   - Güvenlik ve anti-detection bilgileri
   - Sorun giderme rehberi
   - Örnek log çıktıları

2. **BUILD_INSTRUCTIONS.md**
   - Detaylı derleme talimatları
   - Visual Studio proje ayarları (adım adım)
   - Olası hataların çözümleri
   - Configuration seçenekleri (Debug vs Release)
   - Dependency kontrolü
   - Production checklist

3. **CHANGELOG.md** (bu dosya)
   - Sürüm geçmişi
   - Yeni özellikler
   - Değişiklikler
   - Bilinen sorunlar

---

### 🐛 Düzeltmeler

#### Stabilite İyileştirmeleri:
- ✅ Race condition sorunu çözüldü (modül senkronizasyonu ile)
- ✅ Crash durumlarında log kaybı önlendi (SEH ile)
- ✅ Process handle leak kontrolü eklendi
- ✅ Memory buffer'ların güvenli temizlenmesi (SecureZeroMemory)

#### Hata Yönetimi:
- ✅ Tüm Win32 API çağrılarında hata kontrolü
- ✅ GetLastError() değerlerinin anlamlı hale getirilmesi
- ✅ Kullanıcı dostu Türkçe + teknik İngilizce log mesajları
- ✅ Dosya I/O hatalarında graceful handling

---

### 🔄 Değişiklikler

#### Breaking Changes:
Hiçbir breaking change yok, mevcut DLL'ler uyumlu.

#### Behavior Changes:
- **Enjeksiyon süresi artabilir**: Modül bekleme nedeniyle 0-30 saniye arası gecikme
- **Log dosyaları oluşturulur**: `logs/` klasöründe otomatik log dosyaları
- **Konsol çıktısı renkli**: Farklı log seviyeleri için renk kodlaması

#### API Changes:
Yeni fonksiyonlar eklendi, mevcut API değişmedi:
```cpp
// process.h
+ bool WaitForCriticalModules(HANDLE hProcess, DWORD timeoutMs = 30000);
+ bool IsModuleLoaded(HANDLE hProcess, const std::string& moduleName);
+ uintptr_t GetModuleBaseAddress(HANDLE hProcess, const std::string& moduleName);
+ std::vector<std::string> EnumerateModules(HANDLE hProcess);

// logger.h (yeni dosya)
+ class Logger
+ void Log(LogLevel level, const std::string& message);
+ void LogWin32Error(const std::string& context, DWORD errorCode);
+ void LogException(DWORD exceptionCode, PVOID exceptionAddress, ...);

// exception_handler.h (yeni dosya)
+ void InstallExceptionHandler();
+ void RemoveExceptionHandler();
```

---

### 📦 Dependencies

#### Yeni Bağımlılıklar:
- `Psapi.lib` - Process enumeration için

#### Mevcut Bağımlılıklar (değişmedi):
- `kernel32.lib`
- `user32.lib`
- `advapi32.lib`
- BlackBone statik library

---

### ⚡ Performans

#### Bellek Kullanımı:
- Logger buffer: ~4KB (1000 satır için)
- Log dosyası: Disk I/O (asenkron flush)
- DLL file buffer: Geçici (enjeksiyon sonrası temizlenir)

#### Gecikme Eklemeleri:
- Modül bekleme: 0-30 saniye (CS2 başlatma hızına bağlı)
- Modül initialization: 2 saniye (sabit)
- Stability delay: 350ms (mevcut)
- **Toplam:** Eski versiyona göre +2-32 saniye

**Not:** Gecikme sadece ilk enjeksiyonda, CS2 zaten çalışıyorsa ~2.5 saniye

---

### 🔐 Güvenlik

#### Yeni Güvenlik Özellikleri:
- ✅ Exception handling ile bilgi sızıntısı önleme
- ✅ Detaylı loglama ile diagnostic (production'da kapatılabilir)
- ✅ Buffer temizleme (SecureZeroMemory)

#### Mevcut Özellikler (korundu):
- ✅ PE Header wiping (double wipe)
- ✅ BlackBone manual mapping
- ✅ Private memory allocation
- ✅ Debug privilege kullanımı

---

### 🧪 Test Edilen Ortamlar

#### İşletim Sistemleri:
- ✅ Windows 10 22H2 (x64)
- ✅ Windows 11 23H2 (x64)

#### Derleyiciler:
- ✅ Visual Studio 2019 (v142 toolset)
- ✅ Visual Studio 2022 (v143 toolset)

#### CS2 Durumları:
- ✅ Yeni başlatılan CS2 (soğuk başlangıç)
- ✅ Menüde bekleyen CS2
- ✅ Bot matchte CS2
- ❌ VAC korumalı sunucu (test edilmedi - ÖNERİLMEZ)

---

### 📈 Bilinen Sorunlar

#### Modül Bekleme:
- ⚠️ Bazı sistemlerde `rendersystemdx11.dll` geç yüklenebilir (timeout artırılabilir)
- ⚠️ CS2'nin `-novid` parametresi ile başlatılması modül yükleme süresini kısaltır

#### Loglama:
- ⚠️ Çok hızlı log yazımı disk I/O'yu yavaşlatabilir (nadir)
- ⚠️ Log dosyası silinmezse zamanla büyüyebilir (manuel temizlik gerekebilir)

#### Uyumluluk:
- ⚠️ Windows 7 desteği yok (Windows 10+ gerekli)
- ⚠️ ARM64 mimarisinde test edilmedi

---

### 🔮 Gelecek Planlar (v2.1.0+)

#### Öncelikli:
- [ ] Thread Hijacking implementasyonu
- [ ] APC Queue Injection alternatiifi
- [ ] Kernel-mode driver desteği
- [ ] Manual syscall kullanımı

#### Orta Öncelikli:
- [ ] GUI arayüz (Qt veya ImGui)
- [ ] Multi-DLL enjeksiyon desteği
- [ ] Profil sistemi (farklı oyunlar için)
- [ ] Auto-update mekanizması

#### Düşük Öncelikli:
- [ ] Code obfuscation entegrasyonu
- [ ] String encryption
- [ ] Import hiding
- [ ] Anti-anti-cheat research

---

### 🙏 Teşekkürler

Bu sürümde katkıda bulunan veya ilham veren:
- **BlackBone Library** - DarthTon
- **Windows Internals** - Alex Ionescu, Mark Russinovich
- **UnknownCheats Community** - Manuel mapping theory

---

## [1.0.0] - Initial Release

### Özellikler:
- ✅ BlackBone manuel haritalama
- ✅ PE header wiping
- ✅ CS2 process detection
- ✅ Administrator privilege check
- ✅ Anti-debug kontrolleri
- ✅ Basit konsol arayüzü

---

**Format:** [Major.Minor.Patch]
- **Major**: API değişiklikleri, breaking changes
- **Minor**: Yeni özellikler, geriye uyumlu
- **Patch**: Bug düzeltmeleri, küçük iyileştirmeler

**Not:** Semantik versiyonlama (SemVer 2.0.0) kullanılmaktadır.
