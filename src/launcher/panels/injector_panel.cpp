#include "injector_panel.h"
#include "launcher/util/dll_injector.h"
#include "launcher/util/driver_detector.h"
#include "launcher/overlay_theme.h"
#include "core/process.h"
#include <imgui.h>
#include <commdlg.h>
#include <filesystem>
#include <spdlog/spdlog.h>

// Game directory for proxy deployment
static const std::wstring GAME_DIR =
    L"D:\\SteamLibrary\\steamapps\\common\\Dark and Darker\\DungeonCrawler\\Binaries\\Win64";

InjectorPanel::InjectorPanel(LogBuffer& log) : m_log(log) {
    // Pre-fill Dumper-7 DLL path
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto projectDir = std::filesystem::path(exePath).parent_path().parent_path().parent_path();
    auto dllPath = projectDir / "tools" / "dumper7" / "build" / "bin" / "Release" / "Dumper-7.dll";

    std::string pathStr = dllPath.string();
    strncpy_s(m_dllPathBuf, pathStr.c_str(), sizeof(m_dllPathBuf) - 1);

    // Check if proxy is already deployed
    CheckProxyStatus();
}

void InjectorPanel::CheckProxyStatus() {
    auto proxyPath = std::filesystem::path(GAME_DIR) / "version.dll";
    m_proxyDeployed = std::filesystem::exists(proxyPath);
}

void InjectorPanel::DeployProxy() {
    try {
        // Find our built version.dll
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto buildDir = std::filesystem::path(exePath).parent_path();
        auto srcProxy = buildDir / "version.dll";

        if (!std::filesystem::exists(srcProxy)) {
            // Try alternate location
            auto projectDir = buildDir.parent_path().parent_path();
            srcProxy = projectDir / "build" / "Release" / "version.dll";
        }

        if (!std::filesystem::exists(srcProxy)) {
            m_lastStatus = "version.dll proxy not found in build output!";
            m_lastSuccess = false;
            m_log.Push(m_lastStatus, spdlog::level::err);
            return;
        }

        auto destDir = std::filesystem::path(GAME_DIR);
        auto destProxy = destDir / "version.dll";

        // Also copy Dumper-7.dll to game directory
        auto dumperSrc = std::filesystem::path(m_dllPathBuf);
        auto dumperDest = destDir / "Dumper-7.dll";

        // Deploy proxy
        std::filesystem::copy_file(srcProxy, destProxy,
            std::filesystem::copy_options::overwrite_existing);
        m_log.Push("Proxy version.dll deployed to game directory", spdlog::level::info);

        // Deploy Dumper-7.dll alongside
        if (std::filesystem::exists(dumperSrc)) {
            std::filesystem::copy_file(dumperSrc, dumperDest,
                std::filesystem::copy_options::overwrite_existing);
            m_log.Push("Dumper-7.dll copied to game directory", spdlog::level::info);
        }

        m_proxyDeployed = true;
        m_lastStatus = "Proxy deployed! Start the game — Dumper-7 will auto-load.";
        m_lastSuccess = true;
        m_log.Push(m_lastStatus, spdlog::level::info);
    }
    catch (const std::exception& e) {
        m_lastStatus = std::string("Deploy failed: ") + e.what();
        m_lastSuccess = false;
        m_log.Push(m_lastStatus, spdlog::level::err);
    }
}

void InjectorPanel::RemoveProxy() {
    try {
        auto destProxy = std::filesystem::path(GAME_DIR) / "version.dll";
        auto destDumper = std::filesystem::path(GAME_DIR) / "Dumper-7.dll";
        auto destLog = std::filesystem::path(GAME_DIR) / "proxy_log.txt";

        if (std::filesystem::exists(destProxy))
            std::filesystem::remove(destProxy);
        if (std::filesystem::exists(destDumper))
            std::filesystem::remove(destDumper);
        if (std::filesystem::exists(destLog))
            std::filesystem::remove(destLog);

        m_proxyDeployed = false;
        m_lastStatus = "Proxy removed from game directory.";
        m_lastSuccess = true;
        m_log.Push("Proxy files removed from game directory", spdlog::level::info);
    }
    catch (const std::exception& e) {
        m_lastStatus = std::string("Remove failed: ") + e.what();
        m_lastSuccess = false;
        m_log.Push(m_lastStatus, spdlog::level::err);
    }
}

