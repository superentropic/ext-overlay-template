// ============================================================================
//  SilentHelper — In-Process ViewPoint Interceptor & Redirection Engine
//  Implementation
// ============================================================================

#include "silent_helper.h"
#include "core/trigger_state.h"
#include <algorithm>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdio>
#include <limits>

namespace SilentHelper {

// ============================================================================
//  Constants & Globals
// ============================================================================
static constexpr double PI = 3.14159265358979323846;
static constexpr double RAD_TO_DEG = 180.0 / PI;
static constexpr double DEG_TO_RAD = PI / 180.0;

static Config g_config;
static std::mutex g_configMutex;
static RuntimeState g_state;
static std::mutex g_stateMutex;
static SharedGameData* g_pShm = nullptr;

// Function pointer to originals
static std::atomic<void*> g_origGetPlayerViewPoint{nullptr};
static std::atomic<void*> g_origGetViewPoint{nullptr};

// Set by Hooked_GetViewPoint before it calls the original function.
// Hooked_GetPlayerViewPoint only applies the redirect when this flag is
// set (i.e. the call came in through the GetViewPoint chain, which is the
// attack-trace path).  Direct GetPlayerViewPoint calls from the camera
// manager — the ones that update APlayerCameraManager::CameraCache — are
// NOT redirected, so CameraCache and the ESP overlay stay at the natural
// aim direction.
// Not used in the current hook design but kept to avoid breaking any future
// caller that might reference it by name.
static thread_local bool g_inGetViewPointCall = false;

// Track hooked instances and their original VMTs for clean restoration
struct HookedVmtInstance {
    void** instancePtr = nullptr;    // Pointer to the object (whose first 8 bytes is vptr)
    void** originalVtable = nullptr; // Pointer to the original .rdata vtable
    std::vector<void*> shadowVtable; // Cloned shadow table
    int hookedIndex = -1;
};

static std::vector<HookedVmtInstance> g_hookedInstances;
static std::mutex g_hookMutex;

// ============================================================================
//  Math Helper Implementations
// ============================================================================

void NormalizeRotator(FRotator& rot) {
    if (std::isfinite(rot.Yaw)) {
        rot.Yaw = std::remainder(rot.Yaw, 360.0);
    }

    if (std::isfinite(rot.Pitch)) {
        rot.Pitch = (std::max)(-89.0, (std::min)(89.0, rot.Pitch));
    }

    rot.Roll = 0.0;
}

FRotator CalculateLookAtRotation(const FVector& source, const FVector& target) {
    FVector delta = target - source;
    double hyp = std::sqrt(delta.X * delta.X + delta.Y * delta.Y);

    double pitch = std::atan2(delta.Z, hyp) * RAD_TO_DEG;
    double yaw   = std::atan2(delta.Y, delta.X) * RAD_TO_DEG;
    double roll  = 0.0;

    FRotator result(pitch, yaw, roll);
    NormalizeRotator(result);
    return result;
}

double CalculateFOV(const FRotator& currentRot, const FRotator& targetRot) {
    const double pitchDelta = std::remainder(targetRot.Pitch - currentRot.Pitch, 360.0);
    const double yawDelta = std::remainder(targetRot.Yaw - currentRot.Yaw, 360.0);

    // Euclidean distance in degrees
    return std::hypot(pitchDelta, yawDelta);
}

static Config GetConfigSnapshot() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

// ============================================================================
//  Hook Callbacks
// ============================================================================

static void TraceExperimentalRedirectDecision(const char* phase,
                                             bool triggerActive,
                                             bool hasTarget,
                                             const FVector& source,
                                             const FRotator& sourceRot,
                                             const FVector& target,
                                             double distanceMeters,
                                             double fovDegrees) {
    // This callback runs on an engine thread. Avoid taking the stderr stream
    // lock for every viewpoint query, which can introduce frame-time spikes.
    static std::atomic<uint64_t> lastTraceTick{0};
    const uint64_t now = GetTickCount64();
    uint64_t previous = lastTraceTick.load(std::memory_order_relaxed);
    if (now < previous || now - previous < 1000 ||
        !lastTraceTick.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
        return;
    }

    std::fprintf(stderr,
        "[SilentHelperTrace] phase=%s triggerActive=%d hasTarget=%d src=(%.2f,%.2f,%.2f) srcYaw=%.2f target=(%.2f,%.2f,%.2f) dist=%.2f fov=%.2f\n",
        phase,
        triggerActive ? 1 : 0,
        hasTarget ? 1 : 0,
        source.X, source.Y, source.Z,
        sourceRot.Yaw,
        target.X, target.Y, target.Z,
        distanceMeters,
        fovDegrees);
}

// ─── GetPlayerViewPoint hook ──────────────────────────────────────────────────
static void __fastcall Hooked_GetPlayerViewPoint(void* this_PlayerController, FVector* Location, FRotator* Rotation) {
    if (!this_PlayerController || !Location || !Rotation) return;

    if (g_pShm) {
        g_pShm->silentHelper.hookCallCountController = g_pShm->silentHelper.hookCallCountController + 1;
    }

    auto original = reinterpret_cast<fn_GetPlayerViewPoint_t>(
        g_origGetPlayerViewPoint.load(std::memory_order_acquire));
    if (!original) return;

    // Read ControlRotation BEFORE any write.
    // When redirect is active, ControlRotation holds targetRot from the previous
    // frame.  The mouse input handler added a delta on top of that this frame, so:
    //   currentCtrlRot = lastWrittenTargetRot + mouseDelta
    //   naturalCtrlRot += mouseDelta  =  naturalCtrlRot + (currentCtrlRot - lastWrittenTargetRot)
    //
    // Temporarily restore ControlRotation to naturalCtrlRot so that original()
    // reads the correct natural camera transform.  We then write targetRot back
    // after saving that natural transform for GetViewPoint to restore.
    constexpr ptrdiff_t kCtrlRotOff = 0x0320;
    auto* ctrlRotPtr = reinterpret_cast<FRotator*>(
        reinterpret_cast<uint8_t*>(this_PlayerController) + kCtrlRotOff);

    const FRotator ctrlRotCurrent = *ctrlRotPtr;

    // Compute naturalCtrlRot and temporarily write it back so original() sees
    // an uncontaminated camera state.
    FRotator naturalCtrlRot;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_state.isRedirecting.load()) {
            // Accumulate mouse delta since our last targetRot write
            FRotator delta;
            delta.Pitch = ctrlRotCurrent.Pitch - g_state.redirectedRotation.Pitch;
            delta.Yaw   = std::remainder(ctrlRotCurrent.Yaw - g_state.redirectedRotation.Yaw, 360.0);
            delta.Roll  = 0.0;
            g_state.originalRotation.Pitch += delta.Pitch;
            g_state.originalRotation.Yaw   += delta.Yaw;
        } else {
            // First frame of redirect — ControlRotation is still truly natural
            g_state.originalRotation.Pitch = ctrlRotCurrent.Pitch;
            g_state.originalRotation.Yaw   = ctrlRotCurrent.Yaw;
            g_state.originalRotation.Roll  = ctrlRotCurrent.Roll;
        }
        naturalCtrlRot = g_state.originalRotation;
    }

    // Temporarily set ControlRotation to natural so original() returns the
    // correct natural camera transform (location + rotation).
    *ctrlRotPtr = naturalCtrlRot;
    original(this_PlayerController, Location, Rotation);
    // Save the natural location (rotation is already tracked in g_state.originalRotation).
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.originalLocation = *Location;
    }

    const Config config = GetConfigSnapshot();
    if (!config.enabled) {
        // Leave ControlRotation at naturalCtrlRot (already restored above).
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.isRedirecting.store(false);
        return;
    }

    const bool keyHeld = config.triggerKey != 0 &&
        (GetAsyncKeyState(config.triggerKey) & 0x8000) != 0;
    const bool isTriggerActive = TriggerState::IsTriggerActive(
        config.triggerKey, config.requireKey, keyHeld);

    FVector target;
    bool hasTarget = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.lastTriggerActive = isTriggerActive;
        target    = g_state.targetLocation;
        hasTarget = g_state.hasTarget.load();
    }

    if (!isTriggerActive || !hasTarget) {
        // ControlRotation already restored to natural above; leave it there.
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.isRedirecting.store(false);
        return;
    }

    FVector adjustedTarget = target;
    adjustedTarget.Z += config.targetZOffset;

    double distanceMeters = Location->DistanceTo(adjustedTarget) / 100.0;
    if (!std::isfinite(distanceMeters) || distanceMeters > config.maxDistanceMeters) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.lastDistanceMeters = distanceMeters;
        g_state.isRedirecting.store(false);
        return;
    }

    FRotator targetRot = CalculateLookAtRotation(*Location, adjustedTarget);
    targetRot.Roll = Rotation->Roll;
    double fov = CalculateFOV(naturalCtrlRot, targetRot);

    TraceExperimentalRedirectDecision(
        "pvp-pre-redirect", isTriggerActive, true,
        *Location, naturalCtrlRot, adjustedTarget, distanceMeters, fov);

    if (!std::isfinite(fov) || fov > config.maxFovDegrees) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.lastDistanceMeters = distanceMeters;
        g_state.lastFovDegrees = fov;
        g_state.isRedirecting.store(false);
        return;
    }

    // Apply redirect to output parameter and ControlRotation.
    *Rotation = targetRot;
    *ctrlRotPtr = targetRot;  // attack reads this via GetControlRotation()

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.lastDistanceMeters = distanceMeters;
        g_state.lastFovDegrees = fov;
        g_state.redirectedRotation = targetRot;
        g_state.isRedirecting.store(true);
        g_state.lastRedirectTick = GetTickCount64();
    }

    TraceExperimentalRedirectDecision(
        "pvp-redirect-applied", isTriggerActive, true,
        *Location, *Rotation, adjustedTarget, distanceMeters, fov);

    if (g_pShm) {
        g_pShm->silentHelper.redirectCount = g_pShm->silentHelper.redirectCount + 1;
    }
}

