#pragma once
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "../utils/Vector.h"
#include "../entity/Classes.h"
#include "../utils/SafeMemory.h"
#include "../utils/Globals.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD(x) ((x) * (float)M_PI / 180.0f)
#define RAD2DEG(x) ((x) * 180.0f / (float)M_PI)

constexpr float CS_GRAVITY = 800.0f;
constexpr float CS_TICK_INTERVAL = 1.0f / 64.0f;


struct QAngle
{
    float pitch, yaw, roll;

    QAngle() : pitch(0), yaw(0), roll(0) {}
    QAngle(float p, float y, float r = 0.f) : pitch(p), yaw(y), roll(r) {}

    QAngle operator+(const QAngle& o) const { return { pitch + o.pitch, yaw + o.yaw, roll + o.roll }; }
    QAngle operator-(const QAngle& o) const { return { pitch - o.pitch, yaw - o.yaw, roll - o.roll }; }
    QAngle operator*(float f) const { return { pitch * f, yaw * f, roll * f }; }

    void Normalize()
    {
        while (yaw > 180.f) yaw -= 360.f;
        while (yaw < -180.f) yaw += 360.f;

        pitch = std::clamp(pitch, -89.f, 89.f);
        roll = 0.f;
    }
};

struct Color
{
    float r, g, b, a;

    Color(int R, int G, int B, int A = 255)
        : r(R / 255.f), g(G / 255.f), b(B / 255.f), a(A / 255.f) {
    }

    static Color Red() { return { 255, 0, 0 }; }
    static Color White() { return { 255, 255, 255 }; }
    static Color Yellow() { return { 255, 255, 0 }; }
};


enum class BoneID : int {
    Origin = 0,
    Pelvis = 1,
    Spine0 = 2,
    Spine1 = 3,
    Spine2 = 4,
    Neck = 6,
    Head = 7,
    ClavicleL = 8,
    ShoulderL = 9,
    ElbowL = 10,
    HandL = 11,
    ClavicleR = 12,
    ShoulderR = 13,
    ElbowR = 14,
    HandR = 15,
    HipL = 17,
    KneeL = 18,
    FootHeelL = 19,
    HipR = 20,
    KneeR = 21,
    FootHeelR = 22,
    Chest = 23,
    Gun = 24,
    EyeL = 25,
    EyeR = 26,
    Random = 27,
    CvjBone = 28,
    FootToesLT = 74,
    FootToesRT = 77,
    FootToesLCT = 81,
    FootToesRCT = 86,

    
    Spine = Spine1,
    LeftShoulder = ShoulderL,
    LeftArm = ElbowL,
    LeftHand = HandL,
    RightShoulder = ShoulderR,
    RightArm = ElbowR,
    RightHand = HandR,
    LeftHip = HipL,
    LeftKnee = KneeL,
    LeftFoot = FootHeelL,
    RightHip = HipR,
    RightKnee = KneeR,
    RightFoot = FootHeelR,
    LeftToes = FootToesLT,
    RightToes = FootToesRT
};


struct BoneConnection
{
    BoneID bone1;
    BoneID bone2;
};

namespace Bones
{
    inline const std::vector<BoneConnection> connections =
    {
        
        { BoneID::Pelvis, BoneID::Spine1 },
        { BoneID::Spine1, BoneID::Spine2 },
        { BoneID::Spine2, BoneID::Chest },
        { BoneID::Chest, BoneID::Neck },
        { BoneID::Neck, BoneID::Head },

        
        { BoneID::Neck, BoneID::ShoulderL },
        { BoneID::ShoulderL, BoneID::ElbowL },
        { BoneID::ElbowL, BoneID::HandL },

        
        { BoneID::Neck, BoneID::ShoulderR },
        { BoneID::ShoulderR, BoneID::ElbowR },
        { BoneID::ElbowR, BoneID::HandR },

        
        { BoneID::Pelvis, BoneID::HipL },
        { BoneID::HipL, BoneID::KneeL },
        { BoneID::KneeL, BoneID::FootHeelL },
        { BoneID::FootHeelL, BoneID::FootToesLT },

        
        { BoneID::Pelvis, BoneID::HipR },
        { BoneID::HipR, BoneID::KneeR },
        { BoneID::KneeR, BoneID::FootHeelR },
        { BoneID::FootHeelR, BoneID::FootToesRT }
    };
}


namespace Utils
{
    inline uintptr_t ResolveRelativeAddress(uintptr_t addr, int start, int end)
    {
        if (!addr) return 0;
        uint32_t offset = *reinterpret_cast<uint32_t*>(addr + start);
        return addr + end + offset;
    }

