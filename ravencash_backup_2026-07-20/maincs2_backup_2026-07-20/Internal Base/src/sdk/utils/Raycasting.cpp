#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Raycasting.h"
#include "../memory/Offsets.h"
#include "../memory/PatternScan.h"
#include "Utils.h"
#include "Globals.h"
#include <Windows.h>
#include <winhttp.h>
#include <cstdint>
#include <limits>
#include <stack>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

static float DotProduct(const Vector& a, const Vector& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vector CrossProduct(const Vector& a, const Vector& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static float LengthSquared(const Vector& v)
{
    return DotProduct(v, v);
}

static Vector NormalizeSafe(const Vector& v)
{
    float lenSq = LengthSquared(v);
    if (lenSq <= 1e-8f)
        return {};

    float invLen = 1.0f / std::sqrt(lenSq);
    return v * invLen;
}






static std::string GetMapsDirectory()
{
    
    return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\raven\\maps\\";
}

bool Raycasting::Load(const std::string& mapName)
{
    std::string path = GetMapsDirectory() + mapName;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file.seekg(0, std::ios::end);
    const size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size % sizeof(Triangle) != 0)
    {
        file.close();
        return false;
    }

    std::vector<Triangle> tris(size / sizeof(Triangle));
    if (!file.read(reinterpret_cast<char*>(tris.data()), size))
    {
        file.close();
        return false;
    }

    file.close();

    std::unique_lock lock(m_mutex);
    m_root = BuildKDTree(tris);

    return true;
}

void Raycasting::Unload()
{
    std::unique_lock lock(m_mutex);
    m_root.reset();
}

bool Raycasting::IsVisible(const Vector& start, const Vector& end) const
{
    std::shared_lock lock(m_mutex);
    if (!m_root)
    {
        return true; 
    }

    return !RayHitsNode(m_root.get(), start, end);
}

bool Raycasting::TraceRay(const Vector& start, const Vector& end, Vector& hitPoint) const
{
    std::shared_lock lock(m_mutex);
    if (!m_root)
    {
        hitPoint = end;
        return false;
    }

    float minT = 1.0f;
    if (RayHitsNode(m_root.get(), start, end, minT))
    {
        hitPoint = start + (end - start) * minT;
        return true;
    }

    hitPoint = end;
    return false;
}

bool Raycasting::IsBoneVisible(const Vector& cameraPos, const Vector& bonePos) const
{
    return IsVisible(cameraPos, bonePos);
}

bool Raycasting::TraceHullSphere(const Vector& center, float radius) const
{
    std::shared_lock lock(m_mutex);
    if (!m_root || radius <= 0.0f)
        return false;

    Sphere sphere{ center, radius };
    return SphereHitsNode(m_root.get(), sphere);
}

bool Raycasting::GetSurfaceNormal(const Vector& position, float searchRadius, Vector& collisionNormal) const
{
    std::shared_lock lock(m_mutex);
    if (!m_root || searchRadius <= 0.0f)
        return false;

    Sphere querySphere{ position, searchRadius };
    std::vector<const Triangle*> nearbyTriangles;
    nearbyTriangles.reserve(96);
    CollectTrianglesNearPoint(m_root.get(), querySphere, nearbyTriangles);

    if (nearbyTriangles.empty())
        return false;

    Vector referenceNormal{};
    bool hasReference = false;

    for (const Triangle* tri : nearbyTriangles)
    {
        Vector edge1 = tri->p2 - tri->p1;
        Vector edge2 = tri->p3 - tri->p1;
        Vector normal = NormalizeSafe(CrossProduct(edge1, edge2));
        if (LengthSquared(normal) > 1e-6f)
        {
            referenceNormal = normal;
            hasReference = true;
            break;
        }
    }

    if (!hasReference)
        return false;

    Vector normalAccum{};
    int validCount = 0;

    for (const Triangle* tri : nearbyTriangles)
    {
        Vector edge1 = tri->p2 - tri->p1;
        Vector edge2 = tri->p3 - tri->p1;
        Vector normal = NormalizeSafe(CrossProduct(edge1, edge2));
        if (LengthSquared(normal) <= 1e-6f)
            continue;

        if (DotProduct(normal, referenceNormal) >= 0.35f)
        {
            normalAccum += normal;
            ++validCount;
        }
    }

    if (validCount == 0)
        collisionNormal = referenceNormal;
    else
        collisionNormal = NormalizeSafe(normalAccum);

    return LengthSquared(collisionNormal) > 1e-6f;
}

static bool ReadMapNameSafe(uintptr_t mapNamePtr, char* outBuf, size_t bufSize)
{
    __try
    {
        memcpy(outBuf, reinterpret_cast<const void*>(mapNamePtr), bufSize - 1);
        outBuf[bufSize - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}




bool Raycasting::DownloadTriFile(const std::string& mapName, const std::string& outputPath)
{
    
    std::string urlPath = "/acanJcabBgS6KHNR/" + mapName + ".tri";

    
    std::wstring wHost = L"file.garden";
    std::wstring wPath(urlPath.begin(), urlPath.end());

    HINTERNET hSession = WinHttpOpen(L"AscaniaTriDownloader/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    
    std::string tempPath = outputPath + ".tmp";
    HANDLE hFile = CreateFileA(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD dwSize = 0, dwDownloaded = 0;
    bool success = true;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) { success = false; break; }
        if (dwSize == 0) break;

        std::vector<uint8_t> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) { success = false; break; }

        DWORD written = 0;
        if (!WriteFile(hFile, buffer.data(), dwDownloaded, &written, nullptr) || written != dwDownloaded) {
            success = false; break;
        }
    } while (dwSize > 0);

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (success) {
        
        DeleteFileA(outputPath.c_str()); 
        if (!MoveFileA(tempPath.c_str(), outputPath.c_str())) {
            DeleteFileA(tempPath.c_str());
            return false;
        }
        return true;
    } else {
        DeleteFileA(tempPath.c_str());
        return false;
    }
}

void Raycasting::UpdateMap()
{
    uintptr_t client = Memory::GetModuleBase("client.dll");
    if (!client)
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    
    uintptr_t globalVars = Utils::SafeRead<uintptr_t>(client + Offsets::dwGlobalVars);
    if (!globalVars || globalVars < 0x1000)
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    
    uintptr_t mapNamePtr = Utils::SafeRead<uintptr_t>(globalVars + 0x188);
    if (!mapNamePtr || mapNamePtr < 0x1000)
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    char buf[64]{};
    if (!ReadMapNameSafe(mapNamePtr, buf, sizeof(buf)))
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    std::string newMapName(buf);

    if (newMapName.empty())
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    
    newMapName.erase(0, newMapName.find_first_not_of(" \t\n\r"));
    if (!newMapName.empty())
        newMapName.erase(newMapName.find_last_not_of(" \t\n\r") + 1);

    
    bool weird = std::any_of(newMapName.begin(), newMapName.end(), [](unsigned char c) {
        return !std::isalnum(c) && c != '_' && c != '-' && c != '.' && c != ' ';
    });
    if (weird)
    {
        Unload();
        m_currentMapName.clear();
        return;
    }

    
    static const std::vector<std::string> badNames = { "<empty>", "SNDLVL_35dB", "SNDLVL_0dB", "default", "NULL" };
    for (const auto& bad : badNames)
    {
        if (newMapName == bad)
        {
            Unload();
            m_currentMapName.clear();
            return;
        }
    }

    
    if (newMapName == m_currentMapName)
    {
        return; 
    }

    
    if (m_downloadState == DownloadState::Done && m_downloadingMapName == newMapName)
    {
        
        if (m_downloadThread) {
            WaitForSingleObject(m_downloadThread, INFINITE);
            CloseHandle(m_downloadThread);
            m_downloadThread = nullptr;
        }

        m_downloadState = DownloadState::Idle;

        
        if (Load(newMapName + ".tri"))
        {
            m_currentMapName = newMapName;
        }
        return;
    }

    
    if (m_downloadState == DownloadState::Failed && m_downloadingMapName == newMapName)
    {
        if (m_downloadThread) {
            WaitForSingleObject(m_downloadThread, INFINITE);
            CloseHandle(m_downloadThread);
            m_downloadThread = nullptr;
        }
        m_downloadState = DownloadState::Idle;
        
        return;
    }

    
    if (m_downloadState == DownloadState::Downloading)
    {
        return;
    }

    
    Unload();

    
    std::string triPath = GetMapsDirectory() + newMapName + ".tri";
    if (std::filesystem::exists(triPath))
    {
        if (Load(newMapName + ".tri"))
        {
            m_currentMapName = newMapName;
        }
        return;
    }

    
    
    std::string mapsDir = GetMapsDirectory();
    CreateDirectoryA(mapsDir.c_str(), nullptr);

    m_downloadingMapName = newMapName;
    m_downloadState = DownloadState::Downloading;

    
    if (m_downloadThread) {
        WaitForSingleObject(m_downloadThread, INFINITE);
        CloseHandle(m_downloadThread);
        m_downloadThread = nullptr;
    }

    struct RaycastThreadArgs {
        Raycasting* instance;
        std::string mapName;
        std::string triPath;
    };

    RaycastThreadArgs* args = new RaycastThreadArgs{this, newMapName, triPath};

    m_downloadThread = CreateThread(nullptr, 0, [](LPVOID lpParam) -> DWORD {
        auto* args = static_cast<RaycastThreadArgs*>(lpParam);
        bool ok = args->instance->DownloadTriFile(args->mapName, args->triPath);
        args->instance->m_downloadState = ok ? DownloadState::Done : DownloadState::Failed;
        delete args;
        return 0;
    }, args, 0, nullptr);
}




bool Raycasting::AABB::Intersects(const Vector& rayStart, const Vector& rayEnd) const
{
    Vector dir = rayEnd - rayStart;

    float tmin = (min.x - rayStart.x) / dir.x;
    float tmax = (max.x - rayStart.x) / dir.x;
    if (tmin > tmax) std::swap(tmin, tmax);

    float tymin = (min.y - rayStart.y) / dir.y;
    float tymax = (max.y - rayStart.y) / dir.y;
    if (tymin > tymax) std::swap(tymin, tymax);

    if ((tmin > tymax) || (tymin > tmax))
        return false;

    if (tymin > tmin) tmin = tymin;
    if (tymax < tmax) tmax = tymax;

    float tzmin = (min.z - rayStart.z) / dir.z;
    float tzmax = (max.z - rayStart.z) / dir.z;
    if (tzmin > tzmax) std::swap(tzmin, tzmax);

    if ((tmin > tzmax) || (tzmin > tmax))
        return false;

    return true;
}

bool Raycasting::AABB::IntersectsSphere(const Vector& center, float radius) const
{
    float closestX = (std::max)(min.x, (std::min)(center.x, max.x));
    float closestY = (std::max)(min.y, (std::min)(center.y, max.y));
    float closestZ = (std::max)(min.z, (std::min)(center.z, max.z));

    Vector diff = Vector{ closestX, closestY, closestZ } - center;
    return LengthSquared(diff) <= radius * radius;
}




bool Raycasting::RayHitsTriangle(const Triangle& tri, const Vector& start, const Vector& end) const
{
    const float epsilon = 1e-6f;

    Vector dir = end - start;
    Vector edge1 = tri.p2 - tri.p1;
    Vector edge2 = tri.p3 - tri.p1;

    
    Vector h = {
        dir.y * edge2.z - dir.z * edge2.y,
        dir.z * edge2.x - dir.x * edge2.z,
        dir.x * edge2.y - dir.y * edge2.x
    };

    
    float a = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;
    if (a > -epsilon && a < epsilon)
        return false;

    float f = 1.0f / a;
    Vector s = start - tri.p1;

    
    float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.0f || u > 1.0f)
        return false;

    
    Vector q = {
        s.y * edge1.z - s.z * edge1.y,
        s.z * edge1.x - s.x * edge1.z,
        s.x * edge1.y - s.y * edge1.x
    };

    
    float v = f * (dir.x * q.x + dir.y * q.y + dir.z * q.z);
    if (v < 0.0f || u + v > 1.0f)
        return false;

    
    float t = f * (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z);
    return t > epsilon && t < 1.0f;
}

bool Raycasting::RayHitsTriangle(const Triangle& tri, const Vector& start, const Vector& end, float& t_out) const
{
    const float epsilon = 1e-6f;
    Vector dir = end - start;
    Vector edge1 = tri.p2 - tri.p1;
    Vector edge2 = tri.p3 - tri.p1;

    Vector h = {
        dir.y * edge2.z - dir.z * edge2.y,
        dir.z * edge2.x - dir.x * edge2.z,
        dir.x * edge2.y - dir.y * edge2.x
    };

    float a = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;
    if (a > -epsilon && a < epsilon)
        return false;

    float f = 1.0f / a;
    Vector s = start - tri.p1;

    float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.0f || u > 1.0f)
        return false;

    Vector q = {
        s.y * edge1.z - s.z * edge1.y,
        s.z * edge1.x - s.x * edge1.z,
        s.x * edge1.y - s.y * edge1.x
    };

    float v = f * (dir.x * q.x + dir.y * q.y + dir.z * q.z);
    if (v < 0.0f || u + v > 1.0f)
        return false;

    float t = f * (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z);
    if (t > epsilon && t < 1.0f) {
        t_out = t;
        return true;
    }
    return false;
}

