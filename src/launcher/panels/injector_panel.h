#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "launcher/util/log_buffer.h"
#include <string>

class InjectorPanel {
public:
    explicit InjectorPanel(LogBuffer& log);
    void Render();

private:
    LogBuffer& m_log;

    char m_processNameBuf[256] = "DungeonCrawler.exe";
    char m_dllPathBuf[MAX_PATH] = {};

    DWORD m_targetPid = 0;
    bool m_processFound = false;

    // Injection method: 0 = proxy (recommended), 1 = manual map, 2 = LoadLibrary
    int m_injectionMethod = 0;

    // Legacy compat
    bool m_useManualMap = true;

    std::string m_lastStatus;
    bool m_lastSuccess = false;

    // Proxy deployment
    bool m_proxyDeployed = false;
    void CheckProxyStatus();
    void DeployProxy();
    void RemoveProxy();
};
