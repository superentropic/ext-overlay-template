#include "actor_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <functional>
#include <cctype>
#include <cstring>
#include <cmath>

namespace {
// Unreal FString is UTF-16 in this Proton process. Party and account IDs are
// ASCII UUID-style strings, so non-ASCII code units are intentionally ignored.
std::string ReadUtf16String(const Process& proc, uintptr_t address, int maxChars = 128) {
    const FString value = proc.Read<FString>(address);
    if (!value.Data || value.Count <= 0 || value.Count > maxChars)
        return {};

    std::vector<char16_t> buffer(value.Count);
    if (!proc.ReadRaw(value.Data, buffer.data(), buffer.size() * sizeof(char16_t)))
        return {};

    std::string result;
    result.reserve(value.Count);
    for (char16_t ch : buffer) {
        if (ch == 0) break;
        if (ch < 128) result += static_cast<char>(ch);
    }
    return result;
}

std::string NormalizePlayerName(std::string value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (!std::isspace(ch))
            normalized += static_cast<char>(std::tolower(ch));
    }
    return normalized;
}

bool IsUserPointer(uintptr_t value) {
    return value >= 0x10000 && value <= 0x00007FFFFFFFFFFF;
}

std::string ReadCharacterPartyId(const Process& proc, uintptr_t actorAddr) {
    std::string partyId = ReadUtf16String(proc,
        actorAddr + UEOffsets::DCChar_AccountDataReplication + UEOffsets::AcctData_PartyId);
    if (partyId.empty()) {
        partyId = ReadUtf16String(proc,
            actorAddr + UEOffsets::DCChar_AccountDataReplicationCached + UEOffsets::AcctData_PartyId);
    }
    return partyId;
}

struct ChunkedObjectArrayHeader {
    uintptr_t chunks;
    int32_t numElements;
    int32_t maxElements;
    int32_t numChunks;
    int32_t maxChunks;
};

// FUObjectItem is 0x18 on this UE5 build; its UObject* is at +0x08.
static constexpr size_t UObjectItemSize = 0x18;
static constexpr size_t UObjectItemObjectOffset = 0x08;
static constexpr int32_t GamePartyWidget_MemberWidgets = 0x03D0;
static constexpr int32_t GamePartyMemberWidget_Nickname = 0x0428;
static constexpr int32_t FaustianPartyMemberWidget_Nickname = 0x03D8;
static constexpr int32_t PlayerController_OwnPartyData = 0x0C90;

// FPartyData (0x20): FString PartyId at +0x0, TArray<FPartyMemberData> at +0x10.
// FPartyMemberData is one FString account ID (0x10).
std::unordered_set<std::string> ReadLocalPartyAccounts(
    const Process& proc, const GWorldReader& world, const std::string& localAccountId) {
    std::unordered_set<std::string> accounts;
    if (localAccountId.empty()) return accounts;

    const uintptr_t uWorld = world.GetUWorld(proc);
    const uintptr_t gameState = uWorld
        ? proc.Read<uintptr_t>(uWorld + UEOffsets::UWorld_GameState)
        : 0;
    if (!gameState) return accounts;

    const TArray<uint8_t> parties = proc.Read<TArray<uint8_t>>(
        gameState + UEOffsets::GameState_PartyDataArray);
    constexpr size_t PARTY_DATA_SIZE = 0x20;
    if (!parties.Data || parties.Count <= 0 || parties.Count > 128 ||
        parties.Max < parties.Count) return accounts;

    for (int i = 0; i < parties.Count; ++i) {
        const uintptr_t party = parties.Data + static_cast<size_t>(i) * PARTY_DATA_SIZE;
        const TArray<uint8_t> members = proc.Read<TArray<uint8_t>>(party + 0x10);
        if (!members.Data || members.Count <= 0 || members.Count > 8 ||
            members.Max < members.Count) continue;

        bool containsLocalPlayer = false;
        std::vector<std::string> partyAccounts;
        partyAccounts.reserve(members.Count);
        for (int j = 0; j < members.Count; ++j) {
            const std::string account = ReadUtf16String(
                proc, members.Data + static_cast<size_t>(j) * sizeof(FString));
            if (account == localAccountId) containsLocalPlayer = true;
            if (!account.empty()) partyAccounts.push_back(account);
        }
        if (containsLocalPlayer) {
            accounts.insert(partyAccounts.begin(), partyAccounts.end());
            return accounts;
        }
    }
    return accounts;
}

std::unordered_set<std::string> ReadOwnPartyAccounts(
    const Process& proc, const GWorldReader& world, const std::string& localAccountId) {
    std::unordered_set<std::string> accounts;
    if (localAccountId.empty()) return accounts;

    const uintptr_t controller = world.GetLocalPlayerController(proc);
    if (!controller) return accounts;

    // FPartyData: PartyId FString at +0x00, member TArray at +0x10.
    const TArray<uint8_t> members = proc.Read<TArray<uint8_t>>(
        controller + PlayerController_OwnPartyData + 0x10);
    if (!members.Data || members.Count <= 0 || members.Count > 8 ||
        members.Max < members.Count) return accounts;

    bool containsLocalPlayer = false;
    std::vector<std::string> partyAccounts;
    partyAccounts.reserve(members.Count);
    for (int i = 0; i < members.Count; ++i) {
        const std::string account = ReadUtf16String(
            proc, members.Data + static_cast<size_t>(i) * sizeof(FString));
        if (account == localAccountId) containsLocalPlayer = true;
        if (!account.empty()) partyAccounts.push_back(account);
    }

    if (containsLocalPlayer)
        accounts.insert(partyAccounts.begin(), partyAccounts.end());
    return accounts;
}
} // namespace

// ============================================================================
// ProcessEntity: shared logic for both cache-hit and cache-miss paths.
// Reads type-specific data, applies portal dedup, death debounce.
// Returns SKIP if entity should be filtered out.
// ============================================================================
ActorManager::ProcessResult ActorManager::ProcessEntity(
    const Process& proc, GNamesReader& names,
    GameEntity& entity, ItemRarity minLootRarity,
    std::unordered_set<uint64_t>& seenPortalKeys)
{
    // Health change is derived from the authoritative health read below. Keep
    // this state across the per-scan m_entities rebuild so the overlay can
    // show a short-lived damage/healing number without an event hook.
    auto updateDamage = [&entity, this]() {
        entity.healthChange = 0.0f;
        entity.healthChangeDisplayFrames = 0;
        if ((entity.type != EntityType::PLAYER && entity.type != EntityType::MONSTER) ||
            entity.health < 0.0f || entity.maxHealth <= 0.0f ||
            !std::isfinite(entity.health) || !std::isfinite(entity.maxHealth)) {
            return;
        }

        auto [sampleIt, inserted] = m_damageSamples.emplace(
            entity.address, DamageSample{entity.health, entity.maxHealth});
        if (!inserted) {
            const float change = entity.health - sampleIt->second.health;
            const float maxValidDamage = (std::max)(sampleIt->second.maxHealth, entity.maxHealth);
            // Capture both damage and healing. Ignore implausible values
            // caused by stale/reused actor memory, then advance the baseline.
            if (std::abs(change) > 0.01f && std::abs(change) <= maxValidDamage * 1.25f) {
                entity.healthChange = change;
                entity.healthChangeDisplayFrames = 3; // ~1.5 s at normal 2 Hz
            }
            sampleIt->second = {entity.health, entity.maxHealth};
        }
    };

    switch (entity.type) {
        case EntityType::PLAYER:
            ReadPlayerData(proc, entity.address, names, entity);
            updateDamage();
            entity.headPosition = entity.position;
            entity.headPosition.Z += 170.0;
            if (!entity.isAlive && !entity.isLocalPlayer) {
                ReadDeadBodyEquipment(proc, entity.address, names, entity, minLootRarity);
            }
            break;
        case EntityType::MONSTER:
            ReadMonsterData(proc, entity.address, names, entity);
            updateDamage();
            if (!entity.isAlive) {
                ReadDeadBodyEquipment(proc, entity.address, names, entity, minLootRarity);
            }
            break;
        case EntityType::CHEST_SPECIAL:
            entity.displayName = "RARE CHEST";
            ReadChestContents(proc, entity.address, names, entity, minLootRarity);
            break;
        case EntityType::CHEST_NORMAL:
            entity.displayName = "Chest";
            ReadChestContents(proc, entity.address, names, entity, minLootRarity);
            break;
        case EntityType::LOOT_ITEM:
            ReadItemData(proc, entity.address, names, entity);
            if (entity.type == EntityType::UNKNOWN) return ProcessResult::SKIP;
            break;
        case EntityType::PORTAL: {
            // Portal deduplication: pack position into 64-bit key (50 UU grid)
            int px = static_cast<int>(entity.position.X / 50.0);
            int py = static_cast<int>(entity.position.Y / 50.0);
            int pz = static_cast<int>(entity.position.Z / 50.0);
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(px)) << 32)
                         | (static_cast<uint64_t>(static_cast<uint16_t>(py)) << 16)
                         | static_cast<uint64_t>(static_cast<uint16_t>(pz));
            if (seenPortalKeys.count(key)) return ProcessResult::SKIP;
            seenPortalKeys.insert(key);

            // Descriptive portal names
            if (entity.className.find("Escape") != std::string::npos ||
                entity.className.find("Extraction") != std::string::npos ||
                entity.className.find("StairEscape") != std::string::npos)
                entity.displayName = "EXIT PORTAL";
            else if (entity.className.find("Down") != std::string::npos)
                entity.displayName = "Down Portal";
            else if (entity.className.find("Up") != std::string::npos)
                entity.displayName = "Up Portal";
            else
                entity.displayName = "Portal";
            break;
        }
        default:
            entity.displayName = entity.className;
            break;
    }

    // Death debounce: prevent skeleton fake-death flickering
    if (entity.type == EntityType::MONSTER) {
        auto debounceIt = m_deathDebounce.find(entity.address);
        if (!entity.isAlive && entity.health >= 0.0f && entity.health <= 0.0f) {
            m_deathDebounce[entity.address] = DEATH_DEBOUNCE_CYCLES;
        } else if (debounceIt != m_deathDebounce.end() && entity.health >= 0.0f && entity.health <= 0.0f) {
            entity.isAlive = false;
        } else if (debounceIt != m_deathDebounce.end() && entity.health > 0.0f) {
            m_deathDebounce.erase(debounceIt);
        }
    }

    return ProcessResult::KEEP;
}

