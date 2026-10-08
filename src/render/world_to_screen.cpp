#include "world_to_screen.h"
#include <cmath>
#include <spdlog/spdlog.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool WorldToScreen::UpdateCamera(const Process& proc, GWorldReader& world) {
    FMinimalViewInfo pov = world.GetCameraInfo(proc);

    static int camDiag = 0;
    bool doDiag = (camDiag++ % 300 == 0);

    if (doDiag) {
        spdlog::info("[W2S] Camera raw: pos=({:.1f}, {:.1f}, {:.1f}) rot=({:.1f}, {:.1f}, {:.1f}) FOV={:.1f}",
            pov.Location.X, pov.Location.Y, pov.Location.Z,
            pov.Rotation.Pitch, pov.Rotation.Yaw, pov.Rotation.Roll,
            pov.FOV);
    }

    // Validate the data
    if (pov.FOV <= 0.0f || pov.FOV > 180.0f) {
        if (doDiag) spdlog::warn("[W2S] Camera FOV invalid: {:.1f}, skipping", pov.FOV);
        return false;
    }

    if (pov.Location.X == 0.0 && pov.Location.Y == 0.0 && pov.Location.Z == 0.0) {
        if (doDiag) spdlog::warn("[W2S] Camera position is (0,0,0) - offset chain may be broken!");
    }

    m_cameraPos = pov.Location;
    m_cameraRot = pov.Rotation;
    m_fov = pov.FOV;

    return true;
}

bool WorldToScreen::UpdateCameraFast(const Process& proc, GWorldReader& world) {
    FMinimalViewInfo pov = world.GetCameraInfoFast(proc);

    if (pov.FOV <= 0.0f || pov.FOV > 180.0f) return false;

    m_cameraPos = pov.Location;
    m_cameraRot = pov.Rotation;
    m_fov = pov.FOV;

    return true;
}

bool WorldToScreen::Project(const FVector& worldPos, FVector2D& screenPos) const {
    // =========================================================================
    // Standard UE World-to-Screen using direct vector math
    // UE coordinate system: X=Forward, Y=Right, Z=Up
    // Rotation: Pitch=around Y, Yaw=around Z, Roll=around X
    // FOV from CameraManager is HORIZONTAL FOV in degrees
    // =========================================================================

    float pitch = static_cast<float>(m_cameraRot.Pitch * M_PI / 180.0);
    float yaw   = static_cast<float>(m_cameraRot.Yaw * M_PI / 180.0);

    float sp = sinf(pitch), cp = cosf(pitch);
    float sy = sinf(yaw),   cy = cosf(yaw);

    // Forward vector (camera look direction)
    float fwdX = cp * cy;
    float fwdY = cp * sy;
    float fwdZ = sp;

    // Right vector (perpendicular to forward in XY plane, then adjusted for pitch)
    float rightX = -sy;
    float rightY = cy;
    float rightZ = 0.0f;

    // Up vector (cross product of right x forward)
    float upX = -(sp * cy);
    float upY = -(sp * sy);
    float upZ = cp;

    // Delta from camera to target
    float dx = static_cast<float>(worldPos.X - m_cameraPos.X);
    float dy = static_cast<float>(worldPos.Y - m_cameraPos.Y);
    float dz = static_cast<float>(worldPos.Z - m_cameraPos.Z);

    // Project delta onto camera axes
    float dot_fwd   = dx * fwdX   + dy * fwdY   + dz * fwdZ;
    float dot_right = dx * rightX + dy * rightY + dz * rightZ;
    float dot_up    = dx * upX    + dy * upY    + dz * upZ;

    // Behind camera check
    if (dot_fwd < 0.001f) return false;

    // FOV scaling — UE FOV is horizontal
    float fovRad = static_cast<float>(m_fov * M_PI / 180.0);
    float tanHalfFov = tanf(fovRad / 2.0f);

    // Perspective projection
    float screenCenterX = m_screenWidth / 2.0f;
    float screenCenterY = m_screenHeight / 2.0f;

    // Horizontal: screen_x = center + (right / forward) * (center / tan(hfov/2))
    screenPos.X = screenCenterX + (dot_right / dot_fwd) * (screenCenterX / tanHalfFov);

    // Vertical: screen_y = center - (up / forward) * (center / tan(hfov/2))
    // Note: we use the SAME tanHalfFov for both axes because UE FOV is horizontal,
    // and the aspect ratio is handled by using screenCenterX for X and screenCenterY for Y
    // Actually, for vertical we need to account for aspect ratio:
    // vfov = 2 * atan(tan(hfov/2) * height/width)
    float aspectRatio = static_cast<float>(m_screenWidth) / static_cast<float>(m_screenHeight);
    screenPos.Y = screenCenterY - (dot_up / dot_fwd) * (screenCenterX / tanHalfFov);

    // Bounds check
    return screenPos.X >= -200.0f && screenPos.X <= m_screenWidth + 200.0f &&
           screenPos.Y >= -200.0f && screenPos.Y <= m_screenHeight + 200.0f;
}

void WorldToScreen::SetScreenSize(int width, int height) {
    m_screenWidth = width;
    m_screenHeight = height;
}

void WorldToScreen::BuildViewProjectionMatrix() {
    // No longer used — using direct vector projection in Project()
}