// ─── GetViewPoint hook ────────────────────────────────────────────────────────
static void __fastcall Hooked_GetViewPoint(void* this_LocalPlayer, FMinimalViewInfo* OutViewInfo, int StereoPass) {
    if (!this_LocalPlayer || !OutViewInfo) return;

    if (g_pShm) {
        g_pShm->silentHelper.hookCallCountLocalPlayer = g_pShm->silentHelper.hookCallCountLocalPlayer + 1;
    }

    auto original = reinterpret_cast<fn_GetViewPoint_t>(
        g_origGetViewPoint.load(std::memory_order_acquire));
    if (!original) return;
    original(this_LocalPlayer, OutViewInfo, StereoPass);

    const Config config = GetConfigSnapshot();
    if (!config.enabled || !config.silentCamera) return;

    bool isRedirecting = false;
    FVector naturalLocation;
    FRotator naturalRotation;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        isRedirecting   = g_state.isRedirecting.load();
        naturalLocation = g_state.originalLocation;
        naturalRotation = g_state.originalRotation;
    }

    if (!isRedirecting) return;

    OutViewInfo->Location = naturalLocation;
    OutViewInfo->Rotation = naturalRotation;

    if (g_pShm) {
        g_pShm->silentHelper.viewPointRestoreCount =
            g_pShm->silentHelper.viewPointRestoreCount + 1;
    }
}

