// ============================================================================
// bullet_holes.cpp — see bullet_holes.h. BulletHoles.cs whole, and the C#
// ScriptRuntime's StampBulletHole / ArenaTexture (ScriptRuntime.cs:1295-1335).
// ============================================================================

#include "scripts/bullet_holes.h"

#include <float.h>
#include <math.h>

#include <cmath>
#include <string>
#include <vector>

#include "game/scope.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace bullet_holes {

const char* const kHole = "BHOLE";
const char* const kGorelessHole = "BHOLE2";

namespace {

// System.Numerics as the .NET 10 runtime computes it on the oracle's host (game/scope.h's
// numerics: a dot of rounded products summed left to right, a Normalize that divides by
// the length). The operators (+, -, * by a scalar) are one rounding per lane; nothing
// here is a library Lerp or Transform, so nothing here is fused.
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scaled(const Vec3& v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
Vec3 toVec3(const formats::Vec3f& v) { return Vec3{v.x, v.y, v.z}; }

// Vector3.Max(v, Zero) then Vector3.Min(v, One) per lane (Vector3.Clamp): .NET 9+'s
// IEEE 754:2019 maximum / minimum, which keep a NaN and order -0 below +0.
float clampUnit(float value) {
    if (value != value) {
        return value;
    }
    const float atLeastZero = value > 0.0f ? value : 0.0f;
    return atLeastZero < 1.0f ? atLeastZero : 1.0f;
}

// C# Weights: barycentric weights of a point of the triangle's plane, clamped into the
// triangle (as Godot's get_triangle_barycentric_coords, clamped and normalised).
Vec3 weightsOf(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const float kTiny = 1e-6f;
    const Vec3 v0 = sub(b, a);
    const Vec3 v1 = sub(c, a);
    const Vec3 v2 = sub(p, a);
    const float d00 = numerics::dot(v0, v0);
    const float d01 = numerics::dot(v0, v1);
    const float d11 = numerics::dot(v1, v1);
    const float d20 = numerics::dot(v2, v0);
    const float d21 = numerics::dot(v2, v1);
    const float denominator = d00 * d11 - d01 * d01;
    const float v = (d11 * d20 - d01 * d21) / denominator;
    const float w = (d00 * d21 - d01 * d20) / denominator;
    const Vec3 weights{clampUnit(1.0f - v - w), clampUnit(v), clampUnit(w)};
    // MathF.Max(sum, Tiny): a NaN sum stays NaN.
    const float sum = weights.x + weights.y + weights.z;
    const float divisor = (sum != sum || sum > kTiny) ? sum : kTiny;
    return Vec3{weights.x / divisor, weights.y / divisor, weights.z / divisor};
}

// C# Wrap: (value % size + size) % size.
/* @vfp-exempt R11 — the bullet hole's texel wrap, a 1:1 port of the C#'s
 * (value % size + size) % size by the stamped texture's width or height; it runs
 * twice per sniper hit on a special_130 object (stamp() below wraps the corner
 * once per axis and steps from there), never per frame. measured n/a (corpus
 * empty). Verified 2026-11-20. */
int32_t wrap(int32_t value, int32_t size) { return (value % size + size) % size; }

} // namespace

bool nearest(const formats::Model::Part& part, const std::vector<formats::Vec3f>& vertices,
             const Vec3& point, Textured textured, void* user, Face* out) {
    // C# BulletHoles.Nearest: the first face at the smallest squared distance.
    bool found = false;
    float bestDistance = FLT_MAX;
    const std::vector<int32_t>& materials = part.triangleMaterials;
    const std::vector<int32_t>& indices = part.triangleIndices;
    for (size_t t = 0; t < materials.size(); t++) {
        if (!textured(user, materials[t])) {
            continue;
        }

        const Vec3 a = toVec3(vertices[(size_t)indices[t * 3]]);
        const Vec3 b = toVec3(vertices[(size_t)indices[t * 3 + 1]]);
        const Vec3 c = toVec3(vertices[(size_t)indices[t * 3 + 2]]);
        Vec3 normal = numerics::cross(sub(b, a), sub(c, a));
        if (normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f) {
            continue;
        }

        normal = numerics::normalize(normal);
        const Vec3 onPlane = sub(point, scaled(normal, numerics::dot(normal, sub(point, a))));
        const Vec3 weights = weightsOf(onPlane, a, b, c);
        const Vec3 there = add(add(scaled(a, weights.x), scaled(b, weights.y)), scaled(c, weights.z));
        const Vec3 apart = sub(there, point);
        const float distance = numerics::dot(apart, apart);
        if (distance < bestDistance) {
            bestDistance = distance;
            out->triangle = (int32_t)t;
            out->weights = weights;
            found = true;
        }
    }
    return found;
}

bool stamp(formats::Texture* texture, float u, float v, const formats::Texture& hole) {
    const int32_t width = texture->width();
    const int32_t height = texture->height();
    const int32_t holeWidth = hole.width();
    const int32_t holeHeight = hole.height();
    if (width <= 0 || height <= 0 || holeWidth <= 0 || holeHeight <= 0 ||
        texture->indexCount() < width * height || hole.indexCount() < holeWidth * holeHeight) {
        return false;
    }
    uint8_t* pixels = texture->mutableIndices();
    const uint8_t* holePixels = hole.indices();

    // MathF.Round(x, MidpointRounding.AwayFromZero) is roundf; the halves are integer
    // halves (16 / 2) as the C#'s int division.
    const int32_t left = (int32_t)roundf(u) - holeWidth / 2;
    const int32_t top = (int32_t)roundf(v) - holeHeight / 2;
    // Wrap(left + x, Width) for x = 0, 1, ...: the corner wrapped once, then a step that
    // goes back to 0 at the edge -- the same texels as the C#'s per-pixel modulo.
    int32_t ty = wrap(top, height);
    const int32_t firstX = wrap(left, width);
    for (int32_t y = 0; y < holeHeight; y++) {
        int32_t tx = firstX;
        for (int32_t x = 0; x < holeWidth; x++) {
            const uint8_t index = holePixels[y * holeWidth + x];
            if (index != 0) {
                pixels[ty * width + tx] = index;
            }
            tx = tx + 1 == width ? 0 : tx + 1;
        }
        ty = ty + 1 == height ? 0 : ty + 1;
    }
    return true;
}

} // namespace bullet_holes

