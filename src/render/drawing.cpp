#include "drawing.h"
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <spdlog/spdlog.h>

namespace Drawing {

// ============================================================================
// DPI scale factor — set once at startup from screen resolution.
// S = 1.0 at 1080p, 2.0 at 4K. All pixel constants multiply by S.
// ============================================================================
static float g_dpiScale = 1.0f;

void SetDpiScale(float scale) { g_dpiScale = scale; }
float GetDpiScale() { return g_dpiScale; }

// Shorthand for scaling a pixel value
static inline float S(float px) { return px * g_dpiScale; }

// ============================================================================
// Text shadow offset for all ESP labels (scaled outline for readability)
// ============================================================================
static constexpr ImU32 SHADOW_COLOR = IM_COL32(0, 0, 0, 200);

// Helper: draw text with a dark drop-shadow for readability (2 draws instead of 5)
static void DrawTextShadowed(ImDrawList* draw, const ImVec2& pos, ImU32 color, const char* text) {
    float off = S(1.0f);
    draw->AddText(ImVec2(pos.x + off, pos.y + off), SHADOW_COLOR, text);
    draw->AddText(pos, color, text);
}

// ============================================================================
// Deduplicated rarity color from equipment string (used in dead body + chest lists)
// ============================================================================
static ImU32 GetEquipmentItemColor(const std::string& item) {
    if (item.find("Artifact") != std::string::npos)
        return IM_COL32(255, 0, 0, 255);      // Red
    if (item.find("Unique") != std::string::npos)
        return IM_COL32(255, 212, 0, 255);     // Gold
    if (item.find("Legendary") != std::string::npos || item.find("Legend") != std::string::npos)
        return IM_COL32(255, 170, 0, 255);     // Orange
    if (item.find("Epic") != std::string::npos)
        return IM_COL32(255, 0, 255, 255);     // Purple
    if (item.find("Rare") != std::string::npos)
        return IM_COL32(0, 136, 255, 255);     // Blue
    if (item.find("Uncommon") != std::string::npos)
        return IM_COL32(50, 200, 50, 255);     // Green
    return IM_COL32(255, 200, 50, 255);        // Default gold
}

// ============================================================================
// Basic primitives
// ============================================================================

void DrawBox2D(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight,
               ImU32 color, float thickness) {
    float t = thickness * g_dpiScale;
    // Dark outline behind the colored box for contrast
    draw->AddRect(topLeft, bottomRight, IM_COL32(0, 0, 0, 160), 0.0f, 0, t + S(1.0f));
    draw->AddRect(topLeft, bottomRight, color, 0.0f, 0, t);
}

void DrawCornerBox(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight,
                   ImU32 color, float cornerLength, float thickness) {
    float w = bottomRight.x - topLeft.x;
    float h = bottomRight.y - topLeft.y;
    float cl = (std::min)(cornerLength * g_dpiScale, (std::min)(w, h) / 3.0f);
    float t = thickness * g_dpiScale;

    ImU32 shadow = IM_COL32(0, 0, 0, 160);
    float shadowThick = t + S(1.0f);

    // Top-left (shadow then color)
    draw->AddLine(topLeft, ImVec2(topLeft.x + cl, topLeft.y), shadow, shadowThick);
    draw->AddLine(topLeft, ImVec2(topLeft.x, topLeft.y + cl), shadow, shadowThick);
    draw->AddLine(topLeft, ImVec2(topLeft.x + cl, topLeft.y), color, t);
    draw->AddLine(topLeft, ImVec2(topLeft.x, topLeft.y + cl), color, t);

    // Top-right
    draw->AddLine(ImVec2(bottomRight.x, topLeft.y), ImVec2(bottomRight.x - cl, topLeft.y), shadow, shadowThick);
    draw->AddLine(ImVec2(bottomRight.x, topLeft.y), ImVec2(bottomRight.x, topLeft.y + cl), shadow, shadowThick);
    draw->AddLine(ImVec2(bottomRight.x, topLeft.y), ImVec2(bottomRight.x - cl, topLeft.y), color, t);
    draw->AddLine(ImVec2(bottomRight.x, topLeft.y), ImVec2(bottomRight.x, topLeft.y + cl), color, t);

    // Bottom-left
    draw->AddLine(ImVec2(topLeft.x, bottomRight.y), ImVec2(topLeft.x + cl, bottomRight.y), shadow, shadowThick);
    draw->AddLine(ImVec2(topLeft.x, bottomRight.y), ImVec2(topLeft.x, bottomRight.y - cl), shadow, shadowThick);
    draw->AddLine(ImVec2(topLeft.x, bottomRight.y), ImVec2(topLeft.x + cl, bottomRight.y), color, t);
    draw->AddLine(ImVec2(topLeft.x, bottomRight.y), ImVec2(topLeft.x, bottomRight.y - cl), color, t);

    // Bottom-right
    draw->AddLine(bottomRight, ImVec2(bottomRight.x - cl, bottomRight.y), shadow, shadowThick);
    draw->AddLine(bottomRight, ImVec2(bottomRight.x, bottomRight.y - cl), shadow, shadowThick);
    draw->AddLine(bottomRight, ImVec2(bottomRight.x - cl, bottomRight.y), color, t);
    draw->AddLine(bottomRight, ImVec2(bottomRight.x, bottomRight.y - cl), color, t);
}

void DrawLabel(ImDrawList* draw, const ImVec2& pos, const char* text,
               ImU32 textColor, ImU32 bgColor) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImVec2 bgMin(pos.x - S(3), pos.y - S(1));
    ImVec2 bgMax(pos.x + textSize.x + S(3), pos.y + textSize.y + S(1));

