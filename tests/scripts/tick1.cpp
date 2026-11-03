// ============================================================================
// tick1.cpp — the level tick-1 golden-state check.
//
// The scripts' first end-to-end milestone: for each level, load it, place Kurt
// at the level's start, run one tick, and dump the object table. The arena
// controller (and, on some levels, the DTI's alien/static records) spawns the
// level's objects during that tick, and each runs its type's init script inside
// Spawn. The expected tables are the C# oracle's, byte for byte, in golden/.
//
// The oracle is /tmp/kilo/oracle (headless Mdk.Game). Its driver sets Option=0
// and seed=1; this matches it exactly.
//
// WHY TICK 1 ONLY. A spawned object runs its init script inside Spawn and gets
// no motion update on the first tick beyond path-following, so a non-path object
// stays at its spawn position. Gravity/footing/movement commands are later
// motion slices; the multi-tick golden states land with them.
//
// PATH-RIDING OBJECTS. Level 8's first object (XBSHIP) rides a path: follow_path
// puts it at the path start and Motion's path-following step (motionPath) advances
// it one frame and turns it along the path. That is the one motion that moves an
// object at tick 1 (gravity objects are footing-held at their spawn), so it is in
// scope here; the rest of ObjectMotion is not.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include "test_support.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/sprite_animation.h"
#include "game/arena_space.h"
#include "game/level_data.h"
#include "game/object_view.h"
#include "game/player.h"
#include "render/arena_scene.h"
#include "render/material_resolver.h"
#include "scripts/script_runtime.h"
#include "util/matrix.h"
#include "util/math_lut.h"

using namespace mdk;
using namespace mdk::formats;

namespace {

MdkData locateData() {
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const FileSystem* system = fileSystem();
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    const FileSystem* system = fileSystem();
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

// A whole file's text, read with plain stdio (a test fixture, not game data).
bool readText(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out->clear();
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out->append(buf, n);
    }
    fclose(f);
    return true;
}

// The object table in the oracle's format.
std::string dumpTable(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 currentArena=%s objects=%d\n", level,
             ticks, runtime.currentArena().c_str(), runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "%03d type=%s arena=%s pos=(%.4f,%.4f,%.4f) yaw=%.4f hp=%d flags=0x%08X dead=%d "
                 "move=%d/%d connects=%s path=%d/%.4f/%.4f/%d\n",
                 index, o.type_name.c_str(), o.arena.c_str(), o.position.x, o.position.y,
                 o.position.z, o.yaw, o.health, (unsigned)o.flags, o.dead ? 1 : 0, o.move_command,
                 o.move_parameter, o.connects.c_str(), o.path, o.path_time, o.path_speed,
                 o.path_stop);
        out += line;
    }
    return out;
}

// The identity line both object dumps share. `roll` is left out on purpose:
// it is a transform input, not a pose input, and the port does not write it
// yet (ObjectMotion.Bank is not ported), so including it would make the pose
// golden fail for a reason that has nothing to do with the pose.
std::string objectIdentity(int index, const scripts::MdkObject& o) {
    char line[512];
    snprintf(line, sizeof(line),
             "OBJ %03d type=%s arena=%s model=%s anim=%s frame=%d hidden=%d visible=%d "
             "dead=%d scale=%.4f pos=%.4f,%.4f,%.4f yaw=%.4f pitch=%.4f effect=%d texframe=%d\n",
             index, o.type_name.c_str(), o.arena.c_str(),
             o.model ? o.model->name.c_str() : "-",
             o.animation ? o.animation->name.c_str() : "-", o.animation_frame, o.hidden_parts,
             o.visible ? 1 : 0, o.dead ? 1 : 0, o.scale, o.position.x, o.position.y, o.position.z,
             o.yaw, o.pitch, o.effect_frames, o.texture_frame);
    return std::string(line);
}

// The object pass's pose: the C#'s PoseParts, per object in the C#'s draw order
// (Viewer.DrawObjects walks ScriptRuntime.Objects). The pose is the rest
// vertices or the baked animation frame, and it is what the draw stage copies
// into the renderer's vertex stream, so a wrong bit here is a moved vertex.
std::string dumpObjectPoses(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        out += objectIdentity(index, o);

        // Per part: how many vertices, their sum in order (which pins the
        // order as well as the values), and the first and last.
        const std::vector<std::vector<Vec3f> >& pose = o.pose_parts();
        snprintf(line, sizeof(line), "  PARTS %d\n", (int)pose.size());
        out += line;
        for (size_t p = 0; p < pose.size(); p++) {
            const std::vector<Vec3f>& v = pose[p];
            float sx = 0.0f, sy = 0.0f, sz = 0.0f;
            for (size_t i = 0; i < v.size(); i++) {
                sx += v[i].x;
                sy += v[i].y;
                sz += v[i].z;
            }
            char first[64];
            char last[64];
            if (v.empty()) {
                snprintf(first, sizeof(first), "-");
                snprintf(last, sizeof(last), "-");
            } else {
                snprintf(first, sizeof(first), "%.4f,%.4f,%.4f", v[0].x, v[0].y, v[0].z);
                snprintf(last, sizeof(last), "%.4f,%.4f,%.4f", v[v.size() - 1].x, v[v.size() - 1].y,
                         v[v.size() - 1].z);
            }
            snprintf(line, sizeof(line), "  P %d n=%d sum=%.4f,%.4f,%.4f first=%s last=%s\n",
                     (int)p, (int)v.size(), sx, sy, sz, first, last);
            out += line;
        }
    }
    return out;
}

// The object pass's place: the C#'s Transform (scale * orientation *
// translation), which is where the yaw/pitch/roll order and a rolling object's
// captured basis show up.
std::string dumpObjectTransforms(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        out += objectIdentity(index, o);
        const Mat4 t = o.transform();
        snprintf(line, sizeof(line),
                 "  T %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                 "%.6f %.6f\n",
                 t.m[0], t.m[1], t.m[2], t.m[3], t.m[4], t.m[5], t.m[6], t.m[7], t.m[8], t.m[9],
                 t.m[10], t.m[11], t.m[12], t.m[13], t.m[14], t.m[15]);
        out += line;
    }
    return out;
}

// "x,y,z" -> Vec3, for a harness argument. False unless it is three numbers.
bool parsePoint(const char* text, Vec3* out) {
    float v[3];
    if (sscanf(text, "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
        return false;
    }
    out->x = v[0];
    out->y = v[1];
    out->z = v[2];
    return true;
}

// The Runner's RUNNING half, which nothing else can reach: level 7's Kurt starts
// 22.16 units from the hamster and the run needs 20 (ObjectBehaviors.RunnerStart),
// so in every shipped scenario the hamster only ever idles. This mode places Kurt
// where the C# probe's `runner` mode places him and prints the SW_H150's state
// after EVERY tick, so the push, the turn clamp, the stop test, the return to idle,
// the idle RNG that follows and the RUNNER voice are all in the golden. The C# twin
// of this dump is the oracle probe's `runner` mode and prints the same fields in the
// same order.
std::string dumpRunner(int level, int ticks, const Vec3& kurt, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 kurt=%.4f,%.4f,%.4f\n", level, ticks,
             kurt.x, kurt.y, kurt.z);
    out += line;
    for (int t = 1; t <= ticks; t++) {
        runtime.update(1.0f / 30.0f);
        int index = 0;
        for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
             it != runtime.objects().end(); ++it, ++index) {
            const scripts::MdkObject& o = *it;
            if (runtime.isRemoved(&o)) {
                index--;  // the oracle's list does not contain removed objects
                continue;
            }
            if (o.type_name != "SW_H150") {
                continue;
            }
            // The mixer's RUNNER voice is the audible side effect of entering H150_R
            // (ObjectBehaviors.UpdateRunner); the oracle prints whether it is playing, so
            // a dropped, renamed or wrongly-gated play() cannot pass the runner goldens.
            snprintf(line, sizeof(line),
                     "t=%d obj=%03d pos=%.4f,%.4f,%.4f yaw=%.4f anim=%s frame=%d "
                     "push=%.4f,%.4f,%.4f flags=0x%08X mixer=%s\n",
                     t, index, o.position.x, o.position.y, o.position.z, o.yaw,
                     o.animation ? o.animation->name.c_str() : "-", o.animation_frame, o.push.x,
                     o.push.y, o.push.z, (unsigned)o.flags,
                     runtime.mixer().isPlaying("RUNNER") ? "RUNNER" : "-");
            out += line;
        }
    }
    return out;
}

// Kurt's state names as the oracle prints them: the C# dump writes the Kurt.State
// enum itself, and the C++ enum is in the C#'s order (kurt.h), so the index is the
// state and the name is what both sides print.
const char* kurtStateName(Kurt::State state) {
    static const char* const kNames[] = {
        "Still", "Idle", "Run", "Side", "Turn", "Jump", "RunJump", "Fall", "Chute", "Land",
        "Shot", "RunFire", "Throw", "Knocked", "GetUp", "Dead", "Slip", "Slide", "SlideFast",
        "SlideBrake", "Hang", "HardLand", "RollLeft", "RollRight", "HelmetOn", "HelmetOff"};
    const int index = (int)state;
    return index >= 0 && index < (int)(sizeof(kNames) / sizeof(kNames[0])) ? kNames[index] : "?";
}

// The frame counts Kurt's state machine needs, from TRAVSPRT.BNI. The shipped game
// fills these from the renderer's KurtSprite; a host harness has no renderer, so it
// reads the same two steps KurtSprite.FrameCount does: the state->animation table
// (the C#'s KurtSprite.Animations, kurt_sprite.cpp's stateSpecs_) and the archive's
// frame count for that name. The dump prints the counts in its header line, so a
// table that disagrees with the oracle's fails the golden instead of drifting.
Player::FrameCounts kurtFrameCounts(const Bni& sprites) {
    static const char* const kAnimations[] = {
        "K_STILL", "K_IDLE", "K_RUN", "K_SIDE", "K_TRN45", "K_JUMP", "K_RJMP", "K_FALL",
        "K_CHUTE", "K_LAND", "K_SHOT", "K_RUNFIR", "K_SPWEP", "K_BANG", "K_BFLIP", "K_BANG",
        "K_SLIP", "K_SLIDE", "K_FSLIDE", "K_BSLIDE", "K_HANG", "K_CRASHL", "K_ROLLL", "K_ROLLR",
        "K_HELM", "K_HELM"};
    Player::FrameCounts counts;
    for (int i = 0; i < Player::kStateCount; i++) {
        SpriteAnimation animation;
        counts.values[i] = sprites.has(kAnimations[i]) &&
                                      sprites.animation(kAnimations[i], &animation)
                                  ? animation.frameCount()
                                  : 0;
    }
    return counts;
}

// Kurt's own state machine and the sounds it makes, stepped the way the C# Viewer
// steps him (Viewer.Step, 1/60 s) with a scripted input. This is the only golden
// that drives Player::update at all: until now the walk, the run, the jump, the
// chute, the fall and the landing were pinned by nothing, and so were FOOT1-4,
// LAND, CHUTEOUT, CHUTEON and CHUTEIN. It now reaches HardLand and Dead as well, for
// the two lines of the chute sound that only those two states can show.
//
// The schedule is the same literal as the oracle probe's `kurt` mode, and it was
// chosen to cover every branch of the sound code: the forward run walks Kurt off the
// start pad, so the first LAND comes from a fall rather than a jump; the back-up
// phase runs the animation frame NEGATIVE, which is where Wrap and the floor of a
// negative frame are the whole story; the second forward run crosses the step frames
// going the other way; and the jump is held long enough to open the chute (CHUTEOUT,
// then CHUTEON played Once) and released long enough to close it (CHUTEIN, which is
// the Stop of CHUTEON plus a play). The `voices=` column is the mixer's live voices
// in launch order, which is the sequence of plays minus what was stopped: the
// headless device is never stepped, so a voice ends only when something stops it.
//
// WHY THE FEET ARE PRINTED IN 64THS OF A UNIT AND NOT AS DECIMALS. Player::facing()
// takes its sine and cosine from the trig LUT, whose documented per-period error is
// 4.76e-06 (util/math_lut.h), while C# Kurt.Facing uses MathF.Cos/Sin. Over this
// 470-step walk that leaves a drift of 1.34e-05 in the feet: bigger than 1/1000, so
// a decimal column straddles a rounding boundary in a dozen places, and 1000x
// smaller than the 1/64 quantum used here, so this column is byte-exact. The speeds,
// the yaw, the animation frame and the flags are printed as decimals and match the
// oracle exactly, because nothing in their arithmetic uses the LUT. The finding that
// facing() should use libm the way util/matrix.h's create_from_axis_angle does is
// recorded in docs/OPEN_ITEMS.md with its measurement: it makes the feet match at
// four decimals and moves the pinned frame closer to the oracle (1.240% -> 1.095%),
// and it is owner-gated because it changes the picture and the frame manifest is
// owner-protected.
//
// WHY `closing=` AND `hp=` ARE COLUMNS AND NOT INVISIBLE. Two lines the schedule
// visits had no case that went red when they were deleted (review-8 R8-4):
//
//   * `Player::update`'s `_chuteClosing = false` at the top of every step. The
//     schedule does reach the one place that SETS it (the Chute -> Fall transition),
//     but nothing printed the field, so deleting the clear left the closing-chute
//     frame stuck on after the first hard fall and the golden still passed. The
//     consumer is kurt_sprite.cpp's closing-frame selection, which no host harness
//     runs; this column pins the field the consumer reads.
//   * `Player::land`'s `damage(kHardLandingDamage)` on a hard landing. The only
//     hard landing in the run is followed by a scripted `setHealth(0)`, so the
//     10 points it takes off were invisible: deleting the call changed nothing the
//     dump could see. `hp=` shows 100 -> 90 at the landing step and holds it until
//     the scripted kill, which is the oracle's own Health.
//
// WHAT THE COLUMNS PIN IS REMOVAL, NOT PLACEMENT (review-9 R9-5). Two mutants of
// the same lines survive every scenario on both sides, and both are EQUIVALENT
// mutants rather than gaps a schedule could close:
//
//   * Guarding the top-of-step clear with `if (!dead)` survives, because `dead`
//     and `_chuteClosing` can never be true at the same top-of-step: the clear's
//     setter (`_chuteClosing = true`, the Chute -> Fall transition) is inside the
//     `!onFloor` branch of updateState, and `dead = true` requires `onFloor`, and
//     `onFloor` does not flip back inside the step that sets either. So the guard
//     is dead code, and no input -- including the scenario review-9 asked for, a
//     chute closed in the air followed by a lethal landing, which was built and
//     run and matches byte for byte -- can tell the two apart.
//   * Dropping `if (health > 0)` around the hard landing's `damage` survives,
//     because `Player::damage` floors health at 0 exactly like `Kurt.Damage`'s
//     `Math.Max(Health - damage, 0)`, so the call is a no-op at health 0 and the
//     guard only passes when health is above 0 anyway. In the C# the guard is
//     observable through `HurtFlash` and `KnockDamage`, which `Player` does not
//     model (they live on `mdk::Kurt`, and the split is OPEN_ITEMS.md's next
//     slice); when they land, the guard lands with them and needs its own pin.
//
// WHY A SECOND SCENARIO. Kurt can only die once, so the lethal HARD LANDING --
// the C#'s `HardLanding` returning early at `Health == 0`, which is what keeps a
// dead Kurt out of K_CRASHL and leaves his chute as it was -- cannot share a run
// with the chute-open death the schedule above already pins. It is its own run,
// `golden/L3_kurt_lethal.txt`, and it is not decoration: guarding the HardLand
// state entry on `health > 0` alone (dropping the C#'s `Health == 0` test) is a
// mutant the 950-step schedule does not see and this one does.
std::string dumpKurt(int level, int steps, Player player, const Player::FrameCounts& counts,
                     const audio::SoundMixer& mixer, bool lethal) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d steps=%d counts=", level, steps);
    out += line;
    for (int i = 0; i < Player::kStateCount; i++) {
        snprintf(line, sizeof(line), "%s%s=%d", i ? "," : "", kurtStateName((Kurt::State)i),
                 counts.values[i]);
        out += line;
    }
    out += "\n";

    // The scripted scenario, as literals on both sides (the oracle probe's `kurt`
    // mode holds the same numbers). Steps 1..470 are the original walk / run-back /
    // jump schedule, unchanged; the phases after it exist to reach the two states the
    // original schedule never got to, which are the two states whose sound code was
    // pinned by nothing.
    //
    //   471  lifted kHardLift above the start pad, verticalSpeed zeroed
    //   642  jump pressed: the chute opens at -182 u/s, far too fast for its brake
    //   650  CHUTEON starts flapping
    //   656  HARD LANDING at -124 u/s: land() closes the chute and enters HardLand
    //   657  CHUTEON stops and CHUTEIN plays, while Kurt is still in HardLand
    //   662  jump released (harmless: HardLand does not read the input)
    //   760  K_CRASHL finishes, back to Still
    //   770  jump pressed and HELD to the end of the run
    //   833  CHUTEON starts flapping again
    //   860  a script sets the health to zero in mid-air
    //   902  he touches the floor with the chute open: health 0 on a floor is death
    //   903  CHUTEON stops with NO CHUTEIN, and the chute stays open on the corpse
    //
    // WHY PHASE 4 IS THERE. A hard landing needs verticalSpeed below -100 u/s and
    // nothing reachable from the level-3 start gets past -72.5, so the step at 471
    // moves Kurt straight up the way a script teleport does and lets him fall. The
    // press at 642 is late on purpose: the chute's brake takes 3.9 u/s off every
    // step, so pressing earlier would land him too slowly to be a hard landing, and
    // pressing later would land him before CHUTEON had its four opening frames. Six
    // steps of slack each way. What that buys is the pin on Player::update calling
    // updateChuteSound BEFORE the HardLand early return: land() has closed the chute,
    // CHUTEON is still flapping, and only that call stops it and plays CHUTEIN, one
    // step after the hit. Move the call below the return and CHUTEIN does not appear
    // for another 103 steps.
    //
    // WHY PHASE 5 IS THERE, AND WHY IT IS NOT A FALL OUT OF THE ARENA. The dead branch
    // of Player::update stops the chute sound, and a death that arrives with the chute
    // already closed cannot tell whether that call is there. So the chute is open and
    // CHUTEON is flapping when he dies, and the step after the death shows CHUTEON
    // gone with no CHUTEIN in its place; delete the call and CHUTEON flaps for the
    // rest of the golden. The natural death would be falling out of the arena, but
    // that is unreachable in this harness: ArenaSpace::bottom only ever answers with
    // the lowest point of an arena that CONTAINS the feet, and containment needs the
    // feet to be at or above that point, so `feet.z <= bottom - 50` cannot be true
    // unless a script has pinned a solid arena -- and this harness pins none, on
    // purpose, so that both sides resolve footing the same way. So the kill is the
    // health itself: setHealth(0) is the oracle's own public SetHealth (Kurt.Combat
    // .cs:169), which is how a script or an opcode kills him, and the death is still
    // the ported code's, because it is the landing step that finds health 0 on a
    // floor and decides he is dead.
    const Vec3 kStart = player.feet;
    const int kHardLiftStep = 471;
    const float kHardLift = 300.0f;
    const int kHardChuteStep = 642;
    const int kHardChuteRelease = 662;
    const int kDeathJumpStep = 770;
    const int kMortalStep = 860;
    // The lethal scenario: the same 300-unit lift, but Kurt's health is already 0
    // when he lands, and the fall from that height reaches -194 u/s, well past the
    // -100 hard-landing line. Nothing is pressed: the chute never opens, so the
    // only thing in the dump is the hard landing at zero health and the steps of
    // death that follow it.
    const int kLethalLiftStep = 1;

    for (int step = 1; step <= steps; step++) {
        if (lethal) {
            if (step == kLethalLiftStep) {
                player.feet = Vec3{kStart.x, kStart.y, kStart.z + kHardLift};
                player.verticalSpeed = 0.0f;
                player.setHealth(0);
            }
        }
        if (step == kHardLiftStep && !lethal) {
            player.feet = Vec3{kStart.x, kStart.y, kStart.z + kHardLift};
            player.verticalSpeed = 0.0f;
        }
        if (step == kMortalStep && !lethal) {
            player.setHealth(0);
        }
        Input input;
        if (!lethal) {
            input.down[kKeyForward] = (step >= 10 && step < 46) || (step >= 190 && step < 285);
            input.down[kKeyBack] = step >= 100 && step < 190;
            input.down[kKeyJump] = (step >= 300 && step < 390) ||
                                   (step >= kHardChuteStep && step < kHardChuteRelease) ||
                                   (step >= kDeathJumpStep);
        }
        player.update(input, 1.0f / 60.0f);

        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = mixer.voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }

        snprintf(line, sizeof(line),
                 "s=%d state=%s feet64=%d,%d,%d yaw=%.4f fwd=%.4f str=%.4f vert=%.4f "
                 "frame=%.4f floor=%d chute=%d closing=%d hp=%d voices=%s\n",
                 step, kurtStateName(player.state()),
                 (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                 (int)std::floor(player.feet.z * 64.0f), player.yaw, player.forwardSpeed,
                 player.strafeSpeed, player.verticalSpeed, player.animationFrame(),
                 player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.chuteClosing() ? 1 : 0,
                 player.health(), voices.empty() ? "-" : voices.c_str());
        out += line;
    }
    return out;
}

