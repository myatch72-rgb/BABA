#pragma once
#include "bhop/BhopV2.h"
#include <string>
#include <vector>


namespace Misc {
void Render();
void Run();

void InitHitsound();
void HitManagerUpdate();
void PlayHitsound();
void HitmarkerRender();

void InitDeathSound();
void DeathSoundUpdate();

void SpectatorListUpdate();
void SpectatorListRender();

void BombTimerUpdate();
void BombTimerRender();
void SmokeTimerRender();

void BulletTracerUpdate();
void BulletTracerRender();

void EngineParticleUpdate();
void DeathDustDissolveRender();

void HitParticleUpdate();
void HitParticleRender();

void HitLogUpdate();
void HitLogRender();

void CrosshairIndicatorRender();

void ThirdPerson(void* input, unsigned int slot);
void PrepareThirdPerson();
void ShutdownThirdPerson();
void SyncThirdPersonRequest();

void AntiFlash();
void NoVisualRecoil();

void SmokeColorChanger();
void WorldManipulationUpdate();
void CinematicPostProcessRender();

void ChatSpamUpdate();
void NameChangerUpdate();

void WatermarkRender();
void FeatureStatusRender();

void run_bunnyhop(c_user_cmd *cmd);
void run_longjump();
void run_longjump_render();
void MovementKeysRender();
void VelocityGraphRender();
void run_auto_strafe(c_user_cmd *cmd);
} // namespace Misc
