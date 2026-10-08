#include "settings_panel.h"
#include "launcher/overlay_theme.h"
#include <imgui.h>

SettingsPanel::SettingsPanel(Config& config, LogBuffer& log)
    : m_config(config), m_log(log) {}

void SettingsPanel::Render() {
    if (!ImGui::Begin("Merchant")) {
        ImGui::End();
        return;
    }

    auto& filter = m_config.Get().filter;
    auto& visuals = m_config.Get().visuals;

    // ── Filter Settings ──
    if (ImGui::CollapsingHeader("ESP Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Players", &filter.showPlayers);
        ImGui::Checkbox("Show Loot", &filter.showLoot);
        ImGui::Checkbox("Show NPCs", &filter.showNPCs);
        ImGui::Checkbox("Show Portals", &filter.showPortals);
        ImGui::Checkbox("Show Health Bars", &filter.showHealthBars);
        ImGui::Checkbox("Show Distance", &filter.showDistance);
        ImGui::Checkbox("Show Snap Lines", &filter.showSnapLines);

        ImGui::Spacing();
        ImGui::SliderFloat("Max Distance (m)", &filter.maxDistance, 10.0f, 300.0f, "%.0f");

        // Rarity filter starts at Uncommon — Junk/Poor/Common are never useful
        const char* rarityNames[] = {"Uncommon", "Rare", "Epic", "Legendary", "Unique"};
        const int RARITY_OFFSET = static_cast<int>(ItemRarity::UNCOMMON);
        int rarityIdx = static_cast<int>(filter.minLootRarity) - RARITY_OFFSET;
        if (rarityIdx < 0) rarityIdx = 0;
        if (rarityIdx >= IM_ARRAYSIZE(rarityNames)) rarityIdx = IM_ARRAYSIZE(rarityNames) - 1;
        if (ImGui::Combo("Min Loot Rarity", &rarityIdx, rarityNames, IM_ARRAYSIZE(rarityNames))) {
            filter.minLootRarity = static_cast<ItemRarity>(rarityIdx + RARITY_OFFSET);
        }
    }

    // ── Visual Settings ──
    if (ImGui::CollapsingHeader("Visuals", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4("Player Color", visuals.playerColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("Team Color", visuals.teamColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("NPC Color", visuals.npcColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

        ImGui::SliderFloat("Font Size", &visuals.fontSize, 10.0f, 24.0f, "%.0f");

        const char* boxStyles[] = {"2D Box", "Filled Box", "Corner Box"};
        ImGui::Combo("Box Style", &visuals.boxStyle, boxStyles, IM_ARRAYSIZE(boxStyles));
    }

    // ── Performance ──
    if (ImGui::CollapsingHeader("Performance")) {
        ImGui::SliderInt("Update Rate (FPS)", &m_config.Get().updateRateFps, 10, 60);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Save / Load ──
    if (ImGui::Button("Save Config", ImVec2(-1, 30))) {
        m_config.Save();
        m_log.Push("Config saved", spdlog::level::info);
    }

    if (ImGui::Button("Reload Config", ImVec2(-1, 0))) {
        m_config.Load();
        m_log.Push("Config reloaded from disk", spdlog::level::info);
    }

    ImGui::End();
}
