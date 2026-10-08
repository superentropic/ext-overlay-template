cmake_minimum_required(VERSION 3.25)
project(ext-research-tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Dependencies
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ImGui sources (vendored)
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
if(WIN32)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# Core sources
set(CORE_SOURCES
    src/core/process.cpp
    src/core/process_finder.cpp
    src/core/pattern_scanner.cpp
    src/core/config.cpp
    src/core/runtime_paths.cpp
    src/core/integrity.cpp
)

# SDK sources
set(SDK_SOURCES
    src/sdk/gnames.cpp
    src/sdk/gworld.cpp
)

# Game sources
set(GAME_SOURCES
    src/game/actor_manager.cpp
    src/game/item_database.cpp
)

# Render sources
set(RENDER_SOURCES
    src/render/overlay.cpp
    src/render/imgui_manager.cpp
    src/render/drawing.cpp
    src/render/world_to_screen.cpp
    src/render/menu.cpp
)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    ${CORE_SOURCES}
    ${SDK_SOURCES}
    ${GAME_SOURCES}
    ${RENDER_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    dwmapi.lib
)

# Copy config files to build output
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/config" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
)

# =========================================================================
# Launcher / Dashboard executable
# =========================================================================
set(LAUNCHER_SOURCES
    src/launcher/launcher_main.cpp
    src/launcher/dashboard_window.cpp
    src/launcher/util/process_launcher.cpp
    src/launcher/util/dll_injector.cpp
    src/launcher/panels/tool_launcher_panel.cpp
    src/launcher/panels/log_panel.cpp
    src/launcher/panels/game_status_panel.cpp
    src/launcher/panels/offset_panel.cpp
    src/launcher/panels/settings_panel.cpp
    src/launcher/panels/injector_panel.cpp
)

add_executable(ext-launcher WIN32
    ${LAUNCHER_SOURCES}
    ${CORE_SOURCES}
    src/core/manual_mapper.cpp
    src/render/imgui_manager.cpp
    ${IMGUI_SOURCES}
    resources/launcher.rc
)

target_include_directories(ext-launcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(ext-launcher PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    dwmapi.lib
    comdlg32.lib
)

# Copy config files for launcher too
add_custom_command(TARGET ext-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/config" "$<TARGET_FILE_DIR:dad-launcher>/config"
)

# Copy resources (icon) for launcher
add_custom_command(TARGET ext-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:dad-launcher>/../../resources"
)

# =========================================================================
# CLI DLL injector utility (with manual mapping support)
# =========================================================================
add_executable(inject
    tools/inject.cpp
    src/core/manual_mapper.cpp
)
target_include_directories(inject PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(inject PRIVATE kernel32.lib)

# =========================================================================
# version.dll proxy — DLL search order hijacking
# Bypasses kernel-level anti-cheat by running inside the game process.
# Place in game directory to auto-load before anti-cheat initializes.
# =========================================================================
add_library(version-proxy SHARED
    src/proxy/version_proxy.cpp
    src/proxy/silent_helper.cpp
    src/proxy/version.def
)
set_target_properties(version-proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)
target_include_directories(version-proxy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(version-proxy PRIVATE kernel32.lib user32.lib version.lib winmm.lib)

else()

# Native Linux backend. The Windows DirectX/Win32 overlay, launcher, DLL
# injector, and proxy are intentionally not compiled on Linux.
find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

set(LINUX_IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(ext-research-tool
    src/main.cpp
    src/core/process_linux.cpp
    src/core/process_finder.cpp
    src/core/pattern_scanner.cpp
    src/core/config.cpp
    src/core/runtime_paths.cpp
    src/core/protection.cpp      # PowerVM-derived protection layer
    src/sdk/gnames.cpp
    src/sdk/gworld.cpp
    src/game/actor_manager.cpp
    src/game/item_database.cpp
    src/render/overlay_linux.cpp
    src/render/imgui_manager_linux.cpp
    src/render/menu.cpp
    src/render/drawing.cpp
    src/render/world_to_screen.cpp
    ${LINUX_IMGUI_SOURCES}
)

target_include_directories(ext-research-tool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(ext-research-tool PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Threads::Threads
    glfw
    OpenGL::GL
    X11::X11
    X11::Xext
)

include(CTest)
if(BUILD_TESTING)
    add_executable(startup-test
        tests/startup_test.cpp
        src/core/config.cpp
        src/core/runtime_paths.cpp
        src/core/process_linux.cpp
        src/core/process_finder.cpp
        src/core/pattern_scanner.cpp
        src/sdk/gworld.cpp
        src/sdk/gnames.cpp
    )
    target_include_directories(startup-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(startup-test PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME startup-regression COMMAND startup-test)

    # Link-time syscall mocks exercise the transport without privileges or a game.
    add_executable(kernel-driver-test
        tests/kernel_driver_test.cpp
        src/core/process_linux.cpp
        src/core/process_finder.cpp
    )
    target_include_directories(kernel-driver-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(kernel-driver-test PRIVATE spdlog::spdlog)
    target_link_options(kernel-driver-test PRIVATE
        -Wl,--wrap=open
        -Wl,--wrap=close
        -Wl,--wrap=ioctl
        -Wl,--wrap=process_vm_readv
        -Wl,--wrap=process_vm_writev
    )
    add_test(NAME kernel-driver-regression COMMAND kernel-driver-test)

    # Only accesses this test process's own memory. Skips if ABI 2 is unavailable.
    add_executable(kernel-driver-smoke-test tests/kernel_driver_smoke_test.cpp)
    target_include_directories(kernel-driver-smoke-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(kernel-driver-smoke-test PRIVATE spdlog::spdlog)
    add_test(NAME kernel-driver-self-memory COMMAND kernel-driver-smoke-test)
    set_tests_properties(kernel-driver-self-memory PROPERTIES SKIP_RETURN_CODE 77)
endif()

# MinGW cross-compiler is available, expose an explicit target so the proxy
# DLL can still be rebuilt alongside the Proton/Linux application.
find_program(MINGW64_CXX_COMPILER NAMES x86_64-w64-mingw32-g++)
if(MINGW64_CXX_COMPILER)
    set(VERSION_PROXY_OUTPUT "${CMAKE_SOURCE_DIR}/bin/version.dll")
    add_custom_command(
        OUTPUT "${VERSION_PROXY_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/bin"
        COMMAND "${MINGW64_CXX_COMPILER}"
            -std=c++20 -O2 -shared -static-libgcc -static-libstdc++
            "-I${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/src/proxy/version_proxy.cpp"
            "${CMAKE_SOURCE_DIR}/src/proxy/silent_helper.cpp"
            "${CMAKE_SOURCE_DIR}/src/proxy/version.def"
            -o "${VERSION_PROXY_OUTPUT}"
            -lkernel32 -luser32 -lwinmm
        DEPENDS
            src/proxy/version_proxy.cpp
            src/proxy/silent_helper.cpp
            src/proxy/silent_helper.h
            src/proxy/version.def
            src/shared/shared_memory.h
            src/core/trigger_state.h
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
    add_custom_target(version-proxy DEPENDS "${VERSION_PROXY_OUTPUT}")
endif()

endif()