// Build the world the Kurt dump steps through — the reachable arenas, the level's
// sound bank, TRAVSPRT.BNI's frame counts, Kurt at the level's start — and produce
// the dump. Shared by the golden and by `tick1 dump kurt`, so the diff of a failing
// golden is the same text the golden compares.
bool kurtStepsDump(int level, int steps, bool lethal, const MdkData& data, const Bni* sprites,
                   std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    LevelData levelData;
    audio::SoundBank bank;
    ArenaSpace space;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the sound bank did not load";
        return false;
    }
    bank.preload();
    // The same world the oracle probe builds: the reachable arenas, and no pinned
    // solid arena, so both sides resolve footing from the arenas around Kurt, and no
    // ScriptRuntime, so neither has object solids or a fan to trip over.
    const std::vector<const Arena*>& arenas = levelData.arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (levelData.isReachable(arenas[i]->name())) {
            space.add(arenas[i]);
        }
    }
    audio::SoundMixer mixer(&bank);
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setMixer(&mixer);
    const Player::FrameCounts counts = kurtFrameCounts(*sprites);
    player.setFrameCounts(counts);
    *out = dumpKurt(level, steps, player, counts, mixer, lethal);
    return true;
}

// One scripted hurt_kurt: the step it fires on and the damage it deals. The
// schedule is printed in the dump's own header, so a golden can never be compared
// against a different one (the oracle prints the same literal on its own side).
struct KurtHurt {
    int step;
    int damage;
};

// The bridge between the scripts' hurt_kurt and the ONE Kurt health, as the C++
// twin of the oracle probe's `hurt` mode.
//
// WHY THIS EXISTS. The C# Kurt is one class with one Health (Kurt.Combat.cs:76):
// Hurt writes it, and the same field is what "health 0 --on the floor--> DEAD"
// reads and what the HUD draws. This port splits the class into a movement half
// (mdk::Player) and a combat half (mdk::Kurt), and a split is where a second copy
// hides: if ScriptRuntime::kurt() handed out its own Kurt instead of the entity's,
// hurt_kurt (opcode 109) would move a field the death transition and the HUD never
// read, and every gate would stay green while a script shot a full-health Kurt. The
// two columns hp= (the entity's health, i.e. what the death check and the HUD read)
// and vmhp= (what the VM reads through ScriptRuntime::kurt()) are the whole content
// of the golden: in the oracle they are one property and cannot disagree.
//
// WHY EVERY HURT IS IN THE AIR. On a floor any hurt of 5 or more knocks Kurt down
// (UpdateKnockDamage, Kurt.Combat.cs:214: KnockDamage >= 5 while OnFloor), and the
// knock-down state machine is a subsystem this port has not landed -- so a hurt on
// the ground would put the oracle into K_BANG and the port nowhere, which is a
// different gap, not this one. The oracle's own check declines to knock him unless
// it finds a floor within 13 units below (KnockdownFloorDistance), so the lift at
// step 1 puts every hurt in the part of the fall where the two halves agree, and
// the last hurt takes his health to zero, whose Health > 0 guard disables the knock
// check for the rest of the fall. The landing is then the lethal one
// golden/L3_kurt_lethal.txt already pins, and the hurt at step 200 is Hurt's
// Health == 0 early return on a corpse.
//
// THE SECOND SCHEDULE (`hurtland`, golden/L3_kurt_hurtland.txt) pins the OTHER
// half of Kurt.Hurt: the state guard, not the health guard. Kurt.Combat.cs:125
// declines a hurt while Current is Dead, Knocked, GetUp or HardLand, and this port
// splits the class, so the guard is only faithful if the state hurt() reads is the
// state the machine is in -- a copy of its own is a field nothing writes and the
// guard never fires. That schedule lifts him the same 300 units, so the landing at
// step 183 is a hard one (K_CRASHL is 26 frames x kCrashTicksPerFrame, 2, = 52
// ticks, and UpdateSpecial advances it kTicks x 1/60 = 0.5 ticks a step, so 104
// steps, 183..286), hurts him 10 INSIDE that window -- the oracle declines, hp
// stays 90 -- and 4 AFTER it, where the guard has released and the health moves. The second
// damage is under KnockdownDamage (5) on purpose: on a floor a hurt of 5 or more
// knocks him down, and the knock-down machine is not ported, so the release arm has
// to be provable without dragging in a subsystem neither side has.
//
// THE THIRD SCHEDULE (`knock`, golden/L3_kurt_knock.txt) pins the recent-damage
// counter itself (Kurt.Combat.cs:84, 0x573b20) and the two rules this port did not
// reproduce: the decline at the top of every step (UpdateKnockDamage's tail,
// Kurt.Combat.cs:230-231 -- clamp into [0, KnockdownDamage], then drain
// KnockdownDrain per second) and the hard landing's unconditional reset of it
// (Kurt.cs:583, between the damage and the dead/knocked guard). knock= is the
// counter at every step, so a port that declines nothing, or that lets a counter
// survive a landing, fails on its own line. WHY THE THREE HURTS AND THEIR VALUES:
// the 25 at step 20 is above KnockdownDamage on purpose -- high in the air the
// oracle's own trigger cannot reach a floor (it probes 13 units down), so the
// counter is CLAMPED to 5 rather than knocking him down, and the clamp and the drain
// are both visible on the way down; the 4 at step 175 is under the threshold and
// arrives at the hard landing at about 3.7, the value the reset has to erase (a 5 or
// more there would put the oracle into K_BANG, a state machine neither side has);
// the 3 at step 300 is after the K_CRASHL window, on the floor, and again under the
// threshold, so the drain is visible on the ground too.
//
// The world is kurtStepsDump's (reachable arenas, no pinned solid arena, the
// archive's frame counts, the level's sound bank) plus the ScriptRuntime the runner
// mode builds, bound to THIS Player -- which is the point: the runtime and the
// movement half are the same entity, as C# Viewer's single Kurt is
// (ScriptRuntime.cs:168). The step order is the frontend's (Kurt, then the scripts;
// Viewer.cs:505-535, mdk_linux/src/main.cpp), and the 1/60 call lets the runtime
// complete a script tick every second step.
enum KurtHurtSchedule {
    kHurtHealth,  // the one health, from both sides of the class split
    kHurtHardLand,  // the state guard, inside and after K_CRASHL
    kHurtKnock,     // the recent-damage counter's decline and the landing's reset
    kHurtInvuln,    // the invulnerability timer's decline, on the floor and on a corpse
    kHurtKnockdown  // the knock-down itself: K_BANG, K_BFLIP, and back to Still
};

// An invulnerability write: the step and the seconds. It is the write opcode 244's set
// arm performs (ScriptVm.cs:1598, `runtime.Kurt.Invulnerable = F(o[1])`), which is what
// the C++ twin does through Kurt::setInvulnerable and the oracle probe does through
// scripts.Kurt.Invulnerable -- the same field, the same value, no extra arithmetic.
struct KurtInvulnSet {
    int step;
    float seconds;
};

enum KurtHurtColumns {
    kColsVoices,  // hp= vmhp= voices=
    kColsKnock,   // hp= vmhp= knock=
    kColsInvuln,    // hp= vmhp= invuln= knock=
    kColsKnockdown  // hp= vmhp= invuln= knock= frame=
};

// One schedule as one literal, so the dump body and the oracle probe's twin drive the
// same plan. Every number in it is printed in the header line, so a golden can never be
// compared against a different schedule than the one it was captured from.
struct KurtHurtPlan {
    float lift;       // units above the start pad, applied at step 1
    const KurtHurt* hurts;
    size_t hurtCount;
    const KurtInvulnSet* sets;
    size_t setCount;
    int mortalStep;   // step that sets the health to 0; 0 for none
    int columns;
    // The three the knock-down schedule uses, all zero for the older ones (they are
    // defaulted here so the older aggregate initializers above stay warning-free).
    float airLift = 0.0f;   // units above the pad, applied at airLiftStep; 0 for none
    int airLiftStep = 0;
    int forwardFrom = 0;    // first step the forward key is held (0 = never held)
    int forwardTo = 0;      // last step it is held
};

