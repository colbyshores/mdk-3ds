// ============================================================================
// kurt_cover_tests.cpp -- see kurt_cover_tests.h.
// ============================================================================

#include "kurt_cover_tests.h"

#include <math.h>
#include <stdio.h>

#include <vector>

#include "game/kurt_cover.h"

namespace mdk {
namespace test {

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        printf("  FAIL KurtCover: %s\n", what);
        g_failures++;
    }
}

Vertex at(float x, float y, float z) {
    Vertex v;
    v.pos = Vec3{x, y, z};
    v.uv = Vec2{0.0f, 0.0f};
    v.colour = kVertexWhite;
    return v;
}

// A scene of the given triangles (three corners each), one batch of `pass`.
void sceneOf(const std::vector<Vec3>& corners, Pass pass, ArenaScene* out) {
    out->clear();
    ArenaBatch batch;
    batch.first = 0;
    batch.count = (int32_t)corners.size();
    batch.surface.pass = pass;
    for (int32_t j = 0; j < 3; j++) {
        batch.boundsMin[j] = 1e30f;
        batch.boundsMax[j] = -1e30f;
    }
    for (size_t i = 0; i < corners.size(); i++) {
        out->vertices.push_back(at(corners[i].x, corners[i].y, corners[i].z));
        const float xyz[3] = {corners[i].x, corners[i].y, corners[i].z};
        for (int32_t j = 0; j < 3; j++) {
            batch.boundsMin[j] = xyz[j] < batch.boundsMin[j] ? xyz[j] : batch.boundsMin[j];
            batch.boundsMax[j] = xyz[j] > batch.boundsMax[j] ? xyz[j] : batch.boundsMax[j];
        }
    }
    out->batches.push_back(batch);
}

// The camera 8 behind Kurt and 4 above, looking along +y; Kurt's feet at the
// origin, his sprite a 3 x 4 rectangle in the plane y = 0.
KurtCover::View view() {
    KurtCover::View v;
    v.eye = Vec3{0.0f, -8.0f, 4.0f};
    v.forward = Vec3{0.0f, 1.0f, 0.0f};
    v.spread = 0.0f;
    v.corners[0] = Vec3{-1.5f, 0.0f, 4.0f};
    v.corners[1] = Vec3{1.5f, 0.0f, 4.0f};
    v.corners[2] = Vec3{1.5f, 0.0f, 0.0f};
    v.corners[3] = Vec3{-1.5f, 0.0f, 0.0f};
    v.order = Vec3{0.0f, 0.0f, KurtCover::kOrderHeight};
    return v;
}

// How many triangles one scene marks.
int32_t marks(const std::vector<Vec3>& corners, Pass pass, bool* glass, KurtCover::View v,
              std::vector<Vertex>* outVertices) {
    ArenaScene scene;
    sceneOf(corners, pass, &scene);
    KurtCover cover;
    KurtCover::Source source;
    source.arena = nullptr;
    source.scene = &scene;
    std::vector<Vertex> out(256 * 3);
    KurtCover::Run runs[8];
    const int32_t runCount = cover.collect(&source, 1, v, out.data(), 256, runs, 8);
    int32_t triangles = 0;
    for (int32_t i = 0; i < runCount; i++) {
        triangles += runs[i].count / 3;
        if (glass) {
            *glass = runs[i].glass;
        }
    }
    if (outVertices) {
        out.resize((size_t)triangles * 3);
        *outVertices = out;
    }
    return triangles;
}

} // namespace