void ActorManager::Update(const Process& proc, GWorldReader& world,
                          GNamesReader& names, uintptr_t localPawn,
                          uintptr_t gObjectsAddr,
                          ItemRarity minLootRarity) {
    m_entities.clear();
    static int diagCounter = 0;
    bool doDiag = (diagCounter++ % 900 == 0); // Periodic diagnostics every ~30s (production)

    // Tick down death debounce counters
    for (auto it = m_deathDebounce.begin(); it != m_deathDebounce.end(); ) {
        it->second--;
        if (it->second <= 0)
            it = m_deathDebounce.erase(it);
        else
            ++it;
    }

    // =========================================================================
    // OPTIMIZATION 1: Cache the actor pointer list, refresh every N frames
    // =========================================================================
    m_actorListAge++;
    bool needRefresh = m_actorListAge >= ACTOR_LIST_REFRESH_FRAMES || m_cachedActorPtrs.empty();

    if (needRefresh) {
        auto newActorPtrs = world.GetAllActors(proc);

        // An UNKNOWN class can be a transient read failure while an actor is
        // spawning or while its Blueprint class is being initialized.  Do not
        // let that negative class result survive forever; known entity classes
        // remain cached and cheap to resolve.
        for (auto it = m_classCache.begin(); it != m_classCache.end(); ) {
            if (it->second.type == EntityType::UNKNOWN)
                it = m_classCache.erase(it);
            else
                ++it;
        }

        // Negative address entries are only a short-lived optimization.  A
        // new scan must be allowed to retry actors that were unreadable or
        // unsupported while spawning, despawning, or changing form.
        m_knownUnknownActors.clear();

        int levelCount = static_cast<int>(newActorPtrs.size());
        if (m_lastLevelCount > 0 && std::abs(levelCount - m_lastLevelCount) > m_lastLevelCount / 2) {
            m_knownUnknownActors.clear();
            m_deathDebounce.clear();
            m_damageSamples.clear();
            spdlog::info("[ActorManager] Map transition detected ({} -> {} actors), clearing caches",
                m_lastLevelCount, levelCount);
        }
        m_lastLevelCount = levelCount;
        m_cachedActorPtrs = std::move(newActorPtrs);
        m_actorListAge = 0;
    }

    const auto& actorPtrs = m_cachedActorPtrs;

    if (doDiag) {
        spdlog::info("[ActorManager] {} raw actors, localPawn=0x{:X}, classCache={}, addrCache={}",
            actorPtrs.size(), localPawn, m_classCache.size(), m_knownUnknownActors.size());
    }

    m_entities.reserve(256);
    std::unordered_set<uint64_t> seenPortalKeys;

    std::string localAccountId = localPawn
        ? ReadUtf16String(proc, localPawn + UEOffsets::DCChar_AccountId)
        : std::string{};
    // In dungeon instances AccountId is sometimes only populated through the
    // replicated account-data struct, not the direct character field.
    if (localAccountId.empty() && localPawn) {
        localAccountId = ReadUtf16String(proc,
            localPawn + UEOffsets::DCChar_AccountDataReplication + UEOffsets::AcctData_AccountId);
    }
    auto localPartyAccounts = ReadLocalPartyAccounts(proc, world, localAccountId);
    if (localPartyAccounts.empty()) {
        localPartyAccounts = ReadOwnPartyAccounts(proc, world, localAccountId);
    }
    const auto partyHudNames = ReadPartyHudNames(proc, names, gObjectsAddr);
    const std::string localPartyId = localPawn ? ReadCharacterPartyId(proc, localPawn) : std::string{};
    // Read this for diagnostics only. GenericTeamId is a broad gameplay
    // faction value in some modes and is not reliable enough to identify
    // party members by itself.
    constexpr uint8_t NO_TEAM = 255;
    const uint8_t localTeamId = localPawn
        ? proc.Read<uint8_t>(localPawn + UEOffsets::DCChar_GenericTeamId)
        : NO_TEAM;
    if (doDiag) {
        spdlog::info("[ActorManager] local account available={}, teamId={}, partyId available={}, replicated party members={}, HUD party names={}",
            !localAccountId.empty(), localTeamId, !localPartyId.empty(), localPartyAccounts.size(), partyHudNames.size());
    }

    int skippedByAddrCache = 0, cacheHits = 0, cacheMisses = 0, classified = 0;
    constexpr int MAX_CACHE_MISSES_PER_FRAME = 500;
    std::unordered_set<std::string> seenPlayers;

    for (uintptr_t actorAddr : actorPtrs) {
        // Step 1: Read classPtr
        uintptr_t classPtr = proc.Read<uintptr_t>(actorAddr + UEOffsets::UObject_Class);
        if (!classPtr || classPtr > 0x00007FFFFFFFFFFF) {
            // Do not permanently negative-cache an invalid read.  During
            // spawning/destruction this field is briefly null and the same
            // address may become a valid enemy on the next scan.
            continue;
        }

        // OPTIMIZATION 2: Skip a known-unknown actor only while its class
        // pointer is unchanged.  Actor addresses are routinely recycled.
        auto unknownIt = m_knownUnknownActors.find(actorAddr);
        if (unknownIt != m_knownUnknownActors.end() && unknownIt->second == classPtr) {
            skippedByAddrCache++;
            continue;
        }

        // Step 2: Resolve class name + type (from cache or fresh)
        EntityType type = EntityType::UNKNOWN;
        std::string cname;

        auto cacheIt = m_classCache.find(classPtr);
        if (cacheIt != m_classCache.end()) {
            cacheHits++;
            type = cacheIt->second.type;
            cname = cacheIt->second.name;
        } else {
            // Cache miss — resolve FName
            if (cacheMisses >= MAX_CACHE_MISSES_PER_FRAME) continue;
            cacheMisses++;

            FName className = proc.Read<FName>(classPtr + UEOffsets::UObject_Name);
            cname = names.GetFName(proc, className.ComparisonIndex, className.Number);
            type = ClassifyCharacterHierarchy(proc, names, classPtr);
            if (type == EntityType::UNKNOWN && !cname.empty())
                type = ClassifyByName(cname);
            m_classCache[classPtr] = {type, cname};

            if (type != EntityType::UNKNOWN) {
                spdlog::info("[ActorManager] NEW class: '{}' -> {} (0x{:X})",
                    cname, static_cast<int>(type), classPtr);
            }
        }

        // Skip UNKNOWN types
        if (type == EntityType::UNKNOWN) {
            m_knownUnknownActors[actorAddr] = classPtr;
            // Diagnostic for druid shapeshift discovery
            if (!cname.empty() && (cname.find("Bear") != std::string::npos ||
                cname.find("Druid") != std::string::npos ||
                cname.find("Panther") != std::string::npos ||
                cname.find("Shift") != std::string::npos)) {
                spdlog::info("[ClassifyDiag] UNKNOWN with animal/druid keyword: '{}'", cname);
            }
            continue;
        }

        // The actor has become a supported class again (for example after a
        // class swap/respawn at a reused address).
        m_knownUnknownActors.erase(actorAddr);

        // Step 3: Build entity and read position
        GameEntity entity;
        entity.address = actorAddr;
        entity.classPointer = classPtr;
        entity.objectIndex = proc.Read<int32_t>(actorAddr + UEOffsets::UObject_Index);
        entity.className = cname;
        entity.type = type;
        entity.isLocalPlayer = (actorAddr == localPawn);

        ReadActorPosition(proc, actorAddr, entity);
        bool positionIsInvalid =
            (!std::isfinite(entity.position.X) || !std::isfinite(entity.position.Y) ||
             !std::isfinite(entity.position.Z) || std::abs(entity.position.Z) > 100000.0 ||
             (entity.position.X == 0.0 && entity.position.Y == 0.0 && entity.position.Z == 0.0));

        if (entity.type == EntityType::PLAYER && positionIsInvalid) {
            // Player has a dormant/zeroed transform - use persistent last-known position
            std::string uniqueId;
            auto idIt = m_actorToUniqueId.find(actorAddr);
            if (idIt != m_actorToUniqueId.end()) {
                uniqueId = idIt->second;
            } else {
                uniqueId = std::to_string(actorAddr);
            }

            auto lkIt = m_lastKnownPlayers.find(uniqueId);
            if (lkIt != m_lastKnownPlayers.end() &&
                lkIt->second.classPointer == classPtr &&
                lkIt->second.objectIndex == entity.objectIndex &&
                lkIt->second.staleScanCount < LastKnownPlayer::MAX_STALE_SCANS) {
                entity.position = lkIt->second.position;
                entity.headPosition = lkIt->second.headPosition;
                entity.displayName = lkIt->second.displayName;
                entity.playerClass = lkIt->second.playerClass;
                entity.accountId = lkIt->second.accountId;
                entity.isTeammate = lkIt->second.isTeammate;
                entity.isGhost = true;
                lkIt->second.staleScanCount++;
                
                // Track this ghost by ID so we don't accidentally double-add below
                seenPlayers.insert(uniqueId);
            } else {
                // No cached position or expired - skip this entity
                if (lkIt != m_lastKnownPlayers.end())
                    m_lastKnownPlayers.erase(lkIt);
                m_actorToUniqueId.erase(actorAddr);
                continue;
            }
        } else if (positionIsInvalid) {
            // Non-player with invalid position - skip
            continue;
        } else if (entity.type == EntityType::PLAYER) {
            // Player with valid position - not a ghost
            entity.isGhost = false;
        }

        // Step 4: Read type-specific data via shared helper
        if (ProcessEntity(proc, names, entity, minLootRarity, seenPortalKeys) == ProcessResult::SKIP)
            continue;

        if (entity.type == EntityType::PLAYER) {
            if (!entity.isGhost) {
                const std::string normalizedName = NormalizePlayerName(entity.displayName);
                entity.isTeammate = !entity.isLocalPlayer &&
                    ((!localPartyId.empty() && entity.partyId == localPartyId) ||
                     (!entity.accountId.empty() && localPartyAccounts.contains(entity.accountId)) ||
                     (!normalizedName.empty() && partyHudNames.contains(normalizedName)));
                if (doDiag && !entity.isLocalPlayer) {
                    spdlog::info("[Team] '{}' partyId={} sameParty={} inPartyAccounts={} inHud={} -> teammate={}",
                        entity.displayName, entity.partyId.empty() ? "<none>" : entity.partyId,
                        !localPartyId.empty() && entity.partyId == localPartyId,
                        !entity.accountId.empty() && localPartyAccounts.contains(entity.accountId),
                        !normalizedName.empty() && partyHudNames.contains(normalizedName),
                        entity.isTeammate);
                }
            }

            // Update persistent last-known-position cache for live (non-ghost) players
            if (!entity.isGhost && !entity.isLocalPlayer) {
                std::string uniqueId = !entity.accountId.empty() ? entity.accountId : (!entity.displayName.empty() ? entity.displayName : std::to_string(actorAddr));
                m_actorToUniqueId[actorAddr] = uniqueId;

                if (entity.isAlive) {
                    auto& lk = m_lastKnownPlayers[uniqueId];
                    lk.position = entity.position;
                    lk.headPosition = entity.headPosition;
                    lk.displayName = entity.displayName;
                    lk.playerClass = entity.playerClass;
                    lk.accountId = entity.accountId;
                    lk.classPointer = entity.classPointer;
                    lk.objectIndex = entity.objectIndex;
                    lk.isTeammate = entity.isTeammate;
                    lk.staleScanCount = 0;
                } else {
                    m_lastKnownPlayers.erase(uniqueId);
                }
                seenPlayers.insert(uniqueId);
            }
        }

        m_entities.push_back(std::move(entity));
        classified++;
    }

    // Process players that were completely culled by network relevancy (missing from actorPtrs)
    for (auto it = m_lastKnownPlayers.begin(); it != m_lastKnownPlayers.end(); ) {
        if (seenPlayers.find(it->first) == seenPlayers.end()) {
            it->second.staleScanCount++;
            
            if (it->second.staleScanCount < LastKnownPlayer::MAX_STALE_SCANS) {
                GameEntity entity;
                // Since we no longer use the actor address as key, we pass 0 or a dummy value.
                // We shouldn't need the address to render the ghost anyway.
                entity.address = 0;
                entity.classPointer = it->second.classPointer;
                entity.objectIndex = it->second.objectIndex;
                entity.type = EntityType::PLAYER;
                entity.isGhost = true;
                entity.position = it->second.position;
                entity.headPosition = it->second.headPosition;
                entity.displayName = it->second.displayName;
                entity.playerClass = it->second.playerClass;
                entity.accountId = it->second.accountId;
                entity.isTeammate = it->second.isTeammate;
                
                m_entities.push_back(std::move(entity));
            }
        }
        
        // Clean expired ghost entries from the persistent cache
        if (it->second.staleScanCount >= LastKnownPlayer::MAX_STALE_SCANS) {
            it = m_lastKnownPlayers.erase(it);
        } else {
            ++it;
        }
    }

    if (doDiag) {
        spdlog::info("[ActorManager] {} actors -> {} entities | skip={}, hit={}, miss={}",
            actorPtrs.size(), m_entities.size(), skippedByAddrCache, cacheHits, cacheMisses);

        int players = 0, monsters = 0, chests = 0, loot = 0;
        for (const auto& e : m_entities) {
            switch (e.type) {
                case EntityType::PLAYER: players++; break;
                case EntityType::MONSTER: monsters++; break;
                case EntityType::CHEST_SPECIAL:
                case EntityType::CHEST_NORMAL: chests++; break;
                case EntityType::LOOT_ITEM: loot++; break;
                default: break;
            }
        }
        spdlog::info("[ActorManager] Breakdown: {} players, {} monsters, {} chests, {} loot",
            players, monsters, chests, loot);
    }
}

