// strike_view.cpp — the frontends' half of the full-screen strike. See strike_view.h.

#include "game/strike_view.h"

#include <math.h>

#include "game/cutscene_camera.h"
#include "render/camera_builder.h"
#include "scripts/script_runtime.h"
#include "scripts/strike_scene.h"

namespace mdk {

namespace {

// C# SniperView.RoundNear / Far (SniperView.cs:25-26).
const float kRoundNear = 0.5f;
const float kFar = 20000.0f;

} // namespace

bool advanceStrikeScene(scripts::ScriptRuntime* runtime, bool skip, float elapsed) {
    // C# DrawStrike (SniperView.cs:169-182): no scene, or not active -- null; Esc skips
    // it (null); else it advances, and holds the frame while it is still active.
    if (runtime == nullptr) {
        return false;
    }
    scripts::StrikeScene* scene = runtime->strikeScene();
    if (scene == nullptr || !scene->active()) {
        return false;
    }
    if (skip) {
        scene->skip();
        return false;
    }
    scene->update(elapsed);
    return scene->active();
}

bool buildStrikeCamera(const scripts::StrikeScene& scene, float aspect, Camera* camera) {
    // C# DrawStrike (SniperView.cs:186-189): forward = Normalize(target - eye).
    Vec3 eye;
    Vec3 target;
    scene.camera(&eye, &target);
    const Vec3 d{target.x - eye.x, target.y - eye.y, target.z - eye.z};
    const float length = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(length > 0.0f)) {
        return false;
    }
    const Vec3 forward{d.x / length, d.y / length, d.z / length};
    const Vec3 up = CutsceneCamera::upOf(forward);
    if (!buildLookAtCamera(camera, eye, Vec3{eye.x + forward.x, eye.y + forward.y,
                                             eye.z + forward.z},
                           up, scripts::StrikeScene::fieldOfView(), aspect, kRoundNear, kFar)) {
        return false;
    }
    camera->forward = forward;
    camera->up = up;
    return true;
}

} // namespace mdk
