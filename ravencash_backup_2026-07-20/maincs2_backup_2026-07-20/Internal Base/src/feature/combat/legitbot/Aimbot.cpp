#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Aimbot.h"
#include "../../../menu/Menu.h"
#include "../../../sdk/classes/SceneSystem.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/interfaces/Interfaces.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/utils/CCSGOInput.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/utils/Utils.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>


#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

namespace Aimbot {

static float RandFloat(float min, float max) {
  static std::mt19937 rng(
      static_cast<unsigned int>(std::chrono::high_resolution_clock::now()
          .time_since_epoch()
          .count()));
  std::uniform_real_distribution<float> dist(min, max);
  return dist(rng);
}

struct HumanState {
  int targetIndex = -1;
  float offsetPitch = 0.f;
  float offsetYaw = 0.f;
  float offsetDecay = 1.f;
  float accumulatedTime = 0.f;
  LARGE_INTEGER lastTime = {};
};
static HumanState g_human;

static void ResetHumanState(int newTarget) {
  g_human.targetIndex = newTarget;
  g_human.accumulatedTime = 0.f;
  g_human.offsetDecay = 1.f;
  QueryPerformanceCounter(&g_human.lastTime);

  float angle = RandFloat(0.f, 2.f * M_PI);
  float radius = RandFloat(0.3f, 1.0f);
  g_human.offsetPitch = std::sin(angle) * radius;
  g_human.offsetYaw = std::cos(angle) * radius;
}

static float GetDeltaTime() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  float dt =
      (float)(now.QuadPart - g_human.lastTime.QuadPart) / (float)freq.QuadPart;
  g_human.lastTime = now;
  return std::clamp(dt, 0.0001f, 0.1f);
}

static bool IsBoneVisibleRaycast(const Vector &eyePos, const Vector &bonePos) {
  if (!Raycasting::Get().IsLoaded())
    return true;

  return Raycasting::Get().IsVisible(eyePos, bonePos);
}

static Vector CalculateAngle(const Vector &source, const Vector &destination) {
  Vector angle;
  Vector delta = destination - source;
  float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);
  angle.x = (float)(std::atan2(-delta.z, hyp) * (180.0f / M_PI));
  angle.y = (float)(std::atan2(delta.y, delta.x) * (180.0f / M_PI));
  angle.z = 0.0f;
  return angle;
}

static float GetFov(const Vector &viewAngle, const Vector &aimAngle) {
  Vector delta = aimAngle - viewAngle;
  while (delta.y > 180)
    delta.y -= 360;
  while (delta.y < -180)
    delta.y += 360;
  return (float)std::sqrt(std::pow(delta.x, 2) + std::pow(delta.y, 2));
}

static void NormalizeDelta(Vector &delta) {
  while (delta.y > 180.f)
    delta.y -= 360.f;
  while (delta.y < -180.f)
    delta.y += 360.f;
  delta.x = std::clamp(delta.x, -89.f, 89.f);
}

static Vector ApplySmoothing(const Vector &currentAngles,
                             const Vector &targetAngle, float dt) {
  Vector delta = targetAngle - currentAngles;
  NormalizeDelta(delta);

  float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);

  // Snap zone: if very close to target, lock on immediately
  if (distance < 0.05f)
    return targetAngle;

  // Power curve: low smoothing values become exponentially more aggressive
  float rawSpeed = 1.01f - Globals::aim_smoothing;
  float speed = std::pow(rawSpeed, 3.f); // cubic curve for aggressive low-end
  speed = std::clamp(speed, 0.f, 1.f);

  float normalizedDist = std::clamp(distance / Globals::aim_fov, 0.f, 1.f);

  float curveFactor = 1.0f;
  if (Globals::aim_humanize) {
    float curve = Globals::aim_humanize_curve;
    float easeOut = 1.f - std::pow(1.f - normalizedDist, 2.f);
    float easeIn = std::pow(normalizedDist, 2.f);
    curveFactor = easeOut * (1.f - curve) + easeIn * curve;
    curveFactor = std::clamp(curveFactor, 0.4f, 1.5f);
  }

  // Much higher base multiplier for faster convergence
  float multiplier = 12.f + speed * 88.f;
  multiplier *= curveFactor;

  // Close-range boost: when near target, accelerate convergence
  if (normalizedDist < 0.3f) {
    float closeBoost = 1.f + (1.f - normalizedDist / 0.3f) * 1.5f;
    multiplier *= closeBoost;
  }

  float factor = 1.f - std::exp(-multiplier * dt);
  factor = std::clamp(factor, 0.01f, 1.0f);

  Vector result;
  result.x = currentAngles.x + delta.x * factor;
  result.y = currentAngles.y + delta.y * factor;
  result.z = 0.f;

  return result;
}

