#pragma once

#include <GLFW/glfw3.h>
#include <string>

class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Initialize(void* targetWindow = nullptr);
    void BeginFrame();
    void EndFrame();
    void Shutdown();
    void SetClickThrough(bool clickThrough);

    GLFWwindow* GetWindow() const { return m_window; }
    GLFWwindow* GetOverlayHandle() const { return m_window; }
    void* GetDevice() const { return nullptr; }
    void* GetContext() const { return nullptr; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void SetInputRegion(bool enabled);

    GLFWwindow* m_window = nullptr;
    std::string m_windowTitle;
    int m_width = 0;
    int m_height = 0;
    bool m_clickThrough = true;
    double m_lastRaiseTime = 0.0;
};

using LinuxOverlay = Overlay;
