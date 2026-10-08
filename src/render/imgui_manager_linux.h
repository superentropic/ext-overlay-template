#pragma once

#include <GLFW/glfw3.h>

class ImGuiManager {
public:
    bool Initialize(GLFWwindow* window, float dpiScale = 1.0f);
    bool Initialize(GLFWwindow* window, void*, void*, float dpiScale = 1.0f) {
        return Initialize(window, dpiScale);
    }
    void BeginFrame();
    void EndFrame();
    void Shutdown();

private:
    bool m_initialized = false;
};

using LinuxImGuiManager = ImGuiManager;
