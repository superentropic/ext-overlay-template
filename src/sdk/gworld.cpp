#include "gworld.h"
#include <spdlog/spdlog.h>

namespace {
bool IsObjectPointer(uintptr_t value) {
    return value >= 0x10000 && value <= 0x00007FFFFFFFFFFF &&
           value % alignof(uintptr_t) == 0;
}
}

bool GWorldReader::Initialize(const Process& proc, uintptr_t gWorldAddr) {
    m_gworldAddr = 0;
    m_cachedCameraManager = 0;
    m_cacheAge = 0;

    uintptr_t world = 0;
    if (!IsObjectPointer(gWorldAddr) || !proc.ReadRaw(gWorldAddr, &world, sizeof(world))) {
        spdlog::error("Cannot read GWorld at 0x{:X}; check memory backend and configured offset", gWorldAddr);
        return false;
    }
    if (!IsObjectPointer(world)) {
        spdlog::error("Rejected GWorld at 0x{:X}: invalid UWorld pointer 0x{:X}; "
                      "world may not be ready or the offset may be stale", gWorldAddr, world);
        return false;
    }

    uintptr_t header[3]{}; // UObject vtable, flags/index, class pointer.
    uintptr_t gameInstance = 0;
    uintptr_t level = 0;
    if (!proc.ReadRaw(world, header, sizeof(header)) ||
        !IsObjectPointer(header[0]) || !IsObjectPointer(header[2]) ||
        !proc.ReadRaw(world + UEOffsets::UWorld_OwningGameInstance,
                      &gameInstance, sizeof(gameInstance)) ||
        !IsObjectPointer(gameInstance) ||
        !proc.ReadRaw(world + UEOffsets::UWorld_PersistentLevel, &level, sizeof(level)) ||
        !IsObjectPointer(level)) {
        spdlog::error("Rejected GWorld at 0x{:X}: UWorld=0x{:X} failed object/level/GameInstance validation",
                      gWorldAddr, world);
        return false;
    }

    m_gworldAddr = gWorldAddr;
    spdlog::info("GWorld initialized at 0x{:X}, UWorld at 0x{:X}", gWorldAddr, world);
    return true;
}

uintptr_t GWorldReader::GetUWorld(const Process& proc) const {
    if (!m_gworldAddr) return 0;
    const uintptr_t world = proc.Read<uintptr_t>(m_gworldAddr);
    return IsObjectPointer(world) ? world : 0;
}

