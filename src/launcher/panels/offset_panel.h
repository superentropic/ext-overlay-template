#pragma once
#include "core/config.h"
#include "launcher/util/log_buffer.h"

class OffsetPanel {
public:
    OffsetPanel(Config& config, LogBuffer& log);
    void Render();

private:
    void LoadFromConfig();

    Config& m_config;
    LogBuffer& m_log;

    char m_gworldBuf[32] = {};
    char m_gnamesBuf[32] = {};
    char m_gobjectsBuf[32] = {};
    char m_ueVersionBuf[64] = {};

    char m_gworldPatternBuf[256] = {};
    char m_gnamesPatternBuf[256] = {};
    char m_gobjectsPatternBuf[256] = {};
};
