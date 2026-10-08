#include "core/process.h"
#include "core/pattern_scanner.h"
#include "core/config.h"
#include "core/runtime_paths.h"
#include "core/integrity.h"
#include "core/trigger_state.h"
#ifndef _WIN32
#include "core/protection.h"
#include <signal.h>
#endif
#include "sdk/ue5_types.h"
#include "sdk/gnames.h"
#include "sdk/gworld.h"
#include "game/entity.h"
#include "game/actor_manager.h"
#include "game/item_database.h"
#include "render/overlay.h"
#include "render/imgui_manager.h"
#include "render/drawing.h"
#include "render/world_to_screen.h"
#include "render/menu.h"
#include "shared/shared_memory.h"

#include <imgui.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <unistd.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <iostream>
#include <limits>
#include <immintrin.h>  // _mm_pause for spin-wait

// Forward declare ImGui WndProc handler
#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

static void InitLogging(const std::filesystem::path& logFile, bool consoleOnly) {
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{consoleSink};
    if (!consoleOnly) {
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.string(), true));
        } catch (const spdlog::spdlog_ex& error) {
            std::cerr << "File logging unavailable: " << error.what() << '\n';
        }
    }

    auto logger = std::make_shared<spdlog::logger>("main",
        sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);

    spdlog::info("=== DAD Research Tool Starting ===");
}

#ifdef _WIN32
static HWND FindGameWindow() {
    // Try several possible window titles
    const wchar_t* titles[] = {
        L"Dark and Darker  ",
        L"Dark and Darker",
        L"GameProcess",
    };

    for (const auto* title : titles) {
        HWND hwnd = FindWindowW(nullptr, title);
        if (hwnd) return hwnd;
    }

    // Fallback: find by process and enumerate windows
    return nullptr;
}
#else
// GLFW only reports keys delivered to its own window. The click-through
// overlay is unfocused while the game is active, so poll the X11 server.
static bool IsX11KeyDown(KeySym symbol) {
    Display* display = glfwGetX11Display();
    if (!display) return false;
    const KeyCode code = XKeysymToKeycode(display, symbol);
    if (!code) return false;
    char keys[32]{};
    XQueryKeymap(display, keys);
    return (keys[code / 8] & (1 << (code % 8))) != 0;
}

static bool IsX11ButtonDown(unsigned int mask) {
    Display* display = glfwGetX11Display();
    if (!display) return false;
    Window root = 0, child = 0;
    int rootX = 0, rootY = 0, winX = 0, winY = 0;
    unsigned int state = 0;
    XQueryPointer(display, DefaultRootWindow(display), &root, &child,
                  &rootX, &rootY, &winX, &winY, &state);
    return (state & mask) != 0;
}
#endif

// Maps the redirect activation keys offered in the menu (Windows VK codes)
// to a live key/button state, so the tool knows when to hold the target lock.
static bool IsRedirectKeyDown(int vk) {
#ifdef _WIN32
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    switch (vk) {
        case 0x01: return IsX11ButtonDown(Button1Mask);                          // LMB
        case 0x02: return IsX11ButtonDown(Button3Mask);                          // RMB
        case 0x10: return IsX11KeyDown(XK_Shift_L) || IsX11KeyDown(XK_Shift_R);  // Shift
        case 0x12: return IsX11KeyDown(XK_Alt_L) || IsX11KeyDown(XK_Alt_R);      // Alt
        case 'V':  return IsX11KeyDown(XK_v);
        default:   return false;
    }
#endif
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
#else
int main(int argc, char** argv) {
#endif
#ifndef _WIN32
    {
        Protection::Config pCfg;
        pCfg.fakeProcessName         = "kworker/u16:3";
        pCfg.fakeThreadName          = "kworker/u16:3-events";
        pCfg.enableAntiDebug         = true;
        pCfg.enableProcessStealth    = true;
        pCfg.enableMemoryHardening   = true;
        pCfg.enableAntiDump          = true;
        pCfg.enableSeccomp           = true;
        pCfg.enableIntegrityCheck    = true;
        pCfg.enableKernelModuleHide  = true;
        pCfg.enableKernelPidRegister = true;
        Protection::Init(argc, argv, pCfg);
    }
#endif
    std::filesystem::path configOverride;
    bool checkConfig = false;
    bool diagnose = false;
    std::wstring processName = L"GameProcess.exe";
#ifndef _WIN32
    bool processNameSet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: ext-research-tool [process-name] [--config PATH] "
                         "[--check-config] [--diagnose]\n"
                         "--check-config  Print resolved paths and configured offsets, then exit.\n"
                         "--diagnose      Validate memory access, world, names and camera without an overlay.\n";
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            configOverride = argv[++i];
        } else if (arg == "--check-config") {
            checkConfig = true;
        } else if (arg == "--diagnose") {
            diagnose = true;
        } else if (!arg.empty() && arg.front() != '-' && !processNameSet) {
            processName.assign(arg.begin(), arg.end());
            processNameSet = true;
        } else {
            std::cerr << "Invalid or incomplete argument: " << arg << '\n';
            return 2;
        }
    }
#endif
    // Allow config path via environment so --config arg (and its revealing path)
    // is never needed in /proc/[pid]/cmdline.
    if (configOverride.empty()) {
        const char* env_cfg = getenv("EXT_CONFIG");
        if (env_cfg && *env_cfg) configOverride = env_cfg;
    }

    RuntimePaths paths;
    try {
        paths = ResolveRuntimePaths(GetExecutablePath(), configOverride);
    } catch (const std::exception& error) {
        std::cerr << "Cannot resolve application paths: " << error.what() << '\n';
        return 1;
    }

#ifdef _WIN32
    // DPI awareness — render at native resolution instead of Windows bitmap upscaling.
    // Without this, the overlay looks blurry ("144p") on high-DPI or scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Set Windows timer resolution to 1ms (default is 15.6ms!)
    timeBeginPeriod(1);

    // Allocate console for logging
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
#endif

    InitLogging(paths.log, checkConfig || diagnose);
    spdlog::info("Settings path: {}", paths.settings.string());
    spdlog::info("Item database path: {}", paths.items.string());
    if (!checkConfig && !diagnose)
        spdlog::info("Log path: {}", paths.log.string());

    // =========================================================================
    // Integrity check — verify critical source files haven't been tampered with
    // =========================================================================
