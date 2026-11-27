// ============================================================================
// layer_tests.cpp — see layer_tests.h.
//
// THE C# FILE IS THE SPECIFICATION: tests/Mdk.Game.Tests/LayerTests.cs and
// tests/Mdk.Game.Tests/DepthPullTests.cs. Same synthetic arenas, same
// expectations, same two level-6 arenas, same camera.
//
// WHY THE DEPTH LAYERS MATTER. The original draws without a depth buffer,
// back to front, so a poster in its wall's plane simply covers it. With a
// depth buffer the two fight for the same depth and flicker, so the oracle
// gives the poster a LAYER and pulls it towards the eye by a share of the
// distance. The corners stay put: moving it off the wall would open cracks
// to its neighbours.
// ============================================================================

#include "layer_tests.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "formats/arena.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "render/arena_scene.h"
#include "render/camera_builder.h"
#include "render/depth_pull.h"
#include "render/render_types.h"
#include "util/math_lut.h"

using namespace mdk;
using namespace mdk::formats;

namespace {

int failures = 0;

#define LAYER_CHECK(cond)                                                        \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                      \
            failures++;                                                           \
        }                                                                         \
    } while (0)

// A flat arena: triangles in z = 0, wound as MDK's floors. The C#'s
// LayerTests.Flat.
struct FlatArena {
    std::vector<Vec3f> vertices;
    std::vector<int32_t> indices;
};

FlatArena flat(const std::vector<std::vector<Vec2f> >& triangles) {
    FlatArena arena;
    for (size_t t = 0; t < triangles.size(); t++) {
        for (size_t k = 0; k < 3; k++) {
            const Vec2f& corner = triangles[t][k];
            arena.vertices.push_back(Vec3f{corner.x, corner.y, 0.0f});
            arena.indices.push_back((int32_t)arena.vertices.size() - 1);
        }
    }
    return arena;
}

void checkLayers(const char* name, const FlatArena& arena, const std::vector<int32_t>& expected) {
    std::vector<int32_t> layers;
    if (!computeArenaDepthLayers(arena.vertices, arena.indices, &layers)) {
        printf("  FAIL %s: the depth layers did not compute\n", name);
        failures++;
        return;
    }
    if (layers != expected) {
        printf("  FAIL %s: layers", name);
        for (size_t i = 0; i < layers.size(); i++) {
            printf(" %d", layers[i]);
        }
        printf(", wanted");
        for (size_t i = 0; i < expected.size(); i++) {
            printf(" %d", expected[i]);
        }
        printf("\n");
        failures++;
    }
}

// The C#'s wall: the triangle the posters lie on.
std::vector<Vec2f> wall() {
    std::vector<Vec2f> triangle;
    triangle.push_back(Vec2f{0.0f, 0.0f});
    triangle.push_back(Vec2f{0.0f, 10.0f});
    triangle.push_back(Vec2f{10.0f, 0.0f});
    return triangle;
}

void testPosterOnAWallIsOneLayerUp() {
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    std::vector<Vec2f> poster;
    poster.push_back(Vec2f{1.0f, 1.0f});
    poster.push_back(Vec2f{1.0f, 2.0f});
    poster.push_back(Vec2f{2.0f, 1.0f});
    triangles.push_back(poster);

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(1);
    checkLayers("APosterOnAWallIsOneLayerUp", flat(triangles), expected);
}

void testPosterHalfOnAWallIsOneLayerUp() {
    // Its centre (9, 1.3) is off the wall, a corner on it.
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    std::vector<Vec2f> poster;
    poster.push_back(Vec2f{7.0f, 1.0f});
    poster.push_back(Vec2f{7.0f, 2.0f});
    poster.push_back(Vec2f{13.0f, 1.0f});
    triangles.push_back(poster);

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(1);
    checkLayers("APosterHalfOnAWallIsOneLayerUp", flat(triangles), expected);
}

void testNeighboursSharingAnEdgeStayFlat() {
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    std::vector<Vec2f> neighbour;
    neighbour.push_back(Vec2f{0.0f, 10.0f});
    neighbour.push_back(Vec2f{10.0f, 10.0f});
    neighbour.push_back(Vec2f{10.0f, 0.0f});
    triangles.push_back(neighbour);

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(0);
    checkLayers("NeighboursSharingAnEdgeStayFlat", flat(triangles), expected);
}

