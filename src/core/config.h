#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef _WIN32
#include <Windows.h>
#else
constexpr int VK_INSERT = 0x2D;
constexpr int VK_RBUTTON = 0x02;
constexpr int VK_LBUTTON = 0x01;
constexpr int VK_MBUTTON = 0x04;
constexpr int VK_SHIFT = 0x10;
constexpr int VK_CONTROL = 0x11;
constexpr int VK_MENU = 0x12;
constexpr int VK_END = 0x23;
#endif
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

// TODO: align with target game item rarity values
enum class ItemRarity : int {
    NONE = 0,       // EDCItemRarity::None
    POOR = 1,       // EDCItemRarity::Poor (gray)
    COMMON = 2,     // EDCItemRarity::Common (white)
    UNCOMMON = 3,   // EDCItemRarity::Uncommon (green)
    RARE = 4,       // EDCItemRarity::Rare (blue)
    EPIC = 5,       // EDCItemRarity::Epic (purple)
    LEGENDARY = 6,  // EDCItemRarity::Legend (orange)
    UNIQUE = 7,     // EDCItemRarity::Unique (gold)
    ARTIFACT = 8,   // EDCItemRarity::Artifact (red)
    COUNT
};

struct FilterSettings {
    bool showPlayers = true;
    bool showLoot = true;
    bool showNPCs = true;
    bool showPortals = true;
    bool showCorpses = true;
    float maxDistance = 300.0f;        // General max distance (NPCs, loot, chests, etc.) — slider max
    float maxPlayerDistance = 1000.0f; // Separate player distance — slider max, see everyone on the map
    ItemRarity minLootRarity = ItemRarity::UNCOMMON;
    int minMonsterGrade = 0;          // 0=All, 1=Common+, 2=Elite+, 3=Nightmare only
    bool showHealthBars = true;
    bool showDamageNumbers = true;
    bool showDistance = true;
    bool showSnapLines = false;
    bool showGhostPlayers = true; // Show last-known-position markers for dormant players
};

// Legacy aim preferences retained for config compatibility. The external backend
// currently has no verified silent-aim implementation; these are inactive.
struct ProjectileRedirectSettings {
    bool enabled = true;            // Enabled by default
    uint64_t customBaseAddress = 0; // Manual payload (DLL) base in the game process; 0 = auto-scan
    int key = 0;                    // 0 = always active, no key hold required
    float fov = 15.0f;              // FOV in degrees for target selection
    float smoothness = 5.0f;        // Smoothing factor (higher = smoother)
    bool targetPlayers = true;      // Target players
    bool targetNPCs = false;        // Target NPCs/monsters
    bool visibleOnly = true;        // Only target visible entities
    int bone = 0;                   // 0=Head, 1=Chest, 2=Nearest
    float maxRedirectDist = 1000.0f; // Max distance from local player to redirect projectiles (UU)
    float maxTargetDistance = 100.0f; // Max distance to target (meters)
    bool requireKey = false;        // If true, requires key hold; if false, always active
    bool silentCamera = true;       // GetViewPoint restores natural camera rotation (recommended)
    float targetZOffset = 0.0f;     // Vertical offset applied to target Z (UE units; negative = aim lower)
    bool showFOVCircle = false;     // Draw the redirect FOV ring on the overlay
};

// Performance / Stealth settings
struct PerformanceSettings {
    bool lowFootprintMode = false;      // Reduces memory reads, increases cache usage
    int scanIntervalMs = 500;           // Actor scan interval (ms) - higher = less CPU/memory
    int positionUpdateIntervalMs = 16;  // Position update interval (ms) - ~60fps default
    bool batchMemoryReads = true;       // Batch reads where possible
    bool disableExpensiveESP = false;   // Disable bone ESP, health bars, etc.
    int maxEntitiesProcessed = 128;     // Max entities to process per frame
    bool reduceScanThreadPriority = true; // Lower scan thread priority
};

struct VisualSettings {
    float playerColor[4] = {1.0f, 0.2f, 0.2f, 1.0f};
    float teamColor[4] = {0.2f, 0.55f, 1.0f, 1.0f};
    float npcColor[4] = {1.0f, 1.0f, 0.2f, 1.0f};
    float fontSize = 15.0f;
    int boxStyle = 0; // 0=2D Box, 1=Filled Box, 2=Corner Box
    float radarRadius = 140.0f;      // On-screen radar radius in pixels
    float radarRangeMeters = 100.0f; // Radar world range
    bool minimapRotating = false;
};

struct AppConfig {
    FilterSettings filter;
    VisualSettings visuals;
    ProjectileRedirectSettings projectileRedirect;
    PerformanceSettings performance;
    int updateRateFps = 144;
    int toggleMenuKey = VK_INSERT;
    bool externalMode = true;   // Standalone: no DLL/proxy connection, silent aim disabled

    // Patterns — loaded from config so they can be updated without recompile
    std::string gworldPattern;
    std::string gnamesPattern;
    std::string gobjectsPattern;

    // Version-specific offsets must come from the selected settings file.
    uintptr_t gworldOffset = 0;
    uintptr_t gnamesOffset = 0;
    uintptr_t gobjectsOffset = 0;

    // UE version string from GSpots
    std::string ueVersion;
};

class Config {
public:
    explicit Config(const std::string& filepath);

    bool Load();
    void Save() const;
    bool IsLoaded() const { return m_loaded; }

    AppConfig& Get() { return m_config; }
    const AppConfig& Get() const { return m_config; }

private:
    std::string m_filepath;
    AppConfig m_config;
    bool m_loaded = false;
};
