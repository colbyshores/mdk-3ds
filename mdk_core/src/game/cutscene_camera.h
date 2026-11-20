#pragma once

// ============================================================================
// cutscene_camera.h — the camera of the scripts' cutscenes (C# Mdk.Game
// CutsceneCamera.cs, 0x477d94; godot-mdk script_runtime.gd get_cutscene_camera).
//
// The runtime moves its point and turns it each tick (ScriptRuntime::
// updateCutsceneCamera); this looks from there. Its yaw is 90 degrees minus the
// heading, its pitch positive down:
//
//        yaw 0 --> +Y (heading 90)
//   point o-----------> forward = (sin yaw * cos pitch, cos yaw * cos pitch, -sin pitch)
//
// Both frontends own one, as the C# Viewer does (Viewer.cs:142): while active()
// it is the view instead of the FollowCamera (Viewer.SceneView/Eye, :728/:739),
// update() takes each step's shot (Viewer.cs:528-531) and blend() places a frame
// between two ticks (Viewer.BlendFrame, :614-617).
//
// Presentation, not gameplay: the runtime's shot is the oracle's to the bit
// (golden/L*_cut_*.txt); the line of sight made from it here goes through the
// project's one trig path (AGENTS.md R6, fastSin/fastCos) as the follow camera's
// does, so it differs from the C#'s MathF in the last bits of a direction.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"

namespace mdk {

namespace scripts {
class ScriptRuntime;
}

class CutsceneCamera {
public:
    // C# CutsceneCamera.Near / Far; the field of view is FollowCamera's.
    static constexpr float kNear = 0.5f;
    static constexpr float kFar = 20000.0f;
    // C# CutTurn: a shot turning more than this a tick (degrees) is a cut.
    static constexpr float kCutTurn = 45.0f;

    // C# CutsceneCamera.Active: a cutscene holds the camera from the runtime's first
    // shot (the tick after special_event), so it never looks from the origin.
    static bool active(const scripts::ScriptRuntime& scripts);

    // C# Update: takes the runtime's shot of this tick (call it after each step).
    void update(const scripts::ScriptRuntime& scripts);

    // C# Blend: a frame between ticks, `alpha` of the way from the last tick's shot to
    // this one's; a cut is shown at once.
    void blend(float alpha);

    // C# View(aspect): the camera for the frame, at FollowCamera's field of view.
    // Returns false when the look-at is degenerate (the camera is left alone).
    bool build(Camera* camera, float aspect) const;

    // C# ForwardOf: the line of sight for a yaw (90 - heading) and a pitch (positive
    // down), in degrees.
    static Vec3 forwardOf(float yaw, float pitch);

    // C# FollowCamera.UpOf: Normalize(Cross(Cross(forward, UnitZ), forward)).
    static Vec3 upOf(const Vec3& forward);

    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 forward{0.0f, 1.0f, 0.0f};
    Vec3 up{0.0f, 0.0f, 1.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};

private:
    // C# Shot: the runtime's shots of the last two ticks, for frames between them.
    struct Shot {
        Vec3 point{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        float pitch = 0.0f;
    };

    void place(const Vec3& point, float yaw, float pitch);
    static bool isCut(const Shot& from, const Shot& to);

    bool havePrevious_ = false;  // C# _previous is not null
    Shot previous_;
    Shot shot_;
    int32_t tick_ = -1;
};

} // namespace mdk
