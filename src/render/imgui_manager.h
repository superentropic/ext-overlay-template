#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <d3d11.h>

class ImGuiManager {
public:
    bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
                    float dpiScale = 1.0f);
    void BeginFrame();
    void EndFrame();
    void Shutdown();

    float GetDpiScale() const { return m_dpiScale; }

private:
    bool m_initialized = false;
    float m_dpiScale = 1.0f;
};
#else
#include "imgui_manager_linux.h"
#endif
