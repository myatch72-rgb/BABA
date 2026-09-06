#pragma once
#include <cstdint>
#include "../utils/Vector.h"



struct Ray_t 
{
	struct line_t
	{
		Vector start_offset {};
		float radius {};
 
		line_t( ) = default;
	};
 
	struct sphere_t
	{
		Vector center {};
		float radius {};
 
		sphere_t( ) = default;
	};
 
	struct hull_t
	{
		Vector mins {};
		Vector maxs {};
 
		hull_t( ) = default;
	};
 
	struct capsule_t
	{
		Vector center[ 2 ] {};
		float radius {};
 
		capsule_t( ) = default;
	};
 
	struct mesh_t
	{
		Vector mins {};
		Vector maxs {};
		const Vector* vertecies {};
		int num_vertecies {};
 
		mesh_t( ) = default;
	};
 
	union
	{
		line_t line {};
		sphere_t sphere;
		hull_t hull;
		capsule_t capsule;
		mesh_t mesh;
	};
 
	Ray_t( )
	{
		type = 0; 
		line.start_offset = {};
		line.radius = 0.f;
	}
 
	void InitHull( const Vector& mins, const Vector& maxs )
	{
		type = 1; 
		hull.mins = mins;
		hull.maxs = maxs;
	}
 
    void Init(const Vector& start, const Vector& end) {
        type = 0;
        line.start_offset = start; 
    }

public:
	int type;
};

struct c_aggregate_object_data
{
public:
	char pad_0000[4]; 
	int count; 
	char pad_0008[40]; 
	int index; 
}; 
 
struct c_aggregate_object_array
{
public:
	void* object; 
	c_aggregate_object_data* data; 
};



class CAggregateSceneObjectDataWorld {
private:
    char pad_0000[0x38]; 
public:
    unsigned char r; 
    unsigned char g; 
    unsigned char b; 
private:
    char pad_0038[0x9];
};

struct LightDataQueue_t {
    void* light_data;
};

class ISceneSystem {
public:
    char pad_0000[0x8]; 
    LightDataQueue_t* light_data_queue;
};

struct GameTrace_t
{
public:
    GameTrace_t() = default;
 
    void* m_pSurface;                   
    void* m_pHitEntity;                 
    void* m_pHitboxData;                
    uint8_t pad0[0x40 - 0x18];          
 
    uint32_t m_uContents;               
    uint8_t pad1[0x78 - 0x44];          
 
    Vector m_vecStartPos;             
    Vector m_vecEndPos;               
    Vector m_vecNormal;               
    Vector m_vecPosition;             
 
    uint8_t pad2[0xAC - 0xA8];          
 
    float m_flFraction;                 
    uint8_t pad3[0xB6 - 0xB0];          
 
    bool m_bAllSolid;                   
    uint8_t pad4[0x110 - 0xB7];         
};

struct RnQueryShapeAttr_t
{
    uint64_t m_nInteractsWith;               
    uint64_t m_nInteractsExclude;            
    uint64_t m_nInteractsAs;                 
    uint32_t m_nEntityIdsToIgnore[2];        
    uint32_t m_nOwnerIdsToIgnore[2];         
    uint16_t m_nHierarchyIds[2];             
    uint16_t m_nIncludedDetailLayers;        
    uint8_t  m_nTargetDetailLayer;           
    uint8_t  m_nObjectSetMask;               
    uint8_t  m_nObjectGroup;                 
    uint8_t  m_nFlags;                       
    uint8_t  pad[6];                         
};

struct TraceFilter_t
{
    void* m_vftable;            
    RnQueryShapeAttr_t m_nShapeAttr;         
    bool               m_bIterateEntities;   
    uint8_t            pad[7];               
};

class CGameTraceManager {
public:
    bool TraceShape(Ray_t* ray, const Vector& start, const Vector& end, TraceFilter_t* filter, GameTrace_t* out);
};