namespace {

// The materials of the hit model resolved once (the C#'s TextureOf, asked per face, is a
// pure lookup: the same material gives the same texture), for the nearest-face search.
struct MaterialTextures {
    std::vector<formats::Texture*> textures;
};

bool hasTexture(void* user, int32_t material) {
    const MaterialTextures* resolved = static_cast<const MaterialTextures*>(user);
    return material >= 0 && (size_t)material < resolved->textures.size() &&
           resolved->textures[(size_t)material] != nullptr;
}

} // namespace

formats::Texture* ScriptRuntime::arenaTexture(const std::string& arena, const std::string& name) {
    // C# ArenaTexture (ScriptRuntime.cs:1330): the arena's archives (its own, the level's,
    // then every arena's), or the level's alone when no arena has that name. The archives
    // are the level's, held writable by it (the MTO's arenas and the level archive; see
    // LevelData::arenaNamedMutable): archivesOf hands them out const for the drawing.
    const formats::Arena* found = level_ ? level_->arenaNamed(arena) : nullptr;
    if (!level_) {
        return nullptr;
    }
    std::vector<const formats::TextureArchive*> archives;
    if (found) {
        archives = level_->archivesOf(found);
    } else {
        archives.push_back(&level_->levelTextures());
    }
    for (size_t i = 0; i < archives.size(); i++) {
        const formats::Texture* texture = archives[i]->findTexture(name);
        if (texture) {
            return const_cast<formats::Texture*>(texture);
        }
    }
    return nullptr;
}

