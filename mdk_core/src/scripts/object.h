#pragma once

// ============================================================================
// object.h — a scripted object (alien, door, pickup...) or an arena controller.
//
// The C# oracle's Mdk.Game.Objects.MdkObject, ported as the data model first.
// State follows the original object structure (godot-mdk notes_part1.md): MDK
// coordinates (Z up), degrees, yaw 0 = +X, 90 = +Y.
//
// The object data model and its behavior are ported in slices. Some behavior
// lives here, and runtime behavior lives in ScriptRuntime; the remaining
// unported MdkObject behavior and its dependencies are listed below.
//
// Vectors are the render layer's Vec3 (mdk::Vec3, from render/render_types.h),
// matching the game layer (player.h) so an object and Kurt share one vector type
// at their seam. Model/ModelAnimation are the formats' parsed classes, held as
// non-owning pointers (the level owns them).
//
// OWNERSHIP CONTRACT for the inter-object links (linked/leader/cow_target/attached):
// the C# holds these as references into a managed heap and ADDS objects to the
// live list during play (spawn opcodes). The C++ level container MUST therefore
// give objects stable addresses that survive later insertions (a reserved-at-load
// vector, a std::deque, an arena, or index-based links) — a plain growing
// std::vector would dangle every link set before a reallocation. Decide this in
// the level/runtime slice before the first `linked =` is written. Removed
// objects keep their deque slots as well: the oracle retains a dead partner's
// reference, so that link remains safe to inspect.
// ============================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "render/render_types.h"
#include "formats/vector_types.h"
#include "util/matrix.h"

namespace mdk {
namespace formats {
class Model;
class ModelAnimation;
} // namespace formats

namespace scripts {

struct MdkObject {
    // ---- Engine flags (obj+0x148..0x14b as one integer) ----
    static constexpr int32_t FlagGravity = 0x2;
    static constexpr int32_t FlagCollides = 0x4;
    static constexpr int32_t FlagLoop = 0x8;
    static constexpr int32_t FlagNotSolid = 0x10;      // Kurt goes through, can't stand on it
    static constexpr int32_t FlagNotTarget = 0x20;     // the chain gun doesn't aim at it
    static constexpr int32_t FlagRolling = 0x40;
    static constexpr int32_t FlagNoBanking = 0x80;
    static constexpr int32_t FlagPlatform = 0x100;     // Kurt can stand on it, unless 0x10
    static constexpr int32_t FlagPathOnce = 0x400;
    static constexpr int32_t FlagNotSolid2 = 0x800;    // walks through, but stands if platform
    static constexpr int32_t FlagWeakParts = 0x2000;   // set_weak_parts
    static constexpr int32_t FlagNoTurning = 0x10000;
    static constexpr int32_t FlagLanded = 0x20000;     // a pickup that has landed
    static constexpr int32_t FlagCollected = 0x40000;  // a pickup Kurt has taken
    static constexpr int32_t FlagChangesArena = 0x80000;
    static constexpr int32_t FlagDoor = 0x100000;
    static constexpr int32_t FlagPickup = 0x200000;
    static constexpr int32_t FlagSwinging = 0x400000;  // swings on a rope (opcode 226)
    static constexpr int32_t FlagStandable = 0x800000; // Kurt stays on it
    static constexpr int32_t FlagPathPushes = 0x8000000;
    static constexpr int32_t FlagPathSpeedByKurt = 0x10000000; // opcode 164
    static constexpr int32_t FlagBounces = 0x20000000;
    static constexpr int32_t FlagCow = 0x40000000;
    // obj+0x14b bit 7: the animation's root motion doesn't move it. 0x80000000 as int32 (INT_MIN).
    static constexpr int32_t FlagNoRootMotion = -2147483647 - 1;

    // ---- Contact flags (obj+0x14c), set by the movement code each frame ----
    static constexpr int32_t ContactCollided = 0x1;
    static constexpr int32_t ContactFloor = 0x2;
    static constexpr int32_t ContactTouchedKurt = 0x4;
    static constexpr int32_t ContactStuck = 0x8;

    // ---- Animation sentinels ----
    static constexpr int32_t AnimationEnded = -256; // obj+0x118 = 0xFF00
    static constexpr int32_t NoHoldFrame = -1;
    static constexpr float AnimationFps = 30.0f;

    // ---- Identity ----
    std::string type_name = "";
    int32_t instance_id = -1;      // obj+0x146
    std::string arena = "";        // the arena the object belongs to (e.g. HMO_1)
    const formats::Model* model = nullptr;

    // ---- Placement (yaw obj+0x4c, pitch obj+0x13c, roll obj+0x54, degrees) ----
    Vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float scale = 1.0f;            // obj+0x58
    Vec3 spawn_position{};
    Vec3 previous_position{};      // obj+0x180 (end of the previous frame)

