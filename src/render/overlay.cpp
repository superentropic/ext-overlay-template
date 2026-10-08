#include "overlay.h"
#include <dwmapi.h>
#include <spdlog/spdlog.h>
#include <imgui.h>
#include <random>
#include <string>

// ImGui WndProc handler (defined in imgui_impl_win32.cpp)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

Overlay::~Overlay() {
    Shutdown();
}

bool Overlay::Initialize(HWND targetWindow) {
    m_targetHwnd = targetWindow;

    if (!m_targetHwnd) {
        spdlog::error("Target window handle is null");
        return false;
    }

    if (!CreateOverlayWindow()) {
        spdlog::error("Failed to create overlay window");
        return false;
    }

    if (!InitD3D11()) {
        spdlog::error("Failed to initialize D3D11");
        return false;
    }

    spdlog::info("Overlay initialized ({}x{})", m_width, m_height);
    return true;
}

bool Overlay::CreateOverlayWindow() {
    // Generate a random window class name to avoid detection by class name scanning
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 25);

    for (int i = 0; i < 12; i++) {
        m_className[i] = L'a' + static_cast<wchar_t>(dist(gen));
    }
    m_className[12] = L'\0';

    m_wndClass = {};
    m_wndClass.cbSize = sizeof(WNDCLASSEXW);
    m_wndClass.style = CS_HREDRAW | CS_VREDRAW;
    m_wndClass.lpfnWndProc = WndProc;
    m_wndClass.hInstance = GetModuleHandleW(nullptr);
    m_wndClass.lpszClassName = m_className;

    if (!RegisterClassExW(&m_wndClass)) {
        spdlog::error("Failed to register window class. Error: {}", GetLastError());
        return false;
    }

    // Get target window CLIENT area (excludes title bar and borders)
    RECT clientRect;
    GetClientRect(m_targetHwnd, &clientRect);
    POINT clientTopLeft = {0, 0};
    ClientToScreen(m_targetHwnd, &clientTopLeft);
    m_width = clientRect.right;
    m_height = clientRect.bottom;

    spdlog::info("Game client area: {}x{} at ({},{})", m_width, m_height,
        clientTopLeft.x, clientTopLeft.y);

    // Create transparent, topmost, click-through window
    m_overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        m_className,
        L"",
        WS_POPUP,
        clientTopLeft.x, clientTopLeft.y,
        m_width, m_height,
        nullptr, nullptr,
        m_wndClass.hInstance,
        nullptr
    );

    if (!m_overlayHwnd) {
        spdlog::error("Failed to create overlay window. Error: {}", GetLastError());
        return false;
    }

    // Make black pixels transparent
    SetLayeredWindowAttributes(m_overlayHwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    // Extend glass frame for proper alpha blending
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_overlayHwnd, &margins);

    ShowWindow(m_overlayHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_overlayHwnd);

    // Log window info for debugging
    LONG targetExStyle = GetWindowLongW(m_targetHwnd, GWL_EXSTYLE);
    LONG targetStyle = GetWindowLongW(m_targetHwnd, GWL_STYLE);
    spdlog::info("Target window style=0x{:08X}, exStyle=0x{:08X}", targetStyle, targetExStyle);
    spdlog::info("Overlay HWND=0x{:X}, size={}x{}, pos=({},{})",
        reinterpret_cast<uintptr_t>(m_overlayHwnd), m_width, m_height,
        clientTopLeft.x, clientTopLeft.y);

    // Check if overlay is actually visible
    if (IsWindowVisible(m_overlayHwnd)) {
        spdlog::info("Overlay window IS visible");
    } else {
        spdlog::error("Overlay window is NOT visible!");
    }

    return true;
}

bool Overlay::InitD3D11() {
    // NOTE: We use legacy DXGI_SWAP_EFFECT_DISCARD (not FLIP_DISCARD) because our
    // transparency relies on SetLayeredWindowAttributes(LWA_COLORKEY), which is
    // incompatible with the flip presentation model. The frame rate limiter in main.cpp
    // handles the GPU load — no need for flip model to control presentation.
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {0, 1};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_overlayHwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &m_swapchain, &m_device, &featureLevel, &m_context
    );

    if (FAILED(hr)) {
        spdlog::error("D3D11CreateDeviceAndSwapChain failed: 0x{:X}", static_cast<unsigned>(hr));
        return false;
    }

    // Create render target view
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }

    spdlog::info("D3D11 initialized. Feature level: 0x{:X}", static_cast<unsigned>(featureLevel));
    return true;
}

void Overlay::BeginFrame() {
    TrackTargetWindow();

    // Clear with transparent black
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_context->ClearRenderTargetView(m_rtv, clearColor);
}

void Overlay::EndFrame() {
    m_swapchain->Present(0, 0); // No VSync — frame rate managed by sleep limiter in main loop
}

void Overlay::TrackTargetWindow() {
    if (!IsWindow(m_targetHwnd)) return;

    // Throttle: only call SetWindowPos every TRACK_INTERVAL_MS (~100ms).
    // SetWindowPos is a heavy Win32 DWM call — doing it 500+ fps wastes CPU.
    DWORD now = GetTickCount();
    if (now - m_lastTrackTick < TRACK_INTERVAL_MS) return;
    m_lastTrackTick = now;

    // Always use client area — works for both windowed and borderless
    RECT clientRect;
    GetClientRect(m_targetHwnd, &clientRect);
    POINT clientTopLeft = {0, 0};
    ClientToScreen(m_targetHwnd, &clientTopLeft);

    int newWidth = clientRect.right;
    int newHeight = clientRect.bottom;

    // Move overlay to match game's client area
    SetWindowPos(m_overlayHwnd, HWND_TOPMOST,
                 clientTopLeft.x, clientTopLeft.y,
                 newWidth, newHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Resize if needed
    if (newWidth != m_width || newHeight != m_height) {
        ResizeBuffers(newWidth, newHeight);
    }
}

void Overlay::ResizeBuffers(int width, int height) {
    if (!m_swapchain || width <= 0 || height <= 0) return;

    m_width = width;
    m_height = height;

    if (m_rtv) {
        m_rtv->Release();
        m_rtv = nullptr;
    }

    m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backBuffer = nullptr;
    m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }
}

void Overlay::SetClickThrough(bool clickThrough) {
    if (m_clickThrough == clickThrough) return;
    m_clickThrough = clickThrough;

    LONG exStyle = GetWindowLongW(m_overlayHwnd, GWL_EXSTYLE);
    if (clickThrough) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongW(m_overlayHwnd, GWL_EXSTYLE, exStyle);
}

void Overlay::Shutdown() {
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
    if (m_swapchain) { m_swapchain->Release(); m_swapchain = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }

    if (m_overlayHwnd) {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }

    if (m_wndClass.lpszClassName) {
        UnregisterClassW(m_className, m_wndClass.hInstance);
    }
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
