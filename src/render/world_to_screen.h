#pragma once
#include "sdk/ue5_types.h"
#include "sdk/gworld.h"
#include "core/process.h"

class WorldToScreen {
public:
    // Update camera state from game memory (full chain — 7 reads)
    bool UpdateCamera(const Process& proc, GWorldReader& world);

    // Fast camera update — uses cached CameraManager pointer (1 read)
    bool UpdateCameraFast(const Process& proc, GWorldReader& world);

    // Project 3D world position to 2D screen coordinate
    // Returns false if the point is behind the camera
    bool Project(const FVector& worldPos, FVector2D& screenPos) const;

    const FVector& GetCameraPosition() const { return m_cameraPos; }
    const FRotator& GetCameraRotation() const { return m_cameraRot; }
    float GetFOV() const { return m_fov; }

    void SetScreenSize(int width, int height);

private:
    void BuildViewProjectionMatrix();

    FVector m_cameraPos{};
    FRotator m_cameraRot{};
    float m_fov = 90.0f;

    int m_screenWidth = 1920;
    int m_screenHeight = 1080;

    FMatrix m_viewMatrix{};
    FMatrix m_projectionMatrix{};
    FMatrix m_viewProjectionMatrix{};
};
