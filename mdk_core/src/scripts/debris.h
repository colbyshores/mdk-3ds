#pragma once

// ============================================================================
// debris.h — flying pieces: shattered triangle groups, sparks, break-up parts.
//
// A 1:1 port of src/Mdk.Game/Scripts/Debris.cs. Pieces fly, spin, fall,
// bounce off the arena, ride fan updrafts and vanish after their lifetime;
// some leave a smoke trail. The arena queries the C# gets as delegates
// (Debris.Ray / Trail / Updraft / GroupFacets) are plain function pointers
// with a context, set by ScriptRuntime at construction — no std::function
// (AGENTS.md §10). ScriptRuntime wires all four, as the C# does; an unset
// Trail or Updraft (a harness's Debris) spawns no puff and lifts nothing,
// which perturbs no random stream -- SpawnTrail consumes no RNG in the C# --
// but leaves the effects list short of the oracle's puffs.
//
// Ownership: a Piece's `names` borrows the material-name list of the arena
// it came from (level-owned, outlives the runtime), or -- for a break-up
// piece -- the runtime's cached name list of the model's materials
// (ScriptRuntime::materialNamesOf, stable for the runtime's life). Pieces are
// pooled (600 made up front, reused), exactly as the C#.
// ============================================================================

#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include "render/render_types.h"
#include "util/random.h"

namespace mdk {
namespace formats {
class Model;
} // namespace formats
namespace scripts {

struct MdkObject;

// A segment hit in an arena: point, surface normal, arena, triangle group
// (the C# ScriptRuntime.RayHit).
struct DebrisRayHit {
    Vec3 point;
    Vec3 normal;
    std::string arena;
    int32_t group = 0;
};

// One triangle of a shattered group: corners, UVs (texels), material value.
struct DebrisFacet {
    Vec3 a, b, c;
    Vec2 uvA, uvB, uvC;
    int32_t material = 0;
};

// A triangle group's facets plus the material names their values index.
struct DebrisGroup {
    const std::vector<std::string>* names = nullptr;  // borrowed
    std::vector<DebrisFacet> facets;
};

class Debris {
public:
    // The arena's nearest hit along a segment (arena, from, to); false = none.
    typedef bool (*RayFn)(void* ctx, const std::string& arena, const Vec3& from,
                          const Vec3& to, DebrisRayHit* out);
    // Leaves a smoke puff at a point in an arena.
    typedef void (*TrailFn)(void* ctx, const std::string& arena, const Vec3& point);
    // The fans' push: (arena, point, vertical speed u/s, seconds) -> the new
    // speed in u/s, or NaN when no fan applies.
    typedef float (*UpdraftFn)(void* ctx, const std::string& arena, const Vec3& point,
                               float vz, float dt);
    // The triangles of an arena's group; false when there are none.
    typedef bool (*GroupFacetsFn)(void* ctx, const std::string& arena, int32_t group,
                                  DebrisGroup* out);

    enum class Launch { Random, Still };

    struct Piece {
        std::string arena;
        const std::vector<std::string>* names = nullptr;  // borrowed; null = spark
        std::vector<int32_t> materials;
        std::vector<Vec3> corners;
        std::vector<Vec2> uvs;
        int32_t colourBase = 0;
        int32_t colourRange = 0;
        int32_t trailEvery = 0;
        int32_t trailTicks = 0;
        Vec3 center;
        Vec3 velocity;
        // Orientation/spin as quaternions (x, y, z, w), System.Numerics layout.
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float spin[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        int32_t ticks = 0;

        bool isSpark() const { return names == nullptr; }
        void reset();
    };

    static constexpr float kGravity = 0.284444f * 0.25f;
    static constexpr int32_t kMaxPieces = 600;
    static constexpr int32_t kLifeMin = 60;

    explicit Debris(Random& rng);

    void setCallbacks(void* ctx, RayFn ray, TrailFn trail, UpdraftFn updraft,
                      GroupFacetsFn groupFacets) {
        ctx_ = ctx;
        ray_ = ray;
        trail_ = trail;
        updraft_ = updraft;
        groupFacets_ = groupFacets;
    }

    // Shatters a triangle group: life in seconds, pieces no larger than
    // size/4, speed (units per tick) at point, falling off to 0 at the
    // group's farthest corner; pieces fly along direction, or away from the
    // point when it is zero.
    void shatter(const std::string& arena, int32_t group, float life, float size,
                 float speed, const Vec3& point, const Vec3& direction);
    // An object's break-up (Debris.cs:287, the exploding object's model + "D"): one
    // piece per part not hidden on the object, its triangles turned by the object's roll
    // and yaw, launched from two units above the object with its last move added. The
    // names are the model's material names, borrowed (see the ownership note above).
    void breakUp(const MdkObject& obj, const formats::Model& model,
                 const std::vector<std::string>* names);
    // Sparks at a point: size 0.5 or 1.0, palette colours colour..colour+range.
    void spark(const std::string& arena, const Vec3& point, int32_t count, float size,
               int32_t colour, int32_t range, float speed = 1.0f,
               Launch launch = Launch::Random);
    // Moves the pieces by ticks.
    void update(float ticks);
    bool hasPieces(const std::string& arena) const;
    int32_t pieceCount() const { return (int32_t)pieces_.size(); }
    const std::vector<Piece*>& pieces() const { return pieces_; }

private:
    struct Shot {
        std::string arena;
        const std::vector<std::string>* names = nullptr;
        float maxCross = 0.0f;
        float life = 0.0f;
        float speed = 0.0f;
        Vec3 point;
        Vec3 direction;
        float far = 0.0f;
    };

    Piece* take();
    void split(const Vec3 c[3], const Vec2 uv[3], int32_t material, const Shot& shot);
    void start(Piece* piece, const std::string& arena, float speed);
    void move(Piece* piece, float ticks);
    void randomSpin(float degrees, float out[4]);
    int32_t randomHalf();

    Random& rng_;
    void* ctx_ = nullptr;
    RayFn ray_ = nullptr;
    TrailFn trail_ = nullptr;
    UpdraftFn updraft_ = nullptr;
    GroupFacetsFn groupFacets_ = nullptr;
    std::deque<Piece> pool_;  // deque: take() may grow it without moving pieces
    std::vector<Piece*> free_;
    std::vector<Piece*> pieces_;
    Vec3 sparkCorners_[4];
};

} // namespace scripts
} // namespace mdk
