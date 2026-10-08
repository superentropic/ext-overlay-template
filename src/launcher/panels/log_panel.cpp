#include "log_panel.h"
#include "launcher/overlay_theme.h"
#include <imgui.h>
#include <ctime>
#include <algorithm>

LogPanel::LogPanel(LogBuffer& log) : m_log(log) {}

void LogPanel::Render() {
    if (!ImGui::Begin("Adventure Log")) {
        ImGui::End();
        return;
    }

    // ── Controls bar ──
    const char* levelNames[] = {"All", "Info+", "Warn+", "Error"};
    ImGui::SetNextItemWidth(80);
    ImGui::Combo("Level", &m_levelFilter, levelNames, IM_ARRAYSIZE(levelNames));

    ImGui::SameLine();
    const char* sourceNames[] = {"All", "dashboard", "research-tool", "gspots"};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Source", &m_sourceFilter, sourceNames, IM_ARRAYSIZE(sourceNames));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("Filter", m_searchFilter, sizeof(m_searchFilter));

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_log.Clear();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu entries)", m_log.Size());

    ImGui::Separator();

    // ── Log content ──
    ImGui::BeginChild("LogScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_HorizontalScrollbar);

    auto entries = m_log.GetAll();
    std::string searchStr(m_searchFilter);
    std::string searchLower = searchStr;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

    // Map level filter to spdlog level
    spdlog::level::level_enum minLevel = spdlog::level::trace;
    if (m_levelFilter == 1) minLevel = spdlog::level::info;
    else if (m_levelFilter == 2) minLevel = spdlog::level::warn;
    else if (m_levelFilter == 3) minLevel = spdlog::level::err;

    for (const auto& entry : entries) {
        // Level filter
        if (entry.level < minLevel) continue;

        // Source filter
        if (m_sourceFilter > 0) {
            const char* requiredSource = sourceNames[m_sourceFilter];
            if (entry.source != requiredSource) continue;
        }

        // Text search filter
        if (!searchLower.empty()) {
            std::string msgLower = entry.message;
            std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);
            if (msgLower.find(searchLower) == std::string::npos) continue;
        }

        // Format timestamp
        auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
        struct tm tm_buf{};
        localtime_s(&tm_buf, &time);
        char timeBuf[16];
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm_buf);

        // Level color — themed
        ImVec4 color;
        const char* levelTag;
        switch (entry.level) {
            case spdlog::level::warn:
                color = DaDTheme::Warning;
                levelTag = "WARN";
                break;
            case spdlog::level::err:
            case spdlog::level::critical:
                color = DaDTheme::Danger;
                levelTag = "ERR ";
                break;
            case spdlog::level::debug:
                color = DaDTheme::Info;
                levelTag = "DBG ";
                break;
            default:
                color = DaDTheme::TextPrimary;
                levelTag = "INFO";
                break;
        }

        // Render line
        ImGui::TextColored(DaDTheme::TextDisabled, "[%s]", timeBuf);
        ImGui::SameLine();
        ImGui::TextColored(color, "[%s]", levelTag);
        ImGui::SameLine();
        ImGui::TextColored(DaDTheme::GoldDim, "[%s]", entry.source.c_str());
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", entry.message.c_str());
    }

    // Auto-scroll
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}
