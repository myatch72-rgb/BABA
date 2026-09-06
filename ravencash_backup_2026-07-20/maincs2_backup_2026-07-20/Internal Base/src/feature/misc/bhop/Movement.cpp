#include "Movement.h"
#include "BhopV2.h"

#include "../../../sdk/entity/Classes.h"
#include "../../../sdk/memory/Globals.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/utils/CCSGOInput.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/utils/Vector.h"

#include <Windows.h>
#include <cmath>

namespace {
constexpr uint8_t MOVETYPE_NOCLIP = 7;
constexpr uint8_t MOVETYPE_LADDER = 9;

CBaseUserCmdPB *GetBaseCmd(c_user_cmd *cmd) {
  if (!cmd)
    return nullptr;

  __try {
    CUserCmd *full = reinterpret_cast<CUserCmd *>(cmd);
    return full->csgoUserCmd.pBaseCmd;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool HasButton(c_user_cmd *cmd, uint64_t button) {
  if (!cmd)
    return false;

  __try {
    if ((cmd->m_button_state & button) != 0)
      return true;

    CBaseUserCmdPB *base = GetBaseCmd(cmd);
    return base && base->pInButtonState &&
           ((base->pInButtonState->nValue & button) != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void SetButton(c_user_cmd *cmd, uint64_t button, bool pressed) {
  if (!cmd)
    return;

  __try {
    if (pressed)
      cmd->m_button_state |= button;
    else
      cmd->m_button_state &= ~button;
    cmd->m_button_state2 |= button;

    CBaseUserCmdPB *base = GetBaseCmd(cmd);
    if (!base || !base->pInButtonState)
      return;

    if (pressed)
      base->pInButtonState->nValue |= button;
    else
      base->pInButtonState->nValue &= ~button;
    base->pInButtonState->nValueChanged |= button;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void RunMouseStrafe(c_user_cmd *cmd, uintptr_t pawnAddr) {
  CBaseUserCmdPB *base = GetBaseCmd(cmd);
  if (!base)
    return;

  const Vector velocity =
      Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecVelocity);
  const float speed = std::sqrt(velocity.x * velocity.x +
                                velocity.y * velocity.y);
  if (!std::isfinite(speed) || speed < 15.0f)
    return;

  const bool left = HasButton(cmd, Movement::IN_MOVELEFT) ||
                    ((GetAsyncKeyState('A') & 0x8000) != 0);
  const bool right = HasButton(cmd, Movement::IN_MOVERIGHT) ||
                     ((GetAsyncKeyState('D') & 0x8000) != 0);

  float direction = 0.0f;
  if (left != right)
    direction = left ? -1.0f : 1.0f;
  else if (base->nMousedX != 0)
    direction = base->nMousedX < 0 ? -1.0f : 1.0f;

  // No explicit steering input: preserve the player's own movement.
  if (direction == 0.0f)
    return;

  __try {
    base->flForwardMove = 0.0f;
    base->flSideMove = direction * 450.0f;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
} // namespace

void Movement::Run(c_user_cmd *cmd) {
  if (!cmd || !Globals::bunnyhop_enabled)
    return;

  const uintptr_t pawnAddr = Memory::Globals::LocalPawn();
  if (!pawnAddr)
    return;

  auto *pawn = reinterpret_cast<C_CSPlayerPawn *>(pawnAddr);
  if (!pawn->IsAlive())
    return;

  const uint8_t moveType =
      Utils::SafeRead<uint8_t>(pawnAddr + Offsets::m_MoveType);
  if (moveType == MOVETYPE_NOCLIP || moveType == MOVETYPE_LADDER)
    return;

  const bool jumpHeld =
      ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) ||
      HasButton(cmd, IN_JUMP);
  if (!jumpHeld)
    return;

  const uint32_t flags =
      Utils::SafeRead<uint32_t>(pawnAddr + Offsets::m_fFlags);
  const bool onGround = (flags & 1U) != 0;

  // Create a clean release while airborne and a fresh press on landing.
  SetButton(cmd, IN_JUMP, onGround);

  if (!onGround)
    RunMouseStrafe(cmd, pawnAddr);
}
