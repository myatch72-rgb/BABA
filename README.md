# Raven CS2 Internal Project

Complete Counter-Strike 2 internal utility with Custom Model Changer, Inventory & Skin Changer, Legit Features, Visuals, and Native Injector.

---

## 📦 Proje İçeriği

- **Internal Base (avencash_backup_...)**: Visual Studio 2022 C++20 CS2 Internal DLL kaynak kodları.
- **Injector (1)**: CS2 Native & Manual Map Injector kaynak kodları.
- **MODELLER**: CS2 karakter modelleri (.vmdl_c, textures, materials):
  - 2B Nier Automata
  - Adult Neptune
  - Adult Jailer
  - Lego Batman
  - Muscular Gura
  - Spider-Man (No Way Home)
  - T-Rex Noun
  - Yoshino
- **inventory_data**: CS2 skin ve eşya veritabanı (skins.json).
- **TEK_PAKET**: Derlenmiş ve hazır kullanım dosyaları (aven.dll, Hellfire-Injector.exe, semirage.cfg, vb.).
- **MODELLERI_EKLE.bat**: Özel karakter modellerini tek tıkla otomatik olarak CS2 oyun dizinine kopyalar.

---

## 🚀 Hızlı Başlangıç

1. **Modelleri Yükleyin**:
   - MODELLERI_EKLE.bat dosyasını çalıştırın (karakter modellerini game\csgo\characters\ klasörüne yükler).

2. **Oyunu Başlatın**:
   - Counter-Strike 2'yi açın.

3. **Enjeksiyon**:
   - TEK_PAKET\Hellfire-Injector.exe veya HELLFIRE_INJECTOR.bat dosyasını yönetici olarak çalıştırın.
   - Injector CS2'yi otomatik algılayıp aven.dll dosyasını yükleyecektir.

4. **Menüyü Açın**:
   - Oyunda **INSERT** tuşuna basarak menüyü açabilirsiniz.

---

## 🛠️ Kaynak Koddan Derleme

- **Gereksinimler**: Visual Studio 2022 (v143 araç seti), C++20 desteği, Windows 10/11 SDK.
- **Çözümler**:
  - Internal Base.sln -> **Release | x64** modunda derleyin (aven.dll üretir).
  - Injector.sln -> **Release | x64** modunda derleyin (aven.exe üretir).
