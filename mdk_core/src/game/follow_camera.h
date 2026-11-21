#pragma once

// ============================================================================
// follow_camera.h — the follow camera's placement (C# FollowCamera).
//
// The third-person camera behind Kurt. The full C# class is coupled to Kurt's
// physics and the BSP camera-clear (ArenaSpace.Sight); those land with the
// collision port. What lands here is the placement math that does not depend
// on them: the camera's place for a pair of feet, a facing and a pitch
// (Point), the view axes for that facing and pitch (Orient), and a helper
// that turns them into the look-at Camera the renderer consumes.
//
// Pitch is in RADIANS here and POSITIVE LOOKS DOWN, exactly as the C#'s
// Orient: forward = facing*cos(pitch) - Z*sin(pitch).
// ============================================================================

#include "game/input.h"
#include "render/render_types.h"
#include "util/random.h"

namespace mdk {

class ArenaSpace;
class Player;

// The follow camera's constants (C# FollowCamera).
namespace follow_camera {

constexpr float kFieldOfView = 71.5f;
constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;
constexpr float kDistance = 8.0f;
constexpr float kDownBack = 5.0f;
constexpr float kCloseUpPitch = -20.0f;   // degrees
constexpr float kCloseUpRange = 80.0f;
constexpr float kCloseUpBias = 100.0f;
constexpr float kDefaultPitch = 4.0f;     // degrees
// The C#'s FollowCamera reads its pivot off Kurt (Kurt.CameraPivot, default
// Kurt.DefaultPivot), so the live value is Player::cameraPivot; this is the same
// 4.5 the C# ships as Kurt.DefaultPivot, kept for the camera's own default Aim
// and for callers that place a view without a Player.
constexpr float kDefaultPivot = 4.5f;
// The view's pitch stays short of straight up or down: past it the view turns
// over (C# FollowCamera.ViewPitchLimit). The orient's up-vector degenerates at
// exactly 90 degrees.
constexpr float kViewPitchLimit = 89.0f;  // degrees

// The pitch smoothing (C# FollowCamera). The look offset is clamped to this
// range once the arena pitch and the air tilt are taken off it.
constexpr float kMouseDegrees = 0.15f;    // degrees per mouse count
constexpr float kMinPitch = -60.0f;
constexpr float kMaxPitch = 90.0f;
constexpr float kTicks = 30.0f;           // the original's ticks per second
// The arena pitch eases in by 0.85·old + 0.15·new per tick.
constexpr float kPitchEase = 0.85f;
// Air time starts below this fall speed; the tilt grows by 0.667·Ticks per
// second up to 40, and decays by 40 per second back on the ground.
constexpr float kFallStartSpeed = -16.0f;
constexpr float kAirPitchRate = 0.667f * 30.0f;
constexpr float kAirPitchMax = 40.0f;
constexpr float kAirPitchDecay = 40.0f;
// The climb: the smoothed rise of the feet per tick, clamped to ±0.5, eased by
// 0.97·old + 0.03·new, returning by 0.02 per tick; 40 degrees per unit.
constexpr float kClimbClamp = 0.5f;
constexpr float kClimbKeep = 0.97f;
constexpr float kClimbReturn = 0.02f;
constexpr float kClimbPitch = 40.0f;
// The screen shake (C# FollowCamera, 0x573aa8; package #15): each tick above 1 the
// view turns by up to ±1.64 × the shake in pixels of the 360-high view (at most ±19
// across, ±59 up and down); it drains by 0.25 per tick. 16384 × 0.0001 is a power-of-two
// scaling of 0.0001f, so the product is exact either way it is folded.
constexpr float kShakeScale = 16384.0f * 0.0001f;
constexpr float kShakeLimitX = 19.0f;
constexpr float kShakeLimitY = 59.0f;
constexpr float kShakeDrain = 0.25f;
constexpr float kShakeMin = 1.0f;
constexpr float kViewHeight = 360.0f;

// C# float.DegreesToRadians (Single.DegreesToRadians): multiply by the float
// pi, then divide by 180 -- TWO roundings, left-associative. This is the one
// spelling for the follow camera and the Player's facing (the debris and script
// trees keep their own separately pinned copies); the pre-folded
// `degrees * (pi / 180)` form differs from the oracle by 1 ULP on many inputs.
inline float degreesToRadians(float degrees) {
    constexpr float kPi = 3.14159265358979323846f;
    return degrees * kPi / 180.0f;
}

} // namespace follow_camera

// The camera's place for the feet, the horizontal facing (unit) and the pitch
// in radians (C# FollowCamera.Point).
Vec3 followCameraPoint(const Vec3& feet, const Vec3& facing, float pitch, float pivot);

// The view axes for the facing and pitch in radians (C# FollowCamera.Orient).
void followCameraOrient(const Vec3& facing, float pitch, Vec3* forward, Vec3* up);

// The roll half of C# FollowCamera.Turn (FollowCamera.cs:196-212): swing the up
// vector about the forward by rollDegrees (positive banks the view to the right;
// Kurt's moves write it). Forward is untouched, so the picture banks without
// turning, and a roll of exactly zero leaves the axes alone exactly as the C#'s
// early return does. followCameraTurn below is the whole of Turn, shake included.
void followCameraRoll(const Vec3& forward, Vec3* up, float rollDegrees);

// C# FollowCamera.Turn whole (FollowCamera.cs:194-213; package #15): first the shake
// turns the view by `shakeX` across and `shakeY` up (radians) -- forward leans along
// Orient's right and up by their tangents, then right and up are made square to it again
// -- then the roll, about the turned axes. With no shake it is followCameraRoll exactly
// (the C#'s `shake != Vector2.Zero` test), so a still view is bit-for-bit what it was.
// The camera's tangents go through the project's one trig path (R6), as Orient's do.
void followCameraTurn(Vec3* forward, Vec3* up, float shakeX, float shakeY, float rollDegrees);

// The camera's clearance (C# FollowCamera.Clear, camera_clearance 0x417ee8): a
// wall between Kurt's head (feet + 5.5) and the camera pushes Kurt d·n away
// from it (with a floor there, else half of it to a side), and the camera moves
// as far as Kurt did. May move the player. `space` may be null (no clearance).
// `camera` is the un-cleared place from followCameraPoint; returns the cleared
// place. `clip` is the C#'s ClipView delegate (package #3): the solid objects that
// block the view, then Kurt is shoved toward the clipped end and the camera with
// him. Null is the C#'s null delegate (no object pass).
struct ClipViewHook {
    void* context = nullptr;
    // C# FollowCamera.ClipView (ScriptRuntime.ClipView): where the line of sight from
    // `from` to `to` first meets a blocking object's part, or `to`.
    Vec3 (*clip)(void* context, const Vec3& from, const Vec3& to) = nullptr;
};
Vec3 followCameraClear(ArenaSpace* space, Player& player, const Vec3& camera,
                       const ClipViewHook* clip = nullptr);

// Fills camera with the follow camera's view for the feet, the horizontal
// facing (unit), the pitch in DEGREES (positive looks down) and the pivot, at
// the game's FOV/near/far. `rollDegrees` is Kurt's CameraRoll (0 for a view that
// has not banked). Returns false if the look-at is degenerate.
bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect,
                       float rollDegrees);

