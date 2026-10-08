#pragma once
#include "launcher/util/log_buffer.h"
#include "launcher/util/process_launcher.h"
#include "core/config.h"
#include <string>

class ToolLauncherPanel {
public:
    ToolLauncherPanel(LogBuffer& log, Config& config);
    void Render();

private:
    LogBuffer& m_log;
    Config& m_config;

    ProcessLauncher m_researchTool;
    ProcessLauncher m_gspots;

    // Paths
    std::wstring m_exeDir;        // Directory where launcher exe lives
    std::wstring m_projectDir;    // Project root
    std::wstring m_researchToolPath;
    std::wstring m_gspotsPath;
    std::wstring m_gameExePath;
    std::wstring m_configPath;
};