std::vector<uintptr_t> GWorldReader::GetAllActors(const Process& proc) const {
    std::vector<uintptr_t> actors;
    static int diagCounter = 0;
    bool doDiag = (diagCounter++ % 900 == 0); // Log diagnostics every ~30s (production)

    uintptr_t world = GetUWorld(proc);
    if (!world) {
        if (doDiag) spdlog::warn("[GetAllActors] UWorld is null!");
        return actors;
    }

    if (doDiag) {
        spdlog::info("[GetAllActors] UWorld=0x{:X}", world);
    }

    // Traverse ALL loaded levels (UWorld+0x0178 = TArray<ULevel*>)
    // This is critical for DaD — dungeon sublevels contain the actual gameplay actors
    TArray<uintptr_t> levelsArray = proc.Read<TArray<uintptr_t>>(world + UEOffsets::UWorld_Levels);

    if (doDiag) {
        spdlog::info("[GetAllActors] Levels TArray at UWorld+0x{:X}: Data=0x{:X} Count={} Max={}",
            UEOffsets::UWorld_Levels, levelsArray.Data, levelsArray.Count, levelsArray.Max);
    }

    if (levelsArray.Count <= 0 || levelsArray.Count > 1000 || !levelsArray.Data) {
        if (doDiag) spdlog::warn("[GetAllActors] Levels array invalid, falling back to PersistentLevel");

        // Fallback to PersistentLevel only
        uintptr_t level = proc.Read<uintptr_t>(world + UEOffsets::UWorld_PersistentLevel);
        if (doDiag) spdlog::info("[GetAllActors] PersistentLevel at UWorld+0x{:X}: 0x{:X}",
            UEOffsets::UWorld_PersistentLevel, level);
        if (!level) return actors;

        TArray<uintptr_t> actorArray = proc.Read<TArray<uintptr_t>>(level + UEOffsets::ULevel_Actors);
        if (doDiag) spdlog::info("[GetAllActors] PersistentLevel Actors at +0x{:X}: Data=0x{:X} Count={} Max={}",
            UEOffsets::ULevel_Actors, actorArray.Data, actorArray.Count, actorArray.Max);

        if (actorArray.Count > 0 && actorArray.Count < 100000 && actorArray.Data) {
            actors.resize(actorArray.Count);
            proc.ReadRaw(actorArray.Data, actors.data(), actorArray.Count * sizeof(uintptr_t));
        }
    } else {
        // Read all level pointers
        std::vector<uintptr_t> levelPtrs(levelsArray.Count);
        if (!proc.ReadRaw(levelsArray.Data, levelPtrs.data(), levelsArray.Count * sizeof(uintptr_t))) {
            if (doDiag) spdlog::error("[GetAllActors] Failed to read level pointers array");
            return actors;
        }

        if (doDiag) {
            spdlog::info("[GetAllActors] Successfully read {} level pointers", levelsArray.Count);
        }

        // Auto-detect ULevel actors offset if the configured one doesn't work
        // We scan the first valid level for a TArray that contains valid actor pointers
        static uintptr_t detectedActorsOffset = UEOffsets::ULevel_Actors;
        static bool offsetDetected = false;

        if (!offsetDetected && !levelPtrs.empty()) {
            uintptr_t testLevel = levelPtrs[0];
            if (testLevel) {
                // First check if the configured offset works
                TArray<uintptr_t> testArray = proc.Read<TArray<uintptr_t>>(testLevel + UEOffsets::ULevel_Actors);
                if (testArray.Data > 0x10000 && testArray.Data < 0x00007FFFFFFFFFFF &&
                    testArray.Count > 0 && testArray.Count < 100000 &&
                    testArray.Max >= testArray.Count) {
                    offsetDetected = true;
                    spdlog::info("[GetAllActors] Configured ULevel_Actors offset 0x{:X} works!", UEOffsets::ULevel_Actors);
                } else {
                    // Scan for the correct offset
                    spdlog::info("[GetAllActors] ULevel_Actors offset 0x{:X} BROKEN, scanning for correct offset...",
                        UEOffsets::ULevel_Actors);

                    // ULevel inherits UObject (0x28 bytes), scan reasonable range
                    // The actors TArray is typically early in ULevel (0x28 to 0x200)
                    struct ActorArrayCandidate {
                        uintptr_t offset;
                        int count;
                        int verifiedActors; // How many entries look like valid actor pointers
                    };
                    std::vector<ActorArrayCandidate> candidates;

                    for (uintptr_t scanOff = 0x28; scanOff <= 0x200; scanOff += 8) {
                        TArray<uintptr_t> candidate = proc.Read<TArray<uintptr_t>>(testLevel + scanOff);
                        if (candidate.Data > 0x10000 && candidate.Data < 0x00007FFFFFFFFFFF &&
                            candidate.Count > 0 && candidate.Count < 100000 &&
                            candidate.Max >= candidate.Count && candidate.Max < 200000) {
                            // Verify: read first few entries and check if they look like actor pointers
                            int verifyCount = (candidate.Count < 5) ? candidate.Count : 5;
                            std::vector<uintptr_t> testPtrs(verifyCount);
                            int validPtrs = 0;
                            if (proc.ReadRaw(candidate.Data, testPtrs.data(), verifyCount * sizeof(uintptr_t))) {
                                for (int i = 0; i < verifyCount; i++) {
                                    if (testPtrs[i] > 0x10000 && testPtrs[i] < 0x00007FFFFFFFFFFF) {
                                        // Check if this looks like a UObject (has a vtable pointer)
                                        uintptr_t vtable = proc.Read<uintptr_t>(testPtrs[i]);
                                        if (vtable > 0x7FF000000000 && vtable < 0x7FFFFFFFFFFF) {
                                            validPtrs++;
                                        }
                                    }
                                }
                            }

                            if (validPtrs > 0) {
                                candidates.push_back({scanOff, candidate.Count, validPtrs});
                                spdlog::info("  ULevel+0x{:03X}: Count={} Max={} verified={}/{} (CANDIDATE)",
                                    scanOff, candidate.Count, candidate.Max, validPtrs, verifyCount);
                            }
                        }
                    }

                    // Also try the same scan on a few more levels to confirm
                    if (!candidates.empty()) {
                        // Pick the candidate with the highest count of verified actors
                        auto best = std::max_element(candidates.begin(), candidates.end(),
                            [](const ActorArrayCandidate& a, const ActorArrayCandidate& b) {
                                // Prefer higher verification rate, then higher count
                                if (a.verifiedActors != b.verifiedActors)
                                    return a.verifiedActors < b.verifiedActors;
                                return a.count < b.count;
                            });

                        detectedActorsOffset = best->offset;
                        offsetDetected = true;
                        spdlog::info("[GetAllActors] Auto-detected ULevel_Actors offset: 0x{:03X} "
                                     "(count={}, verified={})",
                            detectedActorsOffset, best->count, best->verifiedActors);

                        // Verify on a second level too
                        if (levelPtrs.size() > 1 && levelPtrs[1]) {
                            TArray<uintptr_t> verify = proc.Read<TArray<uintptr_t>>(levelPtrs[1] + detectedActorsOffset);
                            spdlog::info("[GetAllActors] Verification on Level[1]: Count={} Max={} Data=0x{:X}",
                                verify.Count, verify.Max, verify.Data);
                        }
                    } else {
                        spdlog::error("[GetAllActors] No valid actor array found in ULevel scan!");
                        offsetDetected = true; // Don't keep scanning
                    }
                }
            }
        }

        // Iterate each level's actor list using the detected offset
        int levelIdx = 0;
        int levelsWithActors = 0;
        for (uintptr_t levelPtr : levelPtrs) {
            if (!levelPtr) { levelIdx++; continue; }

            TArray<uintptr_t> actorArray = proc.Read<TArray<uintptr_t>>(levelPtr + detectedActorsOffset);

            if (doDiag && levelIdx < 5) {
                spdlog::info("[GetAllActors] Level[{}] at 0x{:X}: Actors(+0x{:X}) Data=0x{:X} Count={} Max={}",
                    levelIdx, levelPtr, detectedActorsOffset, actorArray.Data, actorArray.Count, actorArray.Max);
            }

            if (actorArray.Count <= 0 || actorArray.Count > 100000 || !actorArray.Data) {
                levelIdx++;
                continue;
            }

            levelsWithActors++;
            size_t prevSize = actors.size();
            actors.resize(prevSize + actorArray.Count);
            if (!proc.ReadRaw(actorArray.Data, actors.data() + prevSize, actorArray.Count * sizeof(uintptr_t))) {
                actors.resize(prevSize); // Rollback on failure
            }
            levelIdx++;
        }

        if (doDiag) {
            spdlog::info("[GetAllActors] {} of {} levels had valid actors, total raw: {}",
                levelsWithActors, levelsArray.Count, actors.size());
        }
    }

    // Remove null pointers
    actors.erase(
        std::remove(actors.begin(), actors.end(), 0ull),
        actors.end()
    );

    if (doDiag) {
        spdlog::info("[GetAllActors] Final actor count (after removing nulls): {}", actors.size());
    }

    return actors;
}