#ifdef _WIN32
    spdlog::info("Build: {}", Integrity::GetBuildFingerprint());
    auto integrity = Integrity::Verify();
    for (const auto& w : integrity.warnings) spdlog::warn("[Integrity] {}", w);
    for (const auto& f : integrity.failures) spdlog::error("[Integrity] {}", f);
    if (!integrity.passed) {
        spdlog::error("INTEGRITY CHECK FAILED — critical source files have been modified!");
        spdlog::error("This build may contain unauthorized modifications (cheats, exploits).");
        spdlog::error("Use the original source from the official repository.");
        MessageBoxW(nullptr,
            L"Integrity check failed!\n\n"
            L"Critical source files have been modified.\n"
            L"This build may contain unauthorized modifications.\n\n"
            L"Use the original source from the official repository.",
            L"DAD Research Tool - Tamper Detected", MB_OK | MB_ICONERROR);
        timeEndPeriod(1);
        FreeConsole();
        return 1;
    }
#endif

    // Load config
    Config config(paths.settings.string());
    if (!config.IsLoaded()) {
        spdlog::error("Startup stopped: a readable, valid settings file is required. "
                      "Restore config/settings.json or supply --config PATH.");
        return 1;
    }
    spdlog::info("Configured offsets: GWorld=0x{:X}, GNames=0x{:X}, GObjects=0x{:X}",
                 config.Get().gworldOffset, config.Get().gnamesOffset, config.Get().gobjectsOffset);
    if (checkConfig) return 0;

    if (config.Get().externalMode) {
        spdlog::info("[External] Standalone mode: DLL/proxy connection and silent aim are disabled.");
    } else if (config.Get().projectileRedirect.enabled) {
        spdlog::warn("[SilentAim] Requested in settings but unavailable: "
                     "the external backend has no verified attack-time integration. "
                     "Camera-rotation writes are disabled.");
    }

    // Load item database
    ItemDatabase itemDb;
    itemDb.LoadFromFile(paths.items.string());

    // =========================================================================
    // Phase 1: Attach to game process
    // =========================================================================
    Process game;
    spdlog::info("Waiting for {}...", std::string(processName.begin(), processName.end()));

    while (!game.Attach(processName)) {
        if (diagnose) return 1;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    spdlog::info("Attached to {} (PID: {})", std::string(processName.begin(), processName.end()), game.GetPid());
    if (!config.Get().externalMode && config.Get().projectileRedirect.customBaseAddress != 0) {
        spdlog::info("Configuring manual proxy base from settings: 0x{:X}",
                     config.Get().projectileRedirect.customBaseAddress);
        game.SetManualProxyBase(config.Get().projectileRedirect.customBaseAddress);
    }
#ifndef _WIN32
    spdlog::info("Memory backend: kernel IOCTL ABI {} (effective UID={})",
                 EXT_MEM_ABI_VERSION, geteuid());
#endif

    // Try multiple possible module names (anti-cheat may report different names)
    uintptr_t gameBase = game.GetModuleBase(L"GameProcess.exe");
    if (!gameBase) gameBase = game.GetModuleBase(L"GameProcess-Win64-Shipping.exe");
    if (!gameBase) {
        // Last resort: use whatever the first/main module is
        // The PEB fallback in Process may have stored it under the process name
        spdlog::warn("Could not find game module by name, listing available modules...");
        for (const auto& [name, info] : game.GetModules()) {
            spdlog::info("  Module: {} at 0x{:X} (size 0x{:X})",
                         std::string(name.begin(), name.end()), info.base, info.size);
            // Use the largest module as game base (main exe is typically the biggest)
            if (info.size > game.GetModuleSize(L"GameProcess.exe")) {
                gameBase = info.base;
            }
        }
    }
    spdlog::info("Game base address: 0x{:X}", gameBase);
    uint16_t imageSignature = 0;
    if (!gameBase || !game.ReadRaw(gameBase, &imageSignature, sizeof(imageSignature)) ||
        imageSignature != 0x5A4D) {
        spdlog::error("Startup stopped: cannot read a valid PE image header at 0x{:X} "
                      "for PID {}. Check the selected process and memory backend.", gameBase, game.GetPid());
        return 1;
    }
    spdlog::info("Memory probe passed: readable game PE header (MZ)");
    if (game.IsUsingRelay()) {
        spdlog::info("*** Using shared memory relay (version.dll proxy) for memory reads ***");
    }

    // =========================================================================
    // Phase 2: Validate configured offsets; scan only if validation fails
    // =========================================================================
    PatternScanner scanner;

    // Generic signatures are fallback candidates, never proof of a valid address.
    std::string gworldPattern = config.Get().gworldPattern.empty()
        ? "48 8B 05 ? ? ? ? 48 3B C3 48 0F 44 C6"  // Common UE5 GWorld pattern
        : config.Get().gworldPattern;

    std::string gnamesPattern = config.Get().gnamesPattern.empty()
        ? "48 8D 05 ? ? ? ? EB 16"  // Common UE5 GNames pattern
        : config.Get().gnamesPattern;

    // Determine which module name to use for pattern scanning
    std::wstring gameModuleName = L"GameProcess.exe";
    if (!game.GetModuleBase(gameModuleName))
        gameModuleName = L"GameProcess-Win64-Shipping.exe";

    GWorldReader world;
    GNamesReader names;
    uintptr_t gworldAddr = 0;
    uintptr_t gnamesAddr = 0;
    const auto configuredAddress = [gameBase](uintptr_t offset) -> uintptr_t {
        return offset && offset <= (std::numeric_limits<uintptr_t>::max)() - gameBase
            ? gameBase + offset : 0;
    };

    if (const uintptr_t candidate = configuredAddress(config.Get().gworldOffset)) {
        spdlog::info("Checking GWorld from config: base + 0x{:X} = 0x{:X}",
                     config.Get().gworldOffset, candidate);
        if (world.Initialize(game, candidate)) gworldAddr = candidate;
    }
    if (!gworldAddr) {
        spdlog::info("Scanning for a GWorld fallback...");
        if (const auto result = scanner.FindPattern(game, gameModuleName, gworldPattern)) {
            const uintptr_t candidate = PatternScanner::ResolveRelative(game, *result, 3, 7);
            if (world.Initialize(game, candidate)) gworldAddr = candidate;
        }
    }
    if (const uintptr_t candidate = configuredAddress(config.Get().gnamesOffset)) {
        spdlog::info("Checking GNames from config: base + 0x{:X} = 0x{:X}",
                     config.Get().gnamesOffset, candidate);
        if (names.Initialize(game, candidate)) gnamesAddr = candidate;
    }
    if (!gnamesAddr) {
        spdlog::info("Scanning for a GNames fallback...");
        if (const auto result = scanner.FindPattern(game, gameModuleName, gnamesPattern)) {
            const uintptr_t candidate = PatternScanner::ResolveRelative(game, *result, 3, 7);
            if (names.Initialize(game, candidate)) gnamesAddr = candidate;
        }
    }

    if (!gworldAddr || !gnamesAddr) {
        spdlog::error("Startup stopped: GWorld/GNames validation failed. Check game readiness "
                      "and the offsets in {}. No actor scan will be started.", paths.settings.string());
#ifdef _WIN32
        MessageBoxW(nullptr,
            L"Failed to find GWorld/GNames addresses.\n\n"
            L"Run GSpots to get correct offsets,\n"
            L"then update config/settings.json.",
            L"DAD Research Tool", MB_OK | MB_ICONWARNING);
        timeEndPeriod(1);
        FreeConsole();
#endif
        return 1;
    }

    spdlog::info("GWorld resolved: 0x{:X}", gworldAddr);
    spdlog::info("GNames resolved: 0x{:X}", gnamesAddr);

    // =========================================================================
    // Phase 3: Initialize UE5 readers
    // =========================================================================
    if (diagnose) {
        const auto camera = world.GetCameraInfo(game);
        const auto actorPointers = world.GetAllActors(game);
        const uintptr_t controller = world.GetLocalPlayerController(game);
        const bool cameraValid = std::isfinite(camera.FOV) && camera.FOV > 0.0f && camera.FOV <= 180.0f;
        spdlog::info("Diagnostics: UWorld=0x{:X}, controller=0x{:X}, camera FOV={}, actors={}",
                     world.GetUWorld(game), controller, camera.FOV, actorPointers.size());
        if (!controller || !cameraValid) {
            spdlog::error("Diagnostics failed: local camera/controller is not ready or offsets are invalid");
            return 1;
        }
        spdlog::info("Diagnostics passed");
        return 0;
    }

    ActorManager actors;
    WorldToScreen w2s;

    // =========================================================================
    // Phase 4: Create overlay
    // =========================================================================
#ifdef _WIN32
    HWND gameWindow = FindGameWindow();
    if (!gameWindow) {
        spdlog::error("Game window not found. Make sure the game is running and visible.");
        MessageBoxW(nullptr, L"Game window not found.", L"DAD Research Tool", MB_OK | MB_ICONERROR);
        return 1;
    }
    spdlog::info("Found game window: 0x{:X}", reinterpret_cast<uintptr_t>(gameWindow));
#else
    void* gameWindow = nullptr;
    spdlog::info("Using Linux fullscreen overlay for the Proton game");
#endif

    Overlay overlay;
    if (!overlay.Initialize(gameWindow)) {
        spdlog::error("Failed to initialize overlay");
        return 1;
    }

    // Compute DPI scale: reference resolution is 1080p (1920x1080).
    // 4K (3840x2160) → scale 2.0, 1440p → 1.33, 1080p → 1.0.
    // Also factor in the user's font size preference (default 15 → 1.0x).
    float resScale = overlay.GetHeight() / 1080.0f;
    float fontScale = config.Get().visuals.fontSize / 15.0f;
    float dpiScale = resScale * fontScale;
    spdlog::info("DPI scaling: resolution={}x{} -> resScale={:.2f}, fontSize={:.0f} -> fontScale={:.2f}, combined={:.2f}",
        overlay.GetWidth(), overlay.GetHeight(), resScale, config.Get().visuals.fontSize, fontScale, dpiScale);

    // Apply DPI scale to all drawing functions
    Drawing::SetDpiScale(dpiScale);

    ImGuiManager imgui;
    if (!imgui.Initialize(overlay.GetOverlayHandle(), overlay.GetDevice(), overlay.GetContext(), dpiScale)) {
        spdlog::error("Failed to initialize ImGui");
        return 1;
    }

    w2s.SetScreenSize(overlay.GetWidth(), overlay.GetHeight());

    Menu menu(config);
    menu.SetProcess(&game);
    menu.SetGWorldAddr(gworldAddr);
    menu.SetGNamesAddr(gnamesAddr);
    menu.SetAttached(true);

    // =========================================================================
    // Phase 5: Main loop
    // =========================================================================
    spdlog::info("Entering main loop");
    spdlog::info("NOTE: Is the game in Borderless Windowed mode? Overlay REQUIRES borderless/windowed, NOT exclusive fullscreen.");

    int frameCount = 0;
    auto fpsTimer = std::chrono::steady_clock::now();

    // Thread-safe entity sharing between scan thread and render thread
    std::mutex entityMutex;
    std::vector<GameEntity> sharedEntities;       // Written by scan thread
    std::vector<GameEntity> renderEntities;        // Copy used by render thread
    std::vector<const GameEntity*> filtered;       // Filtered pointers into renderEntities
    uintptr_t lockedTargetAddr = 0;                // Redirect target held while the trigger is active
    std::atomic<bool> running{true};
#ifndef _WIN32
    /* SIGINT/SIGTERM flip running so the main loop exits cleanly,
     * reaching the cleanup block that calls Protection::Shutdown(). */
    static std::atomic<bool>* s_running_ptr = &running;
    signal(SIGINT,  [](int){ s_running_ptr->store(false); });
    signal(SIGTERM, [](int){ s_running_ptr->store(false); });
#endif
    std::atomic<bool> newDataAvailable{false};
    std::atomic<int> scanEntityCount{0};
    std::atomic<bool> externalActive{config.Get().externalMode};
    bool prevExternalMode = config.Get().externalMode;

    // =========================================================================
    // Background actor scan thread — does the heavy work without blocking render
    // =========================================================================
    std::thread scanThread([&]() {
        spdlog::info("[ScanThread] Started");
        while (running.load()) {
            if (!game.IsValid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            auto scanStart = std::chrono::steady_clock::now();

            // Try to connect to proxy SHM (expensive scan, safe on background thread)
            if (!externalActive.load()) game.TryConnectProxy();

            // Heavy actor scan
            uintptr_t localPawn = world.GetLocalPawn(game);
            actors.Update(game, world, names, localPawn,
                gameBase + config.Get().gobjectsOffset,
                config.Get().filter.minLootRarity);

            // Copy entities under lock
            {
                std::lock_guard<std::mutex> lock(entityMutex);
                sharedEntities = actors.GetEntities(); // deep copy
                scanEntityCount.store(static_cast<int>(sharedEntities.size()));
                newDataAvailable.store(true);
            }

            auto scanEnd = std::chrono::steady_clock::now();
            float scanMs = std::chrono::duration<float, std::milli>(scanEnd - scanStart).count();

            static int diagCount = 0;
            if (diagCount++ % 60 == 0) {  // Every ~30s instead of 5s
                spdlog::info("[ScanThread] Scan took {:.0f}ms, {} entities", scanMs, actors.GetEntities().size());
            }

            // Wait before next scan (500ms minus scan time, minimum 50ms)
            float waitMs = (std::max)(50.0f, 500.0f - scanMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(waitMs)));
        }
        spdlog::info("[ScanThread] Stopped");
    });

    // Frame rate limiter: cap overlay to target FPS to avoid starving the game GPU.
    // A transparent overlay doesn't need 1000+ fps — 144 is buttery smooth.
    const float targetFrameTime = 1.0f / static_cast<float>(config.Get().updateRateFps);
    auto lastFrameTime = std::chrono::steady_clock::now();

    bool quit = false;
#ifdef _WIN32
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
#else
    bool insertWasDown = false;
    bool endWasDown = false;
    while (!quit) {
        if (glfwWindowShouldClose(overlay.GetWindow()))
            break;
        glfwPollEvents();
#endif

        // Frame rate limiter — sleep until next frame is due
        {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - lastFrameTime).count();
            if (elapsed < targetFrameTime) {
                // Sleep for most of the remaining time (leave 1ms margin for accuracy)
                float sleepMs = (targetFrameTime - elapsed) * 1000.0f - 1.0f;
                if (sleepMs > 1.0f) {
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        static_cast<int64_t>(sleepMs * 1000.0f)));
                }
                // Spin-wait for the last fraction of a ms (precise timing)
                while (std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - lastFrameTime).count() < targetFrameTime) {
                    _mm_pause(); // CPU hint: we're spin-waiting
                }
            }
            lastFrameTime = std::chrono::steady_clock::now();
        }

        // Check if game is still running
        if (!game.IsValid()) {
            spdlog::warn("Game process terminated");
            break;
        }

        // Toggle menu with INSERT key
