#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wincodec.h>
#include <wininet.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <Windows.h>

#include "../entity/EntityManager.h"
#include "../memory/Offsets.h"
#include "Utils.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "windowscodecs.lib")

// Steam Avatar Manager
// Steam'den avatar indirir ve D3D11 texture olarak cache'ler
class SteamAvatarCache
{
public:
    static SteamAvatarCache& Get()
    {
        static SteamAvatarCache instance;
        return instance;
    }

    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_device = device;
        m_context = context;

        // WIC factory oluştur
        if (!m_wicFactory)
        {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&m_wicFactory));
        }

        // Worker thread'i başlat
        if (!m_running)
        {
            m_running = true;
            m_worker = CreateThread(nullptr, 0, [](LPVOID lpParam) -> DWORD {
                auto* instance = static_cast<SteamAvatarCache*>(lpParam);
                instance->WorkerThread();
                return 0;
            }, this, 0, nullptr);
        }
    }

    // SteamID için avatar texture al (FAST - minimal lock)
    ID3D11ShaderResourceView* GetAvatar(uint64_t steamID)
    {
        if (steamID == 0 || !m_device) return nullptr;

        // Cache'de var mı? (hızlı lookup)
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            auto it = m_cache.find(steamID);
            if (it != m_cache.end())
                return it->second;
        }

        // İndirme kuyruğuna ekle (bir kez)
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_requested.find(steamID) == m_requested.end())
            {
                m_queue.push(steamID);
                m_requested.insert(steamID);
            }
        }

        return nullptr;
    }

    // Her frame çağrılmalı - indirilen avatarları texture'a çevirir (OPTIMIZED)
    void ProcessFinished()
    {
        if (!m_device || !m_wicFactory) return;

        // Periyodik temizleme (5 dakikada bir) - cache boyutu 100'ü geçerse
        static auto s_lastCleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastCleanup).count();

        if (elapsed > 300) // 5 dakika
        {
            // Cache boyut kontrolü (atomic - lock-free!)
            if (m_cache_size.load() > 100)
            {
                CleanupUnusedAvatars();
                s_lastCleanup = now;
            }
        }

        // İndirilen avatarları işle
        std::vector<PendingDownload> finished;
        {
            std::lock_guard<std::mutex> lock(m_finished_mutex);
            if (m_finished.empty()) return; // Boşsa erken çık
            finished = std::move(m_finished);
            m_finished.clear();
        }

        // Texture oluşturma (lock dışında, yavaş işlem)
        for (auto& p : finished)
        {
            if (p.failed || p.image_data.empty())
                continue;

            // WIC ile decode et
            IWICStream* stream = nullptr;
            if (FAILED(m_wicFactory->CreateStream(&stream)))
                continue;

            if (FAILED(stream->InitializeFromMemory(p.image_data.data(), static_cast<DWORD>(p.image_data.size()))))
            {
                stream->Release();
                continue;
            }

            IWICBitmapDecoder* decoder = nullptr;
            if (FAILED(m_wicFactory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
            {
                stream->Release();
                continue;
            }

            IWICBitmapFrameDecode* frame = nullptr;
            if (FAILED(decoder->GetFrame(0, &frame)))
            {
                decoder->Release();
                stream->Release();
                continue;
            }

            IWICFormatConverter* converter = nullptr;
            if (FAILED(m_wicFactory->CreateFormatConverter(&converter)))
            {
                frame->Release();
                decoder->Release();
                stream->Release();
                continue;
            }

            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            {
                converter->Release();
                frame->Release();
                decoder->Release();
                stream->Release();
                continue;
            }

            UINT width = 0, height = 0;
            converter->GetSize(&width, &height);

            std::vector<uint8_t> pixels(width * height * 4);
            converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());

            // D3D11 texture oluştur
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = pixels.data();
            initData.SysMemPitch = width * 4;

            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(m_device->CreateTexture2D(&desc, &initData, &texture)))
            {
                ID3D11ShaderResourceView* srv = nullptr;
                if (SUCCEEDED(m_device->CreateShaderResourceView(texture, nullptr, &srv)))
                {
                    std::lock_guard<std::mutex> lock(m_cache_mutex);
                    m_cache[p.steam_id] = srv;
                    m_cache_size.store(m_cache.size()); // Atomic update
                    // Sadece başarılı yüklemeleri logla
                    printf("[AVATAR] Yuklendi: %llu\n", p.steam_id);
                }
                texture->Release();
            }

            converter->Release();
            frame->Release();
            decoder->Release();
            stream->Release();
        }
    }

    void Clear()
    {
        m_running = false;
        if (m_worker) {
            WaitForSingleObject(m_worker, INFINITE);
            CloseHandle(m_worker);
            m_worker = nullptr;
        }

        std::lock_guard<std::mutex> lock(m_cache_mutex);
        for (auto& pair : m_cache)
        {
            if (pair.second)
                pair.second->Release();
        }
        m_cache.clear();
        m_cache_size.store(0); // Atomic reset

        if (m_wicFactory)
        {
            m_wicFactory->Release();
            m_wicFactory = nullptr;
        }
    }

    void ResetForDisconnect()
    {
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            for (auto& pair : m_cache)
            {
                if (pair.second)
                    pair.second->Release();
            }
            m_cache.clear();
            m_cache_size.store(0);
        }

        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            std::queue<uint64_t> emptyQueue;
            std::swap(m_queue, emptyQueue);
            m_requested.clear();
        }

        {
            std::lock_guard<std::mutex> lock(m_finished_mutex);
            m_finished.clear();
        }
    }

    // Kullanılmayan avatarları temizle (OPTIMIZED - round-aware)
    void CleanupUnusedAvatars()
    {
        // Şu anda oyunda olan oyuncuların steamID'lerini al
        std::unordered_set<uint64_t> active_players;
        const auto entities = EntityManager::Get().GetEntities();
        int count = (int)entities.size();
        
        // Round değişimi algılama (oyuncu sayısı aniden düştüyse round değişmiş olabilir)
        static int s_lastPlayerCount = 0;
        static auto s_lastCountChange = std::chrono::steady_clock::now();
        
        auto now = std::chrono::steady_clock::now();
        auto timeSinceChange = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastCountChange).count();
        
        // Oyuncu sayısı %50'den fazla düştüyse ve 10 saniyeden az süre geçtiyse → round değişimi
        bool possibleRoundChange = (count < s_lastPlayerCount / 2) && (timeSinceChange < 10);
        
        if (count != s_lastPlayerCount) {
            s_lastPlayerCount = count;
            s_lastCountChange = now;
        }
        
        // Round değişimi sırasında cleanup yapma (avatarlar kaybolmasın)
        if (possibleRoundChange || count == 0)
            return;
        
        for (const auto& ent : entities)
        {
            if (ent.controller)
            {
                uint64_t steamID = Utils::SafeRead<uint64_t>(reinterpret_cast<uintptr_t>(ent.controller) + Offsets::m_steamID);
                if (steamID != 0)
                    active_players.insert(steamID);
            }
        }

        int cache_removed = 0;
        int requested_removed = 0;

        // Cache temizleme
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            auto it = m_cache.begin();
            while (it != m_cache.end())
            {
                if (active_players.find(it->first) == active_players.end())
                {
                    if (it->second)
                        it->second->Release();
                    it = m_cache.erase(it);
                    cache_removed++;
                }
                else
                {
                    ++it;
                }
            }
            m_cache_size.store(m_cache.size()); // Atomic update
        }

        // Requested set temizleme (ayrı lock - deadlock önleme)
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            auto it = m_requested.begin();
            while (it != m_requested.end())
            {
                uint64_t steamID = *it;
                bool inGame = (active_players.find(steamID) != active_players.end());
                
                // Oyunda değil → sil
                if (!inGame)
                {
                    it = m_requested.erase(it);
                    requested_removed++;
                }
                else
                {
                    ++it;
                }
            }
        }

        // Sadece bir şey silindiyse logla
        if (cache_removed > 0 || requested_removed > 0)
        {
            printf("[AVATAR] Temizlendi: %d cache, %d requested\n", cache_removed, requested_removed);
        }
    }

    ~SteamAvatarCache()
    {
        Clear();
    }

