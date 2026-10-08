#pragma once
#include "core/config.h"
#include "core/process.h"
#include <chrono>

class GameStatusPanel {
public:
    explicit GameStatusPanel(Config& config);
    void Render();
    void Update();  // Call periodically to refresh status

private:
    Config& m_config;

    bool m_gameRunning = false;
    DWORD m_gamePid = 0;
    uintptr_t m_gameBase = 0;
    bool m_driverLoaded = false;

    std::chrono::steady_clock::time_point m_lastUpdate{};
    static constexpr float UPDATE_INTERVAL_SECONDS = 2.0f;
};