#ifdef _WIN32
        if (GetAsyncKeyState(config.Get().toggleMenuKey) & 1) {
#else
        const bool insertDown = IsX11KeyDown(XK_Insert);
        if (insertDown && !insertWasDown) {
#endif
            menu.Toggle();
            overlay.SetClickThrough(!menu.IsVisible());
        }
#ifndef _WIN32
        insertWasDown = insertDown;
#endif

        // Exit with END key
#ifdef _WIN32
        if (GetAsyncKeyState(VK_END) & 1) {
#else
        const bool endDown = IsX11KeyDown(XK_End);
        if (endDown && !endWasDown) {
#endif
            spdlog::info("Exit key pressed");
            break;
        }
#ifndef _WIN32
        endWasDown = endDown;
#endif

        // FPS counter
        frameCount++;
        auto fpsNow = std::chrono::steady_clock::now();
        float fpsElapsed = std::chrono::duration<float>(fpsNow - fpsTimer).count();
        if (fpsElapsed >= 1.0f) {
            menu.SetFps(frameCount / fpsElapsed);
            frameCount = 0;
            fpsTimer = fpsNow;
        }

        // Update screen size
        w2s.SetScreenSize(overlay.GetWidth(), overlay.GetHeight());

        // External mode runs standalone: no proxy scan, SHM reads or target sync.
        const bool externalMode = config.Get().externalMode;
        externalActive.store(externalMode);
        if (externalMode != prevExternalMode) {
            prevExternalMode = externalMode;
            lockedTargetAddr = 0;
            if (externalMode) {
                // The DLL keeps its last target until told otherwise, so switch it off once.
                if (SharedGameData* last = game.GetSharedData()) {
                    SilentHelperIPC off = last->silentHelper;
                    off.enabled = 0;
                    off.hasTarget = 0;
                    game.SyncProxyTarget(off);
                }
            } else if (config.Get().projectileRedirect.customBaseAddress != 0) {
                game.SetManualProxyBase(config.Get().projectileRedirect.customBaseAddress);
            }
        }

        SharedGameData* shm = nullptr;
        Menu::ProxyTelemetry proxyTel = {};
        if (!externalMode) {
            // Update shared memory proxy sync on every frame
            game.UpdateProxySync();
            shm = game.GetSharedData();

            proxyTel.isConnected = game.IsProxyConnected() && (shm != nullptr && shm->magic == EXT_SHM_MAGIC);
            proxyTel.moduleFound = game.HasProxyModule(&proxyTel.moduleBase, &proxyTel.moduleSize);
            proxyTel.shmAddr = game.GetRemoteShmAddress();
            proxyTel.heartbeat = shm ? shm->heartbeat : 0;
            proxyTel.hooksInstalled = shm ? (shm->hooksInstalled != 0) : false;
            if (shm) {
                proxyTel.hookCallCountController = shm->silentHelper.hookCallCountController;
                proxyTel.hookCallCountLocalPlayer = shm->silentHelper.hookCallCountLocalPlayer;
                proxyTel.redirectCount = shm->silentHelper.redirectCount;
                proxyTel.viewPointRestoreCount = shm->silentHelper.viewPointRestoreCount;
                proxyTel.lastHookStatus = shm->silentHelper.lastHookStatus;
                proxyTel.playerControllerAddr = static_cast<uintptr_t>(shm->silentHelper.playerControllerAddr);
                proxyTel.localPlayerAddr = static_cast<uintptr_t>(shm->silentHelper.localPlayerAddr);
                proxyTel.isRedirecting = (shm->silentHelper.isRedirecting != 0);
                proxyTel.redirectPitch = shm->silentHelper.lastRedirectPitch;
                proxyTel.redirectYaw = shm->silentHelper.lastRedirectYaw;
            }
            proxyTel.manualBase = game.GetManualProxyBase();
            proxyTel.manualBaseFailed = game.ManualProxyBaseFailed();
        }

        // =====================================================================
        // Update game data — render thread only does cheap work
        // =====================================================================
        if (gworldAddr && gnamesAddr) {
            // Pick up new entity data from scan thread (non-blocking)
            if (newDataAvailable.load()) {
                std::lock_guard<std::mutex> lock(entityMutex);
                renderEntities = std::move(sharedEntities);
                newDataAvailable.store(false);

                // Rebuild filtered list from new render entities
                FVector localPos = w2s.GetCameraPosition();
                filtered.clear();
                for (const auto& entity : renderEntities) {
                    if (entity.isLocalPlayer) continue;
                    // Skip ghost players if the toggle is off
                    if (entity.isGhost && !config.Get().filter.showGhostPlayers) continue;
                    // Skip corpses if the toggle is off
                    if (!config.Get().filter.showCorpses && !entity.isAlive) continue;
                    
                    switch (entity.type) {
                        case EntityType::PLAYER: if (!config.Get().filter.showPlayers) continue; break;
                        case EntityType::MONSTER:
                            if (!config.Get().filter.showNPCs) continue;
                            if (config.Get().filter.minMonsterGrade > 0 &&
                                static_cast<int>(entity.monsterGrade) < config.Get().filter.minMonsterGrade) continue;
                            break;
                        case EntityType::CHEST_NORMAL: case EntityType::CHEST_SPECIAL: break;
                        case EntityType::LOOT_ITEM: if (!config.Get().filter.showLoot) continue; break;
                        case EntityType::PORTAL: if (!config.Get().filter.showPortals) continue; break;
                        case EntityType::INTERACTABLE: break;
                        default: continue;
                    }
                    float dist = static_cast<float>(entity.position.DistanceToMeters(localPos));
                    // Players get their own (much larger) distance filter
                    float maxDist = (entity.type == EntityType::PLAYER)
                        ? config.Get().filter.maxPlayerDistance
                        : config.Get().filter.maxDistance;
                    if (dist > maxDist) continue;
                    if (entity.type == EntityType::LOOT_ITEM &&
                        entity.rarity < config.Get().filter.minLootRarity) continue;
                    filtered.push_back(&entity);
                }
                menu.SetEntityCount(static_cast<int>(filtered.size()));
            }

            // =================================================================
            // STEP 1: Read all world positions + bones (no projection yet)
            // We read all positions FIRST, then camera LAST, then project.
            // This minimizes time between camera read and drawing,
            // which eliminates the "blinking when turning" artifact.
            //
            // Position extrapolation: we compute velocity from consecutive
            // reads and extrapolate forward to compensate for read latency.
            // =================================================================
            for (const auto* entityPtr : filtered) {
                auto& entity = const_cast<GameEntity&>(*entityPtr);

                // Cache rootComp pointer (doesn't change between frames)
                if (!entity.cachedRootComp) {
                    entity.cachedRootComp = game.Read<uintptr_t>(entity.address + UEOffsets::AActor_RootComponent);
                }
                if (!entity.cachedRootComp) continue;

                // Read world position from ComponentToWorld
                FVector rawPos = game.Read<FVector>(entity.cachedRootComp + UEOffsets::SceneComp_ComponentToWorld + UEOffsets::FTransform_Translation);
                if (rawPos.X == 0.0 && rawPos.Y == 0.0 && rawPos.Z == 0.0)
                    rawPos = game.Read<FVector>(entity.cachedRootComp + UEOffsets::SceneComp_RelativeLocation);
                if (std::abs(rawPos.Z) >= 100000.0)
                    continue; // Skip invalid position

                entity.position = rawPos;
                entity.hasBoneData = false;

                // For characters: read health + bone positions from SkeletalMeshComponent
                if (entity.type == EntityType::PLAYER || entity.type == EntityType::MONSTER) {
                    // Fast health read: use cached AttrSet pointer (1 read, no chain traversal)
                    if (entity.cachedAttrSet) {
                        float newHealth = game.Read<float>(
                            entity.cachedAttrSet + UEOffsets::Attr_Health + UEOffsets::AttrData_CurrentValue);
                        if (newHealth >= 0.0f && newHealth < 10000.0f) {
                            entity.health = newHealth;
                            // Death is STICKY: once dead, only revive if health is substantial
                            // (> 20% max). This prevents flickering from stale/garbage reads.
                            // The scan thread is the authority for alive→dead transitions.
                            if (entity.isAlive) {
                                // Currently alive: can transition to dead
                                entity.isAlive = newHealth >= 0.5f;
                            } else {
                                // Currently dead: only revive if clearly alive (real revive/heal)
                                float reviveThreshold = entity.maxHealth > 0 ? entity.maxHealth * 0.2f : 10.0f;
                                entity.isAlive = newHealth >= reviveThreshold;
                            }
                        }
                    }
                    // Cache mesh component pointer (doesn't change between frames)
                    if (!entity.cachedMeshComp) {
                        entity.cachedMeshComp = game.Read<uintptr_t>(entity.address + UEOffsets::ACharacter_Mesh);
                    }

                    // Raw bone positions (before extrapolation)
                    FVector rawBoneHead{}, rawBoneNeck{}, rawBoneChest{}, rawBoneFeet{};

                    if (entity.cachedMeshComp) {
                        // Read full mesh component ComponentToWorld transform (Rotation, Translation, Scale)
                        FTransform componentToWorld = game.Read<FTransform>(
                            entity.cachedMeshComp + UEOffsets::SceneComp_ComponentToWorld);

                        // Read bone array header
                        TArray<FTransform> boneArray = game.Read<TArray<FTransform>>(
                            entity.cachedMeshComp + UEOffsets::SkelMesh_CachedComponentSpaceTransforms);

                        if (boneArray.Data && boneArray.Count > 2 && boneArray.Count < 300) {
                            // Root bone (index 0) = feet
                            FVector rootBoneLocal = game.Read<FVector>(
                                boneArray.Data + UEOffsets::FTransform_Translation);

                            FVector bestHeadLocal = rootBoneLocal;
                            FVector bestNeckLocal = rootBoneLocal;
                            FVector bestChestLocal = rootBoneLocal;

                            if (entity.cachedHeadBoneIdx >= 0 && entity.cachedHeadBoneIdx < boneArray.Count) {
                                // Fast path: reuse cached bone index
                                bestHeadLocal = game.Read<FVector>(
                                    boneArray.Data + entity.cachedHeadBoneIdx * UEOffsets::FTransform_Size
                                    + UEOffsets::FTransform_Translation);
                                if (entity.cachedNeckBoneIdx >= 0 && entity.cachedNeckBoneIdx < boneArray.Count) {
                                    bestNeckLocal = game.Read<FVector>(
                                        boneArray.Data + entity.cachedNeckBoneIdx * UEOffsets::FTransform_Size
                                        + UEOffsets::FTransform_Translation);
                                }
                                if (entity.cachedChestBoneIdx >= 0 && entity.cachedChestBoneIdx < boneArray.Count) {
                                    bestChestLocal = game.Read<FVector>(
                                        boneArray.Data + entity.cachedChestBoneIdx * UEOffsets::FTransform_Size
                                        + UEOffsets::FTransform_Translation);
                                }
                            } else {
                                // First time: scan candidate bones and cache head, neck, and chest
                                if (boneArray.Count >= 68) {
                                    entity.cachedHeadBoneIdx = BoneIndices::Humanoid_Head;  // 67
                                    entity.cachedNeckBoneIdx = BoneIndices::Humanoid_Neck;  // 66
                                    entity.cachedChestBoneIdx = BoneIndices::Humanoid_Chest; // 4
                                } else {
                                    double bestHeadZ = rootBoneLocal.Z;
                                    int bestIdx = 0;

                                    int candidates[] = {
                                        (std::min)(67, boneArray.Count - 1),
                                        (std::min)(6, boneArray.Count - 1),
                                        (std::min)(10, boneArray.Count - 1),
                                        (std::min)(20, boneArray.Count - 1),
                                        boneArray.Count / 2,
                                        boneArray.Count * 3 / 4,
                                        boneArray.Count - 1,
                                    };

                                    for (int idx : candidates) {
                                        if (idx <= 0 || idx >= boneArray.Count) continue;
                                        FVector boneLocal = game.Read<FVector>(
                                            boneArray.Data + idx * UEOffsets::FTransform_Size
                                            + UEOffsets::FTransform_Translation);
                                        if (boneLocal.Z > bestHeadZ) {
                                            bestHeadZ = boneLocal.Z;
                                            bestHeadLocal = boneLocal;
                                            bestIdx = idx;
                                        }
                                    }
                                    entity.cachedHeadBoneIdx = bestIdx;
                                    entity.cachedNeckBoneIdx = (std::max)(0, bestIdx - 1);
                                    entity.cachedChestBoneIdx = (std::max)(0, bestIdx / 2);
                                }

                                bestHeadLocal = game.Read<FVector>(
                                    boneArray.Data + entity.cachedHeadBoneIdx * UEOffsets::FTransform_Size
                                    + UEOffsets::FTransform_Translation);
                                bestNeckLocal = game.Read<FVector>(
                                    boneArray.Data + entity.cachedNeckBoneIdx * UEOffsets::FTransform_Size
                                    + UEOffsets::FTransform_Translation);
                                bestChestLocal = game.Read<FVector>(
                                    boneArray.Data + entity.cachedChestBoneIdx * UEOffsets::FTransform_Size
                                    + UEOffsets::FTransform_Translation);
                            }

                            // Component-space → world-space via full matrix/quaternion transformation
                            rawBoneFeet = componentToWorld.TransformPosition(rootBoneLocal);
                            rawBoneHead = componentToWorld.TransformPosition(bestHeadLocal);
                            rawBoneNeck = componentToWorld.TransformPosition(bestNeckLocal);
                            rawBoneChest = componentToWorld.TransformPosition(bestChestLocal);

                            // Validate: head must be above feet, reasonable height, valid position
                            double boneHeight = rawBoneHead.Z - rawBoneFeet.Z;
                            if (boneHeight > 10.0 &&  // At least 10 UU (~10cm) tall
                                boneHeight < 500.0 &&
                                std::abs(rawBoneFeet.Z) < 100000.0) {
                                entity.hasBoneData = true;
                                entity.position = rawBoneFeet;
                                entity.headPosition = rawBoneHead;
                                entity.boneHead = rawBoneHead;
                                entity.boneNeck = rawBoneNeck;
                                entity.boneChest = rawBoneChest;
                                entity.boneFeet = rawBoneFeet;
                            }
                        }
                    }

                    if (!entity.hasBoneData) {
                        entity.headPosition = entity.position;
                        entity.headPosition.Z += 170.0;
                    }

                    // ---------------------------------------------------------
                    // Position extrapolation: predict where the entity IS NOW
                    // based on where it WAS (prevPosition = last frame's raw read)
                    // and where it IS (this frame's raw read).
                    //
                    // The key insight: by the time we finish reading positions,
                    // building the projection matrix, and drawing, the entity
                    // has moved ~1 more frame ahead. So we extrapolate by 1.0x
                    // the inter-frame velocity to cancel out the render pipeline
                    // latency and make boxes feel "locked" to the model.
                    //
                    // prevPosition stores the RAW read (not extrapolated), so
                    // velocity = raw_this_frame - raw_last_frame = true movement.
                    // ---------------------------------------------------------
                    if (entity.hasPrevPosition) {
                        // Use raw position for velocity (not extrapolated)
                        FVector rawThisFrame = entity.hasBoneData ? rawBoneFeet : rawPos;
                        double vx = rawThisFrame.X - entity.prevPosition.X;
                        double vy = rawThisFrame.Y - entity.prevPosition.Y;
                        double vz = rawThisFrame.Z - entity.prevPosition.Z;

                        // Only extrapolate if movement is reasonable
                        // (> 0.1 UU to avoid jitter on stationary, < 500 UU to avoid teleport)
                        double speedSq = vx * vx + vy * vy + vz * vz;
                        if (speedSq > 0.01 && speedSq < 250000.0) {
                            // 1.0x = predict 1 full frame ahead (cancels render pipeline lag)
                            constexpr double EXTRAP_FACTOR = 1.0;
                            entity.position.X += vx * EXTRAP_FACTOR;
                            entity.position.Y += vy * EXTRAP_FACTOR;
                            entity.position.Z += vz * EXTRAP_FACTOR;

                            if (entity.hasBoneData) {
                                // Bone velocity (head/feet may differ from root during animation)
                                double hvx = rawBoneHead.X - entity.prevBoneHead.X;
                                double hvy = rawBoneHead.Y - entity.prevBoneHead.Y;
                                double hvz = rawBoneHead.Z - entity.prevBoneHead.Z;
                                entity.boneHead.X += hvx * EXTRAP_FACTOR;
                                entity.boneHead.Y += hvy * EXTRAP_FACTOR;
                                entity.boneHead.Z += hvz * EXTRAP_FACTOR;
                                entity.boneFeet.X += vx * EXTRAP_FACTOR;
                                entity.boneFeet.Y += vy * EXTRAP_FACTOR;
                                entity.boneFeet.Z += vz * EXTRAP_FACTOR;
                                entity.headPosition = entity.boneHead;
                            } else {
                                entity.headPosition.X += vx * EXTRAP_FACTOR;
                                entity.headPosition.Y += vy * EXTRAP_FACTOR;
                                entity.headPosition.Z += vz * EXTRAP_FACTOR;
                            }
                        }
                    }

                    // Store RAW read (not extrapolated) for next frame's velocity calc
                    entity.prevPosition = entity.hasBoneData ? rawBoneFeet : rawPos;
                    entity.prevBoneHead = rawBoneHead;
                    entity.prevBoneFeet = rawBoneFeet;
                    entity.hasPrevPosition = true;
                }
            }

            // =================================================================
            // STEP 2: Read camera AS LATE AS POSSIBLE — right before projection
            // Uses cached CameraManager pointer (1 read instead of 7)
            // =================================================================
            w2s.UpdateCameraFast(game, world);
            FVector localPos = w2s.GetCameraPosition();

            // =================================================================
            // STEP 3: Project all positions to screen (pure CPU math, instant)
            // Camera and positions are now maximally synchronized
            // =================================================================
            for (const auto* entity : filtered) {
                auto& mutableEntity = const_cast<GameEntity&>(*entity);
                mutableEntity.isOnScreen = w2s.Project(entity->position, mutableEntity.screenPos);
                mutableEntity.distanceMeters = static_cast<float>(entity->position.DistanceToMeters(localPos));

                if (entity->hasBoneData) {
                    mutableEntity.headOnScreen = w2s.Project(entity->boneHead, mutableEntity.screenHead);
                    w2s.Project(entity->boneNeck, mutableEntity.screenNeck);
                    w2s.Project(entity->boneChest, mutableEntity.screenChest);
                    mutableEntity.feetOnScreen = w2s.Project(entity->boneFeet, mutableEntity.screenFeet);
                }
            }

            // =================================================================
            // STEP 4: Resolve best target for SilentHelper & Sync Proxy
            // =================================================================
            if (shm && shm->magic == EXT_SHM_MAGIC && shm->version >= 2) {
                const auto& pr = config.Get().projectileRedirect;
                SilentHelperIPC ipc = shm->silentHelper;
                ipc.enabled = pr.enabled ? 1 : 0;
                ipc.triggerKey = static_cast<uint32_t>(pr.key);
                ipc.maxFovDegrees = static_cast<double>(pr.fov);
                ipc.maxDistanceMeters = static_cast<double>(pr.maxTargetDistance);
                ipc.targetBone = static_cast<uint32_t>(pr.bone);
                ipc.playerControllerAddr = static_cast<uint64_t>(world.GetLocalPlayerController(game));
                ipc.localPlayerAddr = static_cast<uint64_t>(world.GetLocalPlayer(game));
                ipc.requireKey = pr.requireKey ? 1 : 0;
                ipc.silentCamera = pr.silentCamera ? 1 : 0;
                ipc.targetZOffset = static_cast<double>(pr.targetZOffset);

                const bool redirectKeyHeld = pr.key != 0 && IsRedirectKeyDown(pr.key);
                const bool triggerActive = TriggerState::IsTriggerActive(pr.key, pr.requireKey, redirectKeyHeld);

                if (pr.enabled && triggerActive) {
                    float screenCenterX = overlay.GetWidth() * 0.5f;
                    float screenCenterY = overlay.GetHeight() * 0.5f;

                    auto isValidTarget = [&](const GameEntity* entity) {
                        if (!entity->isAlive || entity->isGhost) return false;
                        if (entity->isTeammate) return false;
                        if (entity->type == EntityType::PLAYER && !pr.targetPlayers) return false;
                        if (entity->type == EntityType::MONSTER && !pr.targetNPCs) return false;
                        if (entity->type != EntityType::PLAYER && entity->type != EntityType::MONSTER) return false;
                        return entity->distanceMeters <= pr.maxTargetDistance;
                    };

                    auto aimBone = [&](const GameEntity* entity) -> FVector {
                        if (!entity->hasBoneData) return entity->headPosition;
                        switch (pr.bone) {
                            case 0: return entity->boneHead;
                            case 1: return entity->boneNeck;
                            case 2: return entity->boneChest;
                            case 3: { // Nearest to crosshair
                                FVector2D sHead{}, sNeck{}, sChest{};
                                w2s.Project(entity->boneHead, sHead);
                                w2s.Project(entity->boneNeck, sNeck);
                                w2s.Project(entity->boneChest, sChest);
                                double dH = std::hypot(sHead.X - screenCenterX, sHead.Y - screenCenterY);
                                double dN = std::hypot(sNeck.X - screenCenterX, sNeck.Y - screenCenterY);
                                double dC = std::hypot(sChest.X - screenCenterX, sChest.Y - screenCenterY);
                                if (dH <= dN && dH <= dC) return entity->boneHead;
                                if (dN <= dC) return entity->boneNeck;
                                return entity->boneChest;
                            }
                            default: return entity->boneHead;
                        }
                    };

                    // Keep the locked target for as long as the trigger stays active;
                    // drop it once it dies, leaves range or is no longer scanned.
                    const GameEntity* target = nullptr;
                    if (lockedTargetAddr != 0) {
                        for (const auto* entity : filtered) {
                            if (entity->address == lockedTargetAddr) {
                                if (isValidTarget(entity)) target = entity;
                                break;
                            }
                        }
                    }

                    // No lock yet: take the nearest valid enemy in front of the camera.
                    if (!target) {
                        for (const auto* entity : filtered) {
                            if (!isValidTarget(entity)) continue;
                            FVector2D candidateScreen;
                            if (!w2s.Project(aimBone(entity), candidateScreen)) continue;
                            if (!target || entity->distanceMeters < target->distanceMeters)
                                target = entity;
                        }
                    }

                    lockedTargetAddr = target ? target->address : 0;

                    if (target) {
                        const FVector targetBone = aimBone(target);
                        ipc.hasTarget = 1;
                        ipc.targetX = targetBone.X;
                        ipc.targetY = targetBone.Y;
                        ipc.targetZ = targetBone.Z;
                        uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                        ipc.targetTimestamp = nowMs;

                        proxyTel.hasTarget = true;
                        proxyTel.targetX = targetBone.X;
                        proxyTel.targetY = targetBone.Y;
                        proxyTel.targetZ = targetBone.Z;
                        proxyTel.targetDistance = target->distanceMeters;
                    } else {
                        ipc.hasTarget = 0;
                    }
                } else {
                    lockedTargetAddr = 0;
                    ipc.hasTarget = 0;
                }
                game.SyncProxyTarget(ipc);
            }

            // =================================================================
            // Render overlay
            // =================================================================
            overlay.BeginFrame();
            imgui.BeginFrame();

            // Debug logging: every 100 frames, log filtered and on-screen entities
            static int frameCount = 0;
            frameCount++;
            bool doLog = (frameCount % 100 == 0);

            if (doLog) {
                int onScreenCount = 0;
                std::vector<const GameEntity*> sampled;
                for (const auto* entity : filtered) {
                    if (entity->isOnScreen) {
                        onScreenCount++;
                        if (sampled.size() < 5) {
                            sampled.push_back(entity);
                        }
                    }
                }
                spdlog::info("[DrawDebug] filtered={} onScreen={}", filtered.size(), onScreenCount);
                for (size_t i = 0; i < sampled.size(); ++i) {
                    const auto* e = sampled[i];
                    spdlog::info("[DrawDebug]  [{}] type={} dist={:.1f}m onScreen={}",
                                 i, static_cast<int>(e->type), e->distanceMeters, e->isOnScreen);
                }
            }

            ImDrawList* draw = ImGui::GetBackgroundDrawList();

            // Show loading indicator until first scan data arrives
            if (filtered.empty() && scanEntityCount.load() == 0) {
                Drawing::DrawLoadingStatus(draw, overlay.GetWidth(), overlay.GetHeight(),
                    "Scanning for actors...", -1.0f);
            }

            for (const auto* entity : filtered) {
                if (!entity->isOnScreen) continue;

                switch (entity->type) {
                    case EntityType::PLAYER: {
                        const float* playerColor = entity->isTeammate
                            ? config.Get().visuals.teamColor
                            : config.Get().visuals.playerColor;
                        ImU32 color = IM_COL32(
                            static_cast<int>(playerColor[0] * 255),
                            static_cast<int>(playerColor[1] * 255),
                            static_cast<int>(playerColor[2] * 255),
                            static_cast<int>(playerColor[3] * 255)
                        );
                        if (entity->isGhost) {
                            Drawing::DrawGhostPlayerESP(draw, *entity, color);
                        } else {
                            Drawing::DrawPlayerESP(draw, *entity, color, config.Get().visuals.boxStyle);
                        }

                        if (config.Get().filter.showSnapLines) {
                            Drawing::DrawSnapLine(draw,
                                ImVec2(entity->screenPos.X, entity->screenPos.Y),
                                overlay.GetWidth(), overlay.GetHeight(), color);
                        }
                        break;
                    }

                    case EntityType::LOOT_ITEM:
                        Drawing::DrawItemESP(draw, *entity);
                        break;

                    case EntityType::MONSTER: {
                        ImU32 color = IM_COL32(
                            static_cast<int>(config.Get().visuals.npcColor[0] * 255),
                            static_cast<int>(config.Get().visuals.npcColor[1] * 255),
                            static_cast<int>(config.Get().visuals.npcColor[2] * 255),
                            static_cast<int>(config.Get().visuals.npcColor[3] * 255)
                        );
                        Drawing::DrawPlayerESP(draw, *entity, color, config.Get().visuals.boxStyle);
                        break;
                    }

                    case EntityType::CHEST_SPECIAL: {
                        // Rare chests get bright gold highlight
                        ImU32 gold = IM_COL32(255, 215, 0, 255);
                        Drawing::DrawPlayerESP(draw, *entity, gold, 0);
                        break;
                    }

                    case EntityType::CHEST_NORMAL: {
                        // Normal chests get subtle white
                        ImU32 white = IM_COL32(200, 200, 200, 180);
                        Drawing::DrawPlayerESP(draw, *entity, white, 0);
                        break;
                    }

                    case EntityType::PORTAL: {
                        // Portals in blue
                        ImU32 blue = IM_COL32(50, 150, 255, 255);
                        Drawing::DrawPlayerESP(draw, *entity, blue, 0);
                        break;
                    }

                    default:
                        break;
                }
            }

            // Radar minimap
            Drawing::DrawRadar(draw, w2s.GetCameraPosition(),
                static_cast<float>(w2s.GetCameraRotation().Yaw),
                config.Get().visuals.minimapRotating,
                filtered, overlay.GetWidth(), overlay.GetHeight(),
                config.Get().visuals.radarRadius,
                config.Get().visuals.radarRangeMeters * 100.0f, // DrawRadar expects UU (1 m = 100 UU)
                IM_COL32(static_cast<int>(config.Get().visuals.teamColor[0] * 255),
                         static_cast<int>(config.Get().visuals.teamColor[1] * 255),
                         static_cast<int>(config.Get().visuals.teamColor[2] * 255), 255));

            // Draw in-game live Proxy HUD status indicator
            if (!externalMode) {
                char targetLabel[64] = {};
                if (proxyTel.hasTarget) {
                    snprintf(targetLabel, sizeof(targetLabel), "LOCK: %.1fm", proxyTel.targetDistance);
                }
                Drawing::DrawProxyHUD(draw, overlay.GetWidth(), overlay.GetHeight(),
                    proxyTel.moduleFound, proxyTel.isConnected, proxyTel.heartbeat,
                    proxyTel.isRedirecting, targetLabel);

                if (config.Get().projectileRedirect.showFOVCircle && proxyTel.isConnected) {
                    Drawing::DrawSilentAimFOV(draw, overlay.GetWidth(), overlay.GetHeight(),
                        proxyTel.hasTarget, proxyTel.isRedirecting,
                        config.Get().projectileRedirect.enabled,
                        config.Get().projectileRedirect.fov, w2s.GetFOV());
                    Drawing::DrawRedirectFireIndicator(draw, overlay.GetWidth(), overlay.GetHeight(),
                        proxyTel.isRedirecting);
                }
            }

            // Render menu
            menu.SetProxyTelemetry(proxyTel);
            menu.Render();

            imgui.EndFrame();
            overlay.EndFrame();
        } else {
            // No game data (lobby/loading) — render overlay and menu with live proxy telemetry
            menu.SetProxyTelemetry(proxyTel);

            overlay.BeginFrame();
            imgui.BeginFrame();
            menu.Render();
            imgui.EndFrame();
            overlay.EndFrame();
        }
    }
#ifdef _WIN32
    }
#endif

    // =========================================================================
    // Cleanup
    // =========================================================================
    spdlog::info("Shutting down...");
    running.store(false);
    if (scanThread.joinable()) scanThread.join();
    config.Save();
    imgui.Shutdown();
    overlay.Shutdown();
    game.Detach();

#ifdef _WIN32
    timeEndPeriod(1);
    FreeConsole();
#else
    Protection::Shutdown();
#endif
    return 0;
}