// ============================================================================
//  Shadow VMT Hook Helper
// ============================================================================

static bool IsReadableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xff) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

static bool IsReadableRange(const void* address, size_t size) {
    if (!address || size == 0) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (size > (std::numeric_limits<uintptr_t>::max)() - start) return false;
    uintptr_t cursor = start;
    const uintptr_t end = start + size;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
            return false;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > (std::numeric_limits<uintptr_t>::max)() - regionBase) return false;
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = (std::min)(regionEnd, end);
    }
    return true;
}

static bool SafeReadMemory(const void* address, void* destination, size_t size) {
    if (!destination || !IsReadableRange(address, size)) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &bytesRead) && bytesRead == size;
}

static bool IsExecutableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (mbi.Protect & 0xff) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

static int CountVmtEntries(void** vtable) {
    if (!vtable) return 0;
    int count = 0;
    while (count < 2048) {
        void* function = nullptr;
        const auto* slot = reinterpret_cast<const uint8_t*>(vtable) +
            static_cast<size_t>(count) * sizeof(void*);
        if (!SafeReadMemory(slot, &function, sizeof(function)) || !IsExecutableAddress(function)) break;
        ++count;
    }
    return count;
}

static bool InstallShadowVmtHook(void* instance, int index, void* hookFunc,
                                 std::atomic<void*>& originalFunction) {
    if (!instance || index < 0 || index >= 2048 || !hookFunc) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 2;
        return false;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);

    void*** pInstanceVptr = reinterpret_cast<void***>(instance);
    void** originalVtable = nullptr;
    if (!SafeReadMemory(pInstanceVptr, &originalVtable, sizeof(originalVtable)) || !originalVtable) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 3;
        return false;
    }

    // Check if already hooked
    for (auto& entry : g_hookedInstances) {
        if (entry.instancePtr == reinterpret_cast<void**>(instance) && entry.hookedIndex == index) {
            if (originalVtable == entry.shadowVtable.data() + 1) {
                void* original = nullptr;
                const auto* slot = reinterpret_cast<const uint8_t*>(entry.originalVtable) +
                    static_cast<size_t>(index) * sizeof(void*);
                if (!SafeReadMemory(slot, &original, sizeof(original)) || !IsExecutableAddress(original)) {
                    if (g_pShm) g_pShm->silentHelper.lastHookStatus = 4;
                    return false;
                }
                originalFunction.store(original, std::memory_order_release);
                return true;
            }
        }
    }

    int totalEntries = CountVmtEntries(originalVtable);

    if (index >= totalEntries) {
        // The requested slot may be beyond the first queried region. Validate
        // every slot through it before copying, rather than indexing unchecked.
        for (int slotIndex = totalEntries; slotIndex <= index; ++slotIndex) {
            void* function = nullptr;
            const auto* slot = reinterpret_cast<const uint8_t*>(originalVtable) +
                static_cast<size_t>(slotIndex) * sizeof(void*);
            if (!SafeReadMemory(slot, &function, sizeof(function)) || !IsExecutableAddress(function)) {
                if (g_pShm) g_pShm->silentHelper.lastHookStatus = 4;
                return false;
            }
        }
        totalEntries = index + 1;
    }

    // Clone VTable including RTTI (1 pointer before the VTable start)
    int requiredSize = totalEntries + 1;

    HookedVmtInstance record;
    record.instancePtr = reinterpret_cast<void**>(instance);
    record.originalVtable = originalVtable;
    record.hookedIndex = index;

    try {
        record.shadowVtable.resize(requiredSize);
    } catch (...) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 5;
        return false;
    }

    // Copy the RTTI slot and the executable entries only after checking the
    // complete source range. This works with MSVC's one-slot RTTI prefix.
    const auto* sourceStart = reinterpret_cast<const uint8_t*>(originalVtable) - sizeof(void*);
    if (!SafeReadMemory(sourceStart, record.shadowVtable.data(),
                        static_cast<size_t>(requiredSize) * sizeof(void*))) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 6;
        return false;
    }

    void* original = nullptr;
    const auto* originalSlot = reinterpret_cast<const uint8_t*>(originalVtable) +
        static_cast<size_t>(index) * sizeof(void*);
    if (!SafeReadMemory(originalSlot, &original, sizeof(original)) || !IsExecutableAddress(original)) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 4;
        return false;
    }
    record.shadowVtable[index + 1] = hookFunc;

    try {
        g_hookedInstances.push_back(std::move(record));
    } catch (...) {
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 5;
        return false;
    }

    HookedVmtInstance& saved = g_hookedInstances.back();
    void** shadowVptr = saved.shadowVtable.data() + 1;
    DWORD oldProtect = 0;
    if (!VirtualProtect(pInstanceVptr, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        g_hookedInstances.pop_back();
        if (g_pShm) g_pShm->silentHelper.lastHookStatus = 7;
        return false;
    }

    // Store the real target before publishing the cloned vtable to game code.
    originalFunction.store(original, std::memory_order_release);
    *pInstanceVptr = shadowVptr;
    DWORD ignoredProtect = 0;
    VirtualProtect(pInstanceVptr, sizeof(void*), oldProtect, &ignoredProtect);

    if (g_pShm) g_pShm->silentHelper.lastHookStatus = 1;
    return true;
}