std::unordered_set<std::string> ActorManager::ReadPartyHudNames(
    const Process& proc, GNamesReader& names, uintptr_t gObjectsAddr) {
    // The widget's member array changes when a party member joins/leaves, so
    // refresh it every ~5 seconds (the actor scanner runs at about 2 Hz).
    if (m_partyWidget && (++m_partyHudRefreshTicks % 10) != 0)
        return m_cachedPartyHudNames;

    if (!m_partyWidget) {
        if (m_partyWidgetSearchCooldown-- > 0)
            return m_cachedPartyHudNames;
        m_partyWidgetSearchCooldown = 60; // Retry discovery after ~30 seconds.

        const ChunkedObjectArrayHeader objects = proc.Read<ChunkedObjectArrayHeader>(gObjectsAddr);
        if (!IsUserPointer(objects.chunks) || objects.numElements <= 0 ||
            objects.numElements > 1'000'000 || objects.numChunks <= 0 ||
            objects.numChunks > 32 || objects.maxChunks < objects.numChunks ||
            objects.maxElements < objects.numElements) {
            spdlog::warn("[PartyHUD] GObjects header is invalid; HUD-name matching unavailable");
            return m_cachedPartyHudNames;
        }

        const int32_t perChunk = objects.maxElements / objects.maxChunks;
        if (perChunk < 0x8000 || perChunk > 0x80000) {
            spdlog::warn("[PartyHUD] unsupported GObjects chunk size {}", perChunk);
            return m_cachedPartyHudNames;
        }

        std::vector<uintptr_t> chunks(objects.numChunks);
        if (!proc.ReadRaw(objects.chunks, chunks.data(), chunks.size() * sizeof(uintptr_t)))
            return m_cachedPartyHudNames;

        for (int32_t chunkIndex = 0; chunkIndex < objects.numChunks && !m_partyWidget; ++chunkIndex) {
            const uintptr_t chunk = chunks[chunkIndex];
            if (!IsUserPointer(chunk)) continue;

            const int32_t firstIndex = chunkIndex * perChunk;
            const int32_t count = std::min(perChunk, objects.numElements - firstIndex);
            if (count <= 0) break;

            std::vector<uint8_t> items(static_cast<size_t>(count) * UObjectItemSize);
            if (!proc.ReadRaw(chunk, items.data(), items.size())) continue;

            for (int32_t i = 0; i < count; ++i) {
                uintptr_t object = 0;
                std::memcpy(&object, items.data() + static_cast<size_t>(i) * UObjectItemSize
                                + UObjectItemObjectOffset,
                            sizeof(object));
                if (!IsUserPointer(object)) continue;

                const uintptr_t classPtr = proc.Read<uintptr_t>(object + UEOffsets::UObject_Class);
                if (!IsUserPointer(classPtr)) continue;

                auto cache = m_partyWidgetClassCache.find(classPtr);
                int32_t nicknameOffset = 0;
                if (cache != m_partyWidgetClassCache.end()) {
                    nicknameOffset = cache->second;
                } else {
                    const FName className = proc.Read<FName>(classPtr + UEOffsets::UObject_Name);
                    const std::string resolvedName = names.GetFName(
                        proc, className.ComparisonIndex, className.Number);
                    if (resolvedName == "GamePartyWidget") {
                        nicknameOffset = GamePartyMemberWidget_Nickname;
                    } else if (resolvedName == "FaustianBargainGamePartyWidget") {
                        nicknameOffset = FaustianPartyMemberWidget_Nickname;
                    }
                    m_partyWidgetClassCache.emplace(classPtr, nicknameOffset);
                }

                if (nicknameOffset != 0) {
                    // GObjects also contains class default objects (CDOs).
                    // They have the right class but an empty PartyMemberWidgets
                    // array, so keep searching for a live HUD instance.
                    const TArray<uintptr_t> candidateMembers = proc.Read<TArray<uintptr_t>>(
                        object + GamePartyWidget_MemberWidgets);
                    if (!candidateMembers.Data || candidateMembers.Count <= 0 ||
                        candidateMembers.Count > 8 || candidateMembers.Max < candidateMembers.Count) {
                        continue;
                    }
                    m_partyWidget = object;
                    m_partyWidgetNicknameOffset = nicknameOffset;
                    spdlog::info("[PartyHUD] party widget found at 0x{:X}, nickname offset=0x{:X}",
                        m_partyWidget, m_partyWidgetNicknameOffset);
                    break;
                }
            }
        }
    }

    if (!m_partyWidget) return m_cachedPartyHudNames;

    const TArray<uintptr_t> members = proc.Read<TArray<uintptr_t>>(
        m_partyWidget + GamePartyWidget_MemberWidgets);
    if (!members.Data || members.Count <= 0 || members.Count > 8 || members.Max < members.Count) {
        // The HUD is rebuilt between rounds. Forget a stale widget and retry
        // discovery later instead of retaining old party identities.
        m_partyWidget = 0;
        m_partyWidgetNicknameOffset = GamePartyMemberWidget_Nickname;
        m_cachedPartyHudNames.clear();
        return m_cachedPartyHudNames;
    }

    std::vector<uintptr_t> widgets(members.Count);
    if (!proc.ReadRaw(members.Data, widgets.data(), widgets.size() * sizeof(uintptr_t)))
        return m_cachedPartyHudNames;

    std::unordered_set<std::string> discovered;
    for (const uintptr_t widget : widgets) {
        if (!IsUserPointer(widget)) continue;
        const std::string nickname = ReadUtf16String(
            proc, widget + m_partyWidgetNicknameOffset);
        const std::string normalized = NormalizePlayerName(nickname);
        if (!normalized.empty()) discovered.insert(normalized);
    }

    if (!discovered.empty()) m_cachedPartyHudNames = std::move(discovered);
    return m_cachedPartyHudNames;
}

