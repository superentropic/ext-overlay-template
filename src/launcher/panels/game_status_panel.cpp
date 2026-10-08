#include "game_status_panel.h"
#include "launcher/util/driver_detector.h"
#include "launcher/overlay_theme.h"
#include <imgui.h>

GameStatusPanel::GameStatusPanel(Config& config) : m_config(config) {
    Update(); // Initial check
}

void GameStatusPanel::Update() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - m_lastUpdate).count();

    if (elapsed < UPDATE_INTERVAL_SECONDS && m_lastUpdate.time_since_epoch().count() > 0) {
        return; // Not time yet
    }
    m_lastUpdate = now;

    // Check game process
    DWORD pid = Process::FindProcessId(L"DungeonCrawler.exe");
    m_gameRunning = (pid != 0);
    m_gamePid = pid;

    if (m_gameRunning && m_gameBase == 0) {
        // Try to get base address
        Process probe;
        if (probe.Attach(pid)) {
            m_gameBase = probe.GetModuleBase(L"DungeonCrawler.exe");
            if (!m_gameBase) m_gameBase = probe.GetModuleBase(L"DungeonCrawler-Win64-Shipping.exe");
            probe.Detach();
        }
    } else if (!m_gameRunning) {
        m_gameBase = 0;
    }

    // Check anti-cheat driver
    m_driverLoaded = DriverDetector::IsTvkDriverLoaded();
}

void GameStatusPanel::Render() {
    if (!ImGui::Begin("Game Status")) {
        ImGui::End();
        return;
    }

    // ── Game Process ──
    DaDTheme::GoldSeparator("Dungeon Status");

    if (m_gameRunning) {
        ImGui::TextColored(DaDTheme::Success, "  DungeonCrawler.exe — ALIVE");
        ImGui::TextColored(DaDTheme::TextSecondary, "  PID: %u", m_gamePid);
        if (m_gameBase) {
            ImGui::TextColored(DaDTheme::TextSecondary, "  Base: 0x%llX", m_gameBase);
        }
    } else {
        ImGui::TextColored(DaDTheme::Danger, "  DungeonCrawler.exe — OFFLINE");
        ImGui::TextColored(DaDTheme::TextDisabled, "  Launch the game to begin");
    }

    ImGui::Spacing();

    // ── Anti-Cheat Driver ──
    DaDTheme::GoldSeparator("Anti-Cheat (tvk.sys)");

    if (m_driverLoaded) {
        ImGui::TextColored(DaDTheme::Warning, "  tvk.sys — ACTIVE");
        ImGui::TextColored(DaDTheme::TextSecondary,
            "  Use Manual Map injection to bypass");
    } else {
        ImGui::TextColored(DaDTheme::TextDisabled, "  tvk.sys — Not loaded");
    }

    ImGui::Spacing();

    // ── Quick Offset Info ──
    DaDTheme::GoldSeparator("Active Offsets");

    ImGui::TextColored(DaDTheme::Gold, "GWorld:  "); ImGui::SameLine();
    ImGui::Text("0x%llX", m_config.Get().gworldOffset);

    ImGui::TextColored(DaDTheme::Gold, "GNames:  "); ImGui::SameLine();
    ImGui::Text("0x%llX", m_config.Get().gnamesOffset);

    ImGui::TextColored(DaDTheme::Gold, "GObjects:"); ImGui::SameLine();
    ImGui::Text("0x%llX", m_config.Get().gobjectsOffset);

    if (m_gameBase && m_config.Get().gworldOffset) {
        ImGui::Spacing();
        ImGui::TextColored(DaDTheme::TextDisabled, "Resolved addresses:");
        ImGui::TextColored(DaDTheme::TextDisabled, "  GWorld:   0x%llX", m_gameBase + m_config.Get().gworldOffset);
        ImGui::TextColored(DaDTheme::TextDisabled, "  GNames:   0x%llX", m_gameBase + m_config.Get().gnamesOffset);
        ImGui::TextColored(DaDTheme::TextDisabled, "  GObjects: 0x%llX", m_gameBase + m_config.Get().gobjectsOffset);
    }

    ImGui::End();
}