std::string dumpKurtHurt(int level, int steps, scripts::ScriptRuntime& runtime, Player& player,
                         audio::SoundMixer& mixer, const KurtHurtPlan& plan) {
    const KurtHurt* kHurts = plan.hurts;
    const size_t hurtCount = plan.hurtCount;
    const float kLift = plan.lift;

    char line[512];
    std::string out;
    snprintf(line, sizeof(line), "level=%d steps=%d seed=1 lift=%.4f hurts=", level, steps, kLift);
    out += line;
    for (size_t i = 0; i < hurtCount; i++) {
        if (i) {
            out += ",";
        }
        snprintf(line, sizeof(line), "%d:%d", kHurts[i].step, kHurts[i].damage);
        out += line;
    }
    if (plan.setCount) {
        out += " sets=";
        for (size_t i = 0; i < plan.setCount; i++) {
            if (i) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%d:%.4f", plan.sets[i].step, plan.sets[i].seconds);
            out += line;
        }
    }
    if (plan.mortalStep) {
        snprintf(line, sizeof(line), " mortal=%d", plan.mortalStep);
        out += line;
    }
    if (plan.airLiftStep) {
        snprintf(line, sizeof(line), " air=%d:%.4f", plan.airLiftStep, plan.airLift);
        out += line;
    }
    if (plan.forwardFrom) {
        snprintf(line, sizeof(line), " forward=%d..%d", plan.forwardFrom, plan.forwardTo);
        out += line;
    }
    out += "\n";

    const Vec3 kStart = player.feet;
    for (int step = 1; step <= steps; step++) {
        if (step == 1 && kLift != 0.0f) {
            player.feet = Vec3{kStart.x, kStart.y, kStart.z + kLift};
            player.verticalSpeed = 0.0f;
        }
        if (step == plan.airLiftStep && plan.airLift != 0.0f) {
            // The same scripted move dumpKurt uses at its hard-lift step: a script
            // teleport, so the feet move and the vertical speed is zeroed with them.
            player.feet = Vec3{kStart.x, kStart.y, kStart.z + plan.airLift};
            player.verticalSpeed = 0.0f;
        }
        Input input;  // nothing pressed except the plan's own forward window
        if (plan.forwardFrom) {
            input.down[kKeyForward] = step >= plan.forwardFrom && step <= plan.forwardTo;
        }
        player.update(input, 1.0f / 60.0f);
        // The scripts' step, and the writes inside it: in the oracle a hurt_kurt is
        // dispatched from a script object's program during ScriptRuntime::update,
        // never between the oracle's own statements. The invulnerability write is the
        // same kind of event (opcode 244), and the health write is Kurt.SetHealth, the
        // oracle's own public setter, which the `kurtlethal` schedule already uses.
        for (size_t i = 0; i < plan.setCount; i++) {
            if (plan.sets[i].step == step) {
                runtime.kurt().setInvulnerable(plan.sets[i].seconds);
            }
        }
        if (plan.mortalStep == step) {
            runtime.kurt().setHealth(0);
        }
        for (size_t i = 0; i < hurtCount; i++) {
            if (kHurts[i].step == step) {
                runtime.hurtKurt(kHurts[i].damage);
            }
        }
        runtime.update(1.0f / 60.0f);

        // The C# probe prints one shared mixer; the port's runtime owns its own and
        // the Player has another, so the column is the two lists concatenated. Both
        // are empty here, which is itself the pin that a hurt plays no sound. The
        // knock schedule prints the counter instead of the voices, because that is
        // what it pins.
        std::string voices;
        const audio::SoundMixer* mixers[2] = {&runtime.mixer(), &mixer};
        for (size_t m = 0; m < 2; m++) {
            const std::vector<audio::SoundMixer::Voice>& live = mixers[m]->voices();
            for (size_t i = 0; i < live.size(); i++) {
                if (!voices.empty()) {
                    voices += ",";
                }
                voices += live[i].name;
            }
        }

        if (plan.columns == kColsKnockdown) {
            snprintf(line, sizeof(line),
                     "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                     "invuln=%.4f knock=%.4f frame=%.4f\n",
                     step, kurtStateName(player.state()),
                     (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                     (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                     player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                     runtime.kurt().health(), player.combat().invulnerable(),
                     player.combat().knockDamage(), player.animationFrame());
            out += line;
            continue;
        }

        if (plan.columns == kColsInvuln) {
            snprintf(line, sizeof(line),
                     "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                     "invuln=%.4f knock=%.4f\n",
                     step, kurtStateName(player.state()),
                     (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                     (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                     player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                     runtime.kurt().health(), player.combat().invulnerable(),
                     player.combat().knockDamage());
            out += line;
            continue;
        }

        if (plan.columns == kColsKnock) {
            snprintf(line, sizeof(line),
                     "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                     "knock=%.4f\n",
                     step, kurtStateName(player.state()),
                     (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                     (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                     player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                     runtime.kurt().health(), player.combat().knockDamage());
            out += line;
            continue;
        }

        snprintf(line, sizeof(line),
                 "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                 "voices=%s\n",
                 step, kurtStateName(player.state()),
                 (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                 (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                 player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                 runtime.kurt().health(), voices.empty() ? "-" : voices.c_str());
        out += line;
    }
    return out;
}

// Build the world the hurt dump steps through: kurtStepsDump's, plus the CMI and
// the ScriptRuntime bound to the Player it drives. Shared by the golden and by
// `tick1 dump hurt`, so a failing golden diffs the same text it compares.
bool kurtHurtDump(int level, int steps, int schedule, const MdkData& data, const Bni* sprites,
                  std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    LevelData levelData;
    audio::SoundBank bank;
    ArenaSpace space;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    char cmiPath[128];
    snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
    std::vector<uint8_t> cmiBytes;
    Cmi cmi;
    if (!data.read(cmiPath, &cmiBytes) ||
        !Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
        *why = "the CMI did not parse";
        return false;
    }
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the sound bank did not load";
        return false;
    }
    bank.preload();
    const std::vector<const Arena*>& arenas = levelData.arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (levelData.isReachable(arenas[i]->name())) {
            space.add(arenas[i]);
        }
    }
    audio::SoundMixer mixer(&bank);
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setMixer(&mixer);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    // The runtime is bound to THIS Player, so its kurt() is the entity's own combat
    // half. Passing nullptr here would be the two-healths defect wearing a harness:
    // the runtime would fall back to its unbound combat state and hp= and vmhp=
    // would part on the first hurt.
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 0;
    // The schedules, driven on the same world: the one that pins the health bridge,
    // the one that pins the state guard, the one that pins the recent-damage counter,
    // and the one that pins the invulnerability timer. All literals are the oracle's own
    // and all print themselves in the header line.
    static const KurtHurt kHealthHurts[] = {{20, 10}, {40, 25}, {60, 30}, {80, 35}, {200, 10}};
    static const KurtHurt kHardLandHurts[] = {{220, 10}, {300, 4}};
    static const KurtHurt kKnockHurts[] = {{20, 25}, {175, 4}, {300, 3}};
    // The invulnerability schedule, on the FLOOR and with no lift, because the point is
    // to refuse a hurt for no reason but the timer -- the HardLand state is the other
    // reason hurt() refuses, and a lift would bring it in. Kurt stands on the level's own
    // start pad, and every hurt is under KnockdownDamage (5), because on a floor a hurt
    // of 5 or more puts the oracle into K_BANG, a state machine neither side has.
    //   20: the timer opens for 3 s, the oracle's KnockdownInvulnerability, the value a
    //       knock-down would have set it to (Kurt.Combat.cs:176).
    //   40: a hurt inside the window. Refused, and hurt()'s guard zeroes the counter.
    //  210: thirty steps after the window closes -- 3 s is 180 steps, so the timer reads
    //       0.0000 from step 200 -- the guard has released and the health moves.
    //  280: a second hurt, so the release is the timer expiring and not one lucky step.
    //       It is late on purpose: the hurt at 210 leaves the recent-damage counter at 4
    //       and it drains by 2 per second, so a hurt before step 271 would take the
    //       counter to KnockdownDamage or past it and the oracle -- which is standing on a
    //       floor, where the trigger needs no floor probe -- would enter K_BANG, the
    //       knock-down state machine neither side has. At 280 the counter is at 1.6667 and
    //       the hurt of 3 leaves it at 4.6667.
    //  300: Kurt.SetHealth(0), the oracle's own setter, so the next step is the death
    //       transition on the floor.
    //  305: the timer opens again on a corpse, five steps after the death, and the oracle
    //       keeps running it down through UpdateDeath's early return (Kurt.cs:139 is the
    //       second statement of Update, ahead of the Dead return at :152). A decay placed
    //       below Player::update's `if (dead) return` freezes it at 2.0000 here.
    static const KurtHurt kInvulnHurts[] = {{40, 4}, {210, 4}, {280, 3}};
    static const KurtInvulnSet kInvulnSets[] = {{20, Kurt::kKnockdownInvulnerability},
                                                {305, 2.0f}};
    // THE KNOCK-DOWN SCHEDULE (golden/L3_kurt_knockdown.txt). Every schedule above kept
    // its damage under KnockdownDamage (5) precisely so the oracle would NOT enter
    // K_BANG, a state machine this port did not have. Now it has one, and this is the
    // golden that was waiting for it. What each number is for:
    //   20: a hurt of 5 on the start pad. On a floor the trigger needs no floor probe,
    //       so step 21 is KnockDown: the counter spent, 3 s of invulnerability open,
    //       and K_BANG (25 frames) starting on that same step.
    //   25..60: the forward key held, entirely inside K_BANG. The C# zeroes the walk
    //       INPUT while Kurt is thrown, knocked down or getting up (Kurt.cs:213), so
    //       his feet must not move; drop that and he walks while flat on his back.
    //   30: an opcode-244 write of ZERO. KnockDown had just opened 3 s of
    //       invulnerability and the whole animation is 104 steps, so without this the
    //       hurt at 40 would be refused by the TIMER and hurt()'s Knocked arm -- the
    //       arm batch 41 could not reach -- would be pinned by nothing. It is the same
    //       write opcode 244 performs, with 0 in it, and the decline keeps it there.
    //   40: a hurt of 4 with the timer at zero and Kurt in K_BANG. The only thing that
    //       can refuse it is the state guard.
    //   70 and 124: K_BANG's 25 frames end and K_BFLIP's 27 begin (one frame per tick,
    //       two steps per frame at 1/60), and Still follows. frame= pins the flip's
    //       leftover arithmetic, which is AnimationFrame - bang and not a reset.
    //   150: lifted 6 units above the pad, so he is airborne with a floor inside the
    //       13-unit probe -- the geometry the port could not reach before.
    //   156: a hurt of 5 in the air, the OTHER arm: ArenaSpace::Floor finds the pad,
    //       the slam sets his vertical speed to -64, and he falls onto the floor
    //       already in K_BANG. vert= pins the slam; the landing stays under the
    //       -100 u/s hard-landing line on purpose, so the hard landing does not
    //       overlap this one.
    static const KurtHurt kKnockdownHurts[] = {{20, 5}, {40, 4}, {156, 5}};
    static const KurtInvulnSet kKnockdownSets[] = {{30, 0.0f}};
    KurtHurtPlan plan = {300.0f, kHealthHurts, sizeof(kHealthHurts) / sizeof(kHealthHurts[0]),
                         nullptr, 0, 0, kColsVoices};
    if (schedule == kHurtHardLand) {
        plan.hurts = kHardLandHurts;
        plan.hurtCount = sizeof(kHardLandHurts) / sizeof(kHardLandHurts[0]);
    } else if (schedule == kHurtKnock) {
        plan.hurts = kKnockHurts;
        plan.hurtCount = sizeof(kKnockHurts) / sizeof(kKnockHurts[0]);
        plan.columns = kColsKnock;
    } else if (schedule == kHurtInvuln) {
        plan.lift = 0.0f;
        plan.hurts = kInvulnHurts;
        plan.hurtCount = sizeof(kInvulnHurts) / sizeof(kInvulnHurts[0]);
        plan.sets = kInvulnSets;
        plan.setCount = sizeof(kInvulnSets) / sizeof(kInvulnSets[0]);
        plan.mortalStep = 300;
        plan.columns = kColsInvuln;
    } else if (schedule == kHurtKnockdown) {
        plan.lift = 0.0f;
        plan.hurts = kKnockdownHurts;
        plan.hurtCount = sizeof(kKnockdownHurts) / sizeof(kKnockdownHurts[0]);
        plan.sets = kKnockdownSets;
        plan.setCount = sizeof(kKnockdownSets) / sizeof(kKnockdownSets[0]);
        plan.airLift = 6.0f;
        plan.airLiftStep = 150;
        plan.forwardFrom = 25;
        plan.forwardTo = 60;
        plan.columns = kColsKnockdown;
    }
    *out = dumpKurtHurt(level, steps, runtime, player, mixer, plan);
    return true;
}

// The two floor probes, driven directly over every reachable arena: the C#'s
// ArenaSpace.Floor and ArenaSpace.FloorCrossing side by side on the same segments.
// The knock-down golden exercises floor THROUGH the game, but at one point on one pad,
// and the two probes differ in which arenas they search (around BOTH ends, pin ignored,
// vs Kurt's arenas around `from`, the pinned solid one when there is one) and in which
// hit they keep (the one NEAREST `from` by squared distance vs the FIRST found in arena
// order). Those differences only show on a segment that reaches across arenas or crosses
// more than one face, so this walks a long segment through every arena the level can put
// Kurt in, the knock-down's own 13-unit shape, and the same short segment reversed. The
// twin of the oracle probe's `floor` mode; the golden is golden/L3_floor_probes.txt.
//
// What it does NOT do is claim the two probes disagree somewhere: it prints both, so if a
// future level makes them part company the golden says where, and a port that quietly
// implements one as the other fails the moment a segment separates them.
bool floorProbeDump(int level, const MdkData& data, std::string* out, std::string* why) {
    LevelData levelData;
    ArenaSpace space;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    std::vector<const Arena*> probed;
    for (const Arena* arena : levelData.arenas()) {
        if (!levelData.isReachable(arena->name()) || arena->vertices().empty()) {
            continue;
        }
        space.add(arena);
        probed.push_back(arena);
    }

    char line[256];
    std::string o;
    snprintf(line, sizeof(line), "level=%d arenas=%d\n", level, (int)probed.size());
    o += line;

    for (size_t i = 0; i < probed.size(); i++) {
        Vec mn = probed[i]->vertices()[0];
        Vec mx = probed[i]->vertices()[0];
        for (const Vec& v : probed[i]->vertices()) {
            if (v.x < mn.x) mn.x = v.x;
            if (v.y < mn.y) mn.y = v.y;
            if (v.z < mn.z) mn.z = v.z;
            if (v.x > mx.x) mx.x = v.x;
            if (v.y > mx.y) mx.y = v.y;
            if (v.z > mx.z) mx.z = v.z;
        }
        const Vec c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
        const struct { const char* tag; Vec from; Vec to; } kProbes[] = {
            // Long, straight down through the arena and 20 units past its own floor, so
            // the segment's far end leaves the arena that holds its near end.
            {"long", Vec{c.x, c.y, mx.z + 1.0f}, Vec{c.x, c.y, mn.z - 20.0f}},
            // The knock-down's own query: 6 above the middle, 13 down from there.
            {"short", Vec{c.x, c.y, c.z + 6.0f}, Vec{c.x, c.y, c.z - 7.0f}},
            // The same short segment reversed and pushed up: an up-facing face is only
            // crossed one way, so this is the arm that answers "no floor".
            {"up", Vec{c.x, c.y, mn.z - 1.0f}, Vec{c.x, c.y, mx.z + 1.0f}},
        };
        for (const auto& p : kProbes) {
            Vec near;
            ArenaSpace::Floor first;
            const bool haveNear = space.floor(p.from, p.to, &near);
            const bool haveFirst = space.floorCrossing(p.from, p.to, &first);
            snprintf(line, sizeof(line), "i=%d name=%s probe=%s from=%.4f,%.4f,%.4f to=%.4f,%.4f,%.4f ",
                     (int)i, probed[i]->name().c_str(), p.tag, p.from.x, p.from.y, p.from.z, p.to.x,
                     p.to.y, p.to.z);
            o += line;
            if (haveNear) {
                snprintf(line, sizeof(line), "floor=%.4f,%.4f,%.4f ", near.x, near.y, near.z);
            } else {
                snprintf(line, sizeof(line), "floor=none ");
            }
            o += line;
            o += haveFirst ? (snprintf(line, sizeof(line), "cross=%.4f,%.4f,%.4f\n", first.point.x,
                                       first.point.y, first.point.z),
                              std::string(line))
                            : std::string("cross=none\n");
        }
    }
    *out = o;
    return true;
}

// Comma-join a list of ints, as the oracle's string.Join(",", list) does (empty when
// the list is empty).
std::string joinInts(const std::vector<int32_t>& v) {
    std::string s;
    char n[16];
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        snprintf(n, sizeof(n), "%d", (int)v[i]);
        s += n;
    }
    return s;
}

// The first line where two dumps differ, as one readable line. A whole object
// dump is hundreds of lines and only the first difference is a cause.
// One level at one tick count. Each object-pass golden carries its own list of
// these, so a case is pinned only at the ticks where the port reproduces the
// oracle byte for byte -- see the object-pass block in main().
struct GoldenCase {
    int level;
    int ticks;
};

bool pinnedIn(const GoldenCase* cases, size_t count, int level, int ticks) {
    for (size_t i = 0; i < count; i++) {
        if (cases[i].level == level && cases[i].ticks == ticks) {
            return true;
        }
    }
    return false;
}

std::string firstDifference(const std::string& got, const std::string& expected) {
    size_t i = 0;
    while (i < got.size() && i < expected.size() && got[i] == expected[i]) {
        i++;
    }
    if (i == got.size() && i == expected.size()) {
        return "identical";
    }
    size_t lineStart = got.rfind('\n', i);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    size_t lineEnd = got.find('\n', i);
    if (lineEnd == std::string::npos) {
        lineEnd = got.size();
    }
    size_t expectedEnd = expected.find('\n', i);
    if (expectedEnd == std::string::npos) {
        expectedEnd = expected.size();
    }
    // The expected side can be SHORTER than the line being reported (got has text
    // the golden does not), and its line can start past the end of the golden. A
    // substr past size() throws, which would replace the report with a crash.
    size_t expectedStart = lineStart > expected.size() ? expected.size() : lineStart;
    return "at byte " + std::to_string((int)i) + "\n  got      " + got.substr(lineStart, lineEnd - lineStart) +
           "\n  expected " + expected.substr(expectedStart, expectedEnd - expectedStart);
}

// The pass and shading names, spelled as the C# enum names print them.
const char* passName(Pass pass) {
    switch (pass) {
        case Pass::Solid: return "Solid";
        case Pass::DoubleSided: return "DoubleSided";
        case Pass::Mirror: return "Mirror";
        case Pass::Blended: return "Blended";
        case Pass::OnTop: return "OnTop";
        case Pass::Overlay: return "Overlay";
    }
    return "?";
}

const char* shadingName(Shading shading) {
    switch (shading) {
        case Shading::Original: return "Original";
        case Shading::Lit: return "Lit";
        case Shading::Sprite: return "Sprite";
    }
    return "?";
}

// The C#'s Viewer.ObjectLook: an arena's palette and archives, the level's own
// outside the arenas. The C++ LevelData caches the palette per arena, exactly as
// the C# does, so asking twice hands back the same palette (which is what makes
// the layout cache key the same across objects of one arena).
ObjectLook lookOf(LevelData& level, const std::string& arenaName) {
    ObjectLook look;
    const formats::Arena* arena = level.arenaNamed(arenaName);
    if (arena != nullptr) {
        look.palette = &level.paletteOf(arena);
        look.archives = level.archivesOf(arena);
    } else {
        look.palette = &level.dti().palette();
        look.archives.push_back(&level.levelTextures());
    }
    return look;
}

// The object pass's layout: what ObjectView.Build makes once per (model,
// palette), and what Arrange/Visible make of it per draw. The objects are walked
// in the draw stage's order (Viewer.DrawObjects: skip invisible, skip dead, skip
// no model) and each layout is printed the first time its pair is laid out,
// which is the order the C#'s layout cache fills.
//
// This is the golden that pins the layout stage at all: the surface grouping, the
// corner and first-triangle tables, the UV scale, the arrangement under hidden
// masks and the per-batch corner count. The pose goldens cannot see any of it.
std::string dumpObjectLayout(int level, int ticks, LevelData& levelData,
                             scripts::ScriptRuntime& runtime) {
    MaterialResolver resolver(&levelData.dti());
    ObjectView view(resolver);
    std::vector<std::pair<const formats::Model*, const formats::Palette*> > seen;

    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;
            continue;
        }
        if (!o.visible || o.dead || o.model == nullptr) {
            continue;
        }
        ObjectLook look = lookOf(levelData, o.arena);
        std::pair<const formats::Model*, const formats::Palette*> key(o.model, look.palette);
        bool done = false;
        for (size_t i = 0; i < seen.size(); i++) {
            done = done || seen[i] == key;
        }
        if (done) {
            continue;
        }
        seen.push_back(key);

        ObjectLayout* layout = view.layoutOf(o.model, look);
        if (layout == nullptr) {
            snprintf(line, sizeof(line), "LAYOUT obj=%d model=%s arena=%s batches=0\n", index,
                     o.model->name.c_str(), o.arena.c_str());
            out += line;
            continue;
        }
        snprintf(line, sizeof(line), "LAYOUT obj=%d model=%s arena=%s batches=%d\n", index,
                 o.model->name.c_str(), o.arena.c_str(), (int)layout->batches.size());
        out += line;

        for (size_t b = 0; b < layout->batches.size(); b++) {
            const ObjectBatch& batch = layout->batches[b];
            const Surface& s = batch.surface;
            snprintf(line, sizeof(line),
                     "  B%d tex=%s colour=%.4f,%.4f,%.4f,%.4f frames=%d pass=%s shading=%s "
                     "rowshift=%.4f corners=%d\n",
                     (int)b, s.texture ? s.texture->name().c_str() : "flat", s.colour.r, s.colour.g,
                     s.colour.b, s.colour.a, (int)s.frameCount, passName(s.pass), shadingName(s.shading),
                     s.rowShift, (int)batch.parts.size());
            out += line;
            out += "    parts=" + joinInts(batch.parts) + "\n";
            out += "    indices=" + joinInts(batch.indices) + "\n";
            for (size_t p = 0; p < batch.partCorners.size(); p++) {
                if (batch.partCorners[p] == 0) {
                    continue;
                }
                float su = 0.0f, sv = 0.0f;
                int n = 0;
                char first[64], last[64];
                snprintf(first, sizeof(first), "-");
                snprintf(last, sizeof(last), "-");
                for (size_t c = 0; c < batch.parts.size(); c++) {
                    if (batch.parts[c] != (int32_t)p) {
                        continue;
                    }
                    su += batch.uvs[c].x;
                    sv += batch.uvs[c].y;
                    char text[64];
                    snprintf(text, sizeof(text), "%.4f,%.4f", batch.uvs[c].x, batch.uvs[c].y);
                    if (n == 0) {
                        snprintf(first, sizeof(first), "%s", text);
                    }
                    snprintf(last, sizeof(last), "%s", text);
                    n++;
                }
                snprintf(line, sizeof(line), "    uv p=%d n=%d sum=%.4f,%.4f first=%s last=%s\n",
                         (int)p, n, su, sv, first, last);
                out += line;
            }
        }

        const int32_t masks[] = {0, 1, 3, o.hidden_parts, -1};
        for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
            int32_t hidden = masks[m];
            int32_t count = arrangeObjectLayout(layout, hidden);
            std::vector<int32_t> order, visible;
            for (int32_t i = 0; i < count; i++) {
                order.push_back(layout->order[i]);
                visible.push_back(objectBatchCorners(layout->batches[layout->order[i]], hidden));
            }
            snprintf(line, sizeof(line), "  A hidden=%d count=%d order=%s visible=%s\n",
                     (int)hidden, (int)count, joinInts(order).c_str(), joinInts(visible).c_str());
            out += line;
        }
    }
    return out;
}

// The rolling basis, driven directly (the C#'s ObjectMotion.Roll through the
// reflection the probe used). No level the tests tick has a rolling object in a
// live state, so the motion is synthesized here: the basis after each move, and
// the transform that draws with it -- which is the rolling branch of
// MdkObject::transform(), the one that reads the basis instead of the yaw.
//
// The basis is printed as raw bits (the transform golden's %.6f would not see a
// last-bit difference) and the transform at %.6f, because the transform's
// translation is the object's position and its rotation is the basis scaled.
// The draw stage's digest of one batch's corners: FNV-1a over the raw bits of
// the five floats of each vertex, in stream order. The sums and the end vertices
// below say what a batch is; this says it is EXACTLY the oracle's, to the bit,
// including the order the corners were written in (a rolling hash is not
// order-independent the way a plain XOR would be).
uint32_t hashCorners(const std::vector<Vertex>& vertices, int32_t first, int32_t corners) {
    uint32_t hash = 2166136261u;
    for (int32_t i = 0; i < corners; i++) {
        const Vertex& v = vertices[(size_t)(first + i)];
        const float values[5] = {v.pos.x, v.pos.y, v.pos.z, v.uv.x, v.uv.y};
        for (int k = 0; k < 5; k++) {
            uint32_t bits = 0;
            memcpy(&bits, &values[k], sizeof(bits));
            hash ^= bits;
            hash *= 16777619u;
        }
    }
    return hash;
}

// The identity of a palette: an FNV-1a digest of its 1024 RGBA bytes. That is
// what a backend needs from a palette -- the same index texture through another
// palette is another picture -- so the digest is what the draw golden pins next
// to the texture's name. The C# twin is ObjectView.PaletteDigest.
uint32_t paletteDigest(const formats::Palette* palette) {
    if (palette == nullptr) {
        return 0;
    }
    uint32_t hash = 2166136261u;
    const uint8_t* rgba = palette->rgba();
    for (int32_t i = 0; i < formats::Palette::kSize * 4; i++) {
        hash ^= rgba[i];
        hash *= 16777619u;
    }
    return hash;
}

// A transform as the golden prints it: the sixteen elements at %.6f for a reader,
// then the same sixteen as raw bits, because %.6f would not see a last-bit
// difference and the transform is what places the corners.
std::string matrixLine(const char* label, const Mat4& m) {
    uint32_t b[16];
    for (int i = 0; i < 16; i++) {
        memcpy(&b[i], &m.m[i], sizeof(b[i]));
    }
    char buf[640];
    snprintf(buf, sizeof(buf),
             "  %s %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f"
             " b=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
             label, m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5], m.m[6], m.m[7], m.m[8], m.m[9],
             m.m[10], m.m[11], m.m[12], m.m[13], m.m[14], m.m[15],
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
    return std::string(buf);
}

// The frame blend: the same object table as the transform golden, but every
// object drawn at the alphas a frame BETWEEN two ticks would use. The scripts
// tick at 1/30 s and a 60 Hz screen draws two frames a tick, so the frame that
// is not the tick's own draws the object between the pose the tick started at and
// the pose it ended at -- MdkObject::remember() plus position_at/yaw_at/
// transform_at, and the alpha the frontend gets from ScriptRuntime::tick_blend.
//
// alpha=1.0000 is the tick's own pose, so the last T line of every object here is
// the transform golden's line; the four before it are the interpolation, which is
// what this pins. The T lines carry the raw bits (matrixLine), and the
// interpolated place is in the transform's translation row, so the bits pin the
// lerp to the last bit; the A line is %.6f for a reader.
std::string dumpObjectBlend(int level, int ticks, scripts::ScriptRuntime& runtime) {
    static const float kAlphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const int alphaCount = (int)(sizeof(kAlphas) / sizeof(kAlphas[0]));
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "BLEND level=%d ticks=%d seed=1 objects=%d alphas=%d\n", level,
             ticks, runtime.objectCount(), alphaCount);
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        out += objectIdentity(index, o);
        for (int a = 0; a < alphaCount; a++) {
            const float alpha = kAlphas[a];
            const Vec3 at = o.position_at(alpha);
            snprintf(line, sizeof(line), "  A alpha=%.4f pos=%.6f,%.6f,%.6f yaw=%.6f\n", alpha,
                     at.x, at.y, at.z, o.yaw_at(alpha));
            out += line;
            out += matrixLine("T", o.transform_at(alpha));
        }
    }
    return out;
}