bool Raycasting::RayHitsNode(const KDNode* node, const Vector& start, const Vector& end) const
{
    if (!node || !node->bounds.Intersects(start, end))
        return false;

    for (const auto& tri : node->triangles)
    {
        if (RayHitsTriangle(tri, start, end))
            return true;
    }

    return RayHitsNode(node->left.get(), start, end) || RayHitsNode(node->right.get(), start, end);
}

bool Raycasting::RayHitsNode(const KDNode* node, const Vector& start, const Vector& end, float& minT) const
{
    if (!node || !node->bounds.Intersects(start, end))
        return false;

    bool hit = false;
    for (const auto& tri : node->triangles)
    {
        float t = 1.0f;
        if (RayHitsTriangle(tri, start, end, t))
        {
            if (t < minT) {
                minT = t;
                hit = true;
            }
        }
    }

    bool hitLeft = RayHitsNode(node->left.get(), start, end, minT);
    bool hitRight = RayHitsNode(node->right.get(), start, end, minT);

    return hit || hitLeft || hitRight;
}

bool Raycasting::SphereHitsTriangle(const Triangle& tri, const Sphere& sphere) const
{
    Vector edge1 = tri.p2 - tri.p1;
    Vector edge2 = tri.p3 - tri.p1;
    Vector normal = NormalizeSafe(CrossProduct(edge1, edge2));
    if (LengthSquared(normal) <= 1e-8f)
        return false;

    float planeDist = DotProduct(sphere.center - tri.p1, normal);
    if (std::fabs(planeDist) > sphere.radius)
        return false;

    Vector projectedCenter = sphere.center - normal * planeDist;
    float u = 0.0f, v = 0.0f, w = 0.0f;
    Vector closestPoint = ClosestPointOnTriangle(projectedCenter, tri, u, v, w);
    return LengthSquared(closestPoint - sphere.center) <= sphere.radius * sphere.radius;
}

