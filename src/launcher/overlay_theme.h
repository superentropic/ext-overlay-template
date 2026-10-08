#pragma once
// ============================================================================
//  Dark and Darker — ImGui Theme
//
//  Medieval dungeon aesthetic: deep blacks, warm gold accents, stone grays.
//  Inspired by the game's UI: dark backgrounds with gold/amber highlights,
//  gothic typography feel, and muted earth tones.
// ============================================================================

#include <imgui.h>

namespace DaDTheme {

// ── Color palette ──
// Primary gold/amber tones (from the game's logo and UI)
inline constexpr ImVec4 Gold          = {0.85f, 0.65f, 0.13f, 1.0f};   // #D9A621
inline constexpr ImVec4 GoldDark      = {0.60f, 0.45f, 0.10f, 1.0f};   // #997319
inline constexpr ImVec4 GoldDim       = {0.45f, 0.35f, 0.10f, 1.0f};   // #735919
inline constexpr ImVec4 GoldBright    = {1.00f, 0.80f, 0.20f, 1.0f};   // #FFCC33
inline constexpr ImVec4 GoldSubtle    = {0.30f, 0.24f, 0.08f, 1.0f};   // #4D3D14

// Background tones (deep dungeon blacks)
inline constexpr ImVec4 BgDarkest     = {0.04f, 0.03f, 0.03f, 1.0f};   // #0A0808
inline constexpr ImVec4 BgDark        = {0.07f, 0.06f, 0.06f, 1.0f};   // #120F0F
inline constexpr ImVec4 BgMedium      = {0.10f, 0.09f, 0.08f, 1.0f};   // #1A1714
inline constexpr ImVec4 BgLight       = {0.14f, 0.12f, 0.10f, 1.0f};   // #241F1A
inline constexpr ImVec4 BgLighter     = {0.18f, 0.15f, 0.12f, 1.0f};   // #2E261F

// Stone/metal grays
inline constexpr ImVec4 Stone         = {0.35f, 0.32f, 0.28f, 1.0f};   // #595247
inline constexpr ImVec4 StoneDark     = {0.22f, 0.20f, 0.17f, 1.0f};   // #38332B
inline constexpr ImVec4 StoneLight    = {0.45f, 0.42f, 0.38f, 1.0f};   // #736B61

// Status colors
inline constexpr ImVec4 Success       = {0.30f, 0.75f, 0.30f, 1.0f};   // Green
inline constexpr ImVec4 Warning       = {0.90f, 0.70f, 0.20f, 1.0f};   // Amber
inline constexpr ImVec4 Danger        = {0.85f, 0.25f, 0.20f, 1.0f};   // Red
inline constexpr ImVec4 Info          = {0.40f, 0.60f, 0.85f, 1.0f};   // Blue

// Text
inline constexpr ImVec4 TextPrimary   = {0.88f, 0.83f, 0.73f, 1.0f};   // Parchment white
inline constexpr ImVec4 TextSecondary = {0.58f, 0.53f, 0.45f, 1.0f};   // Muted stone
inline constexpr ImVec4 TextDisabled  = {0.38f, 0.35f, 0.30f, 1.0f};   // Dark stone

// Item rarity colors (matching in-game)
inline constexpr ImVec4 RarityCommon    = {0.65f, 0.65f, 0.65f, 1.0f}; // Gray
inline constexpr ImVec4 RarityUncommon  = {0.30f, 0.70f, 0.30f, 1.0f}; // Green
inline constexpr ImVec4 RarityRare      = {0.30f, 0.50f, 0.90f, 1.0f}; // Blue
inline constexpr ImVec4 RarityEpic      = {0.60f, 0.30f, 0.85f, 1.0f}; // Purple
inline constexpr ImVec4 RarityLegendary = {0.90f, 0.55f, 0.10f, 1.0f}; // Orange
inline constexpr ImVec4 RarityUnique    = {0.85f, 0.20f, 0.25f, 1.0f}; // Red

// ============================================================================
//  Apply the Dark and Darker theme to ImGui
// ============================================================================
inline void Apply() {
    ImGuiStyle& s = ImGui::GetStyle();

    // ── Shape / Layout ──
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 5);
    s.CellPadding       = ImVec2(6, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 12.0f;

    // ── Rounding (slightly rounded — medieval panels, not modern) ──
    s.WindowRounding    = 3.0f;
    s.ChildRounding     = 2.0f;
    s.FrameRounding     = 2.0f;
    s.PopupRounding     = 3.0f;
    s.ScrollbarRounding = 2.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 3.0f;

    // ── Borders ──
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 1.0f;

    // ── Alignment ──
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f); // Center window titles
    s.SeparatorTextAlign = ImVec2(0.5f, 0.5f); // Center separator text

