#pragma once

// ============================================================================
// post_params.h -- the enhanced look's post pass inputs (shaders/post.hlsl,
// shaders/occlusion.hlsl, driven by the C#'s Renderer.Enhanced.cs): the
// matrices that take a clip-space point back to the point relative to the
// camera (and the way back), and the pass's constants.
//
// ClipToDirection in the C# is Invert(rotation * projection), the rotation
// being CreateLookAt(Zero, forward, up) built from the camera's own
// forward and up (FollowCamera.cs) -- NOT the full view with its
// translation stripped: CreateLookAt's axes come out a last-bit
// different when the eye is not the origin, and the probe that pinned
// this (tests/formats) caught exactly that.
// ============================================================================

#include <cstdint>

#include "render/render_types.h"

namespace mdk {

// Ambient occlusion: the disc's radius (units) and how dark it gets
// (Renderer.Enhanced.cs OcclusionRadius / OcclusionStrength).
const float kOcclusionRadius = 12.0f;
const float kOcclusionStrength = 1.5f;

// The glow: colours above the threshold shine, scaled by the intensity,
// blurred over the scene's mips starting at this one (GlowThreshold /
// GlowIntensity / GlowMip). The scene keeps this many mips (SceneMips).
const float kGlowThreshold = 0.8f;
const float kGlowIntensity = 0.8f;
const float kGlowMip = 3.0f;
const uint32_t kSceneMips = 6;

// The post pass's clip matrices for a camera looking along `forward`
// with `up` (the Camera's own fields): viewToClip is the rotation-only
// view-projection, clipToView its inverse. Row-major row-vector, like
// every engine matrix. False only if the product is singular (a
// degenerate camera), leaving the outputs untouched.
bool postClipMatrices(const Vec3& forward, const Vec3& up, const float projection[16],
                      float clipToView[16], float viewToClip[16]);

} // namespace mdk