    draw->AddRectFilled(bgMin, bgMax, bgColor, S(2.0f));
    draw->AddText(pos, textColor, text);
}

void DrawLabelCentered(ImDrawList* draw, const ImVec2& pos, const char* text,
                       ImU32 textColor, ImU32 bgColor) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImVec2 textPos(pos.x - textSize.x / 2.0f, pos.y - textSize.y / 2.0f);

    ImVec2 bgMin(textPos.x - S(3), textPos.y - S(1));
    ImVec2 bgMax(textPos.x + textSize.x + S(3), textPos.y + textSize.y + S(1));

    draw->AddRectFilled(bgMin, bgMax, bgColor, S(2.0f));
    draw->AddText(textPos, textColor, text);
}

// ============================================================================
// Health bar — smooth gradient from green -> yellow -> red
// ============================================================================
void DrawHealthBar(ImDrawList* draw, const ImVec2& pos, float width,
                   float healthPct, float height) {
    healthPct = (std::max)(0.0f, (std::min)(1.0f, healthPct));

    // Background (dark with subtle border)
    draw->AddRectFilled(
        ImVec2(pos.x - S(1), pos.y - S(1)),
        ImVec2(pos.x + width + S(1), pos.y + height + S(1)),
        IM_COL32(0, 0, 0, 220), S(1.0f)
    );

    // Smooth gradient: 100% = green, 50% = yellow, 0% = red
    int r, g;
    if (healthPct > 0.5f) {
        float t = (healthPct - 0.5f) * 2.0f;
        r = static_cast<int>((1.0f - t) * 255);
        g = 255;
    } else {
        float t = healthPct * 2.0f;
        r = 255;
        g = static_cast<int>(t * 255);
    }
    ImU32 healthColor = IM_COL32(r, g, 0, 255);

    // Health fill
    float fillWidth = width * healthPct;
    if (fillWidth > S(1.0f)) {
        draw->AddRectFilled(
            ImVec2(pos.x, pos.y),
            ImVec2(pos.x + fillWidth, pos.y + height),
            healthColor, S(1.0f)
        );
    }
}

void DrawHealthBarVertical(ImDrawList* draw, const ImVec2& pos,
                           float healthPct, float width, float height) {
    healthPct = (std::max)(0.0f, (std::min)(1.0f, healthPct));

    // Background bar
    draw->AddRectFilled(
        ImVec2(pos.x - S(1), pos.y - S(1)),
        ImVec2(pos.x + width + S(1), pos.y + height + S(1)),
        IM_COL32(0, 0, 0, 220), S(1.0f)
    );

    // Smooth gradient: 100% = green, 50% = yellow, 0% = red
    int r, g;
    if (healthPct > 0.5f) {
        float t = (healthPct - 0.5f) * 2.0f;
        r = static_cast<int>((1.0f - t) * 255);
        g = 255;
    } else {
        float t = healthPct * 2.0f;
        r = 255;
        g = static_cast<int>(t * 255);
    }
    ImU32 healthColor = IM_COL32(r, g, 0, 255);

    // Vertical fill from bottom up
    float fillHeight = height * healthPct;
    if (fillHeight > S(1.0f)) {
        draw->AddRectFilled(
            ImVec2(pos.x, pos.y + height - fillHeight),
            ImVec2(pos.x + width, pos.y + height),
            healthColor, S(1.0f)
        );
    }
}

void DrawSnapLine(ImDrawList* draw, const ImVec2& entityPos,
                  int screenWidth, int screenHeight, ImU32 color) {
    ImVec2 screenBottom(screenWidth / 2.0f, static_cast<float>(screenHeight));
    draw->AddLine(screenBottom, entityPos, IM_COL32(0, 0, 0, 100), S(2.0f));
    draw->AddLine(screenBottom, entityPos, color, S(1.0f));
}

void DrawCircleFilled(ImDrawList* draw, const ImVec2& center,
                      float radius, ImU32 color) {
    draw->AddCircleFilled(center, radius, color);
}

void DrawDistance(ImDrawList* draw, const ImVec2& pos, float distanceMeters) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0fm", distanceMeters);
    DrawLabelCentered(draw, pos, buf, IM_COL32(200, 200, 200, 255), IM_COL32(0, 0, 0, 140));
}

