// ============================================================================
//  SilentHelper — Experimental In-Process ViewPoint Override
//
//  The controller hook changes the rotation returned by
//  APlayerController::GetPlayerViewPoint. Whether a particular game attack
//  consumes that result is call-site specific and is not verified here.
//  The local-player hook restores the original view for camera rendering.
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <atomic>
#include "../shared/shared_memory.h"

namespace SilentHelper {

// ============================================================================
//  UE5 Core Math Types (Double-Precision / Large World Coordinates)
// ============================================================================
struct FVector {
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    FVector() = default;
    FVector(double inX, double inY, double inZ) : X(inX), Y(inY), Z(inZ) {}

    FVector operator-(const FVector& other) const {
        return {X - other.X, Y - other.Y, Z - other.Z};
    }
    FVector operator+(const FVector& other) const {
        return {X + other.X, Y + other.Y, Z + other.Z};
    }
    FVector operator*(double scalar) const {
        return {X * scalar, Y * scalar, Z * scalar};
    }
    double Length() const {
        return std::sqrt(X * X + Y * Y + Z * Z);
    }
    double DistanceTo(const FVector& other) const {
        return (*this - other).Length();
    }
    double Dot(const FVector& other) const {
        return X * other.X + Y * other.Y + Z * other.Z;
    }
};

struct FRotator {
    double Pitch = 0.0;
    double Yaw = 0.0;
    double Roll = 0.0;

    FRotator() = default;
    FRotator(double inP, double inY, double inR = 0.0) : Pitch(inP), Yaw(inY), Roll(inR) {}
};

struct FMinimalViewInfo {
    FVector Location;    // 0x00 (24 bytes)
    FRotator Rotation;   // 0x18 (24 bytes)
    float FOV = 90.0f;   // 0x30 (4 bytes)
    float DesiredFOV = 90.0f;
};

enum EStereoscopicPass : int32_t {
    eSSP_FULL = 0,
    eSSP_LEFT_EYE = 1,
    eSSP_RIGHT_EYE = 2,
    eSSP_MONOSCOPIC_AFTER_PASS = 3,
};

// ============================================================================
//  Configuration & Runtime State
// ============================================================================
struct Config {
    bool enabled = true;
    bool requireKey = false;         // false = always active; true = key must be held
    int triggerKey = 0;              // 0 = keyless/always-active mode unless requireKey is true
    double maxFovDegrees = 90.0;      // Maximum angular FOV from crosshair
    double maxDistanceMeters = 50.0;  // Max range for silent redirection
    bool teamCheck = true;
    bool requireVisible = false;
    int vmtIndexPlayerController = 0x103; // UE5.3 APlayerController::GetPlayerViewPoint (index 259, offset 0x818)
    int vmtIndexLocalPlayer = 0x56;       // UE5.3 ULocalPlayer::GetViewPoint (index 86, offset 0x2B0)
    // silentCamera=true (default): after GetPlayerViewPoint redirects the aim,
    // GetViewPoint restores the natural rotation for the camera renderer so the
    // screen and ESP overlay never show the snapped aim direction.
    // silentCamera=false: skip the restore so the camera visibly follows the
    // redirect (useful for debugging; not recommended for play).
    bool silentCamera = true;
    double targetZOffset = 0.0;  // Vertical offset applied to target position (UE units; negative = aim lower)
};

struct RuntimeState {
    std::atomic<bool> isRedirecting{false};
    std::atomic<bool> hasTarget{false};
    FVector targetLocation{0.0, 0.0, 0.0};
    FVector originalLocation{0.0, 0.0, 0.0};  // saved natural camera position
    FRotator originalRotation{0.0, 0.0, 0.0}; // saved natural camera rotation
    FRotator redirectedRotation{0.0, 0.0, 0.0};
    double lastDistanceMeters = 0.0;
    double lastFovDegrees = 0.0;
    bool lastTriggerActive = false;
    uint64_t lastRedirectTick = 0;
};

// ============================================================================
//  Function Hook Signatures
// ============================================================================
using fn_GetPlayerViewPoint_t = void(__fastcall*)(void* this_PlayerController, FVector* Location, FRotator* Rotation);
using fn_GetViewPoint_t       = void(__fastcall*)(void* this_LocalPlayer, FMinimalViewInfo* OutViewInfo, int StereoPass);

// ============================================================================
//  Public Lifecycle & Control API
// ============================================================================

// Initialize SilentHelper module with optional shared memory reference
bool Initialize(SharedGameData* shm = nullptr);

// Shutdown and safely restore all hooked VMT pointers
void Shutdown();

// Periodic update / maintenance tick (called from proxy thread)
void Tick();

// Update configuration
void SetConfig(const Config& newConfig);
Config GetConfig();

// Set target bone location from internal scanner or external relay
void SetTargetLocation(const FVector& target, bool valid = true);

// Install VMT / Detour hooks on specific object instances
bool HookPlayerController(void* pPlayerController, int vmtIndex = -1);
bool HookLocalPlayer(void* pLocalPlayer, int vmtIndex = -1);

// Math helpers
FRotator CalculateLookAtRotation(const FVector& source, const FVector& target);
double CalculateFOV(const FRotator& currentRot, const FRotator& targetRot);
void NormalizeRotator(FRotator& rot);

} // namespace SilentHelper