    // ---- Health and flags ----
    int32_t health = 100;
    int32_t max_health = 0;        // obj+0x2a2, the health bar's maximum
    bool indestructible = false;
    int32_t flags = 0;
    int32_t script_flags = 0;      // obj+0x244
    int32_t contact_flags = 0;     // obj+0x14c
    std::array<float, 4> variables{{0.0f, 0.0f, 0.0f, 0.0f}}; // obj+0x234
    MdkObject* linked = nullptr;   // obj+0x2b8 (variable kind "other")
    MdkObject* leader = nullptr;   // obj+0x138
    bool dead = false;
    int32_t hidden_parts = 0;      // obj+0x2c8
    int32_t locked_parts = 0;      // obj+0x2cc (opcode 129)
    int32_t texture_frame = -1;    // -1 = by the level's clock
    bool visible = true;
    MdkObject* cow_target = nullptr; // what the holy cow falls on (null: Kurt)

    // ---- Script state ----
    int32_t restart = 0;           // absolute CMI offset, 0 = no script
    float wait_time = 0.0f;
    int32_t wait_resume = 0;
    int32_t death_script = 0;
    int32_t command_target = 0;    // obj+0x10c
    std::vector<int32_t> gosub_returns;
    std::vector<int32_t> gosub_restarts;
    std::array<float, 5> level_timers{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f}}; // per gosub level, for if_timer
    int32_t hit_event = 0;         // obj+0x21e
    int32_t hit_type = 0;          // obj+0x21d
    float hit_direction = 0.0f;    // obj+0x224, degrees
    std::string weak_prefix = "";  // set_weak_parts
    int32_t weak_prefix_length = 0;
    std::vector<int32_t> part_health;
    std::vector<int32_t> part_max_health;
    int32_t priority = 0;          // obj+0x11a
    int32_t obey_level = 0;        // obj+0x11b
    int32_t target_mode = 0;       // obj+7 (opcode 251)
    float value_2c0 = 0.0f;        // obj+0x2c0 (opcode 210), not identified
    float value_104 = 0.0f;        // obj+0x104 (opcode 199), not identified

    // ---- Movement state ----
    int32_t move_command = 0;      // obj+0x11e
    int32_t move_parameter = 0;    // obj+0x11f
    Vec3 move_destination{};
    int32_t stuck_count = 0;       // obj+0x2a0
    float stuck_ticks = 0.0f;      // obj+0x2a1
    Vec3 stuck_moved{};            // obj+0x2a4
    float banking_yaw = 0.0f;      // obj+0x50
    Vec3 waypoint{};               // obj+0x12c
    int32_t attach_point_a = 0;    // own (A), kept on the leader's (B) by attach_to
    int32_t attach_point_b = 0;
    float height_offset = 0.0f;    // obj+0x5c
    float speed = 0.0f;
    float max_speed = 50.0f;
    float acceleration = 10.0f;
    float deceleration = 15.0f;
    float friction = 64.0f;        // obj+0x44
    float gravity = 32.0f;         // obj+0x48, units/s^2
    Vec3 velocity{};
    Vec3 push{};                   // obj+0x294 (this frame only: walking, pushes)
    float parameter = 0.0f;        // obj+0x302
    float parameter_timer = 0.0f;  // obj+0x306
    int32_t touch_damage = 0;      // obj+0x30a

    // ---- Path state (obj+0xe6..0x100) ----
    int32_t path = 0;
    float path_time = 0.0f;
    float path_speed = 1.0f;
    std::array<float, 4> path_speeds{{0.0f, 0.0f, 0.0f, 0.0f}}; // opcode 164
    int32_t path_stop = -1;
    Vec3 path_origin{};
    float path_yaw_offset = 0.0f;

    // ---- Sound / effects ----
    std::string frame_sound = "";  // obj+0x140
    int32_t frame_sound_frame = 0; // obj+0x144
    int32_t effect_frames = 0;
    float effect_time = 0.0f;
    int32_t thrown_kind = 0;       // obj+0x30a (Kurt's thrown item)
    int32_t item_ticks = 0;        // obj+0x30e
    float blast_range = 1000.0f;   // obj+0x2c4 (opcode 177)
    int32_t shot_part = 0;         // obj+0x21c (part + 1 of the last sniper round)
    Vec3 shot_point{};             // obj+0x210
    Mat4 rolling_basis = Mat4::identity(); // obj+0x326 basis while rolling
    float roll_radius = 0.0f;      // obj+0x326 (0 or less counts as 1)
    Vec3 swing_pivot{};            // obj+0x1c
    float swing_speed = 0.0f;      // obj+0x302
    float swing_gain = 0.0f;       // obj+0x30a
    float swing_length = 0.0f;     // obj+0x306
    float swing_yaw = 0.0f;        // obj+0x30e
    int32_t rope_color = 0;        // obj+0x2d0
    int32_t rope_mask = 0;
    std::array<Vec3, 4> rope_points{{}};
    MdkObject* attached = nullptr; // carried along (a pickup's chute)