    inline bool IsValidPtr(uintptr_t addr)
    {
        return SafeMemory::IsValidPtr(addr);
    }

    template <typename T>
    inline T SafeRead(uintptr_t addr, T defaultVal = T())
    {
        return SafeMemory::Read<T>(addr, defaultVal);
    }

    template <typename T>
    inline bool SafeWrite(uintptr_t addr, T val)
    {
        return SafeMemory::Write<T>(addr, val);
    }

    template <typename T>
    inline bool SafeWriteProtected(uintptr_t addr, T val)
    {
        return SafeMemory::WriteProtected<T>(addr, val);
    }

    inline bool SafeReadString(uintptr_t addr, char* out, size_t maxLen = 64)
    {
        if (!addr || !out || maxLen == 0 || !IsValidPtr(addr))
            return false;

        
        memset(out, 0, maxLen);

        __try
        {
            for (size_t i = 0; i < maxLen - 1; i++)
            {
                if (!IsValidPtr(addr + i)) break;

                char c;
                memcpy(&c, reinterpret_cast<void*>(addr + i), 1);

                if (c == '\0')
                    return true;

                out[i] = c;
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline int SafeTeam(C_CSPlayerPawn* pawn)
    {
        if (!pawn) return -1;
        return SafeRead<uint8_t>(reinterpret_cast<uintptr_t>(pawn) + Offsets::m_iTeamNum, 255);
    }

    inline bool SafeAlive(C_CSPlayerPawn* pawn)
    {
        if (!pawn) return false;
        
        int hp = SafeRead<int>(reinterpret_cast<uintptr_t>(pawn) + Offsets::m_iHealth, 0);
        uint8_t ls = SafeRead<uint8_t>(reinterpret_cast<uintptr_t>(pawn) + Offsets::m_lifeState, 1);
        return hp > 0 && ls == 0;
    }

    inline void NormalizeAngles(Vector& a)
    {
        while (a.y > 180.f) a.y -= 360.f;
        while (a.y < -180.f) a.y += 360.f;

        a.x = std::clamp(a.x, -89.f, 89.f);
        a.z = 0.f;
    }

    inline Vector CalcAngle(const Vector& src, const Vector& dst)
    {
        Vector d = dst - src;
        float hyp = std::sqrt(d.x * d.x + d.y * d.y);

        return {
            -RAD2DEG(std::atan2(d.z, hyp)),
             RAD2DEG(std::atan2(d.y, d.x)),
             0.f
        };
    }

    inline float GetFoV(const Vector& view, const Vector& aim)
    {
        Vector delta = aim - view;
        NormalizeAngles(delta);
        return delta.Length();
    }

    inline void AngleVectors(const Vector& ang, Vector& forward)
    {
        float sp = sinf(DEG2RAD(ang.x));
        float cp = cosf(DEG2RAD(ang.x));
        float sy = sinf(DEG2RAD(ang.y));
        float cy = cosf(DEG2RAD(ang.y));

        forward = { cp * cy, cp * sy, -sp };
    }

    inline bool WorldToScreen(
        const Vector& world,
        Vector& screen,
        const float* matrix,
        float width,
        float height)
    {
        float w = matrix[12] * world.x + matrix[13] * world.y + matrix[14] * world.z + matrix[15];
        if (w < 0.01f)
            return false;

        float x = matrix[0] * world.x + matrix[1] * world.y + matrix[2] * world.z + matrix[3];
        float y = matrix[4] * world.x + matrix[5] * world.y + matrix[6] * world.z + matrix[7];

        float inv = 1.f / w;
        x *= inv;
        y *= inv;

        
        
        screen.x = (width * 0.5f) * (1.f + x) + Globals::GameViewportX;
        screen.y = (height * 0.5f) * (1.f - y) + Globals::GameViewportY;
        screen.z = 0.f;

        return true;
    }

    inline Vector GetBonePos(C_CSPlayerPawn* pawn, BoneID boneID)
    {
        if (!IsValidPtr(reinterpret_cast<uintptr_t>(pawn)))
            return {};

        uintptr_t scene = SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(pawn) + Offsets::m_pGameSceneNode);
        if (!IsValidPtr(scene))
            return {};

        uintptr_t boneArray = SafeRead<uintptr_t>(scene + Offsets::m_modelState + 0x80);
        if (!IsValidPtr(boneArray))
            return {};

        return SafeRead<Vector>(boneArray + static_cast<int>(boneID) * 0x20);
    }
}