bool Raycasting::SphereHitsNode(const KDNode* node, const Sphere& sphere) const
{
    if (!node || !node->bounds.IntersectsSphere(sphere.center, sphere.radius))
        return false;

    for (const auto& tri : node->triangles)
    {
        if (SphereHitsTriangle(tri, sphere))
            return true;
    }

    return SphereHitsNode(node->left.get(), sphere) || SphereHitsNode(node->right.get(), sphere);
}

void Raycasting::UpdateClosestPointOnEdge(const Vector& point, const Vector& a, const Vector& b, Vector& closestPoint, float& minDistSq) const
{
    Vector ab = b - a;
    float abLenSq = LengthSquared(ab);
    if (abLenSq <= 1e-8f)
        return;

    float t = DotProduct(point - a, ab) / abLenSq;
    t = (std::max)(0.0f, (std::min)(1.0f, t));

    Vector projection = a + ab * t;
    float distSq = LengthSquared(point - projection);
    if (distSq < minDistSq)
    {
        minDistSq = distSq;
        closestPoint = projection;
    }
}

Vector Raycasting::ClosestPointOnTriangle(const Vector& point, const Triangle& tri, float& u, float& v, float& w) const
{
    Vector edge1 = tri.p2 - tri.p1;
    Vector edge2 = tri.p3 - tri.p1;
    Vector normal = NormalizeSafe(CrossProduct(edge1, edge2));
    if (LengthSquared(normal) <= 1e-8f)
    {
        u = 1.0f;
        v = 0.0f;
        w = 0.0f;
        return tri.p1;
    }

    float planeDistance = DotProduct(point - tri.p1, normal);
    Vector projectedPoint = point - normal * planeDistance;

    Vector v0 = tri.p2 - tri.p1;
    Vector v1 = tri.p3 - tri.p1;
    Vector v2 = projectedPoint - tri.p1;

    float d00 = DotProduct(v0, v0);
    float d01 = DotProduct(v0, v1);
    float d11 = DotProduct(v1, v1);
    float d20 = DotProduct(v2, v0);
    float d21 = DotProduct(v2, v1);
    float denom = d00 * d11 - d01 * d01;

    if (std::fabs(denom) <= 1e-8f)
    {
        u = 1.0f;
        v = 0.0f;
        w = 0.0f;
        return tri.p1;
    }

    v = (d11 * d20 - d01 * d21) / denom;
    w = (d00 * d21 - d01 * d20) / denom;
    u = 1.0f - v - w;

    if (u >= 0.0f && v >= 0.0f && w >= 0.0f)
        return tri.p1 * u + tri.p2 * v + tri.p3 * w;

    Vector closestPoint = tri.p1;
    float minDistSq = LengthSquared(point - tri.p1);
    UpdateClosestPointOnEdge(point, tri.p1, tri.p2, closestPoint, minDistSq);
    UpdateClosestPointOnEdge(point, tri.p2, tri.p3, closestPoint, minDistSq);
    UpdateClosestPointOnEdge(point, tri.p3, tri.p1, closestPoint, minDistSq);
    return closestPoint;
}