// The frame blend, driven directly (the C#'s MdkObject.Remember + PositionAt/
// YawAt/TransformAt, fed the same numbers on both sides).
//
// WHY SYNTHESIZED. No shipped level's tick gives a clean in-game case: at tick 1
// the objects are spawned DURING the tick, so nothing has a previous pose to blend
// from, and by tick 2 the oracle's ObjectMotion.Bank has written a roll the port
// does not have (OPEN_ITEMS.md) -- the same reason the transform golden stops at
// t1. Measured at L3 t2: every interpolated place and yaw line matches bit for bit,
// and the transforms differ only in rows 2 and 3 (the roll-driven ones) for the
// seven objects Bank has turned. So the interpolation is pinned the way the rolling
// basis is -- a table of poses blended at the frame's alphas, printed by the oracle
// probe and by this program from the same numbers -- and the table reaches the
// branches the corpus cannot: a teleport (drawn where it ends), an arena change, an
// object with no snapshot yet, and a rolling object (whose basis is NOT blended).
std::string dumpBlendCases() {
    struct BlendCase {
        const char* label;
        Vec3 from;
        float yaw, pitch, roll;
        Vec3 to;
        float nyaw, npitch, nroll;
        const char* arena_from;
        const char* arena_to;
        float scale;
        float lift;
        int rolling;
        int fresh;  // 1: no snapshot at all (spawned during the tick)
    };
    // The numbers are the game's, not invented ones: `gravity` is level 3's SW_BONES
    // falling 0.0356 in a tick, `lift` is level 6's lift at 25 a tick, `bank` is the
    // XF's lean the oracle has by tick 120, `boulder` is level 6's centred XBO
    // (radius 5.15, lifted 5), and the yaws are the oracle's own test values for the
    // short way round (350 -> 10, 170 -> 190, 0 -> 270).
    static const BlendCase kCases[] = {
        {"gravity", {-64, 349, 350}, 0, 0, 0, {-64, 349, 349.9644f}, 0, 0, 0,
         "HMO_1", "HMO_1", 1.0f, 0.0f, 0, 0},
        {"lift", {0, 0, 100}, 0, 0, 0, {0, 0, 125}, 0, 0, 0,
         "OLYM_8", "OLYM_8", 1.0f, 0.0f, 0, 0},
        {"teleport", {0, 0, 100}, 0, 0, 0, {500, 0, 100}, 0, 0, 0,
         "HMO_1", "HMO_1", 1.0f, 0.0f, 0, 0},
        {"short_way", {100, 200, 300}, 358, 0, 0, {100, 200, 300}, 2, 0, 0,
         "HMO_1", "HMO_1", 1.0f, 0.0f, 0, 0},
        {"turn_far", {100, 200, 300}, 0, 0, 0, {100, 200, 300}, 270, 0, 0,
         "HMO_1", "HMO_1", 1.0f, 0.0f, 0, 0},
        {"bank", {-8.7548f, 702.9811f, 303.9043f}, 190.7759f, 0, 0,
         {-12.9303f, 702.2599f, 302.053f}, 189.498f, 0, -6.3411f,
         "HMO_4", "HMO_4", 1.0f, 0.0f, 0, 0},
        {"arena_change", {0, 0, 0}, 0, 0, 0, {10, 10, 10}, 45, 0, 0,
         "HMO_1", "HMO_2", 1.0f, 0.0f, 0, 0},
        {"boulder", {200, 300, 40}, 37, 0, 0, {204, 300, 40}, 37, 0, 0,
         "OLYM_1", "OLYM_1", 1.0f, 5.0f, 1, 0},
        {"scaled", {-59, 314, 104}, 0, 0, 0, {-59, 314, 105.45f}, 12, 0, 0,
         "HMO_1", "HMO_1", 2.0f, 0.0f, 0, 0},
        {"fresh", {13, 460, 149}, 0, 0, 0, {13, 460, 149}, 358, 0, 0,
         "HMO_1", "HMO_1", 1.0f, 0.0f, 0, 1},
    };
    static const float kAlphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const int alphaCount = (int)(sizeof(kAlphas) / sizeof(kAlphas[0]));

    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "BLEND cases=%d alphas=%d\n",
             (int)(sizeof(kCases) / sizeof(kCases[0])), alphaCount);
    out += line;
    for (size_t c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
        const BlendCase& k = kCases[c];
        scripts::MdkObject o;
        o.position = k.from;
        o.yaw = k.yaw;
        o.pitch = k.pitch;
        o.roll = k.roll;
        o.scale = k.scale;
        o.height_offset = k.lift;
        o.roll_radius = k.rolling != 0 ? 5.15f : 0.0f;
        o.arena = k.arena_from;
        if (k.rolling != 0) {
            o.set_rolling(true);  // captures the basis at the first pose, as the C# does
        }
        if (k.fresh == 0) {
            o.remember();
        }
        o.position = k.to;
        o.yaw = k.nyaw;
        o.pitch = k.npitch;
        o.roll = k.nroll;
        o.arena = k.arena_to;

        snprintf(line, sizeof(line),
                 "CASE %d label=%s scale=%.4f lift=%.4f rolling=%d fresh=%d arena=%s>%s\n", (int)c,
                 k.label, k.scale, k.lift, k.rolling, k.fresh, k.arena_from, k.arena_to);
        out += line;
        for (int a = 0; a < alphaCount; a++) {
            const float alpha = kAlphas[a];
            const Vec3 at = o.position_at(alpha);
            snprintf(line, sizeof(line), "  A alpha=%.4f pos=%.6f,%.6f,%.6f yaw=%.6f\n", alpha,
                     at.x, at.y, at.z, o.yaw_at(alpha));
            out += line;
            out += matrixLine("T", o.transform_at(alpha));
        }
    }
    return out;
}

// The per-object queue account (see dumpObjectDraw): the digest covers exactly the
// corners written by this object, which is the range the stream grew by.
std::string queueLine(int index, int32_t count, int32_t queued, const ObjectStream& stream,
                      size_t start) {
    uint32_t hash = 2166136261u;
    for (size_t i = start; i < stream.vertices.size(); i++) {
        const Vertex& v = stream.vertices[i];
        const float values[5] = {v.pos.x, v.pos.y, v.pos.z, v.uv.x, v.uv.y};
        for (int k = 0; k < 5; k++) {
            uint32_t bits = 0;
            memcpy(&bits, &values[k], sizeof(bits));
            hash ^= bits;
            hash *= 16777619u;
        }
    }
    char line[160];
    snprintf(line, sizeof(line), "  Q obj=%d count=%d queued=%d stream=%d hash=%08X\n", index,
             (int)count, (int)queued, (int)stream.vertices.size(), (unsigned)hash);
    return std::string(line);
}

