#include "launcher/dashboard_window.h"
#include "launcher/overlay_theme.h"
#include "core/config.h"
#include "launcher/util/log_buffer.h"
#include "launcher/panels/tool_launcher_panel.h"
#include "launcher/panels/log_panel.h"
#include "launcher/panels/game_status_panel.h"
#include "launcher/panels/offset_panel.h"
#include "launcher/panels/settings_panel.h"
#include "launcher/panels/injector_panel.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Windows.h>
#include <filesystem>

// ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // =========================================================================
    // 1. Console for debug output
    // =========================================================================
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    // =========================================================================
    // 2. Shared log buffer
    // =========================================================================
    LogBuffer logBuffer;

    // =========================================================================
    // 3. Set up spdlog with console + log buffer sinks
    // =========================================================================
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto bufferSink = std::make_shared<LogBufferSink_mt>(logBuffer, "dashboard");
    auto logger = std::make_shared<spdlog::logger>("launcher",
        spdlog::sinks_init_list{consoleSink, bufferSink});
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    spdlog::info("=== DAD Research Dashboard Starting ===");

    // =========================================================================
    // 4. Determine working directory (project root)
    // =========================================================================
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto projectDir = std::filesystem::path(exePath).parent_path().parent_path().parent_path();

    // Set working directory to project root so config paths work
    std::filesystem::current_path(projectDir);
    spdlog::info("Working directory: {}", projectDir.string());

    // =========================================================================
    // 5. Load config
    // =========================================================================
    Config config("config/settings.json");
    spdlog::info("Config loaded. GWorld=0x{:X}, GNames=0x{:X}",
                 config.Get().gworldOffset, config.Get().gnamesOffset);

    // =========================================================================
    // 6. Create window + D3D11
    // =========================================================================
    DashboardWindow window;
    if (!window.Initialize(1200, 800)) {
        spdlog::error("Failed to create dashboard window");
        MessageBoxW(nullptr, L"Failed to create dashboard window.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // =========================================================================
    // 7. Initialize ImGui with docking
    // =========================================================================
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "dad_launcher_layout.ini";

    // Apply Dark and Darker theme (gold + dark medieval)
    ImGui::StyleColorsDark();  // Base dark, then override
    DaDTheme::Apply();

    // Initialize ImGui backends
    ImGui_ImplWin32_Init(window.GetHwnd());
    ImGui_ImplDX11_Init(window.GetDevice(), window.GetContext());

    // Load fonts — try Gabriola (medieval feel) for headers, Segoe UI for body
    ImFont* fontBody = nullptr;
    ImFont* fontHeader = nullptr;

    // Body font: Segoe UI (clean, readable)
    if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        fontBody = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
        spdlog::info("Loaded Segoe UI font (body)");
    }

    // Header font: Gabriola (medieval/gothic feel) or fallback to bold Segoe
    if (std::filesystem::exists("C:\\Windows\\Fonts\\gabriola.ttf")) {
        fontHeader = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\gabriola.ttf", 24.0f);
        spdlog::info("Loaded Gabriola font (header)");
    } else if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeuib.ttf")) {
        fontHeader = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 20.0f);
        spdlog::info("Loaded Segoe UI Bold font (header fallback)");
    }

    if (!fontBody) {
        io.Fonts->AddFontDefault();
    }

    spdlog::info("ImGui initialized with Dark and Darker theme");

    // =========================================================================
    // 8. Create panels
    // =========================================================================
    ToolLauncherPanel toolPanel(logBuffer, config);
    LogPanel logPanel(logBuffer);
    GameStatusPanel gamePanel(config);
    OffsetPanel offsetPanel(config, logBuffer);
    SettingsPanel settingsPanel(config, logBuffer);
    InjectorPanel injectorPanel(logBuffer);

    spdlog::info("All panels initialized");
    spdlog::info("Dashboard ready!");

    // =========================================================================
    // 9. Main loop
    // =========================================================================
    MSG msg = {};
    while (!window.ShouldClose()) {
        // Process Windows messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                goto cleanup;
            }
        }

        // Update game status periodically
        gamePanel.Update();

        // Begin frame
        window.BeginFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Create fullscreen dockspace with passthrough for our header
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                      ImGuiDockNodeFlags_PassthruCentralNode);

        // Render all panels (with themed names)
        toolPanel.Render();
        logPanel.Render();
        gamePanel.Render();
        offsetPanel.Render();
        settingsPanel.Render();
        injectorPanel.Render();

        // End frame
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        window.EndFrame();
    }

cleanup:
    // =========================================================================
    // 10. Cleanup
    // =========================================================================
    spdlog::info("Dashboard shutting down...");
    config.Save();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    window.Shutdown();
    FreeConsole();

    return 0;
}
