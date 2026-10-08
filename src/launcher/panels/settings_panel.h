#pragma once
#include "core/config.h"
#include "launcher/util/log_buffer.h"

class SettingsPanel {
public:
    SettingsPanel(Config& config, LogBuffer& log);
    void Render();

private:
    Config& m_config;
    LogBuffer& m_log;
};