void testPosterOnAPosterIsTwoLayersUp() {
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    std::vector<Vec2f> poster;
    poster.push_back(Vec2f{1.0f, 1.0f});
    poster.push_back(Vec2f{1.0f, 5.0f});
    poster.push_back(Vec2f{5.0f, 1.0f});
    triangles.push_back(poster);
    std::vector<Vec2f> sticker;
    sticker.push_back(Vec2f{1.5f, 1.5f});
    sticker.push_back(Vec2f{1.5f, 2.0f});
    sticker.push_back(Vec2f{2.0f, 1.5f});
    triangles.push_back(sticker);

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(1);
    expected.push_back(2);
    checkLayers("APosterOnAPosterIsTwoLayersUp", flat(triangles), expected);
}

void testOfTwoEqualTrianglesTheLaterIsOnTop() {
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    triangles.push_back(wall());

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(1);
    checkLayers("OfTwoEqualTrianglesTheLaterIsOnTop", flat(triangles), expected);
}

void testOtherPlanesStayFlat() {
    std::vector<std::vector<Vec2f> > triangles;
    triangles.push_back(wall());
    std::vector<Vec2f> poster;
    poster.push_back(Vec2f{1.0f, 1.0f});
    poster.push_back(Vec2f{1.0f, 2.0f});
    poster.push_back(Vec2f{2.0f, 1.0f});
    triangles.push_back(poster);

    FlatArena arena = flat(triangles);
    arena.vertices[3].z = 1.0f;
    arena.vertices[4].z = 1.0f;
    arena.vertices[5].z = 1.0f;

    std::vector<int32_t> expected;
    expected.push_back(0);
    expected.push_back(0);
    checkLayers("OtherPlanesStayFlat", arena, expected);
}

// The real arenas of level 6: the C#'s two cases.
void testLevel6Arenas(const MdkData& data) {
    std::vector<uint8_t> bytes;
    if (!data.read("TRAVERSE/LEVEL6/LEVEL6O.MTO", &bytes)) {
        printf("  FAIL: level 6's arenas did not read\n");
        failures++;
        return;
    }
    Mto mto = Mto::parse(bytes.data(), (int32_t)bytes.size());

    // OLYM_5: a hub's rim (159) lies half on the glass floor around it (115).
    Arena* hubs = mto.getArena("OLYM_5");
    if (!hubs) {
        printf("  FAIL: OLYM_5 did not parse\n");
        failures++;
    } else {
        std::vector<int32_t> layers;
        if (!computeArenaDepthLayers(hubs->vertices(), hubs->triangleIndices(), &layers)) {
            printf("  FAIL: OLYM_5's depth layers did not compute\n");
            failures++;
        } else {
            LAYER_CHECK(layers.size() > 159 && layers.size() > 115);
            LAYER_CHECK(layers[159] > layers[115]);
        }
    }

    // OLYM_7: a dark floor triangle (2) under a mirror tile (56).
    Arena* mirrors = mto.getArena("OLYM_7");
    if (!mirrors) {
        printf("  FAIL: OLYM_7 did not parse\n");
        failures++;
    } else {
        std::vector<int32_t> layers;
        if (!computeArenaDepthLayers(mirrors->vertices(), mirrors->triangleIndices(), &layers)) {
            printf("  FAIL: OLYM_7's depth layers did not compute\n");
            failures++;
        } else {
            LAYER_CHECK(layers.size() > 56 && layers.size() > 2);
            LAYER_CHECK(layers[2] > layers[56]);
        }
    }
}

// ---------------------------------------------------------------------------
// The depth pull
// ---------------------------------------------------------------------------

// The C# DepthPullTests' camera: the eye, the target one unit below it, the
// world up, a 1.2 radian field of view, and a 0.5 to 20000 depth range.
const Vec3 kEye = {-2222.0f, 3200.0f, -2885.0f};

Camera testCamera() {
    Camera camera;
    const float fovDegrees = 1.2f * 180.0f / 3.14159265358979323846f;
    buildLookAtCamera(&camera, kEye, Vec3{kEye.x, kEye.y - 1.0f, kEye.z}, Vec3{0.0f, 0.0f, 1.0f},
                      fovDegrees, 4.0f / 3.0f, 0.5f, 20000.0f);
    return camera;
}