std::vector<const GameEntity*> ActorManager::GetFiltered(
    const FVector& localPos,
    const FilterSettings& filter) const
{
    std::vector<const GameEntity*> result;

    for (const auto& entity : m_entities) {
        if (entity.isLocalPlayer)
            continue;

        // Corpse filter
        if (!filter.showCorpses && !entity.isAlive)
            continue;

        // Type filter
        switch (entity.type) {
            case EntityType::PLAYER:
                if (!filter.showPlayers) continue;
                break;
            case EntityType::MONSTER:
                if (!filter.showNPCs) continue;
                // Monster grade filter: 0=All, 1=Common+, 2=Elite+, 3=Nightmare
                if (filter.minMonsterGrade > 0 &&
                    static_cast<int>(entity.monsterGrade) < filter.minMonsterGrade)
                    continue;
                break;
            case EntityType::CHEST_NORMAL:
            case EntityType::CHEST_SPECIAL:
                break;
            case EntityType::LOOT_ITEM:
                if (!filter.showLoot) continue;
                break;
            case EntityType::PORTAL:
                if (!filter.showPortals) continue;
                break;
            case EntityType::INTERACTABLE:
                break;
            default:
                continue;
        }

        // Distance filter — players get their own (larger) distance
        float dist = static_cast<float>(entity.position.DistanceToMeters(localPos));
        float maxDist = (entity.type == EntityType::PLAYER)
            ? filter.maxPlayerDistance
            : filter.maxDistance;
        if (dist > maxDist)
            continue;

        // Rarity filter for loot
        if (entity.type == EntityType::LOOT_ITEM &&
            entity.rarity < filter.minLootRarity)
            continue;

        result.push_back(&entity);
    }

    return result;
}

void ActorManager::RefreshPositions(const Process& proc,
                                     const std::vector<const GameEntity*>& filtered) {
    for (const auto* entityPtr : filtered) {
        auto& entity = const_cast<GameEntity&>(*entityPtr);

        uintptr_t rootComp = proc.Read<uintptr_t>(entity.address + UEOffsets::AActor_RootComponent);
        if (!rootComp) continue;

        FVector newPos = proc.Read<FVector>(
            rootComp + UEOffsets::SceneComp_ComponentToWorld + UEOffsets::FTransform_Translation);

        // Fallback if C2W returns zeros
        if (newPos.X == 0.0 && newPos.Y == 0.0 && newPos.Z == 0.0) {
            newPos = proc.Read<FVector>(rootComp + UEOffsets::SceneComp_RelativeLocation);
        }

        // Only update if we got a valid position (skip inventory items)
        if (std::abs(newPos.Z) < 100000.0) {
            entity.position = newPos;
            if (entity.type == EntityType::PLAYER) {
                entity.headPosition = newPos;
                entity.headPosition.Z += 170.0;
            }
        }
    }
}

void ActorManager::ClassifyActor(const Process& proc, uintptr_t actorAddr,
                                  GNamesReader& names, GameEntity& out,
                                  uintptr_t localPawn) {
    // Read UObject::Class pointer
    uintptr_t classPtr = proc.Read<uintptr_t>(actorAddr + UEOffsets::UObject_Class);
    if (!classPtr) return;

    // Read class FName
    FName className = proc.Read<FName>(classPtr + UEOffsets::UObject_Name);
    out.className = names.GetFName(proc, className.ComparisonIndex, className.Number);

    if (out.className.empty()) return;

    // Classify by class name patterns (using real DaD class names from SDK)
    out.type = ClassifyByName(out.className);

    if (out.type == EntityType::UNKNOWN) return;

    // Read position for all classified actors
    ReadActorPosition(proc, actorAddr, out);

    // Check if this is the local player
    out.isLocalPlayer = (actorAddr == localPawn);

    // Read type-specific data
    switch (out.type) {
        case EntityType::PLAYER:
            ReadPlayerData(proc, actorAddr, names, out);
            out.headPosition = out.position;
            out.headPosition.Z += 170.0; // ~170cm standing height
            break;

        case EntityType::MONSTER:
            ReadCharacterHealth(proc, actorAddr, names, out);
            out.displayName = out.className;
            break;

        case EntityType::CHEST_SPECIAL:
            out.displayName = "RARE CHEST";
            break;

        case EntityType::CHEST_NORMAL:
            out.displayName = "Chest";
            break;

        case EntityType::LOOT_ITEM:
            ReadItemData(proc, actorAddr, names, out);
            break;

        case EntityType::PORTAL:
            out.displayName = "Portal";
            break;

        default:
            out.displayName = out.className;
            break;
    }
}

void ActorManager::ReadActorPosition(const Process& proc, uintptr_t actorAddr,
                                      GameEntity& out) {
    uintptr_t rootComp = proc.Read<uintptr_t>(actorAddr + UEOffsets::AActor_RootComponent);
    if (!rootComp) return;

    // Read world position from ComponentToWorld FTransform.
    // This is the RESOLVED world-space transform — handles parented components and level offsets.
    // RelativeLocation only gives position relative to parent — Z=0 for many ground items.
    //
    // FTransform::Translation is at +0x20 in this SDK. Live unattached root
    // components also place their world translation at root+0x210.
    out.position = proc.Read<FVector>(
        rootComp + UEOffsets::SceneComp_ComponentToWorld + UEOffsets::FTransform_Translation);

    // Fallback: if ComponentToWorld gave all zeros, try RelativeLocation
    if (out.position.X == 0.0 && out.position.Y == 0.0 && out.position.Z == 0.0) {
        out.position = proc.Read<FVector>(rootComp + UEOffsets::SceneComp_RelativeLocation);
    }

    out.rotation = proc.Read<FRotator>(rootComp + UEOffsets::SceneComp_RelativeRotation);
}

void ActorManager::ReadPlayerData(const Process& proc, uintptr_t actorAddr,
                                   GNamesReader& names, GameEntity& out) {
    out.accountId = ReadUtf16String(proc, actorAddr + UEOffsets::DCChar_AccountId);
    if (out.accountId.empty()) {
        out.accountId = ReadUtf16String(proc,
            actorAddr + UEOffsets::DCChar_AccountDataReplication + UEOffsets::AcctData_AccountId);
    }
    out.partyId = ReadCharacterPartyId(proc, actorAddr);

    // Read player nickname from AccountDataReplication chain
    // ADCCharacterBase + 0x0820 = FAccountDataReplication
    // FAccountDataReplication + 0x0010 = FNickname
    // FNickname + 0x0000 = FString (OriginalNickName)
    uintptr_t acctDataAddr = actorAddr + UEOffsets::DCChar_AccountDataReplication;
    uintptr_t nicknameStrAddr = acctDataAddr + UEOffsets::AcctData_Nickname + UEOffsets::Nickname_OriginalNickName;

    // Proton stores Unreal FString text as UTF-16 (two bytes per code unit).
    FString nameStr = proc.Read<FString>(nicknameStrAddr);
    if (nameStr.Data && nameStr.Count > 0 && nameStr.Count < 128) {
        std::vector<char16_t> wbuf(nameStr.Count);
        if (proc.ReadRaw(nameStr.Data, wbuf.data(), nameStr.Count * sizeof(char16_t))) {
            // Convert wchar to utf8
            std::string utf8Name;
            for (int i = 0; i < nameStr.Count && wbuf[i] != 0; i++) {
                if (wbuf[i] < 128)
                    utf8Name += static_cast<char>(wbuf[i]);
                else
                    utf8Name += '?'; // Non-ASCII placeholder
            }
            if (!utf8Name.empty())
                out.displayName = utf8Name;
        }
    }

    // Read player level from AccountDataReplication
    out.gearScore = proc.Read<int32_t>(acctDataAddr + UEOffsets::AcctData_Level);

    // Check alive status from AccountDataReplication
    bool bAlive = proc.Read<bool>(acctDataAddr + UEOffsets::AcctData_bAlive);
    bool bDown = proc.Read<bool>(acctDataAddr + UEOffsets::AcctData_bDown);

    // ---- Determine player class ----
    // PlayerCharacterId is a FString in the current SDK. CharacterId is a
    // separate FString, so try both before falling back to CharacterKey.
    auto parseClass = [](const std::string& s) -> std::string {
        if (s.find("Fighter") != std::string::npos)    return "Fighter";
        if (s.find("Barbarian") != std::string::npos)  return "Barbarian";
        if (s.find("Rogue") != std::string::npos)      return "Rogue";
        if (s.find("Ranger") != std::string::npos)     return "Ranger";
        if (s.find("Wizard") != std::string::npos)     return "Wizard";
        if (s.find("Cleric") != std::string::npos)     return "Cleric";
        if (s.find("Bard") != std::string::npos)       return "Bard";
        if (s.find("Warlock") != std::string::npos)    return "Warlock";
        if (s.find("Druid") != std::string::npos)      return "Druid";
        if (s.find("Sorcerer") != std::string::npos)   return "Sorcerer";
        if (s.find("GrandMaster") != std::string::npos) return "GrandMaster";
        return "";
    };

    out.playerClass = "";

    for (uintptr_t offset : {UEOffsets::AcctData_PlayerCharacterId, UEOffsets::AcctData_CharacterId}) {
        FString charIdStr = proc.Read<FString>(acctDataAddr + offset);
        if (charIdStr.Data && charIdStr.Count > 0 && charIdStr.Count < 256) {
            std::vector<char16_t> cidBuf(charIdStr.Count);
            if (proc.ReadRaw(charIdStr.Data, cidBuf.data(), charIdStr.Count * sizeof(char16_t))) {
                std::string charIdUtf8;
                for (int i = 0; i < charIdStr.Count && cidBuf[i] != 0; i++) {
                    if (cidBuf[i] < 128) charIdUtf8 += static_cast<char>(cidBuf[i]);
                }
                out.playerClass = parseClass(charIdUtf8);
            }
        }
        if (!out.playerClass.empty()) break;
    }

    // FDCPlayerCharacterKey starts with EDCCharacterClass (one byte).
    if (out.playerClass.empty()) {
        uint8_t classId = proc.Read<uint8_t>(actorAddr + UEOffsets::DCPlayer_CharacterKey);
        if (classId > 0 && classId <= static_cast<uint8_t>(EDCCharacterClass::GrandMaster))
            out.playerClass = CharacterClassToString(static_cast<EDCCharacterClass>(classId));
    }

    if (out.playerClass.empty()) out.playerClass = "Player";

    if (out.displayName.empty())
        out.displayName = out.playerClass;

    // Read health via AbilitySystemComponent chain
    ReadCharacterHealth(proc, actorAddr, names, out);

    // Health is the primary alive/dead indicator.
    // bIsDead/bAlive from AccountData are secondary — their offsets may have shifted.
    // Only override if health says alive but bIsDead says dead (confirmed kill).
    bool bIsDead = proc.Read<bool>(actorAddr + UEOffsets::DCChar_bIsDead);

    static int aliveDiag = 0;
    if (aliveDiag++ % 500 == 0) {
        spdlog::info("[PlayerData] '{}' health={:.0f}/{:.0f} isAlive(hp)={} bIsDead={} bAlive={} bDown={}",
            out.displayName, out.health, out.maxHealth, out.isAlive, bIsDead, bAlive, bDown);
    }

    // Only use bIsDead to force dead — don't use bAlive (offset may be stale)
    if (bIsDead) {
        out.isAlive = false;
    }
    if (bDown) {
        out.displayName += " [DOWN]";
    }
}

