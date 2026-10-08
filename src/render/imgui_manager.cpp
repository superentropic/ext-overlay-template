#include "imgui_manager.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <spdlog/spdlog.h>

bool ImGuiManager::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
                               float dpiScale) {
    m_dpiScale = dpiScale;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr; // Don't save ImGui layout to disk

    // Polished dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f * dpiScale;
    style.FrameRounding = 3.0f * dpiScale;
    style.GrabRounding = 3.0f * dpiScale;
    style.PopupRounding = 4.0f * dpiScale;
    style.ScrollbarRounding = 4.0f * dpiScale;
    style.TabRounding = 3.0f * dpiScale;
    style.Alpha = 0.97f;
    style.WindowPadding = ImVec2(10 * dpiScale, 10 * dpiScale);
    style.FramePadding = ImVec2(6 * dpiScale, 3 * dpiScale);
    style.ItemSpacing = ImVec2(8 * dpiScale, 5 * dpiScale);
    style.WindowBorderSize = 1.0f;
    style.ScrollbarSize = 14.0f * dpiScale;
    style.GrabMinSize = 12.0f * dpiScale;
    style.IndentSpacing = 21.0f * dpiScale;

    // Dark, semi-transparent backgrounds with subtle accent
    style.Colors[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.06f, 0.08f, 0.92f);
    style.Colors[ImGuiCol_TitleBg]         = ImVec4(0.08f, 0.08f, 0.10f, 0.95f);
    style.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.12f, 0.12f, 0.16f, 0.95f);
    style.Colors[ImGuiCol_Border]          = ImVec4(0.20f, 0.20f, 0.25f, 0.50f);
    style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.12f, 0.12f, 0.15f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.18f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.22f, 0.28f, 0.80f);
    style.Colors[ImGuiCol_SliderGrab]      = ImVec4(0.40f, 0.55f, 0.80f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive]= ImVec4(0.50f, 0.65f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]       = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_Button]          = ImVec4(0.18f, 0.18f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_ButtonHovered]   = ImVec4(0.25f, 0.25f, 0.32f, 0.80f);
    style.Colors[ImGuiCol_ButtonActive]    = ImVec4(0.30f, 0.35f, 0.45f, 0.80f);
    style.Colors[ImGuiCol_Header]          = ImVec4(0.15f, 0.18f, 0.25f, 0.80f);
    style.Colors[ImGuiCol_HeaderHovered]   = ImVec4(0.20f, 0.25f, 0.35f, 0.80f);
    style.Colors[ImGuiCol_HeaderActive]    = ImVec4(0.25f, 0.30f, 0.40f, 0.80f);
    style.Colors[ImGuiCol_Separator]       = ImVec4(0.20f, 0.20f, 0.25f, 0.60f);

    if (!ImGui_ImplWin32_Init(hwnd)) {
        spdlog::error("ImGui Win32 init failed");
        return false;
    }

    if (!ImGui_ImplDX11_Init(device, context)) {
        spdlog::error("ImGui DX11 init failed");
        return false;
    }

    // Load Segoe UI with oversampling for crisp text at any DPI.
    // Font size scales with screen resolution: 16px at 1080p, 32px at 4K, etc.
    float fontSize = 16.0f * dpiScale;
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH = true;
    ImFont* customFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", fontSize, &fontCfg);
    if (customFont) {
        spdlog::info("Loaded Segoe UI font ({:.0f}px, dpiScale={:.2f})", fontSize, dpiScale);
    } else {
        spdlog::warn("Segoe UI font not found, using ImGui default");
        io.Fonts->AddFontDefault();
    }

    m_initialized = true;
    spdlog::info("ImGui initialized (dpiScale={:.2f})", dpiScale);
    return true;
}

void ImGuiManager::BeginFrame() {
    if (!m_initialized) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::EndFrame() {
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiManager::Shutdown() {
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}