    // ====================================================================
    //  Color assignments
    // ====================================================================
    ImVec4* c = s.Colors;

    // Text
    c[ImGuiCol_Text]                  = TextPrimary;
    c[ImGuiCol_TextDisabled]          = TextDisabled;

    // Backgrounds
    c[ImGuiCol_WindowBg]              = BgDark;
    c[ImGuiCol_ChildBg]               = {0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_PopupBg]               = {0.08f, 0.07f, 0.06f, 0.96f};

    // Borders
    c[ImGuiCol_Border]                = {0.25f, 0.20f, 0.12f, 0.60f}; // Warm dark border
    c[ImGuiCol_BorderShadow]          = {0.00f, 0.00f, 0.00f, 0.00f};

    // Frame backgrounds (input fields, checkboxes)
    c[ImGuiCol_FrameBg]               = BgMedium;
    c[ImGuiCol_FrameBgHovered]        = BgLight;
    c[ImGuiCol_FrameBgActive]         = BgLighter;

    // Title bar — gold accent on active
    c[ImGuiCol_TitleBg]               = BgDarkest;
    c[ImGuiCol_TitleBgActive]         = {0.15f, 0.12f, 0.06f, 1.0f};  // Dark gold tint
    c[ImGuiCol_TitleBgCollapsed]      = {0.04f, 0.03f, 0.03f, 0.75f};

