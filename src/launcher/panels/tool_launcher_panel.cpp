#include "tool_launcher_panel.h"
#include "launcher/overlay_theme.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <shellapi.h>
#include <filesystem>

ToolLauncherPanel::ToolLauncherPanel(LogBuffer& log, Config& config)
    : m_log(log), m_config(config),
      m_researchTool("research-tool", log),
      m_gspots("gspots", log)
{
    // Determine paths relative to the executable
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    m_exeDir = std::filesystem::path(exePath).parent_path().wstring();

    // Project root is typically 2 levels up from build/Release/
    auto projectDir = std::filesystem::path(exePath).parent_path().parent_path().parent_path();
    m_projectDir = projectDir.wstring();

    m_researchToolPath = m_exeDir + L"\\ext-research-tool.exe";
    m_gspotsPath = m_projectDir + L"\\tools\\gspots\\GSpots.exe";
    m_gameExePath = L"D:\\SteamLibrary\\steamapps\\common\\Dark and Darker\\DungeonCrawler\\Binaries\\Win64\\DungeonCrawler.exe";
    m_configPath = m_projectDir + L"\\config\\settings.json";
}

void ToolLauncherPanel::Render() {
    if (!ImGui::Begin("Armory")) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));

    // ── ESP Overlay ──
    DaDTheme::GoldSeparator("ESP Overlay");
    {
        bool running = m_researchTool.IsRunning();
        if (running) {
            ImGui::TextColored(DaDTheme::Success, "  Active (PID: %u)", m_researchTool.GetPid());
        } else {
            ImGui::TextColored(DaDTheme::TextDisabled, "  Inactive");
        }

        ImGui::Spacing();

        // Themed button
        if (!running) {
            ImGui::PushStyleColor(ImGuiCol_Button, DaDTheme::GoldSubtle);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DaDTheme::GoldDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, DaDTheme::GoldDark);
            if (ImGui::Button("Enter Dungeon##research", ImVec2(-1, 30))) {
                if (!m_researchTool.Start(m_researchToolPath, L"", m_projectDir)) {
                    m_log.Push("Failed to start research tool", spdlog::level::err);
                }
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.2f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.1f, 0.08f, 1.0f));
            if (ImGui::Button("Leave Dungeon##research", ImVec2(-1, 30))) {
                m_researchTool.Stop();
            }
            ImGui::PopStyleColor(3);
        }
    }

    ImGui::Spacing();

    // ── GSpots Scanner ──
    DaDTheme::GoldSeparator("Offset Scanner");
    {
        bool running = m_gspots.IsRunning();
        if (running) {
            ImGui::TextColored(DaDTheme::Warning, "  Scanning for treasure...");
        } else {
            ImGui::TextColored(DaDTheme::TextDisabled, "  Idle");
        }

        ImGui::Spacing();

        if (!running) {
            if (ImGui::Button("Scan Offsets##gspots", ImVec2(-1, 28))) {
                std::wstring args = L"\"" + m_gameExePath + L"\"";
                if (!m_gspots.Start(m_gspotsPath, args, m_projectDir)) {
                    m_log.Push("Failed to start GSpots", spdlog::level::err);
                }
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Scanning...##gspots", ImVec2(-1, 28));
            ImGui::EndDisabled();
        }

        std::string lastLine = m_gspots.GetLastLine();
        if (!lastLine.empty()) {
            ImGui::TextColored(DaDTheme::TextSecondary, "%s", lastLine.c_str());
        }
    }

    ImGui::Spacing();

    // ── Config File ──
    DaDTheme::GoldSeparator("Scrolls & Settings");
    {
        if (ImGui::Button("Open Config File", ImVec2(-1, 0))) {
            ShellExecuteW(nullptr, L"open", m_configPath.c_str(),
                         nullptr, nullptr, SW_SHOWNORMAL);
        }

        if (ImGui::Button("Reload Config", ImVec2(-1, 0))) {
            m_config.Load();
            m_log.Push("Config reloaded from disk", spdlog::level::info);
        }
    }

    ImGui::PopStyleVar();
    ImGui::End();
}