// As buildFollowCamera, but placed from the player and run through the camera
// clearance (C# FollowCamera.Update: Point then Clear). The clearance may move
// the player (Kurt is shoved off a wall) and the camera with him.
// `shakeX`/`shakeY` are the shake's turn in radians (followCameraTurn), 0 for none.
bool buildFollowCameraCleared(Camera* camera, ArenaSpace* space, Player& player,
                              float pitchDegrees, float pivot, float aspect,
                              float rollDegrees, const ClipViewHook* clip = nullptr,
                              float shakeX = 0.0f, float shakeY = 0.0f);

// ---- Sniper mode's view (FollowCamera.cs:381-430) ----
// The frontends draw the world through these while Player::sniping() (C#
// Viewer.SceneView); the scope's overlay over it is gap package #7's. The
// matrices are the oracle's to the bit (golden/L*_sniper.txt): the axes and the
// look-at are built with System.Numerics' arithmetic (game/scope.h numerics::),
// not buildLookAtCamera's reciprocal normalize, and the trig is libm's (Scope).
//
// C# FollowCamera.UpOf: the camera's up for a line of sight (no roll).
Vec3 followCameraUpOf(const Vec3& forward);

// C# CameraMath.ScopeProjection (0x57428c): the scope's off-centre perspective for
// a window of `aspect` -- the focal length is 384 / zoom pixels of the 480-high
// screen, and the frustum is shifted down so the scope's centre (y 279) is on the
// line of sight:
//
//   y 0   ┌───────────┐  top    =  279 · near / focal
//         │     +     │  ◄── line of sight, y 279
//   y 480 └───────────┘  bottom = -201 · near / focal
//
// `out` is row-major, row-vector (Matrix4x4.CreatePerspectiveOffCenter's layout).
void scopeProjection(float zoom, float aspect, float nearPlane, float farPlane, float out[16]);