    // ---- Doors (obj+0x306..0x32a) ----
    // ---- Door state bits (obj+0x14d): the low 4 are the engine's, the
    // scripts set only the rest (ObjectBehaviors.DoorEngineBits) ----
    static constexpr int32_t DoorEngineBits = 0xF;
    std::array<int32_t, 2> door_animations{{0, 0}}; // CMI offsets: open, close
    int32_t door_state = 0;
    float door_distance = 20.0f;   // Kurt opens it when closer than this
    std::array<std::string, 4> door_sounds{{"", "", "", ""}}; // opening, closing, open, closed
    int32_t lock_parts = 0;        // parts named LOCK
    int32_t hatch_parts = 0;       // parts named HC...
    std::string connects = "";     // arena on the other side of a connector

    // ---- Tracked sounds ----
    std::string tracked_sound = ""; // if_own_sound (play_sound flag 4)
    int32_t tracked_voice = 0;
    int32_t loop_sound = 0;        // the looping sound (voices, 0 = none)
    std::string loop_sound_name = "";
    std::array<std::string, 2> labels{{"", ""}}; // obj+0x154 explosion, obj+0x150 ricochet

    // ---- Animation state ----
    const formats::ModelAnimation* animation = nullptr;
    float animation_time = 0.0f;
    int32_t animation_frame = 0;
    float animation_rate = AnimationFps; // obj+0xe0
    int32_t animation_end_frame = AnimationEnded; // obj+0x118

    // The orientation: yaw around Z, pitch raises the nose, roll turns around
    // the forward axis. Matches the C# RotationOf(roll, pitch, yaw) order.
    Mat4 rotation() const;

    // The C#'s Transform: model space to the world, as the C# composes it,
    // CreateScale(Scale) * (rolling ? RollingBasis : Rotation) *
    // CreateTranslation(Position + UnitZ * Lift), in that order.
    Mat4 transform() const;

    // The C#'s PoseParts(): the vertices of each part in the current pose
    // (model space), hidden parts INCLUDED -- the C#'s Pose() is this with the
    // hidden parts emptied, and ObjectView hides them per batch instead.
    //
    // The reference is to a shared cache (the C#'s static RestPoses / Baked),
    // so it stays valid until forget_poses(). The C# shares these across every
    // object because a level has a handful of models drawn by many objects.
    const std::vector<std::vector<formats::Vec3f> >& pose_parts() const;

    // The C#'s ForgetPoses(): a level change drops the last level's models.
    // The caches are keyed by Model/ModelAnimation ADDRESS, so a level whose
    // models were freed MUST call this or a new model that lands on a freed
    // address would draw the old one's pose.
    static void forget_poses();
    // Starts/stops rolling; enabling captures the current orientation once.
    void set_rolling(bool enabled);

    // A rolling object rests on its origin but its model is lifted by the height
    // offset (0x43b65c: z + obj+0x5c), e.g. LEVEL6's centred boulder XBO.
    float lift() const { return (flags & FlagRolling) != 0 ? height_offset : 0.0f; }

    // World position of a model reference point (the C#'s ReferencePoint): the
    // point scaled and rotated about Z by yaw, added to the position; the position
    // itself when the model has no such point (index out of range). Used by the
    // attach motion (command 74) to hang a chute on its leader.
    Vec3 reference_point(int index) const;
    // World position of the centre of a model part at rest (the C#'s PartCenter):
    // the part box's mean, scaled and rotated about Z by yaw, at the position.
    Vec3 partCenter(int index) const;
    // The first part with this name, case-insensitively (the C#'s FindPart); -1.
    int32_t findPart(const std::string& name) const;

    // Still unported MdkObject behavior:
    //   Footing -> the Solids footing enum (collision slice)
    //   _ticked + remember() -> frame-blend state for position_at()/yaw_at()/transform_at()
    //   transform_at() -> Mat4 arithmetic + FrameBlend (transform() itself is here)
    //   pose() (pose_parts() with the hidden parts emptied) and part_bounds()
    // Animation, pose_bounds(), sound_position(), distance_to(), part_center(),
    // and yaw_to() are implemented in ScriptRuntime, SoundMixer, or this class.
    // sound_position() and distance_to() are inlined/owned elsewhere, not members
    // here. part_center() (rest-pose box centre) is not part_bounds()/pose_bounds()
    // (current-pose boxes).
};

} // namespace scripts
} // namespace mdk
