#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

class DashboardWindow {
public:
    DashboardWindow() = default;
    ~DashboardWindow();

    DashboardWindow(const DashboardWindow&) = delete;
    DashboardWindow& operator=(const DashboardWindow&) = delete;

    bool Initialize(int width = 1200, int height = 800);
    void BeginFrame();
    void EndFrame();
    void Shutdown();

    HWND GetHwnd() const { return m_hwnd; }
    ID3D11Device* GetDevice() const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    bool ShouldClose() const { return m_shouldClose; }

private:
    bool CreateAppWindow(int width, int height);
    bool InitD3D11();
    void HandleResize();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapchain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;

    int m_width = 0;
    int m_height = 0;
    bool m_shouldClose = false;
    bool m_resizePending = false;
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;

    WNDCLASSEXW m_wndClass = {};
};