void ScriptRuntime::stampBulletHole(MdkObject* obj) {
    // C# StampBulletHole (ScriptRuntime.cs:1297, special_130 0x45d140).
    const formats::Model* model = obj->model;
    if (model == nullptr || obj->shot_part <= 0 || obj->shot_part > (int32_t)model->partList.size()) {
        return;
    }

    const size_t index = (size_t)(obj->shot_part - 1);
    const formats::Model::Part& part = model->partList[index];
    // obj.Pose(): the current pose with the hidden parts emptied (C#'s `1 << i` takes the
    // shift count modulo 32).
    const std::vector<std::vector<formats::Vec3f> >& pose = obj->pose_parts();
    if (index >= pose.size() || (obj->hidden_parts & (1 << (index & 31))) != 0 ||
        pose[index].empty()) {
        return;
    }
    const std::vector<formats::Vec3f>& vertices = pose[index];

    // The hit in the model's frame: Vector3.Transform(ShotPoint - Position,
    // CreateRotationZ(-DegreesToRadians(Yaw))) / Scale. The transform is the runtime's
    // fused row sum (X * x, then Y * y and Z * z multiply-added, then + W); the rotation
    // is SinCos of the angle, DegreesToRadians left-associated (yaw * pi / 180).
    /* @vfp-exempt R6 — special_130's world-to-model turn, once per sniper hit on a
     * special_130 object; glibc sinf/cosf match the oracle's MathF.SinCos bit for bit
     * on this machine (MdkObject::reference_point's probe), and the hole's texel is
     * golden-pinned (golden/L*_holes_*.txt) where the fastSin/fastCos LUT would move it.
     * measured n/a (corpus empty). Verified 2026-11-20. */
    const float radians = -(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float s = sinf(radians);
    const float c = cosf(radians);
    const Vec3 d{obj->shot_point.x - obj->position.x, obj->shot_point.y - obj->position.y,
                 obj->shot_point.z - obj->position.z};
    // Rows X = (c, s, 0), Y = (-s, c, 0), Z = (0, 0, 1), W = 0.
    const float x = std::fma(d.z, 0.0f, std::fma(d.y, -s, d.x * c)) + 0.0f;
    const float y = std::fma(d.z, 0.0f, std::fma(d.y, c, d.x * s)) + 0.0f;
    const float z = std::fma(d.z, 1.0f, std::fma(d.y, 0.0f, d.x * 0.0f)) + 0.0f;
    const Vec3 local{x / obj->scale, y / obj->scale, z / obj->scale};

    // TextureOf(value): the material's texture as the object's arena finds it, null for a
    // material index out of the list. Model.Materials holds the names cut to 10 characters.
    MaterialTextures resolved;
    resolved.textures.resize(model->materials.size(), nullptr);
    for (size_t m = 0; m < model->materials.size(); m++) {
        resolved.textures[m] = arenaTexture(obj->arena, model->materials[m].nameString());
    }
    bullet_holes::Face face;
    if (!bullet_holes::nearest(part, vertices, local, hasTexture, &resolved, &face)) {
        return;
    }

    formats::Texture* texture = resolved.textures[(size_t)part.triangleMaterials[(size_t)face.triangle]];
    const Vec3& w = face.weights;
    const size_t corner = (size_t)face.triangle * 3;
    const formats::Vec2f& uv0 = part.triangleUvs[corner];
    const formats::Vec2f& uv1 = part.triangleUvs[corner + 1];
    const formats::Vec2f& uv2 = part.triangleUvs[corner + 2];
    // Vector2 * float + Vector2 * float + Vector2 * float, one rounding per lane each.
    const float u = uv0.x * w.x + uv1.x * w.y + uv2.x * w.z;
    const float v = uv0.y * w.x + uv1.y * w.y + uv2.y * w.z;

    formats::Texture hole;
    if (!sprites_ || !sprites_->image(option != 0 ? bullet_holes::kHole : bullet_holes::kGorelessHole,
                                      &hole)) {
        return;
    }
    if (!bullet_holes::stamp(texture, u, v, hole)) {
        return;
    }
    // The pixels are not the file's any more: the arena stream must neither drop them nor
    // read them back (formats/texture.h). Then the C#'s TextureStamped, for the drawing.
    texture->pinPixels();
    if (textureStampedHook_) {
        textureStampedHook_(textureStampedUser_, texture);
    }
    for (size_t i = 0; i < stamped_.size(); i++) {
        if (stamped_[i] == texture) {
            return;
        }
    }
    stamped_.push_back(texture);
}

} // namespace scripts
} // namespace mdk
