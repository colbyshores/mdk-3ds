#pragma once

// ============================================================================
// stream_tube.h — the stream's tube (C# src/Mdk.Game/Stream/StreamTube.cs;
// original 0x434838, godot-mdk stream_tube.gd).
//
// A ring buffer of 32 rings of 16 points, one ring per segment of 10 units,
// each turned from the one before by three randomly walking angles. The
// segment between two rings has 32 wall planes facing in and a colour per
// point.
//
//   ring:   tail ... head - 1          (head - tail = 31 while generating)
//   slot:   ring & 31
//
//         P[n+1][j] ---- P[n+1][j+1]      two triangles (and planes) per quad,
//            |    \            |          32 per segment
//         P[n][j] ------ P[n][j+1]
//
// THE ARRAYS ARE FIXED. The C# allocates a new array per ring; here every slot
// is preallocated (32 x 16 points, 32 x 32 planes), with a per-slot flag for
// "this slot has been built at least once", which is exactly the C#'s "the
// array is still the empty one": a slot keeps its stale contents from the ring
// 32 before it until it is rebuilt, as the C#'s array does, and AddTriangles
// relies on that (the newest ring's colours are not read).
//
// The random generator is the CALLER's and is shared with the Flight (the
// order of draws is the game). The doubles are the C#'s: the angles walk and
// the radius, jitter and shade are computed in double and narrowed where the
// C# narrows (mechanics-gated DOUBLE, AGENTS.md R4: this is per segment, not a
// hot loop, and a ULP here is carried into every later ring).
// ============================================================================

#include <stdint.h>

#include <vector>

#include "game/kurt.h"
#include "stream/tube_basis.h"
#include "stream/watcom_random.h"

namespace mdk {
namespace stream {

// A corner of the tube's wall: its place, its colour in the ramp (0-63) and its
// distance level (0-5, see StreamTube::rgba). C# TubeVertex.
struct TubeVertex {
    Vec3 position;
    int32_t colour = 0;
    int32_t level = 0;
};

struct Rgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct Rgba8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

class StreamTube {
public:
    // The normal tube, or the one after LEVEL8 that straightens and ends at a planet.
    enum Kind { Normal, Gunter };

    // What advance() asks the stream to create.
    enum SpawnKind { Light, Planet };

    // A light or the planet: where along the tube (segments), across it, its
    // size and speed (segments/s). C# StreamTube.Spawn.
    struct Spawn {
        SpawnKind kind = Light;
        float t = 0.0f;
        float x = 0.0f;
        float z = 0.0f;
        float size = 0.0f;
        float speed = 0.0f;
    };

    static constexpr int kRings = 32;
    static constexpr int kSlotMask = kRings - 1;
    static constexpr int kPoints = 16;
    static constexpr int kPlanesPerSegment = kPoints * 2;
    static constexpr float kSegmentLength = 10.0f;
    // The Gunter tube stops after this segment.
    static constexpr int kLastSegment = 186;
    static constexpr int kRampSize = 64;

    // `index`: the level just played (0-4), C# StreamTube(index, difficulty, kind, random).
    // It generates the first 31 rings, drawing from `random` (which must outlive the tube).
    StreamTube(int32_t index, Kurt::Difficulty difficulty, Kind kind, WatcomRandom* random);

    StreamTube(const StreamTube&) = delete;
    StreamTube& operator=(const StreamTube&) = delete;

    // Kurt passed a segment: the oldest ring goes, a new one comes.
    void advance();

    // The lights and the planet made since the last call, appended to `out`
    // (C# TakeSpawns returns them and starts a new list).
    void takeSpawns(std::vector<Spawn>* out);

    // Whether `t` (in segments) is within the rings still alive.
    bool contains(float t) const;

    // The axis at `t` (0x436668).
    Vec3 centre(float t) const;

    // The frame at `t` (0x4366f4): forward along the axis, right from the camera's up.
    TubeBasis frame(float t, Vec3 up) const;