// ============================================================================
// Full player/monster/chest ESP
// ============================================================================
static void GetEntityBox(const GameEntity& entity, ImVec2& topLeft, ImVec2& bottomRight) {
    float boxTop, boxBottom, boxCenterX, boxWidth;

    if (entity.hasBoneData && entity.headOnScreen && entity.feetOnScreen) {
        // Bone-based: box spans from projected head to projected feet
        boxTop = (std::min)(entity.screenHead.Y, entity.screenFeet.Y);
        boxBottom = (std::max)(entity.screenHead.Y, entity.screenFeet.Y);
        boxCenterX = (entity.screenHead.X + entity.screenFeet.X) / 2.0f;
        float boneHeight = boxBottom - boxTop;
        boneHeight = (std::max)(boneHeight, S(8.0f));
        boxWidth = boneHeight * 0.35f;
        float headPad = boneHeight * 0.10f;
        boxTop -= headPad;
    } else {
        // Fallback: estimate box from distance (scaled for resolution)
        float sx = entity.screenPos.X;
        float sy = entity.screenPos.Y;
        float refScreenHeight = S(100.0f); // 100px at 1080p, 200px at 4K (at 10m)
        float refDistance = 10.0f;
        float scale = refDistance / (std::max)(entity.distanceMeters, 1.0f);
        float estHeight = refScreenHeight * scale;
        estHeight = (std::max)(estHeight, S(12.0f));
        estHeight = (std::min)(estHeight, S(400.0f));
        float estWidth = estHeight * 0.35f;
        estWidth = (std::max)(estWidth, S(6.0f));

        boxTop = sy - estHeight * 0.7f;
        boxBottom = sy + estHeight * 0.3f;
        boxCenterX = sx;
        boxWidth = estWidth;
    }

    // Enforce minimum on-screen size for boxes so distant entities remain visible.
    // Minimum height: 40 pixels (scaled), minimum width: 40 pixels total (20 half-width).
    const float minHeight = S(40.0f);
    const float minHalfWidth = S(20.0f);
    const float heightPadding = (std::max)(0.0f, minHeight - (boxBottom - boxTop)) * 0.5f;
    boxTop -= heightPadding;
    boxBottom += heightPadding;
    if (boxBottom > boxTop) {
        boxWidth = (std::max)(boxWidth, minHalfWidth); // enforce minimum half-width
    }

    topLeft = ImVec2(boxCenterX - boxWidth, boxTop);
    bottomRight = ImVec2(boxCenterX + boxWidth, boxBottom);
}

