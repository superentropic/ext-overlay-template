#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Initialize(HWND targetWindow);
    void BeginFrame();
    void EndFrame();
    void Shutdown();

    HWND GetOverlayHandle() const { return m_overlayHwnd; }
    HWND GetTargetHandle() const { return m_targetHwnd; }

    ID3D11Device* GetDevice() const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // Enable/disable mouse passthrough (for menu interaction)
    void SetClickThrough(bool clickThrough);

private:
    bool CreateOverlayWindow();
    bool InitD3D11();
    void TrackTargetWindow();
    void ResizeBuffers(int width, int height);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_overlayHwnd = nullptr;
    HWND m_targetHwnd = nullptr;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapchain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;

    int m_width = 0;
    int m_height = 0;
    bool m_clickThrough = true;

    // TrackTargetWindow throttling — no need to call SetWindowPos every frame
    DWORD m_lastTrackTick = 0;
    static constexpr DWORD TRACK_INTERVAL_MS = 100; // Check window position every 100ms

    WNDCLASSEXW m_wndClass = {};
    wchar_t m_className[64] = {};
};
#else
#include "overlay_linux.h"
#endif
