#pragma once

// ============================================================================
// fall_layout.h — the arithmetic of the fall's view that draws nothing.
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallView.cs: these are its static
// helpers and constant tables (RadarRing, NearCamera, Orient, the haze's
// HazeBlend, the radar's colours, the explosions' growth, Kurt's turn), lifted
// out of fall_view.cpp so the goldens can pin them without a renderer
// (tests/scripts/skydive_dump.cpp's `view` mode against fall_probe's). The
// view (fall_view.h) is their only other caller.
//
// Cold (once a frame, a handful of models): AGENTS.md §2. The trig here is
// presentation, once per pickup or explosion a frame, never in a loop of 256.
// No virtuals, no templates (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "formats/vector_types.h"
#include "util/matrix.h"

namespace mdk {
namespace fall {

// FallView's constants the frontends and the goldens share.
constexpr float kViewFocal = 250.0f;
constexpr float kViewHeight = 360.0f;
constexpr float kViewCentreY = kViewHeight / 2.0f;
constexpr float kViewNear = 0.5f;
constexpr float kViewFar = 8000.0f;
// The 2D layers far below the camera: the ground at 7000, 10 units a layer.
constexpr float kBackDepth = 7000.0f;
constexpr float kLayerStep = 10.0f;
// The haze's blend offsets b (1-8), the radar's rings and sides.
constexpr int32_t kHazeLevels = 8;
constexpr int32_t kHazeTables = 24;
constexpr int32_t kRadarRings = 4;
constexpr int32_t kRadarSides = 6;
constexpr int32_t kRadarColours = 5;
// The chute is drawn 2 units above its pickup.
constexpr float kChuteAbove = 2.0f;

// FallView.HazeBlend[clamp(level + b, 1, 24) - 1] / 256: the alpha of the haze's
// blend offset `b` (1-8) at the wind level `level` (FallSim::wind() >> 8).
float hazeAlpha(int32_t level, int32_t b);

// FallView.RadarAlpha[c] / 256: the beam's band `c` (0 the fan, 1-3 between the
// rings, 4 the cap), translucent green.
float radarAlpha(int32_t band);

// FallView.NearCamera: whether a model or trail point is too near the camera to
// draw (a missile past Kurt, a pickup dropped above him would fill the view).
bool nearCamera(const formats::Vec3f& position, const formats::Vec3f& camera);

// FallView.Orient: points the model's +y along a direction, z up (0x4123e8).
Mat4 orient(const formats::Vec3f& direction);

// FallView.RadarRing: the centre and radius of the beam's ring `ring` (0-3):
// rings 1-3 halve the way left to the spot, ring 4 is the spot (0x412af8).
void radarRing(const formats::Vec3f& basePoint, const formats::Vec3f& spot, float radius,
               int32_t ring, formats::Vec3f* centre, float* ringRadius);

// FallView.KurtTurn: Kurt lies face down (+0x4c yaw 90°, +0x13c pitch −90°):
// CreateRotationY(π/2) * CreateRotationZ(π/2).
Mat4 kurtTurn();

// FallView.ExplosionTurn: the explosion is pitched 90 degrees (+0x13c) before
// its random yaw, CreateRotationY(-pi/2): EXPLODE is a fireball thin along x,
// so this turns its face to the camera above (upstream 8bc42528).
Mat4 explosionTurn();

// System.Numerics' builders, row-vector convention (render_types.h's).
Mat4 rotationZ(float radians);
Mat4 scaling(float scale);
Mat4 translation(const formats::Vec3f& at);

// float.DegreesToRadians: (degrees × π) / 180, in that order (a pickup's yaw).
float degreesToRadians(float degrees);

// An explosion's scale at its texture frame: it grows to twice its size over
// its frames (FallView.QueueModels: max(2 frame / 26, 0.01)).
float explosionScale(int32_t frame);

} // namespace fall
} // namespace mdk
