#include "offset_panel.h"
#include "launcher/overlay_theme.h"
#include <imgui.h>
#include <cstdio>
#include <cstdlib>

OffsetPanel::OffsetPanel(Config& config, LogBuffer& log)
    : m_config(config), m_log(log)
{
    LoadFromConfig();
}

void OffsetPanel::LoadFromConfig() {
    snprintf(m_gworldBuf, sizeof(m_gworldBuf), "0x%llX", m_config.Get().gworldOffset);
    snprintf(m_gnamesBuf, sizeof(m_gnamesBuf), "0x%llX", m_config.Get().gnamesOffset);
    snprintf(m_gobjectsBuf, sizeof(m_gobjectsBuf), "0x%llX", m_config.Get().gobjectsOffset);
    snprintf(m_ueVersionBuf, sizeof(m_ueVersionBuf), "%s", m_config.Get().ueVersion.c_str());

    snprintf(m_gworldPatternBuf, sizeof(m_gworldPatternBuf), "%s",
             m_config.Get().gworldPattern.c_str());
    snprintf(m_gnamesPatternBuf, sizeof(m_gnamesPatternBuf), "%s",
             m_config.Get().gnamesPattern.c_str());
    snprintf(m_gobjectsPatternBuf, sizeof(m_gobjectsPatternBuf), "%s",
             m_config.Get().gobjectsPattern.c_str());
}

void OffsetPanel::Render() {
    if (!ImGui::Begin("Ancient Scrolls")) {
        ImGui::End();
        return;
    }

    // ── Direct Offsets ──
    DaDTheme::GoldSeparator("GSpots Offsets");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GWorld", m_gworldBuf, sizeof(m_gworldBuf));

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GNames", m_gnamesBuf, sizeof(m_gnamesBuf));

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GObjects", m_gobjectsBuf, sizeof(m_gobjectsBuf));

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("UE Version", m_ueVersionBuf, sizeof(m_ueVersionBuf));

    ImGui::Spacing();

    // ── Pattern Signatures ──
    DaDTheme::GoldSeparator("Pattern Signatures");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GWorld Pattern", m_gworldPatternBuf, sizeof(m_gworldPatternBuf));

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GNames Pattern", m_gnamesPatternBuf, sizeof(m_gnamesPatternBuf));

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("GObjects Pattern", m_gobjectsPatternBuf, sizeof(m_gobjectsPatternBuf));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Action Buttons ──
    if (ImGui::Button("Save to Config", ImVec2(-1, 30))) {
        // Parse hex offsets
        auto parseHex = [](const char* str) -> uintptr_t {
            if (!str || str[0] == '\0') return 0;
            return std::strtoull(str, nullptr, 16);
        };

        m_config.Get().gworldOffset = parseHex(m_gworldBuf);
        m_config.Get().gnamesOffset = parseHex(m_gnamesBuf);
        m_config.Get().gobjectsOffset = parseHex(m_gobjectsBuf);
        m_config.Get().ueVersion = m_ueVersionBuf;
        m_config.Get().gworldPattern = m_gworldPatternBuf;
        m_config.Get().gnamesPattern = m_gnamesPatternBuf;
        m_config.Get().gobjectsPattern = m_gobjectsPatternBuf;

        m_config.Save();
        m_log.Push("Offsets saved to config", spdlog::level::info);
    }

    if (ImGui::Button("Reset from Config", ImVec2(-1, 0))) {
        m_config.Load();
        LoadFromConfig();
        m_log.Push("Offsets reset from config file", spdlog::level::info);
    }

    ImGui::End();
}