// C# CameraMath.ScopeView: the view from `eye` along `forward` through the scope.
// Fills the camera's view (CreateLookAt(eye, eye + forward, UpOf(forward))), its
// projection (scopeProjection), position, forward, up and planes. The C#'s View also
// carries the product and the sky's ClipToDirection; the port's renderers form
// those from these fields, as they do for the follow camera. False when the line of
// sight is degenerate (no up).
bool buildScopeCamera(Camera* camera, const Vec3& eye, const Vec3& forward, float zoom,
                      float aspect, float nearPlane, float farPlane);

// C# FollowCamera.SniperView: Kurt's scope view (his sniper eye and line of sight,
// the scope's zoom) at the follow camera's near and far planes.
bool buildSniperCamera(Camera* camera, const Player& player, float aspect);

// The follow camera's smoothing state (C# FollowCamera). It owns the eased arena
// pitch, the mouse's look offset, the air tilt and the climb, and places the
// view each step. Kurt's roll is applied here too, read off the Player the way the
// C# reads it off Kurt. The between-step blend is ported for the camera's own
// Aim triple; wiring it to a frontend still needs FrameBlend's Kurt pose. Its
// sniper guards are ported (no clearance and no mouse look while Kurt snipes; the
// scope's own view is buildSniperCamera above). The shake is ported (package #15:
// RaiseShake, UpdateShake and Turn's shake half, in update and blend), and so is the
// bomber's top-down view (LookDown, package #11: while Player::hasTopDownHeight the
// view looks straight down from that height, before any shake, and the mouse does not
// look).
class FollowCamera {
public:
    // C# FollowCamera.RaiseShake (FollowCamera.cs:94, 0x467f7c, opcode 135): shake the
    // screen at least this much. MathF.Max, NaN-propagating as the C#'s.
    void raiseShake(float amount);
    // The scripts' ShakeRaised subscriber (Viewer.cs:238, `ShakeRaised +=
    // _follow.RaiseShake`): bound with ScriptRuntime::setShakeRaisedHook, `camera` is the
    // FollowCamera. Its address must outlive the binding (the frontends' camera does).
    static void raiseShakeHook(void* camera, float amount);
    // The shake's random: the C#'s `_random` is a time-seeded `new Random()`, so its
    // offsets are not reproducible there; here it is the oracle's seeded generator
    // (util/random.h) with a seed the caller injects. The frontends seed it as they seed
    // the scripts (a clock, or a fixed one in harness runs); a fresh camera has seed 0.
    void seedShake(int32_t seed) { _random = Random(seed); }
    // The shake left (C#'s private _shake) and this tick's offset (C# ShakeOffset, whole
    // pixels of the 360-high view), for the goldens.
    float shake() const { return _shake; }
    float shakeOffsetX() const { return _shakeOffsetX; }
    float shakeOffsetY() const { return _shakeOffsetY; }

