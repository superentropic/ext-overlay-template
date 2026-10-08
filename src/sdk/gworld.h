#pragma once
#include "core/process.h"
#include "ue5_types.h"
#include <vector>

class GWorldReader {
public:
    bool Initialize(const Process& proc, uintptr_t gWorldAddr);

    // Get pointer to UWorld
    uintptr_t GetUWorld(const Process& proc) const;

    // Get all actor pointers from PersistentLevel
    std::vector<uintptr_t> GetAllActors(const Process& proc) const;

    // Get local player controller and local player instance
    uintptr_t GetLocalPlayerController(const Process& proc) const;
    uintptr_t GetLocalPlayer(const Process& proc) const;

    // Get local player's pawn (the character we control)
    uintptr_t GetLocalPawn(const Process& proc) const;

    // Get camera info for world-to-screen projection (full chain, slow — 7 reads)
    FMinimalViewInfo GetCameraInfo(const Process& proc) const;

    // Fast camera read — uses cached CameraManager pointer (1 read instead of 7)
    // Call CacheCameraManager() once, then use GetCameraInfoFast() per frame
    void CacheCameraManager(const Process& proc);
    FMinimalViewInfo GetCameraInfoFast(const Process& proc) const;

    uintptr_t GetAddress() const { return m_gworldAddr; }

private:
    uintptr_t m_gworldAddr = 0;
    mutable uintptr_t m_cachedCameraManager = 0;
    mutable int m_cacheAge = 0;
};