void DrawPlayerESP(ImDrawList* draw, const GameEntity& entity,
                   ImU32 color, int boxStyle, bool showDamageNumbers) {
    if (!entity.isOnScreen) return;

    float sx = entity.screenPos.X;
    float sy = entity.screenPos.Y;

    // ---- Dead entity: simple X marker + name + equipment ----
    if (!entity.isAlive) {
        bool hasLoot = !entity.equipment.empty();
        ImU32 deadColor = hasLoot
            ? IM_COL32(255, 200, 50, 220)
            : ((color & 0x00FFFFFF) | 0x80000000);

        float xSize = hasLoot ? S(10.0f) : S(8.0f);
        if (entity.distanceMeters > 10.0f) xSize *= (10.0f / entity.distanceMeters);
        xSize = (std::max)(xSize, S(4.0f));

        // X shadow + color
        draw->AddLine(ImVec2(sx - xSize, sy - xSize), ImVec2(sx + xSize, sy + xSize), SHADOW_COLOR, S(3.0f));
        draw->AddLine(ImVec2(sx + xSize, sy - xSize), ImVec2(sx - xSize, sy + xSize), SHADOW_COLOR, S(3.0f));
        draw->AddLine(ImVec2(sx - xSize, sy - xSize), ImVec2(sx + xSize, sy + xSize), deadColor, S(2.0f));
        draw->AddLine(ImVec2(sx + xSize, sy - xSize), ImVec2(sx - xSize, sy + xSize), deadColor, S(2.0f));

        // Name label
        char label[128];
        if (entity.type == EntityType::PLAYER && !entity.playerClass.empty()) {
            if (hasLoot) {
                snprintf(label, sizeof(label), "%s - %s (LOOT)", entity.displayName.c_str(), entity.playerClass.c_str());
            } else {
                snprintf(label, sizeof(label), "%s - %s (Dead)", entity.displayName.c_str(), entity.playerClass.c_str());
            }
        } else {
            if (hasLoot) {
                snprintf(label, sizeof(label), "%s (LOOT)", entity.displayName.c_str());
            } else {
                snprintf(label, sizeof(label), "%s (Dead)", entity.displayName.c_str());
            }
        }
        DrawLabelCentered(draw, ImVec2(sx, sy - xSize - S(10)), label, deadColor);

        if (showDamageNumbers && entity.healthChange != 0.0f &&
            entity.healthChangeDisplayFrames > 0) {
            char changeLabel[32];
            snprintf(changeLabel, sizeof(changeLabel), "%c%.0f",
                     entity.healthChange > 0.0f ? '+' : '-',
                     std::abs(entity.healthChange));
            DrawLabelCentered(draw, ImVec2(sx, sy - xSize - S(26)),
                              changeLabel,
                              entity.healthChange > 0.0f
                                  ? IM_COL32(255, 225, 40, 255)  // healing
                                  : IM_COL32(190, 70, 255, 255)); // damage
        }

        // Distance
        DrawDistance(draw, ImVec2(sx, sy + xSize + S(4)), entity.distanceMeters);

        // Equipment on dead bodies
        if (hasLoot) {
            float equipY = sy + xSize + S(18);
            for (const auto& item : entity.equipment) {
                ImU32 itemColor = GetEquipmentItemColor(item);
                DrawLabelCentered(draw, ImVec2(sx, equipY), item.c_str(), itemColor);
                equipY += S(14);
            }
        }
        return;
    }

    // ---- Alive entity: full box + health bar ----
    ImVec2 topLeft, bottomRight;
    GetEntityBox(entity, topLeft, bottomRight);
    const float boxCenterX = (topLeft.x + bottomRight.x) * 0.5f;

    // Debug: every 10 frames, log box size for non-loot entities.
    static int drawFrame = 0;
    drawFrame++;
    if (drawFrame % 10 == 0 &&
        (entity.type == EntityType::PLAYER || entity.type == EntityType::MONSTER ||
         entity.type == EntityType::CHEST_NORMAL || entity.type == EntityType::CHEST_SPECIAL)) {
        spdlog::info("[DrawESP] type={} dist={:.1f}m headOn={:d} feetOn={:d} "
                     "boxH={:.1f}px boxW={:.1f}px onScreen={:d}",
                     static_cast<int>(entity.type), entity.distanceMeters,
                     entity.headOnScreen, entity.feetOnScreen,
                     bottomRight.y - topLeft.y, bottomRight.x - topLeft.x, entity.isOnScreen);
    }

    // Draw box based on style
    switch (boxStyle) {
        case 0: // 2D box
            DrawBox2D(draw, topLeft, bottomRight, color, 1.5f);
            break;
        case 1: // Filled box
            draw->AddRectFilled(topLeft, bottomRight, (color & 0x00FFFFFF) | 0x30000000);
            DrawBox2D(draw, topLeft, bottomRight, color, 2.0f);
            break;
        case 2: // Corner box
            DrawCornerBox(draw, topLeft, bottomRight, color);
            break;
        default:
            DrawBox2D(draw, topLeft, bottomRight, color, 1.5f);
            break;
    }

    // Name label (top line)
    char label[128];
    if (entity.type == EntityType::PLAYER && !entity.playerClass.empty()) {
        snprintf(label, sizeof(label), "%s - %s",
                 entity.displayName.c_str(), entity.playerClass.c_str());
    } else if (entity.type == EntityType::MONSTER && entity.monsterGrade != MonsterGrade::None
               && entity.monsterGrade != MonsterGrade::Common) {
        snprintf(label, sizeof(label), "%s (%s)",
                 entity.displayName.c_str(), MonsterGradeToString(entity.monsterGrade));
    } else {
        snprintf(label, sizeof(label), "%s", entity.displayName.c_str());
    }
    DrawLabelCentered(draw, ImVec2(boxCenterX, topLeft.y - S(14)), label, color);

    if (showDamageNumbers && entity.healthChange != 0.0f &&
        entity.healthChangeDisplayFrames > 0) {
        char changeLabel[32];
        snprintf(changeLabel, sizeof(changeLabel), "%c%.0f",
                 entity.healthChange > 0.0f ? '+' : '-',
                 std::abs(entity.healthChange));
        DrawLabelCentered(draw, ImVec2(boxCenterX, topLeft.y - S(30)),
                          changeLabel,
                          entity.healthChange > 0.0f
                              ? IM_COL32(255, 225, 40, 255)  // healing
                              : IM_COL32(190, 70, 255, 255)); // damage
    }

    // Health bar (vertical, left of box)
    if (entity.maxHealth > 0 && entity.health >= 0.0f) {
        float healthPct = entity.health / entity.maxHealth;
        float barW = S(6.0f);
        float barH = (bottomRight.y - topLeft.y) + S(4); // span box height
        // Place just left of the box
        DrawHealthBarVertical(draw, ImVec2(topLeft.x - barW - S(3), topLeft.y),
                              healthPct, barW, barH);

        char healthLabel[64];
        snprintf(healthLabel, sizeof(healthLabel), "%.0f/%.0f",
                 entity.health, entity.maxHealth);
        DrawLabelCentered(draw,
                          ImVec2(topLeft.x - barW - S(3) - S(30), topLeft.y + barH / 2.0f),
                          healthLabel, IM_COL32(235, 235, 235, 255));
    }

    // Distance (below box)
    DrawDistance(draw, ImVec2(boxCenterX, bottomRight.y + S(4)), entity.distanceMeters);

    // Equipment/contents list
    float equipY = bottomRight.y + S(20);
    if (!entity.equipment.empty()) {
        for (const auto& item : entity.equipment) {
            ImU32 itemColor = GetEquipmentItemColor(item);
            DrawLabelCentered(draw, ImVec2(boxCenterX, equipY), item.c_str(), itemColor);
            equipY += S(14);
        }
    }
}

