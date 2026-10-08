#pragma once
#include "launcher/util/log_buffer.h"

class LogPanel {
public:
    explicit LogPanel(LogBuffer& log);
    void Render();

private:
    LogBuffer& m_log;
    bool m_autoScroll = true;
    int m_levelFilter = 0;         // 0=ALL, 1=INFO+, 2=WARN+, 3=ERROR only
    char m_searchFilter[256] = {};
    int m_sourceFilter = 0;        // 0=All, 1=dashboard, 2=research-tool, 3=gspots
};
