#include "imgui_manager_linux.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

bool ImGuiManager::Initialize(GLFWwindow* window, float dpiScale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.Alpha = 0.97f;

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 130")) {
        spdlog::error("Linux ImGui backend initialization failed");
        ImGui::DestroyContext();
        return false;
    }

    const char* fonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"
    };
    for (const char* font : fonts) {
        if (io.Fonts->AddFontFromFileTTF(font, 16.0f * dpiScale))
            break;
    }

    m_initialized = true;
    return true;
}

void ImGuiManager::BeginFrame() {
    if (!m_initialized)
        return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::EndFrame() {
    if (!m_initialized)
        return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiManager::Shutdown() {
    if (!m_initialized)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}
