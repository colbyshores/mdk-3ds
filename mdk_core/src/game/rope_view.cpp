#include "game/rope_view.h"

#include <math.h>
#include <string.h>

#include "formats/palette.h"
#include "game/level_data.h"
#include "scripts/object.h"
#include "scripts/script_runtime.h"

namespace mdk {

namespace {

// RopeView.ByteToUnit.
const float kByteToUnit = 1.0f / 255.0f;
// The quad path: an 8x8 texture (the PICA200's smallest), sampled at its centre.
const int32_t kColourTextureSize = 8;
// The quad's half width: 0.75 of a pixel of a 240-line view, at the point's depth
// (the view's vertical half extent at depth d is d / projection[5]).
const float kHalfWidthPerHalfExtent = 0.75f / 120.0f;

Vec3 sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vertex vertexAt(Vec3 p) {
    Vertex v;
    v.pos = p;
    v.uv = Vec2{0.5f, 0.5f};
    v.colour = kVertexWhite;
    return v;
}

// RopeView.Colour: a colour of the arena's palette (the level's for an unknown
// arena), opaque, as RGBA8 packed r | g << 8 | b << 16 | a << 24.
uint32_t ropeColour(LevelData& level, const std::string& arena, int32_t index) {
    const formats::Arena* found = level.arenaNamed(arena);
    const formats::Palette& palette =
        found != nullptr ? level.paletteOf(found) : level.dti().palette();
    const uint8_t* rgba = palette.rgba();
    const int32_t at = (index < 0 ? 0 : (index > 255 ? 255 : index)) * 4;
    return (uint32_t)rgba[at] | ((uint32_t)rgba[at + 1] << 8) | ((uint32_t)rgba[at + 2] << 16) |
           (0xFFu << 24);
}

} // namespace

bool RopeView::ensureMesh(MdkRenderer renderer) {
    if (mesh_ >= 0) {
        return true;
    }
    // Asked for once (the C# makes its mesh in the constructor): a refusal is not
    // asked again every frame, and the ropes stay undrawn for the level.
    if (failed_) {
        return false;
    }
    mesh_ = mdk_render_create_dynamic_mesh(renderer, kMaxVertices);
    failed_ = mesh_ < 0;
    return !failed_;
}

MdkTextureId RopeView::colourTexture(MdkRenderer renderer, uint32_t rgba) {
    for (int32_t i = 0; i < colourCount_; i++) {
        if (colourKeys_[i] == rgba) {
            return colourIds_[i];
        }
    }
    if (colourCount_ == kMaxColours) {
        return -1;
    }
    uint8_t texels[kColourTextureSize * kColourTextureSize * 4];
    for (int32_t i = 0; i < kColourTextureSize * kColourTextureSize; i++) {
        texels[i * 4 + 0] = (uint8_t)(rgba & 0xFFu);
        texels[i * 4 + 1] = (uint8_t)((rgba >> 8) & 0xFFu);
        texels[i * 4 + 2] = (uint8_t)((rgba >> 16) & 0xFFu);
        texels[i * 4 + 3] = (uint8_t)(rgba >> 24);
    }
    const MdkTextureId id =
        mdk_render_create_rgba_texture(renderer, kColourTextureSize, kColourTextureSize, texels);
    // A refused texture is remembered as refused (-1), so it is not asked for again
    // every frame for every rope of that colour.
    colourKeys_[colourCount_] = rgba;
    colourIds_[colourCount_] = id;
    colourCount_++;
    return id;
}

void RopeView::draw(MdkRenderer renderer, const scripts::ScriptRuntime& runtime, LevelData& level,
                    const Camera& camera, float tickAlpha, const std::string& backArena) {
    if (!renderer) {
        return;
    }
    const bool lines = mdk_render_capabilities(renderer).line_primitive;
    vertices_.clear();
    queued_.clear();
    const float halfExtent = fabsf(camera.projection[5]) > 0.0f ? 1.0f / fabsf(camera.projection[5])
                                                                : 1.0f;
    const std::deque<scripts::MdkObject>& objects = runtime.objects();
    for (size_t i = 0; i < objects.size(); i++) {
        const scripts::MdkObject& obj = objects[i];
        if (obj.rope_mask == 0 || !obj.visible || obj.dead) {
            continue;
        }
        // The object pass's alpha (main.cpp): the ride Kurt drives and the XE he
        // flies are drawn at their tick's place, so their ropes are too.
        const scripts::Rides& rides = runtime.rides();
        const bool carried = rides.bomber() && &obj == rides.ridden();
        const float alpha = &obj == rides.driven() || carried ? 1.0f : tickAlpha;
        Vec3 ends[scripts::MdkObject::kMaxRopeEnds];
        const int32_t count = obj.rope_lines(alpha, ends);
        const int32_t corners = lines ? count : count / 2 * 6;
        if (count == 0 || (int32_t)vertices_.size() + corners > kMaxVertices) {
            continue;
        }
        Queued queued;
        queued.first = (int32_t)vertices_.size();
        queued.count = corners;
        queued.rgba = ropeColour(level, obj.arena, obj.rope_color);
        queued.back = !backArena.empty() && obj.arena == backArena;
        if (lines) {
            for (int32_t k = 0; k < count; k++) {
                vertices_.push_back(vertexAt(ends[k]));
            }
        } else {
            for (int32_t k = 0; k + 1 < count; k += 2) {
                const Vec3 a = ends[k];
                const Vec3 b = ends[k + 1];
                const Vec3 along = sub(b, a);
                const Vec3 mid = Vec3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
                Vec3 side = cross(along, sub(mid, camera.position));
                const float length = sqrtf(dot(side, side));
                if (!(length > 0.0f)) {
                    // Seen end on (or a zero-length line): nothing to draw, but
                    // keep the six corners so the draw's count stays whole.
                    side = Vec3{0.0f, 0.0f, 0.0f};
                } else {
                    side = Vec3{side.x / length, side.y / length, side.z / length};
                }
                const float depthA = fmaxf(dot(sub(a, camera.position), camera.forward),
                                           camera.near_plane);
                const float depthB = fmaxf(dot(sub(b, camera.position), camera.forward),
                                           camera.near_plane);
                const float wa = depthA * halfExtent * kHalfWidthPerHalfExtent;
                const float wb = depthB * halfExtent * kHalfWidthPerHalfExtent;
                const Vec3 a0{a.x - side.x * wa, a.y - side.y * wa, a.z - side.z * wa};
                const Vec3 a1{a.x + side.x * wa, a.y + side.y * wa, a.z + side.z * wa};
                const Vec3 b0{b.x - side.x * wb, b.y - side.y * wb, b.z - side.z * wb};
                const Vec3 b1{b.x + side.x * wb, b.y + side.y * wb, b.z + side.z * wb};
                vertices_.push_back(vertexAt(a0));
                vertices_.push_back(vertexAt(a1));
                vertices_.push_back(vertexAt(b1));
                vertices_.push_back(vertexAt(a0));
                vertices_.push_back(vertexAt(b1));
                vertices_.push_back(vertexAt(b0));
            }
        }
        queued_.push_back(queued);
    }
    if (vertices_.empty() || !ensureMesh(renderer)) {
        return;
    }
    mdk_render_update_mesh(renderer, mesh_, vertices_.data(), (int32_t)vertices_.size());
    const Mat4 identity = Mat4::identity();
    for (size_t i = 0; i < queued_.size(); i++) {
        const Queued& q = queued_[i];
        MeshDraw draw;
        draw.mesh = mesh_;
        draw.first_vertex = q.first;
        draw.vertex_count = q.count;
        memcpy(draw.world, identity.m, sizeof(draw.world));
        // The other drawn arena's ropes with its objects, under the camera arena's
        // cover (the ride-end ordering: render/draw_queue.h).
        draw.back = q.back;
        if (lines) {
            // Material.Flat(Colour(arena, RopeColor), Pass.Solid), drawn as lines.
            draw.primitive = Primitive::Lines;
            draw.material.texture = -1;
            draw.material.palette = -1;
            draw.material.pass = Pass::Solid;
            draw.material.colour = Colour{(float)(q.rgba & 0xFFu) * kByteToUnit,
                                          (float)((q.rgba >> 8) & 0xFFu) * kByteToUnit,
                                          (float)((q.rgba >> 16) & 0xFFu) * kByteToUnit, 1.0f};
        } else {
            // The quads face the camera whichever way the line runs, so they are
            // drawn from both sides rather than trusting a winding.
            const MdkTextureId texture = colourTexture(renderer, q.rgba);
            if (texture < 0) {
                continue;
            }
            draw.primitive = Primitive::Triangles;
            draw.material.texture = texture;
            draw.material.palette = -1;
            draw.material.pass = Pass::DoubleSided;
        }
        mdk_render_draw_mesh(renderer, &draw);
    }
}

} // namespace mdk