int runKurtCoverTests() {
    g_failures = 0;
    const KurtCover::View v = view();

    // The floor under him, seen from above (a front face: its winding normal
    // points down, away from the eye).
    const std::vector<Vec3> floor = {Vec3{-5, -6, 0}, Vec3{-5, 5, 0}, Vec3{5, -6, 0}};
    std::vector<Vertex> cut;
    check(marks(floor, Pass::Solid, nullptr, v, &cut) > 0, "the floor he stands on is marked");
    // Cut to the pyramid: every corner of the marks lies inside it (the eye-ward
    // side of each side plane, with the margin), and on the floor's plane.
    bool inside = true;
    for (size_t i = 0; i < cut.size(); i++) {
        const Vec3 p = cut[i].pos;
        const float t = (p.y - v.eye.y) / (0.0f - v.eye.y);   // the share of the way to the sprite
        const float halfX = 1.5f * 1.2f + 0.2f;
        if (fabsf(p.z) > 1e-4f || t <= 0.0f || fabsf(p.x) > halfX * t + 1e-3f) {
            inside = false;
        }
    }
    check(inside, "the floor's marks are cut to the pyramid and stay on its plane");

    // The wall he walks along, at x = 0.8, seen from the -x side he and the
    // camera are on (normal +x: away from the eye).
    const std::vector<Vec3> wall = {Vec3{0.8f, -6, 0}, Vec3{0.8f, 2, 0}, Vec3{0.8f, -6, 6}};
    check(marks(wall, Pass::Solid, nullptr, v, nullptr) > 0, "the wall he walks along is marked");

    // A pillar's face between the camera and him, at y = -3, facing the camera
    // (normal +y). The camera is on its -y side, his middle on its +y side.
    const std::vector<Vec3> pillar = {Vec3{-0.5f, -3, 0}, Vec3{0.5f, -3, 0}, Vec3{-0.5f, -3, 6}};
    check(marks(pillar, Pass::Solid, nullptr, v, nullptr) == 0,
          "a pillar between the camera and him is not marked");

    // A ledge taller than half his height between the camera and him: its top
    // at z = 3.5 (above his middle), its face towards the camera.
    const std::vector<Vec3> ledgeTop = {Vec3{-3, -5, 3.5f}, Vec3{-3, -2, 3.5f}, Vec3{3, -5, 3.5f}};
    check(marks(ledgeTop, Pass::Solid, nullptr, v, nullptr) == 0,
          "a ledge top above his middle, between the camera and him, is not marked");
    // A step lower than half his height just in front of him (its top at z = 1,
    // where the rays to his legs cross it).
    const std::vector<Vec3> stepTop = {Vec3{-3, -2, 1}, Vec3{-3, -0.2f, 1}, Vec3{3, -2, 1}};
    check(marks(stepTop, Pass::Solid, nullptr, v, nullptr) > 0,
          "a step top below his middle is marked");

    // The wall's back face (the winding reversed): culled for Solid, drawn as
    // glass.
    const std::vector<Vec3> wallBack = {Vec3{0.8f, -6, 0}, Vec3{0.8f, -6, 6}, Vec3{0.8f, 2, 0}};
    check(marks(wallBack, Pass::Solid, nullptr, v, nullptr) == 0, "a Solid back face is not marked");
    bool glass = false;
    check(marks(wallBack, Pass::Blended, &glass, v, nullptr) > 0 && glass,
          "a glass wall of his space is marked, as glass");

    // Outside the pyramid (far to the side), and beyond his depth.
    const std::vector<Vec3> aside = {Vec3{20, -6, 0}, Vec3{20, 2, 0}, Vec3{20, -6, 6}};
    check(marks(aside, Pass::Solid, nullptr, v, nullptr) == 0, "a wall far aside is not marked");
    const std::vector<Vec3> behind = {Vec3{-5, 3, 0}, Vec3{-5, 6, 0}, Vec3{5, 3, 0}};
    check(marks(behind, Pass::Solid, nullptr, v, nullptr) == 0, "a floor beyond his depth is not marked");

    // A many-triangle scene (a box tree of several leaves): the floor cut into
    // 40 strips, plus the pillar; the strips under the pyramid are marked, the
    // pillar is not.
    std::vector<Vec3> many;
    for (int32_t i = 0; i < 40; i++) {
        const float y0 = -8.0f + 0.25f * (float)i;
        const float y1 = y0 + 0.25f;
        many.push_back(Vec3{-5, y0, 0});
        many.push_back(Vec3{-5, y1, 0});
        many.push_back(Vec3{5, y0, 0});
    }
    const int32_t floorStrips = marks(many, Pass::Solid, nullptr, v, nullptr);
    many.insert(many.end(), pillar.begin(), pillar.end());
    check(floorStrips > 0 && marks(many, Pass::Solid, nullptr, v, nullptr) == floorStrips,
          "a many-leaf tree marks the floor strips and not the pillar");

    return g_failures;
}

} // namespace test
} // namespace mdk