void Raycasting::CollectTrianglesNearPoint(const KDNode* node, const Sphere& sphere, std::vector<const Triangle*>& outTriangles) const
{
    if (!node)
        return;

    std::stack<const KDNode*> stack;
    stack.push(node);

    while (!stack.empty())
    {
        const KDNode* current = stack.top();
        stack.pop();

        if (!current || !current->bounds.IntersectsSphere(sphere.center, sphere.radius))
            continue;

        for (const auto& tri : current->triangles)
        {
            if (SphereHitsTriangle(tri, sphere))
                outTriangles.push_back(&tri);
        }

        if (current->left)
            stack.push(current->left.get());
        if (current->right)
            stack.push(current->right.get());
    }
}




std::unique_ptr<Raycasting::KDNode> Raycasting::BuildKDTree(std::vector<Triangle>& triangles, int depth)
{
    if (triangles.empty())
        return nullptr;

    AABB bounds = { triangles[0].p1, triangles[0].p1 };
    for (const auto& tri : triangles)
    {
        for (const auto& p : { tri.p1, tri.p2, tri.p3 })
        {
            bounds.min.x = (std::min)(bounds.min.x, p.x);
            bounds.min.y = (std::min)(bounds.min.y, p.y);
            bounds.min.z = (std::min)(bounds.min.z, p.z);
            bounds.max.x = (std::max)(bounds.max.x, p.x);
            bounds.max.y = (std::max)(bounds.max.y, p.y);
            bounds.max.z = (std::max)(bounds.max.z, p.z);
        }
    }

    auto node = std::make_unique<KDNode>();
    node->bounds = bounds;
    node->axis = depth % 3;

    if (triangles.size() <= 4)
    {
        node->triangles = std::move(triangles);
        return node;
    }

    auto axis = node->axis;
    auto mid = triangles.size() / 2;
    auto comparator = [axis](const Triangle& a, const Triangle& b) {
        auto getAxisValue = [axis](const Vector& v) -> float {
            switch (axis)
            {
            case 0: return v.x;
            case 1: return v.y;
            default: return v.z;
            }
        };

        float a_mid = (getAxisValue(a.p1) + getAxisValue(a.p2) + getAxisValue(a.p3)) / 3.0f;
        float b_mid = (getAxisValue(b.p1) + getAxisValue(b.p2) + getAxisValue(b.p3)) / 3.0f;
        return a_mid < b_mid;
    };

    std::nth_element(triangles.begin(), triangles.begin() + mid, triangles.end(), comparator);
    std::vector<Triangle> left(triangles.begin(), triangles.begin() + mid);
    std::vector<Triangle> right(triangles.begin() + mid, triangles.end());

    node->left = BuildKDTree(left, depth + 1);
    node->right = BuildKDTree(right, depth + 1);
    return node;
}
