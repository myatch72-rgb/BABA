#pragma once
#include <cmath>

struct Vector {
    float x, y, z;

    Vector() : x(0.f), y(0.f), z(0.f) {}
    Vector(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector operator+(const Vector& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vector operator-(const Vector& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vector operator*(float f) const { return { x * f, y * f, z * f }; }

    Vector& operator+=(const Vector& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vector& operator-=(const Vector& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    Vector operator/(float f) const { return { x / f, y / f, z / f }; }

    void Normalize()
    {
        while (y > 180.f) y -= 360.f;
        while (y < -180.f) y += 360.f;
        if (x > 89.f) x = 89.f;
        if (x < -89.f) x = -89.f;
        z = 0.f;
    }

    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float Dot(const Vector& o) const { return x * o.x + y * o.y + z * o.z; }
    bool IsZero() const { return x == 0.f && y == 0.f && z == 0.f; }
};