// A point `distance` in front of the eye.
Vec3 ahead(float distance) {
    return Vec3{kEye.x + 0.3f * distance, kEye.y - distance, kEye.z - 0.1f * distance};
}

// The row-vector matrix product: out = a * b.
void multiply(const float a[16], const float b[16], float out[16]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

struct Ndc {
    float x;
    float y;
    float z;
};

// The point in normalised device coordinates: p * transform, divided by w.
Ndc ndc(const Vec3& point, const float transform[16]) {
    float clip[4];
    for (int col = 0; col < 4; col++) {
        clip[col] = point.x * transform[col] + point.y * transform[4 + col] +
                    point.z * transform[8 + col] + transform[12 + col];
    }
    const float w = clip[3];
    Ndc result;
    result.x = clip[0] / w;
    result.y = clip[1] / w;
    result.z = clip[2] / w;
    return result;
}

// The camera's transform with the pull in front of it: the C#'s
// DepthPull.Of(eye, primitive, layer) * Camera.
void pulledCamera(const Camera& camera, Primitive primitive, int32_t layer, float out[16]) {
    float pull[16];
    buildDepthPullMatrix(camera.position, primitive, layer, pull);
    float viewProjection[16];
    multiply(camera.view, camera.projection, viewProjection);
    multiply(pull, viewProjection, out);
}

// THE STEREO PAIR'S CULL (camera_builder.h buildStereoFrustum, 2026-11-27): the
// converged eyes see past the centre frustum's side planes by a strip that
// widens with depth, so the 3DS's object and effects culls widen the side
// planes by the eyes' slide. A far point just past the right edge is culled by
// the centre frustum and kept by the stereo one; with no separation or no
// convergence the two are the same volume; the depth planes do not move.
void testStereoFrustum() {
    Camera camera;
    // The 3DS's follow camera's projection, looking down +y from the origin, so
    // the view's right axis is world +x and depth is world y.
    LAYER_CHECK(buildLookAtCamera(&camera, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                  Vec3{0.0f, 0.0f, 1.0f}, 71.5f, 400.0f / 240.0f, 0.5f,
                                  20000.0f));
    const float half = 0.5f;
    const float convergence = 10.0f;
    const float p00 = camera.projection[0];
    const float shift = half * p00 / convergence;
    const float depth = 1000.0f;
    const Vec3 past{(1.0f + shift * 0.5f) * depth / p00, depth, 0.0f};
    Frustum centre;
    Frustum wide;
    LAYER_CHECK(buildFrustum(camera, &centre));
    LAYER_CHECK(buildStereoFrustum(camera, half, convergence, &wide));
    LAYER_CHECK(frustumSphereOutside(centre, past, 0.01f));
    LAYER_CHECK(!frustumSphereOutside(wide, past, 0.01f));
    // Past the widened edge it is culled again.
    const Vec3 beyond{(1.0f + shift * 2.0f) * depth / p00, depth, 0.0f};
    LAYER_CHECK(frustumSphereOutside(wide, beyond, 0.01f));
    // The far plane is the camera's.
    LAYER_CHECK(frustumSphereOutside(wide, Vec3{0.0f, 30000.0f, 0.0f}, 1.0f));
    // No pair, or the parallel pair: buildFrustum, plane for plane.
    Frustum same;
    LAYER_CHECK(buildStereoFrustum(camera, 0.0f, convergence, &same));
    LAYER_CHECK(memcmp(&same, &centre, sizeof(Frustum)) == 0);
    LAYER_CHECK(buildStereoFrustum(camera, half, 0.0f, &same));
    LAYER_CHECK(memcmp(&same, &centre, sizeof(Frustum)) == 0);
}

// THE SCOPE'S SEPARATION (camera_builder.h stereoSeparationForView): divided by
// the view's magnification over the follow camera, never multiplied.
void testStereoSeparationForView() {
    Camera follow;
    follow.projection[0] = 0.8f;
    Camera scope = follow;
    scope.projection[0] = 3.2f;   // 4x
    LAYER_CHECK(fabsf(stereoSeparationForView(1.0f, scope, follow) - 0.25f) < 1e-6f);
    // A wider view than the reference keeps the separation.
    Camera wider = follow;
    wider.projection[0] = 0.4f;
    LAYER_CHECK(stereoSeparationForView(1.0f, wider, follow) == 1.0f);
    LAYER_CHECK(stereoSeparationForView(1.0f, follow, follow) == 1.0f);
    // The on-screen disparity is what matters: separation x P00 is the follow
    // camera's at any zoom (both parallax terms scale with it).
    LAYER_CHECK(fabsf(stereoSeparationForView(1.0f, scope, follow) * scope.projection[0] -
                      1.0f * follow.projection[0]) < 1e-6f);
    // A degenerate reference keeps the separation rather than dividing by 0.
    Camera none;
    LAYER_CHECK(stereoSeparationForView(1.0f, scope, none) == 1.0f);
}

// The object cull's frustum: a sphere ahead of the camera is kept, one
// behind or far to the side is outside, and a sphere crossing a plane is
// kept (the test is conservative). Ahead uses the C# camera's own axes.
void testFrustumSphere() {
    const Camera camera = testCamera();
    Frustum frustum;
    LAYER_CHECK(buildFrustum(camera, &frustum));
    LAYER_CHECK(!frustumSphereOutside(frustum, ahead(100.0f), 2.0f));
    LAYER_CHECK(frustumSphereOutside(
        frustum, Vec3{kEye.x, kEye.y + 500.0f, kEye.z}, 2.0f));
    LAYER_CHECK(frustumSphereOutside(
        frustum, Vec3{kEye.x + 5000.0f, kEye.y, kEye.z}, 2.0f));
    LAYER_CHECK(!frustumSphereOutside(frustum, ahead(100.0f), 500.0f));
    // The [0,1] depth planes: behind the near plane and past the far
    // plane (0.5..20000) are both outside -- the near extraction is the
    // one assertion a golden cannot make for this code.
    LAYER_CHECK(frustumSphereOutside(
        frustum, Vec3{kEye.x - 0.3f * 10.0f, kEye.y + 10.0f, kEye.z + 0.1f * 10.0f}, 1.0f));
    LAYER_CHECK(frustumSphereOutside(frustum, ahead(30000.0f), 10.0f));
    // A point the NDC read places inside the volume is never outside.
    const Vec3 inside = ahead(50.0f);
    float vp[16];
    multiply(camera.view, camera.projection, vp);
    const Ndc at = ndc(inside, vp);
    LAYER_CHECK(at.x > -1.0f && at.x < 1.0f && at.z > 0.0f && at.z < 1.0f);
    LAYER_CHECK(!frustumSphereOutside(frustum, inside, 0.5f));
}

// The projected pixel size (camera_builder.h): the formula's own numbers on
// the test camera (fov 1.2 rad, 4:3), its zoom term, and the near-plane
// guard -- a sphere reaching the camera is never small.
void testProjectedPixelSize() {
    const Camera camera = testCamera();
    // Depth along the view: ahead(d) is d in front, so a radius-1 sphere's
    // nearest point is d - 1.
    LAYER_CHECK(fabsf(sphereNearDepth(camera, ahead(100.0f), 1.0f) - 99.0f) < 0.01f);
    LAYER_CHECK(sphereNearDepth(camera, Vec3{kEye.x, kEye.y + 50.0f, kEye.z}, 1.0f) < 0.0f);
    // P11 = 1 / tan(0.6), P00 = P11 / (4/3); on 400 x 240 the x axis wins:
    // 400/2 * P00 = 219.26 pixels per unit at depth 1.
    const float p11 = 1.0f / tanf(0.6f);
    const float perUnit = 200.0f * p11 * 0.75f;
    const float px = projectedPixelSize(camera, 2.0f, 99.0f, 400, 240);
    LAYER_CHECK(fabsf(px - 2.0f * perUnit / 99.0f) < 0.01f);
    // A taller picture takes the y axis: 400 x 800 -> 400 * P11.
    LAYER_CHECK(fabsf(projectedPixelSize(camera, 1.0f, 1.0f, 400, 800) - 400.0f * p11) < 0.01f);
    // Twice the resolution, twice the pixels.
    LAYER_CHECK(fabsf(projectedPixelSize(camera, 2.0f, 99.0f, 800, 480) - 2.0f * px) < 0.01f);
    // The zoom: a camera with half the field of view's tangent doubles it.
    Camera zoomed;
    const float zoomFov = 2.0f * atanf(tanf(0.6f) * 0.5f) * 180.0f / 3.14159265358979323846f;
    buildLookAtCamera(&zoomed, kEye, Vec3{kEye.x, kEye.y - 1.0f, kEye.z}, Vec3{0.0f, 0.0f, 1.0f},
                      zoomFov, 4.0f / 3.0f, 0.5f, 20000.0f);
    LAYER_CHECK(fabsf(projectedPixelSize(zoomed, 2.0f, 99.0f, 400, 240) - 2.0f * px) < 0.02f);
    // At or in front of the near plane (0.5): never small.
    LAYER_CHECK(projectedPixelSize(camera, 0.001f, 0.5f, 400, 240) == FLT_MAX);
    LAYER_CHECK(projectedPixelSize(camera, 0.001f, -3.0f, 400, 240) == FLT_MAX);
}

void testPlainTrianglesStayPut() {
    float pull[16];
    buildDepthPullMatrix(kEye, Primitive::Triangles, 0, pull);
    const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 16; i++) {
        LAYER_CHECK(pull[i] == identity[i]);
    }
}

