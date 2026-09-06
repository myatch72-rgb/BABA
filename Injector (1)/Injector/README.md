# 🦅 RAVEN CS2 INJECTOR v2.0.0

**BlackBone Manuel Haritalama ile Gelişmiş CS2 DLL Enjektörü**

[![Platform](https://img.shields.io/badge/platform-Windows%2010%2B-blue)]()
[![Architecture](https://img.shields.io/badge/arch-x64-green)]()
[![Language](https://img.shields.io/badge/language-C%2B%2B20-orange)]()
[![License](https://img.shields.io/badge/license-Educational-yellow)]()

---

## 📋 İçindekiler

- [Özellikler](#-özellikler)
- [Yeni Neler? (v2.0.0)](#-yeni-neler-v200)
- [Hızlı Başlangıç](#-hızlı-başlangıç)
- [Derleme](#-derleme)
- [Kullanım](#-kullanım)
- [Dokümantasyon](#-dokümantasyon)
- [Güvenlik ve Yasal](#-güvenlik-ve-yasal)
- [Sorun Giderme](#-sorun-giderme)

---

## ✨ Özellikler

### 🎯 Ana Özellikler
- ✅ **BlackBone Manuel Haritalama** - Gelişmiş PE injection
- ✅ **PE Header Wiping** - Çift katmanlı header temizleme
- ✅ **VAD Hiding** - Sanal adres tanımlayıcısından gizleme
- ✅ **Modül Senkronizasyonu** - Race condition önleme (YENİ!)
- ✅ **Kapsamlı Loglama** - Dosya ve konsol logging (YENİ!)
- ✅ **Exception Handling** - Crash raporlama (YENİ!)

### 🛡️ Güvenlik
- ✅ Administrator yetki kontrolü
- ✅ SeDebugPrivilege acquisition
- ✅ Anti-debug kontrolleri
- ✅ x64 mimari doğrulama
- ✅ Güvenli bellek temizleme

### 🎨 Kullanıcı Deneyimi
- ✅ Türkçe arayüz
- ✅ Renkli konsol çıktısı
- ✅ Gerçek zamanlı durum gösterimi
- ✅ Detaylı hata mesajları
- ✅ Otomatik DLL arama

---

## 🆕 Yeni Neler? (v2.0.0)

### 1️⃣ Modül Senkronizasyonu Sistemi
**Problem:** DLL enjekte edildiğinde CS2'nin kritik modülleri henüz yüklenmemiş olabiliyordu, bu da çökmelere neden oluyordu.

**Çözüm:** Enjeksiyon öncesi aşağıdaki kritik modüllerin yüklenmesini bekler:
- `client.dll` - Ana oyun mantığı
- `engine2.dll` - Source 2 motor
- `tier0.dll` - Sistem fonksiyonları  
- `rendersystemdx11.dll` - DirectX renderer
- `inputsystem.dll` - Giriş sistemi
- `scenesystem.dll` - Sahne yönetimi

### 2️⃣ Kapsamlı Loglama Sistemi
**Tüm işlemler detaylı şekilde kaydedilir:**
- 📁 Dosya: `logs/injector_YYYYMMDD_HHMMSS.log`
- 🎨 Renkli konsol çıktısı (INFO: Beyaz, WARNING: Sarı, ERROR: Kırmızı)
- ⏱️ Milisaniye hassasiyetinde zaman damgası
- 🖥️ Sistem bilgileri (OS, CPU, kullanıcı)
- ⚠️ Win32 hata açıklamaları

### 3️⃣ Structured Exception Handling
**Çökme durumlarında detaylı crash raporu:**
- CPU register değerleri (RIP, RSP, RBP, RAX)
- Exception kodu ve adresi
- Stack trace bilgisi
- Tüm detaylar log dosyasına yazılır

---

## 🚀 Hızlı Başlangıç

### Gereksinimler:
- Windows 10/11 (x64)
- Administrator yetkileri
- `raven.dll` dosyası

### 3 Adımda Kullanım:

#### 1. Dosyaları Hazırlayın
```
TEK_PAKET/
├── INJECTOR_CALISTIR.bat
├── Raven-Injector.exe
└── raven.dll
```

#### 2. Çalıştırın
`INJECTOR_CALISTIR.bat` dosyasına çift tıklayın (Otomatik olarak Yönetici onayı ister ve pencere asla kapanmaz).

#### 3. CS2'yi Başlatın
Enjektör otomatik olarak CS2'yi ve kritik modüllerin yüklenmesini bekler, ardından enjekte eder.

---

## 🔨 Derleme

```cmd
cd "Injector (1)\Injector"
msbuild Injector.sln /p:Configuration=Release /p:Platform=x64
```

---

<div align="center">

**⚠️ UYARI: Sadece eğitim ve test amaçlıdır. Online kullanım yasaktır! ⚠️**

**🦅 RAVEN CS2 INJECTOR v2.0.0 🦅**

</div>
