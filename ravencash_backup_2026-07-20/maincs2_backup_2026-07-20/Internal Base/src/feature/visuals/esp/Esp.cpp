#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Esp.h"
#include "../../../../ext/imgui/imgui.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/memory/Offsets.h"
#include "../../../sdk/memory/PatternScan.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../sdk/utils/Raycasting.h"
#include "../../../sdk/utils/SteamAvatarManager.h"
#include "../../../sdk/utils/Utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <vector>

static ImVec2 CatmullRom(const ImVec2 &p0, const ImVec2 &p1, const ImVec2 &p2,
                         const ImVec2 &p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return ImVec2(0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                        (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                        (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
                0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                        (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                        (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3));
}

static const std::vector<std::vector<BoneID>> g_BoneChains = {

    {BoneID::Head, BoneID::Neck, BoneID::Spine, BoneID::Pelvis},

    {BoneID::Spine, BoneID::LeftShoulder, BoneID::LeftArm, BoneID::LeftHand},

    {BoneID::Spine, BoneID::RightShoulder, BoneID::RightArm, BoneID::RightHand},

    {BoneID::Pelvis, BoneID::LeftHip, BoneID::LeftKnee, BoneID::LeftFoot},

    {BoneID::Pelvis, BoneID::RightHip, BoneID::RightKnee, BoneID::RightFoot},
};

extern ImFont *esp_font;
extern ImFont *esp_flags_font;
extern ImFont *weapon_icon_font;

#include "../../skinchanger/SkinData.h"   // for WEP_* defindex enum

// ============================================================================
//  ESP Flags
//  Small uppercase pill chips stacked vertically to the RIGHT of each
//  enemy box: SCOPED, FLASH, AIR, DUCK, KIT, DEFUSE, PLANT, C4, LOW HP,
//  HE/SMOKE/FLASH/MOL/DECOY. Toggled individually from the menu.
// ============================================================================
namespace EspFlags {
    struct Tag {
        const char* label;
        const float* color4;   // pointer into Globals
    };

    // Read [defIndex...] of every weapon this pawn carries. Returns the
    // count of weapons examined so the caller knows how much of the array
    // was filled. Caps at maxOut so we never overflow the buffer even on a
    // malicious or corrupted entity list.
    inline int CollectWeaponDefs(uintptr_t pawn, uint16_t* outDefs, int maxOut)
    {
        int count = 0;
        if (!pawn || !outDefs || maxOut <= 0) return 0;

        uintptr_t ws = Utils::SafeRead<uintptr_t>(pawn + Offsets::m_pWeaponServices);
        if (!Utils::IsValidPtr(ws)) return 0;

        // Live C_NetworkUtlVectorBase layout stores the size at +0x0 and the
        // aligned element pointer at +0x8.
        uintptr_t base = ws + Offsets::sc_m_hMyWeapons;
        int size = Utils::SafeRead<int>(base + 0x0);
        uintptr_t elements = Utils::SafeRead<uintptr_t>(base + 0x8);
        if (!Utils::IsValidPtr(elements) || size <= 0 || size > 64) return 0;

        for (int i = 0; i < size && count < maxOut; ++i) {
            uint32_t handle = Utils::SafeRead<uint32_t>(elements + i * 4);
            if (handle == 0 || handle == 0xFFFFFFFFu) continue;
            uint32_t serial = (handle >> 15) & 0x1FFFF;
            (void)serial;
            // Index out of plausible range → end of populated handles.
            uint32_t idx = handle & 0x7FFF;
            if (idx == 0 || idx > 16384) continue;

            auto* ent = EntityManager::Get().GetEntityFromHandle(handle);
            if (!ent) continue;
            uintptr_t weapon = reinterpret_cast<uintptr_t>(ent);
            if (!Utils::IsValidPtr(weapon)) continue;

            uintptr_t item = weapon + Offsets::sc_m_AttributeManager
                                    + Offsets::sc_m_Item;
            uint16_t def = Utils::SafeRead<uint16_t>(
                item + Offsets::sc_m_iItemDefinitionIndex);
            if (def == 0) continue;
            outDefs[count++] = def;
        }
        return count;
    }

    inline bool HasDef(const uint16_t* defs, int count, uint16_t needle) {
        for (int i = 0; i < count; ++i) if (defs[i] == needle) return true;
        return false;
    }

} // namespace EspFlags

static Vector GetLocalEyePosition() {
  auto localPawn = EntityManager::Get().GetLocalPawn();
  if (!localPawn)
    return {};

  uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(localPawn);
  uintptr_t scene =
      Utils::SafeRead<uintptr_t>(pawnAddr + Offsets::m_pGameSceneNode);
  if (!Utils::IsValidPtr(scene))
    return {};

  Vector origin = Utils::SafeRead<Vector>(scene + Offsets::m_vecAbsOrigin);
  Vector viewOffset =
      Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vecViewOffset);
  return origin + viewOffset;
}

static bool IsBoneVisibleRaycast(const Vector &bonePos) {
  if (!Raycasting::Get().IsLoaded())
    return true;

  Vector eyePos = GetLocalEyePosition();
  if (eyePos.IsZero())
    return true;

  return Raycasting::Get().IsVisible(eyePos, bonePos);
}

static bool IsAnyBoneVisible(C_CSPlayerPawn *pawn) {
  if (!Raycasting::Get().IsLoaded())
    return true;

  Vector eyePos = GetLocalEyePosition();
  if (eyePos.IsZero())
    return true;

  static const BoneID majorBones[] = {
      BoneID::Head,      BoneID::Neck,          BoneID::Spine,
      BoneID::Pelvis,    BoneID::LeftShoulder,  BoneID::LeftArm,
      BoneID::LeftHand,  BoneID::RightShoulder, BoneID::RightArm,
      BoneID::RightHand, BoneID::LeftHip,       BoneID::LeftKnee,
      BoneID::LeftFoot,  BoneID::RightHip,      BoneID::RightKnee,
      BoneID::RightFoot};

  for (auto bone : majorBones) {
    Vector bonePos = Utils::GetBonePos(pawn, bone);
    if (!bonePos.IsZero() && Raycasting::Get().IsVisible(eyePos, bonePos))
      return true;
  }

  return false;
}

const char *GetIconFromWeaponId(uint16_t id) {
  switch (id) {
  case 1:
    return "A";
  case 2:
    return "B";
  case 3:
    return "C";
  case 4:
    return "D";
  case 7:
    return "W";
  case 8:
    return "U";
  case 9:
    return "Z";
  case 10:
    return "R";
  case 11:
    return "X";
  case 13:
    return "Q";
  case 14:
    return "g";
  case 16:
    return "S";
  case 17:
    return "K";
  case 19:
    return "O";
  case 23:
    return "M";
  case 24:
    return "L";
  case 25:
    return "b";
  case 26:
    return "M";
  case 27:
    return "d";
  case 28:
    return "f";
  case 29:
    return "c";
  case 30:
    return "H";
  case 31:
    return "h";
  case 32:
    return "E";
  case 33:
    return "N";
  case 34:
    return "P";
  case 35:
    return "e";
  case 36:
    return "F";
  case 38:
    return "Y";
  case 39:
    return "V";
  case 40:
    return "a";
  case 42:
    return "]";
  case 43:
    return "i";
  case 44:
    return "j";
  case 45:
    return "k";
  case 46:
    return "l";
  case 47:
    return "m";
  case 48:
    return "n";
  case 49:
    return "o";
  case 59:
    return "[";
  case 60:
    return "T";
  case 61:
    return "G";
  case 63:
    return "I";
  case 64:
    return "J";
  default:
    if (id >= 500 && id <= 526)
      return "]";
    return "";
  }
}

struct SoundRingInfo {
  Vector origin;
  double startTime;
};

static std::unordered_map<uintptr_t, std::vector<SoundRingInfo>>
    g_SoundRingCache;
static std::unordered_map<uintptr_t, float> g_LastSoundVal;

static double GetTimeSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void DrawSoundESP(ImDrawList *dl, const Entity_t &ent,
                         C_CSPlayerPawn *localPawn, float sw, float sh,
                         bool isEnemy) {
  const bool soundEnabled =
      isEnemy ? Globals::esp_sound_enabled : Globals::esp_sound_enabled_team;
  if (!soundEnabled)
    return;

  C_CSPlayerPawn *pawn = ent.pawn;
  uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(pawn);

  if (g_SoundRingCache.find(pawnAddr) == g_SoundRingCache.end()) {
    g_SoundRingCache[pawnAddr] = std::vector<SoundRingInfo>();
    g_LastSoundVal[pawnAddr] = 0.0f;
  }

  float currentSound =
      Utils::SafeRead<float>(pawnAddr + Offsets::m_flEmitSoundTime, 0.0f);
  float lastSound = g_LastSoundVal[pawnAddr];

  if (currentSound != lastSound && currentSound != 0.0f) {
    g_LastSoundVal[pawnAddr] = currentSound;

    Vector origin = Utils::SafeRead<Vector>(pawnAddr + Offsets::m_vOldOrigin);

    SoundRingInfo ring;
    ring.origin = origin;
    ring.startTime = GetTimeSeconds();
    g_SoundRingCache[pawnAddr].push_back(ring);
  }

  auto &rings = g_SoundRingCache[pawnAddr];
  double now = GetTimeSeconds();

  for (int r = (int)rings.size() - 1; r >= 0; r--) {
    SoundRingInfo &ring = rings[r];
    double elapsed = now - ring.startTime;

    if (elapsed >= 1.0) {
      rings.erase(rings.begin() + r);
      continue;
    }

    float radius = 30.0f * (1.0f - (float)elapsed);

    float alpha = 1.0f - (float)elapsed;

    ImU32 ringColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(isEnemy ? Globals::esp_sound_color[0]
                       : Globals::esp_sound_color_team[0],
               isEnemy ? Globals::esp_sound_color[1]
                       : Globals::esp_sound_color_team[1],
               isEnemy ? Globals::esp_sound_color[2]
                       : Globals::esp_sound_color_team[2],
               (isEnemy ? Globals::esp_sound_color[3]
                        : Globals::esp_sound_color_team[3]) *
                   alpha));

    const int segments = 72;
    std::vector<ImVec2> points;
    points.reserve(segments + 1);
    float step = (float)(M_PI * 2.0) / segments;

    for (int i = 0; i <= segments; i++) {
      float angle = step * i;
      float xOffset = radius * cosf(angle);
      float yOffset = radius * sinf(angle);

      Vector point3D(ring.origin.x + xOffset, ring.origin.y + yOffset,
                     ring.origin.z);

      Vector screenPos;
      if (Utils::WorldToScreen(point3D, screenPos, (float *)Globals::ViewMatrix,
                               sw, sh)) {
        points.push_back(ImVec2(screenPos.x, screenPos.y));
      } else {

        if (points.size() > 1) {
          dl->AddPolyline(points.data(), (int)points.size(), ringColor,
                          ImDrawFlags_None, 2.0f);
        }
        points.clear();
      }
    }

    if (points.size() > 1) {
      dl->AddPolyline(points.data(), (int)points.size(), ringColor,
                      ImDrawFlags_None, 2.0f);
    }
  }
}

static void RenderInternal() {
  if (!Globals::esp_enabled && !Globals::esp_enabled_team)
    return;

  ImDrawList *dl = ImGui::GetBackgroundDrawList();
  dl->Flags |=
      ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
  const float sw = Globals::GameViewportWidth;
  const float sh = Globals::GameViewportHeight;

  if (sw <= 0 || sh <= 0)
    return;

  auto entities = EntityManager::Get().GetEntities();
  C_CSPlayerPawn *localPawn = EntityManager::Get().GetLocalPawn();
  if (!localPawn)
    return;
  C_CSPlayerPawn *observedPawn = EntityManager::Get().GetLocalObservedPawn();
  const int localTeam = Utils::SafeTeam(localPawn);
  const Vector localOrigin = Utils::SafeRead<Vector>(
      reinterpret_cast<uintptr_t>(localPawn) + Offsets::m_vOldOrigin);

  for (const auto &ent : entities) {
    C_CSPlayerPawn *pawn = ent.pawn;
    if (!Utils::IsValidPtr(reinterpret_cast<uintptr_t>(pawn)))
      continue;
    if (!Utils::SafeAlive(pawn))
      continue;
    if (pawn == localPawn)
      continue;
    if (observedPawn && pawn == observedPawn)
      continue;

    int team = Utils::SafeTeam(pawn);
    bool isEnemy = (team != localTeam);
    
    if (Globals::esp_radar && isEnemy) {
        pawn->SetSpotted(true);
    }

    const bool espEnabled =
        isEnemy ? Globals::esp_enabled : Globals::esp_enabled_team;
    if (!espEnabled)
      continue;
    const bool espBoxEnabled =
        isEnemy ? Globals::esp_box : Globals::esp_box_team;
    const bool espSkeletonEnabled =
        isEnemy ? Globals::esp_skeleton : Globals::esp_skeleton_team;
    const bool espHeadEnabled =
        isEnemy ? Globals::esp_head : Globals::esp_head_team;
    const bool espNameEnabled =
        isEnemy ? Globals::esp_name : Globals::esp_name_team;
    const bool espDistanceEnabled =
        isEnemy ? Globals::esp_distance : Globals::esp_distance_team;
    const bool espWeaponEnabled =
        isEnemy ? Globals::esp_weapon : Globals::esp_weapon_team;
    const bool espHealthEnabled =
        isEnemy ? Globals::esp_health : Globals::esp_health_team;
    const bool espSoundEnabled =
        isEnemy ? Globals::esp_sound_enabled : Globals::esp_sound_enabled_team;
    if (!espBoxEnabled && !espSkeletonEnabled && !espHeadEnabled &&
        !espNameEnabled && !espDistanceEnabled && !espWeaponEnabled &&
        !espHealthEnabled && !espSoundEnabled)
      continue;
    const int espBoxType =
        isEnemy ? Globals::esp_box_type : Globals::esp_box_type_team;
    const int espSkeletonType =
        isEnemy ? Globals::esp_skeleton_type : Globals::esp_skeleton_type_team;

    const float *boxColorHidden =
        isEnemy ? Globals::esp_box_color : Globals::esp_box_color_team;
    const float *boxColorVisible =
        isEnemy ? Globals::esp_box_color_vis : Globals::esp_box_color_vis_team;
    const float *skeletonColorHidden = isEnemy
                                           ? Globals::esp_skeleton_color
                                           : Globals::esp_skeleton_color_team;
    const float *skeletonColorVisible =
        isEnemy ? Globals::esp_skeleton_color_vis
                : Globals::esp_skeleton_color_vis_team;
    const float *nameColorHidden =
        isEnemy ? Globals::esp_name_color : Globals::esp_name_color_team;
    const float *nameColorVisible = isEnemy ? Globals::esp_name_color_vis
                                            : Globals::esp_name_color_vis_team;
    const float *healthColorHidden =
        isEnemy ? Globals::esp_health_color : Globals::esp_health_color_team;
    const float *healthColorVisible = isEnemy
                                          ? Globals::esp_health_color_vis
                                          : Globals::esp_health_color_vis_team;
    const float *distanceColorHidden = isEnemy
                                           ? Globals::esp_distance_color
                                           : Globals::esp_distance_color_team;
    const float *distanceColorVisible =
        isEnemy ? Globals::esp_distance_color_vis
                : Globals::esp_distance_color_vis_team;
    const float *weaponColorHidden =
        isEnemy ? Globals::esp_weapon_color : Globals::esp_weapon_color_team;
    const float *weaponColorVisible = isEnemy
                                          ? Globals::esp_weapon_color_vis
                                          : Globals::esp_weapon_color_vis_team;

    Vector origin = Utils::SafeRead<Vector>(reinterpret_cast<uintptr_t>(pawn) +
                                            Offsets::m_vOldOrigin);
    if (origin.IsZero())
      continue;
    Vector headPos = Utils::GetBonePos(pawn, BoneID::Head);

    if (headPos.IsZero()) {
      headPos = origin;
      headPos.z += 72.0f;
    } else {
      headPos.z += 7.0f;
    }

    Vector sHead, sFoot;
    const bool onScreen =
        Utils::WorldToScreen(headPos, sHead, (float *)Globals::ViewMatrix, sw,
                             sh) &&
        Utils::WorldToScreen(origin, sFoot, (float *)Globals::ViewMatrix, sw,
                             sh);
    if (!onScreen)
      continue;

    DrawSoundESP(dl, ent, localPawn, sw, sh, isEnemy);

    const bool needAnyVisibleColor = espBoxEnabled || espNameEnabled ||
                                     espDistanceEnabled || espWeaponEnabled ||
                                     espHealthEnabled;
    const bool isVis = needAnyVisibleColor ? IsAnyBoneVisible(pawn) : false;
    const ImU32 boxCol = ImGui::ColorConvertFloat4ToU32(
        ImVec4(isVis ? boxColorVisible[0] : boxColorHidden[0],
               isVis ? boxColorVisible[1] : boxColorHidden[1],
               isVis ? boxColorVisible[2] : boxColorHidden[2],
               isVis ? boxColorVisible[3] : boxColorHidden[3]));
    const ImU32 nameCol = ImGui::ColorConvertFloat4ToU32(
        ImVec4(isVis ? nameColorVisible[0] : nameColorHidden[0],
               isVis ? nameColorVisible[1] : nameColorHidden[1],
               isVis ? nameColorVisible[2] : nameColorHidden[2],
               isVis ? nameColorVisible[3] : nameColorHidden[3]));

    float height = sFoot.y - sHead.y;
    float width = height / 1.8f;
    float x = sHead.x - width / 2.0f;
    float y = sHead.y;

      if (espBoxEnabled) {
        if (espBoxType == 0) {
          dl->AddRect({x - 1, y - 1}, {x + width + 1, y + height + 1},
                      IM_COL32(0, 0, 0, 150));
          dl->AddRect({x, y}, {x + width, y + height}, boxCol, 0, 0,
                      Globals::esp_box_thickness);
          dl->AddRect({x + 1, y + 1}, {x + width - 1, y + height - 1},
                      IM_COL32(0, 0, 0, 150));
        } else {
          float lineW = width / 4.0f;
          float lineH = height / 4.0f;

          auto DrawLineBase = [&](float px1, float py1, float px2, float py2) {
            dl->AddLine({px1, py1}, {px2, py2}, IM_COL32(0, 0, 0, 255),
                        Globals::esp_box_thickness + 2.0f);
            dl->AddLine({px1, py1}, {px2, py2}, boxCol,
                        Globals::esp_box_thickness);
          };

          DrawLineBase(x, y, x + lineW, y);
          DrawLineBase(x, y, x, y + lineH);

          DrawLineBase(x + width - lineW, y, x + width, y);
          DrawLineBase(x + width, y, x + width, y + lineH);

          DrawLineBase(x, y + height - lineH, x, y + height);
          DrawLineBase(x, y + height, x + lineW, y + height);

          DrawLineBase(x + width - lineW, y + height, x + width, y + height);
          DrawLineBase(x + width, y + height - lineH, x + width, y + height);
        }
      }

      if (espHealthEnabled) {
        int health = Utils::SafeRead<int>(
            reinterpret_cast<uintptr_t>(pawn) + Offsets::m_iHealth, 100);
        float hpFrac = std::clamp(health / 100.f, 0.f, 1.f);
        const ImU32 hpCol = ImGui::ColorConvertFloat4ToU32(
            ImVec4(isVis ? healthColorVisible[0] : healthColorHidden[0],
                   isVis ? healthColorVisible[1] : healthColorHidden[1],
                   isVis ? healthColorVisible[2] : healthColorHidden[2],
                   isVis ? healthColorVisible[3] : healthColorHidden[3]));
        dl->AddRectFilled({x - 6, y - 1}, {x - 2, y + height + 1},
                          IM_COL32(0, 0, 0, 150));
        dl->AddRectFilled({x - 5, y + height - (height * hpFrac)},
                          {x - 3, y + height}, hpCol);
      }

      // --- Steam Avatar ---
      const bool avatarEnabled = isEnemy ? Globals::esp_avatar_enabled : Globals::esp_avatar_team;
      if (avatarEnabled && ent.controller) {
        uint64_t steamID = Utils::SafeRead<uint64_t>(
            reinterpret_cast<uintptr_t>(ent.controller) + Offsets::m_steamID);
        if (steamID > 76561197960265728ULL) { // valid SteamID64 check
          ID3D11ShaderResourceView *avatarSRV =
              SteamAvatarCache::Get().GetAvatar(steamID);
          if (avatarSRV) {
            const float avatarSize = 22.0f;
            float ax = x + (width - avatarSize) * 0.5f;
            float ay = y - avatarSize - 4.0f;
            // Background/Shadow (Black)
            dl->AddRectFilled({ ax - 1, ay - 1 }, { ax + avatarSize + 1, ay + avatarSize + 1 }, IM_COL32(0, 0, 0, 200));
            
            // Avatar image (Sharp corners)
            dl->AddImage((ImTextureID)avatarSRV, { ax, ay }, { ax + avatarSize, ay + avatarSize }, { 0, 0 }, { 1, 1 }, IM_COL32(255, 255, 255, 255));
            // Black border (Sharp corners)
            dl->AddRect({ ax - 1, ay - 1 }, { ax + avatarSize + 1, ay + avatarSize + 1 }, IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
          }
        }
      }

      if (espNameEnabled && ent.controller) {
        uintptr_t nameAddr = reinterpret_cast<uintptr_t>(ent.controller) + Offsets::m_iszPlayerName;
        char buf[128];
        if (Utils::SafeReadString(nameAddr, buf, sizeof(buf)) && buf[0] != '\0') {
            ImVec2 ts;
            if (esp_font) {
              ts = esp_font->CalcTextSizeA(esp_font->FontSize, FLT_MAX, 0.0f,
                                           buf);
            } else {
              ts = ImGui::CalcTextSize(buf);
            }

            ImVec2 textPos = {x + (width - ts.x) * 0.5f, y - ts.y - 2.f};

            if (esp_font) {
              dl->AddText(esp_font, esp_font->FontSize,
                          {textPos.x + 1.0f, textPos.y + 1.0f},
                          IM_COL32(0, 0, 0, 255), buf);
              dl->AddText(esp_font, esp_font->FontSize, textPos, nameCol, buf);
            } else {
              dl->AddText({textPos.x + 1.0f, textPos.y + 1.0f},
                          IM_COL32(0, 0, 0, 255), buf);
              dl->AddText(textPos, nameCol, buf);
            }
          }
        }

      // ========================================================
      //  Per-player CHIPS  —  only C4 carrier + Money. Drawn as
      //  small pills stacked vertically to the right of the box.
      // ========================================================
      if (Globals::esp_flags_enabled && isEnemy && ent.pawn) {
        uintptr_t pawnAddr = reinterpret_cast<uintptr_t>(ent.pawn);

        // -- C4 carrier check (cheap inventory scan) --
        bool hasC4 = false;
        if (Globals::esp_flag_carrier) {
          uint16_t weapons[16] = {};
          int wcount = EspFlags::CollectWeaponDefs(pawnAddr, weapons, 16);
          hasC4 = EspFlags::HasDef(weapons, wcount, WEP_C4);
        }

        // -- Money read from the controller's money services --
        int money = -1;
        if (Globals::esp_flag_money && ent.controller) {
          uintptr_t ctrlAddr = reinterpret_cast<uintptr_t>(ent.controller);
          uintptr_t moneyServ =
              Utils::SafeRead<uintptr_t>(ctrlAddr + Offsets::m_pInGameMoneyServices);
          if (Utils::IsValidPtr(moneyServ)) {
            int v = Utils::SafeRead<int>(moneyServ + Offsets::m_iAccount, -1);
            if (v >= 0 && v <= 200000) money = v;   // sanity-clamp range
          }
        }

        // Build chip list — order = priority.
        struct Chip {
            char         label[24];
            const float* col;
        };
        Chip chips[2];
        int  ccount = 0;

        if (hasC4) {
          snprintf(chips[ccount].label, sizeof(chips[0].label), "C4");
          chips[ccount].col = Globals::esp_flag_color_c4;
          ccount++;
        }
        if (money >= 0) {
          snprintf(chips[ccount].label, sizeof(chips[0].label),
                   "$%d", money);
          chips[ccount].col = Globals::esp_flag_color_money;
          ccount++;
        }

        if (ccount > 0) {
            ImFont* chipFont = esp_flags_font ? esp_flags_font : esp_font;
            const float fontSize = chipFont ? chipFont->FontSize : 10.5f;
            const float padX = 4.f, padY = 1.f, gap = 2.f;
            float chipX = x + width + 4.f;
            float chipY = y;
            for (int i = 0; i < ccount; ++i) {
                ImVec2 sz = chipFont
                    ? chipFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f, chips[i].label)
                    : ImGui::CalcTextSize(chips[i].label);
                float w = sz.x + padX * 2.f;
                float h = sz.y + padY * 2.f;

                const float* c = chips[i].col;
                ImU32 bg = IM_COL32((int)(c[0] * 255), (int)(c[1] * 255),
                                    (int)(c[2] * 255), (int)(c[3] * 60));
                ImU32 br = IM_COL32((int)(c[0] * 255), (int)(c[1] * 255),
                                    (int)(c[2] * 255), (int)(c[3] * 230));
                ImU32 tx = IM_COL32(245, 245, 245, 240);

                dl->AddRectFilled({chipX, chipY}, {chipX + w, chipY + h}, bg, 2.5f);
                dl->AddRect      ({chipX, chipY}, {chipX + w, chipY + h}, br, 2.5f, 0, 1.0f);
                if (chipFont)
                    dl->AddText(chipFont, fontSize,
                                {chipX + padX, chipY + padY}, tx, chips[i].label);
                else
                    dl->AddText({chipX + padX, chipY + padY}, tx, chips[i].label);

                chipY += h + gap;
            }
        }
      }

      if (espDistanceEnabled) {
        if (!localOrigin.IsZero()) {
          float distUnits = (origin - localOrigin).Length();
          float distMeters = distUnits * 0.0254f;

          char distBuf[32];
          snprintf(distBuf, sizeof(distBuf), "[ %.0fm ]", distMeters);

          ImVec2 ts;
          if (esp_font) {
            ts = esp_font->CalcTextSizeA(esp_font->FontSize, FLT_MAX, 0.0f,
                                         distBuf);
          } else {
            ts = ImGui::CalcTextSize(distBuf);
          }

          ImVec2 textPos = {x + width + 4.0f, y};
          const ImU32 distCol = ImGui::ColorConvertFloat4ToU32(
              ImVec4(isVis ? distanceColorVisible[0] : distanceColorHidden[0],
                     isVis ? distanceColorVisible[1] : distanceColorHidden[1],
                     isVis ? distanceColorVisible[2] : distanceColorHidden[2],
                     isVis ? distanceColorVisible[3] : distanceColorHidden[3]));

          if (esp_font) {
            dl->AddText(esp_font, esp_font->FontSize,
                        {textPos.x + 1.0f, textPos.y + 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText(esp_font, esp_font->FontSize,
                        {textPos.x - 1.0f, textPos.y - 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText(esp_font, esp_font->FontSize,
                        {textPos.x + 1.0f, textPos.y - 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText(esp_font, esp_font->FontSize,
                        {textPos.x - 1.0f, textPos.y + 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText(esp_font, esp_font->FontSize, textPos, distCol,
                        distBuf);
          } else {
            dl->AddText({textPos.x + 1.0f, textPos.y + 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText({textPos.x - 1.0f, textPos.y - 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText({textPos.x + 1.0f, textPos.y - 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText({textPos.x - 1.0f, textPos.y + 1.0f},
                        IM_COL32(0, 0, 0, 255), distBuf);
            dl->AddText(textPos, distCol, distBuf);
          }
        }
      }

      if (espWeaponEnabled) {
        const ImU32 weapCol = ImGui::ColorConvertFloat4ToU32(
            ImVec4(isVis ? weaponColorVisible[0] : weaponColorHidden[0],
                   isVis ? weaponColorVisible[1] : weaponColorHidden[1],
                   isVis ? weaponColorVisible[2] : weaponColorHidden[2],
                   isVis ? weaponColorVisible[3] : weaponColorHidden[3]));
        uintptr_t weaponServices = Utils::SafeRead<uintptr_t>(
            reinterpret_cast<uintptr_t>(pawn) + Offsets::m_pWeaponServices);
        if (weaponServices) {
          uint32_t activeWeaponHandle = Utils::SafeRead<uint32_t>(
              weaponServices + Offsets::m_hActiveWeapon);
          if (activeWeaponHandle != 0xFFFFFFFF) {
            C_BaseEntity *weaponEnt =
                EntityManager::Get().GetEntityFromHandle(activeWeaponHandle);
            if (weaponEnt) {
              short weaponIdx = Utils::SafeRead<short>(
                  reinterpret_cast<uintptr_t>(weaponEnt) +
                  Offsets::sc_m_AttributeManager + Offsets::sc_m_Item +
                  Offsets::sc_m_iItemDefinitionIndex);
              if (weaponIdx > 0 && weaponIdx < 1000) {
                const char *weaponIconStr = GetIconFromWeaponId(weaponIdx);
                if (weaponIconStr && weaponIconStr[0] != '\0') {
                  ImVec2 wTs;
                  if (weapon_icon_font) {
                    wTs = weapon_icon_font->CalcTextSizeA(
                        weapon_icon_font->FontSize, FLT_MAX, 0.0f,
                        weaponIconStr);
                  } else {
                    wTs = ImGui::CalcTextSize(weaponIconStr);
                  }

                  ImVec2 wPos = {x + (width - wTs.x) * 0.5f, y + height + 2.0f};

                  if (weapon_icon_font) {
                    dl->AddText(weapon_icon_font, weapon_icon_font->FontSize,
                                {wPos.x + 1.0f, wPos.y + 1.0f},
                                IM_COL32(0, 0, 0, 255), weaponIconStr);
                    dl->AddText(weapon_icon_font, weapon_icon_font->FontSize,
                                wPos, weapCol, weaponIconStr);
                  } else {
                    dl->AddText({wPos.x + 1.0f, wPos.y + 1.0f},
                                IM_COL32(0, 0, 0, 255), weaponIconStr);
                    dl->AddText(wPos, weapCol, weaponIconStr);
                  }
                }
              }
            }
          }
        }
      }

      if (espSkeletonEnabled) {

        const ImU32 skelColVis = ImGui::ColorConvertFloat4ToU32(
            ImVec4(skeletonColorVisible[0], skeletonColorVisible[1],
                   skeletonColorVisible[2], skeletonColorVisible[3]));
        const ImU32 skelColHidden = ImGui::ColorConvertFloat4ToU32(
            ImVec4(skeletonColorHidden[0], skeletonColorHidden[1],
                   skeletonColorHidden[2], skeletonColorHidden[3]));

        if (espSkeletonType == 1) {
          for (const auto &chain : g_BoneChains) {
            std::vector<ImVec2> points;

            for (auto boneId : chain) {
              Vector bonePos = Utils::GetBonePos(pawn, boneId);
              Vector screenPos;

              if (!Utils::WorldToScreen(bonePos, screenPos,
                                        (float *)Globals::ViewMatrix, sw, sh))
                continue;

              points.push_back(ImVec2(screenPos.x, screenPos.y));
            }

            if (points.size() < 2)
              continue;

            points.insert(points.begin(), points.front());
            points.push_back(points.back());

            bool chainVisible = false;
            for (auto boneId : chain) {
              Vector bonePos = Utils::GetBonePos(pawn, boneId);
              if (IsBoneVisibleRaycast(bonePos)) {
                chainVisible = true;
                break;
              }
            }
            ImU32 segCol = chainVisible ? skelColVis : skelColHidden;

            ImVec2 last = {};
            bool first = true;

            for (size_t i = 0; i + 3 < points.size(); i++) {
              const auto &p0 = points[i];
              const auto &p1 = points[i + 1];
              const auto &p2 = points[i + 2];
              const auto &p3 = points[i + 3];

              for (float t = 0.f; t <= 1.f; t += 0.08f) {
                auto pt = CatmullRom(p0, p1, p2, p3, t);

                if (first) {
                  last = pt;
                  first = false;
                  continue;
                }

                dl->AddLine(last, pt, segCol, Globals::esp_skeleton_thickness);
                last = pt;
              }
            }
          }
        } else {
          for (const auto &conn : Bones::connections) {
            Vector b1 = Utils::GetBonePos(pawn, conn.bone1);
            Vector b2 = Utils::GetBonePos(pawn, conn.bone2);
            Vector sb1, sb2;
            if (Utils::WorldToScreen(b1, sb1, (float *)Globals::ViewMatrix, sw,
                                     sh) &&
                Utils::WorldToScreen(b2, sb2, (float *)Globals::ViewMatrix, sw,
                                     sh)) {

              bool b1Vis = IsBoneVisibleRaycast(b1);
              bool b2Vis = IsBoneVisibleRaycast(b2);
              ImU32 segCol = (b1Vis || b2Vis) ? skelColVis : skelColHidden;

              dl->AddLine({sb1.x, sb1.y}, {sb2.x, sb2.y}, segCol,
                          Globals::esp_skeleton_thickness);
            }
          }
        }
      }

      if (espHeadEnabled) {
        const ImU32 headColVis = ImGui::ColorConvertFloat4ToU32(
            ImVec4(skeletonColorVisible[0], skeletonColorVisible[1],
                   skeletonColorVisible[2], skeletonColorVisible[3]));
        const ImU32 headColHidden = ImGui::ColorConvertFloat4ToU32(
            ImVec4(skeletonColorHidden[0], skeletonColorHidden[1],
                   skeletonColorHidden[2], skeletonColorHidden[3]));

        Vector head3D = Utils::GetBonePos(pawn, BoneID::Head);
        Vector neck3D = Utils::GetBonePos(pawn, BoneID::Neck);
        Vector sHeadPos, sNeckPos;
        if (Utils::WorldToScreen(head3D, sHeadPos, (float *)Globals::ViewMatrix,
                                 sw, sh) &&
            Utils::WorldToScreen(neck3D, sNeckPos, (float *)Globals::ViewMatrix,
                                 sw, sh)) {
          bool headVis = IsBoneVisibleRaycast(head3D);
          ImU32 headCol = headVis ? headColVis : headColHidden;

          float radius = std::abs(sHeadPos.y - sNeckPos.y) + 2.0f;
          dl->AddCircle({sHeadPos.x, sHeadPos.y}, radius, headCol, 0,
                        Globals::esp_skeleton_thickness);
        }
      }
    }
  }

void ESP::Render() {
  __try {
    RenderInternal();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
