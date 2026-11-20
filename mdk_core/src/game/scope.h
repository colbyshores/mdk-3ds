#pragma once

// ============================================================================
// scope.h — sniper mode's scope (src/Mdk.Game/Kurt/Scope.cs; 0x573a60, the
// controls 0x467384): looking around, the zoom, the clip of 3 rounds and where a
// point shows through the scope.
//
//   turn/tilt keys ──► look speeds ──► yaw, pitch (±50°)     slower when zoomed in
//   zoom keys, wheel ──► zoom speed ──► zoom (1 ... limit)    focal length 384 / zoom
//   clip timer: 3 ──(4/s)──► 0: loaded, fire ──► +1 (a quarter of a second) ... empty ──► 3
//
// Player owns one (the C#'s Kurt.Scope) and drives it from its sniper step
// (game/player_sniper.cpp). The view the scope produces is follow_camera.h's
// buildScopeCamera / buildSniperCamera (the C#'s CameraMath.ScopeView and
// FollowCamera.SniperView).
//
// THE SEAMS FOR WHAT IS NOT HERE.
//   * The rounds (gap package #6, scripts/sniper_rounds.h): updateClip asks its
//     FireHook whether a round slot is free, exactly where the C# calls
//     Kurt.SniperFire (ScriptRuntime.FireSniper). A null hook is the C#'s
//     `SniperFire ?? (_ => true)`: every shot finds a slot. The scripts runtime
//     binds it through Player::setSniperHooks, and its UpdateSniperTarget writes
//     `zoomLimit` each tick through toScreen / locks / lockLimit below.
//   * The overlay and the frontend's view (#7, Hud/SniperOverlay.cs,
//     SniperView.cs): everything they read is public state here -- zoom (the
//     percentage and the mixer's ScopeZoom), pitch, clipRounds and clipTime (the
//     clip inset), and Kurt's selected ammo and ammo counts.
//
// TRIG. Eye and Direction call MathF.Cos/Sin in the C#; this port calls libm
// cosf/sinf, which match them bit for bit on the host (golden/L*_sniper.txt).
// Not the LUT: the line of sight is gameplay (#6 fires the rounds along it, and
// it decides the target lock), and the LUT is not bit-identical. Cold path: a
// handful of calls per step, not per object.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"

namespace mdk {

class Kurt;

// System.Numerics' Vector3 operations as the .NET 10 runtime computes them on
// the oracle's host (an fmaf chain for the dot, a true divide for Normalize).
// The scope's ToScreen and the scope view's CreateLookAt are built from these,
// so the matrices match the oracle's bits.
namespace numerics {
float dot(const Vec3& a, const Vec3& b);
Vec3 cross(const Vec3& a, const Vec3& b);
Vec3 normalize(const Vec3& v);
} // namespace numerics

class Scope {
public:
    enum Turbo { TurboOff, TurboOn };
    // Whether the zoom moves this step (the ZOOM sound loops meanwhile).
    enum Zooming { Still, Moving };
    // What the clip did this step (flags).
    enum Clip { ClipNone = 0, ClipLoaded = 1, ClipFired = 2 };
    // The fire key's state.
    enum Trigger { Released, Held };

    // Zoom (0x57391c, the inverse of the magnification): 1 on entering, down to
    // 0.25 (4x).
    static constexpr float kZoomMin = 0.25f;
    static constexpr float kPitchLimit = 50.0f;
    static constexpr int32_t kClipSize = 3;
    // Normal bullets: as many as wanted.
    static constexpr int32_t kBullets = 0;
    // Bones' air strike, the last ammo type.
    static constexpr int32_t kStrike = 5;

    // The scope's projection (0x57428c): a 384x280 viewport at (127, 139) of the
    // 640x480 screen, centred on (319, 279); the focal length is 384 / zoom pixels.
    static constexpr float kFocal = 384.0f;
    static constexpr float kCentreX = 319.0f;
    static constexpr float kCentreY = 279.0f;
    static constexpr float kScreenHeight = 480.0f;

    // The rounds' slot test (C# Kurt.SniperFire, a Func<int, bool>; §10 forbids the
    // delegate): fire a round of `type`, false when no round slot is free.
    struct FireHook {
        void* context = nullptr;
        bool (*fire)(void* context, int32_t type) = nullptr;
    };

    // Degrees, positive looks down (0x573918).
    float pitch = 0.0f;
    float zoom = 1.0f;
    // How far the zoom goes: kZoomMin, or less on a far locked target (#6's
    // UpdateSniperTarget writes it).
    float zoomLimit = kZoomMin;
    int32_t clipRounds = 0;
    float clipTime = 0.0f;

    // Scope.Reset: entering sniper mode -- level, zoomed out, an empty clip.
    void reset();

    // Scope.Look: the turn (right positive) and tilt (down positive) keys, and the
    // mouse (degrees at zoom 1), turn the view. Returns the yaw change in degrees.
    float look(float turn, float tilt, float mouseX, float mouseY, Turbo turbo, float delta);

    // Scope.Wheel: wheel notches (positive away from the user: zoom in) hold a zoom
    // key for a while.
    void wheel(float notches);

    // Scope.UpdateZoom (0x4687a4): direction -1 zooms in, 1 out.
    Zooming updateZoom(float direction, float delta);

    // Scope.UpdateClip (0x41eb10, 0x461e88): loads, reloads and fires. Returns Clip
    // flags. `inventory` is the combat half of Kurt (the C#'s Kurt.Inventory).
    int32_t updateClip(Kurt& inventory, Trigger trigger, const FireHook& fire, float delta);

    // Scope.SelectAmmo (0x46c900): the next (1) or previous (-1) ammo type with
    // rounds; the clip reloads.
    void selectAmmo(Kurt& inventory, int32_t step);

    // Scope.Eye: 4 above the feet, and forward by 5 (1 - cos pitch) looking down.
    Vec3 eye(const Vec3& feet, const Vec3& facing) const;
    // Scope.Forward: the line of sight for a yaw (degrees) and this pitch.
    Vec3 forward(float yaw) const { return direction(yaw, pitch); }
    // Scope.Direction: a direction from a yaw and a pitch (degrees, positive down).
    static Vec3 direction(float yaw, float pitch);

    // Scope.ToScreen: where a point shows on the 640x480 screen, seen from `eye`
    // along `forward`; false behind the eye. (#6's target lock.)
    static bool toScreen(const Vec3& eye, const Vec3& forward, float zoom, const Vec3& point,
                         Vec2* screen);
    // Scope.Locks: whether a screen box overlaps the lock square around the crosshair.
    static bool locks(const Vec2& min, const Vec2& max);
    // Scope.LockLimit: the zoom limit on a locked target of this height at this distance.
    static float lockLimit(float height, float distance);

private:
    static float accelerate(float speed, float input, Turbo turbo, float delta);

    float lookSpeedX_ = 0.0f;
    float lookSpeedY_ = 0.0f;
    float zoomSpeed_ = 0.0f;
    float wheelTicks_ = 0.0f;
    float wheelDirection_ = 0.0f;
    float sinceShot_ = 3.40282347e+38f;  // float.MaxValue
};

} // namespace mdk