// ============================================================================
// Item ESP (ground loot)
// ============================================================================
void DrawItemESP(ImDrawList* draw, const GameEntity& entity) {
    if (!entity.isOnScreen) return;

    ImU32 color = RarityToColor(entity.rarity);
    float sx = entity.screenPos.X;
    float sy = entity.screenPos.Y;

    // Diamond size scales with distance (bigger when close)
    float size = S(6.0f);
    if (entity.distanceMeters > 5.0f) size *= (5.0f / entity.distanceMeters);
    size = (std::max)(size, S(3.0f));

    // Rarity-colored diamond marker
    ImVec2 diamond[4] = {
        ImVec2(sx, sy - size),
        ImVec2(sx + size, sy),
        ImVec2(sx, sy + size),
        ImVec2(sx - size, sy)
    };

    // Shadow outline
    float so = S(1.0f); // shadow offset
    ImVec2 diamondShadow[4] = {
        ImVec2(sx, sy - size - so),
        ImVec2(sx + size + so, sy),
        ImVec2(sx, sy + size + so),
        ImVec2(sx - size - so, sy)
    };
    draw->AddPolyline(diamondShadow, 4, SHADOW_COLOR, ImDrawFlags_Closed, S(2.5f));

    // Filled diamond
    ImU32 fillColor = (color & 0x00FFFFFF) | 0x60000000;
    draw->AddConvexPolyFilled(diamond, 4, fillColor);
    draw->AddPolyline(diamond, 4, color, ImDrawFlags_Closed, S(1.5f));

    // Epic+ glow ring
    if (entity.rarity >= ItemRarity::EPIC) {
        ImU32 glowColor = (color & 0x00FFFFFF) | 0x30000000;
        draw->AddCircle(ImVec2(sx, sy), size + S(3.0f), glowColor, 8, S(1.0f));
    }

    // Item name
    char label[256];
    snprintf(label, sizeof(label), "%s", entity.displayName.c_str());
    DrawLabelCentered(draw, ImVec2(sx, sy - size - S(12)), label, color);

    // Distance below
    DrawDistance(draw, ImVec2(sx, sy + size + S(4)), entity.distanceMeters);
}

