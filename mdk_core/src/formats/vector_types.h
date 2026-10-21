#pragma once

// ============================================================================
// vector_types.h — the formats' own vectors.
//
// A three-component vector and a two-component one, the C#'s Vector3 and
// Vector2.
//
// WHY THE FORMATS HAVE THEIR OWN. The renderer has Vec3 and Vec2 too, in
// render/render_types.h, and they mean the same thing. But the renderer is
// a layer above the formats: a format is read by the renderer, never the
// other way round, and a format that includes the renderer's header makes
// the dependency two-way in the reader's mind. So the formats have their
// own, and the game code converts between them at the seam.
//
// The fields are public and there are no member functions: these are plain
// old data, like the C#'s structs.
// ============================================================================

namespace mdk {
namespace formats {

// A three-component vector.
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// A two-component vector: a texture coordinate.
struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

} // namespace formats
} // namespace mdk