void InjectorPanel::Render() {
    if (!ImGui::Begin("Spell Injector")) {
        ImGui::End();
        return;
    }

    // ── Target Process ──
    DaDTheme::GoldSeparator("Target");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##procname", m_processNameBuf, sizeof(m_processNameBuf));

    // Check if process exists
    std::wstring processName(m_processNameBuf, m_processNameBuf + strlen(m_processNameBuf));
    m_targetPid = Process::FindProcessId(processName);
    m_processFound = (m_targetPid != 0);

    if (m_processFound) {
        ImGui::TextColored(DaDTheme::Success, "  Target found (PID: %u)", m_targetPid);
    } else {
        ImGui::TextColored(DaDTheme::TextDisabled, "  Target not found");
    }

    ImGui::Spacing();

    // ── DLL Path ──
    DaDTheme::GoldSeparator("Spell Scroll (DLL)");

    ImGui::SetNextItemWidth(-60);
    ImGui::InputText("##dllpath", m_dllPathBuf, sizeof(m_dllPathBuf));

    ImGui::SameLine();
    if (ImGui::Button("...##browse")) {
        OPENFILENAMEA ofn{};
        char fileName[MAX_PATH] = {};
        strncpy_s(fileName, m_dllPathBuf, sizeof(fileName) - 1);

        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "DLL Files\0*.dll\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = sizeof(fileName);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameA(&ofn)) {
            strncpy_s(m_dllPathBuf, fileName, sizeof(m_dllPathBuf) - 1);
        }
    }

    // Check if file exists
    bool dllExists = std::filesystem::exists(m_dllPathBuf);
    if (!dllExists && strlen(m_dllPathBuf) > 0) {
        ImGui::TextColored(DaDTheme::Danger, "  Scroll not found!");
    }

    ImGui::Spacing();

    // ── Injection Method ──
    DaDTheme::GoldSeparator("Casting Method");

    ImGui::PushStyleColor(ImGuiCol_CheckMark, DaDTheme::Gold);
    if (ImGui::RadioButton("DLL Proxy (recommended)", m_injectionMethod == 0)) {
        m_injectionMethod = 0;
    }
    if (ImGui::RadioButton("Manual Map (direct syscalls)", m_injectionMethod == 1)) {
        m_injectionMethod = 1;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("LoadLibrary (legacy)", m_injectionMethod == 2)) {
        m_injectionMethod = 2;
    }
    ImGui::PopStyleColor();

    // Method descriptions
    switch (m_injectionMethod) {
        case 0:
            ImGui::TextColored(DaDTheme::TextSecondary,
                "  Deploys version.dll proxy to game directory.");
            ImGui::TextColored(DaDTheme::TextSecondary,
                "  Loads inside the process — bypasses all AC.");
            break;
        case 1:
            ImGui::TextColored(DaDTheme::TextSecondary,
                "  Direct syscalls — bypasses user-mode hooks.");
            ImGui::TextColored(DaDTheme::Warning,
                "  May fail if AC strips kernel handles.");
            break;
        case 2:
            ImGui::TextColored(DaDTheme::TextSecondary,
                "  Standard injection — blocked by anti-cheat.");
            break;
    }

    ImGui::Spacing();

    // ── Anti-cheat Warning ──
    if (DriverDetector::IsTvkDriverLoaded()) {
        if (m_injectionMethod == 0) {
            ImGui::TextColored(DaDTheme::Success,
                "  tvk.sys detected — Proxy method bypasses it entirely");
        } else if (m_injectionMethod == 1) {
            ImGui::TextColored(DaDTheme::Warning,
                "  tvk.sys is ACTIVE — Manual Map may fail (kernel-level AC)");
        } else {
            ImGui::TextColored(DaDTheme::Danger,
                "  tvk.sys is ACTIVE — LoadLibrary will be BLOCKED!");
        }
        ImGui::Spacing();
    }

    // ── Proxy Deploy Section (Method 0) ──
    if (m_injectionMethod == 0) {
        DaDTheme::GoldSeparator("Proxy Deployment");

        // Refresh status
        CheckProxyStatus();

        if (m_proxyDeployed) {
            ImGui::TextColored(DaDTheme::Success, "  Proxy is deployed in game directory");

            // Check for proxy log file
            auto logPath = std::filesystem::path(GAME_DIR) / "proxy_log.txt";
            if (std::filesystem::exists(logPath)) {
                ImGui::TextColored(DaDTheme::GoldDim, "  proxy_log.txt found (proxy has run!)");
            }

            ImGui::Spacing();

            // Remove button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.2f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.1f, 0.08f, 1.0f));
            if (ImGui::Button("Remove Proxy", ImVec2(-1, 30))) {
                RemoveProxy();
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::TextColored(DaDTheme::TextDisabled, "  Proxy not deployed");
            ImGui::Spacing();

            // Deploy button
            ImGui::PushStyleColor(ImGuiCol_Button, DaDTheme::GoldDark);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DaDTheme::Gold);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, DaDTheme::GoldDim);
            if (ImGui::Button("Deploy Proxy + Dumper-7", ImVec2(-1, 38))) {
                DeployProxy();
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::Spacing();
        ImGui::TextColored(DaDTheme::TextDisabled,
            "  After deploying, start the game normally.");
        ImGui::TextColored(DaDTheme::TextDisabled,
            "  Dumper-7 auto-loads after 10s. Check proxy_log.txt.");
    }

    // ── Direct Inject Button (Methods 1 & 2) ──
    if (m_injectionMethod > 0) {
        ImGui::Spacing();

        m_useManualMap = (m_injectionMethod == 1);
        bool canInject = m_processFound && dllExists;
        if (!canInject) ImGui::BeginDisabled();

        ImGui::PushStyleColor(ImGuiCol_Button, DaDTheme::GoldDark);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DaDTheme::Gold);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, DaDTheme::GoldDim);
        const char* btnLabel = m_useManualMap ? "Cast Spell (Manual Map)" : "Cast Spell (LoadLibrary)";
        if (ImGui::Button(btnLabel, ImVec2(-1, 38))) {
            std::wstring dllPath(m_dllPathBuf, m_dllPathBuf + strlen(m_dllPathBuf));

            std::string methodStr = m_useManualMap ? "manual map" : "LoadLibrary";
            m_log.Push("Injecting " + std::string(m_dllPathBuf) +
                       " into PID " + std::to_string(m_targetPid) +
                       " via " + methodStr, spdlog::level::info);

            DllInjector::InjectionResult result;
            if (m_useManualMap) {
                result = DllInjector::ManualMapInject(m_targetPid, dllPath);
            } else {
                result = DllInjector::Inject(m_targetPid, dllPath);
            }

            m_lastSuccess = result.success;
            m_lastStatus = result.message;

            if (result.success) {
                m_log.Push("Injection successful: " + result.message, spdlog::level::info);
            } else {
                m_log.Push("Injection failed: " + result.message, spdlog::level::err);
            }
        }

        ImGui::PopStyleColor(3);
        if (!canInject) ImGui::EndDisabled();
    }

    // ── Status ──
    if (!m_lastStatus.empty()) {
        ImGui::Spacing();
        ImVec4 statusColor = m_lastSuccess ? DaDTheme::Success : DaDTheme::Danger;
        ImGui::TextColored(statusColor, "%s", m_lastStatus.c_str());
    }

    ImGui::End();
}