void testAnEdgeIsPulledTowardsTheEye() {
    const Camera camera = testCamera();
    float plain[16];
    multiply(camera.view, camera.projection, plain);
    float pulled[16];
    pulledCamera(camera, Primitive::Lines, 0, pulled);

    const float distances[3] = {10.0f, 100.0f, 1000.0f};
    for (int i = 0; i < 3; i++) {
        const float distance = distances[i];
        // Seen at a grazing angle: half a pixel off the edge, the surface is
        // that much nearer.
        const Vec3 edge = ahead(distance);
        const Vec3 surface = Vec3{edge.x, edge.y + 0.004f * distance, edge.z};

        const Ndc line = ndc(edge, pulled);
        const Ndc unmarked = ndc(edge, plain);
        const Ndc behind = ndc(surface, plain);

        LAYER_CHECK(fabsf(line.x - unmarked.x) < 1.0e-4f);
        LAYER_CHECK(fabsf(line.y - unmarked.y) < 1.0e-4f);
        LAYER_CHECK(line.z < behind.z);
    }
}

void testALayerIsNearerThanItsWallInTheDepthBuffer() {
    const Camera camera = testCamera();
    float plain[16];
    multiply(camera.view, camera.projection, plain);
    float pulled[16];
    pulledCamera(camera, Primitive::Triangles, 1, pulled);

    // A 32-bit float depth keeps 24 bits: the pull must span more than one
    // step.
    const float depthStep = 1.0f / (float)(1 << 24);
    const float distances[4] = {10.0f, 100.0f, 1000.0f, 4000.0f};
    for (int i = 0; i < 4; i++) {
        const Vec3 point = ahead(distances[i]);
        const Ndc wall = ndc(point, plain);
        const Ndc poster = ndc(point, pulled);

        LAYER_CHECK(fabsf(wall.x - poster.x) < 1.0e-4f);
        LAYER_CHECK(fabsf(wall.y - poster.y) < 1.0e-4f);
        LAYER_CHECK(wall.z - poster.z > 4.0f * depthStep);
    }
}

} // namespace

namespace mdk {
namespace test {

int runDepthLayerTests(const formats::MdkData& data) {
    // The camera's projection goes through the trig LUT.
    mathLutInit();

    testPosterOnAWallIsOneLayerUp();
    testPosterHalfOnAWallIsOneLayerUp();
    testNeighboursSharingAnEdgeStayFlat();
    testPosterOnAPosterIsTwoLayersUp();
    testOfTwoEqualTrianglesTheLaterIsOnTop();
    testOtherPlanesStayFlat();
    testLevel6Arenas(data);
    testPlainTrianglesStayPut();
    testFrustumSphere();
    testStereoFrustum();
    testStereoSeparationForView();
    testProjectedPixelSize();
    testAnEdgeIsPulledTowardsTheEye();
    testALayerIsNearerThanItsWallInTheDepthBuffer();

    return failures;
}

} // namespace test
} // namespace mdk