// ============================================================================
// Radar minimap
// ============================================================================
void DrawRadar(ImDrawList* draw, const FVector& localPos, float localYaw,
               bool rotating,
               const std::vector<const GameEntity*>& entities,
               int screenWidth, int screenHeight,
               float radarRadius, float radarRange, ImU32 teamColor) {
    // Scale radar size with DPI
    float r = radarRadius * g_dpiScale;

    // Minimap position: top-right corner
    ImVec2 center(screenWidth - r - S(20), r + S(20));
    
    // Bounds for clipping
    ImVec2 min_bounds(center.x - r, center.y - r);
    ImVec2 max_bounds(center.x + r, center.y + r);

    // Background rounded rectangle
    draw->AddRectFilled(min_bounds, max_bounds, IM_COL32(20, 20, 20, 160), S(8.0f));
    draw->AddRect(min_bounds, max_bounds, IM_COL32(80, 80, 80, 200), S(8.0f), 0, S(1.5f));

    // Draw grid
    int gridSize = 4; // 4x4 grid
    float gridStep = (r * 2.0f) / gridSize;
    for (int i = 1; i < gridSize; ++i) {
        float offset = min_bounds.x + i * gridStep;
        draw->AddLine(ImVec2(offset, min_bounds.y), ImVec2(offset, max_bounds.y), IM_COL32(50, 50, 50, 100), S(1.0f));
        
        offset = min_bounds.y + i * gridStep;
        draw->AddLine(ImVec2(min_bounds.x, offset), ImVec2(max_bounds.x, offset), IM_COL32(50, 50, 50, 100), S(1.0f));
    }

    // Cross-hair lines (thicker in center)
    draw->AddLine(ImVec2(center.x - r, center.y),
                  ImVec2(center.x + r, center.y),
                  IM_COL32(70, 70, 70, 150), S(1.5f));
    draw->AddLine(ImVec2(center.x, center.y - r),
                  ImVec2(center.x, center.y + r),
                  IM_COL32(70, 70, 70, 150), S(1.5f));

    // Label showing scale (bottom right inside map)
    float rangeMeters = radarRange / 100.0f; // UU to meters
    char scaleLabel[32];
    snprintf(scaleLabel, sizeof(scaleLabel), "Scale: %.0fm", rangeMeters);
    ImVec2 labelSize = ImGui::CalcTextSize(scaleLabel);
    draw->AddText(ImVec2(max_bounds.x - labelSize.x - S(4), max_bounds.y - labelSize.y - S(4)),
                  IM_COL32(150, 150, 150, 200), scaleLabel);

    float yawRad = static_cast<float>(localYaw * 3.14159265358979 / 180.0);
    float cosYaw = cosf(yawRad);
    float sinYaw = sinf(yawRad);

    // Apply clip rect so dots don't draw outside the square map
    draw->PushClipRect(min_bounds, max_bounds, true);

    for (const auto* entity : entities) {
        if (entity->isLocalPlayer) continue;
        if (entity->type != EntityType::PLAYER &&
            entity->type != EntityType::MONSTER &&
            entity->type != EntityType::PORTAL)
            continue;

        float dx = static_cast<float>(entity->position.X - localPos.X);
        float dy = static_cast<float>(entity->position.Y - localPos.Y);

        float worldDist = sqrtf(dx * dx + dy * dy);
        if (worldDist < 50.0f && entity->type == EntityType::PLAYER) continue; // <0.5m = self

        float rx, ry;
        if (rotating) {
            // Rotating map: rotate world coordinates opposite to player yaw
            // so player is always pointing UP (-Y)
            rx = dy * cosYaw - dx * sinYaw;
            ry = dy * sinYaw + dx * cosYaw;
        } else {
            // Static map: +X is up, -Y is right
            rx = -dy;
            ry = dx;
        }

        float scale = r / radarRange;
        float px = rx * scale;
        float py = -ry * scale;

        ImVec2 dotPos(center.x + px, center.y + py);

        // Don't draw entities completely out of bounds (let clip rect handle edges, but don't draw them if they are way off)
        if (px < -r || px > r || py < -r || py > r) {
            continue;
        }

        // Last-seen (ghost) players: purple question mark instead of a dot.
        if (entity->type == EntityType::PLAYER && entity->isGhost) {
            ImFont* font = ImGui::GetFont();
            const float fontSize = S(16.0f);
            const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "?");
            const ImVec2 textPos(dotPos.x - textSize.x * 0.5f, dotPos.y - textSize.y * 0.5f);
            const float o = S(1.0f);
            draw->AddText(font, fontSize, ImVec2(textPos.x - o, textPos.y), IM_COL32(0, 0, 0, 200), "?");
            draw->AddText(font, fontSize, ImVec2(textPos.x + o, textPos.y), IM_COL32(0, 0, 0, 200), "?");
            draw->AddText(font, fontSize, ImVec2(textPos.x, textPos.y - o), IM_COL32(0, 0, 0, 200), "?");
            draw->AddText(font, fontSize, ImVec2(textPos.x, textPos.y + o), IM_COL32(0, 0, 0, 200), "?");
            draw->AddText(font, fontSize, textPos, IM_COL32(180, 70, 255, 255), "?");
            continue;
        }

        ImU32 dotColor;
        float dotSize;
        if (entity->type == EntityType::PLAYER && entity->isTeammate) {
            dotColor = entity->isAlive ? teamColor
                                       : ((teamColor & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 150));
            dotSize = S(4.0f);
        } else if (entity->type == EntityType::PLAYER) {
            dotColor = entity->isAlive ? IM_COL32(255, 50, 50, 255)
                                       : IM_COL32(150, 50, 50, 150);
            dotSize = S(4.0f);
        } else if (entity->type == EntityType::PORTAL) {
            dotColor = IM_COL32(50, 150, 255, 240);
            dotSize = S(5.0f);
        } else {
            dotColor = entity->isAlive ? IM_COL32(255, 200, 50, 200)
                                       : IM_COL32(100, 80, 30, 120);
            dotSize = S(3.0f);
        }

        draw->AddCircleFilled(dotPos, dotSize + S(1.0f), IM_COL32(0, 0, 0, 120));
        draw->AddCircleFilled(dotPos, dotSize, dotColor);
    }
    
    // Draw local player arrow at center
    {
        float arrowLen = S(8.0f);
        float arrowHalf = S(4.0f);
        ImVec2 forward, side;
        if (rotating) {
            // Map rotates, so player always points UP (-Y)
            forward = ImVec2(0, -1);
        } else {
            // Map is static, so player arrow rotates
            forward = ImVec2(-sinYaw, -cosYaw);
        }
        side = ImVec2(-forward.y, forward.x);
        
        ImVec2 tip(center.x + forward.x * arrowLen,
                  center.y + forward.y * arrowLen);
        ImVec2 left(center.x - forward.x * arrowHalf + side.x * arrowHalf,
                    center.y - forward.y * arrowHalf + side.y * arrowHalf);
        ImVec2 right(center.x - forward.x * arrowHalf - side.x * arrowHalf,
                     center.y - forward.y * arrowHalf - side.y * arrowHalf);
                     
        draw->AddTriangleFilled(tip, left, right, IM_COL32(255, 255, 255, 240));
        draw->AddTriangle(tip, left, right, IM_COL32(200, 200, 200, 255), S(1.0f));
    }

    draw->PopClipRect();

    // Label
    DrawTextShadowed(draw, ImVec2(min_bounds.x, min_bounds.y - S(16)),
                     IM_COL32(180, 180, 180, 200), "MINIMAP");
}

