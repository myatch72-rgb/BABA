#pragma once
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <Windows.h>
#include "Vector.h"

class Raycasting
{
public:
    static Raycasting& Get()
    {
        static Raycasting instance;
        return instance;
    }

    
    bool Load(const std::string& mapName);

    
    void Unload();

    
    bool IsVisible(const Vector& start, const Vector& end) const;
    bool TraceRay(const Vector& start, const Vector& end, Vector& hitPoint) const;

    
    void UpdateMap();

    
    const std::string& GetCurrentMapName() const { return m_currentMapName; }

    
    bool IsLoaded() const { return m_root != nullptr; }

    
    bool IsDownloading() const { return m_downloadState == DownloadState::Downloading; }

    
    bool IsBoneVisible(const Vector& cameraPos, const Vector& bonePos) const;
    bool TraceHullSphere(const Vector& center, float radius) const;
    bool GetSurfaceNormal(const Vector& position, float searchRadius, Vector& collisionNormal) const;

private:
    Raycasting() = default;
    ~Raycasting() { 
        if (m_downloadThread) {
            WaitForSingleObject(m_downloadThread, INFINITE);
            CloseHandle(m_downloadThread);
            m_downloadThread = nullptr;
        }
    }
    Raycasting(const Raycasting&) = delete;
    Raycasting& operator=(const Raycasting&) = delete;

    enum class DownloadState { Idle, Downloading, Done, Failed };
    std::atomic<DownloadState> m_downloadState{ DownloadState::Idle };
    std::string m_downloadingMapName;
    HANDLE m_downloadThread = nullptr;

    static bool DownloadTriFile(const std::string& mapName, const std::string& outputPath);

    struct Triangle
    {
        Vector p1, p2, p3;
    };

    struct AABB
    {
        Vector min;
        Vector max;

        bool Intersects(const Vector& rayStart, const Vector& rayEnd) const;
        bool IntersectsSphere(const Vector& center, float radius) const;
    };

    struct Sphere
    {
        Vector center;
        float radius = 0.0f;
    };

    struct KDNode
    {
        AABB bounds;
        std::vector<Triangle> triangles;
        std::unique_ptr<KDNode> left;
        std::unique_ptr<KDNode> right;
        int axis = 0;
    };

    std::string m_currentMapName;
    std::unique_ptr<KDNode> m_root;
    mutable std::shared_mutex m_mutex;

    std::unique_ptr<KDNode> BuildKDTree(std::vector<Triangle>& triangles, int depth = 0);
    bool RayHitsNode(const KDNode* node, const Vector& start, const Vector& end) const;
    bool RayHitsTriangle(const Triangle& tri, const Vector& start, const Vector& end) const;
    bool RayHitsTriangle(const Triangle& tri, const Vector& start, const Vector& end, float& t) const;
    bool RayHitsNode(const KDNode* node, const Vector& start, const Vector& end, float& minT) const;
    bool SphereHitsNode(const KDNode* node, const Sphere& sphere) const;
    bool SphereHitsTriangle(const Triangle& tri, const Sphere& sphere) const;
    void CollectTrianglesNearPoint(const KDNode* node, const Sphere& sphere, std::vector<const Triangle*>& outTriangles) const;
    Vector ClosestPointOnTriangle(const Vector& point, const Triangle& tri, float& u, float& v, float& w) const;
    void UpdateClosestPointOnEdge(const Vector& point, const Vector& a, const Vector& b, Vector& closestPoint, float& minDistSq) const;
};