// The object pass's DRAW stage (ObjectView.Draw): the batches arrange orders,
// the posed corners each writes, and where in the frame's stream it is drawn
// from. This is the golden that pins any of it: the layout golden stops at the
// corner table, and the pose golden stops at the posed vertices -- what neither
// can see is which corner goes where, in what order, with what surface and what
// animated-texture frame, and which transform the draw carries.
//
// The objects are walked exactly as Viewer.DrawObjects walks them (skip the
// invisible ones; the dead, the model-less and the layout-less are refused inside
// the draw stage itself), and the stream is the one the whole frame shares, so
// the `first` offsets are the oracle's stream offsets.
std::string dumpObjectDraw(int level, int ticks, LevelData& levelData,
                           scripts::ScriptRuntime& runtime) {
    MaterialResolver resolver(&levelData.dti());
    ObjectView view(resolver);
    ObjectStream stream;
    stream.reset();   // the frame's start, as the GPU half will do

    std::string out;
    char line[640];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;

    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;
            continue;
        }
        if (!o.visible) {
            continue;
        }

        ObjectLook look = lookOf(levelData, o.arena);
        const Mat4 transform = o.transform();
        const size_t before = stream.draws.size();
        const size_t start = stream.vertices.size();
        if (!view.addObject(o, look, transform, &stream)) {
            continue;
        }

        ObjectLayout* layout = view.layoutOf(o.model, look);
        const int32_t count = arrangeObjectLayout(layout, o.hidden_parts);
        snprintf(line, sizeof(line), "DRAW obj=%d model=%s arena=%s hidden=%d shines=%d count=%d\n",
                 index, o.model->name.c_str(), o.arena.c_str(), (int)o.hidden_parts,
                 o.effect_frames > 0 ? 1 : 0, (int)count);
        out += line;

        for (size_t d = before; d < stream.draws.size(); d++) {
            const ObjectStreamDraw& draw = stream.draws[d];
            const Surface& s = draw.surface;
            const int32_t first = draw.first;
            const int32_t corners = draw.corners;
            const Vertex& head = stream.vertices[(size_t)first];
            const Vertex& tail = stream.vertices[(size_t)(first + corners - 1)];
            float sx = 0.0f, sy = 0.0f, sz = 0.0f, su = 0.0f, sv = 0.0f;
            for (int32_t i = 0; i < corners; i++) {
                const Vertex& v = stream.vertices[(size_t)(first + i)];
                sx += v.pos.x;
                sy += v.pos.y;
                sz += v.pos.z;
                su += v.uv.x;
                sv += v.uv.y;
            }
            snprintf(line, sizeof(line),
                     "  B%d first=%d corners=%d frame=%d pal=%08X tex=%s colour=%.4f,%.4f,%.4f,%.4f "
                     "frames=%d pass=%s shading=%s rowshift=%.4f sum=%.4f,%.4f,%.4f uvsum=%.4f,%.4f "
                     "first=%.4f,%.4f,%.4f,%.4f,%.4f last=%.4f,%.4f,%.4f,%.4f,%.4f hash=%08X\n",
                     (int)(d - before), (int)first, (int)corners, (int)draw.frame,
                     (unsigned)paletteDigest(draw.palette),
                     s.texture ? s.texture->name().c_str() : "flat", s.colour.r, s.colour.g,
                     s.colour.b, s.colour.a, (int)s.frameCount, passName(s.pass), shadingName(s.shading),
                     s.rowShift, sx, sy, sz, su, sv, head.pos.x, head.pos.y, head.pos.z, head.uv.x,
                     head.uv.y, tail.pos.x, tail.pos.y, tail.pos.z, tail.uv.x, tail.uv.y,
                     hashCorners(stream.vertices, first, corners));
            out += line;
            // The transform the DRAW CARRIES, not the one the dump recomputed: a
            // queued draw that lost its transform (or got the identity) fails here.
            out += matrixLine("W", draw.world);
        }

        // The queue's own account of the object: how many batches were arranged,
        // how many reached the queue, where the stream ended, and a digest of the
        // corners this object wrote. `queued < count` is the oracle's refusal of a
        // batch that did not fit (see dumpStreamBound).
        out += queueLine(index, count, (int32_t)(stream.draws.size() - before), stream, start);
    }

    snprintf(line, sizeof(line), "STREAM vertices=%d\n", (int)stream.vertices.size());
    out += line;
    return out;
}

// The frame stream's capacity rule (the C#'s Renderer.StreamVertices): a batch
// that does not fit in what the frame has left is refused and the cursor does not
// move, so a later, smaller batch still fits. No shipped frame comes near the
// bound -- level 3 tick 1 streams 2559 corners of the 262144 the C# allows -- so
// the stream is prefilled to a chosen size and the first drawable objects are
// drawn into what is left. That is the only way to reach the rule, and the Q line
// is the queue's own account of it: nothing here recomputes what was refused.
std::string dumpStreamBound(int level, int ticks, LevelData& levelData,
                            scripts::ScriptRuntime& runtime) {
    MaterialResolver resolver(&levelData.dti());
    ObjectView view(resolver);
    ObjectStream stream;

    // The first three drawable objects, in the draw stage's order. At level 3 tick 1
    // they stream 150, 60 and 90 corners, which is what the prefills below are
    // chosen against: each one refuses a different set.
    std::vector<std::pair<int, const scripts::MdkObject*> > picks;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;
            continue;
        }
        if (!o.visible) {
            continue;
        }
        if (picks.size() < 3) {
            picks.push_back(std::make_pair(index, &o));
        }
    }

    const int32_t capacity = kObjectStreamCapacity;
    const int32_t prefills[] = {0,           capacity - 300, capacity - 299,
                                capacity - 150, capacity - 149, capacity - 90,
                                capacity - 60, capacity};

    std::string out;
    char line[256];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d picks=%d\n", level, ticks,
             runtime.objectCount(), (int)picks.size());
    out += line;

    const Vertex filler = {Vec3{1.0f, 2.0f, 3.0f}, Vec2{0.25f, 0.5f}, kVertexWhite};
    for (size_t p = 0; p < sizeof(prefills) / sizeof(prefills[0]); p++) {
        const int32_t prefill = prefills[p];
        snprintf(line, sizeof(line), "BOUND prefill=%d\n", (int)prefill);
        out += line;

        stream.reset();
        stream.vertices.assign((size_t)prefill, filler);
        for (size_t i = 0; i < picks.size(); i++) {
            const scripts::MdkObject& o = *picks[i].second;
            ObjectLook look = lookOf(levelData, o.arena);
            const size_t before = stream.draws.size();
            const size_t start = stream.vertices.size();
            if (!view.addObject(o, look, o.transform(), &stream)) {
                continue;
            }
            ObjectLayout* layout = view.layoutOf(o.model, look);
            const int32_t count = arrangeObjectLayout(layout, o.hidden_parts);
            out += queueLine(picks[i].first, count, (int32_t)(stream.draws.size() - before),
                             stream, start);
        }

        snprintf(line, sizeof(line), "STREAM vertices=%d\n", (int)stream.vertices.size());
        out += line;
    }
    return out;
}

std::string dumpRolling() {
    struct Step {
        float x, y, z;
    };
    struct Case {
        float yaw, pitch, roll, scale, radius, lift;
        int steps;
        Step move[7];
    };
    const Case cases[] = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 7,
         {{0.0f, 0.0f, 0.0f}, {1.5f, -0.25f, 0.0f}, {0.0f, 4.0f, 0.0f}, {-2.75f, 1.25f, 3.0f},
          {0.0f, 0.0f, 3.0f}, {12.0f, 9.0f, 3.0f}, {-3.0f, -3.0f, 3.0f}}},
        {37.0f, 11.0f, -23.0f, 2.0f, 5.15f, 5.0f, 5,
         {{0.0f, 0.0f, 0.0f}, {1.5f, -0.25f, 0.0f}, {12.0f, 9.0f, 0.0f}, {-3.0f, -3.0f, 0.0f},
          {0.5f, 0.25f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}},
        {200.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f, 6,
         {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {-1.0f, -1.0f, 0.0f}, {0.25f, 0.0f, 0.0f},
          {0.0f, 0.0f, 0.0f}, {7.0f, -7.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}},
    };

    std::string out;
    char line[512];
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const Case& k = cases[c];
        scripts::MdkObject obj;
        obj.yaw = k.yaw;
        obj.pitch = k.pitch;
        obj.roll = k.roll;
        obj.scale = k.scale;
        obj.roll_radius = k.radius;
        obj.height_offset = k.lift;
        obj.set_rolling(true);
        snprintf(line, sizeof(line),
                 "ROLL case=%d yaw=%.4f pitch=%.4f roll=%.4f scale=%.4f radius=%.4f lift=%.4f\n", (int)c,
                 k.yaw, k.pitch, k.roll, k.scale, k.radius, k.lift);
        out += line;
        for (int s = 0; s < k.steps; s++) {
            obj.position.x = k.move[s].x;
            obj.position.y = k.move[s].y;
            obj.position.z = k.move[s].z;
            obj.update_rolling();
            snprintf(line, sizeof(line), " R%d pos=%.4f,%.4f,%.4f\n", s, k.move[s].x, k.move[s].y,
                     k.move[s].z);
            out += line;

            const Mat4 b = obj.rolling_basis;
            out += "  B";
            for (int i = 0; i < 16; i++) {
                uint32_t bits;
                memcpy(&bits, &b.m[i], sizeof(bits));
                snprintf(line, sizeof(line), " %08X", bits);
                out += line;
            }
            out += "\n";

            const Mat4 t = obj.transform();
            snprintf(line, sizeof(line),
                     "  T %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                     "%.6f %.6f\n",
                     t.m[0], t.m[1], t.m[2], t.m[3], t.m[4], t.m[5], t.m[6], t.m[7], t.m[8], t.m[9],
                     t.m[10], t.m[11], t.m[12], t.m[13], t.m[14], t.m[15]);
            out += line;
            obj.previous_position = obj.position;
        }
    }
    return out;
}

// The script register file + per-object VM state, in the oracle's "script" dump format.
std::string dumpScript(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    // Runtime target-tracking state (the C# resets it to Kurt each tick and SelectTarget
    // may override). alien_target is the alien-target object's index in this dump's own
    // enumeration (removed objects skipped), or -1 when none — the same index the OBJ
    // lines use, so the two views agree.
    int alienIndex = -1;
    {
        int ai = 0;
        for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
             it != runtime.objects().end(); ++it) {
            if (runtime.isRemoved(&(*it))) {
                continue;
            }
            if (&(*it) == runtime.alienTarget()) {
                alienIndex = ai;
                break;
            }
            ai++;
        }
    }
    const Vec3& tp = runtime.targetPosition();
    snprintf(line, sizeof(line),
             "SCRIPT level=%d ticks=%d seed=1 currentArena=%s objects=%d target_pos=%.4f,%.4f,%.4f "
             "target_yaw=%.4f alien_target=%d\n",
             level, ticks, runtime.currentArena().c_str(), runtime.objectCount(), tp.x, tp.y, tp.z,
             runtime.targetYaw(), alienIndex);
    out += line;
    const std::array<float, 4>& g = runtime.globalVariables;
    snprintf(line, sizeof(line), "GVAR %.4f %.4f %.4f %.4f\n", g[0], g[1], g[2], g[3]);
    out += line;
    // Kurt's combat/inventory state (game/kurt.h): hurt_kurt (109) writes
    // health/flash/knock, if_inventory (175) reads the slots.
    const Kurt& k = runtime.kurt();
    snprintf(line, sizeof(line),
             "KURT health=%d hurt_flash=%.4f invulnerable=%.4f knock=%.4f state=%d slots=%d\n",
             k.health(), k.hurtFlash(), k.invulnerable(), k.knockDamage(), (int)k.state(),
             (int)k.slotCount());
    out += line;
    // The mixer's live voices: the ids the scripts were handed, in play order.
    // This is what if_own_sound (249) and if_sound_playing (233) ask about.
    const audio::SoundMixer& mixer = runtime.mixer();
    snprintf(line, sizeof(line), "AUDIO voices=%d", (int)mixer.voices().size());
    out += line;
    out += " list=";
    for (size_t v = 0; v < mixer.voices().size(); v++) {
        if (v) {
            out += ",";
        }
        snprintf(line, sizeof(line), "%d:", (int)mixer.voices()[v].id);
        out += line;
        out += mixer.voices()[v].name;
    }
    out += "\n";
    // The debris pieces in flight: shattered groups and sparks (Debris.Pieces).
    {
        const scripts::Debris& debris = runtime.debris();
        const std::vector<scripts::Debris::Piece*>& pieces = debris.pieces();
        snprintf(line, sizeof(line), "DEBRIS pieces=%d list=", (int)pieces.size());
        out += line;
        for (size_t pi = 0; pi < pieces.size(); pi++) {
            const scripts::Debris::Piece* p = pieces[pi];
            if (pi) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%s@%.4f,%.4f,%.4fv%.4f,%.4f,%.4ft%d", p->arena.c_str(),
                     p->center.x, p->center.y, p->center.z, p->velocity.x, p->velocity.y,
                     p->velocity.z, (int)p->ticks);
            out += line;
        }
        out += "\n";
    }
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "OBJ %03d type=%s sflags=0x%08X vars=%.4f,%.4f,%.4f,%.4f restart=%d wait=%.4f "
                 "wresume=%d death=%d cmdtgt=%d gret=%s gres=%s timers=%.4f,%.4f,%.4f,%.4f,%.4f "
                 "tsnd=%s tvoice=%d tplay=%d lsnd=%s lvoice=%d lplay=%d\n",
                 index, o.type_name.c_str(), (unsigned)o.script_flags, o.variables[0], o.variables[1],
                 o.variables[2], o.variables[3], o.restart, o.wait_time, o.wait_resume, o.death_script,
                 o.command_target, joinInts(o.gosub_returns).c_str(), joinInts(o.gosub_restarts).c_str(),
                 o.level_timers[0], o.level_timers[1], o.level_timers[2], o.level_timers[3],
                 o.level_timers[4], o.tracked_sound.c_str(), (int)o.tracked_voice,
                 mixer.isVoicePlaying(o.tracked_voice) ? 1 : 0, o.loop_sound_name.c_str(),
                 (int)o.loop_sound, mixer.isVoicePlaying(o.loop_sound) ? 1 : 0);
        out += line;
    }
    return out;
}

// The DTI records in the oracle's "dti" dump format. The floats are printed as
// raw IEEE bits (not %.4f) so a one-ULP parser difference is caught, not rounded
// away. This locks the record layout -- including the boxEnd/name rewind quirk --
// against the C# Dti.ParseRecords.
std::string floatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    char buf[12];
    snprintf(buf, sizeof(buf), "%08X", u);
    return std::string(buf);
}