uintptr_t GWorldReader::GetLocalPlayerController(const Process& proc) const {
    static int lpcDiag = 0;
    bool doDiag = (lpcDiag++ % 900 == 0);

    uintptr_t world = GetUWorld(proc);
    if (!world) return 0;

    // UWorld -> OwningGameInstance
    uintptr_t gameInstance = proc.Read<uintptr_t>(world + UEOffsets::UWorld_OwningGameInstance);
    if (!IsObjectPointer(gameInstance)) {
        if (doDiag) spdlog::warn("[GetLPC] GameInstance is NULL! (UWorld=0x{:X}, offset=0x{:X})",
            world, UEOffsets::UWorld_OwningGameInstance);
        return 0;
    }

    // UGameInstance -> LocalPlayers (TArray<ULocalPlayer*>)
    TArray<uintptr_t> localPlayers = proc.Read<TArray<uintptr_t>>(gameInstance + UEOffsets::GameInstance_LocalPlayers);
    if (localPlayers.Count <= 0 || localPlayers.Count > 16 ||
        localPlayers.Max < localPlayers.Count || !IsObjectPointer(localPlayers.Data)) {
        if (doDiag) spdlog::warn("[GetLPC] LocalPlayers empty! (GameInstance=0x{:X}, offset=0x{:X})",
            gameInstance, UEOffsets::GameInstance_LocalPlayers);
        return 0;
    }

    // First local player
    uintptr_t localPlayer = proc.Read<uintptr_t>(localPlayers.Data);
    if (!IsObjectPointer(localPlayer)) return 0;

    // ULocalPlayer -> PlayerController
    uintptr_t controller = proc.Read<uintptr_t>(localPlayer + UEOffsets::LocalPlayer_PlayerController);

    if (doDiag) {
        spdlog::info("[GetLPC] World=0x{:X} -> GameInst=0x{:X} -> LocalPlayers[0]=0x{:X} -> Controller=0x{:X}",
            world, gameInstance, localPlayer, controller);
    }

    return IsObjectPointer(controller) ? controller : 0;
}