// ============================================================================
// Loading/status display
// ============================================================================
void DrawLoadingStatus(ImDrawList* draw, int screenWidth, int screenHeight,
                       const char* statusText, float progress) {
    float cx = screenWidth / 2.0f;
    float cy = screenHeight / 2.0f;

    ImVec2 boxMin(cx - S(200), cy - S(40));
    ImVec2 boxMax(cx + S(200), cy + S(40));
    draw->AddRectFilled(boxMin, boxMax, IM_COL32(10, 10, 10, 200), S(8.0f));
    draw->AddRect(boxMin, boxMax, IM_COL32(80, 80, 80, 200), S(8.0f), 0, S(1.0f));

    // Title
    DrawTextShadowed(draw, ImVec2(cx - ImGui::CalcTextSize("DAD Research Tool").x / 2.0f, cy - S(30)),
                     IM_COL32(200, 200, 200, 255), "DAD Research Tool");

    // Status text
    ImVec2 statusSize = ImGui::CalcTextSize(statusText);
    DrawTextShadowed(draw, ImVec2(cx - statusSize.x / 2.0f, cy - S(5)),
                     IM_COL32(150, 150, 150, 255), statusText);

    // Progress bar
    if (progress >= 0.0f) {
        float barWidth = S(300.0f);
        float barHeight = S(4.0f);
        float barX = cx - barWidth / 2.0f;
        float barY = cy + S(20);

        draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barWidth, barY + barHeight),
                            IM_COL32(40, 40, 40, 200), S(2.0f));
        float fillWidth = barWidth * (std::min)(1.0f, progress);
        if (fillWidth > 0) {
            draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + fillWidth, barY + barHeight),
                                IM_COL32(0, 180, 255, 255), S(2.0f));
        }
    }
}

void DrawGhostPlayerESP(ImDrawList* draw, const GameEntity& entity, ImU32 color) {
    if (!entity.isOnScreen) return;

    float sx = entity.screenPos.X;
    float sy = entity.screenPos.Y;

    // Reduce alpha to ~50% for ghost appearance
    ImU32 ghostColor = (color & 0x00FFFFFF) | 0x80000000;
    ImU32 ghostOutline = IM_COL32(0, 0, 0, 100);

    // Draw a dot (small filled circle with outline)
    float dotRadius = S(5.0f);
    draw->AddCircleFilled(ImVec2(sx, sy), dotRadius + S(1.0f), ghostOutline, 12);
    draw->AddCircleFilled(ImVec2(sx, sy), dotRadius, ghostColor, 12);

    // Inner bright core
    draw->AddCircleFilled(ImVec2(sx, sy), dotRadius * 0.5f,
        (color & 0x00FFFFFF) | 0xB0000000, 8);

    // Name label above dot
    char label[128];
    if (!entity.playerClass.empty()) {
        snprintf(label, sizeof(label), "%s - %s",
                 entity.displayName.c_str(), entity.playerClass.c_str());
    } else if (!entity.displayName.empty()) {
        snprintf(label, sizeof(label), "%s", entity.displayName.c_str());
    } else {
        snprintf(label, sizeof(label), "Player");
    }
    DrawLabelCentered(draw, ImVec2(sx, sy - dotRadius - S(12)), label, ghostColor);

    // "LAST SEEN" tag
    DrawLabelCentered(draw, ImVec2(sx, sy - dotRadius - S(26)), "~LAST SEEN~",
        IM_COL32(255, 180, 50, 140));

    // Distance below dot
    DrawDistance(draw, ImVec2(sx, sy + dotRadius + S(4)), entity.distanceMeters);
}

void DrawSilentAimFOV(ImDrawList* draw, int screenWidth, int screenHeight,
                      bool hasTarget, bool isRedirecting, bool enabled,
                      float redirectFovDeg, float cameraFovDeg) {
    if (!enabled || redirectFovDeg <= 0.0f || cameraFovDeg <= 1.0f) return;

    const float cx = screenWidth  / 2.0f;
    const float cy = screenHeight / 2.0f;
    constexpr float PI = 3.14159265f;

    const float halfCamRad = (cameraFovDeg / 2.0f) * (PI / 180.0f);
    const float redirectRad = (std::min)(redirectFovDeg, 89.0f) * (PI / 180.0f);
    const float tanRatio = tanf(redirectRad) / tanf(halfCamRad);
    float radius = tanRatio * (screenWidth / 2.0f);

    const float maxRadius = (std::min)(static_cast<float>(screenWidth), static_cast<float>(screenHeight)) * 0.45f;
    radius = (std::min)(radius, maxRadius);
    if (radius < S(4.0f)) return;

    ImU32 ringColor;
    float thickness;

    if (isRedirecting) {
        ringColor = IM_COL32(50, 255, 80, 230);
        thickness = S(2.0f);
    } else if (hasTarget) {
        ringColor = IM_COL32(50, 200, 255, 180);
        thickness = S(1.5f);
    } else {
        ringColor = IM_COL32(130, 130, 130, 70);
        thickness = S(1.0f);
    }

    draw->AddCircle(ImVec2(cx, cy), radius + S(1.0f), IM_COL32(0, 0, 0, 90), 64, thickness + S(1.0f));
    draw->AddCircle(ImVec2(cx, cy), radius, ringColor, 64, thickness);

    if (hasTarget || isRedirecting) {
        char label[24];
        snprintf(label, sizeof(label), "FOV %.0f\xC2\xB0", redirectFovDeg);
        DrawLabelCentered(draw, ImVec2(cx, cy - radius - S(12.0f)), label, ringColor, IM_COL32(0, 0, 0, 130));
    }
}

