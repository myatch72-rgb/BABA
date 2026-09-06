#include "EnemyCounter.h"
#include "../../../sdk/entity/EntityManager.h"
#include "../../../sdk/utils/Utils.h"
#include "../../../sdk/utils/Globals.h"
#include "../../../../ext/imgui/imgui.h"
#include <algorithm>

extern ImFont* esp_font;
void EnemyCounter::Render()
{
    auto& em = EntityManager::Get();
    C_CSPlayerPawn* local = em.GetLocalPawn();
    if (!local)
        return;

    int aliveEnemies = 0;

    for (const auto& ent : em.GetEntities())
    {
        if (!ent.pawn)
            continue;

        if (!ent.pawn->IsAlive())
            continue;

        if (ent.pawn->m_iTeamNum() == local->m_iTeamNum())
            continue;

        aliveEnemies++;
    }

    char buf[32];
    sprintf_s(buf, "ENEMIES: %d", aliveEnemies);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 pos = { 20.f, 20.f };
    
    if (esp_font) {
        dl->AddText(esp_font, esp_font->FontSize, { pos.x + 1.f, pos.y + 1.f }, IM_COL32(0, 0, 0, 255), buf);
        dl->AddText(esp_font, esp_font->FontSize, pos, IM_COL32(255, 255, 255, 220), buf);
    } else {
        dl->AddText({ pos.x + 1.f, pos.y + 1.f }, IM_COL32(0, 0, 0, 255), buf);
        dl->AddText(pos, IM_COL32(255, 255, 255, 220), buf);
    }
}