// ============================================================================
//  Public Lifecycle & Control API
// ============================================================================

bool Initialize(SharedGameData* shm) {
    g_pShm = shm;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.isRedirecting.store(false);
    g_state.hasTarget.store(false);
    g_state.lastRedirectTick = 0;
    g_state.lastDistanceMeters = 0.0;
    g_state.lastFovDegrees = 0.0;
    g_state.lastTriggerActive = false;
    g_state.targetLocation = FVector{};
    g_state.originalLocation = FVector{};
    g_state.originalRotation = FRotator{};
    g_state.redirectedRotation = FRotator{};
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_hookMutex);

    // Restore original VTables for all hooked instances
    for (auto& entry : g_hookedInstances) {
        if (entry.instancePtr && entry.originalVtable) {
            void** currentVtable = nullptr;
            if (!SafeReadMemory(entry.instancePtr, &currentVtable, sizeof(currentVtable)) ||
                currentVtable != entry.shadowVtable.data() + 1) {
                continue;
            }
            DWORD oldProtect = 0;
            if (VirtualProtect(entry.instancePtr, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *entry.instancePtr = entry.originalVtable;
                DWORD ignoredProtect = 0;
                VirtualProtect(entry.instancePtr, sizeof(void*), oldProtect, &ignoredProtect);
            }
        }
    }
    g_hookedInstances.clear();

    g_origGetPlayerViewPoint.store(nullptr, std::memory_order_release);
    g_origGetViewPoint.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> stateLock(g_stateMutex);
        g_state.isRedirecting.store(false);
        g_state.hasTarget.store(false);
    }
    g_pShm = nullptr;
}