void ActorManager::ReadCharacterHealth(const Process& proc, uintptr_t actorAddr,
                                        GNamesReader& names, GameEntity& out) {
    // DCCharacterBase health chain (offsets from 2026 SDK dump):
    // Actor + 0x0730 -> AbilitySystemComponent
    // ASC + 0x1080   -> SpawnedAttributes (TArray<UAttributeSet*>)
    // First attribute (UDCAttributeSet):
    //   + 0x0820 -> Health (FGameplayAttributeData, 0x10 bytes)
    //   + 0x0840 -> MaxHealth (FGameplayAttributeData, 0x10 bytes)
    // FGameplayAttributeData: +0x08 = BaseValue, +0x0C = CurrentValue
    //
    // IMPORTANT: If the pointer chain fails (NULL ASC, empty SpawnedAttrs),
    // we leave isAlive as true (default). Only explicitly set dead if we
    // successfully read health and it's <= 0. This prevents false-dead
    // from race conditions during ability system updates.

    uintptr_t asc = proc.Read<uintptr_t>(actorAddr + UEOffsets::DCChar_AbilitySystemComponent);
    if (!asc) {
        // ASC not found — don't mark dead, just show no health bar
        out.health = -1.0f;  // -1 = unknown (don't show health bar)
        out.maxHealth = 0.0f;
        // Leave isAlive = true (default)
        return;
    }

    TArray<uintptr_t> spawnedAttrs = proc.Read<TArray<uintptr_t>>(asc + UEOffsets::ASC_SpawnedAttributes);
    if (spawnedAttrs.Count <= 0 || spawnedAttrs.Count > 32 ||
        spawnedAttrs.Max < spawnedAttrs.Count || !spawnedAttrs.Data) {
        out.health = -1.0f;
        out.maxHealth = 0.0f;
        return;
    }

    // The first spawned attribute need not be UDCAttributeSet. Check each
    // class before reading the health fields at the DCAttributeSet offsets.
    uintptr_t attrSet = 0;
    for (int i = 0; i < spawnedAttrs.Count; ++i) {
        uintptr_t candidate = proc.Read<uintptr_t>(spawnedAttrs.Data + i * sizeof(uintptr_t));
        if (candidate < 0x10000 || candidate > 0x00007FFFFFFFFFFF) continue;
        uintptr_t attrClass = proc.Read<uintptr_t>(candidate + UEOffsets::UObject_Class);
        if (attrClass < 0x10000 || attrClass > 0x00007FFFFFFFFFFF) continue;
        FName className = proc.Read<FName>(attrClass + UEOffsets::UObject_Name);
        if (names.GetFName(proc, className.ComparisonIndex, className.Number) != "DCAttributeSet")
            continue;
        attrSet = candidate;
        break;
    }
    if (!attrSet) {
        out.health = -1.0f;
        out.maxHealth = 0.0f;
        return;
    }

    float health = proc.Read<float>(attrSet + UEOffsets::Attr_Health + UEOffsets::AttrData_CurrentValue);
    float maxHealth = proc.Read<float>(attrSet + UEOffsets::Attr_MaxHealth + UEOffsets::AttrData_CurrentValue);
    if (!std::isfinite(health) || !std::isfinite(maxHealth) || maxHealth <= 0.0f || maxHealth > 100000.0f) {
        out.health = -1.0f;
        out.maxHealth = 0.0f;
        return;
    }

    out.cachedAttrSet = attrSet;
    out.health = health;
    out.maxHealth = maxHealth;

    // Only mark dead if we got a valid read AND health is effectively 0
    // Use epsilon because float reads may not be exactly 0.0
    if (out.health >= 0.0f && out.health < 0.5f) {
        out.isAlive = false;
    }
    // If health > 0.5, leave isAlive = true (default)
    // If health is NaN or negative (garbage), leave isAlive = true (safe default)
}

void ActorManager::ReadItemData(const Process& proc, uintptr_t actorAddr,
                                 GNamesReader& names, GameEntity& out) {
    // Read the actor's FName for basic item identification (fallback)
    FName actorName = proc.Read<FName>(actorAddr + UEOffsets::UObject_Name);
    out.itemName = names.GetFName(proc, actorName.ComparisonIndex, actorName.Number);

    // Determine if this is an ItemHolderActorBase (StaticMeshItemHolder, etc.)
    // vs a specific item BP (BP_FlangedMace_C, BP_Buckler_C, BP_Bandage_C, etc.)
    //
    // ItemHolder actors have valid DataAsset pointers at offset 0x0348.
    // Specific item BPs do NOT inherit from AItemHolderActorBase, so reading
    // 0x0348 gives garbage (e.g., 0x7CF297DA900300D). For these, the class
    // name IS the item identity — we use it directly.
    bool isItemHolder = (out.className.find("ItemHolder") != std::string::npos);

    uintptr_t dataAsset = 0;

    if (isItemHolder) {
        // Path 1: Direct DataAsset pointer at 0x0348 (AItemHolderActorBase)
        dataAsset = proc.Read<uintptr_t>(actorAddr + UEOffsets::ItemHolder_DataAsset);

        // Path 2: Through FDCItemInfo at 0x0350 -> ItemInfo_DataAsset at +0x10
        if (!dataAsset) {
            dataAsset = proc.Read<uintptr_t>(
                actorAddr + UEOffsets::ItemHolder_ItemInfo + UEOffsets::ItemInfo_DataAsset);
        }

        // Validate pointer — game heap is in 0x1XX-0x7EX range, garbage is >0x7F...
        if (dataAsset && (dataAsset > 0x00007FFFFFFFFFFF || dataAsset < 0x10000)) {
            dataAsset = 0; // Invalid pointer
        }
    }
    // For specific item BPs: don't even try DataAsset — it's meaningless at that offset

    static int lootDiag = 0;
    if (lootDiag++ % 500 == 0) {
        spdlog::info("[ItemData] '{}' class='{}' isHolder={} DataAsset=0x{:X}",
            out.itemName, out.className, isItemHolder, dataAsset);
    }

    if (dataAsset) {
        // Read the actual item name from DataAsset's IdTag (FGameplayTag = FName)
        FName idTag = proc.Read<FName>(dataAsset + UEOffsets::ItemData_IdTag);
        std::string idStr = names.GetFName(proc, idTag.ComparisonIndex, idTag.Number);
        if (!idStr.empty()) {
            out.itemName = idStr;
        }

        // Read item type
        uint8_t itemType = proc.Read<uint8_t>(dataAsset + UEOffsets::ItemData_ItemType);
        switch (itemType) {
            case 1: out.itemCategory = "Weapon"; break;
            case 2: out.itemCategory = "Armor"; break;
            case 3: out.itemCategory = "Utility"; break;
            case 4: out.itemCategory = "Accessory"; break;
            case 5: out.itemCategory = "Misc"; break;
            case 6: out.itemCategory = "Gem"; break;
            default: out.itemCategory = "Item"; break;
        }

        // Read gear score
        out.gearScore = proc.Read<int32_t>(dataAsset + UEOffsets::ItemData_GearScore);

        // Read rarity from RarityType GameplayTag
        FName rarityTag = proc.Read<FName>(dataAsset + UEOffsets::ItemData_RarityType);
        std::string rarityStr = names.GetFName(proc, rarityTag.ComparisonIndex, rarityTag.Number);

        // Parse rarity from tag string
        if (!rarityStr.empty()) {
            out.rarity = ParseRarityFromTag(rarityStr);
        } else {
            out.rarity = GuessRarityFromName(out.itemName);
        }
    } else {
        // No valid DataAsset — use class name as item identity
        // For specific BPs like BP_FlangedMace_C, the class name IS the item
        out.rarity = GuessRarityFromName(out.className);
    }

    // Filter out Gold Coins / Gold Pouch — junk info
    if (out.itemName.find("GoldCoin") != std::string::npos ||
        out.itemName.find("Gold_Coin") != std::string::npos ||
        out.itemName.find("GoldPouch") != std::string::npos) {
        out.type = EntityType::UNKNOWN; // Mark for exclusion
        return;
    }

    // Clean up display name — strip common prefixes/suffixes for readability
    std::string name = out.itemName;
    // Strip "DesignDataItem:" or "Item." prefix from gameplay tag names
    size_t colonPos = name.rfind(':');
    if (colonPos != std::string::npos) name = name.substr(colonPos + 1);
    size_t dotPos = name.rfind('.');
    if (dotPos != std::string::npos) name = name.substr(dotPos + 1);
    // Strip BP_ prefix
    if (name.find("BP_") == 0) name = name.substr(3);
    // Strip _C suffix
    if (name.size() > 2 && name.substr(name.size() - 2) == "_C")
        name = name.substr(0, name.size() - 2);
    // Strip instance number suffixes like _2147263766
    if (name.size() > 2) {
        size_t lastUnderscore = name.rfind('_');
        if (lastUnderscore != std::string::npos && lastUnderscore + 1 < name.size()) {
            bool allDigits = true;
            for (size_t i = lastUnderscore + 1; i < name.size(); i++) {
                if (!isdigit(name[i])) { allDigits = false; break; }
            }
            if (allDigits && name.size() - lastUnderscore > 4) // Only strip long number suffixes
                name = name.substr(0, lastUnderscore);
        }
    }
    // Replace underscores with spaces
    for (char& c : name) if (c == '_') c = ' ';

    out.displayName = name;
    if (out.displayName.empty())
        out.displayName = out.itemName.empty() ? out.className : out.itemName;
}

