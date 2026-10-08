#pragma once
#include "sdk/ue5_types.h"
#include "core/config.h"
#include <string>
#include <vector>

enum class EntityType {
    UNKNOWN,
    PLAYER,
    MONSTER,         // DCCharacterBase NPCs (skeletons, zombies, etc.)
    CHEST_NORMAL,    // BP_Chest_Base_C
    CHEST_SPECIAL,   // BP_ChestSpecial_Base_C (rare/golden chests)
    LOOT_ITEM,       // Dropped items / ground loot
    PORTAL,          // FloorPortalScrollBase (escape/down portals)
    INTERACTABLE     // Doors, shrines, etc.
};

// Monster difficulty grade (EDCMonsterGradeType from SDK)
enum class MonsterGrade : uint8_t {
    None      = 0,
    Common    = 1,
    Elite     = 2,
    Nightmare = 3
};

inline const char* MonsterGradeToString(MonsterGrade grade) {
    switch (grade) {
        case MonsterGrade::Common:    return "Common";
        case MonsterGrade::Elite:     return "Elite";
        case MonsterGrade::Nightmare: return "Nightmare";
        default:                      return "";
    }
}

inline uint32_t MonsterGradeToColor(MonsterGrade grade) {
    switch (grade) {
        case MonsterGrade::Elite:     return 0xFF00AAFF; // Orange
        case MonsterGrade::Nightmare: return 0xFF0000FF; // Red
        default:                      return 0xFFFFFFFF; // White
    }
}

struct GameEntity {
    uintptr_t address = 0;
    uintptr_t classPointer = 0;
    int32_t objectIndex = -1;
    int missedScans = 0;
    std::string className;
    std::string displayName;
    EntityType type = EntityType::UNKNOWN;

    // Spatial
    FVector position{};
    FVector headPosition{};  // For players: estimated head location
    FRotator rotation{};

    // Common
    float health = 0.0f;
    float maxHealth = 0.0f;
    bool isAlive = true;

    // Health delta from the latest background scan. Positive values are
    // healing; negative values are damage. Because health is sampled rather
    // than event-driven, fast consecutive changes may be combined.
    float healthChange = 0.0f;
    int healthChangeDisplayFrames = 0;

    // Loot-specific
    std::string itemName;
    ItemRarity rarity = ItemRarity::COMMON;
    std::string itemCategory; // Weapon, Armor, Consumable, etc.

    // Player-specific
    std::string playerClass; // Fighter, Barbarian, Rogue, Wizard, etc.
    std::string accountId;   // Replicated account ID, used for party matching
    std::string partyId;     // Replicated party ID, empty when solo
    std::vector<std::string> equipment;
    int32_t gearScore = 0;
    bool isLocalPlayer = false;
    bool isTeammate = false;
    bool isGhost = false;        // true when using last-known position (server stopped replicating)

    // Monster-specific
    MonsterGrade monsterGrade = MonsterGrade::None;

    // Death debounce: once entity is marked dead, it stays dead for N scan cycles
    // to prevent flickering during skeleton fake-death animations
    int deathDebounceFrames = 0;

    // Cached component pointers (don't change between frames, avoids re-reading)
    uintptr_t cachedRootComp = 0;
    uintptr_t cachedMeshComp = 0;
    uintptr_t cachedAttrSet = 0;   // Cached UDCAttributeSet* for fast HP reads in render thread
    int cachedHeadBoneIdx = -1;    // Best head bone index (found once, reused every frame)
    int cachedNeckBoneIdx = -1;    // Best neck bone index
    int cachedChestBoneIdx = -1;   // Best chest bone index

    // Bone positions (read per-frame for accurate boxes and aiming)
    FVector boneHead{};     // Head bone world position
    FVector boneNeck{};     // Neck bone world position
    FVector boneChest{};    // Chest bone world position
    FVector boneFeet{};     // Root/feet bone world position
    bool hasBoneData = false;

    // Position interpolation (smooths micro-lag between reads)
    FVector prevPosition{};
    FVector prevBoneHead{};
    FVector prevBoneFeet{};
    bool hasPrevPosition = false;

    // Rendering cache
    FVector2D screenPos{};
    FVector2D screenHead{};  // Projected head bone
    FVector2D screenNeck{};  // Projected neck bone
    FVector2D screenChest{}; // Projected chest bone
    FVector2D screenFeet{};  // Projected feet/root bone
    bool isOnScreen = false;
    bool headOnScreen = false;
    bool feetOnScreen = false;
    float distanceMeters = 0.0f;
};

// Rarity display helpers (aligned with EDCItemRarity from SDK)
inline const char* RarityToString(ItemRarity rarity) {
    switch (rarity) {
        case ItemRarity::NONE:      return "None";
        case ItemRarity::POOR:      return "Poor";
        case ItemRarity::COMMON:    return "Common";
        case ItemRarity::UNCOMMON:  return "Uncommon";
        case ItemRarity::RARE:      return "Rare";
        case ItemRarity::EPIC:      return "Epic";
        case ItemRarity::LEGENDARY: return "Legendary";
        case ItemRarity::UNIQUE:    return "Unique";
        case ItemRarity::ARTIFACT:  return "Artifact";
        default:                    return "Unknown";
    }
}

inline uint32_t RarityToColor(ItemRarity rarity) {
    // ImGui ABGR format (0xAABBGGRR)
    switch (rarity) {
        case ItemRarity::NONE:      return 0xFF606060; // Dark gray
        case ItemRarity::POOR:      return 0xFF808080; // Gray
        case ItemRarity::COMMON:    return 0xFFFFFFFF; // White
        case ItemRarity::UNCOMMON:  return 0xFF00FF00; // Green
        case ItemRarity::RARE:      return 0xFFFF8800; // Blue (BGR)
        case ItemRarity::EPIC:      return 0xFFFF00FF; // Purple
        case ItemRarity::LEGENDARY: return 0xFF00AAFF; // Orange (BGR)
        case ItemRarity::UNIQUE:    return 0xFF00D4FF; // Gold (BGR)
        case ItemRarity::ARTIFACT:  return 0xFF0000FF; // Red
        default:                    return 0xFFFFFFFF; // White
    }
}