void Tick() {
    if (g_pShm && g_pShm->magic == EXT_SHM_MAGIC && g_pShm->version >= 2) {
        auto& ipc = g_pShm->silentHelper;
        Config config = GetConfigSnapshot();
        config.enabled = (ipc.enabled != 0);
        config.requireKey = (ipc.requireKey != 0);
        config.triggerKey = static_cast<int>(ipc.triggerKey);
        config.silentCamera = (ipc.silentCamera != 0);
        config.targetZOffset = ipc.targetZOffset;
        if (ipc.maxFovDegrees > 0.0) {
            config.maxFovDegrees = ipc.maxFovDegrees;
        }
        if (ipc.maxDistanceMeters > 0.0) {
            config.maxDistanceMeters = ipc.maxDistanceMeters;
        }
        SetConfig(config);

        // Automatic dynamic hooking of active player controller and local player
        static void* s_lastHookedController = nullptr;
        static void* s_lastHookedLocalPlayer = nullptr;
        static uint64_t s_lastControllerHookAttempt = 0;
        static uint64_t s_lastLocalPlayerHookAttempt = 0;
        uint64_t nowTick = GetTickCount64();

        void* currentController = reinterpret_cast<void*>(ipc.playerControllerAddr);
        void* currentLocalPlayer = reinterpret_cast<void*>(ipc.localPlayerAddr);

        if (currentController && (currentController != s_lastHookedController ||
            (!g_origGetPlayerViewPoint.load(std::memory_order_acquire) && nowTick - s_lastControllerHookAttempt > 2000))) {
            s_lastControllerHookAttempt = nowTick;
            if (HookPlayerController(currentController)) {
                s_lastHookedController = currentController;
            }
        }

        if (currentLocalPlayer && (currentLocalPlayer != s_lastHookedLocalPlayer ||
            (!g_origGetViewPoint.load(std::memory_order_acquire) && nowTick - s_lastLocalPlayerHookAttempt > 2000))) {
            s_lastLocalPlayerHookAttempt = nowTick;
            if (HookLocalPlayer(currentLocalPlayer)) {
                s_lastHookedLocalPlayer = currentLocalPlayer;
            }
        }

        g_pShm->hooksInstalled = g_origGetPlayerViewPoint.load(std::memory_order_acquire) ? 1 : 0;

        if (ipc.hasTarget != 0) {
            SetTargetLocation(FVector(ipc.targetX, ipc.targetY, ipc.targetZ), true);
        } else {
            SetTargetLocation(FVector(0, 0, 0), false);
        }

    }

    // Clear stale redirect state and restore ControlRotation when redirect ends.
    const Config config = GetConfigSnapshot();
    const bool keyHeld = config.triggerKey != 0 && (GetAsyncKeyState(config.triggerKey) & 0x8000) != 0;
    const bool triggerActive = TriggerState::IsTriggerActive(config.triggerKey, config.requireKey, keyHeld);
    bool redirecting = false;
    FRotator redirectedRotation;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!config.enabled || !g_state.hasTarget.load() || !triggerActive) {
            g_state.isRedirecting.store(false);
        }
        redirecting        = g_state.isRedirecting.load();
        redirectedRotation = g_state.redirectedRotation;
    }

    if (g_pShm && g_pShm->magic == EXT_SHM_MAGIC && g_pShm->version >= 2) {
        auto& ipc = g_pShm->silentHelper;
        ipc.isRedirecting     = redirecting ? 1 : 0;
        ipc.lastRedirectPitch = redirectedRotation.Pitch;
        ipc.lastRedirectYaw   = redirectedRotation.Yaw;
    }
}

