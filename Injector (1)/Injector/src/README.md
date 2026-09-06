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
- [Güvenlik ve Yasal](#-güvenlik-ve-yasal)

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

---

## 🚀 Hızlı Başlangıç

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