std::string dumpDti(int level, const Dti& dti) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "DTI level=%d arenas=%d\n", level, (int)dti.arenas().size());
    out += line;
    for (size_t i = 0; i < dti.arenas().size(); i++) {
        const Dti::ArenaEntry& a = dti.arenas()[i];
        snprintf(line, sizeof(line), "ARENA name=%s pitch=%s records=%d\n", a.name.c_str(),
                 floatBits(a.pitch).c_str(), (int)a.records.size());
        out += line;
        for (size_t j = 0; j < a.records.size(); j++) {
            const Dti::Record& r = a.records[j];
            snprintf(line, sizeof(line),
                     "REC type=%u id=%d angle=%s pos=%s,%s,%s box=%s,%s,%s name=%s\n",
                     r.type, r.id, floatBits(r.angle).c_str(),
                     floatBits(r.position[0]).c_str(), floatBits(r.position[1]).c_str(),
                     floatBits(r.position[2]).c_str(), floatBits(r.boxEnd[0]).c_str(),
                     floatBits(r.boxEnd[1]).c_str(), floatBits(r.boxEnd[2]).c_str(), r.name);
            out += line;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    // The shared camera/geometry trig path must be initialized before any
    // runtime can call fastAtan2 (opcode 203's camera-track calculation).
    mdk::mathLutInit();

    // The formats library reaches the filesystem through this seam.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    MdkData data = locateData();
    if (!data.found()) {
        printf("SKIP: no MDK data (set MDK_DATA_DIR)\n");
        return 0;
    }

    // The sprites archive, which is the C#'s `sprites` and is a constructor argument
    // of its ScriptRuntime for a reason: the built-in behaviours play their
    // animations out of it (SW_H150's H150_R and H150_I, ObjectBehaviors.UpdateRunner).
    // A runtime without it is a world where those animations do not exist, which is
    // not the oracle's world, so every runtime below is given it.
    formats::Bni sprites;
    const formats::Bni* spritesArg = nullptr;
    {
        std::vector<uint8_t> spriteBytes;
        if (data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) &&
            formats::Bni::parse(&sprites, spriteBytes.data(), (int32_t)spriteBytes.size())) {
            spritesArg = &sprites;
        } else {
            // FAIL, and fail loudly. A runtime without the archive is not the
            // oracle's world: the Runner's idle chance is drawn only while the
            // object is idle or its animation is done, so with no archive the
            // random stream is consumed differently and EVERY golden that touches
            // a scripted random decision would silently be a different game's
            // output. The C# cannot run without it either — Viewer loads it with
            // Bni.Load, which throws — so refusing here is the oracle's behaviour,
            // not a convenience.
            printf("FAIL: TRAVERSE/TRAVSPRT.BNI did not load; the built-in behaviours that play "
                   "their own animations cannot run, and the random stream would not be the "
                   "oracle's\n");
            return 1;
        }
    }

    // Golden dumps (`tick1 dump <pose|transform|blend|layout|draw|bound|rolling> [level] [ticks]`):
    // print one dump to stdout, which is how a golden is written and how a failing
    // golden is diffed against the C# probe (the oracle twin of each of these
    // dumps; see docs/PROGRESS_LOG.md for the probe's recipe).
    if (argc > 1 && std::string(argv[1]) == "dump") {
        std::string what = argc > 2 ? argv[2] : "rolling";
        int level = argc > 3 ? atoi(argv[3]) : 3;
        int ticks = argc > 4 ? atoi(argv[4]) : 1;
        if (what == "rolling") {
            printf("%s", dumpRolling().c_str());
            return 0;
        }
        if (what == "blendcases") {
            printf("%s", dumpBlendCases().c_str());
            return 0;
        }
        if (what == "kurt" || what == "kurtlethal") {
            std::string got;
            std::string why;
            if (!kurtStepsDump(level, ticks, what == "kurtlethal", data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "floor") {
            std::string got;
            std::string why;
            if (!floorProbeDump(level, data, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "hurt" || what == "hurtland" || what == "knock" || what == "invuln" ||
            what == "knockdown") {
            std::string got;
            std::string why;
            const int schedule = what == "hurtland"                                     ? kHurtHardLand
                                 : (what == "knock"                                     ? kHurtKnock
                                 : (what == "invuln"                                    ? kHurtInvuln
                                 : (what == "knockdown"                                 ? kHurtKnockdown
                                    : kHurtHealth)));
            if (!kurtHurtDump(level, ticks, schedule, data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("the level did not load\n");
            return 1;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        Cmi cmi;
        if (!data.read(cmiPath, &cmiBytes) ||
            !Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("the CMI did not parse\n");
            return 1;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("the sound bank did not load\n");
            return 1;
        }
        bank.preload();
        // The `runner` mode drives the world with Kurt standing where no shipped
        // start puts him (see dumpRunner). The C# probe's twin sets Kurt.Feet to
        // the same point, so both trees feed the runtime the same live Kurt; the
        // other dump modes keep the C#'s no-live-Kurt harness.
        Player kurt;
        // Non-const because the runtime borrows the whole entity now, not a read-only
        // view of it: ScriptRuntime::kurt() is this Player's combat half (the C#
        // runtime's Kurt is the same object Viewer's Kurt is, ScriptRuntime.cs:168).
        // The runner mode is the only arm that binds one; the others pass nullptr and
        // get the runtime's unbound combat state.
        Player* kurtArg = nullptr;
        Vec3 kurtAt = {0.0f, 0.0f, 0.0f};
        if (what == "runner") {
            if (argc <= 5 || !parsePoint(argv[5], &kurtAt)) {
                printf("usage: tick1 dump runner <level> <ticks> <x,y,z>\n");
                return 1;
            }
            kurt.feet = kurtAt;
            kurt.yaw = levelData.dti().startAngle();
            kurtArg = &kurt;
        }
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, kurtArg, spritesArg);
        runtime.option = 0;
        // dumpRunner ticks the world itself, one tick per printed line; ticking it
        // here too would print the state 2*ticks ticks in, not `ticks`.
        if (what != "runner") {
            for (int t = 0; t < ticks; t++) {
                runtime.update(1.0f / 30.0f);
            }
        }
        if (what == "pose") {
            printf("%s", dumpObjectPoses(level, ticks, runtime).c_str());
        } else if (what == "transform") {
            printf("%s", dumpObjectTransforms(level, ticks, runtime).c_str());
        } else if (what == "blend") {
            printf("%s", dumpObjectBlend(level, ticks, runtime).c_str());
        } else if (what == "layout") {
            printf("%s", dumpObjectLayout(level, ticks, levelData, runtime).c_str());
        } else if (what == "draw") {
            printf("%s", dumpObjectDraw(level, ticks, levelData, runtime).c_str());
        } else if (what == "bound") {
            printf("%s", dumpStreamBound(level, ticks, levelData, runtime).c_str());
        } else if (what == "runner") {
            printf("%s", dumpRunner(level, ticks, kurtAt, runtime).c_str());
        } else {
            printf("unknown dump\n");
            return 1;
        }
        return 0;
    }

    // Reachability probe (`tick1 probe [ticks]`): run each level and list the
    // opcodes the scripts reached that the VM does not implement yet, with
    // hit counts. This is how the next opcode slice is chosen (evidence, not
    // memory — see docs/OPCODE_COVERAGE.md).
    if (argc > 1 && std::string(argv[1]) == "probe") {
        int probeTicks = argc > 2 ? atoi(argv[2]) : 10000;
        for (int level = 3; level <= 8; level++) {
            LevelData levelData;
            if (!LevelData::load(&levelData, &data, level)) {
                continue;
            }
            char cmiPath[128];
            snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
            std::vector<uint8_t> cmiBytes;
            if (!data.read(cmiPath, &cmiBytes)) {
                continue;
            }
            Cmi cmi;
            if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                continue;
            }
            audio::SoundBank bank;
            if (!audio::SoundBank::forLevel(&bank, data, level)) {
                continue;
            }
            bank.preload();
            scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
            runtime.option = 0;
            for (int t = 0; t < probeTicks; t++) {
                runtime.update(1.0f / 30.0f);
            }
            printf("PROBE L%d ticks=%d unimplemented:", level, probeTicks);
            const std::vector<std::pair<int32_t, int32_t> >& un = runtime.unimplemented();
            for (size_t i = 0; i < un.size(); i++) {
                printf(" %d(x%d)", (int)un[i].first, (int)un[i].second);
            }
            printf("\n");
        }
        return 0;
    }

    int failures = 0;
    // (level, ticks) cases. Tick 1 covers every shipped level; the multi-tick cases
    // exercise the motion slices that move an object after its spawn tick:
    // path-following, gravity free-fall, the BSP footing sweep, and the
    // face_target / turn_to_yaw turning opcodes. A level/tick is added here only
    // once its whole table matches the oracle byte for byte. The deeper ticks still
    // differ and are deferred to their own slices: the walking/chasing enemies'
    // stuck detection and replanning (DetectStuck/HandleStuck), the combat and
    // death paths, and the transient object effects (projectiles, explosions).
    struct Case {
        int level;
        int ticks;
    };
    const Case cases[] = {
        {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
        {3, 2}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {5, 10}, {6, 2}, {6, 3}, {6, 4}, {6, 5}, {6, 6}, {6, 10}, {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}, {7, 7}, {7, 10},
        {4, 15}, {5, 15}, {6, 15}, {7, 15}, {4, 20}, {5, 20}, {6, 20}, {7, 20},
        {5, 25}, {6, 25}, {7, 25}, {5, 30}, {6, 30}, {7, 30},
        {5, 40}, {6, 40}, {7, 40}, {5, 60}, {6, 60}, {7, 60},
        // The XGEN spawner's aliens: the countdown (add_var_dt) opens the spawn at
        // ~t119, and the spawned XG runs its spawn script, lands (footing), and walks
        // off with set_move_x (command 88) — the tables at t240/t300 keep matching
        // while it moves.
        {5, 120}, {5, 240}, {5, 300},
        // Stuck detection/replan (DetectStuck: 16 ticks walking, 9 with 197;
        // HandleStuck: replan once per stuck flag, slide, give up): L8's walkers
        // wander GUNT for hundreds of ticks — the stuck block is what keeps the
        // long walks deterministic between the walkers.
        {6, 300}, {8, 40}, {8, 60}, {8, 120},
        // With .NET's degree/radian conversions spelled left-associatively (the
        // constant-folded forms are 1 ULP off at many inputs) the yaws are
        // bit-exact, so the previously-excluded cases match: L5_t180 and L3_t3
        // (their 4th-decimal yaw shifts), L8_t2+ (the walk planner's detour picked
        // a different waypoint off the ULP), and every level runs green to t300
        // except where the walk's stuck/replan slice begins to matter (L8_t120's
        // table, L3_t300's XS, and past t300).
        {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 10}, {3, 15}, {3, 30}, {3, 60}, {3, 120},
        // The XS controller fires the BOLT (fire, opcode 61): the projectile
        // spawns at the reference point, flies straight, and dies on its lifetime.
        {3, 300},
        {8, 300},
        {4, 25}, {4, 30}, {4, 40}, {4, 60}, {4, 120}, {4, 300},
        {5, 180},
        {6, 120}, {6, 300},
        {7, 120}, {7, 300},
        {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 6}, {8, 10}, {8, 15}, {8, 20}, {8, 25}, {8, 30},
        {8, 40}, {8, 60},
        // Deeper ticks that stay byte-exact with the oracle: the spawner/walker
        // scripts run past t300 without a new slice (L3 and L8 diverge earlier).
        {4, 360}, {4, 420}, {4, 480}, {4, 540}, {4, 600}, {5, 360}, {5, 420}, {5, 480}, {5, 540}, {6, 360}, {6, 420}, {6, 480}, {6, 540}, {6, 600}, {7, 360}, {7, 420}, {7, 480}, {7, 540}, {7, 600}, {8, 360},
        // L5 past t540: the XGEN spawner's 5th XG opens (~t585) and dies (~t595), and
        // object 004 takes a move_near_target (opcode 43) at ~t577. The old "XG[004]
        // divergence ~t577" was the same degree/radian constant-fold ULP class the
        // left-associative spelling resolves (see the note above), so the whole region
        // now matches the oracle byte for byte (table and script state).
        {5, 577}, {5, 585}, {5, 590}, {5, 595}, {5, 600},
        // Deeper ticks that stay byte-exact with the oracle, generated from it. Each
        // level stops at the first tick where an unported slice diverges (the pickup/
        // chute/death lifecycle and the transient effects), so these are the honest
        // frontier per level: L3 to t420 (objects drop off as pickups/chutes resolve),
        // L5 to t660 (the XGEN region past t600), L6 to t900 (the XD keeps walking —
        // the table genuinely cycles, not a frozen steady state), L7 t660/t900 bracket
        // a steady arena to prove no slow drift. L4 empties out past t600 (0 objects)
        // and L8 diverges at t420 (SW_H01/SW_CHUTE removal), so neither extends.
        {3, 360}, {3, 420},
        {5, 660},
        {6, 660}, {6, 720}, {6, 780}, {6, 840}, {6, 900},
        {7, 660}, {7, 900},
        // The pickup/chute/death lifecycle slice (faithful Kill death-script branch +
        // Remove taking a live Attached with it) moved the frontier from ~t400-900 to
        // the end of the simulated corpus: every level now matches the oracle byte for
        // byte (table AND script) far past the old boundary. L3/L6/L7/L8 match to at
        // least t30000 with live objects (11/5/7/10 — a genuinely cycling steady
        // state, not an empty arena); L4 empties its arena (0 objects, matching the
        // oracle's empty at the same ticks) and matches to t10000. L5's t2303 BOLT
        // spawn was the unported if_wall (opcode 115): the XG's approach script
        // probes 42 units ahead for a wall before committing to the walk, and the
        // missing case fell through to the no-wall branch, skipping the fire that
        // spawned the 16th BOLT. With it ported, L5 matches the oracle byte for
        // byte (table AND script) to t30000 like the other full levels.
        {3, 480}, {3, 600}, {3, 1200}, {3, 2400}, {3, 5000}, {3, 10000}, {3, 30000},
        {4, 660}, {4, 900}, {4, 1200}, {4, 2400}, {4, 5000}, {4, 10000}, {4, 30000},
        {5, 720}, {5, 900}, {5, 1200}, {5, 1800}, {5, 2300}, {5, 2302},
        {5, 2303}, {5, 2350}, {5, 2400}, {5, 3000}, {5, 5000}, {5, 10000}, {5, 30000},
        {6, 960}, {6, 1200}, {6, 2400}, {6, 5000}, {6, 10000}, {6, 30000},
        {7, 960}, {7, 1200}, {7, 2400}, {7, 5000}, {7, 10000}, {7, 30000},
        {8, 420}, {8, 600}, {8, 1200}, {8, 2400}, {8, 5000}, {8, 10000}, {8, 30000},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("FAIL L%d: did not load\n", level);
            failures++;
            continue;
        }

        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            printf("FAIL L%d: CMI did not read\n", level);
            failures++;
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("FAIL L%d: CMI did not parse\n", level);
            failures++;
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d: sound bank did not load\n", level);
            failures++;
            continue;
        }
        bank.preload();

        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
        runtime.option = 0;  // the oracle's Option
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }

        std::string got = dumpTable(level, ticks, runtime);

        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d: no golden file %s\n", level, ticks, goldenPath);
            failures++;
            continue;
        }

        if (got == expected) {
            printf("PASS L%d_t%d: table matches the oracle (%d objects)\n", level, ticks,
                   runtime.objectCount());
        } else {
            printf("FAIL L%d_t%d: table differs\n--- got ---\n%s--- expected ---\n%s", level, ticks,
                   got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Script register file + per-object VM state. The object table can match while a
    // script variable or the VM's per-object state (restart/wait/timers) diverges, so
    // this is a second, independent byte-for-byte check over the same cases.
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            continue;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d_t%d_script: sound bank did not load\n", level, ticks);
            failures++;
            continue;
        }
        bank.preload();
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }
        std::string got = dumpScript(level, ticks, runtime);
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_script.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            // No script golden yet: the script state diverges on a later slice (the
            // door's wait_anim_frame parks restart on an animation frame the advance
            // slice has not landed). Skip, not fail — the object table above still
            // covers this case. A golden is added here once the state matches.
            printf("SKIP L%d_t%d_script: no script golden (later slice)\n", level, ticks);
            continue;
        }
        if (got == expected) {
            printf("PASS L%d_t%d_script: register file + VM state match the oracle\n", level, ticks);
        } else {
            printf("FAIL L%d_t%d_script: script state differs\n--- got ---\n%s--- expected ---\n%s",
                   level, ticks, got.c_str(), expected.c_str());
            failures++;
        }
    }

    // The object pass: pose, place, frame blend, layout and draw, each pinned at
    // the ticks where the port reproduces the oracle byte for byte.
    //
    // These went multi-tick with ObjectMotion.Bank (the automatic lean into a
    // turn, the only writer of roll for a walking alien). While Bank was missing
    // the port never wrote roll from motion, so the oracle's XF had roll=-6.3411
    // at t120 and 9.0251 at t1200 where the port had 0.0000, and every transform
    // that turns by roll differed -- which is why all five goldens below were
    // pinned at t1. With Bank ported, the transform, layout and draw dumps match
    // the oracle byte for byte at t120 and t1200 (and at L4-L8 t120).
    //
    // LEVEL 7 AT t1000 is the case that bites on the BUILT-IN behaviours, not the
    // scripted ones. SW_H150 (the hamster) has played H150_I since tick 329, so its
    // posed corners are animation frame 36 and not the rest pose, and until
    // ObjectBehaviors.UpdateRunner was ported the port had no animation for it at
    // all -- every other line of level 7's dumps matched, so the whole divergence
    // was that one object. It is also the only place the port failed to consume the
    // shared Random when the oracle did: the idle chance is drawn from the same
    // stream every scripted random decision takes, so a missing runner silently
    // moves every later draw in the level.
    //
    // The blend golden is pinned at t1, t2 and t1200 and NOT at t120: at t120 one
    // object's interpolated place is 1 ULP off, because FrameBlend.Lerp is
    // Vector3.Lerp and this port does not reproduce its rounding for every input.
    // That is a separate divergence with its evidence in OPEN_ITEMS.md, and t120
    // stays OUT of the suite rather than being pinned to the port's own output.
    const GoldenCase poseCases[] = {{3, 1}, {3, 120}, {3, 1200}, {7, 1000}};
    const GoldenCase transformCases[] = {{3, 1}, {3, 120}, {3, 1200}, {7, 1000}};
    const GoldenCase blendCases[] = {{3, 1}, {3, 2}, {3, 1200}};
    const GoldenCase layoutCases[] = {{3, 1}, {3, 120}, {3, 1200}, {7, 1000}};
    const GoldenCase drawCases[] = {{3, 1}, {3, 120}, {3, 1200}, {7, 1000}};
    // The loop runs the union of every list's ticks; each golden checks its own.
    const GoldenCase tickCases[] = {{3, 1}, {3, 2}, {3, 120}, {3, 1200}, {7, 1000}};
    const size_t tickCount = sizeof(tickCases) / sizeof(tickCases[0]);
    const size_t poseCount = sizeof(poseCases) / sizeof(poseCases[0]);
    const size_t transformCount = sizeof(transformCases) / sizeof(transformCases[0]);
    const size_t blendCount = sizeof(blendCases) / sizeof(blendCases[0]);
    const size_t layoutCount = sizeof(layoutCases) / sizeof(layoutCases[0]);
    const size_t drawCount = sizeof(drawCases) / sizeof(drawCases[0]);
    for (size_t c = 0; c < tickCount; c++) {
        int level = tickCases[c].level;
        int ticks = tickCases[c].ticks;
        // These are fixed acceptance cases, not optional probes: a level that will
        // not load, a CMI that will not parse or a sound bank that will not open
        // is a regression in the port, so it FAILS here rather than skipping the
        // goldens below (a skipped golden reads like a passing one).
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("FAIL L%d_t%d_object: the level did not load\n", level, ticks);
            failures++;
            continue;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            printf("FAIL L%d_t%d_object: %s did not read\n", level, ticks, cmiPath);
            failures++;
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("FAIL L%d_t%d_object: the CMI did not parse\n", level, ticks);
            failures++;
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d_t%d_object: the sound bank did not load\n", level, ticks);
            failures++;
            continue;
        }
        bank.preload();
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }

        std::string got;
        char goldenPath[160];
        std::string expected;

        // Each golden checks its OWN list, so a tick can be pinned for the blend at
        // t2 without being pinned for the pose at t2 (the lists are not nested).
        if (pinnedIn(poseCases, poseCount, level, ticks)) {
            got = dumpObjectPoses(level, ticks, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_pose.txt", level, ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_pose: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_pose: every object's posed vertices match the oracle\n", level,
                       ticks);
            } else {
                printf("FAIL L%d_t%d_pose: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }

        if (pinnedIn(transformCases, transformCount, level, ticks)) {
            got = dumpObjectTransforms(level, ticks, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_transform.txt", level, ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_transform: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_transform: every object's transform matches the oracle\n",
                       level, ticks);
            } else {
                printf("FAIL L%d_t%d_transform: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }

        // The frame blend. At t1 every object was spawned DURING the tick, so none
        // has a previous pose and every alpha is the raw transform: that case pins
        // the plumbing (the snapshot runs, and a mid-tick spawn has none). At t2 the
        // objects do have a previous pose, so it pins the interpolation over a real
        // tick -- the place, the yaw and the transform, at five alphas, as raw bits.
        if (pinnedIn(blendCases, blendCount, level, ticks)) {
            got = dumpObjectBlend(level, ticks, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_blend.txt", level, ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_blend: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_blend: every object's blended place, yaw and transform match "
                       "the oracle\n", level, ticks);
            } else {
                printf("FAIL L%d_t%d_blend: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }

        // The layout stage (ObjectView.Build/Arrange/Visible), over the same tick.
        if (pinnedIn(layoutCases, layoutCount, level, ticks)) {
            got = dumpObjectLayout(level, ticks, levelData, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_layout.txt", level, ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_layout: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_layout: every model's batches, corners, UVs and order match "
                       "the oracle\n", level, ticks);
            } else {
                printf("FAIL L%d_t%d_layout: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }

        if (pinnedIn(drawCases, drawCount, level, ticks)) {
            // The draw stage (ObjectView.Draw), over the same tick: the frame's
            // stream, the offset and corner count of every batch, and the surface,
            // palette, animated frame and transform each queued draw carries.
            got = dumpObjectDraw(level, ticks, levelData, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_draw.txt", level, ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_draw: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_draw: every object's streamed corners, batch order, surfaces, "
                       "palettes, frames and transforms match the oracle\n", level, ticks);
            } else {
                printf("FAIL L%d_t%d_draw: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }

            // The same draw stage with the frame's stream prefilled to the C#'s
            // capacity minus a chosen amount: the refusal of a batch that does not fit,
            // and the fact that a later smaller batch still fits.
            got = dumpStreamBound(level, ticks, levelData, runtime);
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_stream_bound.txt", level,
                     ticks);
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_stream_bound: no golden file %s\n", level, ticks, goldenPath);
                failures++;
            } else if (got == expected) {
                printf("PASS L%d_t%d_stream_bound: the frame stream's capacity rule matches the "
                       "oracle\n", level, ticks);
            } else {
                printf("FAIL L%d_t%d_stream_bound: %s\n", level, ticks,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The Runner's RUNNING half (ObjectBehaviors.UpdateRunner's H150_R branch).
    // Nothing else can pin it: level 7's Kurt starts 22.16 units from the hamster
    // and the run needs 20, so in every shipped scenario the hamster only ever
    // idles (that half is pinned by L7_t1000_pose). These two cases place Kurt
    // beside it, which starts the run, and the dump covers every tick, so the
    // push, the 270 deg/s turn clamp, the plan-distance stop test, the return to
    // H150_I and the idle RNG after it are all in the golden. Both goldens are the
    // oracle probe's `runner` mode output with Kurt at the same point.
    {
        struct RunnerCase {
            int level;
            int ticks;
            const char* kurt;
            const char* tag;
        };
        const RunnerCase runnerCases[] = {
            {7, 120, "-21,-13,0.73", "a"},
            {7, 120, "-26,-18,0.73", "b"},
        };
        for (size_t c = 0; c < sizeof(runnerCases) / sizeof(runnerCases[0]); c++) {
            const RunnerCase& rc = runnerCases[c];
            char goldenPath[160];
            snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_runner_%s.txt", rc.level,
                     rc.ticks, rc.tag);
            std::string expected;
            if (!readText(goldenPath, &expected)) {
                printf("FAIL L%d_t%d_runner_%s: no golden file %s\n", rc.level, rc.ticks, rc.tag,
                       goldenPath);
                failures++;
                continue;
            }
            LevelData levelData;
            if (!LevelData::load(&levelData, &data, rc.level)) {
                printf("FAIL L%d_t%d_runner_%s: the level did not load\n", rc.level, rc.ticks,
                       rc.tag);
                failures++;
                continue;
            }
            char cmiPath[128];
            snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", rc.level, rc.level);
            std::vector<uint8_t> cmiBytes;
            Cmi cmi;
            if (!data.read(cmiPath, &cmiBytes) ||
                !Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                printf("FAIL L%d_t%d_runner_%s: the CMI did not load\n", rc.level, rc.ticks, rc.tag);
                failures++;
                continue;
            }
            audio::SoundBank bank;
            if (!audio::SoundBank::forLevel(&bank, data, rc.level)) {
                printf("FAIL L%d_t%d_runner_%s: the sound bank did not load\n", rc.level, rc.ticks,
                       rc.tag);
                failures++;
                continue;
            }
            bank.preload();
            Vec3 at;
            if (!parsePoint(rc.kurt, &at)) {
                printf("FAIL L%d_t%d_runner_%s: bad Kurt point\n", rc.level, rc.ticks, rc.tag);
                failures++;
                continue;
            }
            Player player;
            player.feet = at;
            player.yaw = levelData.dti().startAngle();
            scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, spritesArg);
            runtime.option = 0;
            std::string got = dumpRunner(rc.level, rc.ticks, at, runtime);
            if (got == expected) {
                printf("PASS L%d_t%d_runner_%s: the hamster's run from tick to tick matches the "
                       "oracle\n", rc.level, rc.ticks, rc.tag);
            } else {
                printf("FAIL L%d_t%d_runner_%s: %s\n", rc.level, rc.ticks, rc.tag,
                       firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Kurt's own state machine and the sounds it makes (Kurt.Update at Viewer.Step,
    // which is 1/60 s). Nothing else pins any of it: the walk, the run, the jump,
    // the chute, the fall and the landing had no golden before this, and neither did
    // FOOT1-4, LAND, CHUTEOUT, CHUTEON or CHUTEIN, nor the chute closing on a hard
    // landing nor the chute stopping on a corpse. The golden is the oracle probe's
    // `kurt` mode output, byte for byte, over the same scripted input, the same
    // archive frame counts and the same level's sound bank; the voices column is
    // what makes the sounds visible. See dumpKurt for what the schedule covers.
    {
        const int kurtLevel = 3;
        const int kurtSteps = 950;
        std::string expected;
        if (!readText("golden/L3_kurt_steps.txt", &expected)) {
            printf("FAIL L3_kurt_steps: no golden file golden/L3_kurt_steps.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtStepsDump(kurtLevel, kurtSteps, false, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_steps: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_steps: Kurt's walk, run, back-up, jump, chute, landing, "
                       "hard landing and death match the oracle, footsteps and chute sounds "
                       "included\n");
            } else {
                printf("FAIL L3_kurt_steps: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // The lethal hard landing, as its own run (review-9 R9-5). The C#'s HardLanding
    // tests `Health == 0` before it enters K_CRASHL, so a Kurt who is already dead
    // when a -194 u/s landing arrives does not play the crash and does not have his
    // chute closed for him; the 950-step schedule cannot show that, because its
    // hard landing happens at full health and its death happens on a soft landing,
    // and Kurt only dies once. Guarding the state entry on `health > 0` alone --
    // dropping the oracle's `Health == 0` test -- is a mutant only this run sees.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_lethal.txt", &expected)) {
            printf("FAIL L3_kurt_lethal: no golden file golden/L3_kurt_lethal.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtStepsDump(3, 200, true, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_lethal: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_lethal: a hard landing at zero health enters no K_CRASHL "
                       "(the one thing this run can see; the chute close and the floored damage "
                       "are pinned by L3_kurt_steps step 656 and by Player::damage)\n");
            } else {
                printf("FAIL L3_kurt_lethal: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The one health, seen from both sides of the class split: hurt_kurt (opcode 109)
    // through the ScriptRuntime, and the death transition and HUD through the Player.
    // The oracle prints hp= and vmhp= off one property and cannot express a
    // divergence; a port that gave the runtime its own copy of Kurt's health would
    // print vmhp dropping while hp stayed 100 while the entity kept walking. See
    // dumpKurtHurt for why every hurt lands in the air. What this run does NOT pin
    // is a script hurt that reaches zero and kills him: the hurts do take his health
    // to 0 at step 80, but the step that makes him Dead is the health-zero-on-a-floor
    // transition at the landing, which golden/L3_kurt_lethal.txt pins on its own.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_hurt.txt", &expected)) {
            printf("FAIL L3_kurt_hurt: no golden file golden/L3_kurt_hurt.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 220, kHurtHealth, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_hurt: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_hurt: the health hurt_kurt writes is the one the death "
                       "transition and the HUD read (hp == vmhp at every step); the death on the "
                       "landing is the health-zero-on-a-floor step L3_kurt_lethal pins\n");
            } else {
                printf("FAIL L3_kurt_hurt: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The state half of the same guard (review-11 R11-3). Kurt.Hurt declines a hurt
    // while the entity is Dead, Knocked, GetUp or HardLand (Kurt.Combat.cs:125), and
    // this port splits the class, so the guard is only faithful if the state it reads
    // is the state the machine is in. This run hard-lands him at 183 (K_CRASHL to 286),
    // hurts him inside the window -- the oracle declines, hp stays 90 -- and hurts him
    // again after it, where the guard has released and the health moves. Two mutants,
    // one per arm: a Kurt::state_ nothing writes (the pre-fix shape) damages at 220, and
    // a guard that always fires does not damage at 300. See dumpKurtHurt for why the
    // second damage is 4 and not 5.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_hurtland.txt", &expected)) {
            printf("FAIL L3_kurt_hurtland: no golden file golden/L3_kurt_hurtland.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 340, kHurtHardLand, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_hurtland: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_hurtland: hurt_kurt is refused through the whole K_CRASHL "
                       "window and lands again the step the guard releases, so hurt() reads the "
                       "entity's one state and not a copy\n");
            } else {
                printf("FAIL L3_kurt_hurtland: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The recent-damage counter itself (review-12 A). Kurt.UpdateKnockDamage declines
    // it every step -- clamp into [0, KnockdownDamage], then drain 2 per second -- and
    // Kurt.HardLanding zeroes it between the damage and the dead/knocked guard
    // (Kurt.cs:583). Neither was ported, so a counter a script raised with
    // set_hurt_flash (240) or a hurt taken in the air survived a landing the oracle
    // clears, which is the shape that will knock Kurt down on the step after every
    // hard landing once the knock-down machine lands. Three mutants, one per rule: no
    // decline at all fails at step 21 (knock stays 25), a decline without the clamp
    // fails there too (24.9667), and no reset in Player::land fails at step 183
    // (3.7333 where the oracle is 0). See dumpKurtHurt for why no hurt is 5 or more
    // once he is near the floor.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_knock.txt", &expected)) {
            printf("FAIL L3_kurt_knock: no golden file golden/L3_kurt_knock.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 340, kHurtKnock, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_knock: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_knock: the knock counter clamps to 5 and drains by 2 per "
                       "second, and the hard landing zeroes it, so a counter raised in the air "
                       "cannot survive a landing the oracle clears\n");
            } else {
                printf("FAIL L3_kurt_knock: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The invulnerability timer (Kurt.Combat.cs:82, 0x573bd4) and the decline this port
    // did not run at all: Kurt.cs:139 takes delta off it every step, floored at zero,
    // ahead of Update's early returns. Opcode 244 was its only writer here, so that
    // opcode was a permanent switch: hurt() would refuse forever afterwards and opcode 240
    // could never raise the knock-down counter again. Three mutants, one per rule: no
    // decline at all fails at step 210 (hp stays 100 where the oracle is 96), a decline
    // without the floor at zero fails where the timer crosses past it, and a decline
    // placed below Player::update's `if (dead) return` freezes at 2.0000 from step 306.
    // See dumpKurtHurt for why every hurt is under 5 and nothing is lifted.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_invuln.txt", &expected)) {
            printf("FAIL L3_kurt_invuln: no golden file golden/L3_kurt_invuln.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 340, kHurtInvuln, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_invuln: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_invuln: the invulnerability timer runs down by delta every "
                       "step and floors at zero, so opcode 244 is a timer and not a switch, and "
                       "it runs down on a corpse as well\n");
            } else {
                printf("FAIL L3_kurt_invuln: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The knock-down itself (Kurt.Combat.cs:173 and 214, Kurt.Combat.cs:379-399): the
    // counter's trigger, K_BANG, the flip to K_BFLIP, and the way back to Still. This is
    // the state machine every earlier hurt golden stepped around by keeping its damage
    // under 5, and it is what makes the Knocked and GetUp arms of hurt()'s guard -- the
    // half of that guard batch 41 left unreachable -- testable at all. It is also the
    // first golden to reach ArenaSpace::floor, the probe an airborne knock-down makes 13
    // units below Kurt's feet.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_knockdown.txt", &expected)) {
            printf("FAIL L3_kurt_knockdown: no golden file golden/L3_kurt_knockdown.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 340, kHurtKnockdown, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_knockdown: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_knockdown: a hurt of KnockdownDamage knocks Kurt down on a "
                       "floor and in the air above one, K_BANG flips to K_BFLIP and back to "
                       "Still, the walk input is dead while he is down, and hurt() refuses him "
                       "for the state and not only for the timer it opens\n");
            } else {
                printf("FAIL L3_kurt_knockdown: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The two floor probes over every reachable arena (ArenaSpace.Floor vs its
    // FloorCrossing), driven directly: the query the knock-down's air arm makes, pinned on
    // geometry instead of through one step of one schedule. See floorProbeDump.
    {
        std::string expected;
        if (!readText("golden/L3_floor_probes.txt", &expected)) {
            printf("FAIL L3_floor_probes: no golden file golden/L3_floor_probes.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!floorProbeDump(3, data, &got, &why)) {
                printf("FAIL L3_floor_probes: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_floor_probes: the nearest-crossing probe and the first-crossing "
                       "probe answer the same on every arena the level reaches, or the golden "
                       "says exactly where they part\n");
            } else {
                printf("FAIL L3_floor_probes: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The rolling basis (ObjectMotion.Roll), driven directly: see dumpRolling.
    {
        std::string got = dumpRolling();
        std::string expected;
        if (!readText("golden/rolling_basis.txt", &expected)) {
            printf("FAIL rolling_basis: no golden file golden/rolling_basis.txt\n");
            failures++;
        } else if (got == expected) {
            printf("PASS rolling_basis: the rolling basis and the transform that draws with it "
                   "match the oracle\n");
        } else {
            printf("FAIL rolling_basis: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // The frame blend (MdkObject::remember + position_at/yaw_at/transform_at),
    // driven directly over the same poses on both sides: see dumpBlendCases.
    {
        std::string got = dumpBlendCases();
        std::string expected;
        if (!readText("golden/frame_blend.txt", &expected)) {
            printf("FAIL frame_blend: no golden file golden/frame_blend.txt\n");
            failures++;
        } else if (got == expected) {
            printf("PASS frame_blend: the place, yaw and transform between two ticks match the "
                   "oracle, including the teleport, arena and no-snapshot branches\n");
        } else {
            printf("FAIL frame_blend: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // Footing (the BSP sweep that stops a gravity object on a floor). The L3_t3
    // table is now a whole-table golden; the footing probes below pin the clean
    // landings directly: each XG rests on its arena floor exactly as the oracle,
    // with velocity.Z zeroed so it stays put on later ticks.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 3)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The two XG spawn at z=127.0 and fall; the oracle lands them at
                    // 126.9500 and 126.9644 (their arena floors), which free-fall alone
                    // would overshoot (126.8933).
                    int checked = 0;
                    const float expectedZ[2] = {126.9500f, 126.9644f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        if (xi < 2) {
                            float z = it->position.z;
                            bool ok = z > expectedZ[xi] - 0.0001f && z < expectedZ[xi] + 0.0001f;
                            printf("%s footing XG[%d] landed z=%.4f (oracle %.4f)\n", ok ? "PASS" : "FAIL",
                                   xi, z, expectedZ[xi]);
                            if (!ok) {
                                failures++;
                            }
                            checked++;
                            xi++;
                        }
                    }
                    if (checked != 2) {
                        printf("FAIL footing: expected 2 XG objects, found %d\n", checked);
                        failures++;
                    }
                }
            }
        }
    }

    // Walk command (move_to opcode 78 -> StartMove -> PlanMove -> Walk). The commanded
    // XGs prove the turn math the oracle agrees with byte for byte: opcode 78 records
    // the command, PlanMove finds a clear line (waypoint = destination), and Walk turns
    // the yaw toward it (90 -> 84.5768 and 90 -> 84.0) via the atan2f/wrap/clamp turn;
    // they have not translated yet (speed still ramping from 0). The first XD proves the
    // FULL chain including translation: its script gates the move behind if_sees_kurt
    // and if_flag_set (both now ported), so it walks to the oracle's exact position and
    // yaw. The second XD is a near-miss (yaw 356.3 vs the oracle's 354.0): its direct
    // line is blocked and the detour waypoint differs, a BSP-raycast/detour fidelity
    // slice separate from this one, so the whole L8_t2 table is not yet green.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 8)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL8/LEVEL8.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 8)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
                    runtime.option = 0;
                    for (int t = 0; t < 2; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The first two XGs (in table order) are commanded to move; the
                    // oracle turns them to these yaws with move command 78 active.
                    const float expectedYaw[2] = {84.5768f, 84.0f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end() && xi < 2; ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        float yaw = it->yaw;
                        bool yawOk = yaw > expectedYaw[xi] - 0.0001f && yaw < expectedYaw[xi] + 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XG[%d] yaw=%.4f move=%d (oracle yaw %.4f move 78)\n",
                               (yawOk && cmdOk) ? "PASS" : "FAIL", xi, yaw, it->move_command,
                               expectedYaw[xi]);
                        if (!yawOk || !cmdOk) {
                            failures++;
                        }
                        xi++;
                    }
                    if (xi != 2) {
                        printf("FAIL walk: expected 2 commanded XG objects, found %d\n", xi);
                        failures++;
                    }
                    // The first XD (x > 0) is commanded via if_sees_kurt + if_flag_set
                    // and walks to the oracle's exact position and yaw (full translation).
                    const float xdPos[3] = {18.0663f, 115.0070f, 11.9600f};
                    const float xdYaw = 6.0f;
                    bool xdFound = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XD" || it->position.x <= 0.0f) {
                            continue;
                        }
                        xdFound = true;
                        bool posOk = fabsf(it->position.x - xdPos[0]) < 0.0001f &&
                                     fabsf(it->position.y - xdPos[1]) < 0.0001f &&
                                     fabsf(it->position.z - xdPos[2]) < 0.0001f;
                        bool yawOk = fabsf(it->yaw - xdYaw) < 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XD pos=(%.4f,%.4f,%.4f) yaw=%.4f move=%d (oracle "
                               "(18.0663,115.0070,11.9600) yaw 6.0000 move 78)\n",
                               (posOk && yawOk && cmdOk) ? "PASS" : "FAIL", it->position.x,
                               it->position.y, it->position.z, it->yaw, it->move_command);
                        if (!posOk || !yawOk || !cmdOk) {
                            failures++;
                        }
                        break;
                    }
                    if (!xdFound) {
                        printf("FAIL walk: expected a commanded XD object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    // Pickup landing (UpdatePickup -> Fall). L7_t3 proves the Runner (SW_H150) lands
    // with hover 0, but that does not exercise the Z += hover arithmetic. SW_H01 is a
    // normal pickup that lands on the floor at tick 3 with hover 1.5 (z 103.95 ->
    // 105.45) and FlagLanded set, so it verifies the non-Runner landing path.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 3)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, spritesArg);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    bool found = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "SW_H01") {
                            continue;
                        }
                        found = true;
                        bool zOk = fabsf(it->position.z - 105.4500f) < 0.0001f;
                        bool landed = (it->flags & 0x20000) != 0;  // FlagLanded
                        printf("%s pickup SW_H01 z=%.4f landed=%d (oracle z 105.4500 landed)\n",
                               (zOk && landed) ? "PASS" : "FAIL", it->position.z, landed ? 1 : 0);
                        if (!zOk || !landed) {
                            failures++;
                        }
                        break;
                    }
                    if (!found) {
                        printf("FAIL pickup: expected an SW_H01 object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    // DTI record layout. The object-table goldens above prove the DTI-derived
    // SPAWNED objects match, but they do not observe every record field (the
    // boxEnd far-corner and the name bytes a fan/wind zone keeps in them). This
    // dumps every arena's records with the floats as raw bits and compares to the
    // oracle's, closing the "DTI record discrepancy" claim with a reproduction:
    // the C++ Dti::parseRecords is byte-identical to the C# Dti.ParseRecords.
    for (int level = 3; level <= 8; level++) {
        char dtiPath[128];
        snprintf(dtiPath, sizeof(dtiPath), "TRAVERSE/LEVEL%d/LEVEL%d.DTI", level, level);
        std::vector<uint8_t> dtiBytes;
        if (!data.read(dtiPath, &dtiBytes)) {
            printf("FAIL L%d_dti: DTI did not read\n", level);
            failures++;
            continue;
        }
        Dti dti = Dti::parse(dtiBytes.data(), (int32_t)dtiBytes.size());
        std::string got = dumpDti(level, dti);
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_dti.txt", level);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_dti: no golden file %s\n", level, goldenPath);
            failures++;
            continue;
        }
        if (got == expected) {
            printf("PASS L%d_dti: %d arenas, records match the oracle byte for byte\n", level,
                   (int)dti.arenas().size());
        } else {
            printf("FAIL L%d_dti: records differ\n--- got ---\n%s--- expected ---\n%s", level,
                   got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Dynamic scene updates (open item 1). The scene is built once but the
    // scripts change group state (group_set_state) and group material
    // (group_set_texture) mid-play. This checks the three things that make that
    // work, against real arena geometry:
    //   (a) tagging  -- every batch belongs to exactly one group, and the
    //                   batches partition the drawn triangles by group (so a
    //                   per-batch hidden check skips exactly the hidden group);
    //   (b) hide     -- a group hidden after the build is not visible, while the
    //                   other groups still are (the C# draw-time Hidden check,
    //                   LevelView.cs:126), with no rebuild;
    //   (c) retexture-- a group given a new material shows it on a rebuild (the
    //                   C# LevelView.Rebuild path, LevelView.cs:251).
    {
        int arenasChecked = 0;
        int crossAdjTotal = 0;  // adjacent drawn triangles in different groups sharing a material
        bool testedHide = false, testedRetexture = false;
        for (int level = 3; level <= 8; level++) {
            LevelData levelData;
            if (!LevelData::load(&levelData, &data, level)) {
                continue;
            }
            for (size_t ai = 0; ai < levelData.arenas().size(); ai++) {
                const Arena* arenaC = levelData.arenas()[ai];
                Arena* arena = levelData.arenaNamedMutable(arenaC->name());
                if (!arena) {
                    continue;
                }
                Palette& palette = levelData.paletteOf(arena);
                const std::vector<const TextureArchive*> archives = levelData.archivesOf(arena);
                MaterialResolver resolver(&levelData.dti(), Shading::Original);
                ArenaScene scene;
                if (!buildArenaScene(arena, resolver, palette, archives, &scene) ||
                    scene.batches.empty()) {
                    continue;
                }
                arenasChecked++;

                // (a) Tagging, on EVERY arena: per group, the batch triangles must
                // equal the drawn triangles -- a triangle is drawn when it is not
                // clip/disabled and its material (the group's, else its own) is
                // drawn. A batch that spanned two groups would misattribute its
                // triangles and break this equality, so this proves the build
                // breaks runs at group boundaries (the run-break) and that the
                // batches partition the drawn triangles by group exactly once.
                const std::vector<uint32_t>& flags = arena->triangleFlags();
                const std::vector<int32_t>& mats = arena->triangleMaterials();
                const std::vector<std::string>& names = arena->materials();
                std::map<int32_t, int32_t> expected, got;
                int32_t prevGroup = -1, prevMat = -999;
                bool prevDrawn = false;
                for (size_t t = 0; t < flags.size(); t++) {
                    // A clip/disabled triangle is skipped without breaking the run
                    // (buildArenaScene `continue`s before the run logic), so it does
                    // not reset the previous-drawn tracking either.
                    if ((flags[t] & (arena->clipFlag() | Arena::kGroupDisabled)) != 0) {
                        continue;
                    }
                    const int32_t g = (int32_t)(flags[t] >> 24);
                    int32_t gm = 0;
                    const int32_t mat = arena->groupMaterial(g, &gm) ? gm : mats[t];
                    if (MaterialResolver::isDrawn(mat, names, archives)) {
                        expected[g]++;
                        // Two consecutive drawn triangles in different groups that
                        // share a material: only the run-break keeps them in
                        // separate batches. Count them so the test proves it is
                        // exercised, not just present.
                        if (prevDrawn && g != prevGroup && mat == prevMat) {
                            crossAdjTotal++;
                        }
                        prevDrawn = true;
                        prevGroup = g;
                        prevMat = mat;
                    } else {
                        prevDrawn = false;  // a not-drawn material breaks the run
                    }
                }
                for (size_t i = 0; i < scene.batches.size(); i++) {
                    got[scene.batches[i].group] += scene.batches[i].count / 3;
                }
                if (!(expected == got)) {
                    printf("FAIL scene tagging L%d %s: batches do not partition drawn triangles by group\n",
                           level, arena->name().c_str());
                    failures++;
                }

                // (b) Hide: a non-zero group with batches, hidden after the build.
                if (!testedHide) {
                    int32_t g = 0;
                    for (size_t i = 0; i < scene.batches.size(); i++) {
                        if (scene.batches[i].group != 0) { g = scene.batches[i].group; break; }
                    }
                    if (g != 0) {
                        arena->setGroupState(g, Arena::kGroupHidden);
                        bool anyOfG = false, hiddenAllHidden = true, othersVisible = true;
                        for (size_t i = 0; i < scene.batches.size(); i++) {
                            const bool vis = arenaBatchVisible(arena, scene.batches[i]);
                            if (scene.batches[i].group == g) {
                                anyOfG = true;
                                if (vis) hiddenAllHidden = false;
                            } else if (!vis) {
                                othersVisible = false;
                            }
                        }
                        const bool ok = anyOfG && hiddenAllHidden && othersVisible;
                        printf("%s scene hide L%d %s group %d: hidden group's batches not drawn, others still drawn\n",
                               ok ? "PASS" : "FAIL", level, arena->name().c_str(), g);
                        if (!ok) failures++;
                        testedHide = true;
                        arena->setGroupState(g, 0);  // restore for any later check
                    }
                }

                // (c) Retexture: a non-zero group with a textured batch, given a
                // flat colour; on a rebuild its batches must all be flat (the new
                // material took effect), proving the scene reflects group_set_texture.
                if (!testedRetexture) {
                    int32_t g = 0;
                    for (size_t i = 0; i < scene.batches.size(); i++) {
                        if (scene.batches[i].group != 0 && scene.batches[i].surface.texture != nullptr) {
                            g = scene.batches[i].group;
                            break;
                        }
                    }
                    if (g != 0) {
                        arena->setGroupMaterial(g, -1);  // flat palette colour 1
                        ArenaScene rebuilt;
                        buildArenaScene(arena, resolver, palette, archives, &rebuilt);
                        bool anyOfG = false, allFlat = true;
                        for (size_t i = 0; i < rebuilt.batches.size(); i++) {
                            if (rebuilt.batches[i].group == g) {
                                anyOfG = true;
                                if (rebuilt.batches[i].surface.texture != nullptr) allFlat = false;
                            }
                        }
                        const bool ok = anyOfG && allFlat;
                        printf("%s scene retexture L%d %s group %d: after group_set_texture(-1) its batches are flat on rebuild\n",
                               ok ? "PASS" : "FAIL", level, arena->name().c_str(), g);
                        if (!ok) failures++;
                        testedRetexture = true;
                    }
                }
            }
        }
        printf("PASS scene tagging: %d arenas partition the drawn triangles by group (%d cross-group same-material adjacencies exercised the run-break)\n",
               arenasChecked, crossAdjTotal);
        if (arenasChecked == 0 || crossAdjTotal == 0) {
            printf("FAIL scene dynamic: tagging did not exercise the group run-break (arenas=%d crossAdj=%d)\n",
                   arenasChecked, crossAdjTotal);
            failures++;
        }
        if (!testedHide || !testedRetexture) {
            printf("FAIL scene dynamic: did not exercise hide/retexture (hide=%d retexture=%d)\n",
                   (int)testedHide, (int)testedRetexture);
            failures++;
        }
    }

    if (failures == 0) {
        printf("\nAll cases match the oracle.\n");
        return 0;
    }
    printf("\n%d case(s) differ.\n", failures);
    return 1;
}