void SetConfig(const Config& newConfig) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config = newConfig;
}

Config GetConfig() {
    return GetConfigSnapshot();
}

void SetTargetLocation(const FVector& target, bool valid) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.targetLocation = target;
    g_state.hasTarget.store(valid);
}

bool HookPlayerController(void* pPlayerController, int vmtIndex) {
    if (!pPlayerController) return false;
    const Config config = GetConfigSnapshot();
    // Default UE5 AController::GetPlayerViewPoint index (0x103 = 259, offset 0x818 in UE 5.3)
    int index = (vmtIndex >= 0) ? vmtIndex : 0x103;
    if (config.vmtIndexPlayerController > 0) {
        index = config.vmtIndexPlayerController;
    }

    return InstallShadowVmtHook(pPlayerController, index,
        reinterpret_cast<void*>(Hooked_GetPlayerViewPoint), g_origGetPlayerViewPoint);
}

bool HookLocalPlayer(void* pLocalPlayer, int vmtIndex) {
    if (!pLocalPlayer) return false;
    const Config config = GetConfigSnapshot();
    // Default UE5 ULocalPlayer::GetViewPoint index (0x56 = 86, offset 0x2B0 in UE 5.3)
    int index = (vmtIndex >= 0) ? vmtIndex : 0x56;
    if (config.vmtIndexLocalPlayer > 0) {
        index = config.vmtIndexLocalPlayer;
    }

    return InstallShadowVmtHook(pLocalPlayer, index,
        reinterpret_cast<void*>(Hooked_GetViewPoint), g_origGetViewPoint);
}

} // namespace SilentHelper
