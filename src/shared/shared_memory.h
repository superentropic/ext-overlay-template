// ============================================================================
//  Shared Memory IPC — Proxy (in-process) <-> External Tool
//
//  The version.dll proxy runs INSIDE the game process and has full memory
//  access. tvk.sys strips PROCESS_VM_READ from external handles via
//  ObRegisterCallbacks, but code running in-process can read freely.
//
//  Two channels:
//    1. SharedGameInfo — proxy writes game base, PID, and key pointers
//    2. MemoryRelay — external tool requests reads, proxy fulfills them
//    3. SilentHelperIPC — passes real-time bone aim coordinates and config
//
//  Named section: "Local\\DaDResearchShm"
// ============================================================================
#pragma once

#include <cstdint>

// Shared memory section name — must match on both sides
#define EXT_SHM_NAME "Local\\DaDResearchShm"

// Magic value to verify shared memory is initialized
#define EXT_SHM_MAGIC 0x44414452  // "DADR"

// Protocol version — bump on layout changes
#define EXT_SHM_VERSION 5

// Memory relay: max bytes per read request
#define EXT_RELAY_MAX_SIZE 0x4000  // 16 KB per request (enough for actor arrays)

// ============================================================================
//  Memory relay request/response
//  External tool writes a request, proxy fulfills it in-place
// ============================================================================
#pragma pack(push, 1)

struct MemoryRelaySlot {
    volatile uint32_t state;       // 0=idle, 1=request pending, 2=response ready, 3=error
    uint64_t address;              // Address to read from (in game process)
    uint32_t size;                 // Bytes to read (max EXT_RELAY_MAX_SIZE)
    uint8_t  data[EXT_RELAY_MAX_SIZE]; // Response data written by proxy
};

// Multiple relay slots for concurrent reads (reduces round-trip latency)
#define EXT_RELAY_SLOTS 4

// In-process SilentHelper IPC bridge structure
struct SilentHelperIPC {
    // ── Inputs (Written by External Tool -> Read by In-Process Proxy) ──
    volatile uint32_t enabled;              // 1 = enabled, 0 = disabled
    volatile uint32_t requireKey;           // 1 = key hold required, 0 = always active
    volatile uint32_t triggerKey;           // Virtual key code (e.g. VK_RBUTTON or VK_LBUTTON)
    volatile double   maxFovDegrees;        // Maximum angular FOV
    volatile double   maxDistanceMeters;    // Maximum target range in meters
    volatile uint32_t targetBone;           // 0 = Head, 1 = Neck, 2 = Chest, 3 = Nearest

    // Target coordinate feed from overlay
    volatile uint32_t hasTarget;            // 1 = valid target active, 0 = no target
    volatile double   targetX;
    volatile double   targetY;
    volatile double   targetZ;
    volatile uint64_t targetTimestamp;      // GetTickCount64 timestamp

    // Active instance pointer feed for in-process VMT hooking
    volatile uint64_t playerControllerAddr; // Active APlayerController* address
    volatile uint64_t localPlayerAddr;      // Active ULocalPlayer* address

    // ── Behaviour flags (Written by External Tool -> Read by In-Process Proxy) ──
    // silentCamera=1 (default/recommended): redirect happens in GetPlayerViewPoint
    // so the attack trace gets the aim toward target; GetViewPoint then restores
    // the natural rotation for the renderer so the camera never visibly moves.
    // silentCamera=0: redirect still happens in GetPlayerViewPoint but GetViewPoint
    // does NOT restore — the camera snaps visibly toward the target.
    volatile uint32_t silentCamera;
    volatile double   targetZOffset;   // UE units added to target Z coord (negative = aim lower)

    // ── Diagnostics & Feedback (Written by In-Process Proxy -> Read by External Tool) ──
    volatile uint32_t isRedirecting;        // 1 = hook is currently redirecting
    volatile double   lastRedirectPitch;
    volatile double   lastRedirectYaw;
    volatile uint32_t hookCallCountController; // Invocations of GetPlayerViewPoint
    volatile uint32_t hookCallCountLocalPlayer; // Invocations of GetViewPoint
    volatile uint32_t redirectCount;           // Redirections executed
    volatile uint32_t lastHookStatus;          // 0=None, 1=Success, >1 Error code
    volatile uint32_t viewPointRestoreCount;   // Times GetViewPoint restored rotation (diagnostic)
};

struct SharedGameData {
    // ── Header ──
    uint32_t magic;                // Must be EXT_SHM_MAGIC
    uint32_t version;              // Must be EXT_SHM_VERSION
    uint32_t pid;                  // Game process PID
    volatile uint32_t heartbeat;   // Proxy increments this each tick (detect stale)
    uint32_t proxyLoaded;          // 1 when proxy payload is active
    uint32_t hooksInstalled;       // 1 if VMT hooks installed
    uint64_t proxyBase;            // Base address of version.dll in game

    // ── Module info (set once at startup) ──
    uint64_t gameBase;             // Base address of main game executable module
    uint64_t gameSize;             // Size of main game executable module (SizeOfImage)

    // ── SilentHelper IPC bridge ──
    SilentHelperIPC silentHelper;

    // ── Memory relay slots ──
    MemoryRelaySlot relay[EXT_RELAY_SLOTS];
};

#pragma pack(pop)

// Total shared memory size
#define EXT_SHM_SIZE sizeof(SharedGameData)

// Relay slot states
#define EXT_RELAY_IDLE     0
#define EXT_RELAY_REQUEST  1
#define EXT_RELAY_READY    2
#define EXT_RELAY_ERROR    3