static Vector ApplyHumanization(const Vector &targetAngle, float dt) {
  if (!Globals::aim_humanize)
    return targetAngle;

  Vector result = targetAngle;

  // Scale humanize effect down when smoothing is low (aggressive aim)
  float smoothScale = std::clamp(Globals::aim_smoothing * 2.f, 0.05f, 1.f);

  g_human.accumulatedTime += dt;
  // Faster decay so offset disappears quicker
  float decaySpeed = 2.5f + Globals::aim_smoothing * 3.f;
  g_human.offsetDecay = std::exp(-g_human.accumulatedTime * decaySpeed);
  g_human.offsetDecay = std::clamp(g_human.offsetDecay, 0.f, 1.f);

  float strength = Globals::aim_humanize_strength * smoothScale;
  result.x += g_human.offsetPitch * strength * g_human.offsetDecay;
  result.y += g_human.offsetYaw * strength * g_human.offsetDecay;

  float jitter = Globals::aim_humanize_jitter * smoothScale;
  if (jitter > 0.01f) {
    float jitterScale = g_human.offsetDecay * 0.5f + 0.1f;
    result.x += RandFloat(-jitter, jitter) * jitterScale * 0.1f;
    result.y += RandFloat(-jitter, jitter) * jitterScale * 0.1f;
  }

  return result;
}

