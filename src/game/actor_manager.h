#pragma once
#include "entity.h"
#include "sdk/gnames.h"
#include "sdk/gworld.h"
#include "core/config.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>


// Persistent cache entry for players whose transforms have gone dormant
// (server stopped replicating). Survives across scan cycles.
struct LastKnownPlayer {
    FVector position{};
    FVector headPosition{};
    std::string displayName;
    std::string playerClass;
    std::string accountId;
    uintptr_t classPointer = 0;
    int32_t objectIndex = -1;
    int staleScanCount = 0;
    bool isTeammate = false;
    static constexpr int MAX_STALE_SCANS = 120; // ~60 seconds at 2 Hz scan rate
};

class ActorManager {
public:
    void Update(const Process& proc, GWorldReader& world,
                GNamesReader& names, uintptr_t localPawn,
                uintptr_t gObjectsAddr,
                ItemRarity minLootRarity = ItemRarity::RARE);

    const std::vector<GameEntity>& GetEntities() const { return m_entities; }

    // Get filtered entities based on settings
    std::vector<const GameEntity*> GetFiltered(
        const FVector& localPos,
        const FilterSettings& filter) const;

    // Re-read just positions for filtered entities (cheap, call every frame for smooth ESP)
    void RefreshPositions(const Process& proc, const std::vector<const GameEntity*>& filtered);

private:
    void ClassifyActor(const Process& proc, uintptr_t actorAddr,
                       GNamesReader& names, GameEntity& out,
                       uintptr_t localPawn);

    void ReadActorPosition(const Process& proc, uintptr_t actorAddr,
                           GameEntity& out);

    void ReadPlayerData(const Process& proc, uintptr_t actorAddr,
                        GNamesReader& names, GameEntity& out);

    void ReadCharacterHealth(const Process& proc, uintptr_t actorAddr,
                             GNamesReader& names, GameEntity& out);

    void ReadItemData(const Process& proc, uintptr_t actorAddr,
                      GNamesReader& names, GameEntity& out);

    void ReadDeadBodyEquipment(const Process& proc, uintptr_t actorAddr,
                               GNamesReader& names, GameEntity& out,
                               ItemRarity minDisplayRarity = ItemRarity::RARE);

    void ReadMonsterData(const Process& proc, uintptr_t actorAddr,
                         GNamesReader& names, GameEntity& out);

    void ReadChestContents(const Process& proc, uintptr_t actorAddr,
                           GNamesReader& names, GameEntity& out,
                           ItemRarity minDisplayRarity = ItemRarity::RARE);

    // Process a classified entity: read type-specific data, apply portal dedup, death debounce.
    // Returns false if entity should be skipped (filtered out).
    enum class ProcessResult { KEEP, SKIP };
    ProcessResult ProcessEntity(const Process& proc, GNamesReader& names,
                                GameEntity& entity, ItemRarity minLootRarity,
                                std::unordered_set<uint64_t>& seenPortalKeys);

    static EntityType ClassifyByName(const std::string& className);
    static EntityType ClassifyCharacterHierarchy(const Process& proc,
                                                 GNamesReader& names,
                                                 uintptr_t classPtr);
    static ItemRarity ParseRarityFromTag(const std::string& tagStr);
    static ItemRarity GuessRarityFromName(const std::string& name);
    std::unordered_set<std::string> ReadPartyHudNames(
        const Process& proc, GNamesReader& names, uintptr_t gObjectsAddr);

    std::vector<GameEntity> m_entities;

    // Class pointer cache: classPtr -> {type, className}
    // Avoids re-resolving FName for every actor every frame
    // ~264 unique classes out of 26k actors = 99% cache hit rate
    struct ClassInfo {
        EntityType type = EntityType::UNKNOWN;
        std::string name;
    };
    std::unordered_map<uintptr_t, ClassInfo> m_classCache;

    // Negative actor cache.  The address alone is not a safe key: Unreal reuses
    // actor memory and a pawn can be replaced/transformed in-place.  Keep the
    // class pointer alongside the address and only skip when both are unchanged.
    std::unordered_map<uintptr_t, uintptr_t> m_knownUnknownActors;

    // Cached actor list — re-read from levels every N frames instead of every frame
    std::vector<uintptr_t> m_cachedActorPtrs;
    int m_actorListAge = 0;        // Frames since last full actor list refresh
    int m_lastLevelCount = 0;      // Detect map transitions
    // Update() runs at about 2 Hz, so 15 here delayed newly spawned enemies by
    // up to 7.5 seconds.  A one-second refresh keeps the pointer cache useful
    // without making the full level traversal run for every health update.
    static constexpr int ACTOR_LIST_REFRESH_FRAMES = 2;

    // Death debounce: tracks actor addresses that were recently marked dead.
    // Value = remaining scan cycles to stay dead. Prevents skeleton fake-death flickering.
    std::unordered_map<uintptr_t, int> m_deathDebounce;
    static constexpr int DEATH_DEBOUNCE_CYCLES = 4; // ~2 seconds at 2 Hz scan rate

    struct DamageSample {
        float health = -1.0f;
        float maxHealth = 0.0f;
    };
    std::unordered_map<uintptr_t, DamageSample> m_damageSamples;

    // Persistent last-known-position cache for players that went dormant.
    // Survives across scan rebuilds so ghosts persist until timeout.
    std::unordered_map<std::string, LastKnownPlayer> m_lastKnownPlayers;
    std::unordered_map<uintptr_t, std::string> m_actorToUniqueId;

    // UMG HUD widget cache. The party widget is allocated after the round HUD
    // exists, so discovery is retried at a low frequency until it appears.
    uintptr_t m_partyWidget = 0;
    int m_partyHudRefreshTicks = 0;
    int m_partyWidgetSearchCooldown = 0;
    // Class pointer -> nickname offset for supported party HUD variants;
    // zero means unsupported.
    std::unordered_map<uintptr_t, int32_t> m_partyWidgetClassCache;
    std::unordered_set<std::string> m_cachedPartyHudNames;
    int32_t m_partyWidgetNicknameOffset = 0x0428;

};
