#pragma once

#include <cstdint>
#include "../utils/Vector.h"

struct CUtlString {
    const char* m_pString;
};

template <typename T>
class CUtlVector {
public:
    int m_Size;
    int m_Pad;
    T* m_pElements;

    int Count() const { return m_Size; }
    T& operator[](int i) { return m_pElements[i]; }
    const T& operator[](int i) const { return m_pElements[i]; }
};

struct Quaternion {
    float x, y, z, w;
};

struct CTransform {
    Vector vecPosition;
    Quaternion quatOrientation;
};

struct CHitBox {
    CUtlString m_name;
    CUtlString m_sSurfaceProperty;
    CUtlString m_sBoneName;
    Vector     m_vMinBounds;
    Vector     m_vMaxBounds;
    float      m_flShapeRadius;
    uint32_t   m_nBoneNameHash;
    int32_t    m_nGroupId;
    uint8_t    nShapeType;
    bool       m_bTranslationOnly;
    uint32_t   m_CRC;
    uint16_t   m_nHitBoxIndex;
    uint8_t    m_cRenderColor[4];
    bool       m_bForcedTransform;
    CTransform m_forcedTransform;
};
 
class CHitBoxSet {
public:
    CUtlString          m_name;
    uint32_t            m_nNameHash;
    CUtlVector<CHitBox> m_HitBoxes;
    CUtlString          m_SourceFilename;
};
