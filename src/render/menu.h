#pragma once
#include "core/config.h"

class Process;

class Menu {
public:
    explicit Menu(Config& config) : m_configStore(config), m_config(config.Get()) {}

    void Render();
    bool IsVisible() const { return m_visible; }
    void Toggle() { m_visible = !m_visible; }

    void SetProcess(Process* process) { m_process = process; }

    // Debug info setters
    void SetEntityCount(int count) { m_entityCount = count; }
    void SetFps(float fps) { m_fps = fps; }
    void SetGWorldAddr(uintptr_t addr) { m_gworldAddr = addr; }
    void SetGNamesAddr(uintptr_t addr) { m_gnamesAddr = addr; }
    void SetAttached(bool attached) { m_attached = attached; }
    void SetProxyActive(bool active) { m_proxyActive = active; }
    void SetRedirecting(bool redirecting) { m_redirecting = redirecting; }

    struct ProxyTelemetry {
        bool moduleFound = false;
        uintptr_t moduleBase = 0;
        uint32_t moduleSize = 0;
        uintptr_t shmAddr = 0;
        bool isConnected = false;
        uint32_t heartbeat = 0;
        bool hooksInstalled = false;
        uint32_t hookCallCountController = 0;
        uint32_t hookCallCountLocalPlayer = 0;
        uint32_t redirectCount = 0;
        uint32_t viewPointRestoreCount = 0;
        uint32_t lastHookStatus = 0;
        uintptr_t playerControllerAddr = 0;
        uintptr_t localPlayerAddr = 0;
        bool isRedirecting = false;
        double redirectPitch = 0.0;
        double redirectYaw = 0.0;
        bool hasTarget = false;
        double targetX = 0.0;
        double targetY = 0.0;
        double targetZ = 0.0;
        float targetDistance = 0.0f;
        uintptr_t manualBase = 0;
        bool manualBaseFailed = false;
    };

    void SetProxyTelemetry(const ProxyTelemetry& tel) {
        m_proxyTel = tel;
        m_proxyActive = tel.isConnected;
        m_redirecting = tel.isRedirecting;
    }

    const ProxyTelemetry& GetProxyTelemetry() const { return m_proxyTel; }

private:
    Config& m_configStore;
    AppConfig& m_config;
    Process* m_process = nullptr;
    bool m_visible = false;
    bool m_fontSizeChanged = false;

    char m_manualBaseBuffer[32] = {};
    bool m_manualBaseBufferInit = false;

    // Debug info
    int m_entityCount = 0;
    float m_fps = 0.0f;
    uintptr_t m_gworldAddr = 0;
    uintptr_t m_gnamesAddr = 0;
    bool m_attached = false;
    bool m_proxyActive = false;
    bool m_redirecting = false;
    ProxyTelemetry m_proxyTel = {};
};