void ActorManager::ReadDeadBodyEquipment(const Process& proc, uintptr_t actorAddr,
                                          GNamesReader& names, GameEntity& out,
                                          ItemRarity minDisplayRarity) {
    // Read inventory component pointer from ADCCharacterBase
    uintptr_t invComp = proc.Read<uintptr_t>(actorAddr + UEOffsets::DCChar_InventoryComponentV2);

    static int bodyDiag = 0;
    bool doDiag = (bodyDiag++ % 300 == 0);

    if (!invComp || invComp > 0x00007FFFFFFFFFFF || invComp < 0x10000)
        return;

    // Read InventoryList TArray<UDCInventoryBase*>
    TArray<uintptr_t> invList = proc.Read<TArray<uintptr_t>>(invComp + UEOffsets::InvContainer_InventoryList);
    if (invList.Count <= 0 || invList.Count > 20 || !invList.Data ||
        invList.Data > 0x00007FFFFFFFFFFF) return;

    // Read all inventory base pointers
    std::vector<uintptr_t> invPtrs(invList.Count);
    if (!proc.ReadRaw(invList.Data, invPtrs.data(), invList.Count * sizeof(uintptr_t))) return;

    // Helper: try reading items with a specific element size and item info offset.
    // Returns true if valid items with real names were found.
    auto tryReadWithLayout = [&](uintptr_t itemsData, int itemsCount,
                                  uintptr_t elemSize, uintptr_t infoOffset,
                                  int invIdx) -> bool {
        bool foundAny = false;
        for (int i = 0; i < itemsCount; i++) {
            uintptr_t elemAddr = itemsData + i * elemSize;
            uintptr_t dataAsset = proc.Read<uintptr_t>(elemAddr + infoOffset + UEOffsets::ItemInfo_DataAsset);
            if (!dataAsset || dataAsset > 0x00007FFFFFFFFFFF || dataAsset < 0x10000) continue;

            // Strong validation: rarity tag must be a real gameplay tag
            FName rarityTag = proc.Read<FName>(dataAsset + UEOffsets::ItemData_RarityType);
            std::string rarityStr = names.GetFName(proc, rarityTag.ComparisonIndex, rarityTag.Number);
            if (rarityStr.find("Rarity.") == std::string::npos) continue;

            // Item name must be valid
            FName itemName = proc.Read<FName>(dataAsset + UEOffsets::ItemData_IdTag);
            std::string name = names.GetFName(proc, itemName.ComparisonIndex, itemName.Number);
            if (name.empty() || name == "None" || name.size() < 5) continue;

            ItemRarity rarity = ParseRarityFromTag(rarityStr);
            foundAny = true;

            if (rarity < minDisplayRarity) continue;

            // Skip Gold Coins / Gold Pouch — user explicitly requested filtering these
            if (name.find("GoldCoin") != std::string::npos ||
                name.find("Gold_Coin") != std::string::npos ||
                name.find("GoldPouch") != std::string::npos) continue;

            // Clean up item name for display
            std::string displayName = name;
            size_t colonPos = displayName.rfind(':');
            if (colonPos != std::string::npos) displayName = displayName.substr(colonPos + 1);
            size_t dotPos = displayName.rfind('.');
            if (dotPos != std::string::npos) displayName = displayName.substr(dotPos + 1);

            out.equipment.push_back(std::string(RarityToString(rarity)) + " " + displayName);
        }
        return foundAny;
    };

    // Helper: try reading items from a TArray at a given offset, trying multiple element layouts.
    auto tryReadItemsAt = [&](uintptr_t invBase, uintptr_t itemsOffset, int invIdx) -> bool {
        TArray<uintptr_t> items = proc.Read<TArray<uintptr_t>>(invBase + itemsOffset);
        if (items.Count <= 0 || items.Count > 50 || !items.Data ||
            items.Data > 0x00007FFFFFFFFFFF || items.Data < 0x10000)
            return false;

        // Current SDK: FDCInventoryDataElement (0x288), Value at +0x10.
        if (tryReadWithLayout(items.Data, items.Count, UEOffsets::InvDataElem_Size,
                              UEOffsets::InvDataElem_ItemInfo, invIdx))
            return true;
        // Historical layouts remain as guarded fallbacks for unusual inventory types.
        if (tryReadWithLayout(items.Data, items.Count, 0x0240, UEOffsets::InvDataElem_ItemInfoAlt, invIdx))
            return true;
        if (tryReadWithLayout(items.Data, items.Count, 0x0240, UEOffsets::InvDataElem_ItemInfo, invIdx))
            return true;
        // Layout 3: info at +0x18 (DataAsset directly at elem+0x18)
        if (tryReadWithLayout(items.Data, items.Count, 0x0240, 0x0018, invIdx))
            return true;
        // Layout 4: info at +0x20
        if (tryReadWithLayout(items.Data, items.Count, 0x0240, 0x0020, invIdx))
            return true;
        // Layout 5: smaller element sizes
        if (tryReadWithLayout(items.Data, items.Count, 0x0238, 0x0010, invIdx))
            return true;
        if (tryReadWithLayout(items.Data, items.Count, 0x0228, 0x0000, invIdx))
            return true;
        return false;
    };

    for (int invIdx = 0; invIdx < static_cast<int>(invPtrs.size()); invIdx++) {
        uintptr_t invBase = invPtrs[invIdx];
        if (!invBase || invBase > 0x00007FFFFFFFFFFF) continue;

        uint8_t invId = proc.Read<uint8_t>(invBase + UEOffsets::InvBase_InventoryId);

        // The current SDK places Items at InventoryData + 0x110.
        uintptr_t stdItemsOffset = UEOffsets::InvBase_InventoryData + UEOffsets::InvData_Items;
        if (tryReadItemsAt(invBase, stdItemsOffset, invIdx))
            continue;

        // Historical runtime offset for monster inventories.
        if (tryReadItemsAt(invBase, UEOffsets::InvBase_MonsterItems, invIdx))
            continue;

        // Fallback: probe a few offsets near the known ones (minimal — keep scan fast)
        static const uintptr_t fallbackOffsets[] = {
            0x00C0, 0x00C8, 0x00D0, 0x00E0, 0x00E8, 0x00F0,
            0x0100, 0x0110, 0x0120, 0x0128, 0x0140, 0x0150,
            0x0200, 0x0210, 0x0220, 0x0230, 0x0248, 0x0258,
        };
        for (uintptr_t probeOff : fallbackOffsets) {
            if (probeOff == UEOffsets::InvBase_MonsterItems || probeOff == stdItemsOffset)
                continue;
            if (tryReadItemsAt(invBase, probeOff, invIdx)) {
                if (doDiag) {
                    spdlog::info("[DeadBody] Items found at probed offset 0x{:X} for invId={} on '{}'",
                        probeOff, invId, out.displayName);
                }
                break;
            }
        }
    }
}