private:
    SteamAvatarCache() = default;

    struct PendingDownload
    {
        uint64_t steam_id = 0;
        std::vector<uint8_t> image_data;
        bool finished = false;
        bool failed = false;
    };

    // Worker thread - Steam'den avatar indirir
    void WorkerThread()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        while (m_running)
        {
            uint64_t steam_id = 0;
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                if (!m_queue.empty())
                {
                    steam_id = m_queue.front();
                    m_queue.pop();
                }
            }

            if (!steam_id)
            {
                Sleep(100);
                continue;
            }

            // Steam profilinin XML verisini çek
            char profile_url[256];
            snprintf(profile_url, sizeof(profile_url), "https://steamcommunity.com/profiles/%llu/?xml=1", steam_id);
            std::string xml = GetString(profile_url);

            std::string avatar_url;
            if (!xml.empty())
            {
                // XML'den avatar URL'ini çıkar
                auto extract_tag = [&](const std::string& tag) -> std::string {
                    size_t start_tag = xml.find("<" + tag + ">");
                    if (start_tag == std::string::npos) return "";
                    
                    start_tag += tag.length() + 2;
                    size_t end_tag = xml.find("</" + tag + ">", start_tag);
                    if (end_tag == std::string::npos) return "";
                    
                    std::string content = xml.substr(start_tag, end_tag - start_tag);
                    if (content.find("<![CDATA[") == 0 && content.find("]]>") != std::string::npos)
                        return content.substr(9, content.length() - 12);
                    
                    return content;
                };

                // En yüksek çözünürlükten başla
                avatar_url = extract_tag("avatarFull");
                if (avatar_url.empty()) avatar_url = extract_tag("avatarMedium");
                if (avatar_url.empty()) avatar_url = extract_tag("avatarIcon");
            }

            PendingDownload p;
            p.steam_id = steam_id;

            if (!avatar_url.empty())
            {
                p.image_data = GetBytes(avatar_url);
                p.finished = true;
                p.failed = p.image_data.empty();
                
                if (p.failed)
                {
                    printf("[AVATAR] Indirme basarisiz: %llu\n", steam_id);
                }
            }
            else
            {
                p.finished = true;
                p.failed = true;
                printf("[AVATAR] URL bulunamadi: %llu\n", steam_id);
            }

            // Ana thread'e gönder
            {
                std::lock_guard<std::mutex> lock(m_finished_mutex);
                m_finished.push_back(std::move(p));
            }
        }

        CoUninitialize();
    }

    // URL'den string indir (XML için)
    std::string GetString(const std::string& url)
    {
        std::string result;
        HINTERNET h_internet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (h_internet)
        {
            DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
            if (url.find("https://") == 0)
                flags |= INTERNET_FLAG_SECURE;

            HINTERNET h_url = InternetOpenUrlA(h_internet, url.c_str(), NULL, 0, flags, 0);
            if (h_url)
            {
                char buffer[4096];
                DWORD bytes_read = 0;
                while (InternetReadFile(h_url, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0)
                {
                    result.append(buffer, bytes_read);
                }
                InternetCloseHandle(h_url);
            }
            InternetCloseHandle(h_internet);
        }
        return result;
    }

    // URL'den bytes indir (resim için)
    std::vector<uint8_t> GetBytes(const std::string& url)
    {
        std::vector<uint8_t> result;
        HINTERNET h_internet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (h_internet)
        {
            DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
            if (url.find("https://") == 0)
                flags |= INTERNET_FLAG_SECURE;

            HINTERNET h_url = InternetOpenUrlA(h_internet, url.c_str(), NULL, 0, flags, 0);
            if (h_url)
            {
                char buffer[4096];
                DWORD bytes_read = 0;
                while (InternetReadFile(h_url, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0)
                {
                    const auto start = result.size();
                    result.resize(start + bytes_read);
                    std::memcpy(result.data() + start, buffer, bytes_read);
                }
                InternetCloseHandle(h_url);
            }
            InternetCloseHandle(h_internet);
        }
        return result;
    }

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IWICImagingFactory* m_wicFactory = nullptr;
    
    std::unordered_map<uint64_t, ID3D11ShaderResourceView*> m_cache;
    std::mutex m_cache_mutex;
    std::atomic<size_t> m_cache_size{0}; // Thread-safe cache size tracking
    
    std::queue<uint64_t> m_queue;
    std::unordered_set<uint64_t> m_requested;
    std::mutex m_queue_mutex;
    
    std::vector<PendingDownload> m_finished;
    std::mutex m_finished_mutex;
    
    HANDLE m_worker = nullptr;
    std::atomic<bool> m_running{false};
};