static void RunInternal() {
  static int lastTargetIndex = -1;

  // Mutual-exclusion with Silent Aim — if the user has Silent Aim ON the
  // camera-side aimbot MUST stay quiet, otherwise its dwViewAngles write
  // visibly snaps the crosshair and defeats Silent Aim's whole purpose.
  // The menu's checkboxes already enforce this on toggle, but this
  // runtime gate covers the case where a stale config / hot-reload
  // ended up with both flags set.
  if (Globals::silent_aim_enabled) {
    lastTargetIndex = -1;
    g_human.targetIndex = -1;
    return;
  }

  const bool keyActive = (Globals::aim_key == 0) ||
                         ((GetAsyncKeyState(Globals::aim_key) & 0x8000) != 0);
  if (!Globals::aim_enabled || !keyActive) {
    lastTargetIndex = -1;
    g_human.targetIndex = -1;
    return;
  }

  if (Menu::IsOpen)
    return;

  uintptr_t client = Memory::GetModuleBase("client.dll");
  if (!client)
    return;

  auto localPawn = EntityManager::Get().GetLocalPawn();
  if (!localPawn || !Utils::SafeAlive(localPawn)) {
    lastTargetIndex = -1;
    g_human.targetIndex = -1;
    return;
  }

  uintptr_t scene = Utils::SafeRead<uintptr_t>(
      reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_pGameSceneNode);
  if (!Utils::IsValidPtr(scene))
    return;

  Vector localOrigin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
  Vector viewOffset = Utils::SafeRead<Vector>(
      reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_vecViewOffset);
  Vector localPos = localOrigin + viewOffset;

  uintptr_t viewAnglesAddr = client + Offsets::dwViewAngles;
  if (!Utils::IsValidPtr(viewAnglesAddr))
    return;

  Vector currentAngles = Utils::SafeRead<Vector>(viewAnglesAddr);

  const Entity_t *bestTarget = nullptr;
  Vector bestTargetPos;

  auto entities = EntityManager::Get().GetEntities();

  float currentFov =
      Globals::visuals_fov_enabled ? Globals::visuals_fov : 90.0f;
  float fovScale = currentFov / 90.0f;
  float scaledAimFov = Globals::aim_fov * fovScale;

  std::vector<BoneID> activeBones;
  if (Globals::aim_hitbox_head)
    activeBones.push_back(BoneID::Head);
  if (Globals::aim_hitbox_neck)
    activeBones.push_back(BoneID::Neck);
  if (Globals::aim_hitbox_chest)
    activeBones.push_back(BoneID::Spine);
  if (Globals::aim_hitbox_stomach)
    activeBones.push_back(BoneID::Spine);
  if (Globals::aim_hitbox_pelvis)
    activeBones.push_back(BoneID::Pelvis);

  if (activeBones.empty())
    return;

  if (Globals::aim_persistent && lastTargetIndex != -1) {
    for (const auto &entity : entities) {
      if (entity.index == lastTargetIndex && entity.pawn &&
          Utils::SafeAlive(entity.pawn) &&
          (entity.isEnemy || Globals::target_teammates)) {
        if (entity.pawn == localPawn)
          continue;
        float bestFovForEntity = scaledAimFov * 2.0f;
        bool foundBone = false;

        for (BoneID bone : activeBones) {
          Vector bonePos = Utils::GetBonePos(entity.pawn, bone);
          if (!bonePos.IsZero() && IsBoneVisibleRaycast(localPos, bonePos)) {
            Vector angle = CalculateAngle(localPos, bonePos);
            float fov = GetFov(currentAngles, angle);

            if (fov < bestFovForEntity) {
              bestFovForEntity = fov;
              bestTarget = &entity;
              bestTargetPos = bonePos;
              foundBone = true;
            }
          }
        }
        if (foundBone)
          break;
      }
    }
  }

  if (!bestTarget) {
    float bestFov = scaledAimFov;
    for (const auto &entity : entities) {
      if (!entity.pawn || !(entity.isEnemy || Globals::target_teammates) ||
          !Utils::SafeAlive(entity.pawn))
        continue;
      if (entity.pawn == localPawn)
        continue;

      for (BoneID bone : activeBones) {
        Vector bonePos = Utils::GetBonePos(entity.pawn, bone);
        if (bonePos.IsZero())
          continue;

        if (!IsBoneVisibleRaycast(localPos, bonePos))
          continue;

        Vector angle = CalculateAngle(localPos, bonePos);
        float fov = GetFov(currentAngles, angle);

        if (fov < bestFov) {
          bestFov = fov;
          bestTarget = &entity;
          bestTargetPos = bonePos;
        }
      }
    }
  }

  if (bestTarget) {
    lastTargetIndex = bestTarget->index;

    if (g_human.targetIndex != bestTarget->index) {
      ResetHumanState(bestTarget->index);
    }

    float dt = GetDeltaTime();

    Vector rawAngle = CalculateAngle(localPos, bestTargetPos);

    Vector humanizedAngle = ApplyHumanization(rawAngle, dt);

    Vector finalAngle = ApplySmoothing(currentAngles, humanizedAngle, dt);

    finalAngle.x = std::clamp(finalAngle.x, -89.f, 89.f);
    finalAngle.z = 0.f;

    Utils::SafeWrite<Vector>(viewAnglesAddr, finalAngle);
  } else {
    lastTargetIndex = -1;
    g_human.targetIndex = -1;
  }
}

void Run() {
  __try {
    RunInternal();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

} // namespace Aimbot