void ActorManager::ReadChestContents(const Process& proc, uintptr_t actorAddr,
                                      GNamesReader& names, GameEntity& out,
                                      ItemRarity minDisplayRarity) {
    // Chests are ADCInteractableActorBase subclasses — NOT characters.
    // They don't have DCChar_InventoryComponentV2 at 0x0A68.
    // Instead, they have their own inventory component as a UObject* member.
    //
    // Strategy: probe offsets where the inventory component pointer might live.
    // The probing is EXPENSIVE (many relay reads), so we:
    // 1. Cache the discovered offset (same for all chests of same type)
    // 2. Limit to 1 probe attempt per scan cycle (global throttle)
    // 3. After 3 failed probes, give up on probing entirely (chests may not have inventory)

    // Cache the discovered offset across calls
    static uintptr_t cachedChestInvOffset = 0;
    static bool probeComplete = false;
    static int probeFailCount = 0;

    // Helper: try reading items from an inventory base with multiple layouts.
    auto tryReadItemsFromInvBase = [&](uintptr_t invBase) -> bool {
        if (!invBase || invBase > 0x00007FFFFFFFFFFF) return false;

        // Item offsets to try within UDCInventoryBase:
        static const uintptr_t itemOffsets[] = {
            UEOffsets::InvBase_InventoryData + UEOffsets::InvData_Items,
            UEOffsets::InvBase_MonsterItems,
        };

        // Element layouts to try (elemSize, infoOffset)
        struct ElemLayout { uintptr_t size; uintptr_t info; };
        static const ElemLayout layouts[] = {
            { UEOffsets::InvDataElem_Size, UEOffsets::InvDataElem_ItemInfo },
            { 0x0240, UEOffsets::InvDataElem_ItemInfoAlt },  // 0x08 — monster layout
            { 0x0240, UEOffsets::InvDataElem_ItemInfo },     // 0x10 — standard layout
            { 0x0240, 0x0018 },
        };

        for (uintptr_t itemsOff : itemOffsets) {
            TArray<uintptr_t> items = proc.Read<TArray<uintptr_t>>(invBase + itemsOff);
            if (items.Count <= 0 || items.Count > 50 || !items.Data ||
                items.Data > 0x00007FFFFFFFFFFF || items.Data < 0x10000)
                continue;

            for (const auto& layout : layouts) {
                bool foundAny = false;
                for (int i = 0; i < items.Count; i++) {
                    uintptr_t elemAddr = items.Data + i * layout.size;
                    uintptr_t dataAsset = proc.Read<uintptr_t>(
                        elemAddr + layout.info + UEOffsets::ItemInfo_DataAsset);
                    if (!dataAsset || dataAsset > 0x00007FFFFFFFFFFF || dataAsset < 0x10000) continue;

                    FName rarityTag = proc.Read<FName>(dataAsset + UEOffsets::ItemData_RarityType);
                    std::string rarityStr = names.GetFName(proc, rarityTag.ComparisonIndex, rarityTag.Number);
                    if (rarityStr.find("Rarity.") == std::string::npos) continue;

                    FName itemName = proc.Read<FName>(dataAsset + UEOffsets::ItemData_IdTag);
                    std::string name = names.GetFName(proc, itemName.ComparisonIndex, itemName.Number);
                    if (name.empty() || name == "None" || name.size() < 5) continue;

                    ItemRarity rarity = ParseRarityFromTag(rarityStr);
                    foundAny = true;

                    if (rarity < minDisplayRarity) continue;
                    if (name.find("GoldCoin") != std::string::npos ||
                        name.find("GoldPouch") != std::string::npos) continue;

                    // Clean up item name
                    std::string displayName = name;
                    size_t colonPos = displayName.rfind(':');
                    if (colonPos != std::string::npos) displayName = displayName.substr(colonPos + 1);
                    size_t dotPos = displayName.rfind('.');
                    if (dotPos != std::string::npos) displayName = displayName.substr(dotPos + 1);

                    out.equipment.push_back(std::string(RarityToString(rarity)) + " " + displayName);
                }
                if (foundAny) return true;
            }
        }
        return false;
    };

    auto tryReadInventory = [&](uintptr_t invComp) -> bool {
        if (!invComp || invComp > 0x00007FFFFFFFFFFF || invComp < 0x10000)
            return false;

        TArray<uintptr_t> invList = proc.Read<TArray<uintptr_t>>(invComp + UEOffsets::InvContainer_InventoryList);
        if (invList.Count <= 0 || invList.Count > 20 || !invList.Data ||
            invList.Data > 0x00007FFFFFFFFFFF)
            return false;

        std::vector<uintptr_t> invPtrs(invList.Count);
        if (!proc.ReadRaw(invList.Data, invPtrs.data(), invList.Count * sizeof(uintptr_t)))
            return false;

        bool foundAnyItem = false;
        for (uintptr_t invBase : invPtrs) {
            if (tryReadItemsFromInvBase(invBase))
                foundAnyItem = true;
        }
        return foundAnyItem;
    };

    // Fast path: use cached offset if already found
    if (probeComplete && cachedChestInvOffset != 0) {
        uintptr_t invComp = proc.Read<uintptr_t>(actorAddr + cachedChestInvOffset);
        tryReadInventory(invComp);
        return;
    }

    // Give up after 3 failed full probes — chests likely don't expose inventory this way
    if (probeFailCount >= 3) return;

    // Throttle: only probe once every ~5 seconds to avoid killing FPS
    static auto lastProbeTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastProbeTime).count() < 5) return;
    lastProbeTime = now;

    // Slow path: probe offsets to find inventory component
    // Use a smaller range with wider stride to reduce relay reads
    for (uintptr_t offset = 0x0300; offset <= 0x0600; offset += 0x08) {
        uintptr_t candidate = proc.Read<uintptr_t>(actorAddr + offset);
        if (tryReadInventory(candidate)) {
            cachedChestInvOffset = offset;
            probeComplete = true;
            probeFailCount = 0;
            spdlog::info("[ChestProbe] Found chest inventory at offset 0x{:X} on '{}'",
                offset, out.className);
            return;
        }
    }

    probeFailCount++;
    spdlog::info("[ChestProbe] No inventory for '{}' (attempt {}/3)", out.className, probeFailCount);
}

void ActorManager::ReadMonsterData(const Process& proc, uintptr_t actorAddr,
                                    GNamesReader& names, GameEntity& out) {
    // Read health first
    ReadCharacterHealth(proc, actorAddr, names, out);

    // Fallback: if health chain failed (health=-1), check bIsDead flag directly.
    // Some monsters (e.g. SkeletonGuardmanShield) don't have valid ASC/SpawnedAttributes
    // but DO have bIsDead=true when killed. Without this fallback, dead monsters show
    // as "alive" and their loot inventory is never read.
    if (out.health < 0.0f) {
        bool bIsDead = proc.Read<bool>(actorAddr + UEOffsets::DCChar_bIsDead);
        if (bIsDead) {
            out.isAlive = false;
            out.health = 0.0f;
        }
    }

    // FromFakeDeath actors are the "waking up" version of skeletons.
    // They spawn as a separate actor during the fake-death animation.
    // Force them to show as alive (they're literally standing up to attack you).
    if (out.className.find("FromFakeDeath") != std::string::npos) {
        out.isAlive = true;
    }

    static int monsterDiag = 0;
    if (monsterDiag++ % 500 == 0) {
        spdlog::info("[MonsterData] '{}' health={:.0f}/{:.0f} isAlive={} addr=0x{:X}",
            out.className, out.health, out.maxHealth, out.isAlive, actorAddr);
    }

    // Clean up display name from class name (strip BP_ prefix and _C/_Common suffixes)
    std::string name = out.className;
    if (name.find("BP_") == 0) name = name.substr(3);
    // Remove trailing _C
    if (name.size() > 2 && name.substr(name.size() - 2) == "_C")
        name = name.substr(0, name.size() - 2);
    // Remove _Common, _Soulflame, _Nightmare suffixes (grade is shown separately)
    auto stripSuffix = [&](const char* suffix) {
        size_t pos = name.rfind(suffix);
        if (pos != std::string::npos && pos + strlen(suffix) == name.size())
            name = name.substr(0, pos);
    };
    stripSuffix("_Common");
    stripSuffix("_Soulflame");
    stripSuffix("_Nightmare");
    stripSuffix("FromFakeDeath");
    // Replace underscores with spaces for readability
    for (char& c : name) if (c == '_') c = ' ';
    out.displayName = name;

    // Read monster grade from DesignDataAssetMonster -> FDesignDataMonster -> GradeType
    uintptr_t designAsset = proc.Read<uintptr_t>(actorAddr + UEOffsets::DCMonster_DesignDataAssetMonster);
    if (designAsset) {
        // GradeType is a FGameplayTag (just an FName inside)
        FName gradeTag = proc.Read<FName>(
            designAsset + UEOffsets::MonsterDataAsset_Item + UEOffsets::DesignMonster_GradeType);
        std::string gradeStr = names.GetFName(proc, gradeTag.ComparisonIndex, gradeTag.Number);

        // Parse grade from tag string (e.g., "Monster.Grade.Elite")
        std::string lower = gradeStr;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("nightmare") != std::string::npos)
            out.monsterGrade = MonsterGrade::Nightmare;
        else if (lower.find("elite") != std::string::npos)
            out.monsterGrade = MonsterGrade::Elite;
        else if (lower.find("common") != std::string::npos)
            out.monsterGrade = MonsterGrade::Common;
    }
}

// ============================================================================
//  Classification — Dark and Darker LIVE class names (Feb 2026)
//  Source: Runtime actor census from Goblin Caves / Ruins dungeon
//  264 unique classes identified across 26,598 actors
//
//  Key patterns (all end with _C suffix for Blueprint classes):
//    BP_PlayerCharacterDungeon_C — players in dungeon
//    BP_Skeleton*_Common_C, BP_Mummy_Common_C, etc. — monsters
//    BP_*Chest*_N0_C — chests (N0 = tier 0?)
//    BP_StaticMeshItemHolder_C — generic ground items
//    BP_*Sword_C, BP_*Shield_C, BP_*Potion_C — specific item BPs
// ============================================================================
EntityType ActorManager::ClassifyCharacterHierarchy(const Process& proc,
                                                     GNamesReader& names,
                                                     uintptr_t classPtr) {
    // Blueprint names change between builds; the native parent classes are
    // stable in the SDK. Walk only the class chain, with a depth cap for bad reads.
    uintptr_t current = classPtr;
    for (int depth = 0; current && depth < 32; ++depth) {
        FName name = proc.Read<FName>(current + UEOffsets::UObject_Name);
        std::string className = names.GetFName(proc, name.ComparisonIndex, name.Number);
        if (className == "DCPlayerCharacterBase") return EntityType::PLAYER;
        if (className == "DCMonsterBase") return EntityType::MONSTER;

        uintptr_t parent = proc.Read<uintptr_t>(current + UEOffsets::UStruct_SuperStruct);
        if (parent == current || parent < 0x10000 || parent > 0x00007FFFFFFFFFFF)
            break;
        current = parent;
    }
    return EntityType::UNKNOWN;
}

