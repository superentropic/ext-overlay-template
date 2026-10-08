#pragma once
#include <imgui.h>
#include <vector>
#include "sdk/ue5_types.h"
#include "game/entity.h"

namespace Drawing {
    // DPI scale factor (1.0 = 1080p, 2.0 = 4K, etc.)
    // Set once at startup, read by all drawing functions.
    void SetDpiScale(float scale);
    float GetDpiScale();

    // Draw 2D bounding box
    void DrawBox2D(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight,
                   ImU32 color, float thickness = 1.0f);

    // Draw corner-style box (only corners, not full rectangle)
    void DrawCornerBox(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight,
                       ImU32 color, float cornerLength = 8.0f, float thickness = 2.0f);

    // Draw text with dark background for readability
    void DrawLabel(ImDrawList* draw, const ImVec2& pos, const char* text,
                   ImU32 textColor = IM_COL32(255, 255, 255, 255),
                   ImU32 bgColor = IM_COL32(0, 0, 0, 180));

    // Draw centered text
    void DrawLabelCentered(ImDrawList* draw, const ImVec2& pos, const char* text,
                           ImU32 textColor = IM_COL32(255, 255, 255, 255),
                           ImU32 bgColor = IM_COL32(0, 0, 0, 180));

    // Draw health bar (horizontal)
    void DrawHealthBar(ImDrawList* draw, const ImVec2& pos, float width,
                       float healthPct, float height = 4.0f);

    // Draw health bar (vertical, spans height, fills up)
    void DrawHealthBarVertical(ImDrawList* draw, const ImVec2& pos,
                                float healthPct, float width, float height);

    // Draw line from screen bottom center to entity
    void DrawSnapLine(ImDrawList* draw, const ImVec2& entityPos,
                      int screenWidth, int screenHeight, ImU32 color);

    // Draw a filled circle
    void DrawCircleFilled(ImDrawList* draw, const ImVec2& center,
                          float radius, ImU32 color);

    // Draw distance text below entity
    void DrawDistance(ImDrawList* draw, const ImVec2& pos, float distanceMeters);

    // Full player ESP (box + name + class + health + distance)
    void DrawPlayerESP(ImDrawList* draw, const GameEntity& entity,
                       ImU32 color, int boxStyle = 0,
                       bool showDamageNumbers = true);

    // Ghost player marker (dot + name + distance for dormant/last-known players)
    void DrawGhostPlayerESP(ImDrawList* draw, const GameEntity& entity, ImU32 color);

    // Full item ESP (icon/dot + name + rarity)
    void DrawItemESP(ImDrawList* draw, const GameEntity& entity);

    // Minimap showing player positions
    void DrawRadar(ImDrawList* draw, const FVector& localPos, float localYaw,
               bool rotating,
                   const std::vector<const GameEntity*>& entities,
                   int screenWidth, int screenHeight,
                   float radarRadius = 80.0f, float radarRange = 5000.0f,
                   ImU32 teamColor = IM_COL32(50, 140, 255, 255));

    // Loading/status display (shown while waiting for first scan data)
    void DrawLoadingStatus(ImDrawList* draw, int screenWidth, int screenHeight,
                           const char* statusText, float progress = -1.0f);

    // Silent aim FOV ring (grey=idle, cyan=locked, green=firing)
    void DrawSilentAimFOV(ImDrawList* draw, int screenWidth, int screenHeight,
                          bool hasTarget, bool isRedirecting, bool enabled,
                          float redirectFovDeg, float cameraFovDeg);

    // Small firing indicator dot at screen centre while redirecting
    void DrawRedirectFireIndicator(ImDrawList* draw, int screenWidth, int screenHeight,
                                   bool isRedirecting);

    // Live in-game Proxy & Hook HUD widget
    void DrawProxyHUD(ImDrawList* draw, int screenWidth, int screenHeight,
                      bool proxyLoaded, bool proxyConnected, uint32_t heartbeat,
                      bool isRedirecting, const char* targetInfo = nullptr);
}