uintptr_t GWorldReader::GetLocalPlayer(const Process& proc) const {
    uintptr_t world = GetUWorld(proc);
    if (!world) return 0;

    uintptr_t gameInstance = proc.Read<uintptr_t>(world + UEOffsets::UWorld_OwningGameInstance);
    if (!IsObjectPointer(gameInstance)) return 0;

    TArray<uintptr_t> localPlayers = proc.Read<TArray<uintptr_t>>(gameInstance + UEOffsets::GameInstance_LocalPlayers);
    if (localPlayers.Count <= 0 || localPlayers.Count > 16 ||
        localPlayers.Max < localPlayers.Count || !IsObjectPointer(localPlayers.Data)) {
        return 0;
    }

    uintptr_t localPlayer = proc.Read<uintptr_t>(localPlayers.Data);
    return IsObjectPointer(localPlayer) ? localPlayer : 0;
}

uintptr_t GWorldReader::GetLocalPawn(const Process& proc) const {
    uintptr_t controller = GetLocalPlayerController(proc);
    if (!controller) return 0;

    uintptr_t pawn = proc.Read<uintptr_t>(controller + UEOffsets::PlayerController_AcknowledgedPawn);

    static int pawnDiag = 0;
    if (pawnDiag++ % 900 == 0) {
        spdlog::info("[GetLocalPawn] Controller=0x{:X} -> Pawn=0x{:X} (offset=0x{:X})",
            controller, pawn, UEOffsets::PlayerController_AcknowledgedPawn);
    }

    return pawn;
}

FMinimalViewInfo GWorldReader::GetCameraInfo(const Process& proc) const {
    FMinimalViewInfo info{};

    static int camChainDiag = 0;
    bool doDiag = (camChainDiag++ % 900 == 0);

    uintptr_t controller = GetLocalPlayerController(proc);
    if (!controller) {
        if (doDiag) spdlog::warn("[GetCameraInfo] PlayerController is NULL!");
        return info;
    }

    // APlayerController -> PlayerCameraManager
    uintptr_t cameraManager = proc.Read<uintptr_t>(
        controller + UEOffsets::PlayerController_PlayerCameraManager);
    if (!cameraManager) {
        if (doDiag) spdlog::warn("[GetCameraInfo] CameraManager is NULL! (Controller=0x{:X}, offset=0x{:X})",
            controller, UEOffsets::PlayerController_PlayerCameraManager);
        return info;
    }

    // Cache for fast path
    m_cachedCameraManager = cameraManager;

    // APlayerCameraManager -> CameraCachePrivate -> POV (FMinimalViewInfo)
    uintptr_t povAddr = cameraManager
        + UEOffsets::CameraManager_CameraCachePrivate
        + UEOffsets::CameraCache_POV;

    if (doDiag) {
        spdlog::info("[GetCameraInfo] Controller=0x{:X} -> CameraManager=0x{:X} -> POV at 0x{:X}",
            controller, cameraManager, povAddr);
    }

    proc.ReadRaw(povAddr, &info, sizeof(FMinimalViewInfo));
    return info;
}

void GWorldReader::CacheCameraManager(const Process& proc) {
    uintptr_t controller = GetLocalPlayerController(proc);
    if (!controller) return;
    m_cachedCameraManager = proc.Read<uintptr_t>(
        controller + UEOffsets::PlayerController_PlayerCameraManager);
    if (m_cachedCameraManager) {
        spdlog::info("[CacheCameraManager] Cached at 0x{:X}", m_cachedCameraManager);
    }
}

FMinimalViewInfo GWorldReader::GetCameraInfoFast(const Process& proc) const {
    FMinimalViewInfo info{};

    // Re-cache every ~300 frames in case of level transition
    m_cacheAge++;
    if (!m_cachedCameraManager || m_cacheAge > 300) {
        m_cacheAge = 0;
        return GetCameraInfo(proc); // Full chain, also updates cache
    }

    // Single ReadRaw instead of 7 separate reads through the pointer chain
    uintptr_t povAddr = m_cachedCameraManager
        + UEOffsets::CameraManager_CameraCachePrivate
        + UEOffsets::CameraCache_POV;
    proc.ReadRaw(povAddr, &info, sizeof(FMinimalViewInfo));

    // Sanity — if cache is stale, POV will have garbage
    if (info.FOV <= 0.0f || info.FOV > 180.0f) {
        m_cachedCameraManager = 0;
        return GetCameraInfo(proc);
    }

    return info;
}