EntityType ActorManager::ClassifyByName(const std::string& className) {
    // ---------- EARLY REJECT ----------
    // GameplayCue / VFX actors (GC_ prefix) — visual effects, not real entities
    if (className.find("GC_") == 0) {
        return EntityType::UNKNOWN;
    }
    // Projectiles — arrows, magic missiles, thrown objects
    if (className.find("_Arrow_C") != std::string::npos ||
        className.find("_MagicArrow_C") != std::string::npos ||
        className.find("_Missile_C") != std::string::npos ||
        className.find("_Projectile_C") != std::string::npos ||
        className.find("Thrown") != std::string::npos) {
        return EntityType::UNKNOWN;
    }
    // AoE effects, ability actors, poison zones — not real entities
    if (className.find("_Aoe_") != std::string::npos ||
        className.find("PoisonArea") != std::string::npos ||
        className.find("PoisonSting") != std::string::npos ||
        className.find("_Area_C") != std::string::npos ||
        className.find("_Effect_C") != std::string::npos ||
        className.find("Decal") != std::string::npos) {
        return EntityType::UNKNOWN;
    }

    // ---------- Players ----------
    // BP_PlayerCharacterDungeon_C — normal player pawns
    // Druid shapeshift forms spawn as separate actors with different class names
    if (className.find("BP_PlayerCharacter") == 0) {
        return EntityType::PLAYER;
    }
    // Druid shapeshift forms — spawn as separate actor pawns, NOT BP_PlayerCharacter.
    // Patterns: "ShapeShift", "DruidBear", "BearForm", etc.
    // NOTE: Must NOT match actual animal mobs (BP_Bear_Common_C, BP_DireWolf_Common_C).
    // If the exact class name is unknown, add a diagnostic in the "cache miss" path below
    // to log any actor near a player that has "Bear"/"Druid"/"Shift" in its name.
    if (className.find("ShapeShift") != std::string::npos ||
        className.find("DruidBear") != std::string::npos ||
        className.find("DruidPanther") != std::string::npos ||
        className.find("DruidChicken") != std::string::npos ||
        className.find("DruidRat") != std::string::npos ||
        className.find("BearForm") != std::string::npos ||
        className.find("PantherForm") != std::string::npos) {
        return EntityType::PLAYER;
    }

    // ---------- Special / Rare Chests only ----------
    // Gold, Marvelous, Ornate — the big ones worth showing
    if (className.find("GoldChest") != std::string::npos ||
        className.find("MarvelousChest") != std::string::npos ||
        className.find("OrnateChest") != std::string::npos) {
        return EntityType::CHEST_SPECIAL;
    }

    // ---------- Monsters / NPCs ----------
    // Must start with BP_ to avoid GC_ (GameplayCue) VFX actors
    if (className.find("BP_") != 0) {
        // Not a BP_ actor — skip monster/loot checks entirely
        return EntityType::UNKNOWN;
    }

    // Skeleton variants (exclude corpse/bones decorations)
    if (className.find("Skeleton") != std::string::npos &&
        (className.find("Swordman") != std::string::npos ||
         className.find("Archer") != std::string::npos ||
         className.find("Footman") != std::string::npos ||
         className.find("Guardman") != std::string::npos ||
         className.find("Champion") != std::string::npos)) {
        return EntityType::MONSTER;
    }
    // Mummy (not SpiderMummy, not corpse props)
    if (className.find("Mummy") != std::string::npos &&
        className.find("Spider") == std::string::npos &&
        className.find("Corpse") == std::string::npos) {
        return EntityType::MONSTER;
    }
    // Named monster types
    if (className.find("SpiderMummy") != std::string::npos ||
        className.find("Cockatrice") != std::string::npos ||
        className.find("Banshee") != std::string::npos ||
        className.find("SpectralKnight") != std::string::npos ||
        className.find("DeathSkull") != std::string::npos ||
        className.find("LivingArmor") != std::string::npos ||
        className.find("DireWolf") != std::string::npos ||
        className.find("Wisp") != std::string::npos ||
        className.find("GiantDragonfly") != std::string::npos ||
        className.find("Zombie") != std::string::npos ||
        className.find("Gargoyle") != std::string::npos ||
        className.find("GiantBat") != std::string::npos ||
        className.find("GiantSpider") != std::string::npos ||
        className.find("GiantCentipede") != std::string::npos ||
        className.find("Wraith") != std::string::npos ||
        className.find("GhostKing") != std::string::npos ||
        className.find("Lich") != std::string::npos ||
        className.find("Mimic") != std::string::npos ||
        className.find("Wyvern") != std::string::npos ||
        className.find("Troll") != std::string::npos ||
        className.find("Goblin") != std::string::npos ||
        className.find("Wolf") != std::string::npos ||
        className.find("Bear") != std::string::npos ||
        className.find("LivingStatue") != std::string::npos ||
        className.find("Living_Statue") != std::string::npos ||
        className.find("Demon") != std::string::npos ||
        className.find("DemonDog") != std::string::npos) {
        return EntityType::MONSTER;
    }
    // Generic monster suffix patterns (BP_*_Common_C, BP_*_Soulflame_C, etc.)
    if (className.find("_Common_C") != std::string::npos ||
        className.find("_Soulflame_C") != std::string::npos ||
        className.find("_Nightmare_C") != std::string::npos) {
        return EntityType::MONSTER;
    }

    // ---------- Loot Items (ground drops) ----------
    // ItemHolder = generic container for ground items
    if (className.find("ItemHolder") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }

    // Specific item BPs (player drops, world spawns, etc.)
    // Weapons
    if (className.find("Sword_C") != std::string::npos ||
        className.find("Axe_C") != std::string::npos ||
        className.find("Mace_C") != std::string::npos ||
        className.find("Dagger_C") != std::string::npos ||
        className.find("Rapier_C") != std::string::npos ||
        className.find("Halberd_C") != std::string::npos ||
        className.find("Spear_C") != std::string::npos ||
        className.find("Longsword_C") != std::string::npos ||
        className.find("Falchion_C") != std::string::npos ||
        className.find("Bardiche_C") != std::string::npos ||
        className.find("Zweihander_C") != std::string::npos ||
        className.find("Quarterstaff_C") != std::string::npos ||
        className.find("HandCannon_C") != std::string::npos ||
        className.find("Crossbow_C") != std::string::npos ||
        className.find("Longbow_C") != std::string::npos ||
        className.find("RecurveBow_C") != std::string::npos ||
        className.find("Spellbook_C") != std::string::npos ||
        className.find("Staff_C") != std::string::npos ||
        className.find("CrystalBall_C") != std::string::npos ||
        className.find("Wand_C") != std::string::npos ||
        className.find("Flute_C") != std::string::npos ||
        className.find("Lute_C") != std::string::npos ||
        className.find("Drum_C") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }
    // Shields
    if (className.find("Buckler_C") != std::string::npos ||
        className.find("HeaterShield_C") != std::string::npos ||
        className.find("RoundShield_C") != std::string::npos ||
        className.find("Pavise_C") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }
    // Armor pieces (generic patterns)
    if (className.find("Helmet_C") != std::string::npos ||
        className.find("Chestplate_C") != std::string::npos ||
        className.find("Leggings_C") != std::string::npos ||
        className.find("Gloves_C") != std::string::npos ||
        className.find("Boots_C") != std::string::npos ||
        className.find("Cloak_C") != std::string::npos ||
        className.find("Hood_C") != std::string::npos ||
        className.find("Hat_C") != std::string::npos ||
        className.find("Robe_C") != std::string::npos ||
        className.find("Tunic_C") != std::string::npos ||
        className.find("Cape_C") != std::string::npos ||
        className.find("Gauntlet_C") != std::string::npos ||
        className.find("Necklace_C") != std::string::npos ||
        className.find("Pendant_C") != std::string::npos ||
        className.find("Ring_C") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }
    // Consumables & utility
    if (className.find("HealingPotion") != std::string::npos ||
        className.find("ProtectionPotion") != std::string::npos ||
        className.find("InvisibilityPotion") != std::string::npos ||
        className.find("Potion_C") != std::string::npos ||
        className.find("Bandage_C") != std::string::npos ||
        className.find("Lantern_C") != std::string::npos ||
        className.find("Torch_C") != std::string::npos ||
        className.find("Ale_C") != std::string::npos ||
        className.find("CampfireKit_C") != std::string::npos ||
        className.find("Scroll_C") != std::string::npos ||
        className.find("Trap_C") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }
    // Gold Coins / Gold Pouch — user explicitly requested filtering these out (junk info)
    if (className.find("GoldCoin") != std::string::npos ||
        className.find("GoldPouch") != std::string::npos ||
        className.find("Gold_Coin") != std::string::npos) {
        return EntityType::UNKNOWN;  // Skip — junk
    }
    // Gems, valuables & misc loot
    if (className.find("Gem_C") != std::string::npos ||
        className.find("Coin_C") != std::string::npos ||
        className.find("Ingot_C") != std::string::npos ||
        className.find("Ore_C") != std::string::npos ||
        className.find("Trophy_C") != std::string::npos) {
        return EntityType::LOOT_ITEM;
    }

    // ---------- Portals (escape, down, floor portals) ----------
    if (className.find("FloorPortal") != std::string::npos ||
        className.find("EscapePortal") != std::string::npos ||
        className.find("DownPortal") != std::string::npos ||
        className.find("UpPortal") != std::string::npos ||
        className.find("Portal_C") != std::string::npos ||
        className.find("Escape_C") != std::string::npos ||
        className.find("ExtractionPortal") != std::string::npos) {
        return EntityType::PORTAL;
    }

    return EntityType::UNKNOWN;
}

ItemRarity ActorManager::ParseRarityFromTag(const std::string& tagStr) {
    // FGameplayTag rarity strings from SDK:
    // "Item.Rarity.Poor", "Item.Rarity.Common", etc.
    std::string lower = tagStr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("artifact") != std::string::npos) return ItemRarity::ARTIFACT;
    if (lower.find("unique") != std::string::npos)   return ItemRarity::UNIQUE;
    if (lower.find("legend") != std::string::npos)   return ItemRarity::LEGENDARY;
    if (lower.find("epic") != std::string::npos)     return ItemRarity::EPIC;
    if (lower.find("rare") != std::string::npos)     return ItemRarity::RARE;
    if (lower.find("uncommon") != std::string::npos) return ItemRarity::UNCOMMON;
    if (lower.find("common") != std::string::npos)   return ItemRarity::COMMON;
    if (lower.find("poor") != std::string::npos)     return ItemRarity::POOR;

    return ItemRarity::COMMON;
}

ItemRarity ActorManager::GuessRarityFromName(const std::string& name) {
    // Fallback: keyword-based heuristic from actor/class name
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("artifact") != std::string::npos) return ItemRarity::ARTIFACT;
    if (lower.find("unique") != std::string::npos)   return ItemRarity::UNIQUE;
    if (lower.find("legendary") != std::string::npos || lower.find("legend") != std::string::npos)
        return ItemRarity::LEGENDARY;
    if (lower.find("epic") != std::string::npos)     return ItemRarity::EPIC;
    if (lower.find("rare") != std::string::npos)     return ItemRarity::RARE;
    if (lower.find("uncommon") != std::string::npos) return ItemRarity::UNCOMMON;
    if (lower.find("common") != std::string::npos)   return ItemRarity::COMMON;
    if (lower.find("poor") != std::string::npos)     return ItemRarity::POOR;

    return ItemRarity::COMMON;
}
