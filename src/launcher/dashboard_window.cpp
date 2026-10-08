#include "dashboard_window.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <filesystem>

// ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

DashboardWindow::~DashboardWindow() {
    Shutdown();
}

bool DashboardWindow::Initialize(int width, int height) {
    if (!CreateAppWindow(width, height)) {
        spdlog::error("Failed to create dashboard window");
        return false;
    }

    if (!InitD3D11()) {
        spdlog::error("Failed to initialize D3D11 for dashboard");
        return false;
    }

    spdlog::info("Dashboard window initialized ({}x{})", m_width, m_height);
    return true;
}

bool DashboardWindow::CreateAppWindow(int width, int height) {
    m_wndClass = {};
    m_wndClass.cbSize = sizeof(WNDCLASSEXW);
    m_wndClass.style = CS_HREDRAW | CS_VREDRAW;
    m_wndClass.lpfnWndProc = WndProc;
    m_wndClass.hInstance = GetModuleHandleW(nullptr);
    m_wndClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    m_wndClass.lpszClassName = L"DADLauncherWindow";

    // Try to load the DaD icon from resources directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring iconPath = std::filesystem::path(exePath).parent_path().parent_path().parent_path().wstring()
        + L"\\resources\\dad_icon.ico";

    HICON hIcon = static_cast<HICON>(LoadImageW(
        nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED));

    if (hIcon) {
        m_wndClass.hIcon = hIcon;
        m_wndClass.hIconSm = hIcon;
        spdlog::info("Loaded DaD icon from resources");
    } else {
        // Fallback: try loading from embedded resource
        m_wndClass.hIcon = LoadIconW(m_wndClass.hInstance, L"IDI_APPICON");
        m_wndClass.hIconSm = m_wndClass.hIcon;
    }

    if (!RegisterClassExW(&m_wndClass)) {
        spdlog::error("Failed to register window class. Error: {}", GetLastError());
        return false;
    }

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - width) / 2;
    int posY = (screenH - height) / 2;

    // Adjust for title bar and borders
    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        L"DADLauncherWindow",
        L"Dark and Darker \u2014 Research Dashboard",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr,
        m_wndClass.hInstance,
        this  // Pass this pointer for WndProc
    );

    if (!m_hwnd) {
        spdlog::error("Failed to create window. Error: {}", GetLastError());
        return false;
    }

    m_width = width;
    m_height = height;

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    return true;
}

bool DashboardWindow::InitD3D11() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

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
    m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                           reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }

    spdlog::info("D3D11 initialized. Feature level: 0x{:X}", static_cast<unsigned>(featureLevel));
    return true;
}

void DashboardWindow::BeginFrame() {
    // Handle deferred resize
    if (m_resizePending) {
        HandleResize();
        m_resizePending = false;
    }

    // Clear with DaD theme darkest background (warm black)
    float clearColor[4] = {0.04f, 0.03f, 0.03f, 1.0f};
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_context->ClearRenderTargetView(m_rtv, clearColor);
}

void DashboardWindow::EndFrame() {
    m_swapchain->Present(1, 0); // VSync on
}

void DashboardWindow::HandleResize() {
    if (!m_swapchain || m_pendingWidth <= 0 || m_pendingHeight <= 0) return;

    m_width = m_pendingWidth;
    m_height = m_pendingHeight;

    if (m_rtv) {
        m_rtv->Release();
        m_rtv = nullptr;
    }

    m_swapchain->ResizeBuffers(0, m_width, m_height, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backBuffer = nullptr;
    m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                           reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }
}

void DashboardWindow::Shutdown() {
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
    if (m_swapchain) { m_swapchain->Release(); m_swapchain = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    UnregisterClassW(L"DADLauncherWindow", m_wndClass.hInstance);
}

LRESULT CALLBACK DashboardWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Forward to ImGui
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    // Get our window pointer
    DashboardWindow* self = nullptr;
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<DashboardWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DashboardWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_SIZE:
            if (self && wParam != SIZE_MINIMIZED) {
                self->m_pendingWidth = LOWORD(lParam);
                self->m_pendingHeight = HIWORD(lParam);
                self->m_resizePending = true;
            }
            return 0;

        case WM_CLOSE:
            if (self) self->m_shouldClose = true;
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
