#define _USE_MATH_DEFINES
#include "Multipoint.h"
#include <cmath>

namespace Multipoint
{
    static Vector QuaternionRotate(const CTransform& t, const Vector& v)
    {
        const float x = t.quatOrientation.x;
        const float y = t.quatOrientation.y;
        const float z = t.quatOrientation.z;
        const float w = t.quatOrientation.w;
     
        const float tx = 2.0f * (y * v.z - z * v.y);
        const float ty = 2.0f * (z * v.x - x * v.z);
        const float tz = 2.0f * (x * v.y - y * v.x);
     
        return {
            v.x + w * tx + (y * tz - z * ty),
            v.y + w * ty + (z * tx - x * tz),
            v.z + w * tz + (x * ty - y * tx)
        };
    }
     
    static Vector TransformPoint(const CTransform& t, const Vector& v)
    {
        const Vector r = QuaternionRotate(t, v);
        return { r.x + t.vecPosition.x, r.y + t.vecPosition.y, r.z + t.vecPosition.z };
    }

    std::vector<HitboxPointSet> GetHitboxWorldPoints(C_CSPlayerPawn* pawn, float pointscale, int targetHitbox)
    {
        std::vector<HitboxPointSet> result;
        if (!pawn)
            return result;
     
        CHitBoxSet* hitbox_set = pawn->GetHitboxSet(0);
        if (!hitbox_set)
            return result;
     
        auto& hitboxes = hitbox_set->m_HitBoxes;
        int hitbox_count = hitboxes.Count();
        if (hitbox_count <= 0)
            return result;
     
        if (hitbox_count > HITBOX_MAX)
            hitbox_count = HITBOX_MAX;
     
        CTransform transforms[HITBOX_MAX] = {};
        if (pawn->HitboxToWorldTransform(hitbox_set, transforms) == -1)
            return result;
     
        for (int i = 0; i < hitbox_count; ++i)
        {
            CHitBox* hb = &hitboxes[i];
            if (!hb)
                continue;
            
            // Only generate points for the targeted hitbox to save performance, 
            // unless targetHitbox == -1 (generate for all)
            if (targetHitbox != -1 && hb->m_nHitBoxIndex != targetHitbox)
                continue;

            const CTransform& tr = transforms[i];
            const float radius = hb->m_flShapeRadius;
     
            HitboxPointSet set;
            set.arrayIndex = i;
            set.hitboxId   = hb->m_nHitBoxIndex;
            set.shapeType  = hb->nShapeType;
            set.name       = hb->m_name.m_pString;
     
            if (hb->nShapeType == 0) // Box
            {
                Vector minB = hb->m_vMinBounds - Vector{radius, radius, radius};
                Vector maxB = hb->m_vMaxBounds + Vector{radius, radius, radius};
     
                const Vector corners[8] = {
                    { minB.x, minB.y, minB.z }, { maxB.x, minB.y, minB.z },
                    { maxB.x, maxB.y, minB.z }, { minB.x, maxB.y, minB.z },
                    { minB.x, minB.y, maxB.z }, { maxB.x, minB.y, maxB.z },
                    { maxB.x, maxB.y, maxB.z }, { minB.x, maxB.y, maxB.z },
                };
                for (const auto& c : corners)
                    set.points.push_back(TransformPoint(tr, c));
     
                set.points.push_back(TransformPoint(tr, { (minB.x + maxB.x) * 0.5f, (minB.y + maxB.y) * 0.5f, minB.z }));
                set.points.push_back(TransformPoint(tr, { (minB.x + maxB.x) * 0.5f, (minB.y + maxB.y) * 0.5f, maxB.z }));
                set.points.push_back(TransformPoint(tr, { (minB.x + maxB.x) * 0.5f, minB.y, (minB.z + maxB.z) * 0.5f }));
                set.points.push_back(TransformPoint(tr, { (minB.x + maxB.x) * 0.5f, maxB.y, (minB.z + maxB.z) * 0.5f }));
                set.points.push_back(TransformPoint(tr, { minB.x, (minB.y + maxB.y) * 0.5f, (minB.z + maxB.z) * 0.5f }));
                set.points.push_back(TransformPoint(tr, { maxB.x, (minB.y + maxB.y) * 0.5f, (minB.z + maxB.z) * 0.5f }));
            }
            else if (hb->nShapeType == 1) // Sphere
            {
                Vector center = (hb->m_vMinBounds + hb->m_vMaxBounds) * 0.5f;
                if (radius > 0.0f)
                {
                    constexpr int numPoints = 12; // Reduced from 20 for performance
                    float goldenAngle = static_cast<float>(M_PI * (3.0 - std::sqrt(5.0)));
                    for (int j = 0; j < numPoints; ++j)
                    {
                        float y = 1.0f - (j / static_cast<float>(numPoints - 1)) * 2.0f;
                        float radiusAtY = std::sqrtf(1.0f - y * y);
                        float theta = goldenAngle * j;
                        float x = std::cosf(theta) * radiusAtY;
                        float z = std::sinf(theta) * radiusAtY;
                        Vector local = center + Vector{ x, y, z } * radius;
                        set.points.push_back(TransformPoint(tr, local));
                    }
                }
            }
            else if (hb->nShapeType == 2) // Capsule
            {
                Vector axis = hb->m_vMaxBounds - hb->m_vMinBounds;
                float axisLen = std::sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                if (axisLen < 0.001f || radius <= 0.0f)
                    continue;
     
                Vector axisDir = axis * (1.0f / axisLen);
                Vector center  = (hb->m_vMinBounds + hb->m_vMaxBounds) * 0.5f;
     
                Vector arbitrary = (std::abs(axisDir.x) < 0.99f) ? Vector{ 1, 0, 0 } : Vector{ 0, 1, 0 };
     
                Vector u{
                    axisDir.y * arbitrary.z - axisDir.z * arbitrary.y,
                    axisDir.z * arbitrary.x - axisDir.x * arbitrary.z,
                    axisDir.x * arbitrary.y - axisDir.y * arbitrary.x
                };
                float uLen = std::sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
                if (uLen > 0.0f) { u.x /= uLen; u.y /= uLen; u.z /= uLen; }
     
                Vector v{
                    axisDir.y * u.z - axisDir.z * u.y,
                    axisDir.z * u.x - axisDir.x * u.z,
                    axisDir.x * u.y - axisDir.y * u.x
                };
                float vLen = std::sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
                if (vLen > 0.0f) { v.x /= vLen; v.y /= vLen; v.z /= vLen; }
     
                constexpr int cylinderRings = 2; // Reduced for performance
                constexpr int ringPoints    = 4; // Reduced for performance
                constexpr int capRings      = 1;
                constexpr int capRingPoints = 4;
     
                for (int ring = 0; ring < cylinderRings; ++ring)
                {
                    float t = -axisLen * 0.5f + axisLen * (ring / static_cast<float>(cylinderRings - 1));
                    for (int p = 0; p < ringPoints; ++p)
                    {
                        float angle = (2.0f * static_cast<float>(M_PI) * p) / ringPoints;
                        float c = std::cosf(angle);
                        float s = std::sinf(angle);
                        Vector local = center
                            + axisDir * t
                            + Vector{ u.x * c + v.x * s, u.y * c + v.y * s, u.z * c + v.z * s } * radius;
                        set.points.push_back(TransformPoint(tr, local));
                    }
                }
     
                Vector tipTop    = center + axisDir * (axisLen * 0.5f + radius);
                Vector tipBottom = center - axisDir * (axisLen * 0.5f + radius);
                set.points.push_back(TransformPoint(tr, tipTop));
                set.points.push_back(TransformPoint(tr, tipBottom));
     
                for (int cap = 0; cap < 2; ++cap)
                {
                    float capSign = (cap == 0) ? -1.0f : 1.0f;
                    Vector capCenter = center + axisDir * (capSign * axisLen * 0.5f);
                    for (int ring = 1; ring <= capRings; ++ring)
                    {
                        float lat = (static_cast<float>(M_PI) * 0.5f * ring) / (capRings + 1);
                        float ringR = radius * std::cosf(lat);
                        float zOff  = radius * std::sinf(lat);
                        for (int p = 0; p < capRingPoints; ++p)
                        {
                            float angle = (2.0f * static_cast<float>(M_PI) * p) / capRingPoints;
                            float c = std::cosf(angle);
                            float s = std::sinf(angle);
                            Vector offset{
                                u.x * c * ringR + v.x * s * ringR + axisDir.x * (capSign * zOff),
                                u.y * c * ringR + v.y * s * ringR + axisDir.y * (capSign * zOff),
                                u.z * c * ringR + v.z * s * ringR + axisDir.z * (capSign * zOff)
                            };
                            set.points.push_back(TransformPoint(tr, capCenter + offset));
                        }
                    }
                }
            }
     
            if (!set.points.empty())
            {
                Vector localCenter = (hb->m_vMinBounds + hb->m_vMaxBounds) * 0.5f;
                Vector worldCenter = TransformPoint(tr, localCenter);
                
                // Add center point as the FIRST point (highest priority usually)
                set.points.insert(set.points.begin(), worldCenter);

                // Apply point scale to shrink points towards the center
                for (auto& p : set.points)
                {
                    p = worldCenter + (p - worldCenter) * pointscale;
                }
                result.push_back(std::move(set));
            }
        }
     
        return result;
    }
}
