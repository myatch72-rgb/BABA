#pragma once
#define NOMINMAX
#include <Windows.h>
#include <stdint.h>
#include <limits>
#include <string>
#include "Utils.h"

#define MULTIPLAYER_BACKUP 150

enum ECommandButtons : uint64_t
{
	IN_ATTACK = (1ULL << 0),
	IN_JUMP = (1ULL << 1),
	IN_DUCK = (1ULL << 2),
	IN_FORWARD = (1ULL << 3),
	IN_BACK = (1ULL << 4),
	IN_USE = (1ULL << 5),
	IN_CANCEL = (1ULL << 6),
	IN_LEFT = (1ULL << 7),
	IN_RIGHT = (1ULL << 8),
	IN_MOVELEFT = (1ULL << 9),
	IN_MOVERIGHT = (1ULL << 10),
	IN_SECOND_ATTACK = (1ULL << 11),
	IN_RUN = (1ULL << 12),
	IN_RELOAD = (1ULL << 13),
	IN_LEFT_ALT = (1ULL << 14),
	IN_RIGHT_ALT = (1ULL << 15),
	IN_SCORE = (1ULL << 16),
	IN_SPEED = (1ULL << 17),
	IN_WALK = (1ULL << 18),
	IN_ZOOM = (1ULL << 19),
	IN_FIRST_WEAPON = (1ULL << 20),
	IN_SECOND_WEAPON = (1ULL << 21),
	IN_BULLRUSH = (1ULL << 22),
	IN_FIRST_GRENADE = (1ULL << 23),
	IN_SECOND_GRENADE = (1ULL << 24),
	IN_MIDDLE_ATTACK = (1ULL << 25),
	IN_USE_OR_RELOAD = (1ULL << 26)
};

template <typename T>
struct RepeatedPtrField_t
{
	struct Rep_t
	{
		int nAllocatedSize;
		T* tElements[((std::numeric_limits<int>::max)() - 2 * sizeof(int)) / sizeof(void*)];
	};

	void* pArena;
	int nCurrentSize;
	int nTotalSize;
	Rep_t* pRep;
};

class CBasePB
{
public:
	std::byte pad01[0x8];
	std::uint32_t nHasBits; 
	std::uint64_t nCachedBits; 
};

class CMsgQAngle : public CBasePB
{
public:
	QAngle angValue;
};

class CMsgVector : public CBasePB
{
public:
	void* vecValue; 
};

class CCSGOInterpolationInfoPB : public CBasePB
{
public:
	float flFraction;
};

class CCSGOInputHistoryEntryPB : public CBasePB
{
public:
	CMsgQAngle* pViewAngles; 
	CMsgVector* pShootPosition; 
	CMsgVector* pTargetHeadPositionCheck; 
	CMsgVector* pTargetAbsPositionCheck; 
	CMsgQAngle* pTargetAngPositionCheck; 
	CCSGOInterpolationInfoPB* cl_interp; 
	CCSGOInterpolationInfoPB* sv_interp0; 
	CCSGOInterpolationInfoPB* sv_interp1; 
	CCSGOInterpolationInfoPB* player_interp; 
	int nRenderTickCount; 
	float flRenderTickFraction; 
	int nPlayerTickCount; 
	float flPlayerTickFraction; 
	int nFrameNumber; 
	int nTargetEntIndex; 
};

struct CInButtonStatePB : CBasePB
{
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

struct CSubtickMoveStep : CBasePB
{
public:
	std::uint64_t nButton;
	bool bPressed;
	float flWhen;
	float flAnalogForwardDelta;
	float flAnalogLeftDelta;
};

class CBaseUserCmdPB : public CBasePB
{
public:
	RepeatedPtrField_t<CSubtickMoveStep> subtickMovesField; 
	std::string* strMoveCrc; 
	CInButtonStatePB* pInButtonState; 
	CMsgQAngle* pViewAngles;
	std::int32_t nLegacyCommandNumber;
	std::int32_t nClientTick;
	float flForwardMove;
	float flSideMove;
	float flUpMove;
	std::int32_t nImpulse;
	std::int32_t nWeaponSelect;
	std::int32_t nRandomSeed;
	std::int32_t nMousedX;
	std::int32_t nMousedY;
	std::uint32_t nConsumedServerAngleChanges;
	std::int32_t nCmdFlags;
	std::uint32_t nPawnEntityHandle;
};

class CCSGOUserCmdPB
{
public:
	std::uint32_t nHasBits; 
	std::uint64_t nCachedSize; 
	RepeatedPtrField_t<CCSGOInputHistoryEntryPB> inputHistoryField; 
	CBaseUserCmdPB* pBaseCmd; 
	bool bLeftHandDesired; 
	bool bIsPredictingBodyShotFX;
	bool bIsPredictingHeadShotFX;
	bool bIsPredictingKillRagdolls;
	std::int32_t nAttack3StartHistoryIndex; 
	std::int32_t nAttack1StartHistoryIndex; 
	std::int32_t nAttack2StartHistoryIndex; 
};

struct CInButtonState
{
public:
	std::byte pad01[0x8];
	std::uint64_t nValue; 
	std::uint64_t nValueChanged; 
	std::uint64_t nValueScroll; 
};

class CUserCmd
{
public:
	std::byte pad01[0x8];
	std::byte pad02[0x10];
	CCSGOUserCmdPB csgoUserCmd; 
	CInButtonState nButtons; 
	std::byte pad03[0x20];

	CCSGOInputHistoryEntryPB* GetInputHistoryEntry(int nIndex)
	{
		if (!csgoUserCmd.inputHistoryField.pRep || nIndex >= csgoUserCmd.inputHistoryField.pRep->nAllocatedSize || nIndex >= csgoUserCmd.inputHistoryField.nCurrentSize)
			return nullptr;

		return csgoUserCmd.inputHistoryField.pRep->tElements[nIndex];
	}

	bool IsButtonPressed(uint64_t button) const
	{
		if (!csgoUserCmd.pBaseCmd || !csgoUserCmd.pBaseCmd->pInButtonState)
			return false;

		return csgoUserCmd.pBaseCmd->pInButtonState->nValue & button;
	}
};

class CCSGOInput
{
public:
	char pad_0000[0x228]; 
	bool block_shot; 
	bool in_thirdperson; 
	char pad_022A[0x6]; 
	Vector third_person_angles; 
	char pad_023C[0x14]; 
	uint64_t button_pressed; 
	uint64_t mouse_button_pressed; 
	uint64_t button_un_pressed; 
	uint64_t keyboard_copy; 
	float forward_move; 
	float left_move; 
	float up_move; 
	int mouse_delta_x; 
	int mouse_delta_y; 
	int32_t subtick_count; 
	void* subticks[0xC]; 
	QAngle view_angles; 
	int32_t target_entity_index; 
	char pad_03B8[0x258]; 
};