    // The per-frame mouse look up/down (C# FollowCamera.MouseLook): the look
    // offset, clamped against the eased arena pitch and the air tilt.
    void mouseLook(Input& input);
    // C# FollowCamera.MouseLook(kurt, input) whole: in sniper mode the mouse turns the
    // scope instead (Player::mouseLook took it), and in the bomber's top-down view the
    // mouse moves the cursor (scripts/bomber.cpp), so the look offset is left alone.
    void mouseLook(const Player& player, Input& input);

    // A frame between two steps (C# FollowCamera.Blend): place the camera for
    // Kurt as drawn (`feet`, `yaw`) and blend the camera's pitch, pivot and roll
    // from the last two steps by `alpha`. The feet and yaw are supplied by the
    // caller because the oracle gets them from FrameBlend's Kurt pose. Returns
    // false before the first step (there is no Aim to blend) or when the
    // look-at is degenerate; the camera is left alone in that case.
    bool blend(Camera* camera, const Vec3& feet, float yaw, float alpha, float aspect);
    // C# FollowCamera.Blend(kurt, ...) whole: in the bomber's top-down view
    // (Player::hasTopDownHeight) the view is straight down from that height above the
    // drawn feet, his drawn heading at the top (FollowCamera.cs:174-181); otherwise the
    // blend above.
    bool blend(Camera* camera, const Player& player, const Vec3& feet, float yaw, float alpha,
               float aspect);

    // The eased arena pitch, in degrees. This is the camera's own state, exposed
    // read-only so the host harness can pin the oracle's exact rounding; no
    // gameplay path needs to read it back.
    float arenaPitch() const { return _arenaPitch; }

    // The air tilt, in degrees. Read-only for the same reason as arenaPitch(): the
    // host harness pins the oracle's NaN propagation at this camera's MathF.Max.
    float airPitch() const { return _airPitch; }

    // The smoothed rise of the feet per tick (C# FollowCamera._climb). Read-only for the
    // host harness: the ledge goldens (package #12) pin it through a hang and a climb.
    float climb() const { return _climb; }

    // One step of the climb (C# FollowCamera.Climb). The oracle's method is public,
    // so this one is too; the host harness uses it to pin the MathF.Min/Max arms.
    static float climbStep(float climb, float rise, float ticks);

    // One step (C# FollowCamera.Update): ease the arena pitch toward its target,
    // update the air time and the climb, compute the pitch, then place the camera
    // and run the clearance (buildFollowCameraCleared). `arenaPitchTarget` is the
    // pitch of the arena Kurt is in (the C# PitchGoal). In the bomber's top-down view
    // (Player::hasTopDownHeight) it is LookDown alone, and nothing else moves.
    void update(Camera* camera, ArenaSpace* space, Player& player, float arenaPitchTarget,
                float delta, float aspect);

    // C# FollowCamera.ClipView (FollowCamera.cs:87), set by the frontend to the scripts
    // runtime's ClipView (Viewer.cs:237). Left empty, there is no object pass.
    ClipViewHook clipView;

private:
    struct Aim {
        float pitch = 0.0f;
        float pivot = follow_camera::kDefaultPivot;
        float roll = 0.0f;
    };

    void updateAirTime(const Player& player, float delta);
    void updateClimb(float feetZ, float delta);
    // C# FollowCamera.UpdateShake (FollowCamera.cs:216-242).
    void updateShake(float delta);
    // The shake's turn this tick in radians (ShakeOffset × DegreesToRadians(FieldOfView)
    // / ViewHeight).
    float shakeTurnX() const;
    float shakeTurnY() const;

    float _arenaPitch = follow_camera::kDefaultPitch;
    float _lookOffset = 0.0f;
    float _airPitch = 0.0f;
    float _airTime = 0.0f;
    float _climb = 0.0f;
    bool _haveLastFeetZ = false;
    float _lastFeetZ = 0.0f;
    float _climbTime = 0.0f;
    float _shake = 0.0f;
    float _shakeTime = 0.0f;
    float _shakeOffsetX = 0.0f;
    float _shakeOffsetY = 0.0f;
    Random _random{0};
    Aim _previousAim;
    Aim _aim;
    bool _haveAim = false;
};

} // namespace mdk