    // Menu bar
    c[ImGuiCol_MenuBarBg]             = BgMedium;

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]           = {0.05f, 0.04f, 0.04f, 0.60f};
    c[ImGuiCol_ScrollbarGrab]         = StoneDark;
    c[ImGuiCol_ScrollbarGrabHovered]  = Stone;
    c[ImGuiCol_ScrollbarGrabActive]   = StoneLight;

    // Checkmark
    c[ImGuiCol_CheckMark]             = Gold;

    // Slider
    c[ImGuiCol_SliderGrab]            = GoldDark;
    c[ImGuiCol_SliderGrabActive]      = Gold;

    // Buttons — gold accent
    c[ImGuiCol_Button]                = GoldSubtle;
    c[ImGuiCol_ButtonHovered]         = GoldDim;
    c[ImGuiCol_ButtonActive]          = GoldDark;

    // Headers (collapsing headers, selectable, menu items)
    c[ImGuiCol_Header]                = {0.18f, 0.15f, 0.08f, 0.80f};  // Gold-tinted
    c[ImGuiCol_HeaderHovered]         = {0.25f, 0.20f, 0.10f, 0.80f};
    c[ImGuiCol_HeaderActive]          = {0.30f, 0.24f, 0.10f, 0.80f};

    // Separator
    c[ImGuiCol_Separator]             = {0.30f, 0.24f, 0.12f, 0.50f};  // Gold tint
    c[ImGuiCol_SeparatorHovered]      = GoldDark;
    c[ImGuiCol_SeparatorActive]       = Gold;

    // Resize grip
    c[ImGuiCol_ResizeGrip]            = {0.25f, 0.20f, 0.10f, 0.25f};
    c[ImGuiCol_ResizeGripHovered]     = GoldDim;
    c[ImGuiCol_ResizeGripActive]      = Gold;

    // Tabs — gold active tab
    c[ImGuiCol_Tab]                   = {0.12f, 0.10f, 0.07f, 0.90f};
    c[ImGuiCol_TabHovered]            = {0.25f, 0.20f, 0.10f, 0.90f};
    c[ImGuiCol_TabSelected]           = {0.20f, 0.16f, 0.06f, 1.0f};   // Active tab: dark gold
    c[ImGuiCol_TabSelectedOverline]   = Gold;
    c[ImGuiCol_TabDimmed]             = {0.08f, 0.07f, 0.05f, 0.90f};
    c[ImGuiCol_TabDimmedSelected]     = {0.14f, 0.12f, 0.06f, 1.0f};

    // Docking
    c[ImGuiCol_DockingPreview]        = {0.60f, 0.45f, 0.10f, 0.70f};  // Gold preview
    c[ImGuiCol_DockingEmptyBg]        = BgDarkest;

    // Table
    c[ImGuiCol_TableHeaderBg]         = {0.12f, 0.10f, 0.07f, 1.0f};
    c[ImGuiCol_TableBorderStrong]     = {0.25f, 0.20f, 0.12f, 1.0f};
    c[ImGuiCol_TableBorderLight]      = {0.18f, 0.15f, 0.10f, 1.0f};
    c[ImGuiCol_TableRowBg]            = {0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_TableRowBgAlt]         = {1.00f, 1.00f, 1.00f, 0.03f};

    // Plot
    c[ImGuiCol_PlotLines]             = Gold;
    c[ImGuiCol_PlotLinesHovered]      = GoldBright;
    c[ImGuiCol_PlotHistogram]         = GoldDark;
    c[ImGuiCol_PlotHistogramHovered]  = Gold;

    // Drag/drop
    c[ImGuiCol_DragDropTarget]        = {1.0f, 0.8f, 0.2f, 0.90f};

    // Nav
    c[ImGuiCol_NavCursor]             = Gold;
    c[ImGuiCol_NavWindowingHighlight] = {1.0f, 1.0f, 1.0f, 0.70f};
    c[ImGuiCol_NavWindowingDimBg]     = {0.00f, 0.00f, 0.00f, 0.50f};

    // Modal dim
    c[ImGuiCol_ModalWindowDimBg]      = {0.00f, 0.00f, 0.00f, 0.65f};

    // Text selection
    c[ImGuiCol_TextSelectedBg]        = {0.45f, 0.35f, 0.10f, 0.50f};
}

// ============================================================================
//  Render the title/header bar with game branding
// ============================================================================
inline void RenderDashboardHeader() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BgDarkest);
    ImGui::BeginChild("##header_bar", ImVec2(0, 48), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // Centered title with gold color
    const char* title = "DARK AND DARKER";
    const char* subtitle = "Research Dashboard";

    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle);
    float windowWidth = ImGui::GetWindowWidth();

    // Title
    ImGui::SetCursorPosX((windowWidth - titleSize.x) * 0.5f);
    ImGui::SetCursorPosY(4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Gold);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    // Subtitle
    ImGui::SetCursorPosX((windowWidth - subtitleSize.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, TextSecondary);
    ImGui::TextUnformatted(subtitle);
    ImGui::PopStyleColor();

    // Gold separator line at bottom
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lineY = ImGui::GetWindowPos().y + 46;
    dl->AddLine(
        ImVec2(ImGui::GetWindowPos().x + 20, lineY),
        ImVec2(ImGui::GetWindowPos().x + windowWidth - 20, lineY),
        IM_COL32(217, 166, 33, 180), 1.5f  // Gold line
    );

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// ============================================================================
//  Styled separator with optional gold text
// ============================================================================
inline void GoldSeparator(const char* text = nullptr) {
    if (text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Gold);
        ImGui::SeparatorText(text);
        ImGui::PopStyleColor();
    } else {
        ImGui::Separator();
    }
}

// ============================================================================
//  Status indicator with consistent styling
// ============================================================================
inline void StatusDot(bool active, const char* activeText, const char* inactiveText) {
    ImVec4 color = active ? Success : ImVec4(0.5f, 0.4f, 0.3f, 1.0f);
    const char* text = active ? activeText : inactiveText;
    ImGui::TextColored(color, "%s", text);
}

} // namespace DaDTheme