    // A point of segment `t` at (x, z) across it.
    Vec3 place(float t, float x, float z) const;

    // The walls (0x43637c): for the first plane of segment `t` that `position`
    // is within `margin` of, the fraction of the way to the axis that puts it
    // back; -1 when it touches none.
    float wallHit(Vec3 position, float t, float margin) const;

    // The segments drawn, far to near (0x436b00): head - 2 down to tail
    // (C# DrawnSegments). Iterate `for (n = firstDrawn(); n >= tail(); n--)`.
    int32_t firstDrawn() const { return head_ - 2; }

    // The triangles of segment `n` facing `eye`, appended to `into` (3 vertices each).
    void addTriangles(int32_t n, Vec3 eye, std::vector<TubeVertex>* into) const;

    // A vertex's colour (0x436b00): its ramp colour and its level's alpha (of 256).
    Rgba8 rgba(const TubeVertex& v) const;

    // The 64 colours of the walls.
    const Rgb8* ramp() const { return ramp_; }

    // The oldest ring still alive (~ Kurt's segment), and the next ring to make.
    int32_t tail() const { return tail_; }
    int32_t head() const { return head_; }

    float maxTurn() const { return maxTurn_; }
    float minRadius() const { return minRadius_; }
    float maxRadius() const { return maxRadius_; }

    // Ring n's centre, frame, point and colour, and a wall plane of its segment
    // (tests and the reference dump).
    Vec3 origin(int32_t n) const { return origins_[n & kSlotMask]; }
    const TubeBasis& basis(int32_t n) const { return bases_[n & kSlotMask]; }
    Vec3 point(int32_t n, int32_t j) const { return points_[n & kSlotMask][j]; }
    int32_t colour(int32_t n, int32_t j) const { return colourOf(n, j); }
    Vec3 planeNormal(int32_t n, int32_t i) const { return normals_[n & kSlotMask][i]; }
    float planeDistance(int32_t n, int32_t i) const { return distances_[n & kSlotMask][i]; }

    // The turn per segment about x, y (forward: roll) and z, in degrees.
    Vec3 angles() const { return Vec3{angles_[0], angles_[1], angles_[2]}; }
    // C# Radius is (float)_radius; the double is exposed for the goldens.
    float radius() const { return (float)radius_; }
    double radiusExact() const { return radius_; }
    double shade() const { return shade_; }
    int32_t shadeFrom() const { return shadeFrom_; }
    int32_t shadeTo() const { return shadeTo_; }

private:
    void buildRamp();
    int32_t levelOf(int32_t n) const;
    int32_t colourOf(int32_t n, int32_t j) const;
    void generate();
    void walkAngles();
    void turnRing(int32_t n);
    void makePoints(int32_t slot);
    void spawnLight(int32_t n);
    void spawnPlanet(int32_t n);
    void buildSegment(int32_t p);

    Kind kind_;
    WatcomRandom* random_;
    TubeBasis bases_[kRings];
    Vec3 origins_[kRings];
    Vec3 points_[kRings][kPoints];
    int32_t colours_[kRings][kPoints];
    Vec3 normals_[kRings][kPlanesPerSegment];
    float distances_[kRings][kPlanesPerSegment];
    // Whether the slot's colours/planes have ever been built (the C#'s array
    // is no longer the empty one).
    bool built_[kRings];
    Rgb8 ramp_[kRampSize];
    std::vector<Spawn> spawns_;
    float angles_[3] = {0.0f, 0.0f, 0.0f};
    double radius_;
    int32_t shadeFrom_ = 0;
    int32_t shadeTo_ = 0;
    double shade_ = 0.0;
    bool planetSpawned_ = false;
    int32_t tail_ = 0;
    int32_t head_ = 0;
    float maxTurn_;
    float minRadius_;
    float maxRadius_;
};

} // namespace stream
} // namespace mdk