void DrawRedirectFireIndicator(ImDrawList* draw, int screenWidth, int screenHeight,
                               bool isRedirecting) {
    if (!isRedirecting) return;

    const float cx = screenWidth  / 2.0f;
    const float cy = screenHeight / 2.0f;
    const float sz = S(5.0f);
    ImVec2 pts[4] = {
        ImVec2(cx,      cy - sz),
        ImVec2(cx + sz, cy     ),
        ImVec2(cx,      cy + sz),
        ImVec2(cx - sz, cy     ),
    };
    draw->AddConvexPolyFilled(pts, 4, IM_COL32(50, 255, 80, 200));
    draw->AddPolyline(pts, 4, IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, S(1.0f));
}

void DrawProxyHUD(ImDrawList* draw, int screenWidth, int screenHeight,
                  bool proxyLoaded, bool proxyConnected, uint32_t heartbeat,
                  bool isRedirecting, const char* targetInfo) {
    float x = S(20.0f);
    float y = S(20.0f);
    float pad = S(6.0f);

    char buf[128];
    ImU32 statusBg = IM_COL32(15, 15, 20, 200);
    ImU32 borderCol = IM_COL32(50, 50, 60, 220);
    ImU32 textCol = IM_COL32(200, 200, 200, 255);
    ImU32 dotCol = IM_COL32(240, 50, 50, 255); // Red

    if (proxyConnected) {
        dotCol = IM_COL32(50, 230, 50, 255); // Green
        borderCol = IM_COL32(40, 160, 60, 200);
        snprintf(buf, sizeof(buf), "PROXY: ONLINE #%u", heartbeat);
    } else if (proxyLoaded) {
        dotCol = IM_COL32(240, 180, 40, 255); // Orange/Yellow
        borderCol = IM_COL32(180, 140, 30, 200);
        snprintf(buf, sizeof(buf), "PROXY: INITIALIZING...");
    } else {
        snprintf(buf, sizeof(buf), "PROXY: OFFLINE (Awaiting injection)");
    }

    ImVec2 txtSize = ImGui::CalcTextSize(buf);
    float boxW = txtSize.x + S(32.0f);
    float boxH = txtSize.y + pad * 2.0f;

    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + boxW, y + boxH), statusBg, S(4.0f));
    draw->AddRect(ImVec2(x, y), ImVec2(x + boxW, y + boxH), borderCol, S(4.0f));

    // Status dot
    draw->AddCircleFilled(ImVec2(x + pad + S(4.0f), y + boxH * 0.5f), S(3.5f), dotCol);

    // Status text
    draw->AddText(ImVec2(x + pad + S(12.0f), y + pad), textCol, buf);

    // If currently redirecting, show yellow badge right next to it
    if (isRedirecting) {
        float redX = x + boxW + S(8.0f);
        const char* redTxt = "[SILENT AIM: REDIRECTING]";
        ImVec2 redSize = ImGui::CalcTextSize(redTxt);
        float redW = redSize.x + pad * 2.0f;
        draw->AddRectFilled(ImVec2(redX, y), ImVec2(redX + redW, y + boxH), IM_COL32(160, 120, 0, 220), S(4.0f));
        draw->AddRect(ImVec2(redX, y), ImVec2(redX + redW, y + boxH), IM_COL32(255, 200, 0, 255), S(4.0f));
        draw->AddText(ImVec2(redX + pad, y + pad), IM_COL32(255, 255, 255, 255), redTxt);
    } else if (targetInfo && targetInfo[0] != '\0' && proxyConnected) {
        float tgtX = x + boxW + S(8.0f);
        ImVec2 tgtSize = ImGui::CalcTextSize(targetInfo);
        float tgtW = tgtSize.x + pad * 2.0f;
        draw->AddRectFilled(ImVec2(tgtX, y), ImVec2(tgtX + tgtW, y + boxH), IM_COL32(20, 30, 45, 180), S(4.0f));
        draw->AddRect(ImVec2(tgtX, y), ImVec2(tgtX + tgtW, y + boxH), IM_COL32(60, 100, 150, 200), S(4.0f));
        draw->AddText(ImVec2(tgtX + pad, y + pad), IM_COL32(100, 180, 255, 255), targetInfo);
    }
}

}
