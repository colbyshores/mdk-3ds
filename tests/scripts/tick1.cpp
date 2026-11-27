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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "test_support.h"
#include "stream_dump.h"
#include "effects_dump.h"
#include "skydive_dump.h"
#include "save_dump.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/file_system.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "game/arena_space.h"
#include "game/bomber_overlay.h"
#include "game/follow_camera.h"
#include "game/font_view.h"
#include "game/follow_camera.h"
#include "game/level_data.h"
#include "game/messages.h"
#include "game/object_view.h"
#include "game/player.h"
#include "game/sky_modes.h"

#include "game/sniper_overlay.h"
#include "game/sniper_view.h"
#include "render/arena_scene.h"
#include "render/material_resolver.h"
#include "game/effects_view.h"
#include "scripts/script_runtime.h"
#include "util/matrix.h"
#include "util/math_lut.h"

using namespace mdk;
using namespace mdk::formats;

// The two seam functions the HUD's message stream reaches (FontView's atlas and its
// glyph quads), faked as recorders: `tick1 dump hud` reads the canvas quads exactly
// where the C# probe reads Renderer.DrawImage's. Texture ids count from 1 per dump.
static std::vector<CanvasQuad>* g_canvasQuads = nullptr;
static int32_t g_indexTextures = 0;

MdkTextureId mdk_render_create_index_texture(MdkRenderer, int32_t, int32_t, const uint8_t*,
                                             const uint8_t*) {
    return ++g_indexTextures;
}

MdkTextureId mdk_render_create_streamed_texture(MdkRenderer r, int32_t w, int32_t h,
                                                const uint8_t* px, const uint8_t* paletteRgba) {
    return mdk_render_create_index_texture(r, w, h, px, paletteRgba);
}

// No compressed HD pack here: the index texture (render.h).
MdkTextureId mdk_render_create_hd_image(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                        const uint8_t* paletteRgba, const char*) {
    return mdk_render_create_index_texture(r, w, h, px, paletteRgba);
}

// FontView says its atlas is a canvas texture (render.h); nothing to record.
void mdk_render_mark_canvas(MdkRenderer, MdkTextureId) {}
void mdk_render_mark_unmipped(MdkRenderer, MdkTextureId) {}

void mdk_render_canvas_quad(MdkRenderer, const CanvasQuad* quad) {
    if (g_canvasQuads != nullptr) {
        g_canvasQuads->push_back(*quad);
    }
}

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

// The level's OWN start, dropped and stepped with no input — the C++ twin of the
// oracle probe's `fall` mode, and golden/L7_spawn_fall.txt is its output.
//
// WHY THIS EXISTS. Batch 75 read the Mandarine capture as "the camera films the
// level from outside it", because the guest log ended with Kurt at feet=(0,4,0)
// while the level's first vertex is (11,438,25), and asked why he "falls to z=0
// and stops there instead of landing on a floor". He is on a floor. The first
// vertex of an arena's vertex list says nothing about where the start is: DANT_1
// spans (-172,-17,-27)..(263,1188,279), the start is inside it, and a BSP ray
// down from the spawn hits a floor at z=0.01. The oracle, run the same way, lands
// Kurt on the SAME step at the SAME 0.0002 with the SAME state sequence, so the
// frontend's spawn and footing are faithful and the premise was wrong. This golden
// is what stops the next batch from re-asking it.
//
// WHAT THE TWO SIDES DO AND DO NOT SHARE. Both build the world from the reachable
// arenas, both drop the start by StartDrop (Viewer.cs:100, 3 units), and both step
// Kurt at Viewer.Step with nothing held. The oracle then pins the collision world
// from its ScriptRuntime (`space.SetSolid(scripts.SolidArenas)`, Viewer.cs:533);
// this harness pins it the way mdk_3ds/src/main_3ds.cpp does, geometrically at the
// spawn and again after each step, because the frontend has no ScriptRuntime. The
// columns are the ones BOTH can answer — the pin itself is not printed here, and
// the record carries the oracle's own pin rows separately.
bool spawnFallDump(int level, int steps, const MdkData& data, const Bni* sprites,
                   std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    LevelData levelData;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    ArenaSpace space;
    for (size_t i = 0; i < levelData.arenas().size(); i++) {
        const Arena* arena = levelData.arenas()[i];
        if (levelData.isReachable(arena->name())) {
            space.add(arena);
        }
    }

    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2] + 3.0f};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));

    char line[256];
    const std::vector<Dti::ArenaEntry>& entries = levelData.dti().arenas();
    const int32_t startArena = levelData.dti().startArena();
    snprintf(line, sizeof(line), "level=%d start=%.4f,%.4f,%.4f angle=%.4f startArena=%s\n", level,
             start[0], start[1], start[2], levelData.dti().startAngle(),
             (startArena >= 0 && (size_t)startArena < entries.size())
                 ? entries[(size_t)startArena].name.c_str()
                 : "-");
    *out += line;

    const std::string* pinned =
        space.arenaAt(Vec{player.feet.x, player.feet.y, player.feet.z});
    if (pinned) {
        space.setSolid({*pinned});
    }

    Input input;
    for (int step = 1; step <= steps; step++) {
        player.update(input, 1.0f / 60.0f);
        // The frontend's per-step refresh (main_3ds.cpp, the C#'s own per-step
        // SetSolid shape). It is what a fall-out could in principle move, which is
        // why the golden runs a fall.
        const std::string* now =
            space.arenaAt(Vec{player.feet.x, player.feet.y, player.feet.z});
        if (now) {
            space.setSolid({*now});
        }
        float bottom = 0.0f;
        const bool have =
            space.bottom(Vec{player.feet.x, player.feet.y, player.feet.z}, &bottom);
        if (step <= 12 || step % 20 == 0 || step == steps || !player.onFloor) {
            char bottomText[32];
            if (have) {
                snprintf(bottomText, sizeof(bottomText), "%.4f", bottom);
            } else {
                snprintf(bottomText, sizeof(bottomText), "none");
            }
            snprintf(line, sizeof(line),
                     "s=%d feet=%.4f,%.4f,%.4f state=%s floor=%d vert=%.4f bottom=%s\n", step,
                     player.feet.x, player.feet.y, player.feet.z,
                     kurtStateName(player.state()), player.onFloor ? 1 : 0,
                     player.verticalSpeed, bottomText);
            *out += line;
        }
    }
    snprintf(line, sizeof(line), "END feet=%.4f,%.4f,%.4f state=%s hp=%d\n", player.feet.x,
             player.feet.y, player.feet.z, kurtStateName(player.state()), player.health());
    *out += line;
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
    kHurtKnockdown,  // the knock-down itself: K_BANG, K_BFLIP, and back to Still
    kHurtFlashes,    // the two flashes' fade, the skull's rise, and the end of the beat
    kHurtPush,       // opcode 248: the knock-down push, its drain, and its two guards
    kHurtRoll        // CameraRoll: the view's bank, its two keys and its leveller
};

// An invulnerability write: the step and the seconds. It is the write opcode 244's set
// arm performs (ScriptVm.cs:1598, `runtime.Kurt.Invulnerable = F(o[1])`), which is what
// the C++ twin does through Kurt::setInvulnerable and the oracle probe does through
// scripts.Kurt.Invulnerable -- the same field, the same value, no extra arithmetic.
struct KurtInvulnSet {
    int step;
    float seconds;
};

// A white screen-flash write: the step and the level. It is what opcode 215 (screen_flash,
// ScriptVm.cs:1585) does -- `Kurt.WhiteFlash = MathF.Max(Kurt.WhiteFlash, Round(Value))` --
// so the harness raises the field the same way rather than inventing a setter that maxes.
struct KurtFlashSet {
    int step;
    float value;
};

// A push_kurt write (opcode 248, ScriptVm.cs:1600 -> ScriptRuntime.cs:2826). The eight
// numbers are the operand list the VM would have handed the runtime plus the object it
// would have handed it with: mode is args[0], and the list's arity follows the mode --
// mode 0 reads a and b as the push in the object's frame and up as args[3], while any
// other mode reads a as the speed away from the object and up as args[2]. objYaw is the
// object's yaw, and dx/dy place the object relative to the runtime's own KurtPosition,
// which is what the away branch subtracts (NOT the Player's live feet).
struct KurtPushSet {
    int step;
    int mode;
    float a;
    float b;
    float up;
    float objYaw;
    float dx;
    float dy;
};

// A key window for the roll schedule: the first and last step the key is held. The
// C# probe holds the same windows with input.Hold(Key.TurnLeft/Key.TurnRight,
// Down/Up), so both sides press keys rather than poking the roll field: what is
// pinned is the whole path from the input to the bank, not the arithmetic alone.
struct KurtKeyWindow {
    int from;
    int to;
};

enum KurtHurtColumns {
    kColsVoices,  // hp= vmhp= voices=
    kColsKnock,   // hp= vmhp= knock=
    kColsInvuln,    // hp= vmhp= invuln= knock=
    kColsKnockdown,  // hp= vmhp= invuln= knock= frame=
    kColsFlash,      // hp= vmhp= hurt= white= died=
    kColsPush,       // hp= vmhp= invuln= knock= frame= vel=
    kColsRoll        // roll= (the view's bank in degrees)
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
    const KurtFlashSet* flashes;   // opcode-215 white-flash writes; null for the others
    size_t flashCount;
    int mortalStep;   // step that sets the health to 0; 0 for none
    int columns;
    // The three the knock-down schedule uses, all zero for the older ones (they are
    // defaulted here so the older aggregate initializers above stay warning-free).
    float airLift = 0.0f;   // units above the pad, applied at airLiftStep; 0 for none
    int airLiftStep = 0;
    int forwardFrom = 0;    // first step the forward key is held (0 = never held)
    int forwardTo = 0;      // last step it is held
    const KurtPushSet* pushes = nullptr;  // opcode-248 writes; null for the others
    size_t pushCount = 0;
    // The turn-key windows the roll schedule presses; null for every other one.
    const KurtKeyWindow* turnRight = nullptr;
    size_t turnRightCount = 0;
    const KurtKeyWindow* turnLeft = nullptr;
    size_t turnLeftCount = 0;
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
    if (plan.flashCount) {
        out += " flashes=";
        for (size_t i = 0; i < plan.flashCount; i++) {
            if (i) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%d:%.4f", plan.flashes[i].step, plan.flashes[i].value);
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
    if (plan.turnRightCount) {
        out += " turnRight=";
        for (size_t i = 0; i < plan.turnRightCount; i++) {
            if (i) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%d..%d", plan.turnRight[i].from, plan.turnRight[i].to);
            out += line;
        }
    }
    if (plan.turnLeftCount) {
        out += " turnLeft=";
        for (size_t i = 0; i < plan.turnLeftCount; i++) {
            if (i) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%d..%d", plan.turnLeft[i].from, plan.turnLeft[i].to);
            out += line;
        }
    }
    if (plan.pushCount) {
        out += " pushes=";
        for (size_t i = 0; i < plan.pushCount; i++) {
            if (i) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%d:%d:%.4f,%.4f,%.4f:%.4f,%.4f,%.4f",
                     plan.pushes[i].step, plan.pushes[i].mode, plan.pushes[i].a,
                     plan.pushes[i].b, plan.pushes[i].up, plan.pushes[i].objYaw,
                     plan.pushes[i].dx, plan.pushes[i].dy);
            out += line;
        }
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
        // The turn keys, for the roll schedule only (both lists are empty elsewhere).
        // Both held is the C#'s own axis of zero, which is a case the schedule uses.
        for (size_t i = 0; i < plan.turnRightCount; i++) {
            if (step >= plan.turnRight[i].from && step <= plan.turnRight[i].to) {
                input.down[kKeyTurnRight] = true;
            }
        }
        for (size_t i = 0; i < plan.turnLeftCount; i++) {
            if (step >= plan.turnLeft[i].from && step <= plan.turnLeft[i].to) {
                input.down[kKeyTurnLeft] = true;
            }
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
        for (size_t i = 0; i < plan.flashCount; i++) {
            if (plan.flashes[i].step == step) {
                // The opcode's own max, not a plain write: a weaker flash must not
                // lower a stronger one (ScriptVm.cs:1585).
                const float raise = plan.flashes[i].value;
                runtime.kurt().setWhiteFlash(std::max(runtime.kurt().whiteFlash(), raise));
            }
        }
        for (size_t i = 0; i < hurtCount; i++) {
            if (kHurts[i].step == step) {
                runtime.hurtKurt(kHurts[i].damage);
            }
        }
        for (size_t i = 0; i < plan.pushCount; i++) {
            if (plan.pushes[i].step != step) {
                continue;
            }
            const KurtPushSet& p = plan.pushes[i];
            // The object the pushing script belongs to. The harness builds one the way
            // the oracle probe does: at an offset from the runtime's OWN KurtPosition,
            // so the away branch's subtraction is the same on both sides without either
            // side hard-coding where the start pad is.
            scripts::MdkObject object;
            object.yaw = p.objYaw;
            const Vec3 at = runtime.kurtPosition();
            object.position = Vec3{at.x + p.dx, at.y + p.dy, at.z};
            // The operand list, with the arity the mode has: the C# reads args[3] for the
            // lift in mode 0 and args[2] in every other mode, so one fixed four-element
            // list would feed the wrong operand to the other branch.
            std::vector<formats::ScriptValue> args;
            args.push_back(formats::scriptInt(p.mode));
            args.push_back(formats::scriptFloat(p.a));
            args.push_back(formats::scriptFloat(p.mode == 0 ? p.b : p.up));
            if (p.mode == 0) {
                args.push_back(formats::scriptFloat(p.up));
            }
            runtime.pushKurt(&object, args);
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

        if (plan.columns == kColsFlash) {
            snprintf(line, sizeof(line),
                     "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                     "hurt=%.4f white=%.4f died=%d\n",
                     step, kurtStateName(player.state()),
                     (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                     (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                     player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                     runtime.kurt().health(), player.combat().hurtFlash(),
                     player.combat().whiteFlash(), player.died() ? 1 : 0);
            out += line;
            continue;
        }

        if (plan.columns == kColsPush) {
            // vel= is the knock-down push. With no key held the two walk speeds are
            // exactly zero, so Player::velocity() (C# Kurt.Velocity) reduces to
            // (push.x, push.y, verticalSpeed) and the column reads the push the C#
            // cannot print directly -- _push is private there -- without reading anything
            // the heading LUT can move: a zero walk speed times any facing is still zero.
            const Vec3 vel = player.velocity();
            snprintf(line, sizeof(line),
                     "s=%d state=%s feet64=%d,%d,%d vert=%.4f floor=%d chute=%d hp=%d vmhp=%d "
                     "invuln=%.4f knock=%.4f frame=%.4f vel=%.4f,%.4f\n",
                     step, kurtStateName(player.state()),
                     (int)std::floor(player.feet.x * 64.0f), (int)std::floor(player.feet.y * 64.0f),
                     (int)std::floor(player.feet.z * 64.0f), player.verticalSpeed,
                     player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0, player.health(),
                     runtime.kurt().health(), player.combat().invulnerable(),
                     player.combat().knockDamage(), player.animationFrame(), vel.x, vel.y);
            out += line;
            continue;
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

        if (plan.columns == kColsRoll) {
            // The bank in degrees, exactly as FollowCamera reads it. state= rides along
            // because the walk axis the writer needs is the same one the state machine
            // zeroes: if a knock-down or a hard landing ever crept into this schedule,
            // the state column says so instead of a silent roll that will not move.
            snprintf(line, sizeof(line), "s=%d state=%s roll=%.4f\n", step,
                     kurtStateName(player.state()), player.cameraRoll().roll());
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

// THE CHAIN GUN, end to end (golden/L8_fire.txt, L8_fire_run.txt, L7_fire_run.txt).
// Kurt and the scripts runtime are
// stepped together at 1/60 s, the way the C# Viewer steps them (Kurt, then the scripts),
// with a scripted input that holds fire, and the oracle probe (`Probe <level> <steps>`,
// a C# console program over src/Mdk.Game that builds the same world) prints the same
// line per step. What each column pins:
//
//   state / frame   Kurt.UpdateFiring and the state machine: K_SHOT while he stands and
//                   fires, K_RUNFIR while he runs and fires, K_TRN45 while he turns.
//   fire / muzzle   Kurt.Firing, and whether a muzzle flash is up (UpdateMuzzle: odd
//                   ticks only, and only in the states whose frames do not draw it --
//                   the turn here). The flash's FRAME and jitter come from the oracle's
//                   unseeded `new Random()`, so they are not reproducible and not
//                   printed; that the game's random stream is untouched by them is what
//                   life= and pc= below pin.
//   shots / hits /  ScriptRuntime.FireChainGun and ChainGunHit: 6 shots a fired tick,
//   kills           a hit a tick on the best target, the kill that explodes it.
//   ohp / bar       the targets' health and the health bar the hits raise (BarObject).
//   pieces / life / the sparks and the break-up pieces: the count, the sum of their
//   pc / effects    remaining ticks and of their centres (one decimal), all of which
//                   move with every random draw the hits, the ricochets and the
//                   explosion make -- so a draw out of the C#'s order shows here -- and
//                   the explosion's gore drops and its pieces' smoke trails.
//   white           the explosion's flash (FlashAt).
//   voices          MULTIFIRE looping while he fires (stopped when he stops), the
//                   ricochets every 4 ticks, the explosion's sound.
//
// The world is the hurt dump's -- the reachable arenas, no pinned solid arena -- with one
// difference that is the oracle's own: Kurt's sounds go to the RUNTIME's mixer, because
// the C# has one mixer that Kurt and the scripts share, and the voices column interleaves
// the gun with the ricochets.
//
// TWO SCHEDULES, because one run cannot reach both stops of the gun. The first
// (golden/L8_fire.txt) stands on the start pad, so an alien's laser knocks him down at
// step 297 with the gun held: KnockDown's StopFiring, the gun refused while he is down
// (updateFiring's !Knocked), and the forward key ignored inside K_BANG. The second
// (golden/L8_fire_run.txt) runs forward at 250, which takes him out of that laser's way,
// so he fires while running (K_RUNFIR and its FOOT3/FOOT4 frames 4 and 17) and stops
// because the key comes up (the loop stopped, Shot back to Still).
//
// WHY THE RUN SCHEDULE PRINTS NO pc=. Player::facing() takes its sine and cosine from the
// trig LUT where C# Kurt.Facing calls MathF.Cos/Sin -- the owner-gated difference
// docs/OPEN_ITEMS.md records for golden/L3_kurt_steps.txt. Thirty steps of running leave
// the feet ~1e-5 off the oracle's (feet64= still matches), the hit points and the sparks
// placed from them inherit it, and the sum of ~50 centres then straddles a rounding
// boundary of the printed decimal on four of 480 lines. Measured 2026-11-19: with
// facing() switched to cosf/sinf the run schedule matches the oracle byte for byte, pc=
// included. So the run schedule pins the random stream through life= (the pieces'
// remaining ticks, an integer every Debris.Start draws into) and keeps pc= off; the
// standing schedule never moves Kurt and prints both.
struct FireSchedule {
    int fireFrom, fireTo, turnFrom, turnTo, runFrom, runTo;
    bool centres;
};
const FireSchedule kFireStand = {10, 400, 200, 230, 300, 330, true};
const FireSchedule kFireRun = {10, 460, 200, 230, 250, 280, false};

bool kurtFireDump(int level, int steps, const FireSchedule& plan, const MdkData& data,
                  const Bni* sprites, std::string* out, std::string* why) {
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    char line[256];
    snprintf(line, sizeof(line), "level=%d steps=%d seed=1 fire=%d-%d turn=%d-%d run=%d-%d\n",
             level, steps, plan.fireFrom, plan.fireTo, plan.turnFrom, plan.turnTo, plan.runFrom,
             plan.runTo);
    *out = line;
    for (int step = 1; step <= steps; step++) {
        Input input;
        input.down[kKeyFire] = step >= plan.fireFrom && step < plan.fireTo;
        input.down[kKeyTurnLeft] = step >= plan.turnFrom && step < plan.turnTo;
        input.down[kKeyForward] = step >= plan.runFrom && step < plan.runTo;
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        int32_t ohp = 0;
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i].dead) {
                ohp += objects[i].health;
            }
        }
        Vec3 pc{0.0f, 0.0f, 0.0f};
        int32_t life = 0;
        const std::vector<scripts::Debris::Piece*>& pieces = runtime.debris().pieces();
        for (size_t i = 0; i < pieces.size(); i++) {
            pc.x += pieces[i]->center.x;
            pc.y += pieces[i]->center.y;
            pc.z += pieces[i]->center.z;
            life += pieces[i]->ticks;
        }
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        char centres[96] = "";
        if (plan.centres) {
            snprintf(centres, sizeof(centres), " pc=%.1f,%.1f,%.1f", pc.x, pc.y, pc.z);
        }
        const scripts::ScriptRuntime::BarValues bar = runtime.bar();
        char head[512];
        snprintf(head, sizeof(head),
                 "s=%d state=%s feet64=%d,%d,%d frame=%.4f fire=%d muzzle=%d hp=%d shots=%d "
                 "hits=%d kills=%d objs=%d ohp=%d bar=%d/%d pieces=%d life=%d%s effects=%d "
                 "white=%.4f voices=",
                 step, kurtStateName(player.state()), (int)std::floor(player.feet.x * 64.0f),
                 (int)std::floor(player.feet.y * 64.0f), (int)std::floor(player.feet.z * 64.0f),
                 player.animationFrame(), player.combat().firing() ? 1 : 0,
                 player.combat().hasMuzzle() ? 1 : 0, player.health(), runtime.stats.shots,
                 runtime.stats.shotHits, runtime.stats.kills, runtime.objectCount(), ohp,
                 bar.health, bar.max, runtime.debris().pieceCount(), life, centres,
                 runtime.effects.count(), player.combat().whiteFlash());
        *out += head;
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}

// SNIPER MODE, end to end (golden/L3_sniper.txt, L7_sniper.txt): Kurt alone -- no scripts
// runtime -- stepped at 1/60 s under kSniperHolds, against the oracle probe
// (tests/scripts/sniper_probe, `Probe <level> <steps>`), which runs the same table. It
// pins Player's sniper step (game/player_sniper.cpp: the key, entering and leaving, the
// controls; player.cpp's sniperMove), the scope (game/scope.h: the look, the zoom by keys
// and wheel, the clip, the ammo keys, the eye and line of sight, ToScreen and Locks),
// Kurt's ammo (SelectAmmo/UseAmmo/NextAmmo) and the scope's view (follow_camera.h's
// buildSniperCamera, the C#'s FollowCamera.SniperView) for the 3DS top screen's aspect.
// Every float is its exact bits. What each part of the line is:
//
//   state floor snipe   the state machine (STILL while sniping, IDLE after), OnFloor, Sniping
//   feet yaw vz strafe  the sidestep and the fall, the scope's turn
//   pitch zoom limit    the scope; limit is ZoomMin (#6's lock writes it)
//   clip sel ammo       the clip's rounds and timer, the selected type, the five counts
//   eye fwd             Kurt.SniperEye / SniperForward
//   cam                 the view's position / forward / up / near
//   scr                 Scope.ToScreen of two fixed points (the start + (60, 4, 5), and
//                       the start - (60, 0, 0)) and whether a 16-pixel box there Locks;
//                       `-` behind the eye
//   m                   an FNV-1a hash of the view-projection and projection matrices'
//                       bits (printed whole every 20th sniping step: vp=, proj=)
//   voices              the mixer's voice count, its newest voice, and the BREATH/ZOOM loops
//
// The view-projection is view x projection with System.Numerics' multiply (an fmaf
// chain per element, render/post_params.cpp's), formed here because the port's Camera
// keeps the two apart and the renderers multiply them.
//
// WHY YAW 0. The run starts facing +X, not the level's start angle, and does all of its
// looking down and sidestepping before its first turn: Player::facing() is the trig LUT
// (docs/OPEN_ITEMS.md, owner-gated), and it enters only the sidestep and the eye while
// looking down, where at yaw 0 the LUT's cos/sin are exactly 1 and 0.
struct SniperHold {
    Key key;
    int from;
    int to;
};
const SniperHold kSniperHolds[] = {
    {kKeySniper, 40, 42},         {kKeyBack, 50, 80},          {kKeyStrafeRight, 60, 90},
    {kKeyZoomIn, 90, 150},        {kKeyFire, 110, 125},        {kKeyForward, 160, 290},
    {kKeyFire, 200, 262},         {kKeyTurnRight, 300, 330},   {kKeyTurbo, 310, 320},
    {kKeyZoomOut, 320, 360},      {kKeyTurnLeft, 340, 360},    {kKeyItemNext, 390, 392},
    {kKeyItemPrevious, 410, 412}, {kKeyItemNext, 430, 432},    {kKeyFire, 450, 530},
    {kKeySniper, 540, 542},       {kKeyJump, 560, 566},        {kKeySniper, 564, 566},
};
const struct {
    int step;
    float notches;
} kSniperWheels[] = {{140, 1.0f}, {365, -2.0f}, {375, 1.0f}};
const struct {
    int step;
    const char* model;
} kSniperGives[] = {{5, "SW_HGREN"}, {6, "SW_BONES"}};

std::string sniperBits(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    char text[16];
    snprintf(text, sizeof(text), "%08X", (unsigned)bits);
    return text;
}

std::string sniperVec(const Vec3& v) {
    return sniperBits(v.x) + "," + sniperBits(v.y) + "," + sniperBits(v.z);
}

// System.Numerics' Matrix4x4 multiply on the oracle's host: each element an fmaf chain
// over the row and the column (render/post_params.cpp's measured shape).
void sniperMultiply(const float a[16], const float b[16], float out[16]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            out[row * 4 + col] =
                std::fmaf(a[row * 4 + 3], b[12 + col],
                          std::fmaf(a[row * 4 + 2], b[8 + col],
                                    std::fmaf(a[row * 4 + 1], b[4 + col], a[row * 4] * b[col])));
        }
    }
}

uint32_t sniperFnv(uint32_t hash, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 4; i++) {
        hash = (hash ^ ((bits >> (8 * i)) & 0xFFu)) * 16777619u;
    }
    return hash;
}

bool kurtSniperDump(int level, int steps, const MdkData& data, const Bni* sprites, std::string* out,
                    std::string* why) {
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
    player.yaw = 0.0f;
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    player.setMixer(&mixer);

    const float aspect = 400.0f / 240.0f;
    // The probe's two ToScreen points: ahead and a little to the side (it locks at zoom
    // 1 and leaves the lock square zoomed in), and behind the eye.
    const Vec3 probes[2] = {Vec3{start[0] + 60.0f, start[1] + 4.0f, start[2] + 5.0f},
                            Vec3{start[0] - 60.0f, start[1] - 0.0f, start[2] - 0.0f}};
    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " aspect=" + sniperBits(aspect) + " lock=" + sniperBits(Scope::lockLimit(12.0f, 80.0f)) +
           "," + sniperBits(Scope::lockLimit(4.0f, 0.5f)) + "\n";
    for (int step = 1; step <= steps; step++) {
        for (size_t g = 0; g < sizeof(kSniperGives) / sizeof(kSniperGives[0]); g++) {
            if (kSniperGives[g].step == step) {
                player.collect(kSniperGives[g].model);
            }
        }
        Input input;
        for (size_t h = 0; h < sizeof(kSniperHolds) / sizeof(kSniperHolds[0]); h++) {
            if (step >= kSniperHolds[h].from && step < kSniperHolds[h].to) {
                input.down[kSniperHolds[h].key] = true;
            }
        }
        for (size_t w = 0; w < sizeof(kSniperWheels) / sizeof(kSniperWheels[0]); w++) {
            if (kSniperWheels[w].step == step) {
                input.wheel = kSniperWheels[w].notches;
            }
        }
        player.update(input, 1.0f / 60.0f);

        const Scope& scope = player.scope();
        Camera camera;
        if (!buildSniperCamera(&camera, player, aspect)) {
            *why = "step " + std::to_string(step) + ": the scope view is degenerate";
            return false;
        }
        float vp[16];
        sniperMultiply(camera.view, camera.projection, vp);
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 16; i++) {
            hash = sniperFnv(hash, vp[i]);
        }
        for (int i = 0; i < 16; i++) {
            hash = sniperFnv(hash, camera.projection[i]);
        }
        const Vec3 eye = player.sniperEye();
        const Vec3 forward = player.sniperForward();
        std::string scr;
        for (int p = 0; p < 2; p++) {
            Vec2 screen;
            scr += p ? ";" : "";
            if (!Scope::toScreen(eye, forward, scope.zoom, probes[p], &screen)) {
                scr += "-";
                continue;
            }
            const bool locked = Scope::locks(Vec2{screen.x - 8.0f, screen.y - 8.0f},
                                             Vec2{screen.x + 8.0f, screen.y + 8.0f});
            scr += sniperBits(screen.x) + "," + sniperBits(screen.y) + ":" + (locked ? "1" : "0");
        }
        const Kurt& kurt = player.combat();
        std::string ammo;
        for (int32_t t = 1; t <= Kurt::kAmmoTypes; t++) {
            ammo += (t > 1 ? "," : "") + std::to_string(kurt.ammo(t));
        }
        const std::vector<audio::SoundMixer::Voice>& live = mixer.voices();
        bool breath = false;
        bool zoom = false;
        for (size_t i = 0; i < live.size(); i++) {
            breath = breath || live[i].name == "BREATH";
            zoom = zoom || live[i].name == "ZOOM";
        }
        char hashText[16];
        snprintf(hashText, sizeof(hashText), "%08X", (unsigned)hash);
        *out += "s=" + std::to_string(step) + " state=" + kurtStateName(player.state()) +
                " floor=" + (player.onFloor ? "1" : "0") + " snipe=" + (player.sniping() ? "1" : "0") +
                " feet=" + sniperVec(player.feet) + " yaw=" + sniperBits(player.yaw) +
                " vz=" + sniperBits(player.verticalSpeed) + " strafe=" + sniperBits(player.strafeSpeed) +
                " pitch=" + sniperBits(scope.pitch) + " zoom=" + sniperBits(scope.zoom) +
                " limit=" + sniperBits(scope.zoomLimit) + " clip=" + std::to_string(scope.clipRounds) +
                "/" + sniperBits(scope.clipTime) + " sel=" + std::to_string(kurt.selectedAmmo()) +
                " ammo=" + ammo + " eye=" + sniperVec(eye) + " fwd=" + sniperVec(forward) +
                " cam=" + sniperVec(camera.position) + "/" + sniperVec(camera.forward) + "/" +
                sniperVec(camera.up) + "/" + sniperBits(camera.near_plane) + " scr=" + scr +
                " m=" + hashText + " voices=" + std::to_string(live.size()) + ":" +
                (live.empty() ? std::string("-") : live.back().name) +
                " breath=" + (breath ? "1" : "0") + " zoom=" + (zoom ? "1" : "0") + "\n";
        if (player.sniping() && step % 20 == 0) {
            *out += "  vp=";
            for (int i = 0; i < 16; i++) {
                *out += (i ? "," : "") + sniperBits(vp[i]);
            }
            *out += "\n  proj=";
            for (int i = 0; i < 16; i++) {
                *out += (i ? "," : "") + sniperBits(camera.projection[i]);
            }
            *out += "\n";
        }
    }
    return true;
}

// KURT'S ITEMS, end to end (golden/L*_items_*.txt). Kurt and the scripts runtime stepped
// together at 1/60 s as kurtFireDump does, under a schedule the oracle probe
// (tests/scripts/items_probe, `Probe <level> <steps> <schedule>`) reads the same way:
// comma-separated `key:from-to` holds (fwd back left right sleft sright jump turbo fire
// use next prev) over steps [from, to), `give:step:MODEL` hands Kurt a pickup before
// that step (the console's give, LevelCommands.Give -> Kurt.Collect), and
// `drop:step:MODEL:dx:dy:dz` spawns a pickup (Spawning.Flagged) in his arena at the
// level's start plus that offset, for him to run through. What each column pins:
//
//   state / frame    UpdateItems and the throw: THROW (K_SPWEP) on the floor, the item
//                    leaving his hand on frame 8; in the air ItemUsed at once.
//   hp / kd          Collect's health pickups; the blasts on Kurt (Kurt.Hurt, the knock
//                    damage doubled) and the mortar's knock-down.
//   inv / sel / sgun the inventory: Collect, the item keys (SelectNext), Consume, the
//   ammo / picked    super chain gun's time, the sniper ammo, the PickedUp messages.
//   objs / ohp /     the pickups' removal and shrink (collected=), the thrown items in
//   kills / thrown / flight and active (kind, 0x1000/0x4000 flags, ticks, model,
//   collected        position x64, yaw, scale, animation frame), the blasts' damage.
//   bomb / decoy     Items.Bomb and Items.Decoy.
//   tw               the twisters: how many, and the sum of their positions x64.
//   pieces / effects the explosions' break-ups, sparks and gore.
//   white / shake    the nuke's and the explosions' white flash, the screen shake.
//   voices           COLLECT/APPLE/WMIB..., the item sounds, the explosions.
//
// The world is kurtFireDump's (the reachable arenas; Kurt's sounds on the runtime's
// mixer, the oracle's one mixer). The probe registers every arena's triangle groups, as
// the C# Viewer does through LevelView, which the runtime's constructor does here.
struct ItemsHold {
    Key key;
    int from;
    int to;
};
struct ItemsGive {
    int step;
    std::string model;
    bool drop;      // `drop:`: spawned (Spawning.Flagged) at the start + offset, not given
    Vec3 offset;
};

// `msg:step:NAME:flags:seconds`: Messages.Push before that step (the hud view only).
struct ItemsPush {
    int step;
    std::string name;
    int32_t flags;
    float seconds;
};

bool parseItemsSchedule(const std::string& schedule, std::vector<ItemsHold>* holds,
                        std::vector<ItemsGive>* gives, std::vector<ItemsPush>* pushes) {
    static const struct {
        const char* name;
        Key key;
    } kKeys[] = {{"fwd", kKeyForward},      {"back", kKeyBack},       {"left", kKeyTurnLeft},
                 {"right", kKeyTurnRight},  {"sleft", kKeyStrafeLeft}, {"sright", kKeyStrafeRight},
                 {"jump", kKeyJump},        {"turbo", kKeyTurbo},     {"fire", kKeyFire},
                 {"use", kKeyUseItem},      {"next", kKeyItemNext},   {"prev", kKeyItemPrevious}};
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        if (token.empty()) {
            continue;
        }
        const size_t colon = token.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const std::string head = token.substr(0, colon);
        const std::string rest = token.substr(colon + 1);
        if (head == "msg") {
            char name[32];
            ItemsPush push;
            if (sscanf(rest.c_str(), "%d:%31[^:]:%d:%f", &push.step, name, &push.flags,
                       &push.seconds) != 4) {
                return false;
            }
            push.name = name;
            pushes->push_back(push);
            continue;
        }
        if (head == "give" || head == "drop") {
            std::vector<std::string> fields;
            size_t at = 0;
            while (true) {
                const size_t next = rest.find(':', at);
                fields.push_back(rest.substr(at, next == std::string::npos ? std::string::npos
                                                                         : next - at));
                if (next == std::string::npos) {
                    break;
                }
                at = next + 1;
            }
            const bool drop = head == "drop";
            if (fields.size() != (drop ? 5u : 2u)) {
                return false;
            }
            ItemsGive give;
            give.step = atoi(fields[0].c_str());
            give.model = fields[1];
            give.drop = drop;
            give.offset = drop ? Vec3{(float)atof(fields[2].c_str()), (float)atof(fields[3].c_str()),
                                      (float)atof(fields[4].c_str())}
                               : Vec3{0.0f, 0.0f, 0.0f};
            gives->push_back(give);
            continue;
        }
        const size_t dash = rest.find('-');
        if (dash == std::string::npos) {
            return false;
        }
        bool found = false;
        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
            if (head == kKeys[i].name) {
                ItemsHold hold;
                hold.key = kKeys[i].key;
                hold.from = atoi(rest.substr(0, dash).c_str());
                hold.to = atoi(rest.substr(dash + 1).c_str());
                holds->push_back(hold);
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// The harness's subscriber to Inventory.PickedUp: the names, as the probe's list.
void recordPickedUp(void* context, const std::string& name) {
    static_cast<std::vector<std::string>*>(context)->push_back(name);
}

// A float's bits, the exact value (the probe's B).
std::string hexBits(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    char text[16];
    snprintf(text, sizeof(text), "%08X", bits);
    return text;
}

// FNV-1a over an int's four little-endian bytes (the probe's Fnv).
uint32_t fnvInt(uint32_t hash, uint32_t value) {
    for (int32_t i = 0; i < 4; i++) {
        hash = (hash ^ ((value >> (8 * i)) & 0xFFu)) * 16777619u;
    }
    return hash;
}

uint32_t fnvFloat(uint32_t hash, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return fnvInt(hash, bits);
}

// A message as the probe prints it: flags:line/line#time, printable ASCII as is.
std::string messageText(const Messages::Message& m) {
    std::string text = std::to_string(m.flags) + ":";
    for (int32_t line = 0; line < m.lineCount; line++) {
        if (line) {
            text += "/";
        }
        for (int32_t i = 0; i < m.length[line]; i++) {
            const uint8_t b = m.text[line][i];
            if (b >= 32 && b <= 126) {
                text += (char)b;
            } else {
                char escape[8];
                snprintf(escape, sizeof(escape), "\\x%02X", b);
                text += escape;
            }
        }
    }
    char time[32];
    snprintf(time, sizeof(time), "#%.4f", m.time);
    return text + time;
}

// The fonts and texts of the HUD's messages, as HudView makes them (MDKFONT.FTI's
// FONTBIG and FONTSML, spaces 6 and 4, through SYS_PAL), on the recording seam:
// FONTBIG is texture 1 and FONTSML 2, as the probe numbers them.
struct HudFonts {
    FontView big;
    FontView small;
};

bool loadHudFonts(const MdkData& data, HudFonts* fonts, Messages* messages, std::string* why) {
    std::vector<uint8_t> ftiBytes;
    Fti fti;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    std::vector<uint8_t> sysPal;
    Font bigFont;
    Font smallFont;
    if (!data.read("MISC/MDKFONT.FTI", &ftiBytes) ||
        !Fti::parse(&fti, ftiBytes.data(), (int32_t)ftiBytes.size()) ||
        !fti.getBytes("FONTBIG", &bigBytes) || !fti.getBytes("FONTSML", &smallBytes) ||
        !fti.getBytes("SYS_PAL", &sysPal) ||
        !Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, &bigFont) ||
        !Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, &smallFont)) {
        *why = "MDKFONT.FTI's fonts did not load";
        return false;
    }
    const Palette system = Palette::fromRgb(sysPal.data(), (int32_t)sysPal.size());
    g_indexTextures = 0;
    if (!fonts->big.load(nullptr, bigFont, 0, system.rgba()) ||
        !fonts->small.load(nullptr, smallFont, 0, system.rgba())) {
        *why = "the font atlases did not build";
        return false;
    }
    messages->setTexts(fti);
    return true;
}

// `view` is "" (the items columns, kurtItemsDump's own), "hud" (the message stream:
// Messages, as the probe's HudLine prints it) or "ribbon" (the twisters' ribbons, the
// probe's RibbonLine). The run is the same in all three.
bool kurtItemsDump(int level, int steps, const std::string& schedule, const MdkData& data,
                   const Bni* sprites, std::string* out, std::string* why,
                   const std::string& view = std::string()) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(schedule, &holds, &gives, &pushes)) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    // The pickups' names: recorded (the items columns' picked=), or the HUD's message
    // stream bound as Viewer binds it (the hud view), the scripts' hud_message with it.
    std::vector<std::string> pickedUp;
    std::unique_ptr<HudFonts> fonts;
    Messages messages;
    std::vector<CanvasQuad> quads;
    Kurt::PickupHook hook;
    if (view == "hud") {
        fonts.reset(new HudFonts());
        if (!loadHudFonts(data, fonts.get(), &messages, why)) {
            return false;
        }
        hook.context = &messages;
        hook.pickedUp = Messages::pickedUp;
        runtime.hudMessages = &messages;
    } else {
        hook.context = &pickedUp;
        hook.pickedUp = recordPickedUp;
    }
    player.combat().setPickupHook(hook);

    *out = (view.empty() ? std::string() : view + " ") + "level=" + std::to_string(level) +
           " steps=" + std::to_string(steps) + " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step && !gives[i].drop) {
                player.collect(gives[i].model);
            }
        }
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step && gives[i].drop) {
                const Vec3 at{start[0] + gives[i].offset.x, start[1] + gives[i].offset.y,
                              start[2] + gives[i].offset.z};
                // MODEL>NAME spawns MODEL and names it NAME: no retail arena has SW_EWJ's
                // model, so the cow it drops is reached through a pickup renamed to it.
                const std::string& spec = gives[i].model;
                const size_t arrow = spec.find('>');
                scripts::MdkObject* dropped = runtime.spawn(
                    &runtime.getArenaState(runtime.currentArena()).controller,
                    spec.substr(0, arrow), at, 0.0f, -1, 0, true);
                if (dropped != nullptr && arrow != std::string::npos) {
                    dropped->type_name = spec.substr(arrow + 1);
                }
            }
        }
        for (size_t i = 0; i < pushes.size(); i++) {
            if (pushes[i].step == step && fonts) {
                // Through the runtime's hud_message seam (ScriptVm.cs:1186's
                // `runtime.Messages?.Push`), so its binding is what the golden pins.
                runtime.pushMessage(pushes[i].name, pushes[i].flags, pushes[i].seconds);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        if (view == "hud") {
            // Viewer.DrawScene: Update(elapsed), then the HUD's Draw on a 600-wide canvas.
            messages.update(1.0f / 60.0f);
            quads.clear();
            g_canvasQuads = &quads;
            messages.draw(nullptr, fonts->big, fonts->small, 600.0f);
            g_canvasQuads = nullptr;
            std::string queue;
            for (int32_t i = 0; i < messages.queued(); i++) {
                queue += (i ? "|" : "") + messageText(messages.queuedAt(i));
            }
            uint32_t hash = 2166136261u;
            std::string list;
            for (size_t i = 0; i < quads.size(); i++) {
                const CanvasQuad& q = quads[i];
                hash = fnvInt(hash, (uint32_t)q.texture);
                hash = fnvFloat(fnvFloat(fnvFloat(fnvFloat(hash, q.x0), q.y0), q.x2), q.y2);
                list += (i ? ";T" : "T") + std::to_string(q.texture) + ":" + hexBits(q.x0) +
                        "," + hexBits(q.y0) + "," + hexBits(q.x2) + "," + hexBits(q.y2);
            }
            snprintf(buffer, sizeof(buffer), "s=%d cur=%s t=%.4f z=%.4f q=%s draw=%d:%08X", step,
                     messages.current() ? messageText(*messages.current()).c_str() : "-",
                     messages.time(), messages.zoom(), queue.empty() ? "-" : queue.c_str(),
                     (int)quads.size(), hash);
            *out += buffer;
            if (step % 10 == 0 && !quads.empty()) {
                *out += " quads=" + list;
            }
            *out += "\n";
            continue;
        }
        if (view == "ribbon") {
            const std::vector<scripts::Twister>& ribbons = runtime.twisters();
            *out += "s=" + std::to_string(step) + " tw=" + std::to_string(ribbons.size());
            std::string hashes;
            for (size_t i = 0; i < ribbons.size(); i++) {
                Vec3 positions[scripts::Ribbon::kMaxCorners];
                Vec2 uvs[scripts::Ribbon::kMaxCorners];
                const int32_t count = ribbons[i].ribbon().triangles(positions, uvs);
                uint32_t hash = 2166136261u;
                for (int32_t c = 0; c < count; c++) {
                    hash = fnvFloat(fnvFloat(fnvFloat(hash, positions[c].x), positions[c].y),
                                    positions[c].z);
                    hash = fnvFloat(fnvFloat(hash, uvs[c].x), uvs[c].y);
                }
                snprintf(buffer, sizeof(buffer), "%s%d:%08X", i ? "," : "", (int)count, hash);
                hashes += buffer;
                if (i == 0) {
                    *out += " r0=" + std::to_string(count) + ":";
                    for (int32_t c = 0; c < count; c++) {
                        snprintf(buffer, sizeof(buffer), "%s%s,%s,%s/%.1f,%.1f", c ? ";" : "",
                                 hexBits(positions[c].x).c_str(),
                                 hexBits(positions[c].y).c_str(),
                                 hexBits(positions[c].z).c_str(), uvs[c].x, uvs[c].y);
                        *out += buffer;
                    }
                    if (count == 0) {
                        *out += "-";
                    }
                }
            }
            *out += " h=" + (hashes.empty() ? std::string("-") : hashes) + "\n";
            continue;
        }

        const Kurt& combat = player.combat();
        std::string slots;
        for (int32_t i = 0; i < combat.slotCount(); i++) {
            snprintf(buffer, sizeof(buffer), "%s%d:%d", i ? "|" : "", (int)combat.slots()[i].item,
                     combat.slots()[i].count);
            slots += buffer;
        }
        std::string picked;
        for (size_t i = 0; i < pickedUp.size(); i++) {
            picked += (i ? "|" : "") + pickedUp[i];
        }
        pickedUp.clear();
        int32_t ohp = 0;
        std::string thrown;
        std::string collected;
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (o.dead) {
                continue;
            }
            ohp += o.health;
            if (o.thrown_kind > 0) {
                snprintf(buffer, sizeof(buffer), "%s%d/%X/%d/%s@%d,%d,%d/y%.4f/s%.4f/f%d",
                         thrown.empty() ? "" : ";", o.thrown_kind, o.flags & 0x5000, o.item_ticks,
                         o.type_name.c_str(), p64(o.position.x), p64(o.position.y),
                         p64(o.position.z), o.yaw, o.scale, o.animation_frame);
                thrown += buffer;
            } else if ((o.flags & scripts::MdkObject::FlagCollected) != 0) {
                snprintf(buffer, sizeof(buffer), "%s%s/%.4f", collected.empty() ? "" : ";",
                         o.type_name.c_str(), o.scale);
                collected += buffer;
            }
        }
        Vec3 tw{0.0f, 0.0f, 0.0f};
        const std::vector<scripts::Twister>& twisters = runtime.twisters();
        for (size_t i = 0; i < twisters.size(); i++) {
            tw.x += twisters[i].position().x;
            tw.y += twisters[i].position().y;
            tw.z += twisters[i].position().z;
        }
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        snprintf(buffer, sizeof(buffer),
                 "s=%d state=%s feet64=%d,%d,%d yaw=%.4f frame=%.4f hp=%d kd=%.4f inv=%s sel=%d "
                 "sgun=%d ammo=%d:%d:%d:%d:%d/%d picked=%s objs=%d ohp=%d kills=%d thrown=",
                 step, kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.yaw, player.animationFrame(), player.health(),
                 combat.knockDamage(), slots.empty() ? "-" : slots.c_str(), combat.selected(),
                 combat.superChainGun(), combat.ammo(1), combat.ammo(2), combat.ammo(3),
                 combat.ammo(4), combat.ammo(5), combat.selectedAmmo(),
                 picked.empty() ? "-" : picked.c_str(), runtime.objectCount(), ohp,
                 runtime.stats.kills);
        *out += buffer;
        *out += thrown.empty() ? "-" : thrown;
        *out += " collected=";
        *out += collected.empty() ? "-" : collected;
        snprintf(buffer, sizeof(buffer),
                 " bomb=%d decoy=%d tw=%d@%d,%d,%d pieces=%d effects=%d white=%.4f shake=%.4f "
                 "voices=",
                 runtime.itemsBomb != nullptr && !runtime.itemsBomb->dead ? 1 : 0,
                 runtime.itemsDecoy != nullptr && !runtime.itemsDecoy->dead ? 1 : 0,
                 (int)twisters.size(), p64(tw.x), p64(tw.y), p64(tw.z),
                 runtime.debris().pieceCount(), runtime.effects.count(), combat.whiteFlash(),
                 runtime.shake);
        *out += buffer;
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}

// THE CHEATS' KURT (golden/cheats_L*.txt, gap package #21): god mode and noclip
// (C# Kurt.Cheats.cs, the god guards in Kurt.Fall and Kurt.Damage) under a schedule.
// The oracle is tests/scripts/cheats_probe (`Probe kurt <level> <steps> <schedule>`):
// Kurt, the scripts and Kurt's solid arenas stepped at 1/60 s as Viewer.RunSteps does
// (the solid arenas are what Fall's Bottom reads, so the fall-out death needs them).
// The schedule: `key:from-to` (fwd back left right sleft sright jump turbo down fire),
// `god|mortal|noclip|clip:step` (LevelCommands.SetGod/SetNoclip before that step),
// `hurt:step:N` (Kurt.Hurt before that step), `yaw:DEG` (his yaw at the start).
bool kurtCheatsDump(int level, int steps, const std::string& schedule, const MdkData& data,
                    const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    struct Hold {
        Key key;
        int from;
        int to;
    };
    struct Switch {
        int step;
        std::string what;
    };
    struct Hurt {
        int step;
        int damage;
    };
    static const struct {
        const char* name;
        Key key;
    } kKeys[] = {{"fwd", kKeyForward},      {"back", kKeyBack},       {"left", kKeyTurnLeft},
                 {"right", kKeyTurnRight},  {"sleft", kKeyStrafeLeft}, {"sright", kKeyStrafeRight},
                 {"jump", kKeyJump},        {"turbo", kKeyTurbo},     {"down", kKeyDown},
                 {"fire", kKeyFire}};
    std::vector<Hold> holds;
    std::vector<Switch> switches;
    std::vector<Hurt> hurts;
    bool hasYaw = false;
    float startYaw = 0.0f;
    size_t at = 0;
    while (at < schedule.size()) {
        size_t comma = schedule.find(',', at);
        if (comma == std::string::npos) {
            comma = schedule.size();
        }
        const std::string token = schedule.substr(at, comma - at);
        at = comma + 1;
        if (token.empty()) {
            continue;
        }
        const size_t colon = token.find(':');
        if (colon == std::string::npos) {
            *why = "the schedule did not parse: " + token;
            return false;
        }
        const std::string head = token.substr(0, colon);
        const std::string rest = token.substr(colon + 1);
        if (head == "god" || head == "mortal" || head == "noclip" || head == "clip") {
            switches.push_back(Switch{std::atoi(rest.c_str()), head});
            continue;
        }
        if (head == "hurt") {
            const size_t second = rest.find(':');
            if (second == std::string::npos) {
                *why = "the schedule did not parse: " + token;
                return false;
            }
            hurts.push_back(Hurt{std::atoi(rest.substr(0, second).c_str()),
                                 std::atoi(rest.substr(second + 1).c_str())});
            continue;
        }
        if (head == "yaw") {
            hasYaw = true;
            startYaw = std::strtof(rest.c_str(), nullptr);
            continue;
        }
        bool found = false;
        for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
            if (head == kKeys[k].name) {
                const size_t dash = rest.find('-');
                if (dash == std::string::npos) {
                    *why = "the schedule did not parse: " + token;
                    return false;
                }
                holds.push_back(Hold{kKeys[k].key, std::atoi(rest.substr(0, dash).c_str()),
                                     std::atoi(rest.substr(dash + 1).c_str())});
                found = true;
            }
        }
        if (!found) {
            *why = "the schedule did not parse: " + token;
            return false;
        }
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = hasYaw ? startYaw : levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    *out = "cheats level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < switches.size(); i++) {
            if (switches[i].step != step) {
                continue;
            }
            // LevelCommands.SetGod / SetNoclip (DevTools/LevelCommands.cs:37-39).
            const std::string& what = switches[i].what;
            if (what == "god") {
                player.setMortality(Kurt::God);
            } else if (what == "mortal") {
                player.setMortality(Kurt::Mortal);
            } else if (what == "noclip") {
                player.setClipping(Player::ClipOff);
            } else {
                player.setClipping(Player::ClipOn);
            }
        }
        for (size_t i = 0; i < hurts.size(); i++) {
            if (hurts[i].step == step) {
                player.combat().hurt(hurts[i].damage);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        // Viewer.RunSteps: Kurt, the scripts, then Kurt's solid arenas (Viewer.cs:533).
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        space.setSolid(runtime.solidArenas());
        snprintf(buffer, sizeof(buffer),
                 "s=%d state=%s feet64=%d,%d,%d yaw=%.4f hp=%d floor=%d speeds=%.4f,%.4f,%.4f "
                 "god=%d clip=%d\n",
                 step, kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.yaw, player.health(), player.onFloor ? 1 : 0,
                 player.forwardSpeed, player.strafeSpeed, player.verticalSpeed,
                 player.mortality() == Kurt::God ? 1 : 0,
                 player.clipping() == Player::ClipOn ? 1 : 0);
        *out += buffer;
    }
    return true;
}

// THE SNIPER ROUNDS (golden/L*_rounds_*.txt): Kurt and the scripts runtime stepped together
// at 1/60 s, as kurtItemsDump does, under a schedule that enters sniper mode, aims, fires
// and teleports. The oracle is tests/scripts/rounds_probe (`Probe <level> <steps>
// <schedule>`); see its header for the schedule. Each step prints Kurt's scope (yaw, pitch,
// zoom, the lock's zoom limit, the clip, the ammo), the locked target, the stats, then every
// busy round slot (state, type, position, angles, speeds, life, linger, homing target and
// part, yaw rate, path and time, the camera, the watch, the model's place and basis hash)
// and every object whose hit state changed (hp, hit event/type/direction, shot part/point,
// position). Every float is its bits. Objects are numbered in the order they are first seen
// (the C#'s list drops removed objects, the deque keeps their slots), in the probe's order:
// the target, the rounds' targets, then the list.
struct RoundsHold {
    Key key;
    int from;
    int to;
};
struct RoundsAim {
    int step;
    float yaw;
    float pitch;
};
struct RoundsTeleport {
    int step;
    std::string arena;
    Vec3 at;
};
// `hp:step:ID:HEALTH`: object ID's health set before that step.
struct RoundsHealth {
    int step;
    int id;
    int32_t health;
};

float roundsBits(const std::string& hex) {
    const uint32_t bits = (uint32_t)strtoul(hex.c_str(), nullptr, 16);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

bool parseRoundsSchedule(const std::string& schedule, std::vector<RoundsHold>* holds,
                         std::vector<ItemsGive>* gives, std::vector<RoundsAim>* aims,
                         std::vector<RoundsTeleport>* teleports,
                         std::vector<RoundsHealth>* healths) {
    static const struct {
        const char* name;
        Key key;
    } kKeys[] = {{"snipe", kKeySniper},  {"fire", kKeyFire},    {"next", kKeyItemNext},
                 {"prev", kKeyItemPrevious}, {"zin", kKeyZoomIn}, {"zout", kKeyZoomOut},
                 {"left", kKeyTurnLeft}, {"right", kKeyTurnRight}, {"up", kKeyForward},
                 {"down", kKeyBack}};
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        if (token.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        size_t at = 0;
        while (true) {
            const size_t next = token.find(':', at);
            fields.push_back(token.substr(at, next == std::string::npos ? std::string::npos
                                                                        : next - at));
            if (next == std::string::npos) {
                break;
            }
            at = next + 1;
        }
        if (fields[0] == "give" && fields.size() == 3) {
            ItemsGive give;
            give.step = atoi(fields[1].c_str());
            give.model = fields[2];
            give.drop = false;
            give.offset = Vec3{};
            gives->push_back(give);
            continue;
        }
        if (fields[0] == "aim" && fields.size() == 4) {
            RoundsAim aim;
            aim.step = atoi(fields[1].c_str());
            aim.yaw = roundsBits(fields[2]);
            aim.pitch = roundsBits(fields[3]);
            aims->push_back(aim);
            continue;
        }
        if (fields[0] == "hp" && fields.size() == 4) {
            RoundsHealth health;
            health.step = atoi(fields[1].c_str());
            health.id = atoi(fields[2].c_str());
            health.health = atoi(fields[3].c_str());
            healths->push_back(health);
            continue;
        }
        if (fields[0] == "tp" && fields.size() == 6) {
            RoundsTeleport teleport;
            teleport.step = atoi(fields[1].c_str());
            teleport.arena = fields[2];
            // float.Parse of the probe's decimal text: strtof rounds the same way.
            teleport.at = Vec3{strtof(fields[3].c_str(), nullptr), strtof(fields[4].c_str(), nullptr),
                               strtof(fields[5].c_str(), nullptr)};
            teleports->push_back(teleport);
            continue;
        }
        if (fields.size() != 2) {
            return false;
        }
        bool known = false;
        for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
            if (fields[0] == kKeys[k].name) {
                RoundsHold hold;
                hold.key = kKeys[k].key;
                if (sscanf(fields[1].c_str(), "%d-%d", &hold.from, &hold.to) != 2) {
                    return false;
                }
                holds->push_back(hold);
                known = true;
            }
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

const char* roundStateName(scripts::SniperRounds::State state) {
    static const char* const kNames[] = {"Free", "Flying", "Killed", "Hit", "Dead", "Exploded"};
    return kNames[(int)state];
}

// SNIPER MODE'S SCREEN AND VIEWS (gap package #7; golden/L*_sniper_hud*.txt), the C++ side
// of the rounds probe's `hud` and `iris` modes (tests/scripts/rounds_probe, whose header
// has the columns). The overlay is game/sniper_overlay.cpp loaded through the recorder
// stubs above (texture ids from 1 in load order, palette 7) and drawn into them; the views
// are game/sniper_view.cpp's.
struct SniperHudProbe {
    SniperOverlay overlay;
    float panelX = 0.0f;
    float panelY = 0.0f;
    float panelW = 0.0f;
    float panelH = 0.0f;
};

bool loadSniperHudProbe(const LevelData& levelData, const Bni& sprites, SniperHudProbe* probe,
                        std::string* why) {
    g_indexTextures = 0;
    if (!probe->overlay.load(nullptr, sprites, levelData.dti().palette(), 7)) {
        *why = "the sniper overlay did not load";
        return false;
    }
    Texture panel;
    if (!sprites.image("SC_STAT", &panel)) {
        *why = "no SC_STAT";
        return false;
    }
    // HudView.Draw's panel in the view: the normal HUD's place.
    probe->panelW = (float)panel.width();
    probe->panelH = (float)panel.height();
    probe->panelX = SniperOverlay::kViewWidth - (probe->panelW + 16.0f);
    probe->panelY = SniperOverlay::kViewHeight - (probe->panelH + 10.0f);
    return true;
}

uint32_t fnvMatrix(uint32_t hash, const float m[16]) {
    for (int i = 0; i < 16; i++) {
        hash = fnvFloat(hash, m[i]);
    }
    return hash;
}

// The probe's OverlayLine: the state, then Draw's quads (count and hash; all when `full`).
std::string sniperOverlayLine(SniperHudProbe* probe, float width, int health, bool full) {
    std::vector<CanvasQuad> quads;
    g_canvasQuads = &quads;
    probe->overlay.draw(nullptr, width, health, Blink::Shown, probe->panelX, probe->panelY,
                        probe->panelW, probe->panelH);
    g_canvasQuads = nullptr;
    uint32_t hash = 2166136261u;
    std::string list;
    for (size_t i = 0; i < quads.size(); i++) {
        const CanvasQuad& q = quads[i];
        const float values[12] = {q.colour.r, q.colour.g, q.colour.b, q.colour.a,
                                  q.x0, q.y0, q.x2, q.y2, q.u0, q.v0, q.u2, q.v2};
        hash = fnvInt(fnvInt(hash, (uint32_t)q.texture), (uint32_t)q.palette);
        for (int v = 0; v < 12; v++) {
            hash = fnvFloat(hash, values[v]);
        }
        if (full) {
            list += "\n    q" + std::to_string(i) + " " + std::to_string(q.texture) + " " +
                    std::to_string(q.palette) + " ";
            for (int v = 0; v < 12; v++) {
                list += (v > 0 ? "," : "") + hexBits(values[v]);
            }
        }
    }
    char line[256];
    snprintf(line, sizeof(line), "vis=%d iris=%s pulse=%s tgt=%d gauge=%d quads=%d hash=%08X",
             probe->overlay.visible() ? 1 : 0, hexBits(probe->overlay.iris()).c_str(),
             hexBits(probe->overlay.pulse()).c_str(), probe->overlay.target() ? 1 : 0,
             (int)probe->overlay.gaugeShown(), (int)quads.size(), hash);
    return line + list;
}

// `tick1 dump sniperiris <level> <steps>`: the probe's `iris` mode.
bool sniperIrisDump(int level, int steps, const MdkData& data, const Bni* sprites,
                    std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI";
        return false;
    }
    LevelData levelData;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    SniperHudProbe probe;
    if (!loadSniperHudProbe(levelData, *sprites, &probe, why)) {
        return false;
    }
    *out = "iris level=" + std::to_string(level) + " steps=" + std::to_string(steps) + "\n";
    char buffer[256];
    for (int step = 1; step <= steps; step++) {
        const int32_t selected = step < 40 ? 0 : 5;
        const StrikeAim aim = step < 50 ? StrikeAim::NotChecked
                              : step <= 150 ? StrikeAim::Target
                              : step <= 170 ? StrikeAim::None
                              : step <= 260 ? StrikeAim::Target
                                            : StrikeAim::NotChecked;
        const float zoom = step < 150 ? 1.0f - (float)step * 0.0055f
                                      : 0.2f + (float)(step - 150) * 0.004f;
        SniperHud hud;
        hud.zoom = zoom;
        const int32_t ammo[5] = {0, 4, 0, 2, 3};
        for (int i = 0; i < 5; i++) {
            hud.ammo[i] = ammo[i];
        }
        hud.selectedAmmo = selected;
        hud.cameras[0] = scripts::SniperRounds::RoundCamera{scripts::SniperRounds::ShotEmpty,
                                                            Vec3{}, Vec3{}, 0.0f};
        hud.cameras[1] = scripts::SniperRounds::RoundCamera{scripts::SniperRounds::ShotMiss,
                                                            Vec3{}, Vec3{}, (float)step * 0.5f};
        hud.cameras[2] = scripts::SniperRounds::RoundCamera{
            step % 90 < 45 ? scripts::SniperRounds::ShotHit : scripts::SniperRounds::ShotKill,
            Vec3{}, Vec3{}, 0.0f};
        hud.cameraCount = 3;
        hud.aim = aim;
        probe.overlay.update(1.0f / 60.0f, &hud);
        snprintf(buffer, sizeof(buffer), "s=%d zoom=%s sel=%d aim=%d pct=%d ", step,
                 hexBits(zoom).c_str(), (int)selected, (int)aim,
                 (int)SniperOverlay::zoomPercent(zoom));
        *out += buffer +
                sniperOverlayLine(&probe, 480.0f, 37, step % 60 == 0 || step == 100 || step == 200) +
                "\n";
    }
    return true;
}

// The probe's `hud` lines for one step of kurtRoundsDump (SniperView.Draw's half).
void sniperHudStep(SniperHudProbe* probe, const Player& player, scripts::ScriptRuntime& runtime,
                   int step, std::string* out) {
    const Scope& scope = player.scope();
    if (!player.sniping()) {
        probe->overlay.update(1.0f / 60.0f, nullptr);
    } else {
        const SniperHud hud = SniperView::hudOf(player, &runtime);
        probe->overlay.update(1.0f / 60.0f, &hud);
    }
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "s=%d snipe=%d zoom=%s clip=%d/%s sel=%d ", step,
             player.sniping() ? 1 : 0, hexBits(scope.zoom).c_str(), (int)scope.clipRounds,
             hexBits(scope.clipTime).c_str(), (int)player.combat().selectedAmmo());
    *out += buffer +
            sniperOverlayLine(probe, 600.0f, player.health(), player.sniping() && step % 60 == 0) +
            "\n";
    if (!player.sniping()) {
        return;
    }
    const auto vec = [](const Vec3& v) {
        return hexBits(v.x) + "," + hexBits(v.y) + "," + hexBits(v.z);
    };
    const auto rect = [](const SniperOverlay::Rect& r) {
        return hexBits(r.x) + "," + hexBits(r.y) + "," + hexBits(r.w) + "," + hexBits(r.h);
    };
    float scale = 0.0f;
    float left = 0.0f;
    SniperOverlay::placement(600.0f, &scale, &left);
    for (int32_t i = 0; i < scripts::SniperRounds::kSlots; i++) {
        const scripts::SniperRounds::RoundCamera camera = runtime.sniperRounds().camera(i);
        snprintf(buffer, sizeof(buffer), "  cam%d shot=%d time=%s", (int)i, (int)camera.shot,
                 hexBits(camera.time).c_str());
        *out += buffer;
        if (camera.shot == scripts::SniperRounds::ShotWatching) {
            const SniperOverlay::Rect area =
                SniperView::onCanvas(SniperOverlay::kRoundViews[i], scale, left);
            Camera view;
            SniperView::roundCameraView(&view, camera, area);
            Mat4 v;
            Mat4 pr;
            memcpy(v.m, view.view, sizeof(v.m));
            memcpy(pr.m, view.projection, sizeof(pr.m));
            const Mat4 product = multiply(v, pr);
            snprintf(buffer, sizeof(buffer), " area=%s at=%s view=%08X proj=%08X",
                     rect(area).c_str(), vec(camera.position).c_str(),
                     fnvMatrix(2166136261u, product.m), fnvMatrix(2166136261u, pr.m));
            *out += buffer;
        }
        *out += "\n";
    }
    if (runtime.findModel(runtime.currentArena(),
                          SniperView::roundModel(player.combat().selectedAmmo())) == nullptr) {
        return;
    }
    const SniperOverlay::Rect whole = {0.0f, 0.0f, SniperOverlay::kViewWidth,
                                       SniperOverlay::kViewHeight};
    const SniperOverlay::Rect clipArea = SniperView::onCanvas(whole, scale, left);
    bool shown = false;
    for (int32_t i = 0; i < Scope::kClipSize; i++) {
        const float t = scope.clipTime + (float)i;
        if (i >= scope.clipRounds || t == 0.0f || t >= 3.0f) {
            continue;
        }
        shown = true;
        Mat4 rotation;
        Vec3 position;
        float roundScale = 0.0f;
        SniperView::clipPlace(t, &rotation, &position, &roundScale);
        snprintf(buffer, sizeof(buffer), "  clip%d t=%s rot=%08X at=%s scale=%s\n", (int)i,
                 hexBits(t).c_str(), fnvMatrix(2166136261u, rotation.m), vec(position).c_str(),
                 hexBits(roundScale).c_str());
        *out += buffer;
    }
    if (shown) {
        Camera view;
        SniperView::clipView(&view, clipArea);
        Mat4 v;
        Mat4 pr;
        memcpy(v.m, view.view, sizeof(v.m));
        memcpy(pr.m, view.projection, sizeof(pr.m));
        const Mat4 product = multiply(v, pr);
        snprintf(buffer, sizeof(buffer), "  clipview area=%s view=%08X proj=%08X\n",
                 rect(clipArea).c_str(), fnvMatrix(2166136261u, product.m),
                 fnvMatrix(2166136261u, pr.m));
        *out += buffer;
    }
}

bool kurtRoundsDump(int level, int steps, const std::string& schedule, const MdkData& data,
                    const Bni* sprites, std::string* out, std::string* why, bool hud = false) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<RoundsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<RoundsAim> aims;
    std::vector<RoundsTeleport> teleports;
    std::vector<RoundsHealth> healths;
    if (!parseRoundsSchedule(schedule, &holds, &gives, &aims, &teleports, &healths)) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    // Yaw 0, as the probe: the LUT's facing() is exact there (see the probe's header).
    player.yaw = 0.0f;
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    std::map<const scripts::MdkObject*, int> ids;
    const auto id = [&ids](const scripts::MdkObject* obj) {
        if (obj == nullptr) {
            return -1;
        }
        std::map<const scripts::MdkObject*, int>::iterator found = ids.find(obj);
        if (found != ids.end()) {
            return found->second;
        }
        const int next = (int)ids.size();
        ids[obj] = next;
        return next;
    };
    const auto vec = [](const Vec3& v) {
        return hexBits(v.x) + "," + hexBits(v.y) + "," + hexBits(v.z);
    };
    std::map<const scripts::MdkObject*, std::string> seen;
    SniperHudProbe hudProbe;
    if (hud && !loadSniperHudProbe(levelData, *sprites, &hudProbe, why)) {
        return false;
    }
    *out = std::string(hud ? "hud " : "") + "level=" + std::to_string(level) +
           " steps=" + std::to_string(steps) + " seed=1 schedule=" + schedule + "\n";
    char buffer[768];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step) {
                player.collect(gives[i].model);
            }
        }
        for (size_t i = 0; i < healths.size(); i++) {
            if (healths[i].step != step) {
                continue;
            }
            // The deque's object with that number (the map holds const pointers into it).
            const std::deque<scripts::MdkObject>& list = runtime.objects();
            for (size_t o = 0; o < list.size(); o++) {
                std::map<const scripts::MdkObject*, int>::iterator found = ids.find(&list[o]);
                if (found != ids.end() && found->second == healths[i].id) {
                    runtime.objects()[o].health = healths[i].health;
                }
            }
        }
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, 0.0f);
            }
        }
        for (size_t i = 0; i < aims.size(); i++) {
            if (aims[i].step == step) {
                player.yaw = aims[i].yaw;
                player.scope().pitch = aims[i].pitch;
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        // In `hud` mode the rounds' lines are made but not kept: they number the objects
        // (id), which the schedule's `hp:` refers to.
        std::string discarded;
        std::string* lines = out;
        if (hud) {
            sniperHudStep(&hudProbe, player, runtime, step, out);
            lines = &discarded;
        }
        const Kurt& combat = player.combat();
        const Scope& scope = player.scope();
        snprintf(buffer, sizeof(buffer),
                 "s=%d snipe=%d yaw=%s pitch=%s zoom=%s limit=%s clip=%d/%s sel=%d "
                 "ammo=%d,%d,%d,%d,%d target=%d hp=%d shots=%d hits=%d kills=%d objs=%d "
                 "effects=%d pieces=%d\n",
                 step, player.sniping() ? 1 : 0, hexBits(player.yaw).c_str(),
                 hexBits(scope.pitch).c_str(), hexBits(scope.zoom).c_str(),
                 hexBits(scope.zoomLimit).c_str(), scope.clipRounds, hexBits(scope.clipTime).c_str(),
                 combat.selectedAmmo(), combat.ammo(1), combat.ammo(2), combat.ammo(3),
                 combat.ammo(4), combat.ammo(5), id(runtime.sniperTarget), player.health(),
                 runtime.stats.sniperShots, runtime.stats.sniperHits, runtime.stats.kills,
                 runtime.objectCount(), runtime.effects.count(), runtime.debris().pieceCount());
        *lines += buffer;
        const scripts::SniperRounds& rounds = runtime.sniperRounds();
        for (int32_t i = 0; i < scripts::SniperRounds::kSlots; i++) {
            const scripts::SniperRounds::Round& r = rounds.round(i);
            if (r.state == scripts::SniperRounds::Free) {
                continue;
            }
            std::string visual = "-";
            if (r.hasVisual) {
                uint32_t basis = 2166136261u;
                for (int m = 0; m < 16; m++) {
                    basis = fnvFloat(basis, r.visual.rolling_basis.m[m]);
                }
                snprintf(buffer, sizeof(buffer), "%s:%s:%08X", r.visual.type_name.c_str(),
                         vec(r.visual.position).c_str(), basis);
                visual = buffer;
            }
            snprintf(buffer, sizeof(buffer),
                     "  r%d %s t=%d p=%s yaw=%s pitch=%s roll=%s v=%s,%s life=%s linger=%s "
                     "tgt=%d/%d yr=%s path=%d/%s cam=%s:%s watch=%s vis=%s\n",
                     (int)i, roundStateName(r.state), (int)r.type, vec(r.position).c_str(),
                     hexBits(r.yaw).c_str(), hexBits(r.pitch).c_str(), hexBits(r.roll).c_str(),
                     hexBits(r.speed).c_str(), hexBits(r.vertical).c_str(), hexBits(r.life).c_str(),
                     hexBits(r.linger).c_str(), id(r.target), (int)r.targetPart,
                     hexBits(r.yawRate).c_str(), (int)r.path, hexBits(r.pathTime).c_str(),
                     hexBits(r.cameraDistance).c_str(), vec(r.cameraPosition).c_str(),
                     hexBits(r.watch).c_str(), visual.c_str());
            *lines += buffer;
        }
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (o.dead) {
                continue;
            }
            const int objectId = id(&o);
            snprintf(buffer, sizeof(buffer), "hp=%d ht=%d hd=%s sp=%d pt=%s", (int)o.health,
                     (int)o.hit_type, hexBits(o.hit_direction).c_str(), (int)o.shot_part,
                     vec(o.shot_point).c_str());
            const std::string hitText = buffer;
            std::map<const scripts::MdkObject*, std::string>::iterator before = seen.find(&o);
            if (before == seen.end()) {
                seen[&o] = hitText;
                continue;
            }
            if (before->second != hitText) {
                before->second = hitText;
                snprintf(buffer, sizeof(buffer),
                         "  #%d %s hp=%d ev=%d ht=%d hd=%s sp=%d pt=%s pos=%s\n", objectId,
                         o.type_name.c_str(), (int)o.health, (int)o.hit_event, (int)o.hit_type,
                         hexBits(o.hit_direction).c_str(), (int)o.shot_part,
                         vec(o.shot_point).c_str(), vec(o.position).c_str());
                *lines += buffer;
            }
        }
    }
    return true;
}

// THE BULLET HOLES (golden/L*_holes_*.txt; PORT_GAPS package #16). The sniper rounds dump's
// stepping (Kurt then the scripts at 1/60 s, the rounds probe's schedule) with special_130 in
// view: every texture of the level is copied when the level loads, and on each stamp (the
// runtime's TextureStamped hook, raised after EACH stamp as the C# event is) the texture is
// compared with its copy and the change printed -- the FNV-1a of its indices before and after,
// the changed texels' bounds and count -- then the copy is brought up to date. The oracle is
// tests/scripts/holes_probe (`Probe <level> <steps> <schedule>`); its schedule adds
// `opt:step:N` (the runtime's option: 0 stamps BHOLE2) and `stampall:step:ID` (stampBulletHole
// on object ID once per part, shot_part that part + 1, shot_point its partCenter). A step is
// printed only when its shots or hits changed, a stamp happened, or an object's hit state did.
struct HolesStamps {
    std::map<const formats::Texture*, std::vector<uint8_t> > copies;
    std::vector<std::string> lines;
};

uint32_t holesFnv(const uint8_t* bytes, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

void holesStamped(void* user, const formats::Texture* texture) {
    HolesStamps* stamps = static_cast<HolesStamps*>(user);
    std::vector<uint8_t>& before = stamps->copies[texture];
    const uint8_t* after = texture->indices();
    const size_t count = (size_t)texture->indexCount();
    int x0 = 0x7FFFFFFF;
    int y0 = 0x7FFFFFFF;
    int x1 = -1;
    int y1 = -1;
    int changed = 0;
    for (size_t i = 0; i < count && i < before.size(); i++) {
        if (after[i] == before[i]) {
            continue;
        }
        changed++;
        const int x = (int)(i % (size_t)texture->width());
        const int y = (int)(i / (size_t)texture->width());
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    char rect[64];
    if (changed == 0) {
        snprintf(rect, sizeof(rect), "-");
    } else {
        snprintf(rect, sizeof(rect), "%d,%d,%d,%d", x0, y0, x1, y1);
    }
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "  stamp tex=%s %dx%dx%d before=%08X after=%08X rect=%s n=%d",
             texture->name().c_str(), (int)texture->width(), (int)texture->height(),
             (int)texture->frameCount(), holesFnv(before.data(), before.size()),
             holesFnv(after, count), rect, changed);
    stamps->lines.push_back(buffer);
    before.assign(after, after + count);
}

bool bulletHolesDump(int level, int steps, const std::string& schedule, const MdkData& data,
                     const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    // The holes' own tokens out first; the rest is the rounds probe's.
    std::vector<std::pair<int, int> > options;
    std::vector<std::pair<int, int> > stampAlls;
    std::string rest;
    {
        size_t start = 0;
        while (start < schedule.size()) {
            size_t end = schedule.find(',', start);
            if (end == std::string::npos) {
                end = schedule.size();
            }
            const std::string token = schedule.substr(start, end - start);
            start = end + 1;
            if (token.compare(0, 4, "opt:") == 0 || token.compare(0, 9, "stampall:") == 0) {
                const size_t first = token.find(':');
                const size_t second = token.find(':', first + 1);
                if (second == std::string::npos) {
                    *why = "the schedule did not parse: " + token;
                    return false;
                }
                const std::pair<int, int> entry(atoi(token.substr(first + 1).c_str()),
                                                atoi(token.substr(second + 1).c_str()));
                (token[0] == 'o' ? options : stampAlls).push_back(entry);
            } else if (!token.empty()) {
                rest += (rest.empty() ? "" : ",") + token;
            }
        }
    }
    std::vector<RoundsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<RoundsAim> aims;
    std::vector<RoundsTeleport> teleports;
    std::vector<RoundsHealth> healths;
    if (!parseRoundsSchedule(rest, &holds, &gives, &aims, &teleports, &healths)) {
        *why = "the schedule did not parse: " + schedule;
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
    // Every texture of the level as it loaded: the level's archive, then each arena's.
    HolesStamps stamps;
    {
        std::vector<const formats::TextureArchive*> archives;
        archives.push_back(&levelData.levelTextures());
        for (size_t i = 0; i < arenas.size(); i++) {
            archives.push_back(&arenas[i]->textures());
        }
        for (size_t a = 0; a < archives.size(); a++) {
            const std::vector<std::pair<std::string, formats::Texture> >& textures =
                archives[a]->textures();
            for (size_t t = 0; t < textures.size(); t++) {
                const formats::Texture* texture = &textures[t].second;
                if (stamps.copies.find(texture) == stamps.copies.end()) {
                    const uint8_t* indices = texture->indices();
                    stamps.copies[texture].assign(indices, indices + texture->indexCount());
                }
            }
        }
    }
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = 0.0f;
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    runtime.setTextureStampedHook(holesStamped, &stamps);

    std::map<const scripts::MdkObject*, int> ids;
    const auto id = [&ids](const scripts::MdkObject* obj) {
        if (obj == nullptr) {
            return -1;
        }
        std::map<const scripts::MdkObject*, int>::iterator found = ids.find(obj);
        if (found != ids.end()) {
            return found->second;
        }
        const int next = (int)ids.size();
        ids[obj] = next;
        return next;
    };
    const auto objectNumbered = [&ids, &runtime](int wanted) -> scripts::MdkObject* {
        std::deque<scripts::MdkObject>& list = runtime.objects();
        for (size_t o = 0; o < list.size(); o++) {
            std::map<const scripts::MdkObject*, int>::iterator found = ids.find(&list[o]);
            if (found != ids.end() && found->second == wanted) {
                return &list[o];
            }
        }
        return nullptr;
    };
    const auto vec = [](const Vec3& v) {
        return hexBits(v.x) + "," + hexBits(v.y) + "," + hexBits(v.z);
    };
    std::map<const scripts::MdkObject*, std::string> seen;
    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    int lastShots = -1;
    int lastHits = -1;
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step) {
                player.collect(gives[i].model);
            }
        }
        for (size_t i = 0; i < healths.size(); i++) {
            if (healths[i].step == step) {
                scripts::MdkObject* obj = objectNumbered(healths[i].id);
                if (obj) {
                    obj->health = healths[i].health;
                }
            }
        }
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, 0.0f);
            }
        }
        for (size_t i = 0; i < aims.size(); i++) {
            if (aims[i].step == step) {
                player.yaw = aims[i].yaw;
                player.scope().pitch = aims[i].pitch;
            }
        }
        for (size_t i = 0; i < options.size(); i++) {
            if (options[i].first == step) {
                runtime.option = options[i].second;
            }
        }
        for (size_t i = 0; i < stampAlls.size(); i++) {
            if (stampAlls[i].first != step) {
                continue;
            }
            scripts::MdkObject* obj = objectNumbered(stampAlls[i].second);
            const int parts = obj && obj->model ? (int)obj->model->partList.size() : 0;
            for (int part = 0; part < parts; part++) {
                obj->shot_part = part + 1;
                obj->shot_point = obj->partCenter(part);
                snprintf(buffer, sizeof(buffer), "  direct #%d %s part=%d pt=%s",
                         stampAlls[i].second, obj->type_name.c_str(), part,
                         vec(obj->shot_point).c_str());
                stamps.lines.push_back(buffer);
                runtime.stampBulletHole(obj);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        // Number every object the step made, in list order (the probe's Id pass).
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i].dead) {
                id(&objects[i]);
            }
        }
        std::vector<std::string> changes;
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (o.dead) {
                continue;
            }
            snprintf(buffer, sizeof(buffer), "hp=%d ht=%d sp=%d pt=%s", (int)o.health,
                     (int)o.hit_type, (int)o.shot_part, vec(o.shot_point).c_str());
            const std::string hitText = buffer;
            std::map<const scripts::MdkObject*, std::string>::iterator before = seen.find(&o);
            if (before == seen.end()) {
                seen[&o] = hitText;
                continue;
            }
            if (before->second != hitText) {
                before->second = hitText;
                snprintf(buffer, sizeof(buffer), "  #%d %s %s pos=%s yaw=%s", id(&o),
                         o.type_name.c_str(), hitText.c_str(), vec(o.position).c_str(),
                         hexBits(o.yaw).c_str());
                changes.push_back(buffer);
            }
        }
        const int shots = runtime.stats.sniperShots;
        const int hits = runtime.stats.sniperHits;
        if (shots != lastShots || hits != lastHits || !stamps.lines.empty() || !changes.empty()) {
            snprintf(buffer, sizeof(buffer), "s=%d arena=%s snipe=%d shots=%d hits=%d objs=%d\n",
                     step, runtime.currentArena().c_str(), player.sniping() ? 1 : 0, shots, hits,
                     runtime.objectCount());
            *out += buffer;
            lastShots = shots;
            lastHits = hits;
            for (size_t i = 0; i < stamps.lines.size(); i++) {
                *out += stamps.lines[i] + "\n";
            }
            for (size_t i = 0; i < changes.size(); i++) {
                *out += changes[i] + "\n";
            }
            stamps.lines.clear();
        }
        // The frontends' list: taken every step, as main.cpp / main_3ds.cpp do.
        runtime.clearStampedTextures();
    }
    return true;
}

// KURT THROUGH A DOORWAY AND DOWN A CORRIDOR (golden/L7_corridor_*.txt). Kurt and the
// scripts runtime stepped together at 1/60 s from a placed start (the frontends' --at), as
// Viewer.RunSteps does: Kurt, then the scripts, then ArenaSpace::setSolid(solidArenas()) --
// the step kurtItemsDump leaves out, and the one this dump is about: which arenas Kurt's next
// move collides with. The oracle is tests/scripts/corridor_probe (`Probe <level> <steps>
// <x,y,z,yaw> <schedule>`). The schedule is the items probe's key holds plus `unlock:step`,
// which clears every door's locked bit (0x40) before that step, the same way on both sides.
// Level 7's DANT_1 -> CDANT_1 door starts locked, and in the oracle a locked door is solid
// to Kurt (ScriptRuntime.SolidsWithin, Collision/Solids.cs), so a key-only run never reaches
// the corridor there; this port does not collide Kurt with objects yet, so without the unlock
// he walks through the closed door and the door never opens or closes (docs/OPEN_ITEMS.md,
// "Kurt walks through closed doors"). Per step:
//
//   arena / second   currentArena(), secondArena() and `+` when it is active: the door's
//                    ShowBehind, the doorway crossing (CrossedArena), the door closing
//                    behind him (ShowArena("")), the corridor's load triggers (PreloadArena).
//   solid            solidArenas(): what his NEXT move collides with.
//   feet             where he stands, to the unit (the oracle's facing is libm, this port's
//                    the trig LUT: a diagonal walk differs by a 64th now and then).
//   wall             the walk move he meant this step (facing * forwardSpeed + right *
//                    strafeSpeed, over the step) swept again from where he stood against the
//                    same solid list: the arena and triangle it ran into, or `-`.
bool kurtCorridorDump(int level, int steps, const std::string& at, const std::string& schedule,
                      const MdkData& data, const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    float place[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int placed = 0;
    {
        size_t start = 0;
        while (placed < 4 && start <= at.size()) {
            size_t end = at.find(',', start);
            if (end == std::string::npos) {
                end = at.size();
            }
            place[placed++] = (float)atof(at.substr(start, end - start).c_str());
            start = end + 1;
        }
    }
    if (placed < 3) {
        *why = "the start did not parse (want x,y,z[,yaw]): " + at;
        return false;
    }
    // The unlocks come out of the schedule first; the rest is the items probe's.
    std::vector<int> unlocks;
    std::string keys;
    {
        size_t start = 0;
        while (start < schedule.size()) {
            size_t end = schedule.find(',', start);
            if (end == std::string::npos) {
                end = schedule.size();
            }
            const std::string token = schedule.substr(start, end - start);
            start = end + 1;
            if (token.compare(0, 7, "unlock:") == 0) {
                unlocks.push_back(atoi(token.substr(7).c_str()));
            } else if (!token.empty()) {
                keys += (keys.empty() ? "" : ",") + token;
            }
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(keys, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    Player player;
    player.feet = Vec3{place[0], place[1], place[2]};
    player.yaw = placed > 3 ? place[3] : levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    // Kurt's walk slide (C# Kurt.WalkSlide): the replayed move slides as his did.
    const float kReplaySlide = 0.75f;
    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) + " at=" + at +
           " schedule=" + schedule + "\n";
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < unlocks.size(); i++) {
            if (unlocks[i] != step) {
                continue;
            }
            std::deque<scripts::MdkObject>& objects = runtime.objects();
            for (size_t j = 0; j < objects.size(); j++) {
                if (!objects[j].dead && (objects[j].flags & scripts::MdkObject::FlagDoor) != 0) {
                    objects[j].door_state &= ~0x40;
                }
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        const Vec3 before = player.feet;
        player.update(input, 1.0f / 60.0f);
        const Vec3 f = player.facing();
        const Vec3 r = player.right();
        const float step60 = 1.0f / 60.0f;
        const Vec meant{(f.x * player.forwardSpeed + r.x * player.strafeSpeed) * step60,
                                    (f.y * player.forwardSpeed + r.y * player.strafeSpeed) * step60,
                                    0.0f};
        std::string wall = "-";
        if (meant.x != 0.0f || meant.y != 0.0f) {
            const ArenaSpace::Result replay =
                space.move(Vec{before.x, before.y, before.z}, meant,
                           ArenaSpace::kMotionWalk, kReplaySlide);
            if (replay.hit() && replay.bsp != nullptr) {
                snprintf(buffer, sizeof(buffer), "%s:%d", replay.bsp->arena()->name().c_str(),
                         replay.triangle);
                wall = buffer;
            }
        }
        runtime.update(1.0f / 60.0f);
        space.setSolid(runtime.solidArenas());
        const std::vector<std::string>& solid = runtime.solidArenas();
        std::string solidText;
        for (size_t i = 0; i < solid.size(); i++) {
            solidText += (i ? "|" : "") + solid[i];
        }
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s second=%s%s solid=%s feet=%.0f,%.0f,%.0f yaw=%.1f wall=%s\n",
                 step, runtime.currentArena().empty() ? "-" : runtime.currentArena().c_str(),
                 runtime.secondArena().empty() ? "-" : runtime.secondArena().c_str(),
                 runtime.secondActive() ? "+" : "", solidText.empty() ? "-" : solidText.c_str(),
                 player.feet.x, player.feet.y, player.feet.z, player.yaw, wall.c_str());
        *out += buffer;
    }
    return true;
}

// THE SCREEN (golden/L*_screen_*.txt; PORT_GAPS package #15) against the oracle probe
// tests/scripts/screen_probe: the C++ Player, ScriptRuntime and FollowCamera stepped at 1/60 s
// in the C# Viewer's order (Kurt, the scripts, the camera), the camera shaken by the scripts
// (ShakeRaised) and seeded as the probe seeds the C#'s (`camseed:N`, default 1), the HUD's
// message stream bound. A line for every step whose state differs from the last printed one
// (Kurt's arena, the town timer, the global flags, the sky mode with its backdrop and clear
// colour, the runtime's shake, the camera's shake and offset), then the step's events: each
// ShakeRaised (`raise`), each group whose state or material changed (`group`, the level's
// arenas in order, groups ascending), the message queue when it changed (`msg`). The schedule:
// key holds (fwd, back, left, right, jump, fire), `diff:N` (before the runtime), `camseed:N`,
// and before a step `tp:step:ARENA:x:y:z`, `town:step:N`, `gflag:step:HEX`, `hp:step:ID:N`,
// `kill:step:ID`, `avar:step:ARENA:I:V` (the probe's own `list:step` is not a golden token).
struct ScreenRaised {
    std::vector<float> amounts;
};

void screenShakeRaised(void* user, float amount) {
    static_cast<ScreenRaised*>(user)->amounts.push_back(amount);
}

// Both of the C#'s ShakeRaised subscribers, in the order Viewer and the probe add them: the
// camera, then the probe's record.
struct ScreenShakeFanout {
    FollowCamera* camera;
    ScreenRaised* raised;
};

void screenShakeFanout(void* user, float amount) {
    ScreenShakeFanout* fanout = static_cast<ScreenShakeFanout*>(user);
    FollowCamera::raiseShakeHook(fanout->camera, amount);
    screenShakeRaised(fanout->raised, amount);
}

std::string screenMessage(const Messages::Message& m) {
    std::string text = std::to_string(m.flags) + ":";
    for (int32_t line = 0; line < m.lineCount; line++) {
        if (line) {
            text += "/";
        }
        for (int32_t i = 0; i < m.length[line]; i++) {
            const uint8_t b = m.text[line][i];
            if (b >= 32 && b <= 126) {
                text += (char)b;
            } else {
                char escape[8];
                snprintf(escape, sizeof(escape), "\\x%02X", b);
                text += escape;
            }
        }
    }
    return text + "#" + hexBits(m.time);
}

bool screenDump(int level, int steps, const std::string& schedule, const MdkData& data,
                const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    struct Hold {
        Key key;
        int from;
        int to;
    };
    struct StepInt {
        int step;
        int a;
        int b;
    };
    struct StepArena {
        int step;
        std::string arena;
        Vec3 at;
        int index;
        float value;
    };
    static const struct {
        const char* name;
        Key key;
    } kKeys[] = {{"fwd", kKeyForward},     {"back", kKeyBack}, {"left", kKeyTurnLeft},
                 {"right", kKeyTurnRight}, {"jump", kKeyJump}, {"fire", kKeyFire}};
    std::vector<Hold> holds;
    std::vector<StepArena> teleports;
    std::vector<StepInt> towns;
    std::vector<StepInt> gflags;
    std::vector<StepInt> healths;
    std::vector<StepInt> kills;
    std::vector<StepArena> avars;
    int difficulty = (int)Kurt::Normal;
    int camSeed = 1;
    {
        size_t start = 0;
        while (start < schedule.size()) {
            size_t end = schedule.find(',', start);
            if (end == std::string::npos) {
                end = schedule.size();
            }
            const std::string token = schedule.substr(start, end - start);
            start = end + 1;
            if (token.empty()) {
                continue;
            }
            std::vector<std::string> parts;
            size_t at = 0;
            while (true) {
                const size_t next = token.find(':', at);
                parts.push_back(token.substr(at, next == std::string::npos ? std::string::npos
                                                                         : next - at));
                if (next == std::string::npos) {
                    break;
                }
                at = next + 1;
            }
            const std::string& head = parts[0];
            bool ok = true;
            if (head == "diff" && parts.size() == 2) {
                difficulty = atoi(parts[1].c_str());
            } else if (head == "camseed" && parts.size() == 2) {
                camSeed = atoi(parts[1].c_str());
            } else if (head == "tp" && parts.size() == 6) {
                StepArena t;
                t.step = atoi(parts[1].c_str());
                t.arena = parts[2];
                t.at = Vec3{(float)atof(parts[3].c_str()), (float)atof(parts[4].c_str()),
                            (float)atof(parts[5].c_str())};
                t.index = 0;
                t.value = 0.0f;
                teleports.push_back(t);
            } else if (head == "town" && parts.size() == 3) {
                towns.push_back(StepInt{atoi(parts[1].c_str()), atoi(parts[2].c_str()), 0});
            } else if (head == "gflag" && parts.size() == 3) {
                gflags.push_back(StepInt{atoi(parts[1].c_str()),
                                         (int)(uint32_t)strtoul(parts[2].c_str(), nullptr, 16), 0});
            } else if (head == "hp" && parts.size() == 4) {
                healths.push_back(StepInt{atoi(parts[1].c_str()), atoi(parts[2].c_str()),
                                          atoi(parts[3].c_str())});
            } else if (head == "kill" && parts.size() == 3) {
                kills.push_back(StepInt{atoi(parts[1].c_str()), atoi(parts[2].c_str()), 0});
            } else if (head == "avar" && parts.size() == 5) {
                StepArena v;
                v.step = atoi(parts[1].c_str());
                v.arena = parts[2];
                v.at = Vec3{0.0f, 0.0f, 0.0f};
                v.index = atoi(parts[3].c_str());
                v.value = (float)atof(parts[4].c_str());
                avars.push_back(v);
            } else if (parts.size() == 2) {
                ok = false;
                const size_t dash = parts[1].find('-');
                for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
                    if (head == kKeys[k].name && dash != std::string::npos) {
                        holds.push_back(Hold{kKeys[k].key, atoi(parts[1].substr(0, dash).c_str()),
                                             atoi(parts[1].substr(dash + 1).c_str())});
                        ok = true;
                    }
                }
            } else {
                ok = false;
            }
            if (!ok) {
                *why = "the schedule did not parse: " + token;
                return false;
            }
        }
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
    std::vector<uint8_t> ftiBytes;
    Fti fti;
    if (!data.read("MISC/MDKFONT.FTI", &ftiBytes) ||
        !Fti::parse(&fti, ftiBytes.data(), (int32_t)ftiBytes.size())) {
        *why = "MDKFONT.FTI did not load";
        return false;
    }
    const std::vector<const Arena*>& arenas = levelData.arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (levelData.isReachable(arenas[i]->name())) {
            space.add(arenas[i]);
        }
    }
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    // Viewer.cs:232: the difficulty decides the town's timer, before the scripts.
    player.combat().setDifficulty((Kurt::Difficulty)difficulty);
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    // Viewer.cs:237-238: the follow camera over the space, shaken by the scripts.
    FollowCamera follow;
    follow.seedShake(camSeed);
    ScreenRaised raised;
    ScreenShakeFanout fanout{&follow, &raised};
    runtime.setShakeRaisedHook(screenShakeFanout, &fanout);
    // The HUD's message stream (Viewer.cs:245).
    Messages messages;
    messages.setTexts(fti);
    runtime.hudMessages = &messages;
    Camera camera;

    const auto clearText = [](uint32_t argb) {
        char text[16];
        snprintf(text, sizeof(text), "%02X%02X%02X%02X", (unsigned)((argb >> 16) & 0xFFu),
                 (unsigned)((argb >> 8) & 0xFFu), (unsigned)(argb & 0xFFu),
                 (unsigned)(argb >> 24));
        return std::string(text);
    };
    const char* const kBackdrops[] = {"Sky", "Clear", "Keep"};
    const uint32_t skyColour = sky_modes::skyColour(levelData.dti());

    // Objects numbered in the order first seen (the probe's Id pass).
    std::map<const scripts::MdkObject*, int> ids;
    const auto id = [&ids](const scripts::MdkObject* obj) {
        std::map<const scripts::MdkObject*, int>::iterator found = ids.find(obj);
        if (found != ids.end()) {
            return found->second;
        }
        const int next = (int)ids.size();
        ids[obj] = next;
        return next;
    };
    const auto objectNumbered = [&ids, &runtime](int wanted) -> scripts::MdkObject* {
        std::deque<scripts::MdkObject>& list = runtime.objects();
        for (size_t o = 0; o < list.size(); o++) {
            std::map<const scripts::MdkObject*, int>::iterator found = ids.find(&list[o]);
            if (found != ids.end() && found->second == wanted) {
                return &list[o];
            }
        }
        return nullptr;
    };
    // Every arena's groups (the top bytes of its triangles' flags), ascending, and their
    // (state, material) as the level loaded.
    const auto groupText = [](const Arena* arena, int32_t group) {
        int32_t material = 0;
        const bool has = arena->groupMaterial(group, &material);
        return "state=" + std::to_string((int)arena->groupState(group)) +
               " mat=" + (has ? std::to_string(material) : std::string("-"));
    };
    std::vector<std::vector<int32_t> > groupNumbers(arenas.size());
    std::vector<std::vector<std::string> > groupSeen(arenas.size());
    for (size_t a = 0; a < arenas.size(); a++) {
        std::set<int32_t> numbers;
        const std::vector<uint32_t>& flags = arenas[a]->triangleFlags();
        for (size_t t = 0; t < flags.size(); t++) {
            numbers.insert((int32_t)(flags[t] >> 24));
        }
        groupNumbers[a].assign(numbers.begin(), numbers.end());
        for (size_t g = 0; g < groupNumbers[a].size(); g++) {
            groupSeen[a].push_back(groupText(arenas[a], groupNumbers[a][g]));
        }
    }

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    *out += "start town=" + std::to_string(runtime.townTicks) + " sky=" +
            std::to_string(runtime.skyMode) + " skycolour=" + clearText(skyColour) + "\n";
    std::string lastState;
    std::string lastQueue = "-";
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, 0.0f);
            }
        }
        for (size_t i = 0; i < towns.size(); i++) {
            if (towns[i].step == step) {
                runtime.townTicks = towns[i].a;
            }
        }
        for (size_t i = 0; i < gflags.size(); i++) {
            if (gflags[i].step == step) {
                runtime.globalFlags |= gflags[i].a;
            }
        }
        for (size_t i = 0; i < healths.size(); i++) {
            if (healths[i].step == step) {
                if (scripts::MdkObject* obj = objectNumbered(healths[i].a)) {
                    obj->health = healths[i].b;
                }
            }
        }
        for (size_t i = 0; i < kills.size(); i++) {
            if (kills[i].step == step) {
                if (scripts::MdkObject* obj = objectNumbered(kills[i].a)) {
                    runtime.kill(obj);
                }
            }
        }
        for (size_t i = 0; i < avars.size(); i++) {
            if (avars[i].step == step) {
                runtime.getArenaState(avars[i].arena).variables[(size_t)avars[i].index] =
                    avars[i].value;
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        const float goal = runtime.cameraTrackTicks > 0
                               ? runtime.cameraTrackPitch
                               : levelData.arenaPitch(runtime.currentArena());
        follow.update(&camera, &space, player, goal, 1.0f / 60.0f, 4.0f / 3.0f);
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i].dead) {
                id(&objects[i]);
            }
        }

        const Backdrop backdrop = sky_modes::backdropOf(runtime.skyMode);
        snprintf(buffer, sizeof(buffer),
                 "arena=%s town=%d gflags=%08X sky=%d backdrop=%s clear=%s shake=%s cam=%s "
                 "off=%d,%d",
                 runtime.currentArena().c_str(), (int)runtime.townTicks,
                 (unsigned)runtime.globalFlags, (int)runtime.skyMode,
                 kBackdrops[(int)backdrop],
                 clearText(sky_modes::clearOf(runtime.skyMode, skyColour)).c_str(),
                 hexBits(runtime.shake).c_str(), hexBits(follow.shake()).c_str(),
                 (int)follow.shakeOffsetX(), (int)follow.shakeOffsetY());
        const std::string state = buffer;
        std::vector<std::string> events;
        for (size_t i = 0; i < raised.amounts.size(); i++) {
            events.push_back("  raise " + hexBits(raised.amounts[i]));
        }
        raised.amounts.clear();
        for (size_t a = 0; a < arenas.size(); a++) {
            for (size_t g = 0; g < groupNumbers[a].size(); g++) {
                const std::string text = groupText(arenas[a], groupNumbers[a][g]);
                if (text != groupSeen[a][g]) {
                    groupSeen[a][g] = text;
                    events.push_back("  group " + arenas[a]->name() + " " +
                                     std::to_string(groupNumbers[a][g]) + " " + text);
                }
            }
        }
        std::string queue;
        for (int32_t i = 0; i < messages.queued(); i++) {
            queue += (i ? "|" : "") + screenMessage(messages.queuedAt(i));
        }
        if (queue.empty()) {
            queue = "-";
        }
        if (queue != lastQueue) {
            lastQueue = queue;
            events.push_back("  msg " + queue);
        }
        if (state != lastState || !events.empty()) {
            *out += "s=" + std::to_string(step) + " " + state + " objs=" +
                    std::to_string(runtime.objectCount()) + "\n";
            lastState = state;
            for (size_t i = 0; i < events.size(); i++) {
                *out += events[i] + "\n";
            }
        }
    }
    return true;
}

// The end of a level, end to end against the oracle probe (tests/scripts/end_probe): the
// C++ Player, ScriptRuntime and FollowCamera stepped at 1/60 s in the C# Viewer's order
// (Kurt, the scripts, the camera), under the probe's schedule -- the items schedule's
// key holds plus `end:step:value`, which runs special_event `value` (opcode 131's
// ScriptRuntime::specialEvent) for the arena controller of Kurt's arena before that step.
// Its columns are the probe's, byte for byte: Kurt (state, feet, yaw, vertical speed,
// Frozen, Rising, Pose, the camera tilt, the white flash, the gun), the runtime (LevelOver,
// the town flags, LevelEnded, the shake, the objects), the camera (the eased arena pitch
// and the air pitch, which Rising holds at 0), and once the end runs, EndLevel's own state
// (the arena, the torn triangles and their sum, the pieces and their sums, the first piece,
// the rise, the turn, the flash, the takeoff clock and the tilt), then the voices.
bool levelEndDump(int level, int steps, const std::string& schedule, const MdkData& data,
                  const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    // `end:step:value` tokens are this dump's own; the rest is the items schedule.
    std::vector<std::pair<int, int> > events;
    std::string rest;
    size_t at = 0;
    while (at <= schedule.size()) {
        size_t comma = schedule.find(',', at);
        if (comma == std::string::npos) {
            comma = schedule.size();
        }
        const std::string token = schedule.substr(at, comma - at);
        at = comma + 1;
        if (token.compare(0, 4, "end:") == 0) {
            const size_t colon = token.find(':', 4);
            if (colon == std::string::npos) {
                *why = "the end token did not parse: " + token;
                return false;
            }
            events.push_back(std::make_pair(atoi(token.substr(4, colon - 4).c_str()),
                                            atoi(token.substr(colon + 1).c_str())));
        } else if (!token.empty()) {
            rest += (rest.empty() ? "" : ",") + token;
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(rest, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    // Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
    FollowCamera follow;
    Camera camera;

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[768];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < events.size(); i++) {
            if (events[i].first == step) {
                runtime.specialEvent(&runtime.getArenaState(runtime.currentArena()).controller,
                                     events[i].second);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        // Viewer.RunSteps: Kurt, the scripts, then the camera (PitchGoal: camera_track,
        // else the arena's rest pitch).
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        const float goal = runtime.cameraTrackTicks > 0
                               ? runtime.cameraTrackPitch
                               : levelData.arenaPitch(runtime.currentArena());
        follow.update(&camera, &space, player, goal, 1.0f / 60.0f, 4.0f / 3.0f);

        const Kurt& combat = player.combat();
        char pose[64];
        if (player.hasPose && player.poseName != nullptr) {
            snprintf(pose, sizeof(pose), "%s:%d", player.poseName, (int)player.poseFrame);
        } else {
            snprintf(pose, sizeof(pose), "-");
        }
        snprintf(buffer, sizeof(buffer),
                 "s=%d state=%s feet64=%d,%d,%d yaw=%.4f vz=%.4f frozen=%d rising=%d pose=%s "
                 "tilt=%.4f white=%.4f fire=%d hp=%d over=%d town=%d ended=%d cam=%.4f,%.4f "
                 "shake=%.4f objs=%d",
                 step, kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.yaw, player.verticalSpeed, player.frozen ? 1 : 0,
                 player.rising ? 1 : 0, pose, player.cameraTilt, combat.whiteFlash(),
                 combat.firing() ? 1 : 0, player.health(), runtime.levelOver ? 1 : 0,
                 (int)runtime.stats.townFlags, runtime.levelEnded() ? 1 : 0, follow.arenaPitch(),
                 follow.airPitch(), runtime.shake, runtime.objectCount());
        *out += buffer;
        if (const scripts::EndLevel* end = runtime.endLevel()) {
            long long tsum = 0;
            for (size_t i = 0; i < end->torn().size(); i++) {
                tsum += end->torn()[i];
            }
            long long sx = 0, sy = 0, sz = 0, sa = 0;
            const std::vector<scripts::EndLevel::Piece>& pieces = end->pieces();
            for (size_t i = 0; i < pieces.size(); i++) {
                sx += p64(pieces[i].centre.x);
                sy += p64(pieces[i].centre.y);
                sz += p64(pieces[i].centre.z);
                sa += p64(pieces[i].angle);
            }
            char first[160];
            if (pieces.empty()) {
                snprintf(first, sizeof(first), "-");
            } else {
                snprintf(first, sizeof(first), "%d@%d,%d,%d/a%.4f/r%.4f/s%.4f",
                         (int)pieces[0].triangle, p64(pieces[0].centre.x),
                         p64(pieces[0].centre.y), p64(pieces[0].centre.z), pieces[0].angle,
                         pieces[0].riseSpeed, pieces[0].spin);
            }
            snprintf(buffer, sizeof(buffer),
                     " end=%s torn=%d tsum=%lld last=%d pieces=%d psum=%lld,%lld,%lld asum=%lld "
                     "first=%s rise=%.4f turn=%.4f flash=%.4f takeoff=%.4f pitch=%.4f",
                     end->arena().c_str(), (int)end->torn().size(), tsum,
                     end->torn().empty() ? -1 : (int)end->torn().back(), (int)pieces.size(), sx,
                     sy, sz, sa, first, end->riseSpeedNow(), end->turn(), end->flash(),
                     end->takeoff(), end->pitchOffset());
            *out += buffer;
        }
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        *out += " voices=";
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}

// The cutscenes, end to end against the oracle probe (tests/scripts/cutscene_probe): the C++
// Player and ScriptRuntime stepped at 1/60 s in the C# Viewer's order (Kurt, then the
// scripts), under the probe's schedule: `at:x:y:z:yaw` (Kurt's start, the C#'s --at),
// `ev:step:value` (special_event `value` for the controller of Kurt's arena before that step,
// as every retail call above 50 is an arena controller's), `show:step` (print from there on),
// `snap` (a trailing `can=` column: canSnapshot(), the C#'s CanSnapshot) and key holds. Its
// columns are the
// probe's, byte for byte: Kurt (state, feet in 1/64, Frozen, Visible, firing), Kurt's arena,
// the cutscene (state in hex, target, shot, timer, and the camera's yaw, pitch, distance,
// stored position and point as bit patterns), GameFinished, and the objects (count, shown,
// an FNV-1a hash of the hidden ones' indices, the sum of their positions in 1/64).
bool cutsceneDump(int level, int steps, const std::string& schedule, const MdkData& data,
                  const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<std::pair<int, int> > events;
    bool haveAt = false;
    float at[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int show = 1;
    bool snap = false;
    std::string rest;
    size_t cursor = 0;
    while (cursor <= schedule.size()) {
        size_t comma = schedule.find(',', cursor);
        if (comma == std::string::npos) {
            comma = schedule.size();
        }
        const std::string token = schedule.substr(cursor, comma - cursor);
        cursor = comma + 1;
        if (token.compare(0, 3, "ev:") == 0) {
            int step = 0;
            int value = 0;
            if (sscanf(token.c_str() + 3, "%d:%d", &step, &value) != 2) {
                *why = "the event token did not parse: " + token;
                return false;
            }
            events.push_back(std::make_pair(step, value));
        } else if (token.compare(0, 3, "at:") == 0) {
            if (sscanf(token.c_str() + 3, "%f:%f:%f:%f", &at[0], &at[1], &at[2], &at[3]) != 4) {
                *why = "the start token did not parse: " + token;
                return false;
            }
            haveAt = true;
        } else if (token.compare(0, 5, "show:") == 0) {
            show = atoi(token.c_str() + 5);
        } else if (token == "snap") {
            snap = true;
        } else if (!token.empty()) {
            rest += (rest.empty() ? "" : ",") + token;
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(rest, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = haveAt ? Vec3{at[0], at[1], at[2]} : Vec3{start[0], start[1], start[2]};
    player.yaw = haveAt ? at[3] : levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    const auto bits = [](float v) {
        uint32_t b;
        memcpy(&b, &v, sizeof(b));
        char text[16];
        snprintf(text, sizeof(text), "%08x", b);
        return std::string(text);
    };
    char buffer[1024];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < events.size(); i++) {
            if (events[i].first == step) {
                runtime.specialEvent(&runtime.getArenaState(runtime.currentArena()).controller,
                                     events[i].second);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        // Viewer.RunSteps: Kurt, then the scripts.
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        if (step < show) {
            continue;
        }

        std::string target = "-";
        if (const scripts::MdkObject* t = runtime.cutsceneTarget) {
            target = t->type_name.empty() ? "@" + t->arena
                                          : t->type_name + "_" + std::to_string(t->instance_id);
        }
        int count = 0;
        int shown = 0;
        uint32_t hidden = 2166136261u;
        long long sum = 0;
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& obj = objects[i];
            if (runtime.isRemoved(&obj)) {
                continue;  // not one of the C#'s Objects
            }
            if (obj.visible) {
                shown++;
            } else {
                hidden = (hidden ^ (uint32_t)count) * 16777619u;
            }
            sum += p64(obj.position.x) + p64(obj.position.y) + p64(obj.position.z);
            count++;
        }
        const Vec3& pos = runtime.cameraPosition;
        const Vec3& point = runtime.cameraPoint;
        snprintf(buffer, sizeof(buffer),
                 "s=%d state=%s feet64=%d,%d,%d frozen=%d kvis=%d fire=%d arena=%s cut=%x "
                 "target=%s mode=%d timer=%d yaw=%s pitch=%s dist=%s pos=%s,%s,%s "
                 "point=%s,%s,%s finished=%d objs=%d shown=%d hid=%08x osum=%lld\n",
                 step, kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.frozen ? 1 : 0, player.visible ? 1 : 0,
                 player.combat().firing() ? 1 : 0, runtime.currentArena().c_str(),
                 (unsigned)runtime.cutscene, target.c_str(), (int)runtime.cameraMode,
                 (int)runtime.cameraTimer, bits(runtime.cameraYaw).c_str(),
                 bits(runtime.cameraPitch).c_str(), bits(runtime.cameraDistance).c_str(),
                 bits(pos.x).c_str(), bits(pos.y).c_str(), bits(pos.z).c_str(),
                 bits(point.x).c_str(), bits(point.y).c_str(), bits(point.z).c_str(),
                 runtime.gameFinished() ? 1 : 0, count, shown, (unsigned)hidden, sum);
        *out += buffer;
        if (snap) {
            // The probe's `can=` column: C# bool.ToString(), "True" / "False".
            out->insert(out->size() - 1, runtime.canSnapshot() ? " can=True" : " can=False");
        }
    }
    return true;
}

// THE AIR STRIKE AND THE STRIKE SCENE (golden/L*_strike_*.txt; PORT_GAPS package #14). Kurt
// then the scripts at 1/60 s, as Viewer.RunSteps -- and, as Viewer.Update (Viewer.cs:396-410,
// SniperView.DrawStrike), while the runtime's strike scene is active the game WAITS: the step
// is the scene's update(1/60 s) (`skip:` steps: skip()), and a step on which it ends runs the
// game too. The oracle is tests/scripts/strike_probe (`Probe <level> <steps> <schedule>`), whose
// header says what the lines hold; this prints the same bytes. Tokens: at:x:y:z:yaw,
// give:step:MODEL, aim:step:YAW:PITCH (float bits), ev:step:value, used:step, skip:step,
// show:step, and key holds `snipe|fire|next|prev|zin|zout:from-to` over game steps.
bool strikeDump(int level, int steps, const std::string& schedule, const MdkData& data,
                const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    static const struct {
        const char* name;
        Key key;
    } kKeys[] = {{"snipe", kKeySniper}, {"fire", kKeyFire},  {"next", kKeyItemNext},
                 {"prev", kKeyItemPrevious}, {"zin", kKeyZoomIn}, {"zout", kKeyZoomOut}};
    std::vector<RoundsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<RoundsAim> aims;
    std::vector<std::pair<int, int> > events;
    std::vector<int> used;
    std::vector<int> skips;
    bool haveAt = false;
    float at[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int show = 1;
    size_t cursor = 0;
    while (cursor < schedule.size()) {
        size_t comma = schedule.find(',', cursor);
        if (comma == std::string::npos) {
            comma = schedule.size();
        }
        const std::string token = schedule.substr(cursor, comma - cursor);
        cursor = comma + 1;
        if (token.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        size_t from = 0;
        while (true) {
            const size_t next = token.find(':', from);
            fields.push_back(token.substr(from, next == std::string::npos ? std::string::npos
                                                                          : next - from));
            if (next == std::string::npos) {
                break;
            }
            from = next + 1;
        }
        const std::string& kind = fields[0];
        if (kind == "at" && fields.size() == 5) {
            // float.Parse of the probe's decimal text: strtof rounds the same way.
            for (int i = 0; i < 4; i++) {
                at[i] = strtof(fields[(size_t)i + 1].c_str(), nullptr);
            }
            haveAt = true;
        } else if (kind == "give" && fields.size() == 3) {
            ItemsGive give;
            give.step = atoi(fields[1].c_str());
            give.model = fields[2];
            give.drop = false;
            give.offset = Vec3{};
            gives.push_back(give);
        } else if (kind == "aim" && fields.size() == 4) {
            RoundsAim aim;
            aim.step = atoi(fields[1].c_str());
            aim.yaw = roundsBits(fields[2]);
            aim.pitch = roundsBits(fields[3]);
            aims.push_back(aim);
        } else if (kind == "ev" && fields.size() == 3) {
            events.push_back(std::make_pair(atoi(fields[1].c_str()), atoi(fields[2].c_str())));
        } else if (kind == "used" && fields.size() == 2) {
            used.push_back(atoi(fields[1].c_str()));
        } else if (kind == "skip" && fields.size() == 2) {
            skips.push_back(atoi(fields[1].c_str()));
        } else if (kind == "show" && fields.size() == 2) {
            show = atoi(fields[1].c_str());
        } else {
            bool known = false;
            for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
                if (fields.size() == 2 && kind == kKeys[k].name) {
                    RoundsHold hold;
                    hold.key = kKeys[k].key;
                    if (sscanf(fields[1].c_str(), "%d-%d", &hold.from, &hold.to) != 2) {
                        break;
                    }
                    holds.push_back(hold);
                    known = true;
                }
            }
            if (!known) {
                *why = "the schedule did not parse: " + token;
                return false;
            }
        }
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = haveAt ? Vec3{at[0], at[1], at[2]} : Vec3{start[0], start[1], start[2]};
    player.yaw = haveAt ? at[3] : levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    std::map<const scripts::MdkObject*, int> ids;
    const auto id = [&ids](const scripts::MdkObject* obj) {
        std::map<const scripts::MdkObject*, int>::iterator found = ids.find(obj);
        if (found != ids.end()) {
            return found->second;
        }
        const int next = (int)ids.size();
        ids[obj] = next;
        return next;
    };
    const auto vec = [](const Vec3& v) {
        return hexBits(v.x) + "," + hexBits(v.y) + "," + hexBits(v.z);
    };
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[1024];
    // The probe's SceneLine: the last scene played, or `scene=-`.
    const auto sceneLine = [&]() {
        const scripts::StrikeScene* scene = runtime.strikeScene();
        if (scene == nullptr) {
            return std::string("scene=-");
        }
        Vec3 eye;
        Vec3 target;
        scene->camera(&eye, &target);
        const scripts::MdkObject& subject = scene->subject();
        char line[512];  // its own: the caller's buffer may hold the line it is appended to
        snprintf(line, sizeof(line), "scene=%d f=%s y=%s sub=%s:%d:%X:%s eye=%s at=%s",
                 scene->active() ? 1 : 0, hexBits(scene->frame()).c_str(),
                 hexBits(scene->yaw()).c_str(),
                 subject.type_name.empty() ? "-" : subject.type_name.c_str(),
                 (int)subject.animation_frame, (unsigned)subject.hidden_parts,
                 hexBits(subject.yaw).c_str(), vec(eye).c_str(), vec(target).c_str());
        return std::string(line);
    };
    std::map<const scripts::MdkObject*, std::string> seen;
    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step) {
                player.collect(gives[i].model);
            }
        }
        for (size_t i = 0; i < aims.size(); i++) {
            if (aims[i].step == step) {
                player.yaw = aims[i].yaw;
                player.scope().pitch = aims[i].pitch;
            }
        }
        for (size_t i = 0; i < used.size(); i++) {
            if (used[i] == step) {
                runtime.airStrike().usedUp = true;
            }
        }
        for (size_t i = 0; i < events.size(); i++) {
            if (events[i].first == step) {
                runtime.specialEvent(&runtime.getArenaState(runtime.currentArena()).controller,
                                     events[i].second);
            }
        }

        // Viewer.Update through SniperView.DrawStrike: the scene advances (or Esc skips it)
        // and the game waits while it plays.
        scripts::StrikeScene* scene = runtime.strikeScene();
        if (scene != nullptr && scene->active()) {
            bool skip = false;
            for (size_t i = 0; i < skips.size(); i++) {
                skip = skip || skips[i] == step;
            }
            if (skip) {
                scene->skip();
            } else {
                scene->update(1.0f / 60.0f);
            }
            if (scene->active()) {
                if (step >= show) {
                    *out += "w=" + std::to_string(step) + " " + sceneLine() + "\n";
                }
                continue;
            }
        }

        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        if (step < show) {
            continue;
        }

        // The C#'s list holds no removed object: they are neither numbered nor printed.
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        int live = 0;
        for (size_t i = 0; i < objects.size(); i++) {
            if (runtime.isRemoved(&objects[i])) {
                continue;
            }
            id(&objects[i]);
            if (!objects[i].dead) {
                live++;
            }
        }

        // SniperView.Aim's test, the overlay's strike-target bit.
        std::string target = "-";
        Vec3 point;
        if (runtime.airStrike().findTarget(player.sniperEye(), player.sniperForward(), &point)) {
            target = vec(point);
        }
        const scripts::AirStrike& air = runtime.airStrike();
        snprintf(buffer, sizeof(buffer),
                 "strike=%d used=%d len=%s off=%s toff=%s doff=%s teeth=%d diving=%d",
                 air.isActive() ? 1 : 0, air.usedUp ? 1 : 0,
                 air.hasCurve() ? hexBits(air.curveLength()).c_str() : "-",
                 hexBits(air.offset()).c_str(), hexBits(air.targetOffset()).c_str(),
                 hexBits(air.diveOffset()).c_str(), (int)air.teeth(), air.diving() ? 1 : 0);
        std::string strike = buffer;
        if (const scripts::MdkObject* x = air.strike()) {
            snprintf(buffer, sizeof(buffer), " x#%d=%s:%s:%s:%s:%s:%X:%X:%d", id(x),
                     x->type_name.c_str(), x->dead ? "dead" : "live", vec(x->position).c_str(),
                     hexBits(x->yaw).c_str(), vec(x->velocity).c_str(), (unsigned)x->flags,
                     (unsigned)x->contact_flags, (int)x->health);
            strike += buffer;
        }
        const Kurt& combat = player.combat();
        const Scope& scope = player.scope();
        snprintf(buffer, sizeof(buffer),
                 "s=%d feet64=%d,%d,%d snipe=%d sel=%d ammo5=%d clip=%d/%s hp=%d shots=%d "
                 "kills=%d objs=%d effects=%d cut=%x arena=%s tgt=%s ",
                 step, p64(player.feet.x), p64(player.feet.y), p64(player.feet.z),
                 player.sniping() ? 1 : 0, combat.selectedAmmo(), combat.ammo(5),
                 scope.clipRounds, hexBits(scope.clipTime).c_str(), player.health(),
                 runtime.stats.sniperShots, runtime.stats.kills, live, runtime.effects.count(),
                 (unsigned)runtime.cutscene, runtime.currentArena().c_str(), target.c_str());
        *out += std::string(buffer) + strike + " " + sceneLine() + "\n";

        // A tooth from its drop on, while it changes; any other object when its hit state does.
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (runtime.isRemoved(&o)) {
                continue;
            }
            const int objectId = id(&o);
            const bool tooth = o.type_name == "X_TOOTH";
            if (tooth) {
                snprintf(buffer, sizeof(buffer), "%s p=%s v=%s f=%X k=%d it=%d c=%X",
                         o.dead ? "dead" : "live", vec(o.position).c_str(),
                         vec(o.velocity).c_str(), (unsigned)o.flags, (int)o.thrown_kind,
                         (int)o.item_ticks, (unsigned)o.contact_flags);
            } else {
                snprintf(buffer, sizeof(buffer), "hp=%d ev=%d ht=%d dead=%d", (int)o.health,
                         (int)o.hit_event, (int)o.hit_type, o.dead ? 1 : 0);
            }
            const std::string text = buffer;
            std::map<const scripts::MdkObject*, std::string>::iterator before = seen.find(&o);
            const bool known = before != seen.end();
            if (known && before->second == text) {
                continue;
            }
            seen[&o] = text;
            if (tooth) {
                *out += "  t#" + std::to_string(objectId) + " " + text + "\n";
            } else if (known) {
                *out += "  #" + std::to_string(objectId) + " " + o.type_name + " " + text +
                        " pos=" + vec(o.position) + "\n";
            }
        }
    }
    return true;
}

// The hazards probe's schedule (tests/scripts/hazards_probe): items_probe's tokens, plus
// `tp:step:ARENA:x:y:z` (TeleportKurt), `run:step:MODEL:pc:dx:dy:dz` (a plain spawn in
// Kurt's arena at his feet plus the offset, running the retail script at CMI offset pc)
// and `watch:TYPE` (the objects of that type printed each step). The items tokens go to
// parseItemsSchedule untouched.
struct HazardsTeleport {
    int step;
    std::string arena;
    Vec3 position;
};
struct HazardsRun {
    int step;
    std::string model;
    int32_t pc;
    Vec3 offset;
};

std::vector<std::string> splitColons(const std::string& token) {
    std::vector<std::string> fields;
    size_t at = 0;
    while (true) {
        const size_t next = token.find(':', at);
        fields.push_back(token.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    return fields;
}

bool parseHazardsSchedule(const std::string& schedule, std::vector<ItemsHold>* holds,
                          std::vector<ItemsGive>* gives, std::vector<HazardsTeleport>* teleports,
                          std::vector<HazardsRun>* runs, std::vector<std::string>* watched,
                          bool* flying, std::vector<std::string>* roped, bool* textures,
                          bool* sprites) {
    std::string items;
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        if (token.empty()) {
            continue;
        }
        const std::vector<std::string> f = splitColons(token);
        if (f[0] == "tp") {
            if (f.size() != 6) {
                return false;
            }
            HazardsTeleport t;
            t.step = atoi(f[1].c_str());
            t.arena = f[2];
            t.position = Vec3{strtof(f[3].c_str(), nullptr), strtof(f[4].c_str(), nullptr),
                              strtof(f[5].c_str(), nullptr)};
            teleports->push_back(t);
        } else if (f[0] == "run") {
            if (f.size() != 7) {
                return false;
            }
            HazardsRun r;
            r.step = atoi(f[1].c_str());
            r.model = f[2];
            r.pc = atoi(f[3].c_str());
            r.offset = Vec3{strtof(f[4].c_str(), nullptr), strtof(f[5].c_str(), nullptr),
                            strtof(f[6].c_str(), nullptr)};
            runs->push_back(r);
        } else if (f[0] == "watch") {
            if (f.size() != 2) {
                return false;
            }
            watched->push_back(f[1]);
        } else if (f[0] == "flying") {
            if (f.size() != 1) {
                return false;
            }
            *flying = true;
        } else if (f[0] == "sprites") {
            if (f.size() != 1) {
                return false;
            }
            *sprites = true;
        } else if (f[0] == "frames") {
            if (f.size() != 1) {
                return false;
            }
            *textures = true;
        } else if (f[0] == "rope") {
            if (f.size() != 2) {
                return false;
            }
            roped->push_back(f[1]);
        } else {
            items += (items.empty() ? "" : ",") + token;
        }
    }
    // The items probe's `push:` (a HUD message) is not a hazards token: the hazards
    // probe has no HUD, so a schedule that carries one is refused, not ignored.
    std::vector<ItemsPush> pushes;
    return parseItemsSchedule(items, holds, gives, &pushes) && pushes.empty();
}

// `tick1 dump hazards <level> <steps> <schedule>`: the C++ side of tests/scripts/
// hazards_probe, column for column -- Kurt, the object list's totals, the live spawn_box
// boxes (place and half size), the watched objects (health, place, movement command and
// destination, path speed, flags, last hit), the bomb, the pieces and effects, the white
// flash, the shake and the voices. It pins the combat and hazard opcodes (158, 159, 167, 172, 178, 184)
// and path_speed_by_kurt's consumer.
// L8_hazards_debris's schedule (the hazards table), the effects dump's storm.
const char* const kEffectsStormSchedule =
    "tp:1:GUNT_2:-209:480:40,run:3:XBANG:24840:0:105:0,fire:60-300,watch:XBANG,"
    "watch:BOLT,watch:BIGBOLT,flying";

bool kurtHazardsDump(int level, int steps, const std::string& schedule, const MdkData& data,
                     const Bni* sprites, std::string* out, std::string* why,
                     test::EffectsTrace* trace = nullptr) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<HazardsTeleport> teleports;
    std::vector<HazardsRun> runs;
    std::vector<std::string> watched;
    bool flying = false;
    std::vector<std::string> roped;
    bool textures = false;
    bool quads = false;
    if (!parseHazardsSchedule(schedule, &holds, &gives, &teleports, &runs, &watched, &flying,
                              &roped, &textures, &quads)) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[1024];
    // The `sprites` token's view (EffectsView, the Linux build's: no cull), under a camera
    // whose view-projection has exact 0/1 entries -- right +X, forward +Y, up +Z -- so the
    // C# probe's View carries the same bits.
    MaterialResolver spriteResolver(&levelData.dti(), Shading::Original);
    EffectsView spriteView(spriteResolver, &levelData, Shading::Original);
    EffectsFrame spriteFrame;
    Camera spriteCamera;
    for (int k = 0; k < 16; k++) {
        spriteCamera.view[k] = 0.0f;
        spriteCamera.projection[k] = (k % 5) == 0 ? 1.0f : 0.0f;
    }
    spriteCamera.view[0] = 1.0f;   // M11: right
    spriteCamera.view[7] = 1.0f;   // M24: forward
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].position, player.yaw);
            }
        }
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step && !gives[i].drop) {
                player.collect(gives[i].model);
            }
        }
        for (size_t i = 0; i < gives.size(); i++) {
            if (gives[i].step == step && gives[i].drop) {
                const Vec3 at{start[0] + gives[i].offset.x, start[1] + gives[i].offset.y,
                              start[2] + gives[i].offset.z};
                runtime.spawn(&runtime.getArenaState(runtime.currentArena()).controller,
                              gives[i].model, at, 0.0f, -1, 0, true);
            }
        }
        for (size_t i = 0; i < runs.size(); i++) {
            if (runs[i].step == step) {
                const Vec3 at{player.feet.x + runs[i].offset.x, player.feet.y + runs[i].offset.y,
                              player.feet.z + runs[i].offset.z};
                runtime.spawn(&runtime.getArenaState(runtime.currentArena()).controller,
                              runs[i].model, at, 0.0f, -1, runs[i].pc, false);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        const Kurt& combat = player.combat();
        int32_t ohp = 0;
        long long sx = 0;
        long long sy = 0;
        long long sz = 0;
        std::string watch;
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (o.dead) {
                continue;
            }
            ohp += o.health;
            sx += p64(o.position.x);
            sy += p64(o.position.y);
            sz += p64(o.position.z);
            if (std::find(watched.begin(), watched.end(), o.type_name) != watched.end()) {
                snprintf(buffer, sizeof(buffer),
                         "%s%s#%d:h%d@%d,%d,%d/m%d>%d,%d,%d/v%.4f/f%X/e%d/t%d",
                         watch.empty() ? "" : ";", o.type_name.c_str(), o.instance_id, o.health,
                         p64(o.position.x), p64(o.position.y), p64(o.position.z), o.move_command,
                         p64(o.move_destination.x), p64(o.move_destination.y),
                         p64(o.move_destination.z), o.path_speed, (unsigned)o.flags, o.hit_event,
                         o.hit_type);
                watch += buffer;
            }
        }
        std::string boxes;
        const std::vector<scripts::MdkObject*>& live = runtime.boxes();
        for (size_t i = 0; i < live.size(); i++) {
            const scripts::MdkObject& b = *live[i];
            if (b.dead) {
                continue;
            }
            const formats::Vec3f& half = b.model->bounds.max;
            snprintf(buffer, sizeof(buffer), "%s%s#%d@%d,%d,%d/%d,%d,%d", boxes.empty() ? "" : ";",
                     b.type_name.c_str(), b.instance_id, p64(b.position.x), p64(b.position.y),
                     p64(b.position.z), p64(half.x), p64(half.y), p64(half.z));
            boxes += buffer;
        }
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& playing = runtime.mixer().voices();
        for (size_t i = 0; i < playing.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += playing[i].name;
        }
        snprintf(buffer, sizeof(buffer),
                 "s=%d state=%s arena=%s feet64=%d,%d,%d yaw=%.4f hp=%d kd=%.4f objs=%d ohp=%d "
                 "opos=%lld,%lld,%lld kills=%d boxes=",
                 step, kurtStateName(player.state()), runtime.currentArena().c_str(),
                 p64(player.feet.x), p64(player.feet.y), p64(player.feet.z), player.yaw,
                 player.health(), combat.knockDamage(), runtime.objectCount(), ohp, sx, sy, sz,
                 runtime.stats.kills);
        *out += buffer;
        *out += boxes.empty() ? "-" : boxes;
        *out += " watch=";
        *out += watch.empty() ? "-" : watch;
        snprintf(buffer, sizeof(buffer),
                 " bomb=%d pieces=%d effects=%d white=%.4f shake=%.4f voices=",
                 runtime.itemsBomb != nullptr && !runtime.itemsBomb->dead ? 1 : 0,
                 runtime.debris().pieceCount(), runtime.effects.count(), combat.whiteFlash(),
                 runtime.shake);
        *out += buffer;
        *out += voices.empty() ? "-" : voices;
        if (flying) {
            // The `flying` token: FNV-1a over every piece's centre, velocity and ticks, and
            // every effect's position, velocity and life, as bits, in list order. Each
            // piece's orientation is left out on purpose: .NET's CreateFromYawPitchRoll takes
            // its sines from Vector3.SinCos, not libm (docs/DIVERGENCES.md, D5).
            uint32_t pieces = 2166136261u;
            const std::vector<scripts::Debris::Piece*>& list = runtime.debris().pieces();
            for (size_t i = 0; i < list.size(); i++) {
                const scripts::Debris::Piece* piece = list[i];
                pieces = fnvFloat(pieces, piece->center.x);
                pieces = fnvFloat(pieces, piece->center.y);
                pieces = fnvFloat(pieces, piece->center.z);
                pieces = fnvFloat(pieces, piece->velocity.x);
                pieces = fnvFloat(pieces, piece->velocity.y);
                pieces = fnvFloat(pieces, piece->velocity.z);
                pieces = fnvInt(pieces, (uint32_t)piece->ticks);
            }
            uint32_t effects = 2166136261u;
            const std::vector<scripts::Effects::Effect*>& all = runtime.effects.all();
            for (size_t i = 0; i < all.size(); i++) {
                const scripts::Effects::Effect* effect = all[i];
                effects = fnvFloat(effects, effect->position.x);
                effects = fnvFloat(effects, effect->position.y);
                effects = fnvFloat(effects, effect->position.z);
                effects = fnvFloat(effects, effect->velocity.x);
                effects = fnvFloat(effects, effect->velocity.y);
                effects = fnvFloat(effects, effect->velocity.z);
                effects = fnvFloat(effects, effect->life);
            }
            snprintf(buffer, sizeof(buffer), " fly=%08X,%08X", pieces, effects);
            *out += buffer;
        }
        if (!roped.empty()) {
            // The `rope:TYPE` token: per live object of that type, its place, pitch, yaw,
            // the swing's speed and plane (jump_to, opcode 226), its flags, the rope's
            // colour and mask and RopeView.LinesOf's ends (blending off), at 1/10000, and
            // `#` an FNV-1a over the bits of every one of those floats, so a difference in
            // the last bit fails too.
            std::string rope;
            for (size_t i = 0; i < objects.size(); i++) {
                const scripts::MdkObject& o = objects[i];
                if (o.dead ||
                    std::find(roped.begin(), roped.end(), o.type_name) == roped.end()) {
                    continue;
                }
                uint32_t bits = 2166136261u;
                const float values[] = {o.position.x, o.position.y, o.position.z, o.pitch,
                                        o.yaw, o.swing_speed, o.swing_yaw};
                for (size_t k = 0; k < sizeof(values) / sizeof(values[0]); k++) {
                    bits = fnvFloat(bits, values[k]);
                }
                snprintf(buffer, sizeof(buffer),
                         "%s%s#%d@%.4f,%.4f,%.4f/p%.4f/y%.4f/w%.4f/sy%.4f/f%X/c%d/m%X/l",
                         rope.empty() ? "" : ";", o.type_name.c_str(), o.instance_id,
                         o.position.x, o.position.y, o.position.z, o.pitch, o.yaw,
                         o.swing_speed, o.swing_yaw, (unsigned)o.flags, o.rope_color,
                         (unsigned)o.rope_mask);
                rope += buffer;
                Vec3 ends[scripts::MdkObject::kMaxRopeEnds];
                const int32_t count = o.rope_lines(1.0f, ends);
                for (int32_t k = 0; k < count; k++) {
                    snprintf(buffer, sizeof(buffer), "%s%.4f,%.4f,%.4f", k == 0 ? "" : "|",
                             ends[k].x, ends[k].y, ends[k].z);
                    rope += buffer;
                    bits = fnvFloat(bits, ends[k].x);
                    bits = fnvFloat(bits, ends[k].y);
                    bits = fnvFloat(bits, ends[k].z);
                }
                snprintf(buffer, sizeof(buffer), "%s#%08X", count == 0 ? "-" : "", bits);
                rope += buffer;
            }
            *out += " rope=";
            *out += rope.empty() ? "-" : rope;
        }
        if (quads) {
            // The `sprites` token: EffectsView's frame (EffectsView.Draw) -- the effects'
            // and the spawn_box boxes' quads (opcode 159) -- as its draw count, vertex
            // count, each draw's first:count:frame, and an FNV-1a over every vertex's
            // position and uv bits. Debris pieces would be in it too; the cases using it
            // have none.
            spriteView.build(runtime, spriteCamera, &spriteFrame, 1.0f);
            uint32_t bits = 2166136261u;
            for (size_t k = 0; k < spriteFrame.vertices.size(); k++) {
                const Vertex& v = spriteFrame.vertices[k];
                bits = fnvFloat(bits, v.pos.x);
                bits = fnvFloat(bits, v.pos.y);
                bits = fnvFloat(bits, v.pos.z);
                bits = fnvFloat(bits, v.uv.x);
                bits = fnvFloat(bits, v.uv.y);
            }
            std::string list;
            for (size_t k = 0; k < spriteFrame.draws.size(); k++) {
                snprintf(buffer, sizeof(buffer), "%s%d:%d:%d", k == 0 ? "" : ",",
                         spriteFrame.draws[k].first, spriteFrame.draws[k].count,
                         spriteFrame.draws[k].frame);
                list += buffer;
            }
            snprintf(buffer, sizeof(buffer), " spr=%d/%d/%08X/", (int)spriteFrame.draws.size(),
                     (int)spriteFrame.vertices.size(), bits);
            *out += buffer;
            *out += list.empty() ? "-" : list;
        }
        if (textures) {
            // The `frames` token: the scripts' arena texture frames (arena_texture_frame,
            // opcode 133; AnimatedTextures.Frames) in (arena, texture) order, each as
            // arena/texture:value (1/10000):drawn frame (FrameOf, what LevelView draws).
            std::string list;
            const std::map<std::pair<std::string, std::string>, float>& all =
                runtime.animatedTextures.frames();
            for (std::map<std::pair<std::string, std::string>, float>::const_iterator it =
                     all.begin();
                 it != all.end(); ++it) {
                snprintf(buffer, sizeof(buffer), "%s%s/%s:%.4f:%d", list.empty() ? "" : ";",
                         it->first.first.c_str(), it->first.second.c_str(), it->second,
                         runtime.animatedTextures.frameOf(it->first.first, it->first.second));
                list += buffer;
            }
            *out += " tex=";
            *out += list.empty() ? "-" : list;
        }
        *out += "\n";
        if (trace != nullptr) {
            test::effectsTraceStep(trace, levelData, runtime, player, step);
        }
    }
    if (trace != nullptr) {
        // effects_dump.h: the effects frames replace the hazards text.
        test::effectsTraceEnd(trace, levelData, runtime, player, out);
    }
    return true;
}

// ---- KURT AGAINST THE SOLID OBJECTS, ON PLATFORMS AND LIFTS (package #3) -----------
// (golden/L*_plat_*.txt). The C# Kurt and ScriptRuntime stepped together at 1/60 s, Kurt
// then the scripts, as tests/scripts/platform_probe does; this prints the same columns:
//
//   arena            the runtime's current arena.
//   state / feet64 / Kurt's state, feet x64 (floored), yaw, vertical speed, OnFloor.
//   yaw / vz / floor
//   plat / on        Kurt.Platform (the fall that landed him on it), and GetKurtPlatform
//                    (the platform while he is on a floor), as TYPE#instance.
//   kept             Kurt._keeper (Kurt.Keeper.cs): the platform that keeps him.
//   touched          Kurt.Touched: what his walking box touches, and his platform.
//   hit              his arena's hit-script scratch object's hit type and restart: a group
//                    hit by KurtTouchesGroups runs there with hit type -11.
//   clip             ScriptRuntime::clipView from his head to 30 units north (x64): where
//                    an object that blocks the view (0x1000000) cuts the camera's sight.
//   objs / near      the object count, and the solid objects of his arena within 40 units
//                    (position x64, yaw, hidden parts, flags): the lifts' moves.
//
// The schedule is items_probe's key holds (fwd back left right sleft sright jump turbo
// fire), `tp:step:ARENA:x:y:z:yaw`, ScriptRuntime.TeleportKurt before that step, and
// `spawn:step:TYPE:x:y:z:yaw:script`, ScriptRuntime.Spawn (Spawning.Plain, as spawn_relative
// does) of TYPE in Kurt's arena with its script at that CMI offset, before that step.
struct PlatformTeleport {
    int step;
    std::string arena;  // tp: the arena; spawn: the object type
    Vec3 at;
    float yaw;
    bool spawn;         // `spawn:`: ScriptRuntime::spawn, not teleportKurt
    int32_t script;
};

bool parsePlatformSchedule(const std::string& schedule, std::vector<ItemsHold>* holds,
                           std::vector<PlatformTeleport>* teleports) {
    std::string keys;
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        const bool spawn = token.compare(0, 6, "spawn:") == 0;
        if (token.compare(0, 3, "tp:") != 0 && !spawn) {
            keys += (keys.empty() ? "" : ",") + token;
            continue;
        }
        std::vector<std::string> fields;
        size_t at = spawn ? 6 : 3;
        while (true) {
            const size_t next = token.find(':', at);
            fields.push_back(token.substr(at, next == std::string::npos ? std::string::npos
                                                                      : next - at));
            if (next == std::string::npos) {
                break;
            }
            at = next + 1;
        }
        if (fields.size() != (spawn ? 7u : 6u)) {
            return false;
        }
        // strtof, not atof: C#'s float.Parse rounds the text to a float once.
        PlatformTeleport tp;
        tp.step = atoi(fields[0].c_str());
        tp.arena = fields[1];
        tp.at = Vec3{strtof(fields[2].c_str(), nullptr), strtof(fields[3].c_str(), nullptr),
                     strtof(fields[4].c_str(), nullptr)};
        tp.yaw = strtof(fields[5].c_str(), nullptr);
        tp.spawn = spawn;
        tp.script = spawn ? atoi(fields[6].c_str()) : 0;
        teleports->push_back(tp);
    }
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    return parseItemsSchedule(keys, holds, &gives, &pushes) && gives.empty() && pushes.empty();
}

std::string platformName(const scripts::MdkObject* o) {
    return o == nullptr ? "-" : o->type_name + "#" + std::to_string(o->instance_id);
}

bool kurtPlatformsDump(int level, int steps, const std::string& schedule, const MdkData& data,
                       const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<ItemsHold> holds;
    std::vector<PlatformTeleport> teleports;
    if (!parsePlatformSchedule(schedule, &holds, &teleports)) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[512];
    for (int step = 1; step <= steps; step++) {
        // The probe's order: the teleports of the step, then its spawns.
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step && !teleports[i].spawn) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, teleports[i].yaw);
            }
        }
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step && teleports[i].spawn) {
                runtime.spawn(&runtime.getArenaState(runtime.currentArena()).controller,
                              teleports[i].arena, teleports[i].at, teleports[i].yaw, -1,
                              teleports[i].script, false);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);

        std::string touched;
        for (size_t i = 0; i < player.touched().size(); i++) {
            touched += (i ? "|" : "") + platformName(player.touched()[i]);
        }
        const scripts::MdkObject& hits = runtime.getArenaState(runtime.currentArena()).hit_scripts;
        std::string near;
        const std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            solids::Footing footing;
            const float dx = o.position.x - player.feet.x;
            const float dy = o.position.y - player.feet.y;
            const float dz = o.position.z - player.feet.z;
            if (o.dead || o.arena != runtime.currentArena() ||
                !scripts::ScriptRuntime::footingOf(&o, &footing) ||
                dx * dx + dy * dy + dz * dz > 1600.0f) {
                continue;
            }
            snprintf(buffer, sizeof(buffer), "%s%s@%d,%d,%d/y%.4f/h%X/f%X/r%d", near.empty() ? "" : ";",
                     platformName(&o).c_str(), p64(o.position.x), p64(o.position.y),
                     p64(o.position.z), o.yaw, (unsigned)o.hidden_parts, (unsigned)o.flags,
                     o.restart);
            near += buffer;
        }
        // ScriptRuntime::clipView (the follow camera's object pass) from his head
        // (HeadHeight 5.5) to 30 units north of it.
        const Vec3 head{player.feet.x, player.feet.y, player.feet.z + 5.5f};
        const Vec3 clip = runtime.clipView(head, Vec3{head.x, head.y + 30.0f, head.z});
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s state=%s feet64=%d,%d,%d yaw=%.4f vz=%.4f floor=%d plat=%s on=%s "
                 "kept=%s touched=%s hit=%d/%d clip=%d,%d,%d objs=%d near=",
                 step, runtime.currentArena().c_str(), kurtStateName(player.state()),
                 p64(player.feet.x), p64(player.feet.y), p64(player.feet.z), player.yaw,
                 player.verticalSpeed, player.onFloor ? 1 : 0,
                 platformName(player.platform()).c_str(),
                 platformName(runtime.getKurtPlatform()).c_str(),
                 platformName(player.keeper()).c_str(), touched.empty() ? "-" : touched.c_str(),
                 hits.hit_type, hits.restart, p64(clip.x), p64(clip.y), p64(clip.z),
                 runtime.objectCount());
        *out += buffer;
        *out += near.empty() ? "-" : near;
        *out += "\n";
    }
    return true;
}
// ---- end of the platforms dump (package #3) ------------------------------------------

// ---- the rides dump (package #9) ---------------------------------------------------
// Kurt and the walker ride, end to end against the oracle probe (tests/scripts/ride_probe):
// the C++ Player, ScriptRuntime and FollowCamera stepped at 1/60 s in the C# Viewer's order
// (Kurt, the scripts -- whose update ends with Rides::follow -- then the camera), under the
// probe's schedule: the key holds (`key:from-to`), `tp:step:ARENA:x:y:z:yaw`
// (teleportKurt), `ride:step:TYPE` (Rides::rideWalker on the first live object of that
// type, the tests' --ride), `off:step:TYPE` (its rideable flag cleared, as its script
// does), `kill:step:TYPE` (ScriptRuntime::kill, the tests' --kill) and `hit:step:DAMAGE`
// (ScriptRuntime::hurtKurt), each before its step. Its columns are the probe's, byte for
// byte:
//
//   state ... floor  Kurt's state, feet (1/64 units), yaw, forward/strafe/vertical speeds.
//   walk vis keys    Player::walkMode (N/R), visible, the ride keys (steer, fire).
//   fire hp          the chain gun, Kurt's health.
//   ride ride171     Rides.Ridden, and whether it is the walker (opcode 171's answer).
//   hits alarm       Rides.TakesHits, the alarm countdown the XD2's fire raises.
//   touched          Kurt.Touched (what TryMount reads).
//   walker           the first XD2/XD the level spawned: where it is (1/64), yaw, flags,
//                    animation rate, health, script position, dead.
//   cam64 pitch      the follow camera's position (1/64) and its eased arena and air pitch,
//                    eased toward the arena's rest pitch (see the probe on camera_track).
//   track            the scripts' camera_track ticks (opcode 203).
//   voices           the mixer's voices: DUMMY while steering, ALERT while firing.
bool kurtRideDump(int level, int steps, const std::string& schedule, const MdkData& data,
                  const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    struct RideAction {
        int step;
        std::string kind;
        std::string arg;
        std::string arena;
        Vec3 at;
        float yaw;
    };
    std::vector<RideAction> actions;
    std::string keys;
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        std::vector<std::string> fields;
        size_t at = 0;
        while (at <= token.size()) {
            size_t colon = token.find(':', at);
            if (colon == std::string::npos) {
                colon = token.size();
            }
            fields.push_back(token.substr(at, colon - at));
            at = colon + 1;
        }
        const std::string& kind = fields[0];
        if (kind == "tp" && fields.size() == 7) {
            RideAction a;
            a.step = atoi(fields[1].c_str());
            a.kind = kind;
            a.arena = fields[2];
            a.at = Vec3{strtof(fields[3].c_str(), nullptr), strtof(fields[4].c_str(), nullptr),
                        strtof(fields[5].c_str(), nullptr)};
            a.yaw = strtof(fields[6].c_str(), nullptr);
            actions.push_back(a);
        } else if ((kind == "ride" || kind == "off" || kind == "kill" || kind == "hit") &&
                   fields.size() == 3) {
            RideAction a;
            a.step = atoi(fields[1].c_str());
            a.kind = kind;
            a.arg = fields[2];
            a.at = Vec3{0.0f, 0.0f, 0.0f};
            a.yaw = 0.0f;
            actions.push_back(a);
        } else {
            keys += (keys.empty() ? "" : ",") + token;
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(keys, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* place = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{place[0], place[1], place[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    // Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
    FollowCamera follow;
    Camera camera;

    // The probe's FirstOf: the first live object of that type, in list order.
    const auto firstOf = [&runtime](const std::string& type) -> scripts::MdkObject* {
        std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i].dead && objects[i].type_name == type) {
                return &objects[i];
            }
        }
        return nullptr;
    };

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    const scripts::MdkObject* walker = nullptr;
    char buffer[1024];
    for (int step = 1; step <= steps; step++) {
        // The probe's order: the teleports of the step, then its other actions.
        for (size_t i = 0; i < actions.size(); i++) {
            if (actions[i].step == step && actions[i].kind == "tp") {
                runtime.teleportKurt(actions[i].arena, actions[i].at, actions[i].yaw);
            }
        }
        for (size_t i = 0; i < actions.size(); i++) {
            const RideAction& a = actions[i];
            if (a.step != step || a.kind == "tp") {
                continue;
            }
            if (a.kind == "hit") {
                runtime.hurtKurt(atoi(a.arg.c_str()));
                continue;
            }
            scripts::MdkObject* obj = firstOf(a.arg);
            if (obj == nullptr) {
                continue;
            }
            if (a.kind == "ride") {
                runtime.rides().rideWalker(obj);
            } else if (a.kind == "off") {
                obj->flags &= ~scripts::Rides::kFlagRideable;
            } else if (a.kind == "kill") {
                runtime.kill(obj);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        // The arena's rest pitch, not camera_track's: see the probe (opcode 203's atan2 is
        // the LUT's here, a ULP or two off MathF.Atan2, and the eased pitch carries it).
        follow.update(&camera, &space, player, levelData.arenaPitch(runtime.currentArena()),
                      1.0f / 60.0f, 4.0f / 3.0f);

        if (walker == nullptr) {
            const std::deque<scripts::MdkObject>& objects = runtime.objects();
            for (size_t i = 0; i < objects.size() && walker == nullptr; i++) {
                if (objects[i].type_name == "XD2" || objects[i].type_name == "XD") {
                    walker = &objects[i];
                }
            }
        }
        std::string touched;
        for (size_t i = 0; i < player.touched().size(); i++) {
            touched += (i ? "|" : "") + platformName(player.touched()[i]);
        }
        char w[256];
        if (walker == nullptr) {
            snprintf(w, sizeof(w), "-");
        } else {
            snprintf(w, sizeof(w), "%s@%d,%d,%d/y%.4f/f%X/a%.4f/hp%d/r%d/d%d",
                     platformName(walker).c_str(), p64(walker->position.x),
                     p64(walker->position.y), p64(walker->position.z), walker->yaw,
                     (unsigned)walker->flags, walker->animation_rate, (int)walker->health,
                     (int)walker->restart, walker->dead ? 1 : 0);
        }
        const scripts::MdkObject* ridden = runtime.rides().ridden();
        const Kurt& combat = player.combat();
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s state=%s feet64=%d,%d,%d yaw=%.4f fs=%.4f ss=%.4f vz=%.4f "
                 "floor=%d walk=%s vis=%d keys=%d%d fire=%d hp=%d ride=%s ride171=%d hits=%d "
                 "alarm=%d touched=%s walker=%s cam64=%d,%d,%d pitch=%.4f,%.4f track=%d objs=%d "
                 "voices=",
                 step, runtime.currentArena().c_str(), kurtStateName(player.state()),
                 p64(player.feet.x), p64(player.feet.y), p64(player.feet.z), player.yaw,
                 player.forwardSpeed, player.strafeSpeed, player.verticalSpeed,
                 player.onFloor ? 1 : 0, player.walkMode == Player::WalkRiding ? "R" : "N",
                 player.visible ? 1 : 0, player.rideKeys().steer ? 1 : 0,
                 player.rideKeys().fire ? 1 : 0, combat.firing() ? 1 : 0, player.health(),
                 platformName(ridden).c_str(), walker != nullptr && ridden == walker ? 1 : 0,
                 runtime.rides().takesHits() ? 1 : 0, (int)runtime.alarmTicks,
                 touched.empty() ? "-" : touched.c_str(), w, p64(camera.position.x),
                 p64(camera.position.y), p64(camera.position.z), follow.arenaPitch(),
                 follow.airPitch(), (int)runtime.cameraTrackTicks, runtime.objectCount());
        *out += buffer;
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}
// ---- end of the rides dump (package #9) ----------------------------------------------

// ---- the snowboard dump (package #10) ------------------------------------------------
// Kurt on L4's board, end to end against the oracle probe (tests/scripts/board_probe): the
// C++ Player, ScriptRuntime and FollowCamera stepped at 1/60 s in the C# Viewer's order (Kurt
// -- whose RideHook is the board's step --, the scripts, the camera, then
// ArenaSpace::setSolid(solidArenas()), his arena alone while he rides), under the probe's
// schedule: the key holds (`key:from-to`), `tp:step:ARENA:x:y:z:yaw` (teleportKurt),
// `off:step:TYPE` (its rideable flag cleared, as its script does), `kill:step:TYPE`
// (ScriptRuntime::kill) and `hit:step:DAMAGE` (ScriptRuntime::hurtKurt), each before its
// step; `every:N` and `show:from-to` print only those steps (and the last). Its columns are
// the probe's, byte for byte:
//
//   arena solid       currentArena(), solidArenas() (one name on the board).
//   state ... floor   Kurt's state, feet (1/64), yaw, forward/strafe/vertical speeds.
//   vis pose pivot    visible, the K_SURF/K_SURFJ pose (name:frame), the camera pivot.
//   roll              the view's bank (CameraRoll.Follow on the board).
//   fire muzzle hp    the chain gun, the flash (surf: at K_SURF's -42,12), his health.
//   ride onboard hits Rides.Ridden, OnBoard, TakesHits.
//   touched           Kurt.Touched (TryMount and Ram read it).
//   board             the first XSNOWB: place (1/64), yaw, pitch, flags, path time and
//                     stop, velocity (1/64), gravity, friction, script position, dead.
//   sb                the ride (Snowboard's fields): speed, side, heading, steer, pitch,
//                     grounded, path heading, turn/jump/air ticks, pose, frame.
//   camz64 pitch      the follow camera's height (its x and y follow Kurt's facing, the
//                     LUT's here: see the probe) and its eased arena and air pitch.
//   objs voices       the object count and the mixer's voices (SKI, SKITURN, SKILAND).
bool kurtBoardDump(int level, int steps, const std::string& schedule, const MdkData& data,
                   const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    struct BoardAction {
        int step;
        std::string kind;
        std::string arg;
        std::string arena;
        Vec3 at;
        float yaw;
    };
    std::vector<BoardAction> actions;
    int every = 0;
    std::vector<std::pair<int, int> > shows;
    std::string keys;
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        std::vector<std::string> fields;
        size_t at = 0;
        while (at <= token.size()) {
            size_t colon = token.find(':', at);
            if (colon == std::string::npos) {
                colon = token.size();
            }
            fields.push_back(token.substr(at, colon - at));
            at = colon + 1;
        }
        const std::string& kind = fields[0];
        if (kind == "tp" && fields.size() == 7) {
            BoardAction a;
            a.step = atoi(fields[1].c_str());
            a.kind = kind;
            a.arena = fields[2];
            a.at = Vec3{strtof(fields[3].c_str(), nullptr), strtof(fields[4].c_str(), nullptr),
                        strtof(fields[5].c_str(), nullptr)};
            a.yaw = strtof(fields[6].c_str(), nullptr);
            actions.push_back(a);
        } else if ((kind == "off" || kind == "kill" || kind == "hit") && fields.size() == 3) {
            BoardAction a;
            a.step = atoi(fields[1].c_str());
            a.kind = kind;
            a.arg = fields[2];
            a.at = Vec3{0.0f, 0.0f, 0.0f};
            a.yaw = 0.0f;
            actions.push_back(a);
        } else if (kind == "every" && fields.size() == 2) {
            every = atoi(fields[1].c_str());
        } else if (kind == "show" && fields.size() == 2) {
            const size_t dash = fields[1].find('-');
            if (dash == std::string::npos) {
                *why = "the schedule did not parse: " + schedule;
                return false;
            }
            shows.push_back(std::make_pair(atoi(fields[1].substr(0, dash).c_str()),
                                           atoi(fields[1].substr(dash + 1).c_str())));
        } else {
            keys += (keys.empty() ? "" : ",") + token;
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(keys, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* place = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{place[0], place[1], place[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    FollowCamera follow;
    Camera camera;

    const auto firstOf = [&runtime](const std::string& type) -> scripts::MdkObject* {
        std::deque<scripts::MdkObject>& objects = runtime.objects();
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i].dead && objects[i].type_name == type) {
                return &objects[i];
            }
        }
        return nullptr;
    };
    // The probe's `o.TypeName.ToUpperInvariant() == "XSNOWB"`.
    const auto isBoard = [](const std::string& type) {
        static const char kName[] = "XSNOWB";
        if (type.size() != sizeof(kName) - 1) {
            return false;
        }
        for (size_t i = 0; i < type.size(); i++) {
            if (toupper((unsigned char)type[i]) != kName[i]) {
                return false;
            }
        }
        return true;
    };
    static const char* const kPoses[] = {"Surf", "Jump", "Land"};

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    const scripts::MdkObject* board = nullptr;
    char buffer[2048];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < actions.size(); i++) {
            if (actions[i].step == step && actions[i].kind == "tp") {
                runtime.teleportKurt(actions[i].arena, actions[i].at, actions[i].yaw);
            }
        }
        for (size_t i = 0; i < actions.size(); i++) {
            const BoardAction& a = actions[i];
            if (a.step != step || a.kind == "tp") {
                continue;
            }
            if (a.kind == "hit") {
                runtime.hurtKurt(atoi(a.arg.c_str()));
                continue;
            }
            scripts::MdkObject* obj = firstOf(a.arg);
            if (obj == nullptr) {
                continue;
            }
            if (a.kind == "off") {
                obj->flags &= ~scripts::Rides::kFlagRideable;
            } else if (a.kind == "kill") {
                runtime.kill(obj);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        follow.update(&camera, &space, player, levelData.arenaPitch(runtime.currentArena()),
                      1.0f / 60.0f, 4.0f / 3.0f);
        space.setSolid(runtime.solidArenas());

        if (board == nullptr) {
            const std::deque<scripts::MdkObject>& objects = runtime.objects();
            for (size_t i = 0; i < objects.size() && board == nullptr; i++) {
                if (isBoard(objects[i].type_name)) {
                    board = &objects[i];
                }
            }
        }
        bool shown = (every == 0 && shows.empty()) || step == steps ||
                     (every > 0 && step % every == 0);
        for (size_t i = 0; i < shows.size(); i++) {
            shown = shown || (step >= shows[i].first && step < shows[i].second);
        }
        if (!shown) {
            continue;
        }

        std::string solid;
        const std::vector<std::string>& solids = runtime.solidArenas();
        for (size_t i = 0; i < solids.size(); i++) {
            solid += (i ? "|" : "") + solids[i];
        }
        std::string touched;
        for (size_t i = 0; i < player.touched().size(); i++) {
            touched += (i ? "|" : "") + platformName(player.touched()[i]);
        }
        char b[512];
        if (board == nullptr) {
            snprintf(b, sizeof(b), "-");
        } else {
            snprintf(b, sizeof(b),
                     "%s@%d,%d,%d/y%.4f/p%.4f/f%X/t%.4f/ps%d/v%d,%d,%d/g%.4f/fr%.4f/r%d/d%d",
                     platformName(board).c_str(), p64(board->position.x), p64(board->position.y),
                     p64(board->position.z), board->yaw, board->pitch, (unsigned)board->flags,
                     board->path_time, (int)board->path_stop, p64(board->velocity.x),
                     p64(board->velocity.y), p64(board->velocity.z), board->gravity,
                     board->friction, (int)board->restart, board->dead ? 1 : 0);
        }
        char sb[512];
        const scripts::Snowboard* ride = runtime.rides().board();
        if (ride == nullptr) {
            snprintf(sb, sizeof(sb), "-");
        } else {
            snprintf(sb, sizeof(sb),
                     "v%.4f/l%.4f/h%.4f/s%.4f/p%.4f/g%d/ph%.4f/tt%.4f/jt%.4f/at%.4f/%s/fr%.4f",
                     ride->speed(), ride->side(), ride->heading(), ride->steerAngle(),
                     ride->pitch(), ride->grounded() ? 1 : 0, ride->pathHeading(),
                     ride->turnTicks(), ride->jumpTicks(), ride->airTicks(),
                     kPoses[(int)ride->pose()], ride->frame());
        }
        char pose[64];
        if (player.hasPose && player.poseName != nullptr) {
            snprintf(pose, sizeof(pose), "%s:%d", player.poseName, (int)player.poseFrame);
        } else {
            snprintf(pose, sizeof(pose), "-");
        }
        const Kurt& combat = player.combat();
        // The flash's frame and jitter are the oracle's unseeded Random's (Player's
        // _muzzleRandom note), so the probe prints only whether one is up and whether it
        // sits at K_SURF's (-42, 12) + 0-4.
        const char* muzzle = "-";
        if (combat.hasMuzzle()) {
            const Kurt::MuzzleFlash& m = combat.muzzle();
            muzzle = m.x >= -42 && m.x <= -38 && m.y >= 12 && m.y <= 16 ? "surf" : "state";
        }
        const scripts::MdkObject* ridden = runtime.rides().ridden();
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s solid=%s state=%s feet64=%d,%d,%d yaw=%.4f fs=%.4f ss=%.4f "
                 "vz=%.4f floor=%d vis=%d pose=%s pivot=%.4f roll=%.4f fire=%d muzzle=%s hp=%d "
                 "ride=%s onboard=%d hits=%d touched=%s board=%s sb=%s camz64=%d "
                 "pitch=%.4f,%.4f objs=%d voices=",
                 step, runtime.currentArena().c_str(), solid.c_str(),
                 kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.yaw, player.forwardSpeed, player.strafeSpeed,
                 player.verticalSpeed, player.onFloor ? 1 : 0, player.visible ? 1 : 0, pose,
                 player.cameraPivot, player.cameraRoll().roll(), combat.firing() ? 1 : 0, muzzle,
                 player.health(), platformName(ridden).c_str(),
                 runtime.rides().onBoard() ? 1 : 0, runtime.rides().takesHits() ? 1 : 0,
                 touched.empty() ? "-" : touched.c_str(), b, sb, p64(camera.position.z),
                 follow.arenaPitch(),
                 follow.airPitch(), runtime.objectCount());
        *out += buffer;
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}
// ---- end of the snowboard dump (package #10) -----------------------------------------

// ---- the bomber dump (package #11) --------------------------------------------------
// The XE bomber ride of L7's DANT_5, end to end against the oracle probe
// (tests/scripts/bomber_probe): the C++ Player, ScriptRuntime and FollowCamera stepped at
// 1/60 s in the C# Viewer's order (Kurt -- whose ride step is Bomber::update -- the
// scripts, whose update ends with Rides::follow, then the camera), under the probe's
// schedule: the key holds (`key:from-to`), `mouse:from-to:dx:dy` (the step's
// Input::mouseX/mouseY), `tp:step:ARENA:x:y:z:yaw` (teleportKurt), `call:step:ARENA:GROUP`
// (hitGroup(arena, group, 1, the chain gun, 0): RideTest's call of the XE),
// `board:step` (Kurt 1 unit above the first rideable XE's box, the first step from then
// on that there is one), `hit:step:DAMAGE` (hurtKurt) and `print:step` (the lines from
// that step on). Its columns are the probe's, byte for byte:
//
//   state ... floor   Kurt's state, feet (1/64 units), yaw, forward and vertical speeds.
//   vis fire hp       Kurt's Visible, the chain gun, his health.
//   ride hits         Rides.Ridden, Rides.TakesHits.
//   topdown           Kurt.TopDownHeight (Player::topDownHeight), or `-`.
//   bomber            the ride: view height / locked / hides the XE / cursor / cursor
//                     speed / bombs left / refill timer / fire held.
//   xe                the first XE: where it is (1/64), yaw, flags, health, script
//                     position, dead, visible.
//   cam64 fwd up      the camera while it looks down (LookDown): its place, forward, and
//                     `up=facing` when its up is Kurt's facing (the facing's digits are
//                     the trig table's here and MathF's in the probe, apart in the fourth
//                     place); `cam=follow` off the XE (its pitch is the table's too).
//   objs live ohp kills  the objects (and the live ones' health sum), Stats.Kills.
//   bombs             every live XBN_BOMB: where (1/64), velocity, kind, ticks, flags,
//                     pitch.
//   voices            the mixer's voices (DROP follows each bomb).
//   draw              BomberOverlay.Draw of Rides.Bomber.Shown on a 600-wide canvas: the
//                     quads' count and FNV-1a hash, and every 10th step the quads.
bool kurtBomberDump(int level, int steps, const std::string& schedule, const MdkData& data,
                    const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    struct BomberTeleport {
        int step;
        std::string arena;
        Vec3 at;
        float yaw;
    };
    struct BomberCall {
        int step;
        std::string arena;
        int32_t group;
    };
    struct BomberMouse {
        int from;
        int to;
        float x;
        float y;
    };
    std::vector<BomberTeleport> teleports;
    std::vector<BomberCall> calls;
    std::vector<std::pair<int, int32_t> > hits;
    std::vector<BomberMouse> mice;
    int board = -1;
    int from = 1;
    std::string keys;
    size_t start = 0;
    while (start < schedule.size()) {
        size_t end = schedule.find(',', start);
        if (end == std::string::npos) {
            end = schedule.size();
        }
        const std::string token = schedule.substr(start, end - start);
        start = end + 1;
        std::vector<std::string> fields;
        size_t at = 0;
        while (at <= token.size()) {
            size_t colon = token.find(':', at);
            if (colon == std::string::npos) {
                colon = token.size();
            }
            fields.push_back(token.substr(at, colon - at));
            at = colon + 1;
        }
        const std::string& kind = fields[0];
        if (kind == "tp" && fields.size() == 7) {
            BomberTeleport t;
            t.step = atoi(fields[1].c_str());
            t.arena = fields[2];
            t.at = Vec3{strtof(fields[3].c_str(), nullptr), strtof(fields[4].c_str(), nullptr),
                        strtof(fields[5].c_str(), nullptr)};
            t.yaw = strtof(fields[6].c_str(), nullptr);
            teleports.push_back(t);
        } else if (kind == "call" && fields.size() == 4) {
            BomberCall c;
            c.step = atoi(fields[1].c_str());
            c.arena = fields[2];
            c.group = atoi(fields[3].c_str());
            calls.push_back(c);
        } else if (kind == "board" && fields.size() == 2) {
            board = atoi(fields[1].c_str());
        } else if (kind == "print" && fields.size() == 2) {
            from = atoi(fields[1].c_str());
        } else if (kind == "hit" && fields.size() == 3) {
            hits.push_back(std::make_pair(atoi(fields[1].c_str()), (int32_t)atoi(fields[2].c_str())));
        } else if (kind == "mouse" && fields.size() == 4) {
            const size_t dash = fields[1].find('-');
            BomberMouse m;
            m.from = atoi(fields[1].substr(0, dash).c_str());
            m.to = atoi(fields[1].substr(dash + 1).c_str());
            m.x = strtof(fields[2].c_str(), nullptr);
            m.y = strtof(fields[3].c_str(), nullptr);
            mice.push_back(m);
        } else {
            keys += (keys.empty() ? "" : ",") + token;
        }
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(keys, &holds, &gives, &pushes) || !gives.empty() || !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    const float* place = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{place[0], place[1], place[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    // Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
    FollowCamera follow;
    Camera camera;

    // The overlay over the recording canvas: the HUD's fonts (FONTBIG texture 1, FONTSML
    // 2), then BOMBTARG 3 and CROSS 4 -- the probe's ids. The palette's bytes only reach
    // the faked index-texture call.
    HudFonts fonts;
    Messages unused;
    if (!loadHudFonts(data, &fonts, &unused, why)) {
        return false;
    }
    std::vector<uint8_t> black(768, 0);
    const Palette colours = Palette::fromRgb(black.data(), (int32_t)black.size());
    BomberOverlay overlay;
    if (!overlay.load(nullptr, *sprites, 0, colours)) {
        *why = "BOMBTARG or CROSS is missing";
        return false;
    }

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    std::deque<scripts::MdkObject>& objects = runtime.objects();
    const scripts::MdkObject* xe = nullptr;
    bool boarded = false;
    std::vector<CanvasQuad> quads;
    char buffer[2048];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, teleports[i].yaw);
            }
        }
        for (size_t i = 0; i < calls.size(); i++) {
            if (calls[i].step == step) {
                // ScriptRuntime.HitChainGun (ScriptRuntime.cs:116) is 2.
                runtime.hitGroup(calls[i].arena, calls[i].group, 1, 2, 0);
            }
        }
        // RideTest.StepBomber's boarding: once an XE is rideable, Kurt 1 unit above its box.
        if (board >= 0 && step >= board && !boarded) {
            for (size_t i = 0; i < objects.size(); i++) {
                scripts::MdkObject* o = &objects[i];
                if (!runtime.isRemoved(o) && o->type_name == "XE" &&
                    (o->flags & scripts::Rides::kFlagRideable) != 0) {
                    boarded = true;
                    const float top = runtime.worldBounds(o).max.z;
                    player.teleport(Vec3{o->position.x, o->position.y, top + 1.0f}, player.yaw);
                    break;
                }
            }
        }
        for (size_t i = 0; i < hits.size(); i++) {
            if (hits[i].first == step) {
                runtime.hurtKurt(hits[i].second);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        for (size_t i = 0; i < mice.size(); i++) {
            if (step >= mice[i].from && step < mice[i].to) {
                input.mouseX = mice[i].x;
                input.mouseY = mice[i].y;
                break;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        // The arena's rest pitch (see the rides dump on camera_track); LookDown ignores it.
        follow.update(&camera, &space, player, levelData.arenaPitch(runtime.currentArena()),
                      1.0f / 60.0f, 4.0f / 3.0f);

        for (size_t i = 0; i < objects.size() && xe == nullptr; i++) {
            if (!runtime.isRemoved(&objects[i]) && objects[i].type_name == "XE") {
                xe = &objects[i];
            }
        }
        if (step < from) {
            continue;
        }
        char x[256];
        if (xe == nullptr) {
            snprintf(x, sizeof(x), "-");
        } else {
            snprintf(x, sizeof(x), "%s@%d,%d,%d/y%.4f/f%X/hp%d/r%d/d%d/v%d",
                     platformName(xe).c_str(), p64(xe->position.x), p64(xe->position.y),
                     p64(xe->position.z), xe->yaw, (unsigned)xe->flags, (int)xe->health,
                     (int)xe->restart, xe->dead ? 1 : 0, xe->visible ? 1 : 0);
        }
        const scripts::Bomber* bomber = runtime.rides().bomberRide();
        char b[256];
        if (bomber == nullptr) {
            snprintf(b, sizeof(b), "-");
        } else {
            snprintf(b, sizeof(b), "%.4f/l%d/h%d/c%.4f,%.4f/v%.4f,%.4f/n%d/t%.4f/held%d",
                     bomber->viewHeight(), bomber->locked() ? 1 : 0, bomber->hidesXe() ? 1 : 0,
                     bomber->cursorX(), bomber->cursorY(), bomber->speedX(), bomber->speedY(),
                     (int)bomber->bombsLeft(), bomber->refill(), bomber->fireHeld() ? 1 : 0);
        }
        std::string bombs;
        int live = 0;
        int ohp = 0;
        for (size_t i = 0; i < objects.size(); i++) {
            const scripts::MdkObject& o = objects[i];
            if (runtime.isRemoved(&o) || o.dead) {
                continue;
            }
            live++;
            ohp += o.health;
            if (o.type_name == "XBN_BOMB") {
                char one[256];
                snprintf(one, sizeof(one), "%s@%d,%d,%d/v%.4f,%.4f,%.4f/k%d/t%d/f%X/p%.4f",
                         platformName(&o).c_str(), p64(o.position.x), p64(o.position.y),
                         p64(o.position.z), o.velocity.x, o.velocity.y, o.velocity.z,
                         (int)o.thrown_kind, (int)o.item_ticks, (unsigned)o.flags, o.pitch);
                bombs += (bombs.empty() ? "" : ";") + std::string(one);
            }
        }
        char top[32];
        if (player.hasTopDownHeight) {
            snprintf(top, sizeof(top), "%.4f", player.topDownHeight);
        } else {
            snprintf(top, sizeof(top), "-");
        }
        char view[192];
        if (player.hasTopDownHeight) {
            const Vec3 facing = player.facing();
            char up[64];
            if (camera.up.x == facing.x && camera.up.y == facing.y && camera.up.z == facing.z) {
                snprintf(up, sizeof(up), "facing");
            } else {
                snprintf(up, sizeof(up), "%.4f,%.4f,%.4f", camera.up.x, camera.up.y, camera.up.z);
            }
            snprintf(view, sizeof(view), "cam64=%d,%d,%d fwd=%.4f,%.4f,%.4f up=%s",
                     p64(camera.position.x), p64(camera.position.y), p64(camera.position.z),
                     camera.forward.x, camera.forward.y, camera.forward.z, up);
        } else {
            snprintf(view, sizeof(view), "cam=follow");
        }
        const Kurt& combat = player.combat();
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s state=%s feet64=%d,%d,%d yaw=%.4f fs=%.4f vz=%.4f floor=%d "
                 "vis=%d fire=%d hp=%d ride=%s hits=%d topdown=%s bomber=%s xe=%s %s "
                 "objs=%d live=%d ohp=%d kills=%d bombs=",
                 step, runtime.currentArena().c_str(), kurtStateName(player.state()),
                 p64(player.feet.x), p64(player.feet.y), p64(player.feet.z), player.yaw,
                 player.forwardSpeed, player.verticalSpeed, player.onFloor ? 1 : 0,
                 player.visible ? 1 : 0, combat.firing() ? 1 : 0, player.health(),
                 platformName(runtime.rides().ridden()).c_str(),
                 runtime.rides().takesHits() ? 1 : 0, top, b, x, view,
                 runtime.objectCount(), live, ohp, (int)runtime.stats.kills);
        *out += buffer;
        *out += bombs.empty() ? "-" : bombs;
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& playing = runtime.mixer().voices();
        for (size_t i = 0; i < playing.size(); i++) {
            voices += (i ? "," : "") + playing[i].name;
        }
        *out += " voices=" + (voices.empty() ? std::string("-") : voices);

        scripts::Bomber::Sight sight;
        if (bomber == nullptr || !bomber->shown(&sight)) {
            *out += " draw=-\n";
            continue;
        }
        quads.clear();
        g_canvasQuads = &quads;
        overlay.draw(nullptr, &fonts.big, 600.0f, sight.cursorX, sight.cursorY, sight.bombs);
        g_canvasQuads = nullptr;
        uint32_t hash = 2166136261u;
        std::string list;
        for (size_t i = 0; i < quads.size(); i++) {
            const CanvasQuad& q = quads[i];
            hash = fnvInt(hash, (uint32_t)q.texture);
            hash = fnvFloat(fnvFloat(fnvFloat(fnvFloat(hash, q.x0), q.y0), q.x2), q.y2);
            list += (i ? ";T" : "T") + std::to_string(q.texture) + ":" + hexBits(q.x0) + "," +
                    hexBits(q.y0) + "," + hexBits(q.x2) + "," + hexBits(q.y2);
        }
        snprintf(buffer, sizeof(buffer), " draw=%d:%08X", (int)quads.size(), hash);
        *out += buffer;
        if (step % 10 == 0 && !quads.empty()) {
            *out += " quads=" + list;
        }
        *out += "\n";
    }
    return true;
}
// ---- end of the bomber dump (package #11) --------------------------------------------

// Kurt in the fans' updrafts and the wind zones' slide, end to end against the oracle probe
// (tests/scripts/wind_probe): the C++ Player and ScriptRuntime stepped at 1/60 s in the C#
// Viewer's order (Kurt, the scripts, then ArenaSpace.SetSolid(SolidArenas)) from a placed
// start, under the probe's key schedule (`key:from-to`). Package 8 (docs/PORT_GAPS.md).
// Its columns are the probe's, byte for byte:
//
//   arena / second   currentArena(), secondArena() and `+` when it is active: the
//                    crossing that calls Player::enterArena, and the second arena whose
//                    fans KurtUpdraft falls back to.
//   state ... vz     Kurt's state, feet (1/64 units), yaw and vertical speed.
//   floor chute up slide   onFloor, chuteOpen, inUpdraft (Kurt.Updraft.cs), sliding.
//   sv cap left air  the slide's velocity, its speed cap, the ticks left after an arena
//                    change and the ticks in the air (Kurt.Slide.cs).
//   roll             CameraRoll: the slide's bank.
//   fans wob         the fans' count and their shared wobble (Fans.cs).
//   debris dsum dvz  the debris pieces (all / sparks): the fans' still sparks (FanSpark)
//                    and every other piece, the sums of their centres (1/64) and of their
//                    vertical speeds (1/4096 unit per tick) -- the updraft lifts them.
//   voices           the mixer's voices: FAN, CHUTEOUT, BUTSLIDE / BUTBRAKE.
bool kurtWindDump(int level, int steps, const std::string& at, const std::string& schedule,
                  const MdkData& data, const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    float place[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int placed = 0;
    {
        size_t start = 0;
        while (placed < 4 && start <= at.size()) {
            size_t end = at.find(',', start);
            if (end == std::string::npos) {
                end = at.size();
            }
            place[placed++] = (float)atof(at.substr(start, end - start).c_str());
            start = end + 1;
        }
    }
    if (placed < 3) {
        *why = "the start did not parse (want x,y,z[,yaw]): " + at;
        return false;
    }
    std::vector<ItemsHold> holds;
    std::vector<ItemsGive> gives;
    std::vector<ItemsPush> pushes;
    if (!parseItemsSchedule(schedule, &holds, &gives, &pushes) || !gives.empty() ||
        !pushes.empty()) {
        *why = "the schedule did not parse: " + schedule;
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
    Player player;
    player.feet = Vec3{place[0], place[1], place[2]};
    player.yaw = placed > 3 ? place[3] : levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) + " at=" + at +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[1024];
    for (int step = 1; step <= steps; step++) {
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        space.setSolid(runtime.solidArenas());

        long long sx = 0, sy = 0, sz = 0, vz = 0;
        int sparks = 0;
        const std::vector<scripts::Debris::Piece*>& pieces = runtime.debris().pieces();
        for (size_t i = 0; i < pieces.size(); i++) {
            sparks += pieces[i]->isSpark() ? 1 : 0;
            sx += p64(pieces[i]->center.x);
            sy += p64(pieces[i]->center.y);
            sz += p64(pieces[i]->center.z);
            vz += (long long)std::floor(pieces[i]->velocity.z * 4096.0f);
        }
        const Vec2 sv = player.slideVelocity();
        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s second=%s%s state=%s feet64=%d,%d,%d yaw=%.4f vz=%.4f floor=%d "
                 "chute=%d up=%d slide=%d sv=%.4f,%.4f cap=%.4f left=%.4f air=%.4f roll=%.4f "
                 "hp=%d fans=%d wob=%.4f debris=%d/%d dsum=%lld,%lld,%lld dvz=%lld voices=",
                 step, runtime.currentArena().empty() ? "-" : runtime.currentArena().c_str(),
                 runtime.secondArena().empty() ? "-" : runtime.secondArena().c_str(),
                 runtime.secondActive() ? "+" : "", kurtStateName(player.state()),
                 p64(player.feet.x), p64(player.feet.y), p64(player.feet.z), player.yaw,
                 player.verticalSpeed, player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0,
                 player.inUpdraft() ? 1 : 0, player.sliding() ? 1 : 0, sv.x, sv.y,
                 player.slideCap(), player.slideLeft(), player.slideAir(),
                 player.cameraRoll().roll(), player.health(), runtime.fans.count(),
                 runtime.fans.wobbleNow(), (int)pieces.size(), sparks, sx, sy, sz, vz);
        *out += buffer;
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}

// ---- The ledges (package #12) -------------------------------------------------------
// Kurt's ledge grab, hang and climb, end to end against the oracle probe
// (tests/scripts/ledge_probe): the C++ Player, ScriptRuntime and FollowCamera stepped at
// 1/60 s in the C# Viewer's order (Kurt, the scripts, ArenaSpace.SetSolid(SolidArenas),
// then the camera), under the probe's schedule -- platform_probe's key holds and
// `tp:step:ARENA:x:y:z:yaw` teleports. It pins Kurt.Ledge.cs (game/player_ledge.cpp) and
// the probes under it (ArenaSpace floorCrossing / edge / free, Bsp::edge). Its columns
// are the probe's, byte for byte:
//
//   arena state feet64 yaw   the current arena, Kurt's state, feet (1/64 units), yaw.
//   v                        forward, strafe and vertical speed: the grab zeroes them.
//   floor chute              onFloor (the grab clears it), chuteOpen.
//   frame climb              the animation frame (K_HANG's, a frame per two climb ticks)
//                            and Kurt._climbTicks.
//   hp                       health.
//   cam                      FollowCamera's climb (the smoothed rise of the feet the hang
//                            and the climb move), air pitch and arena pitch. The camera's
//                            position is not printed: its trig is the LUT.
//   voices                   the mixer's voices.
bool kurtLedgeDump(int level, int steps, const std::string& schedule, const MdkData& data,
                   const Bni* sprites, std::string* out, std::string* why) {
    if (!sprites) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    std::vector<ItemsHold> holds;
    std::vector<PlatformTeleport> teleports;
    if (!parsePlatformSchedule(schedule, &holds, &teleports)) {
        *why = "the schedule did not parse: " + schedule;
        return false;
    }
    for (size_t i = 0; i < teleports.size(); i++) {
        if (teleports[i].spawn) {
            *why = "the ledge schedule has no spawn: " + schedule;
            return false;
        }
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
    const float* start = levelData.dti().startPosition();
    Player player;
    player.feet = Vec3{start[0], start[1], start[2]};
    player.yaw = levelData.dti().startAngle();
    player.setSpace(&space);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    player.setMixer(&runtime.mixer());
    // Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
    FollowCamera follow;
    Camera camera;

    *out = "level=" + std::to_string(level) + " steps=" + std::to_string(steps) +
           " seed=1 schedule=" + schedule + "\n";
    const auto p64 = [](float v) { return (int)std::floor(v * 64.0f); };
    char buffer[768];
    for (int step = 1; step <= steps; step++) {
        for (size_t i = 0; i < teleports.size(); i++) {
            if (teleports[i].step == step) {
                runtime.teleportKurt(teleports[i].arena, teleports[i].at, teleports[i].yaw);
            }
        }
        Input input;
        for (size_t i = 0; i < holds.size(); i++) {
            if (step >= holds[i].from && step < holds[i].to) {
                input.down[holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime.update(1.0f / 60.0f);
        space.setSolid(runtime.solidArenas());
        const float goal = runtime.cameraTrackTicks > 0
                               ? runtime.cameraTrackPitch
                               : levelData.arenaPitch(runtime.currentArena());
        follow.update(&camera, &space, player, goal, 1.0f / 60.0f, 4.0f / 3.0f);

        snprintf(buffer, sizeof(buffer),
                 "s=%d arena=%s state=%s feet64=%d,%d,%d yaw=%.4f v=%.4f,%.4f,%.4f floor=%d "
                 "chute=%d frame=%.4f climb=%d hp=%d cam=%.4f,%.4f,%.4f voices=",
                 step, runtime.currentArena().empty() ? "-" : runtime.currentArena().c_str(),
                 kurtStateName(player.state()), p64(player.feet.x), p64(player.feet.y),
                 p64(player.feet.z), player.yaw, player.forwardSpeed, player.strafeSpeed,
                 player.verticalSpeed, player.onFloor ? 1 : 0, player.chuteOpen ? 1 : 0,
                 player.animationFrame(), (int)player.climbTicks(), player.health(),
                 follow.climb(), follow.airPitch(), follow.arenaPitch());
        *out += buffer;
        std::string voices;
        const std::vector<audio::SoundMixer::Voice>& live = runtime.mixer().voices();
        for (size_t i = 0; i < live.size(); i++) {
            if (i) {
                voices += ",";
            }
            voices += live[i].name;
        }
        *out += voices.empty() ? "-" : voices;
        *out += "\n";
    }
    return true;
}
// ---- end of the ledges (package #12) ------------------------------------------------

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
    runtime.option = 1;
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
    // THE FLASH SCHEDULE (golden/L3_kurt_flashes.txt). Nothing printed a flash before,
    // so every writer of the two fields was unpinned and neither side ever faded one.
    // What each number is for:
    //   5: an opcode-215 white flash of 60. It falls by Kurt::kFlashFade (4 per tick,
    //      120/s) and stops at zero, which is the whole of what screen_flash (215) ever
    //      did on its own: without the fade the screen is white for the rest of the level.
    //  10: a hurt of 1, whose flash is clamp(25, 75, 180) = 75 -- the MIN arm of the
    //      damage flash, and small enough to be gone (floored at zero) before the next
    //      writer arrives, so the two rises never overlap.
    //  60: lifted 100 units above the pad. The fall ends in a hard landing (the slam
    //      needs VerticalSpeed under -100), which is the only caller of Player::damage
    //      and therefore the only way this port reaches Kurt::damage from the movement
    //      half: its flash is clamp(0 + 10*25, 75, 180) = 180, the MAX arm, and it is
    //      the term the port used to leave out because nothing faded it.
    // 320: a hurt of 4 on the pad, after K_CRASHL has let him up again, so the red flash
    //      is up (100) when the next line arrives.
    // 330: Kurt.SetHealth(0) on a floor. The death transition zeroes the flash
    //      (Kurt.Combat.cs:375) and the skull starts from that zero; a port that drops
    //      the zeroing starts the beat at ~80 and ends it 40 steps early.
    // 400: an opcode-215 white flash of 200 on a CORPSE, sixty-nine steps into the death
    //      beat. UpdateDeath returns before FadeFlashes (Kurt.cs:152, ahead of :190), so a
    //      corpse fades nothing and this value stands to the last line of the golden. It
    //      is the only line that tells a corpse that fades nothing from one that fades
    //      quietly, and it is white on purpose: the red one is zeroed by the transition and
    //      only a hard landing writes it after death, which a frozen corpse cannot do.
    // Then the skull rises one unit a step, 255 steps to fill SkullEnd, the step it passes
    // the end is the C#'s Died event (died= goes to 1 once), and from the next step the
    // corpse is frozen -- vert, the feet and the floor column stop moving, which is the
    // guard at Kurt.Combat.cs:238 and nothing else.
    static const KurtHurt kFlashHurts[] = {{10, 1}, {320, 4}};
    static const KurtFlashSet kFlashSets[] = {{5, 60.0f}, {400, 200.0f}};
    // THE PUSH SCHEDULE (golden/L3_kurt_push.txt). Opcode 248 is the only writer of a
    // non-zero knock-down push in the whole oracle, so this is the golden that pins the
    // push's own arithmetic, its drain (Kurt::kPushDrain, 1.5 u/s a step) and the two
    // arms of the guard that refuses a shove. What each line is for:
    //   20: mode 0, a=20 b=5 up=3 on an object yawed 37 degrees -- every operand of the
    //       frame branch non-zero, because a schedule whose accepted mode-0 push carries
    //       b=0 and up=0 cannot tell the oracle's formula from a "corrected" one or the
    //       args[3] lift from args[2]. The formula is ASYMMETRIC in the oracle (x is
    //       -a*c - b*s, y is -b*c - a*s) and is copied rather than corrected: a port that
    //       "fixes" it into a rotation lands a different vector in the same number of
    //       steps, and vel= says so; a port that reads the lift from args[2] lifts him 5
    //       instead of 3, and vert= says so.
    //   40: mode 0 again, mid-K_BANG. Refused by the Knocked arm of the guard, so vel=
    //       must keep falling by the drain alone and must not jump to the new push.
    //  130: mode 1, a=15 up=8, the object 5 units in -X of Kurt. The away branch: away
    //       is (5, 0), normalized and scaled to (15, 0), and the +8 lift takes him off
    //       the floor while he is on his back -- the only fall this opcode can make.
    //  240: mode 1, a=12 up=0, the object EXACTLY on the runtime's KurtPosition. away is
    //       zero, so the oracle takes its own fallback and shoves him along +X at 12;
    //       nothing else in the port reaches that branch.
    //  350: Kurt.SetHealth(0) on a floor, so the death transition runs and the next line
    //       is refused by the health arm of the guard and not by the state arm.
    //  355: mode 0, a=30 up=20 on a corpse. vel= must not move: a port that drops the
    //       health test shoves a dead man 20 units up.
    static const KurtKeyWindow kRollRight[] = {{10, 99}, {180, 189}, {192, 196}};
    static const KurtKeyWindow kRollLeft[] = {{100, 179}, {185, 189}};
    static const KurtPushSet kPushSets[] = {
        {20, 0, 20.0f, 5.0f, 3.0f, 37.0f, 0.0f, 0.0f},
        {40, 0, 25.0f, 5.0f, 3.0f, 200.0f, 0.0f, 0.0f},
        {130, 1, 15.0f, 0.0f, 8.0f, 0.0f, -5.0f, 0.0f},
        {240, 1, 12.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {355, 0, 30.0f, 0.0f, 20.0f, 15.0f, 0.0f, 0.0f},
    };
    KurtHurtPlan plan = {300.0f, kHealthHurts, sizeof(kHealthHurts) / sizeof(kHealthHurts[0]),
                         nullptr, 0, nullptr, 0, 0, kColsVoices};
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
    } else if (schedule == kHurtFlashes) {
        plan.lift = 0.0f;
        plan.hurts = kFlashHurts;
        plan.hurtCount = sizeof(kFlashHurts) / sizeof(kFlashHurts[0]);
        plan.flashes = kFlashSets;
        plan.flashCount = sizeof(kFlashSets) / sizeof(kFlashSets[0]);
        plan.airLift = 100.0f;
        plan.airLiftStep = 60;
        plan.mortalStep = 330;
        plan.columns = kColsFlash;
    } else if (schedule == kHurtPush) {
        plan.lift = 0.0f;
        // No hurts at all: the default schedule would knock him down on its own and the
        // whole point of this one is that opcode 248 is the only thing that does it.
        plan.hurts = nullptr;
        plan.hurtCount = 0;
        plan.pushes = kPushSets;
        plan.pushCount = sizeof(kPushSets) / sizeof(kPushSets[0]);
        plan.mortalStep = 350;
        plan.columns = kColsPush;
    } else if (schedule == kHurtRoll) {
        // The bank schedule. Every number here is a KEY window, not a roll value:
        // the oracle probe presses the same keys and prints what CameraRoll ends up
        // holding, so the golden pins the input-to-bank path and not just the maths.
        //   forward 1..189: the walk axis the writer needs, held long enough to reach
        //     its own +/-10 limit and to fall out at 190 while the keys are still up.
        //   turnRight 10..99: ninety steps of +0.125 degrees, so the limit is reached
        //     (at step 89, after eighty increments) and then held against for ten more --
        //     a cap that only just touches its limit would not be pinned.
        //   turnLeft 100..179: the other way from +10, which is where the 2-degree
        //     "back towards level" jump is biggest, and long enough to reach -10 too.
        //   turnRight 180..189: the jump again from -10, crossing zero.
        //   turnLeft 185..189 OVERLAPS it: both keys is the C#'s axis of zero, so the
        //     writer must refuse while the leveller keeps running.
        //   turnRight 192..196 with the walk key released: the OTHER guard, the one on
        //     the walk axis, which nothing else in the port reaches.
        plan.lift = 0.0f;
        plan.hurts = nullptr;
        plan.hurtCount = 0;
        plan.forwardFrom = 1;
        plan.forwardTo = 189;
        plan.turnRight = kRollRight;
        plan.turnRightCount = sizeof(kRollRight) / sizeof(kRollRight[0]);
        plan.turnLeft = kRollLeft;
        plan.turnLeftCount = sizeof(kRollLeft) / sizeof(kRollLeft[0]);
        plan.columns = kColsRoll;
    }
    *out = dumpKurtHurt(level, steps, runtime, player, mixer, plan);
    return true;
}

// THE DOORWAY-CROSSING GOLDEN — golden/L4_crossing.txt and golden/L7_crossing.txt.
//
// WHAT IT PINS. That a move which crosses a doorway connection changes the arena the
// scripts run, and that a move which does not, does not. This is the collision world
// the level is walked in: while CurrentArena was written only on the first tick, Kurt
// kept colliding with the arena he was born in after he walked through a door, which
// is review-6 F1's fall through the room an open door led to.
//
// WHERE THE EXPECTED VALUES COME FROM — the oracle's OWN test suite, and the two
// lines that are NOT from it. Most lines of the two goldens transcribe one assertion
// of tests/Mdk.Game.Tests/ArenaSwitchTests.cs or DoorArenaTests.cs, cited at the line
// with the file:line of the ASSERTION itself. (Review-1 of run-20261110-230107 F1: an
// earlier draft cited where the C# test happened to start instead — a comment line or
// a StepTo call in four places — so the record could not be checked against the file
// it cited. Every citation below is the assertion's own line, re-read from the C#.)
// No C# was run to make this golden: the .NET SDK this loop used before
// (/tmp/kilo/dotnet10) is gone from this host, so transcribing the committed,
// read-only C# tests is the available evidence, and transcription is weaker than
// measurement. Those files were written against the original game — ArenaSwitchTests'
// own comment is the playtest that found the bug: "on level 4's MEAT_7 floor, just
// before the doorway to CMEAT_7 (connection 1012, the plane y = 14822), he was
// already in the corridor".
//
// TWO LINES ARE NOT ORACLE ASSERTIONS, and they are named here instead of hidden:
//   * `L4 s=1 current` — the C# names its first arena by TELEPORTING into it
//     (ArenaSwitchTests.cs:53 `TeleportKurt(Room, …)`) and asserts nothing after that
//     first tick; there is no assertion of CurrentArena after a bare teleport anywhere
//     in the suite. The port has no TeleportKurt, so this harness lets the first tick
//     find the arena geometrically and the line pins that the two agree on MEAT_7.
//     That agreement is the same room reached by two rules, not an oracle assertion.
//   * `L7 s=675 …` — an intermediate state of DoorArenaTests' own walk loop (:76-79),
//     which the C# asserts only at its end (:81). Its value is the C#'s crossing
//     predicate applied to the shipped DTI (connection 1001, plane y = 673, so the
//     step to y = 674 crosses), and it is in the golden because it is the line that
//     separates the crossing rule from a geometric arena lookup: at y = 675 the
//     crossing answer is DANT_2 while the box under the feet is still CDANT_1, so an
//     implementation that took the arena geometrically every tick reddens HERE. L4's
//     s=2 is the same discriminator on the other level; without s=675 the L7 case
//     passes a geometric implementation unchanged (review-1 F3).
// The `doorway=` header lines are not assertions either: they name the constants the
// C# test itself declares (ArenaSwitchTests.cs:20-21, DoorArenaTests.cs:24,27) and the
// DTI connection id, so a reader can tell which doorway the case is about.
//
// HOW THE TWO SCENARIOS ARE DRIVEN, and the one place that differs from the C# test.
// Both tests place Kurt with Kurt.Teleport (or ScriptRuntime.TeleportKurt) and then
// tick; this harness writes Player::feet and ticks, which is the same observable
// thing — a feet position and a tick, with no movement in between. TeleportKurt also
// sets CurrentArena directly, and teleport is not ported; where the C# test names the
// arena it teleports into, this harness lets the first tick find it geometrically and
// the golden pins that the two agree (level 4's spawn point answers MEAT_7 and level
// 7's answers CDANT_1, measured). What the teleport would still add — an arena no
// doorway leads to — is out of reach here and is recorded as unported, not skipped
// silently.
bool crossingDump(int level, const MdkData& data, const Bni* sprites, std::string* out,
                  std::string* why) {
    // The two scenarios, straight out of the oracle's tests.
    const float kL4Doorway = 14822.0f;  // ArenaSwitchTests.Doorway
    const float kL4Floor = 21.0f;       // ArenaSwitchTests.Floor
    const float kL7Floor = -3.0f;       // DoorArenaTests.Floor

    LevelData levelData;
    if (!LevelData::load(&levelData, &data, level)) {
        *why = "the level did not load";
        return false;
    }
    char cmiPath[128];
    snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
    std::vector<uint8_t> cmiBytes;
    if (!data.read(cmiPath, &cmiBytes)) {
        *why = "the CMI did not read";
        return false;
    }
    Cmi cmi;
    if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
        *why = "the CMI did not parse";
        return false;
    }
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the sound bank did not load";
        return false;
    }
    bank.preload();

    // The Player the runtime reads its Kurt from. It is never stepped — the walk is
    // the harness writing feet, which is what Kurt.Teleport amounts to for the arena
    // rule under test — but it is wired the way the kurt goldens wire theirs so that
    // anything a script does to Kurt lands on a real entity.
    ArenaSpace space;
    for (size_t i = 0; i < levelData.arenas().size(); i++) {
        const Arena* arena = levelData.arenas()[i];
        if (levelData.isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(&bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    player.setFrameCounts(kurtFrameCounts(*sprites));

    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    char line[256];
    if (level == 4) {
        snprintf(line, sizeof(line),
                 "L4 doorway=1012 plane_y=%.4f floor_z=%.4f\n", (double)kL4Doorway,
                 (double)kL4Floor);
        *out += line;

        // s=1 — NOT an oracle assertion (see the header). The C# reaches this state by
        // TeleportKurt(MEAT_7, (0, Doorway-100, Floor), 90) at ArenaSwitchTests.cs:53
        // and asserts nothing until :58; the arena is named by the teleport, not
        // derived. This harness has no teleport, so the first tick finds the arena
        // geometrically and the line pins that the two agree on MEAT_7.
        player.feet = Vec3{0.0f, kL4Doorway - 100.0f, kL4Floor};
        player.yaw = 90.0f;
        runtime.update(1.0f / 30.0f);
        snprintf(line, sizeof(line), "L4 s=1 current=%s\n", runtime.currentArena().c_str());
        *out += line;

        // FirstTickHasNoMove — ArenaSwitchTests.cs:81 `Assert.Equal(Vector3.Zero,
        // runtime.KurtVelocity)`: the first tick has no move to observe, so the jump
        // from wherever Kurt was parked is not a velocity.
        snprintf(line, sizeof(line), "L4 s=1 velocity=%.4f,%.4f,%.4f\n",
                 (double)runtime.kurtVelocity.x, (double)runtime.kurtVelocity.y,
                 (double)runtime.kurtVelocity.z);
        *out += line;

        // s=2 — :58 `Assert.Equal(Room, runtime.CurrentArena)`, after StepTo(:57) put
        // him at Doorway-1: inside the corridor's BOX (the geometric arena here is
        // CMEAT_7, measured) but the move did not cross the doorway, so the scripts
        // stay in the room. This is the whole bug, and this is the line that catches it.
        player.feet = Vec3{0.0f, kL4Doorway - 1.0f, kL4Floor};
        runtime.update(1.0f / 30.0f);
        snprintf(line, sizeof(line), "L4 s=2 current=%s\n", runtime.currentArena().c_str());
        *out += line;

        // s=3 — :62 `Assert.Equal(Corridor, runtime.CurrentArena)`,
        // :63 `Assert.Equal(Room, runtime.SecondArena)`,
        // :64 `Assert.True(runtime.SecondActive)`: through the doorway at Doorway+3,
        // and the room stays the active second.
        player.feet = Vec3{0.0f, kL4Doorway + 3.0f, kL4Floor};
        runtime.update(1.0f / 30.0f);
        snprintf(line, sizeof(line), "L4 s=3 current=%s\n", runtime.currentArena().c_str());
        *out += line;
        snprintf(line, sizeof(line), "L4 s=3 second=%s\n", runtime.secondArena().c_str());
        *out += line;
        snprintf(line, sizeof(line), "L4 s=3 second_active=%d\n", runtime.secondActive() ? 1 : 0);
        *out += line;

        // s=4 — :68 `Assert.Equal(Room, runtime.CurrentArena)` and
        // :69 `Assert.Equal(Corridor, runtime.SecondArena)`: and back.
        player.feet = Vec3{0.0f, kL4Doorway - 1.0f, kL4Floor};
        runtime.update(1.0f / 30.0f);
        snprintf(line, sizeof(line), "L4 s=4 current=%s\n", runtime.currentArena().c_str());
        *out += line;
        snprintf(line, sizeof(line), "L4 s=4 second=%s\n", runtime.secondArena().c_str());
        *out += line;
        return true;
    }

    // Level 7: DoorArenaTests.DoorShowsTunnelOnTheWayBack. The walk is that test's
    // own loop — `for (var y = 655f; y < NearDoor.Y; y += 1f) StandAt((0,y,-3), 1)`,"
    // i.e. one tick per unit of y from 655 up to but not including 688 — and the one
    // assertion is :81 `Assert.Equal(Room, runtime.CurrentArena)`, Room = DANT_2.
    // The doorway is CDANT_1's connection 1001, the plane y = 673, so the crossing
    // happens on the step to y = 674 (the C#'s predicate is `to.Y > min.Y`, strict).
    snprintf(line, sizeof(line), "L7 doorway=1001 plane_y=673.0000 floor_z=%.4f\n",
             (double)kL7Floor);
    *out += line;
    player.yaw = 90.0f;
    for (float y = 655.0f; y < 688.0f; y += 1.0f) {
        player.feet = Vec3{0.0f, y, kL7Floor};
        runtime.update(1.0f / 30.0f);
        // The discriminator, one step into the corridor (see the header): the crossing
        // rule has had him in DANT_2 since the step to 674, while the arena BOX under
        // his feet does not change until y = 676. Printing both is the point — a tick
        // that took the arena geometrically answers CDANT_2 here and reddens the
        // golden, which is the failure the end-of-walk line alone cannot see.
        if (y == 675.0f) {
            const std::string* box = space.arenaAt(
                Vec{player.feet.x, player.feet.y, player.feet.z});
            snprintf(line, sizeof(line), "L7 s=675 current=%s geometry=%s\n",
                     runtime.currentArena().c_str(), box ? box->c_str() : "(none)");
            *out += line;
        }
    }
    snprintf(line, sizeof(line), "L7 walk_655_to_687 current=%s\n",
             runtime.currentArena().c_str());
    *out += line;
    return true;
}

// THE DOOR BUBBLE'S SECOND HOP (ScriptRuntime::doorBubbleArenas): LEVEL4's CMEAT_5
// is 31 units door to door, so from MEAT_6 at its doorway the bubble must name the
// arena past it, MEAT_5, after CMEAT_5 itself -- the view prebuild's lead (on the Old
// 3DS MEAT_5's 148 build steps were otherwise finished synchronously as Kurt stepped
// in, 257 ms, 2026-11-27). With a radius that reaches only the near doorway, the far
// side is not named. Frontend residency only: no script state is read or written.
int bubbleSecondHopCheck(const MdkData& data, const Bni* sprites) {
    const int level = 4;
    LevelData levelData;
    char cmiPath[128];
    snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
    std::vector<uint8_t> cmiBytes;
    Cmi cmi;
    audio::SoundBank bank;
    if (!LevelData::load(&levelData, &data, level) || !data.read(cmiPath, &cmiBytes) ||
        !Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size()) ||
        !audio::SoundBank::forLevel(&bank, data, level) || sprites == nullptr) {
        printf("FAIL L4_bubble_second_hop: the level did not load\n");
        return 1;
    }
    bank.preload();
    ArenaSpace space;
    for (size_t i = 0; i < levelData.arenas().size(); i++) {
        const Arena* arena = levelData.arenas()[i];
        if (levelData.isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(&bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    player.setFrameCounts(kurtFrameCounts(*sprites));
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, sprites);
    runtime.option = 1;
    // The hardware route's start (AT=-126,13440,-1989,270 ARENA=MEAT_6): in MEAT_6,
    // at its CMEAT_5 doorway.
    runtime.teleportKurt("MEAT_6", Vec3{-126.0f, 13440.0f, -1989.0f}, 270.0f);
    player.feet = Vec3{-126.0f, 13440.0f, -1989.0f};
    runtime.update(1.0f / 30.0f);
    std::vector<std::string> wide;
    runtime.doorBubbleArenas(player.feet, 220.0f, &wide);
    std::vector<std::string> narrow;
    // 50 units: past the CMEAT_5 doorway (30-40 away, measured), short of MEAT_5's
    // (60-80 away): the near connector alone.
    runtime.doorBubbleArenas(player.feet, 50.0f, &narrow);
    std::string got = "current=" + runtime.currentArena() + " wide=";
    for (size_t i = 0; i < wide.size(); i++) {
        got += (i ? "," : "") + wide[i];
    }
    got += " narrow=";
    for (size_t i = 0; i < narrow.size(); i++) {
        got += (i ? "," : "") + narrow[i];
    }
    size_t corridor = wide.size();
    size_t room = wide.size();
    for (size_t i = 0; i < wide.size(); i++) {
        corridor = wide[i] == "CMEAT_5" ? i : corridor;
        room = wide[i] == "MEAT_5" ? i : room;
    }
    bool narrowHasRoom = false;
    bool narrowHasCorridor = false;
    for (size_t i = 0; i < narrow.size(); i++) {
        narrowHasRoom = narrowHasRoom || narrow[i] == "MEAT_5";
        narrowHasCorridor = narrowHasCorridor || narrow[i] == "CMEAT_5";
    }
    if (runtime.currentArena() == "MEAT_6" && corridor < room && room < wide.size() &&
        narrowHasCorridor && !narrowHasRoom) {
        printf("PASS L4_bubble_second_hop: %s\n", got.c_str());
        return 0;
    }
    printf("FAIL L4_bubble_second_hop: %s (want current=MEAT_6, CMEAT_5 then MEAT_5 at 220, "
           "CMEAT_5 and no MEAT_5 at 50)\n", got.c_str());
    return 1;
}

// The crossing goldens: one dump per level, compared to the oracle's recorded
// assertions. Returns the number of failures; prints one PASS/FAIL line per level.
std::string firstDifference(const std::string& got, const std::string& expected);  // below
int crossingCheck(const MdkData& data, const Bni* sprites) {
    int failures = 0;
    const int levels[] = {4, 7};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        const int level = levels[i];
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_crossing.txt", level);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_crossing: no golden file %s\n", level, goldenPath);
            failures++;
            continue;
        }
        std::string got, why;
        if (!crossingDump(level, data, sprites, &got, &why)) {
            printf("FAIL L%d_crossing: %s\n", level, why.c_str());
            failures++;
            continue;
        }
        if (got == expected) {
            // The PASS line must not claim a provenance the artifact cannot show.
            // A golden here is compared BYTE FOR BYTE (`got == expected` below), so it
            // cannot carry a header or any other prose: a comment added to
            // golden/L4_crossing.txt would fail the case it documented. The lines that
            // are not oracle assertions are therefore named where the dump is written
            // -- the header comment above crossingDump -- and this says so rather than
            // pointing at a header that does not exist (review-3 F1).
            printf("PASS L%d_crossing: the ported doorway crossing answers the oracle's "
                   "own arena-switch / door-arena assertions (the lines that are NOT "
                   "oracle assertions are named above crossingDump, where the dump is "
                   "written: a golden is compared byte for byte and cannot carry "
                   "prose)\n", level);
        } else {
            printf("FAIL L%d_crossing: %s\n", level, firstDifference(got, expected).c_str());
            failures++;
        }
    }
    return failures;
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
        case Pass::Count: break;  // the cardinality sentinel, never a name
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

// The bit pattern of a float / double, for digests that must be exact. A decimal
// float would round away the only thing being pinned.
static uint32_t bitsOf(float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static uint64_t bitsOfD(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

// The audio SAMPLE PLANE, digested one update per line; golden/L3_audio_mix.txt is
// what this prints. The oracle twin is the C# AudioDevice's own mix (the scratch
// driver /tmp/mdk-pi/obj3/MixProbe, which calls the real AudioDevice and reads its
// private _owed and _mix by reflection rather than recomputing anything), and what
// the lines pin is the arithmetic nothing else can see: Sound::fromPcm's two PCM
// widths, MixVoice's interpolation and pan law, AddMusic's one-pole low-pass, the
// bus and master gains, and the limiter.
//
// The plays are driven through SoundDevice directly rather than through SoundMixer on
// purpose: the mixer's 3D listener math is pinned by the Kurt goldens, and routing
// these through it would mix the same samples under the gain the SNI table happens to
// carry instead of under gains chosen to cover the cases. Every number below is
// chosen, not sampled.
bool audioMixDump(int level, int updates, const MdkData& data, std::string* out,
                  std::string* why) {
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the level has no sound archives";
        return false;
    }

    struct Play {
        const char* name;
        float gain, pitch, pan;
        int32_t bus, at;
    };
    // Chosen to cover both PCM widths (FOOT1 16-bit / EXPLODE 8-bit), both buses, both
    // loop states (CHUTEON loops, FOOT1 does not), a voice stopped mid-run, pans at
    // each extreme and past it, pitches away from 1, and enough level to push the mix
    // over the limiter's ceiling.
    //
    // NO VOICE IS ALLOWED TO END MID-RUN, and that is a fidelity decision, not a
    // convenience. The C# sums voices in the enumeration order of a
    // `Dictionary<int, Voice>` it mutates while enumerating, and .NET documents that
    // order as unspecified: removing a voice frees a slot and the next `Play` is
    // inserted there ([1,2,3] -> stop 2 -> play 4 gives [1,4,3]), while this port's
    // vector erases and appends ([1,3,4]). Float addition is not associative, so the
    // samples differ afterwards.
    //
    // THE SIZE OF THE DIFFERENCE, measured (review-1 F3, with an independent twin
    // written from the C# text; the earlier claim here of "1-2 ULP" was wrong and is
    // replaced by the measurement): on this schedule with FOOT1 at pitch 4 ending at
    // update 4 and replaying at 10 and 40, 52 of the 120 updates differ, by at most
    // 1.788e-07 against a 0.944 peak (-134 dBFS) and by up to 6698 ULP on a single
    // sample. Inaudible, and not closeable without pinning .NET's internal slot
    // layout, which is not the oracle's behaviour either. Pinning the schedule that
    // triggers it would pin the container, so the schedule keeps every voice alive
    // (the non-looping ones at a pitch low enough to outlast the run) and the order
    // pinned is the one the oracle actually specifies: the order `Play` was called.
    // An explicit stop() has always been in this schedule and is unaffected: the
    // difference is only in the order the SURVIVING voices are summed in.
    const Play plays[] = {
        {"FOOT1", 1.0f, 0.25f, 0.00f, audio::SoundDevice::Effects, 0},
        {"APPLE", 0.5f, 0.50f, -0.75f, audio::SoundDevice::Effects, 0},
        {"CHUTEON", 0.8f, 1.50f, 0.60f, audio::SoundDevice::Effects, 1},
        {"H10", 0.7f, 1.00f, 0.00f, audio::SoundDevice::Music, 2},
        {"EXPLODE", 1.4f, 0.50f, 0.00f, audio::SoundDevice::Effects, 3},
        {"SIREN", 0.6f, 2.00f, -1.00f, audio::SoundDevice::Music, 5},
        {"GATTFIRE", 0.9f, 1.25f, 1.00f, audio::SoundDevice::Effects, 8},
        {"COW", 2.0f, 0.25f, 0.00f, audio::SoundDevice::Effects, 12},
    };

    char line[512];
    std::string o;
    snprintf(line, sizeof(line), "mix level=%d updates=%d delta=1/60 rate=44100 plays=", level,
             updates);
    o += line;
    for (size_t i = 0; i < sizeof(plays) / sizeof(plays[0]); i++) {
        snprintf(line, sizeof(line), "%s%s:%.2f,%.2f,%.2f,%c@%d", i ? ";" : "", plays[i].name,
                 (double)plays[i].gain, (double)plays[i].pitch, (double)plays[i].pan,
                 plays[i].bus == audio::SoundDevice::Music ? 'M' : 'E', (int)plays[i].at);
        o += line;
    }
    o += " set=6:0.30,2.00,0.40 busmusic=15:0.50 master=30:0.80 stop=34 stopall=35\n";

    audio::SoundDevice device;
    std::vector<int32_t> ids;
    for (int step = 0; step < updates; step++) {
        for (const Play& p : plays) {
            if (p.at != step) {
                continue;
            }
            audio::SoundBank::Entry entry;
            if (!bank.get(p.name, &entry)) {
                snprintf(line, sizeof(line), "MISSING %s\n", p.name);
                o += line;
                continue;
            }
            ids.push_back(device.play(entry, p.gain, p.pitch, p.pan, p.bus));
        }
        // Set on a live voice, the music bus's gain, the master gain, and a Stop — of
        // the LAST-added voice, which is last in call order, so removing it cannot
        // reorder the others either.
        if (step == 6 && !ids.empty()) {
            device.set(ids[0], 0.3f, 2.0f, 0.4f);
        }
        if (step == 15) {
            device.setBusGain(audio::SoundDevice::Music, 0.5f);
        }
        if (step == 30) {
            device.setMasterGain(0.8f);
        }
        if (step == 34 && !ids.empty()) {
            device.stop(ids.back());
        }
        if (step == 35) {
            device.stopAll();
        }
        device.update(1.0f / 60.0f);

        const int32_t frames = device.mixedFrames();
        const float* mix = device.mix();
        // Exactly the C#'s digest: a double accumulator over the update's samples, and
        // a min/max that start at +Inf/-Inf and compare as FLOATS (so a NaN would leave
        // them at the infinities in both languages, which is why they are not compared
        // as bits). Printed as bit patterns, because a decimal float would round away
        // the only thing being pinned.
        double sum = 0.0;
        float mn = INFINITY, mx = -INFINITY;
        for (int32_t i = 0; i < frames * 2; i++) {
            const float v = mix[i];
            sum += v;
            if (v < mn) {
                mn = v;
            }
            if (v > mx) {
                mx = v;
            }
        }
        int live = 0;
        for (int32_t id : ids) {
            if (device.isPlaying(id)) {
                live++;
            }
        }
        snprintf(line, sizeof(line), "u=%d frames=%d live=%d sum=%016llX min=%08X max=%08X head=",
                 step, frames, live, (unsigned long long)bitsOfD(sum), bitsOf(mn), bitsOf(mx));
        o += line;
        for (int i = 0; i < 4 && i < frames * 2; i++) {
            snprintf(line, sizeof(line), "%s%08X", i ? "," : "", bitsOf(mix[i]));
            o += line;
        }
        o += " tail=";
        for (int i = frames * 2 - 4; i < frames * 2; i++) {
            snprintf(line, sizeof(line), "%s%08X", i == frames * 2 - 4 ? "" : ",", bitsOf(mix[i]));
            o += line;
        }
        o += "\n";
    }
    *out = o;
    return true;
}

// `tick1 mixmeta <level>` — the decoded sample plane per sound, so a mix divergence
// can be localised to the decode or to the mixer. Oracle twin is the same digest out
// of the C# SoundBank (Sound.FromPcm over the same WAV bytes).
bool audioMetaDump(int level, const MdkData& data, std::string* out, std::string* why) {
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the level has no sound archives";
        return false;
    }
    const char* names[] = {"FOOT1", "APPLE", "CHUTEON", "H10", "EXPLODE",
                           "SIREN", "GATTFIRE", "COW"};
    char line[512];
    std::string o;
    snprintf(line, sizeof(line), "mixmeta level=%d sounds=", level);
    o += line;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(line, sizeof(line), "%s%s", i ? "," : "", names[i]);
        o += line;
    }
    o += "\n";

    for (const char* name : names) {
        audio::SoundBank::Entry entry;
        if (!bank.get(name, &entry) || !entry.sound) {
            snprintf(line, sizeof(line), "MISSING %s\n", name);
            o += line;
            continue;
        }
        const audio::Sound& s = *entry.sound;
        // Read through sampleAt(), the same conversion the mix uses, so this
        // pins what a voice actually hears and not the store's raw ints.
        double sum = 0.0;
        for (size_t i = 0; i < s.samples.size(); i++) {
            sum += s.sampleAt((int32_t)i);
        }
        const size_t n = s.samples.size();
        snprintf(line, sizeof(line),
                 "sound=%s loop=%d ch=%d rate=%d frames=%d len=%zu sum=%016llX s0=%08X s1=%08X "
                 "mid=%08X last=%08X\n",
                 name, entry.looping ? 1 : 0, s.channels, s.sampleRate,
                 (int)(n / (size_t)(s.channels ? s.channels : 1)), n,
                 (unsigned long long)bitsOfD(sum), bitsOf(s.sampleAt(0)),
                 bitsOf(s.sampleAt((int32_t)(n > 1 ? 1 : 0))),
                 bitsOf(s.sampleAt((int32_t)(n / 2))), bitsOf(s.sampleAt((int32_t)(n - 1))));
        o += line;
    }
    *out = o;
    return true;
}

// `tick1 mixsize <level>` — what a fully decoded bank costs, measured on the build
// that prints it. The element size is read out of the store itself, so `elem=` and
// `bytes=` are the store's width and its payload rather than a restatement of a
// comment, and two builds with different stores print different lines from the same
// data. `archive=` is the raw SNI blobs, which stay resident after the decode: the
// console has to fit BOTH, plus the level's geometry and textures, into FCRAM.
bool audioSizeDump(int level, const MdkData& data, std::string* out, std::string* why) {
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the level has no sound archives";
        return false;
    }
    bank.preload();
    char line[256];
    std::string o;
    snprintf(line, sizeof(line),
             "mixsize level=%d sounds=%d samples=%zu elem=%zu decoded=%zu archive=%zu total=%zu\n",
             level, (int)bank.soundsDecoded(), bank.decodedSamples(),
             sizeof(audio::Sound::Samples::value_type), bank.decodedBytes(),
             bank.archiveBytes(), bank.decodedBytes() + bank.archiveBytes());
    o += line;
    *out = o;
    return true;
}

// `tick1 sfxset <level>` -- the level's archive inventory and what the 3DS's DSP
// sample arena would hold if every effect were copied in at load: per archive,
// its sound entries split music / effect; then every effect name the bank can
// answer (SoundBank::effectNames), its decoded int16 bytes, and its arena span
// as audio_dsp_3ds.cpp stores it (PCM8 when every sample's low byte is zero,
// else PCM16; 32-byte aligned).
bool sfxSetDump(int level, const MdkData& data, std::string* out, std::string* why) {
    audio::SoundBank bank;
    if (level == 0) {
        // The fall's one archive (fall_screen.cpp).
        std::vector<formats::Sni> archives(1);
        std::vector<uint8_t> bytes;
        if (!data.read("FALL3D/FALL3D.SNI", &bytes) ||
            !formats::Sni::parse(&archives[0], std::move(bytes))) {
            *why = "FALL3D/FALL3D.SNI did not load";
            return false;
        }
        audio::SoundBank::withArchives(&bank, std::move(archives));
    } else if (!audio::SoundBank::forLevel(&bank, data, level)) {
        *why = "the level has no sound archives";
        return false;
    }
    char line[256];
    std::string o;
    std::vector<std::string> names;
    bank.effectNames(&names);
    size_t decoded = 0;
    size_t arena = 0;
    int32_t pcm16 = 0;
    int32_t missing = 0;
    size_t largest = 0;
    for (size_t i = 0; i < names.size(); i++) {
        audio::SoundBank::Entry entry;
        if (!bank.get(names[i], &entry) || entry.sound == nullptr) {
            missing++;
            continue;
        }
        const audio::Sound& s = *entry.sound;
        bool pcm8 = true;
        for (size_t k = 0; k < s.samples.size() && pcm8; k++) {
            pcm8 = (s.samples[k] & 0xFF) == 0;
        }
        const size_t bytes = pcm8 ? s.samples.size() : s.samples.size() * 2u;
        const size_t span = (bytes + 31u) & ~(size_t)31u;
        decoded += s.samples.size() * sizeof(audio::Sound::Samples::value_type);
        arena += span;
        pcm16 += pcm8 ? 0 : 1;
        largest = span > largest ? span : largest;
    }
    snprintf(line, sizeof(line),
             "sfxset level=%d effects=%d missing=%d pcm16=%d decoded=%zu arena=%zu largest=%zu "
             "archive=%zu demand_reads=%d demand_bytes=%lld\n",
             level, (int)names.size(), (int)missing, (int)pcm16, decoded, arena, largest,
             bank.archiveBytes(), (int)bank.stats().demandReads,
             (long long)bank.stats().demandBytes);
    o += line;
    // The music-flagged sounds (SNI flag 2) effectNames leaves out, and which of
    // them the level's CMI spells anywhere outside its arena music table: a
    // script could play one of THOSE on the effects bus (the 3DS would then
    // decode and copy it at its first play). The table's own names play on the
    // music bus (LevelMusic), streamed.
    if (level > 0) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "TRAVERSE/LEVEL%d/LEVEL%d", level, level);
        const std::string paths[3] = {"TRAVERSE/TRAVERSE.SNI", std::string(prefix) + "S.SNI",
                                      std::string(prefix) + "O.SNI"};
        std::set<std::string> music;
        for (int a = 0; a < 3; a++) {
            formats::Sni sni;
            if (!formats::Sni::open(&sni, data, paths[a])) {
                continue;
            }
            for (size_t e = 0; e < sni.entries().size(); e++) {
                const formats::Sni::Entry& entry = sni.entries()[e];
                if (sni.isSound(entry) && (entry.flags & formats::Sni::kFlagMusic) != 0) {
                    music.insert(entry.nameString());
                }
            }
        }
        std::vector<uint8_t> cmiBytes;
        Cmi cmi;
        std::string outside;
        if (data.read(std::string(prefix) + ".CMI", &cmiBytes) &&
            Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            std::set<std::string> table;
            for (size_t i = 0; i < cmi.arenaMusic().size(); i++) {
                table.insert(cmi.arenaMusic()[i].first);
                table.insert(cmi.arenaMusic()[i].second);
            }
            for (std::set<std::string>::const_iterator it = music.begin(); it != music.end(); ++it) {
                if (table.count(*it) != 0) {
                    continue;
                }
                const std::string& name = *it;
                bool named = false;
                for (size_t at = 0; at + name.size() <= cmiBytes.size() && !named; at++) {
                    named = memcmp(cmiBytes.data() + at, name.data(), name.size()) == 0 &&
                            (at + name.size() == cmiBytes.size() || cmiBytes[at + name.size()] == 0);
                }
                if (named) {
                    outside += (outside.empty() ? "" : ",") + name;
                }
            }
        }
        snprintf(line, sizeof(line), "sfxset level=%d music_flagged=%d named_outside_music_table=%s\n",
                 level, (int)music.size(), outside.empty() ? "-" : outside.c_str());
        o += line;
    }
    *out = o;
    return true;
}

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

    // `tick1 mix <level> <updates>` / `tick1 mixmeta <level>` — the audio sample
    // plane, digested. See audioMixDump / audioMetaDump above for what is pinned.
    if (argc > 1 && std::string(argv[1]) == "mix") {
        std::string got, why;
        if (!audioMixDump(argc > 2 ? atoi(argv[2]) : 3, argc > 3 ? atoi(argv[3]) : 40, data,
                          &got, &why)) {
            printf("FAIL: %s\n", why.c_str());
            return 1;
        }
        fputs(got.c_str(), stdout);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "mixmeta") {
        std::string got, why;
        if (!audioMetaDump(argc > 2 ? atoi(argv[2]) : 3, data, &got, &why)) {
            printf("FAIL: %s\n", why.c_str());
            return 1;
        }
        fputs(got.c_str(), stdout);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "sfxset") {
        std::string got, why;
        if (!sfxSetDump(argc > 2 ? atoi(argv[2]) : 3, data, &got, &why)) {
            printf("FAIL: %s\n", why.c_str());
            return 1;
        }
        fputs(got.c_str(), stdout);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "mixsize") {
        std::string got, why;
        if (!audioSizeDump(argc > 2 ? atoi(argv[2]) : 3, data, &got, &why)) {
            printf("FAIL: %s\n", why.c_str());
            return 1;
        }
        fputs(got.c_str(), stdout);
        return 0;
    }

    // `tick1 bubble`: the door bubble's second hop alone (bubbleSecondHopCheck).
    if (argc > 1 && std::string(argv[1]) == "bubble") {
        return bubbleSecondHopCheck(data, spritesArg) == 0 ? 0 : 1;
    }

    // `tick1 crossing` — the doorway-crossing goldens on their own, which is what
    // `make -C tests/scripts test_crossing` runs. The same two checks the full suite
    // runs, so the ledger row's acceptance command and the gate cannot disagree.
    if (argc > 1 && std::string(argv[1]) == "crossing") {
        const int failed = crossingCheck(data, spritesArg);
        if (failed == 0) {
            printf("\nAll crossing cases match the oracle.\n");
            return 0;
        }
        printf("\n%d crossing case(s) differ.\n", failed);
        return 1;
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
        // The stream (stream_dump.h): `tick1 dump stream <level> <difficulty> <seed>
        // <health> <steps> <dt> [schedule]` and `tick1 dump streamrand <seeds> <count>`,
        // the C++ side of tests/scripts/stream_probe.
        if (what == "streamview") {
            // The stream view's models (tests/scripts/stream_view_probe), package #18b.
            std::string got;
            std::string why;
            if (!test::streamViewDump(data, &got, &why)) {
                printf("dump streamview: %s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "stream" || what == "streamrand") {
            std::string got;
            std::string why;
            const bool ok =
                what == "streamrand"
                    ? argc > 4 && test::streamRandDump(argv[3], atoi(argv[4]), &got, &why)
                    : argc > 8 && test::streamDump(data, level, atoi(argv[4]), (unsigned)strtoul(argv[5], nullptr, 10),
                                                   atoi(argv[6]), atoi(argv[7]), argv[8], argc > 9 ? argv[9] : "",
                                                   &got, &why);
            if (!ok) {
                printf("usage: dump stream <level> <difficulty> <seed> <health> <steps> <30|60|mix> "
                       "[schedule] | dump streamrand <seeds> <count>%s%s\n",
                       why.empty() ? "" : ": ", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
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
        if (what == "crossing") {
            std::string got;
            std::string why;
            if (!crossingDump(level, data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "skydive") {
            // `tick1 dump skydive <level> <difficulty> <seed> <steps> <delta> <schedule> [full]`:
            // the fall before a level (fall/fall_sim.h), the C++ side of the fall probe
            // (tests/scripts/fall_probe). Not `fall`: that is Kurt's fall at a level's spawn.
            if (argc < 9) {
                printf("usage: tick1 dump skydive <level> <difficulty> <seed> <steps> <delta> "
                       "<schedule> [full]\n");
                return 1;
            }
            std::string got;
            std::string why;
            if (!mdk::test::skydiveDump(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]),
                                        argv[7], argv[8], argc > 9 && std::string(argv[9]) == "full",
                                        data, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "fallview") {
            // `tick1 dump fallview <level> <difficulty> <seed> <steps> <delta> <schedule>`: the
            // fall's view (fall/fall_palette.h, fall_ground.h, fall_layout.h) per step, the C++
            // side of the fall probe's `view` mode (package #17b).
            if (argc < 9) {
                printf("usage: tick1 dump fallview <level> <difficulty> <seed> <steps> <delta> "
                       "<schedule>\n");
                return 1;
            }
            std::string got;
            std::string why;
            if (!mdk::test::fallViewDump(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]),
                                         argv[7], argv[8], data, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "fall") {
            std::string got;
            std::string why;
            if (!spawnFallDump(level, ticks, data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "effects") {
            // `tick1 dump effects`: effects_dump.h, golden/L8_effects_storm.txt.
            std::string got;
            std::string why;
            if (!kurtHazardsDump(8, 400, kEffectsStormSchedule, data, spritesArg, &got, &why,
                                 test::effectsTraceNew())) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "hazards") {
            // `tick1 dump hazards <level> <steps> <schedule>`: kurtHazardsDump's text, the
            // C++ side of the hazards probe.
            std::string got;
            std::string why;
            if (!kurtHazardsDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                 &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "save" || what == "save-load" || what == "save-file") {
            // Package #19, save_dump.h: `tick1 dump save <level> <steps> <after>
            // <schedule>`, `tick1 dump save-load <file> <level> <after>` (the save probe's
            // `Probe ...` and `Probe load ...`), and `tick1 dump save-file <level> <steps>
            // <schedule>` (the C++'s save file alone, for the C# to load).
            std::string got;
            std::string why;
            bool ok = false;
            if (what == "save") {
                ok = mdk::test::saveDump(data, spritesArg, level, ticks, argc > 5 ? atoi(argv[5]) : 0,
                                         argc > 6 ? argv[6] : "", &got, &why);
            } else if (what == "save-file") {
                ok = mdk::test::saveFileText(data, spritesArg, level, ticks, argc > 5 ? argv[5] : "",
                                             &got, &why);
            } else {
                std::string file;
                ok = argc > 5 && readText(argv[3], &file) &&
                     mdk::test::saveLoadDump(data, spritesArg, file, atoi(argv[4]), atoi(argv[5]),
                                             &got, &why);
                if (!ok && why.empty()) {
                    why = "usage: tick1 dump save-load <file> <level> <after>";
                }
            }
            if (!ok) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "items" || what == "hud" || what == "ribbon") {
            // `tick1 dump items|hud|ribbon <level> <steps> <schedule>`: kurtItemsDump's
            // text, the C++ side of the items probe (`Probe [hud|ribbon] ...`).
            std::string got;
            std::string why;
            if (!kurtItemsDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                               &why, what == "items" ? std::string() : what)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "cheats") {
            // `tick1 dump cheats <level> <steps> <schedule>`: kurtCheatsDump's text, the
            // C++ side of the cheats probe's Kurt (`Probe kurt ...`, package #21).
            std::string got;
            std::string why;
            if (!kurtCheatsDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "platforms") {
            // `tick1 dump platforms <level> <steps> <schedule>`: kurtPlatformsDump's text,
            // the C++ side of the platform probe (package #3).
            std::string got;
            std::string why;
            if (!kurtPlatformsDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                   &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "ride") {
            // `tick1 dump ride <level> <steps> <schedule>`: kurtRideDump's text, the C++
            // side of the rides probe (package #9).
            std::string got;
            std::string why;
            if (!kurtRideDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                              &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "board") {
            // `tick1 dump board <level> <steps> <schedule>`: kurtBoardDump's text, the C++
            // side of the snowboard probe (package #10).
            std::string got;
            std::string why;
            if (!kurtBoardDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                               &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "bomber") {
            // `tick1 dump bomber <level> <steps> <schedule>`: kurtBomberDump's text, the
            // C++ side of the bomber probe (package #11).
            std::string got;
            std::string why;
            if (!kurtBomberDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "sniperiris") {
            // `tick1 dump sniperiris <level> <steps>`: the rounds probe's `iris` mode.
            std::string got;
            std::string why;
            if (!sniperIrisDump(level, ticks, data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "rounds" || what == "sniperhud") {
            // `tick1 dump rounds|sniperhud <level> <steps> <schedule>`: kurtRoundsDump's
            // text, the C++ side of the sniper rounds probe (sniperhud: its `hud` mode).
            std::string got;
            std::string why;
            if (!kurtRoundsDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                &why, what == "sniperhud")) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "holes") {
            // `tick1 dump holes <level> <steps> <schedule>`: bulletHolesDump's text, the
            // C++ side of the bullet holes probe.
            std::string got;
            std::string why;
            if (!bulletHolesDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                                 &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "corridor") {
            // `tick1 dump corridor <level> <steps> <x,y,z,yaw> <schedule>`:
            // kurtCorridorDump's text, the C++ side of the corridor probe.
            std::string got;
            std::string why;
            if (!kurtCorridorDump(level, ticks, argc > 5 ? argv[5] : "", argc > 6 ? argv[6] : "",
                                  data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "wind") {
            // `tick1 dump wind <level> <steps> <x,y,z,yaw> <schedule>`: kurtWindDump's
            // text, the C++ side of the updraft / wind probe (tests/scripts/wind_probe).
            std::string got;
            std::string why;
            if (!kurtWindDump(level, ticks, argc > 5 ? argv[5] : "", argc > 6 ? argv[6] : "",
                              data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "ledge") {
            // `tick1 dump ledge <level> <steps> <schedule>`: kurtLedgeDump's text, the C++
            // side of the ledge probe (tests/scripts/ledge_probe), package #12.
            std::string got;
            std::string why;
            if (!kurtLedgeDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                               &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "end") {
            // `tick1 dump end <level> <steps> <schedule>`: levelEndDump's text, the C++
            // side of the end-of-level probe.
            std::string got;
            std::string why;
            if (!levelEndDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                              &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "screen") {
            // `tick1 dump screen <level> <steps> <schedule>`: screenDump's text, the C++
            // side of the screen probe (package #15).
            std::string got;
            std::string why;
            if (!screenDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                            &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "sniper") {
            // `tick1 dump sniper <level> <steps>`: kurtSniperDump's text, the C++ side of
            // the sniper probe.
            std::string got;
            std::string why;
            if (!kurtSniperDump(level, ticks, data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "strike") {
            // `tick1 dump strike <level> <steps> <schedule>`: strikeDump's text, the C++ side
            // of the strike probe (package #14).
            std::string got;
            std::string why;
            if (!strikeDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "cutscene") {
            // `tick1 dump cutscene <level> <steps> <schedule>`: cutsceneDump's text, the C++
            // side of the cutscene probe.
            std::string got;
            std::string why;
            if (!cutsceneDump(level, ticks, argc > 5 ? argv[5] : "", data, spritesArg, &got,
                              &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "fire" || what == "firerun") {
            std::string got;
            std::string why;
            if (!kurtFireDump(level, ticks, what == "fire" ? kFireStand : kFireRun, data,
                              spritesArg, &got, &why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            printf("%s", got.c_str());
            return 0;
        }
        if (what == "hurt" || what == "hurtland" || what == "knock" || what == "invuln" ||
            what == "knockdown" || what == "flashes" || what == "push" || what == "camroll") {
            std::string got;
            std::string why;
            const int schedule = what == "hurtland"                                     ? kHurtHardLand
                                 : (what == "knock"                                     ? kHurtKnock
                                 : (what == "invuln"                                    ? kHurtInvuln
                                 : (what == "knockdown"                                 ? kHurtKnockdown
                                 : (what == "flashes"                                   ? kHurtFlashes
                                 : (what == "push"                                      ? kHurtPush
                                 : (what == "camroll"                                   ? kHurtRoll
                                    : kHurtHealth))))));
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
        runtime.option = 1;
        // dumpRunner ticks the world itself, one tick per printed line; ticking it
        // here too would print the state 2*ticks ticks in, not `ticks`.
        if (what != "runner") {
            for (int t = 0; t < ticks; t++) {
                runtime.update(1.0f / 30.0f);
            }
        }
        if (what == "table") {
            // `tick1 dump table|script <level> <ticks>`: the golden/L*_t*.txt and
            // L*_t*_script.txt texts; the oracle twin is tests/scripts/table_probe.
            printf("%s", dumpTable(level, ticks, runtime).c_str());
        } else if (what == "script") {
            printf("%s", dumpScript(level, ticks, runtime).c_str());
        } else if (what == "pose") {
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
            runtime.option = 1;
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
        runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)
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
        runtime.option = 1;
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
        runtime.option = 1;
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
            runtime.option = 1;
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
    // The level's own start, dropped by StartDrop and stepped with nothing held.
    // This is the golden that says "the 3DS build's level-7 spawn lands on a real
    // floor, where the oracle lands him, on the same step" — batch 75 read the same
    // capture as a camera placed outside the level and asked why Kurt stopped at
    // z=0. He stopped on a floor at z=0.01. See spawnFallDump for the account.
    {
        std::string expected;
        if (!readText("golden/L7_spawn_fall.txt", &expected)) {
            printf("FAIL L7_spawn_fall: no golden file golden/L7_spawn_fall.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!spawnFallDump(7, 400, data, spritesArg, &got, &why)) {
                printf("FAIL L7_spawn_fall: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L7_spawn_fall: the level-7 start drops, falls and lands on a floor "
                       "exactly where the oracle does\n");
            } else {
                printf("FAIL L7_spawn_fall: %s\n", firstDifference(got, expected).c_str());
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

    // The chain gun, end to end against the oracle probe (see kurtFireDump for what each
    // column pins and why there are two schedules). L8 stands on the pad and fires into
    // the aliens until a laser knocks him down; L8 run fires on the run and lets go; L7 is
    // the level the hardware check uses, where every shot is a wall shot (HitGroup and a
    // Hard spark) and he falls off the pad firing (the Fall muzzle offset).
    {
        struct FireCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const FireSchedule* plan;
            const char* pass;
        };
        static const FireCase kFireCases[] = {
            {"L8_fire", "golden/L8_fire.txt", 8, 420, &kFireStand,
             "K_SHOT, the turn's muzzle flash, 112 hits and a kill that explodes (sound, white "
             "flash, break-up, gore drops, trails), and the knock-down that stops the gun"},
            {"L8_fire_run", "golden/L8_fire_run.txt", 8, 480, &kFireRun,
             "K_RUNFIR and its footsteps, the gun loop stopped when fire comes up"},
            {"L7_fire_run", "golden/L7_fire_run.txt", 7, 480, &kFireRun,
             "every tick a wall shot (HitGroup, Hard sparks, ricochets), the Fall muzzle"},
        };
        for (size_t i = 0; i < sizeof(kFireCases) / sizeof(kFireCases[0]); i++) {
            const FireCase& c = kFireCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtFireDump(c.level, c.steps, *c.plan, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Sniper mode, end to end against the oracle probe (tests/scripts/sniper_probe; see
    // kurtSniperDump for the columns): one schedule on two levels -- L3 starts on the
    // floor, L7 lands first (LAND, then the scope).
    {
        struct SniperCase {
            const char* name;
            const char* path;
            int level;
        };
        static const SniperCase kSniperCases[] = {
            {"L3_sniper", "golden/L3_sniper.txt", 3},
            {"L7_sniper", "golden/L7_sniper.txt", 7},
        };
        for (size_t i = 0; i < sizeof(kSniperCases) / sizeof(kSniperCases[0]); i++) {
            const SniperCase& c = kSniperCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtSniperDump(c.level, 600, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: sniper mode in and out, the scope's look, zoom (keys and "
                       "wheel), clip and ammo keys, the sidestep, and the scope view's "
                       "matrices to the bit\n", c.name);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Kurt's pickups and items, end to end against the oracle probe (tests/scripts/
    // items_probe; see kurtItemsDump for the columns). The schedules are the probe's
    // arguments, byte for byte: each golden's first line repeats its schedule.
    {
        struct ItemsCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
            const char* view;   // "" the items columns; "hud" or "ribbon" (Probe hud|ribbon)
        };
        static const ItemsCase kItemsCases[] = {
            // The HUD's message stream (Hud/Messages.cs) and the twisters' ribbons
            // (Scripts/Ribbon.cs Triangles), the same runs seen through the probe's views.
            {"L8_hud_queue", "golden/L8_hud_queue.txt", 8, 700,
             "give:5:SW_HBOMB,give:6:SW_INTER,give:7:SW_H01,give:8:SW_KEY,give:9:SW_H50,"
             "give:10:SW_TWIST,give:11:SW_DUMMY,msg:400:DGW_REAL:2:1.5,msg:401:NOPE:1:2,"
             "msg:402:SW_GATT:0:1",
             "the pickups' names queued (4 deep, the fifth dropped, SW_H01 silent), grown, timed "
             "twice as fast while others wait, shrunk; two-line texts; a front-queued too-wide "
             "line in the small font; a missing name; an unzoomed message; every glyph's quad",
             "hud"},
            {"L7_hud_collect", "golden/L7_hud_collect.txt", 7, 320,
             "left:10-88,fwd:110-200,turbo:110-200",
             "the hamster run down through CollectPickups shows \"I Feel Top!!!\"", "hud"},
            {"L8_ribbon_tornado", "golden/L8_ribbon_tornado.txt", 8, 260,
             "give:5:SW_TWIST,use:30-32",
             "the twisters' ribbons: 1-5 positions, the narrowing scales, the WMIBTEX rows, "
             "through the spiral and the split", "ribbon"},
            {"L7_items_collect", "golden/L7_items_collect.txt", 7, 520,
             "left:10-88,fwd:110-200,turbo:110-200,give:210:SW_TWIST,give:211:SW_HBOMB,"
             "prev:220-222,use:230-232",
             "Kurt runs the hamster down (CollectPickups, SW_H150 to 150 health, APPLE, the "
             "pickup's shrink and removal), then a tornado among walls", ""},
            {"L8_items_grenade", "golden/L8_items_grenade.txt", 8, 400,
             "give:5:SW_HBOMB,give:6:SW_TWIST,give:7:SW_DUMMY,next:20-22,prev:30-32,next:40-42,"
             "next:44-46,prev:48-50,use:60-62,use:200-202",
             "the item keys' wrap both ways, THROW and frame 8, two grenades (75 u/s) and their "
             "blasts", ""},
            {"L8_items_air", "golden/L8_items_air.txt", 8, 300,
             "give:5:SW_HBOMB,jump:30-40,use:40-42", "a grenade used at once in the air", ""},
            {"L8_items_tornado", "golden/L8_items_tornado.txt", 8, 600,
             "give:5:SW_TWIST,use:30-32",
             "the tornado's spin and twisters: the spiral, the split, the chase, the hurt", ""},
            {"L8_items_mortar", "golden/L8_items_mortar.txt", 8, 400,
             "give:5:SW_THUMP,use:30-32",
             "the mortar's thumps (damage, lift, shake) and Kurt knocked down from the fourth", ""},
            {"L8_items_bomb", "golden/L8_items_bomb.txt", 8, 400,
             "give:5:SW_HBOMB,give:6:SW_INTER,use:30-32,use:150-152",
             "the World's Most Interesting Bomb spun, set off by use (CanUse, TriggerBomb), its "
             "blast on Kurt", ""},
            {"L8_items_decoy", "golden/L8_items_decoy.txt", 8, 500,
             "give:5:SW_DUMMY,use:30-32", "the decoy walks and the aliens aim at it", ""},
            {"L8_items_nuke", "golden/L8_items_nuke.txt", 8, 600, "give:5:SW_KEY,use:30-32",
             "the key becomes the nuke: its white, its shake, its doors, its blast", ""},
            {"L8_items_gatt", "golden/L8_items_gatt.txt", 8, 600,
             "give:5:SW_GATT,give:7:SW_DUMMY,fire:20-580,use:30-32",
             "the super chain gun's time runs out: GATTFIRE to MULTIFIRE, its slot goes", ""},
            {"L5_items_seal", "golden/L5_items_seal.txt", 5, 800,
             "give:5:SW_SEAL,give:6:SW_SBONE,use:30-32,prev:60-62,use:70-72,fwd:400-640",
             "the seal and the bone out together, landing (XMT_LAND), and taken back", ""},
            {"L8_items_cow", "golden/L8_items_cow.txt", 8, 300,
             "drop:5:SW_H01>SW_EWJ:0:20:3,fwd:20-120",
             "a pickup named SW_EWJ run through: COLLECT, then DropCow (COW, CowTarget) and the "
             "holy cow's fall, drift and fuse (UpdateCow, CowHits)", ""},
        };
        for (size_t i = 0; i < sizeof(kItemsCases) / sizeof(kItemsCases[0]); i++) {
            const ItemsCase& c = kItemsCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtItemsDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why,
                               c.view ? c.view : "")) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The cheats' Kurt, end to end against the oracle probe (tests/scripts/cheats_probe,
    // `Probe kurt`; see kurtCheatsDump for the columns): god mode and noclip (gap package
    // #21). The schedules are the probe's arguments, byte for byte.
    {
        struct CheatsCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const CheatsCase kCheatsCases[] = {
            {"cheats_L3_noclip", "golden/cheats_L3_noclip.txt", 3, 300,
             "yaw:0,noclip:1,fwd:1-20,sright:20-30,sleft:30-35,jump:35-45,turbo:40-45,"
             "down:45-160,turbo:100-160,left:160-170,clip:180",
             "noclip flies him through the floor (60 u/s, turbo x3, jump up, Down down, the "
             "turn keys still turn), and with clipping back he falls out of the arena and dies"},
            {"cheats_L3_god", "golden/cheats_L3_god.txt", 3, 320,
             "god:1,hurt:5:30,noclip:10,down:10-150,turbo:10-150,clip:150,hurt:200:40,"
             "mortal:250",
             "god mode takes no damage and survives the fall out of the arena; mortal again, "
             "the fall-out death comes at once"},
            {"cheats_L7_hurt", "golden/cheats_L7_hurt.txt", 7, 360,
             "hurt:30:5,god:300,hurt:310:5,mortal:320,hurt:330:5",
             "Kurt.Damage: a hurt lands (knocked in the air), none under god (no knock either), "
             "and one lands again once mortal"},
        };
        for (size_t i = 0; i < sizeof(kCheatsCases) / sizeof(kCheatsCases[0]); i++) {
            const CheatsCase& c = kCheatsCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtCheatsDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The sniper rounds, end to end against the oracle probe (tests/scripts/rounds_probe;
    // see kurtRoundsDump for the columns). The schedules are the probe's arguments, byte
    // for byte: each golden's first line repeats its schedule.
    {
        struct RoundsCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const RoundsCase kRoundsCases[] = {
            {"L8_rounds_ship", "golden/L8_rounds_ship.txt", 8, 450,
             "snipe:30-32,aim:35:42A77D94:C2A22812,fire:70-140,give:150:SW_HOME,fire:200-205,"
             "give:215:SW_SGREN,fire:265-270,give:280:SW_HGREN,fire:330-335",
             "bullets on XBSHIP's faces (part hits, 65000 health untouched, the ship's reaction), "
             "the scope's lock and zoom limit, a homing round steering onto it, a grenade's blast "
             "(hit type 2 then -7), a homing grenade"},
            {"L8_rounds_kill", "golden/L8_rounds_kill.txt", 8, 240,
             "snipe:30-32,aim:45:42B41B0C:C0C13FC0,hp:70:7:8,fire:70-80,"
             "aim:100:00000000:00000000,fire:100-104",
             "a bullet kills an XG (CountEnemy, Kill facing yaw + 180, the round KILLED), then a "
             "wall shot (Raycast, the group hit, 3 grey sparks, DEAD)"},
            {"L7_rounds_mortar", "golden/L7_rounds_mortar.txt", 7, 450,
             "tp:5:DANT_6:520:4030:-108,snipe:30-32,give:35:SW_LGREN,aim:40:41A00000:C1A00000,"
             "fire:85-90,aim:200:43870000:C2340000,fire:200-205",
             "the mortar puzzle: a mortar round lands on DANT_6's group 12, whose hit script's "
             "bomb_follow_path (opcode 28) guides it along path 109157 to its blast; a second "
             "mortar's drag, gravity, bounces and box sweep"},
        };
        for (size_t i = 0; i < sizeof(kRoundsCases) / sizeof(kRoundsCases[0]); i++) {
            const RoundsCase& c = kRoundsCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtRoundsDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The bullet holes (special_130, package #16), end to end against the oracle probe
    // (tests/scripts/holes_probe; see bulletHolesDump for the lines). The schedules are the
    // probe's arguments, byte for byte: each golden's first line repeats its schedule.
    {
        struct HolesCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const HolesCase kHolesCases[] = {
            {"L8_holes_guntam", "golden/L8_holes_guntam.txt", 8, 360,
             "tp:5:GUNT_10:20:3360:-360,snipe:150-152,aim:155:42F79A70:C1D14455,fire:190-200,"
             "aim:240:42F60000:C1C80000,fire:260-270,opt:280:0,aim:280:42F90000:C1DC0000,"
             "fire:300-310,stampall:340:12",
             "two rounds on GUNT_10's XGUNTAM (yaw 270) run its special_130: BHOLE on XGU_BODY, "
             "a miss, then BHOLE2 (option 0) on every part's nearest face, wrapping both ways"},
            {"L6_holes_xb2", "golden/L6_holes_xb2.txt", 6, 360,
             "tp:5:OLYM_10:3400:2440:-2320,snipe:150-152,aim:155:3E66F000:C1195B8E,"
             "fire:190-200,fire:240-250,fire:290-300,fire:340-350,stampall:355:5",
             "OLYM_10's XB2 (yaw 180) behind its glass: two misses, two rounds' holes on "
             "XB2_BODY, then BHOLE on all 20 parts (XB2_BACK, _NOSE, _BHID)"},
        };
        for (size_t i = 0; i < sizeof(kHolesCases) / sizeof(kHolesCases[0]); i++) {
            const HolesCase& c = kHolesCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!bulletHolesDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The screen (package #15: the town timer, the shake, the sky mode, the group textures),
    // end to end against the oracle probe (tests/scripts/screen_probe; see screenDump for the
    // lines). The schedules are the probe's arguments, byte for byte: each golden's first line
    // repeats its schedule. The shake's offsets are the oracle's to the bit because the probe
    // seeds the C# camera's Random as screenDump seeds this one (camseed, default 1).
    {
        struct ScreenCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const ScreenCase kScreenCases[] = {
            {"L3_screen_town", "golden/L3_screen_town.txt", 3, 80, "diff:0,town:5:7",
             "Easy's 81000-tick town timer, set to 7 and counted down a tick at a time; at 0 "
             "FlattenTown: shake 5 (the camera's offsets and drain), OOT_L3 zoomed in front for "
             "5 s, FlagTownFlattened"},
            {"L3_screen_hmo3", "golden/L3_screen_hmo3.txt", 3, 320,
             "tp:5:HMO_3:-60:1410:-960,kill:20:17,avar:120:HMO_3:0:3",
             "HMO_3's sky mode -1 (Keep) on entry; its XG's death script raises shake 2 "
             "(opcode 135) and drops group 1; at arena var 0 = 3 groups 2, 9-12 take -1027 "
             "(opcode 140) and shatter one by one"},
            {"L4_screen_second", "golden/L4_screen_second.txt", 4, 70,
             "diff:2,camseed:7,tp:5:MEAT_6:-120:13600:-1990,gflag:8:80000000,town:10:5",
             "Hard's 36000 ticks; MEAT_6's sky mode 1 (Clear, black); with FlagSecondTown the "
             "town flattens as OOT_L4A, FlagSecondTownFlattened; camera seed 7"},
            {"L5_screen_last", "golden/L5_screen_last.txt", 5, 6, "",
             "the last level of the order has no town (TownTicks 0, never counted)"},
        };
        for (size_t i = 0; i < sizeof(kScreenCases) / sizeof(kScreenCases[0]); i++) {
            const ScreenCase& c = kScreenCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!screenDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // A retextured group is recorded for the frontends once per arena (package #15: the C#'s
    // TriangleGroups.Changed -> LevelView.Rebuild becomes ScriptRuntime::retexturedArenas,
    // which the frontends hand to ArenaViews::refresh): group 0 and a group the arena does
    // not have are SetMaterial's early return and record nothing.
    {
        const char* name = "screen_retextured_arenas";
        LevelData level3;
        char cmiPath[64];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL3/LEVEL3.CMI");
        std::vector<uint8_t> cmiBytes;
        Cmi cmi3;
        audio::SoundBank bank3;
        bool ok = LevelData::load(&level3, &data, 3) && data.read(cmiPath, &cmiBytes) &&
                  Cmi::parse(&cmi3, cmiBytes.data(), (int32_t)cmiBytes.size()) &&
                  audio::SoundBank::forLevel(&bank3, data, 3);
        if (!ok) {
            printf("FAIL %s: level 3 did not load\n", name);
            failures++;
        } else {
            scripts::ScriptRuntime runtime(&level3, &cmi3, 1, &bank3, nullptr, spritesArg);
            runtime.setGroupTexture("HMO_3", 0, -1027);
            runtime.setGroupTexture("HMO_3", 250, -1027);
            const bool none = runtime.retexturedArenas().empty();
            runtime.setGroupTexture("HMO_3", 2, -1027);
            runtime.setGroupTexture("HMO_3", 9, -1027);
            int32_t material = 0;
            const Arena* hmo3 = level3.arenaNamed("HMO_3");
            const bool stored = hmo3 && hmo3->groupMaterial(9, &material) && material == -1027;
            const bool once = runtime.retexturedArenas().size() == 1 &&
                              runtime.retexturedArenas()[0] == "HMO_3";
            runtime.clearRetexturedArenas();
            const bool taken = runtime.retexturedArenas().empty();
            if (none && stored && once && taken) {
                printf("PASS %s: group 0 and a missing group record nothing; two groups of "
                       "HMO_3 record it once, until taken\n", name);
            } else {
                printf("FAIL %s: none=%d stored=%d once=%d taken=%d\n", name, none ? 1 : 0,
                       stored ? 1 : 0, once ? 1 : 0, taken ? 1 : 0);
                failures++;
            }
        }
    }

    // A STAMPED TEXTURE OUTLIVES A PIXEL DROP (formats::Texture::pinPixels). The C#
    // keeps every texture's pixels for the level, so a hole stays; the port's level
    // load drops an arena's pixels and reads them back from the resident MTO -- the
    // file's, hole-less pixels -- so a stamped texture is pinned. Driven through the
    // pixel source itself (game/level_data.cpp): pixels in, a texel changed and
    // pinned, the arena's pixels dropped (an unpinned sibling goes), then read back
    // (adopted from the resident MTO).
    {
        LevelData pinned;
        const char* name = "holes_pinned";
        bool ok = LevelData::load(&pinned, &data, 8);
        formats::Arena* arena = ok ? pinned.arenaNamedMutable("GUNT_10") : nullptr;
        std::vector<std::pair<std::string, formats::Texture> >* textures =
            arena ? &const_cast<formats::TextureArchive&>(arena->textures()).mutableTextures()
                  : nullptr;
        if (!textures || textures->size() < 2) {
            printf("FAIL %s: level 8's GUNT_10 and two of its textures did not load\n", name);
            failures++;
        } else {
            (*textures)[0].second.indices();  // bring the arena's pixels in
            formats::Texture& stamped = (*textures)[0].second;
            formats::Texture& sibling = (*textures)[1].second;
            const uint8_t was = stamped.indices()[0];
            const uint8_t mark = (uint8_t)(was + 1);
            stamped.mutableIndices()[0] = mark;
            stamped.pinPixels();
            arena->dropTexturePixels();
            const bool kept = !stamped.pixelsAway() && stamped.indices()[0] == mark;
            const bool trimmed = sibling.pixelsAway();
            sibling.indices();  // the source re-parses the arena's prefix from RAM
            const bool back = !stamped.pixelsAway() && stamped.indices()[0] == mark &&
                              !sibling.pixelsAway();
            if (kept && trimmed && back) {
                printf("PASS %s: a pinned texture keeps its stamped texel through its arena's "
                       "trim and re-read; its unpinned sibling is dropped and read back\n",
                       name);
            } else {
                printf("FAIL %s: kept=%d trimmed=%d back=%d\n", name, kept ? 1 : 0,
                       trimmed ? 1 : 0, back ? 1 : 0);
                failures++;
            }
        }
    }

    // Full saves (package #19) against the save probe (tests/scripts/save_probe;
    // save_dump.h). Each golden is the C#'s whole run: the level played to the save,
    // the save's text and the save FILE's (line 4), the file parsed back into a fresh
    // world, and that world run on with every step's whole-state hash. The C++ must
    // print the same bytes (it writes the same file), and -- the cross-load -- the C#'s
    // own file (line 4, read as the C# wrote it) loaded by the C++ must print the C#'s
    // restore lines. A `loadOnly` case pins only the load of the C#'s file, for a
    // scene whose play the port does not yet match (none today: L3_save_fan was one
    // until package 8's updrafts landed).
    // The seven goldens carry "town_total" (upstream 6eb0963b) in their save text and
    // file: the C# 1.1.1 probe's output with that key added after "town_ticks", the
    // sizes grown by its length and the hashes over it (checked 2026-11-26: every
    // other byte as the probe wrote it). The 1.1.1 file's form is still loaded below,
    // as a save without the key.
    {
        struct SaveCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            int after;
            const char* schedule;
            bool loadOnly;
            const char* pass;
        };
        static const SaveCase kSaveCases[] = {
            {"L3_save_box", "golden/L3_save_box.txt", 3, 120, 30,
             "tp:1:HMO_3:3:1241:-931,run:5:XG:44160:0:0:0,run:9:XG:8127:10:0:0,"
             "run:13:XG:52115:-10:0:0",
             false,
             "46 objects, a spawn_box object (its \"box\"), a second arena, CMI and arena "
             "animations, saved, reloaded and run 30 steps"},
            {"L3_save_teleport", "golden/L3_save_teleport.txt", 3, 120, 60,
             "tp:1:HMO_7:677.5:2125:41", false,
             "a save in HMO_7, which only a teleport reaches: the restore enters it into "
             "Kurt's space too (the C#'s one ArenaSpace), so he stands, not falls"},
            {"L7_save_decoy", "golden/L7_save_decoy.txt", 7, 200, 60,
             "yaw:0,clip:2:0.5,give:5:SW_HBOMB,give:6:SW_DUMMY,use:60-62", false,
             "the inventory, the sniper clip, the decoy (items.decoy) and its walk"},
            {"L7_save_bomb", "golden/L7_save_bomb.txt", 7, 200, 60,
             "yaw:0,give:5:SW_INTER,use:60-62", false,
             "the World's Most Interesting Bomb out (items.bomb)"},
            {"L4_save_groups", "golden/L4_save_groups.txt", 4, 600, 30, "", false,
             "MEAT_1's triangle groups' states"},
            {"L5_save_idle", "golden/L5_save_idle.txt", 5, 600, 30, "", false,
             "MUSE_1's objects and arena animations"},
            {"L3_save_fan", "golden/L3_save_fan.txt", 3, 200, 30, "tp:1:HMO_6:1:2310:-370",
             false, "HMO_6's fan (fan_create's record), saved, reloaded and run 30 steps"},
        };
        for (size_t i = 0; i < sizeof(kSaveCases) / sizeof(kSaveCases[0]); i++) {
            const SaveCase& c = kSaveCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            bool ok = true;
            if (!c.loadOnly) {
                if (!mdk::test::saveDump(data, spritesArg, c.level, c.steps, c.after, c.schedule,
                                         &got, &why)) {
                    printf("FAIL %s: %s\n", c.name, why.c_str());
                    ok = false;
                } else if (got != expected) {
                    printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                    ok = false;
                }
            }
            // The cross-load: line 4 is the C#'s save file.
            size_t line = 0;
            for (int n = 0; n < 3 && line != std::string::npos; n++) {
                line = expected.find('\n', line) + 1;
            }
            const size_t lineEnd = expected.find('\n', line);
            const size_t parsedAt = expected.find("\nparsed ");
            if (line == std::string::npos || lineEnd == std::string::npos ||
                parsedAt == std::string::npos) {
                printf("FAIL %s: the golden has no save file line\n", c.name);
                ok = false;
            } else {
                const std::string file = expected.substr(line, lineEnd - line);
                char head[64];
                snprintf(head, sizeof(head), "save load level=%d after=%d\n", c.level, c.after);
                const std::string want = head + expected.substr(parsedAt + 1);
                std::string loaded;
                if (!mdk::test::saveLoadDump(data, spritesArg, file, c.level, c.after, &loaded,
                                             &why)) {
                    printf("FAIL %s (the C#'s file): %s\n", c.name, why.c_str());
                    ok = false;
                } else if (loaded != want) {
                    printf("FAIL %s (the C#'s file): %s\n", c.name,
                           firstDifference(loaded, want).c_str());
                    ok = false;
                }
                // A save from before upstream 6eb0963b has no "town_total": it loads
                // with the difficulty's total (the runtime's), which is what the file
                // above carries, so from the restore on it must print the same lines.
                // Two lines are left out: "parsed" names the shorter file's size, and
                // the restored state differs from the file read (it gained the total),
                // so saveLoadDump prints the restored state after "restored".
                std::string older = file;
                const size_t total = older.find(",\"town_total\":");
                if (total != std::string::npos) {
                    const size_t end = older.find_first_not_of("0123456789", total + 14);
                    older.erase(total, end - total);
                }
                std::string olderLoaded;
                std::string olderTail;
                std::string wantTail;
                const bool olderOk = total != std::string::npos &&
                                     mdk::test::saveLoadDump(data, spritesArg, older, c.level,
                                                             c.after, &olderLoaded, &why);
                const size_t olderAt = olderLoaded.find("\nrestored ");
                const size_t wantAt = want.find("\nrestored ");
                if (olderOk && olderAt != std::string::npos && wantAt != std::string::npos) {
                    olderTail = olderLoaded.substr(olderAt);
                    wantTail = want.substr(wantAt);
                    const size_t state = olderTail.find("\n{");
                    if (state != std::string::npos) {
                        olderTail.erase(state, olderTail.find('\n', state + 1) - state);
                    }
                }
                if (!olderOk || olderTail.empty() || olderTail != wantTail) {
                    printf("FAIL %s (a save without town_total): %s\n", c.name,
                           why.empty() ? firstDifference(olderLoaded, want).c_str() : why.c_str());
                    ok = false;
                }
            }
            if (ok) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                failures++;
            }
        }
    }

    // Sniper mode's screen and views (gap package #7: game/sniper_overlay.cpp,
    // game/sniper_view.cpp) against the rounds probe's `hud` and `iris` modes
    // (tests/scripts/rounds_probe; sniperHudStep and sniperIrisDump for the columns): the
    // overlay's state and every quad it draws, the round cameras' and the clip's views.
    {
        struct HudCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;  // null: the iris sequence
            const char* pass;
        };
        static const HudCase kHudCases[] = {
            {"L3_sniper_iris", "golden/L3_sniper_iris.txt", 3, 300, nullptr,
             "the overlay alone: the strike's iris closing on a target, its pulse, opening, the "
             "gauge following a zoom sweep, the zoom percent, the ammo icons and count, the "
             "cameras' fills and the miss animation, every quad on a 480-wide canvas"},
            {"L8_sniper_hud_ship", "golden/L8_sniper_hud_ship.txt", 8, 450,
             "snipe:30-32,aim:35:42A77D94:C2A22812,fire:70-140,give:150:SW_HOME,fire:200-205,"
             "give:215:SW_SGREN,fire:265-270,give:280:SW_HGREN,fire:330-335",
             "the rounds' cameras watching (their windows and views), hit, the clip's loaded "
             "rounds sliding in (ClipPlace) and its view, the ammo types, on the 3DS's canvas"},
            {"L8_sniper_hud_kill", "golden/L8_sniper_hud_kill.txt", 8, 240,
             "snipe:30-32,aim:45:42B41B0C:C0C13FC0,hp:70:7:8,fire:70-80,"
             "aim:100:00000000:00000000,fire:100-104",
             "a kill's camera fill and a miss's SNIPERGA animation"},
        };
        for (size_t i = 0; i < sizeof(kHudCases) / sizeof(kHudCases[0]); i++) {
            const HudCase& c = kHudCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            const bool made = c.schedule == nullptr
                                  ? sniperIrisDump(c.level, c.steps, data, spritesArg, &got, &why)
                                  : kurtRoundsDump(c.level, c.steps, c.schedule, data, spritesArg,
                                                   &got, &why, true);
            if (!made) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The stream after a level (mdk_core/src/stream: Flight, StreamTube, TubeBasis,
    // WatcomRandom) against the stream probe (tests/scripts/stream_probe; stream_dump.cpp
    // for the lines). Every float is printed as its bits, so these are exact. The
    // arguments are the probe's, byte for byte: each golden's first line repeats them.
    {
        struct StreamCase {
            const char* name;
            const char* path;
            int level;
            int difficulty;
            unsigned seed;
            int health;
            const char* dt;
            const char* schedule;
            const char* pass;
        };
        static const StreamCase kStreamCases[] = {
            {"stream_L7_weave", "golden/stream_L7_weave.txt", 7, 1, 1, 100, "mix",
             "right:20-60,up:40-90,left:100-180,down:200-260,right:300-420,up:300-330",
             "the direct-start seed's tube, steered into the walls 41 times (hurt sounds, damage "
             "with its random extra, the slow-down) down to 1 health, Bones' rescue and the fade"},
            {"stream_L7_bonus", "golden/stream_L7_bonus.txt", 7, 1, 1, 100, "fast",
             "auto:1-60,right:61-75,auto:76-130,down:131-140,auto:141-900",
             "a pilot catches the walking health bonus (150, APPLE), Bones comes after segment "
             "177 and the stream ends with 150 health"},
            {"stream_L8_planet", "golden/stream_L8_planet.txt", 8, 0, 7, 100, "fast",
             "auto:1-150,left:151-165,auto:166-900",
             "the Gunter tube: Gunter ahead, the tube straightening after 168, the planet, the "
             "end at 186 with the health LEVEL5 starts with"},
            {"stream_L8_death", "golden/stream_L8_death.txt", 8, 2, 2302799999u, 30, "30",
             "right:10-80,up:30-70,left:90-200,down:150-260,right:220-400,up:300-420",
             "Hard in the Gunter tube from 30 health: the walls kill Kurt, the red then black "
             "fade, Died"},
            {"stream_L3_rescue", "golden/stream_L3_rescue.txt", 3, 0, 42, 1, "30", "left:5-40,up:10-30",
             "at 1 health Bones comes at once; the keys do nothing while he does"},
            {"stream_L6_stop", "golden/stream_L6_stop.txt", 6, 2, 305419896u, 100, "30",
             "auto:1-200,up:15-25,stop:120", "Esc (Stop) fades the stream out early, Survived"},
        };
        for (size_t i = 0; i < sizeof(kStreamCases) / sizeof(kStreamCases[0]); i++) {
            const StreamCase& c = kStreamCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!test::streamDump(data, c.level, c.difficulty, c.seed, c.health, 900, c.dt, c.schedule, &got,
                                  &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }

        std::string expected;
        std::string got;
        std::string why;
        if (!readText("golden/stream_rand.txt", &expected)) {
            printf("FAIL stream_rand: no golden file golden/stream_rand.txt\n");
            failures++;
        } else if (!test::streamRandDump("1,0,4294967295,305419896", 256, &got, &why)) {
            printf("FAIL stream_rand: %s\n", why.c_str());
            failures++;
        } else if (got == expected) {
            printf("PASS stream_rand: Watcom's rand() for four seeds, 256 draws each\n");
        } else {
            printf("FAIL stream_rand: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }

        // The stream view's models (package #18b): STREAM.BNI's images, and every
        // (model, animation) pair StreamView draws, parsed and baked, against
        // tests/scripts/stream_view_probe (the C#'s Mdk.Formats).
        expected.clear();
        got.clear();
        why.clear();
        if (!readText("golden/stream_view_models.txt", &expected)) {
            printf("FAIL stream_view_models: no golden file golden/stream_view_models.txt\n");
            failures++;
        } else if (!test::streamViewDump(data, &got, &why)) {
            printf("FAIL stream_view_models: %s\n", why.c_str());
            failures++;
        } else if (got == expected) {
            printf("PASS stream_view_models: the stream's images, and KURT, BONES, SWH150 and GUNTA in "
                   "every frame of the animations the stream draws them in\n");
        } else {
            printf("FAIL stream_view_models: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // The fall before a level (fall/fall_sim.h, the C#'s FallSim), end to end against the
    // oracle probe (tests/scripts/fall_probe; see skydive_dump.cpp). The arguments are the
    // probe's, byte for byte, and each golden's first line repeats them.
    {
        struct SkydiveCase {
            const char* name;
            const char* path;
            int level;
            int difficulty;
            int seed;
            int steps;
            const char* delta;
            const char* schedule;
            const char* pass;
        };
        static const SkydiveCase kSkydiveCases[] = {
            {"L6_skydive_chase", "golden/L6_skydive_chase.txt", 6, 1, 1, 1200, "0.0333333333",
             "chase:150-1200",
             "Kurt steers onto three pickups under their chutes (SW_HOME, SW_HBOMB, SW_GATT), "
             "is seen and hit, lands at 33 s and carries his health and the pickups into the "
             "level's inventory"},
            {"L8_skydive_hard", "golden/L8_skydive_hard.txt", 8, 2, 1, 1200, "0.0333333333",
             "right:200-260,up:300-340",
             "hard over LEVEL8: Bones, the steering limits, the missiles' hits to 0 health, the "
             "red screen and GameOver with no carry"},
            {"L3_skydive_slow", "golden/L3_skydive_slow.txt", 3, 0, 4, 600, "0.15",
             "left:20-60,down:30-200",
             "easy, 0.15 s frames clamped to 0.1 (three ticks a step): the per-tick loops, the "
             "pull back to the centre, the camera's braking and the fade at 31-33 s"},
            {"L7_skydive_skip", "golden/L7_skydive_skip.txt", 7, 1, 2, 700, "0.0333333333",
             "up:160-200,skip:600",
             "LEVEL7's FALL_T1 message, and Esc skipping the fall (Landed, full health)"},
        };
        for (size_t i = 0; i < sizeof(kSkydiveCases) / sizeof(kSkydiveCases[0]); i++) {
            const SkydiveCase& c = kSkydiveCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!mdk::test::skydiveDump(c.level, c.difficulty, c.seed, c.steps, c.delta, c.schedule,
                                        false, data, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The fall's VIEW (package #17b: fall/fall_palette.h, fall_ground.h, fall_layout.h)
    // against the fall probe's `view` mode: per step, what the C#'s FallView makes of
    // the sim -- the palette's effects, the ground's track and window, the crawler, the
    // haze's alphas, the models' poses, the missiles' Orient, the radar's rings, the
    // fills. The arguments are the probe's, byte for byte.
    {
        struct FallViewCase {
            const char* name;
            const char* path;
            int level;
            int difficulty;
            int seed;
            int steps;
            const char* delta;
            const char* schedule;
            const char* pass;
        };
        static const FallViewCase kFallViewCases[] = {
            {"L6_fallview_chase", "golden/L6_fallview_chase.txt", 6, 1, 1, 1200, "0.0333333333",
             "chase:150-1200",
             "LEVEL6's fall drawn: the intro, the first second's whitening, the track written "
             "under the crawler, the radar's rings, the missiles' Orient, the pickups' yaw and "
             "chutes, the explosions, the pull back and the fade"},
            {"L8_fallview_hard", "golden/L8_fallview_hard.txt", 8, 2, 1, 1200, "0.0333333333",
             "right:200-260,up:300-340",
             "LEVEL8's fall drawn on hard: Bones, the hits' flashes, the red of death and the "
             "skull's palette"},
        };
        for (size_t i = 0; i < sizeof(kFallViewCases) / sizeof(kFallViewCases[0]); i++) {
            const FallViewCase& c = kFallViewCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!mdk::test::fallViewDump(c.level, c.difficulty, c.seed, c.steps, c.delta, c.schedule,
                                         data, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Kurt out of DANT_1 and down the middle of CDANT_1's north leg, against the corridor
    // probe (tests/scripts/corridor_probe; see kurtCorridorDump). It pins the second arena's
    // whole life over a doorway: the unlocked door's ShowBehind makes CDANT_1 the active
    // second (s=204) and the solid list takes it at s=206, the crossing swaps the pair
    // (s=228), the door closing behind him drops DANT_1 (second at s=276, solid at s=278),
    // and the corridor's load triggers preload DANT_1 (s=701) and DANT_2 (s=721). With
    // DANT_1 released he runs the leg at x 140.5 unblocked; were it still solid, its triangle
    // 60 crosses that line at y 575 (docs/OPEN_ITEMS.md, "Kurt walks through closed doors").
    {
        static const char* kCorridorSchedule =
            "unlock:40,fwd:61-261,turbo:61-261,right:271-308,fwd:321-553,turbo:321-553,"
            "left:563-600,sright:606-640,fwd:650-800,turbo:650-800";
        std::string expected;
        std::string got;
        std::string why;
        if (!readText("golden/L7_corridor_middle.txt", &expected)) {
            printf("FAIL L7_corridor_middle: no golden file golden/L7_corridor_middle.txt\n");
            failures++;
        } else if (!kurtCorridorDump(7, 800, "0,380,15,90", kCorridorSchedule, data, spritesArg,
                                     &got, &why)) {
            printf("FAIL L7_corridor_middle: %s\n", why.c_str());
            failures++;
        } else if (got == expected) {
            printf("PASS L7_corridor_middle: the second arena over DANT_1's door (ShowBehind, "
                   "the crossing, the door closing behind him) and the solid list that follows "
                   "behind it\n");
        } else {
            printf("FAIL L7_corridor_middle: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // ---- Package #3: solid objects, platforms, lifts and the keeper -------------------
    // The same corridor run WITHOUT the unlock (docs/OPEN_ITEMS.md, "Kurt walks through
    // closed doors"): X7DOOR starts locked, a locked door is solid to Kurt
    // (ScriptRuntime.SolidsWithin, Solids.Walk), and the oracle's Kurt stops at y = 478.4.
    {
        static const char* kLockedSchedule =
            "fwd:61-261,turbo:61-261,right:271-308,fwd:321-553,turbo:321-553,"
            "left:563-600,sright:606-640,fwd:650-800,turbo:650-800";
        std::string expected;
        std::string got;
        std::string why;
        if (!readText("golden/L7_corridor_locked.txt", &expected)) {
            printf("FAIL L7_corridor_locked: no golden file golden/L7_corridor_locked.txt\n");
            failures++;
        } else if (!kurtCorridorDump(7, 300, "0,380,15,90", kLockedSchedule, data, spritesArg,
                                     &got, &why)) {
            printf("FAIL L7_corridor_locked: %s\n", why.c_str());
            failures++;
        } else if (got == expected) {
            printf("PASS L7_corridor_locked: the locked door is solid, Kurt stops at y=478.4\n");
        } else {
            printf("FAIL L7_corridor_locked: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }
    // The platform probe's goldens (tests/scripts/platform_probe; see kurtPlatformsDump for
    // the columns). Each case is the probe's command line.
    {
        struct PlatformCase {
            const char* golden;
            int level;
            int steps;
            const char* schedule;
            const char* what;
        };
        static const PlatformCase kPlatformCases[] = {
            // L3's hover ship (Kurt.Keeper.cs): the empty XGS a head shot leaves (spawn_relative
            // XGS 117762) lands and becomes a keeping platform (set_targetable 2), and waits
            // on if_player_on_me -- false while Kurt stands beside it (restart 117823 for 200
            // steps). Dropped onto it he lands on it (Solids.Platform), 114 turns true (spawns
            // XF, H9START, follow_path, restart 118098) and it flies him through HMO_9, kept
            // at its XY and carried round its turns.
            {"L3_plat_keeper", 3, 700,
             "tp:1:HMO_9:-200:2950:-195:0,spawn:2:XGS:-200:2965:-185:0:117762,"
             "tp:220:HMO_9:-195:2965:-268:0",
             "the hover ship keeps Kurt and flies him (114 false, then true)"},
            // L7's tank (XTANK/XTANKT, platforms 0x100): Kurt lands on the turret's top and
            // the object loop's Carry takes him along as it drives (ScriptRuntime.cs:983).
            {"L7_plat_tank", 7, 300, "tp:1:DANT_3:136:1292:101:0",
             "Kurt stands on the tank and is carried"},
            // L6's flying gun XBGUN (0x800100): a keeping platform lifts him and keeps him
            // while the guns around fire (their bullets and explosions are package #4's
            // opcodes, which made this stop at step 66 before that package landed).
            {"L6_plat_gun", 6, 600, "tp:1:OLYM_4:-1732:90:-1285:90",
             "the flying gun keeps Kurt"},
            // L7's locked X7DOOR (a wall, Solids.Walk): he stops at its face (y 478.4), slides
            // along it while strafing, and touches it (Kurt.Touched); it also blocks the
            // camera's sight (0x1000000, ClipView).
            {"L7_plat_door", 7, 300,
             "tp:1:DANT_1:0:380:15:90,fwd:61-261,turbo:61-261,sright:200-240",
             "the locked door stops Kurt, he slides along it, and it clips the view"},
            // L4's hatch (MEAT_1 group 1, hit flags 0x48): landing on it is a contact
            // (Kurt.Contact); KurtTouchesGroups hits the group (hit type -11), its script
            // opens it and he falls through.
            {"L4_plat_trigger", 4, 60, "tp:1:MEAT_1:-5:3:226:90",
             "a floor trigger fires through KurtTouchesGroups"},
        };
        for (size_t i = 0; i < sizeof(kPlatformCases) / sizeof(kPlatformCases[0]); i++) {
            const PlatformCase& c = kPlatformCases[i];
            const std::string path = std::string("golden/") + c.golden + ".txt";
            std::string expected;
            std::string got;
            std::string why;
            if (!readText(path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.golden, path.c_str());
                failures++;
            } else if (!kurtPlatformsDump(c.level, c.steps, c.schedule, data, spritesArg, &got,
                                          &why)) {
                printf("FAIL %s: %s\n", c.golden, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.golden, c.what);
            } else {
                printf("FAIL %s: %s\n", c.golden, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // L6 OLYM_4's cannon (in-game level 2): Kurt drops onto the middle XBGUN (script 23159)
    // and holds fire. It rises (114, restart 23182), fires while he stands on it and fires
    // (209, the shot loop at 23250), and after the third shot (if_var 2.0 > 2) takes path
    // 35601 (23266, 23303: the window, group 1, shatters), shows COLYM_4 and carries him
    // through it (23375); it then dies and he drops into OLYM_5. Before the 209 fix
    // (upstream C# 3b072d2a) his feet were 0.02 above its model's top and it never fired:
    // the script sat at 23193 for good. Checked by markers, not a golden: the src/ oracle
    // is v1.1.1, which still has the old test, and no C# probe ran this schedule.
    {
        std::string got;
        std::string why;
        if (!kurtPlatformsDump(6, 700, "tp:1:OLYM_4:-1732:92:-1260:90,fire:60-700", data,
                               spritesArg, &got, &why)) {
            printf("FAIL L6_cannon: %s\n", why.c_str());
            failures++;
        } else {
            const char* kSteps[] = {
                "on=XBGUN#1002",                          // he stands on it
                "/r23250\n",                              // it fires (209 true)
                "/r23266\n",                              // third shot: off on its path
                "/r23303\n",                              // the window breaks at frame 4
                "arena=COLYM_4 state=Shot",               // it carries him into COLYM_4,
                "on=XBGUN#1002",                          // still standing on it
                "arena=OLYM_5",                           // and he ends up in OLYM_5
            };
            size_t at = 0;
            const char* missing = nullptr;
            for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); i++) {
                at = got.find(kSteps[i], at);
                if (at == std::string::npos) {
                    missing = kSteps[i];
                    break;
                }
            }
            const size_t last = got.rfind("\ns=700 ");
            if (missing != nullptr) {
                printf("FAIL L6_cannon: never reached \"%s\" in order\n", missing);
                failures++;
            } else if (last == std::string::npos ||
                       got.compare(last + 1, 25, "s=700 arena=OLYM_5 state=") != 0) {
                printf("FAIL L6_cannon: Kurt is not in OLYM_5 at the end\n");
                failures++;
            } else {
                printf("PASS L6_cannon: Kurt fires the middle XBGUN three times, it breaks "
                       "the window and flies him through COLYM_4 into OLYM_5\n");
            }
        }
    }
    // ---- end of package #3 -------------------------------------------------------------

    // The rides probe's goldens (tests/scripts/ride_probe; see kurtRideDump for the columns),
    // package #9. Each case is the probe's command line. L7 DANT_9's XD2: killed (its death
    // script, 127789), it turns to face Kurt and makes itself rideable while he is behind it
    // (127906); Kurt mounts it on touching it, and its script branches on opcode 171 (127847 ->
    // 128004: health 50, the doors, then 128092 waiting for Kurt in the rect by the guards).
    // Kurt's yaw is 90 at every mount (the XD2's own), so his walk goes through the trig LUT
    // only where a schedule turns him: the feet's 1/64 columns absorb that (as
    // L7_plat_door's walk does), and the turned walks below were checked byte for byte.
    {
        struct RideCase {
            const char* golden;
            int level;
            int steps;
            const char* schedule;
            const char* what;
        };
        static const RideCase kRideCases[] = {
            // The whole ride: firing, Kurt runs into the rideable XD2 and does not get in
            // (TryMount refuses while Kurt.Firing); he lets go and is in (hidden, Walk.Riding,
            // 171 true, hits to the walker); it follows him (DUMMY while he steers); his fire
            // is its alarm (ALERT, AlarmTicks 10, flag 1); jump and strafe do nothing; he
            // turns it; teleported beside the guards' rect he drives in, its script waits
            // 3.5 s, clears the flag and he jumps out (JumpOff), then it blows up (knocked).
            {"L7_ride_xd2", 7, 700,
             "tp:1:DANT_9:45:4868:-25:90,kill:60:XD2,fwd:200-235,fire:200-224,fire:245-252,"
             "jump:262-270,sleft:262-280,left:282-292,fwd:285-300,tp:320:DANT_9:162:4880:-37:90,"
             "fwd:330-420",
             "Kurt mounts the XD2, rides, steers, fires its alarm and jumps out (171 true, "
             "then false)"},
            // Hits on Kurt go to the XD2 (HurtKurt -> TakesHits): 30 takes it to 20, 30 more
            // kills it, Rides.Lost puts him out with a running jump and 50 damage.
            {"L7_ride_lost", 7, 400,
             "tp:1:DANT_9:45:4868:-25:90,kill:60:XD2,fwd:200-235,hit:250:30,hit:270:30,"
             "fwd:300-320",
             "the XD2 takes Kurt's hits and its loss throws him out (Rides.Lost)"},
            // The tests' --ride (Rides.RideWalker) on the live XD2, steered at its yaw of
            // 321 and turned, then its rideable flag cleared: he jumps out. The live walker
            // then turns to face him through turn_to_target_offset (opcode 235's atan2, the
            // LUT's here and a ULP off the oracle's from step 132), so the run stops at 128.
            {"L7_ride_test", 7, 128,
             "tp:1:DANT_9:45:4868:-25:90,ride:30:XD2,fwd:40-80,right:60-70,off:120:XD2",
             "Rides.RideWalker mounts the live XD2; a cleared flag lets Kurt out"},
        };
        for (size_t i = 0; i < sizeof(kRideCases) / sizeof(kRideCases[0]); i++) {
            const RideCase& c = kRideCases[i];
            const std::string path = std::string("golden/") + c.golden + ".txt";
            std::string expected;
            std::string got;
            std::string why;
            if (!readText(path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.golden, path.c_str());
                failures++;
            } else if (!kurtRideDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.golden, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.golden, c.what);
            } else {
                printf("FAIL %s: %s\n", c.golden, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // ---- end of package #9 -------------------------------------------------------------

    // The snowboard probe's goldens (tests/scripts/board_probe; see kurtBoardDump for the
    // columns), package #10. Each case is the probe's command line. L4 CMEAT_3's XSNOWB: Kurt
    // dropped above it lands on it (script 147319 makes it rideable once he is on it) and
    // TryMount puts him on the board; from then on he collides with his arena alone
    // (SolidArenas) and the board's step (Snowboard.Update) moves him down the run. Every
    // Kurt and board column is byte for byte: the board's trig is glibc's (= MathF's), only
    // the camera's x and y (Kurt's facing, the LUT's) are left out of the dump.
    {
        struct BoardCase {
            const char* golden;
            int steps;
            const char* schedule;
            const char* what;
        };
        static const BoardCase kBoardCases[] = {
            // Mounting, then the controls: faster (fwd), the carve angle left and right
            // (the 15-tick ramp, the snap straight on reversing, +-30, SKITURN past 22.5),
            // the strafe keys winning a tie with the turn keys, braking (back), a jump off a
            // ledge (K_SURFJ held on frame 5, the pivot dipping, firing stopped, SKILAND on
            // landing after 15 ticks in the air, the late landing's K_SURFJ 6-10), firing on
            // the ground (the flash at K_SURF's -42,12), a held jump key that does not
            // re-jump, the path heading and time, the board's place, yaw and pitch.
            {"L4_board_ride", 700,
             "tp:1:CMEAT_3:420:4285:-570:90,fwd:60-120,left:130-160,right:165-215,sleft:220-226,"
             "right:220-226,back:240-290,jump:300-306,fire:395-420,jump:400-404,left:450-470,"
             "sright:450-470,jump:500-560,fire:600-640",
             "Kurt mounts XSNOWB and rides it: steering, speed, jumps, the gun, the sounds"},
            // The script's flag cleared mid-run (Rides.Dismount -> Snowboard.GetOff): the
            // sounds stop, the board is thrown on at 1.1x his velocity (gravity 64, no
            // friction), he keeps its speeds as his walking ones, and collides with the
            // drawn arenas again; then he walks.
            {"L4_board_off", 400,
             "tp:1:CMEAT_3:420:4285:-570:90,fwd:60-100,off:200:XSNOWB,fwd:260-300",
             "a cleared rideable flag throws the board on and Kurt walks off"},
            // The board killed under him (Rides.Lost): off, without the XD2's 50 damage.
            {"L4_board_lost", 300, "tp:1:CMEAT_3:420:4285:-570:90,kill:150:XSNOWB,fwd:200-240",
             "the board's loss puts Kurt off unhurt (Rides.Lost)"},
            // Kurt killed on the board: Rides.Update's `_board != null && Health == 0` gets
            // him off (GetOff) and he dies on foot.
            {"L4_board_dead", 260, "tp:1:CMEAT_3:420:4285:-570:90,hit:150:200",
             "Kurt's death gets him off the board"},
            // The whole run from halfway down CMEAT_3 (teleported on the board): the doorway
            // into MEAT_4 (his one solid arena follows him), a jump over MEAT_4's lip at
            // 9356 (without it he stays against it), the two XTGUN turrets he rams (killed, 5
            // damage each: steps 2340, 2645), CMEAT_4, MEAT_5 and its end box (script 147357
            // clears the flag at 6054: Kurt off, the board thrown and, on its first collision,
            // dead). Printed every 100 steps and around the rams and the end.
            {"L4_board_end", 6200,
             "tp:1:CMEAT_3:420:4285:-570:90,tp:100:CMEAT_3:279:7312:-1130:90,jump:1880-1890,"
             "every:100,show:2335-2345,show:2640-2650,show:6040-6090",
             "the run to MEAT_5's end box: rams, arenas, the script lets Kurt off"},
        };
        for (size_t i = 0; i < sizeof(kBoardCases) / sizeof(kBoardCases[0]); i++) {
            const BoardCase& c = kBoardCases[i];
            const std::string path = std::string("golden/") + c.golden + ".txt";
            std::string expected;
            std::string got;
            std::string why;
            if (!readText(path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.golden, path.c_str());
                failures++;
            } else if (!kurtBoardDump(4, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.golden, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.golden, c.what);
            } else {
                printf("FAIL %s: %s\n", c.golden, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // ---- end of package #10 ------------------------------------------------------------

    // The bomber probe's goldens (tests/scripts/bomber_probe; see kurtBomberDump for the
    // columns), package #11. Each case is the probe's command line. L7 DANT_5: RideTest's
    // way onto the XE -- Kurt teleported in front of the comm device, its group 16 hit
    // once, which calls the XE; once its script makes it rideable (step 620) Kurt is put
    // 1 unit above it and mounts on touching it (TryMount's Bombers, MountBomber).
    {
        struct BomberCase {
            const char* golden;
            int level;
            int steps;
            const char* schedule;
            const char* what;
        };
        static const BomberCase kBomberCases[] = {
            // The mount: firing, Kurt above the rideable XE does not get on (TryMount refuses
            // while Kurt.Firing) and falls through it; he lets go and is on (hidden, Kurt.Ride
            // set, hits to the XE); the view sinks 50 -> 0 in 2 s, then the XE is hidden and
            // not solid; locked, fire drops nothing and the HUD is off; a 5-point hit goes to
            // the XE and back to him (PassDamage); the script unlocks the controls.
            {"L7_bomber_mount", 7, 790,
             "tp:30:DANT_5:100:2320:-60:90,call:90:DANT_5:16,board:91,print:600,fire:600-650,"
             "hit:700:5,fire:720-724",
             "Kurt boards the XE once he stops firing; the view sinks and the script unlocks"},
            // The bombs: a drop at the centre (straight down), the cursor steered by the
            // turn keys, forward, strafe and turn together (turn wins), the mouse (only while
            // no key moves it), held fire drops once, the ten bombs run out and come back one
            // a second, a far aim at the box's edge; the bombs fall (Items' kind 0x81) and
            // blow up on what they hit, DROP following each.
            {"L7_bomber_bombs", 7, 1100,
             "tp:30:DANT_5:100:2320:-60:90,call:90:DANT_5:16,board:91,print:750,fire:760-762,"
             "right:780-800,fwd:800-815,fire:815-817,sleft:820-835,right:830-840,fire:836-838,"
             "fire:845-846,fire:850-852,fire:860-890,fire:895-897,fire:900-902,fire:905-907,"
             "fire:910-912,fire:915-917,fire:920-922,fire:925-927,mouse:940-950:30:-15,"
             "back:945-955,fire:960-962,left:970-1000,sright:970-985,fire:990-992,"
             "right:1000-1040,back:1005-1030,fire:1040-1042,fire:1070-1072",
             "the cursor, the drops, the refill and the bombs' fall"},
            // A target: the XE passes over the guard XG#3500 (walking +x) at step 1450; the
            // mouse puts the cursor ahead of it (76, -65 px off the centre: 33 units on
            // the ground at the XE's yaw of 53.7, from 82 up) and four bombs fall there
            // (no object's part under the ray: the arena's floor, Raycast) -- their blasts
            // take its 60 and it dies (kills 1).
            {"L7_bomber_target", 7, 1660,
             "tp:30:DANT_5:100:2320:-60:90,call:90:DANT_5:16,board:91,print:1440,"
             "mouse:1448-1449:228:-195,fire:1450-1452,fire:1456-1458,fire:1462-1464,"
             "fire:1468-1470",
             "bombs led onto a walking guard kill it"},
            // The player's way, no test hooks (the 3DS harness's WALK, docs/PORT_GAPS.md #11):
            // from step 1 in DANT_5 Kurt turns left to the comm device and shoots it (the
            // XE is called at 158), turns right to yaw 120, walks into the landed XE once
            // it is rideable and mounts on the floor (752); the script unlocks the controls
            // (874) and he drops a bomb (880).
            {"L7_bomber_walk", 7, 900,
             "tp:1:DANT_5:100:2320:-60:90,left:60-125,fire:130-180,right:200-250,fwd:720-800,"
             "fire:880-882,print:740",
             "Kurt calls the XE by shooting the comm device and walks into it"},
            // The end: the script clears the rideable flag (and sets its health, which
            // PassDamage hands back as a negative hit Kurt ignores); Kurt drops from where
            // the XE is (Player::getOffBoard: the view, the ride step and the hiding go) and
            // it shows again.
            {"L7_bomber_end", 7, 6450,
             "tp:30:DANT_5:100:2320:-60:90,call:90:DANT_5:16,board:91,print:6320",
             "the script ends the ride and Kurt drops from the XE"},
        };
        for (size_t i = 0; i < sizeof(kBomberCases) / sizeof(kBomberCases[0]); i++) {
            const BomberCase& c = kBomberCases[i];
            const std::string path = std::string("golden/") + c.golden + ".txt";
            std::string expected;
            std::string got;
            std::string why;
            if (!readText(path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.golden, path.c_str());
                failures++;
            } else if (!kurtBomberDump(c.level, c.steps, c.schedule, data, spritesArg, &got,
                                       &why)) {
                printf("FAIL %s: %s\n", c.golden, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.golden, c.what);
            } else {
                printf("FAIL %s: %s\n", c.golden, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // ---- end of package #11 ------------------------------------------------------------

    // The end of a level, end to end against the oracle probe (tests/scripts/end_probe; see
    // levelEndDump for the columns): special_event <= 50 -> EndTheLevel -> EndLevel.cs, and
    // LevelEnded's step. The schedules are the probe's arguments, byte for byte. Kurt does
    // not WALK before the end in any of them: his walk turns his facing through the LUT
    // (Player::facing, R6) and is a ULP off the oracle's, which the feet's 1/64 columns
    // absorb but the pieces, which turn around his feet, do not. Turning and firing are
    // exact, and the keys held after the end (forward, strafe, jump) are what Frozen must
    // ignore.
    {
        struct EndCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const EndCase kEndCases[] = {
            {"L3_end_level", "golden/L3_end_level.txt", 3, 480,
             "fire:10-300,right:20-40,fwd:50-300,sleft:60-120,jump:70-80,end:45:0,end:60:7",
             "HMO_1 torn highest first and lifted around Kurt, who stops firing, freezes, takes "
             "off (K_TAKEOF, K_FLOATC), turns, rises, tilts the view and goes white; a second "
             "special_event is LevelOver's no-op; LevelEnded at 444"},
            {"L7_end_level", "golden/L7_end_level.txt", 7, 520,
             "left:5-30,end:40:0,fwd:41-300,jump:50-60",
             "DANT_1 (665 pieces) torn while Kurt lands from the spawn fall (K_LAND frozen), "
             "the arena pitch still easing"},
            {"L7_end_air", "golden/L7_end_air.txt", 7, 460,
             "left:5-20,end:25:0,fwd:30-300,jump:50-60",
             "the end mid-fall: Frozen stops the fall in the air and Rising drops the camera's "
             "air pitch (3.0) to 0 and holds it there"},
            {"L8_end_level", "golden/L8_end_level.txt", 8, 480,
             "fire:5-100,right:20-40,end:30:50,fwd:31-200",
             "GUNT_1 ended by special_event 50, the last end value, while the objects keep "
             "running"},
        };
        for (size_t i = 0; i < sizeof(kEndCases) / sizeof(kEndCases[0]); i++) {
            const EndCase& c = kEndCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!levelEndDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The combat and hazard opcodes, end to end against the oracle probe (tests/scripts/
    // hazards_probe; see kurtHazardsDump for the columns): 158 touch_damage, 159
    // spawn_box, 167 move_to_bomb, 172 explosion, 178 explosion_damage, 184 explode, and
    // path_speed_by_kurt's consumer. The schedules are the probe's arguments, byte for
    // byte: each golden's first line repeats its schedule. A `run:` token enters the
    // level's own bytecode at an opcode (the CMI offset of the instruction).
    {
        struct HazardsCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const HazardsCase kHazardsCases[] = {
            {"L6_hazards_bomb", "golden/L6_hazards_bomb.txt", 6, 300,
             "tp:1:OLYM_1:-1142:-935:-68,give:2:SW_INTER,use:21-23,watch:XT",
             "the World's Most Interesting Bomb thrown by OLYM_1's XT: move_to_bomb (167) "
             "draws the aside and walks it over"},
            {"L5_hazards_touch", "golden/L5_hazards_touch.txt", 5, 600,
             "tp:1:MUSE_2:190:199:-570,watch:XGUNTAM",
             "MUSE_2's XGUNTAM rides its path through Kurt: touch_damage (158) hurts him "
             "once per part"},
            {"L8_hazards_bang", "golden/L8_hazards_bang.txt", 8, 400,
             "tp:1:GUNT_2:-209:480:40,fire:60-300,watch:XBANG,watch:BOLT,watch:BIGBOLT",
             "GUNT_2's XBANG dies: explode (184), its uncounted blast on everything, the "
             "kill, its bolts"},
            {"L4_hazards_snow", "golden/L4_hazards_snow.txt", 4, 600,
             "tp:1:MEAT_4:284:7382:-1139,fwd:100-400,turbo:100-400,watch:XGSNOW",
             "MEAT_4's snowboarder: path_speed_by_kurt (164) eases its path speed by Kurt's "
             "lead, and the path pushes it"},
            // docs/DIVERGENCES.md D5: package 4's dropped explode-beside-a-victim run. One
            // piece outlived the C#'s by 10 steps (s=152-161, 284 vs 283) because the
            // bounce scaled the normal by (dot * 1.4) where the C# scales it by dot, then
            // by 1.4; the effects' bounce had the same order. `flying` hashes every
            // piece's and effect's motion, so the old debris bounce fails here at s=68
            // and the old effects bounce at s=66 (measured 2026-11-20).
            {"L8_hazards_debris", "golden/L8_hazards_debris.txt", 8, 400,
             "tp:1:GUNT_2:-209:480:40,run:3:XBANG:24840:0:105:0,fire:60-300,watch:XBANG,"
             "watch:BOLT,watch:BIGBOLT,flying",
             "a second XBANG explodes beside GUNT_2's: the debris' and effects' bounces "
             "(Debris.Move, Effects.Move) in the C#'s operation order"},
            {"L8_hazards_kill", "golden/L8_hazards_kill.txt", 8, 300,
             "tp:1:GUNT_2:-156:53:-4,run:3:XG:21471:40:0:0,run:20:XBANG:25067:44:0:0,"
             "watch:XBANG",
             "XBANG's explode (184) next to a grunt: the blast kills it, and the kill is not "
             "counted (Kills.Ignored)"},
            {"L4_hazards_blast", "golden/L4_hazards_blast.txt", 4, 500,
             "tp:1:MEAT_5:-134:13260:-1985,run:3:XC:55585:-20:0:0,run:5:XMT:55470:-20:0:0,"
             "run:7:XMT:55470:-20:0:0,watch:XMT,watch:XC,watch:SW_THUMP",
             "MEAT_5's group 1 blown up twice: explosion_damage (178) on objects and groups "
             "kills an XC and counts it, a BIGBOLT's touch_damage, XC's FIRE boxes (159)"},
            {"L4_hazards_touch", "golden/L4_hazards_touch.txt", 4, 300,
             "tp:1:MEAT_5:-130:13260:-1980,run:5:XMT:54289:-20:0:0,run:5:XMT:54289:-20:0:0,"
             "watch:XMT",
             "two XMTs in one place: touch_damage (158) hits the other and branches (flags 2)"},
            {"L3_hazards_boxes", "golden/L3_hazards_boxes.txt", 3, 300,
             "tp:1:HMO_3:3:1241:-931,run:5:XG:44160:0:0:0,run:9:XG:8127:10:0:0,"
             "run:13:XG:52115:-10:0:0,watch:PULSE,watch:FIRE",
             "spawn_box (159) at a point (PULSE) and at the object (FIRE), and HMO_4's "
             "explosion (172) at a point"},
            {"L8_hazards_points", "golden/L8_hazards_points.txt", 8, 300,
             "tp:1:GUNT_10:-43:3300:-380,run:5:XBSHIP:137303:0:20:0,watch:XBSHIP",
             "GUNT_10's XBSHIP turret explosion (172) at a reference point of its model"},
            // Re-pinned with package 8 (the fans' sparks, FanSpark): both were dropped by
            // package 4 because the C++ fans let out no spark, which moves the scripts'
            // random and the piece list from the first spark on.
            {"L4_hazards_fan", "golden/L4_hazards_fan.txt", 4, 700,
             "tp:1:MEAT_10:-218:23907:-1338,watch:XBN_BOMB",
             "MEAT_10's XBN_BOMBs beside the arena's fans: the fans' still sparks (FanSpark) "
             "from s=4, lifted by the updraft, in the scripts' random"},
            {"L6_hazards_balls", "golden/L6_hazards_balls.txt", 6, 600,
             "tp:1:OLYM_2:-1574:-47:-708,watch:XBG_B1",
             "OLYM_2's XBG_B1 balls with COLYM_2 as the second arena: its fan's sparks (from "
             "s=22) are live there too"},
            // The opcode parity branch (2026-11-26, docs/OPCODE_WATCHLIST.md): jump_to (226)
            // swings OLYM_6's pendulum, and before the port it hung still. The golden is the
            // C# oracle's (hazards_probe at v1.2.6, whose Swing, jump_to and RopeView lines
            // equal 1.1.1's); the rope column also equals 1.1.1 + 683a53ef's byte for byte.
            {"L6_hazards_swing", "golden/L6_hazards_swing.txt", 6, 1200,
             "tp:1:OLYM_6:-1894:3154:-2048,rope:XSWINGB",
             "OLYM_6's pendulum XSWINGB swings under its pivot (jump_to, 226; ObjectMotion."
             "Swing), draws its rope and knocks Kurt down (PENDHIT); Kurt is on the pivot's "
             "line, so the plane stays at yaw 0"},
            // Review fix: Kurt 100 units off the pivot's line, so at every turning point the
            // plane turns towards him, clamped to 15 degrees, at 10 degrees a second (the
            // yaw runs 0 to -15 and back). Equal to v1.2.6 byte for byte, and its rope column
            // to 1.1.1 + 683a53ef's.
            {"L6_hazards_swing_turn", "golden/L6_hazards_swing_turn.txt", 6, 1200,
             "tp:1:OLYM_6:-1894:3254:-2048,rope:XSWINGB",
             "the pendulum's plane turns towards Kurt off its line at the turning points "
             "(Swing's clamp and MoveToward)"},
            // arena_texture_frame (133): the comm device's screen M_COMM, which every
            // frontend drew at frame 0 before the parity branch. The `frames` column is
            // the scripts' frame and the frame LevelView draws (FrameOf); both goldens are
            // the C# oracle's (v1.2.6; L4's also equals 1.1.1 + 683a53ef byte for byte).
            {"L4_hazards_comm", "golden/L4_hazards_comm.txt", 4, 400,
             "tp:1:MEAT_3:293:4166:-548,frames",
             "MEAT_3's comm screen M_COMM runs at 6 frames a second (arena_texture_frame, "
             "133, mode 1), wrapping over its 6 frames"},
            {"L7_hazards_comm", "golden/L7_hazards_comm.txt", 7, 600,
             "tp:1:DANT_5:100:2320:-60,frames,left:20-85,fire:100-400",
             "DANT_5's comm device shot by the chain gun: its screen goes from 6 to 12 "
             "frames a second (133)"},
            // set_2d0_block (242): the dog XBN's four tethers, drawn by RopeView's 0xFF mode
            // from reference points 1-4 (the dog's model has none, so its origin) to the
            // block's points. Both goldens are the C# oracle's (v1.2.6); their rope column
            // also equals 1.1.1 + 683a53ef's byte for byte.
            {"L5_hazards_dog", "golden/L5_hazards_dog.txt", 5, 300,
             "tp:1:MUSE_5:465:-6:-2263,rope:XBN",
             "MUSE_5's dog XBN held by four tether lines (set_2d0_block, 242; RopeView "
             "AllLines)"},
            {"L8_hazards_dog", "golden/L8_hazards_dog.txt", 8, 300,
             "tp:1:GUNT_10:-43:3300:-380,rope:XBN",
             "GUNT_10's dog XBN and its four tethers (242)"},
            // spawn_box's sprites (159, upstream 84ad71e4): L3_hazards_boxes' schedule with
            // the `sprites` column, EffectsView's whole frame (the effects' and the boxes'
            // quads) as bits. The golden is the C# oracle's (v1.2.6, which has 84ad71e4); the
            // same oracle with 84ad71e4 reverted differs on 60 lines, so the column sees it.
            {"L3_hazards_boxsprites", "golden/L3_hazards_boxsprites.txt", 3, 300,
             "tp:1:HMO_3:3:1241:-931,run:5:XG:44160:0:0:0,run:9:XG:8127:10:0:0,"
             "run:13:XG:52115:-10:0:0,watch:PULSE,watch:FIRE,sprites",
             "the PULSE and FIRE boxes' sprites stretched over the box's width and height "
             "(spawn_box, 159; EffectsView as upstream 84ad71e4)"},
        };
        for (size_t i = 0; i < sizeof(kHazardsCases) / sizeof(kHazardsCases[0]); i++) {
            const HazardsCase& c = kHazardsCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtHazardsDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // EffectsView::build over L8_hazards_debris's storm (effects_dump.h): the frames,
    // culls and halos as bits, written by the build before the effects caches.
    {
        std::string expected;
        std::string got;
        std::string why;
        if (!readText("golden/L8_effects_storm.txt", &expected)) {
            printf("FAIL L8_effects_storm: no golden file\n");
            failures++;
        } else if (!kurtHazardsDump(8, 400, kEffectsStormSchedule, data, spritesArg, &got, &why,
                                    test::effectsTraceNew())) {
            printf("FAIL L8_effects_storm: %s\n", why.c_str());
            failures++;
        } else if (got == expected) {
            printf("PASS L8_effects_storm: the debris storm's effects frames, culls and halos, "
                   "bit for bit\n");
        } else {
            printf("FAIL L8_effects_storm: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // Kurt in the fans' updrafts and the wind zones' slide, against the C# probe
    // tests/scripts/wind_probe (see kurtWindDump for the columns): package 8,
    // Kurt.Updraft.cs, Kurt.Slide.cs, ScriptRuntime.KurtUpdraft / FanSpark / WindZone and
    // opcode 224. The schedules are the probe's arguments, byte for byte. Every case also
    // pins the fans' sparks: FanSpark draws from the scripts' random and the updraft lifts
    // the sparks (the debris and dvz columns).
    {
        struct WindCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* at;
            const char* schedule;
            const char* pass;
        };
        static const WindCase kWindCases[] = {
            {"L3_wind_shaft", "golden/L3_wind_shaft.txt", 3, 330, "391,2427,-390,90",
             "fwd:150-200,jump:240-250",
             "CHMO_6's shaft fan opens the chute (CHUTEOUT, FAN) and lifts Kurt at 107 u/s "
             "through the floor doorway into HMO_8, where the second arena's fan holds him "
             "hovering on the wobble; he drifts out, the chute closes and he lands"},
            {"L8_wind_fan", "golden/L8_wind_fan.txt", 8, 300, "-505,858,-45,90",
             "sright:30-90,jump:150-200",
             "GUNT_3's fan; Kurt strafes out mid-rise at 67 u/s and leaves at the 40 u/s "
             "UpdraftExitSpeed, the FAN loop stops and the chute closes"},
            {"L6_wind_tunnel", "golden/L6_wind_tunnel.txt", 6, 450, "-1151,-787,-60,90",
             "fwd:100-200,back:250-330,left:350-380",
             "COLYM_1's wind (opcode 224) starts the slide on the floor: K_SLIP, K_SLIDE, "
             "K_FSLIDE raising the cap, K_BSLIDE and BUTBRAKE lowering it, the turn keys, "
             "air ticks over the bumps and the slope's roll"},
            {"L6_wind_arena", "golden/L6_wind_arena.txt", 6, 140, "-1895,2801,-1980,90", "",
             "COLYM_5's slide carries Kurt into OLYM_6: Kurt.EnterArena gives it 15 ticks, "
             "then he falls and lands"},
            {"L6_wind_fan", "golden/L6_wind_fan.txt", 6, 960, "-1987,2602,-2640,90",
             "fwd:870-920",
             "COLYM_7's wind slides Kurt down to the fan, wind_zone off stops the slide, the "
             "fan lifts him up the shaft into OLYM_8 and holds him there from the second "
             "arena"},
            {"L6_wind_wall", "golden/L6_wind_wall.txt", 6, 220, "-1709,43,-700,90", "",
             "COLYM_2's slide runs into a wall at step 103 and SlideMove turns it along what "
             "Kurt moved, at the mean of the two speeds"},
        };
        for (size_t i = 0; i < sizeof(kWindCases) / sizeof(kWindCases[0]); i++) {
            const WindCase& c = kWindCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtWindDump(c.level, c.steps, c.at, c.schedule, data, spritesArg, &got,
                              &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The cutscenes (package 13), end to end against the oracle probe
    // (tests/scripts/cutscene_probe; see cutsceneDump for the columns): special_event's arms
    // above 50, the Cutscene branch of RunTick, the camera's shots to the bit, Kurt Frozen
    // and Visible, the objects' Visible, GameFinished. Each runs where the retail script
    // calls it (L5 MUSE_5, L7 DANT_10, L8 GUNT_10), from the controller of Kurt's arena.
    // In L5 Kurt stands at the far corner of MUSE_5 (465, -6): next to Gunter (420, 100),
    // Gunter's own yaw leaves the oracle's within a second, a divergence in his script that
    // predates the cutscenes (shot 11 is placed from his yaw, so it would show).
    {
        struct CutsceneCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const CutsceneCase kCutsceneCases[] = {
            {"L7_cut_boss", "golden/L7_cut_boss.txt", 7, 160,
             "at:495:5005:24:0,fire:10-80,ev:40:93,ev:120:92",
             "DANT_10's boss intro (0x5d, shot 12 from 1158,5006,315, its position kept): Kurt "
             "stops firing, freezes and hides, the pickups hide, 92 ends it"},
            {"L8_cut_boss", "golden/L8_cut_boss.txt", 8, 300,
             "at:40:3221:-358:180,fire:10-90,ev:60:91,ev:200:92",
             "GUNT_10's (0x5b, shot 12 from -121,3347,-350): every object runs on, flagged or "
             "not, until 92"},
            {"L5_cut_gunter", "golden/L5_cut_gunter.txt", 5, 200,
             "at:465:-6:-2263:0,ev:30:61,ev:150:53",
             "Gunter's (0x3d, XGUNTAM by FindObjectNamed, shot 11: placed 45 ahead of him and "
             "held), 53 ends it"},
            {"L5_cut_dog", "golden/L5_cut_dog.txt", 5, 340,
             "at:465:-6:-2263:0,ev:30:52,ev:54:54,ev:150:55,ev:300:53",
             "the dog's (0x34, XBN): shot 0 to 1 at 12 units, the orbit easing in, 54 the "
             "oracle's no-op, 55 to shot 2 (25 units, rising), only the cutscene types shown"},
            {"L5_cut_strike", "golden/L5_cut_strike.txt", 5, 200,
             "at:465:-6:-2263:0,fire:5-60,ev:30:51,ev:120:53",
             "51's cutscene half (0x47): Kurt teleported to the controller, which moves 4 "
             "along y, shown; the shot from 10 behind it (the strike scene: L5_strike_event51)"},
            {"L5_cut_end", "golden/L5_cut_end.txt", 5, 120,
             "at:465:-6:-2263:0,fire:5-60,ev:30:81",
             "the end of the game (0x51): GameFinished, Kurt shown, nothing else runs or shows"},
            {"L5_cut_dog_long", "golden/L5_cut_dog_long.txt", 5, 3760,
             "at:465:-6:-2263:0,ev:30:52,ev:1000:55,ev:3720:53,show:3600",
             "the dog's a minute on: the orbit's 1800-tick ease-in runs out (timer 0) and the "
             "shot takes its settled weights (0.7/0.3 pitch, 0.3/0.7 yaw) and point"},
            {"L5_cut_snapshot", "golden/L5_cut_snapshot.txt", 5, 110,
             "at:465:-6:-2263:0,ev:40:61,ev:100:53,show:36,snap",
             "CanSnapshot over Gunter's cutscene: a save allowed before it, refused on every "
             "step it runs (Cutscene != 0), allowed again once 53 ends it"},
        };
        for (size_t i = 0; i < sizeof(kCutsceneCases) / sizeof(kCutsceneCases[0]); i++) {
            const CutsceneCase& c = kCutsceneCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!cutsceneDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The air strike and the strike scene (package #14), end to end against the oracle probe
    // (tests/scripts/strike_probe; see strikeDump for the lines). The schedules are the probe's
    // arguments, byte for byte: each golden's first line repeats its schedule.
    {
        struct StrikeCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const StrikeCase kStrikeCases[] = {
            {"L5_strike_gunter", "golden/L5_strike_gunter.txt", 5, 470,
             "at:250:121:-1622:0,snipe:20-22,give:25:SW_BONES,aim:30:00000000:3FA00000,"
             "fire:80-82,give:430:SW_BONES,fire:440-442",
             "the last levels' dive: X_STRIKB full screen without Bones (the game waits 4 s), "
             "X_STRIKE's curve, the dive and crash into MUSE_4's XGUNTAM (if_hit_weapon -6), "
             "then UsedUp: a second call is a raspberry"},
            {"L3_strike_teeth", "golden/L3_strike_teeth.txt", 3, 760,
             "snipe:20-22,give:25:SW_BONES,aim:30:42B40000:C1300000,fire:80-82,skip:140",
             "Bones' strike before the last levels: the scene skipped (Esc), the curve over "
             "the target, 9 X_TOOTH grenades one every 15 units, their blasts on XS, the end"},
            {"L5_strike_event51", "golden/L5_strike_event51.txt", 5, 400,
             "at:465:-6:-2263:0,ev:30:51,ev:330:53",
             "special_event 51 end to end: X_STRIKD full screen with Kurt (the game waits), "
             "then 51's cutscene (0x47) until 53 ends it"},
        };
        for (size_t i = 0; i < sizeof(kStrikeCases) / sizeof(kStrikeCases[0]); i++) {
            const StrikeCase& c = kStrikeCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!strikeDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // ---- The follow camera leaves a corpse where it fell (FollowCamera.cs:132) ----
    // LEVEL7's start room, Kurt at (12, 100, 12) facing -x with a wall behind him: the
    // camera's clearance pushes a living Kurt away from it (tests/kurt_moves_test.sh's
    // "x 7.0, below 9"). The C# skips the clearance while he is Dead, so the camera does
    // not slide his corpse through the death beat; this pins that arm against the alive
    // control. No probe: the C# dump would be the same feet either way.
    if (spritesArg != nullptr) {
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, 7)) {
            printf("FAIL L7_dead_clearance: the level did not load\n");
            failures++;
        } else {
            ArenaSpace space;
            const std::vector<const Arena*>& arenas = levelData.arenas();
            for (size_t i = 0; i < arenas.size(); i++) {
                if (levelData.isReachable(arenas[i]->name())) {
                    space.add(arenas[i]);
                }
            }
            float movedX[2] = {0.0f, 0.0f};
            for (int dead = 0; dead < 2; dead++) {
                Player player;
                player.feet = Vec3{12.0f, 100.0f, 12.0f};
                player.yaw = 180.0f;
                player.setSpace(&space);
                player.setFrameCounts(kurtFrameCounts(*spritesArg));
                if (dead) {
                    player.setHealth(0);
                }
                Input input;
                // Let him land (and, with no health, die) without the camera.
                for (int step = 0; step < 60; step++) {
                    player.update(input, 1.0f / 60.0f);
                }
                const float startX = player.feet.x;
                FollowCamera follow;
                Camera camera;
                for (int step = 0; step < 90; step++) {
                    player.update(input, 1.0f / 60.0f);
                    follow.update(&camera, &space, player, 0.0f, 1.0f / 60.0f, 4.0f / 3.0f);
                }
                movedX[dead] = startX - player.feet.x;
                if (dead && player.state() != Kurt::Dead) {
                    movedX[dead] = -1.0f;
                }
            }
            if (movedX[0] > 1.0f && movedX[1] == 0.0f) {
                printf("PASS L7_dead_clearance: the wall behind pushes a living Kurt %.2f "
                       "away, and his corpse not at all\n", movedX[0]);
            } else {
                printf("FAIL L7_dead_clearance: alive moved %.4f, dead moved %.4f (want > 1 "
                       "and 0)\n", movedX[0], movedX[1]);
                failures++;
            }
        }
    }

    // ---- The ledges (package #12) ----
    // Kurt's ledge grab, hang and climb against the C# probe tests/scripts/ledge_probe
    // (see kurtLedgeDump for the columns): Kurt.Ledge.cs and ArenaSpace.Probes.cs's
    // FloorCrossing / Edge / Free. The ledges were found by the probe's `scan` (an edge
    // along +Y, so the grab leaves Kurt at a yaw of exactly 0, where the port's LUT facing
    // is MathF's). The schedules are the probe's arguments, byte for byte.
    {
        struct LedgeCase {
            const char* name;
            const char* path;
            int level;
            int steps;
            const char* schedule;
            const char* pass;
        };
        static const LedgeCase kLedgeCases[] = {
            {"L6_ledge_climb", "golden/L6_ledge_climb.txt", 6, 140,
             "tp:1:OLYM_3:-1649:239:-669.9:0,fwd:10-60,jump:12-30",
             "OLYM_3: a running jump at a 16-unit wall, the hands cross its top falling at step "
             "56, Kurt hangs 1 before the edge (K_HANG, speeds zeroed), climbs by the tables "
             "for 31 ticks and stands on the top at step 120"},
            {"L4_ledge_climb", "golden/L4_ledge_climb.txt", 4, 140,
             "tp:1:MEAT_3:285:4166:-547.9:0,fwd:10-60,jump:12-30,fire:30-60",
             "MEAT_3: the same grab on a 17-unit wall with the chain gun firing; the grab "
             "stops MULTIFIRE (StopFiring) and the gun stays off through the hang, the fire key "
             "still held (no UpdateFiring while climbing)"},
            {"L4_ledge_short", "golden/L4_ledge_short.txt", 4, 100,
             "tp:1:MEAT_3:293.5:4166:-547.9:0,fwd:10-60,jump:12-30",
             "MEAT_3 from 1.5 before the wall: the jump into it leaves the hands short of the "
             "top, no surface is crossed and Kurt falls back to the floor"},
            {"L6_ledge_letgo", "golden/L6_ledge_letgo.txt", 6, 120,
             "tp:1:OLYM_3:-1649:239:-669.9:0,fwd:10-48,jump:12-30",
             "OLYM_3's jump with the forward key released before the fall: the same arc to step "
             "55, but GrabLedge refuses without forward and Kurt falls back"},
        };
        for (size_t i = 0; i < sizeof(kLedgeCases) / sizeof(kLedgeCases[0]); i++) {
            const LedgeCase& c = kLedgeCases[i];
            std::string expected;
            if (!readText(c.path, &expected)) {
                printf("FAIL %s: no golden file %s\n", c.name, c.path);
                failures++;
                continue;
            }
            std::string got;
            std::string why;
            if (!kurtLedgeDump(c.level, c.steps, c.schedule, data, spritesArg, &got, &why)) {
                printf("FAIL %s: %s\n", c.name, why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS %s: %s\n", c.name, c.pass);
            } else {
                printf("FAIL %s: %s\n", c.name, firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }
    // ---- end of the ledges (package #12) ----

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

    // The two flashes and the death beat they bookend (Kurt.Combat.cs:203-206 and
    // 234-254): the red damage flash and opcode 215's white one fading to zero, the
    // hard landing's slam raising the red one to its clamp, the death transition zeroing
    // it, the skull rising to SkullEnd, the Died event on the step it passes, the corpse
    // frozen from the next one, and a white flash raised on that corpse standing to the
    // last line because a dead Kurt fades nothing. This is the golden that makes
    // hurtFlash_ and whiteFlash_ live state rather than write-only fields.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_flashes.txt", &expected)) {
            printf("FAIL L3_kurt_flashes: no golden file golden/L3_kurt_flashes.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 620, kHurtFlashes, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_flashes: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_flashes: both flashes fall by FlashFade and stop at zero, "
                       "the slam raises the red one to its clamp, the death transition zeroes "
                       "it, the skull rises to SkullEnd and the beat ends on the step Died "
                       "fires, and a white flash raised on the corpse stands to the end\n");
            } else {
                printf("FAIL L3_kurt_flashes: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Opcode 248, the only writer of a non-zero knock-down push in the oracle
    // (ScriptRuntime.cs:2826-2852): the object-frame push with its asymmetric formula, the
    // away-from-object push with its normalize and its zero-distance fallback, the lift
    // added to the vertical speed, the drain, and the two arms of the guard that refuses a
    // shove -- Kurt already on his back, and Kurt at zero health. vel= is the push itself.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_push.txt", &expected)) {
            printf("FAIL L3_kurt_push: no golden file golden/L3_kurt_push.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 360, kHurtPush, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_push: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_push: opcode 248 knocks Kurt down and shoves him, from the "
                       "object's frame or away from the object, the zero-distance fallback shoves "
                       "him along +X, the push drains and the guard refuses a second one while he "
                       "is down or dead\n");
            } else {
                printf("FAIL L3_kurt_push: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // Kurt's moves bank the view (CameraRoll.cs, original 0x573910): the walk writer,
    // the leveller, and the held step between them. Both sides PRESS KEYS -- the oracle
    // probe holds TurnLeft/TurnRight/Forward over the windows in the header line and
    // prints what Kurt.CameraRoll.Roll holds afterwards -- so this pins the whole path
    // from the input to the bank, which is what makes it able to see the two guards
    // (no walk key, or no turn key, and both keys at once) and the one-step hold that no
    // amount of arithmetic on the roll alone would show. The consumer is pinned
    // separately, in vm_tests' PASS CameraRollBanksTheFollowCamera.
    {
        std::string expected;
        if (!readText("golden/L3_kurt_camroll.txt", &expected)) {
            printf("FAIL L3_kurt_camroll: no golden file golden/L3_kurt_camroll.txt\n");
            failures++;
        } else {
            std::string got;
            std::string why;
            if (!kurtHurtDump(3, 200, kHurtRoll, data, spritesArg, &got, &why)) {
                printf("FAIL L3_kurt_camroll: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_kurt_camroll: turning while walking banks the view 0.125 degrees a "
                       "step up to +/-10, turning the other way jumps 2 degrees back towards level, "
                       "the held step delays the leveller by exactly one step, the bank falls by "
                       "clamp(0.35*|roll|,0.05,2.5) a tick to EXACTLY zero, and neither key alone "
                       "-- or both at once -- banks anything\n");
            } else {
                printf("FAIL L3_kurt_camroll: %s\n", firstDifference(got, expected).c_str());
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
                    runtime.option = 1;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The two XG spawn at z=127.0 and fall; the oracle lands them at
                    // 126.9644 and 126.9500 (their arena floors), which free-fall alone
                    // would overshoot (126.8933). Newest first since upstream d14ed0b5, so
                    // the list holds them in that order (golden/L3_t3.txt, 004 and 005).
                    int checked = 0;
                    const float expectedZ[2] = {126.9644f, 126.9500f};
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
                    runtime.option = 1;
                    for (int t = 0; t < 2; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // Two XGs are commanded to move; the oracle turns them to these yaws
                    // with move command 78 active. Newest first since upstream d14ed0b5,
                    // they are the second and third XG in the list (golden/L8_t2.txt, 003
                    // and 004); the first, 002, has not been commanded yet.
                    const float expectedYaw[2] = {84.0f, 84.5768f};
                    int xi = 0;
                    int seen = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end() && xi < 2; ++it) {
                        if (it->type_name != "XG" || seen++ == 0) {
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
                    runtime.option = 1;
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

    // The doorway crossings (golden/L4_crossing.txt, golden/L7_crossing.txt). Run
    // here as well as by `tick1 crossing`. Being inside this suite is NOT what keeps
    // the crossing from rotting — `tools/check.sh` does not run `make -C tests/scripts`
    // (measured: the suite is 7m27s warm, so it cannot), and review-1 of
    // run-20261110-230107 F2 said so about an earlier draft of this comment. What
    // keeps it from rotting is PARITY-SCRIPTS-CROSSING in tools/runner/
    // gates_core_parity.py, which runs `make -C tests/scripts test_crossing` from the
    // gate suite. This line is here so the full suite cannot pass while the crossing
    // fails; it adds one case and changes no existing case's expectation.
    failures += crossingCheck(data, spritesArg);
    failures += bubbleSecondHopCheck(data, spritesArg);

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

    // The audio SAMPLE PLANE: both PCM widths of Sound::fromPcm, MixVoice's linear
    // interpolation and DirectSound pan law, AddMusic's one-pole music low-pass, the
    // bus and master gains, and the master limiter, digested as bit patterns one line
    // per update. This is the only audio evidence this host can produce at all — there
    // is no sound device here, and the emulator's output cannot be heard or captured —
    // and the golden is the C# AudioDevice's own mix digested, not the port's output.
    {
        std::string expected;
        if (!readText("golden/L3_audio_mix.txt", &expected)) {
            printf("FAIL L3_audio_mix: no golden file golden/L3_audio_mix.txt\n");
            failures++;
        } else {
            std::string got, why;
            if (!audioMixDump(3, 40, data, &got, &why)) {
                printf("FAIL L3_audio_mix: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_audio_mix: both PCM widths decode, the voices interpolate and "
                       "pan, the music bus low-passes, the bus and master gains scale, the "
                       "limiter holds the mix at its ceiling, and Stop/StopAll take voices out "
                       "of the mix on the update the C# takes them out\n");
            } else {
                printf("FAIL L3_audio_mix: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    // The same sounds decoded: what the mix above is made of. A mix divergence that
    // this still passes is the mixer's, and one it fails is the decode's.
    {
        std::string expected;
        if (!readText("golden/L3_audio_meta.txt", &expected)) {
            printf("FAIL L3_audio_meta: no golden file golden/L3_audio_meta.txt\n");
            failures++;
        } else {
            std::string got, why;
            if (!audioMetaDump(3, data, &got, &why)) {
                printf("FAIL L3_audio_meta: %s\n", why.c_str());
                failures++;
            } else if (got == expected) {
                printf("PASS L3_audio_meta: the eight sounds' decoded length, rate, loop flag "
                       "and sample values are the oracle's, so a mix divergence below is the "
                       "mixer's and not the decode's\n");
            } else {
                printf("FAIL L3_audio_meta: %s\n", firstDifference(got, expected).c_str());
                failures++;
            }
        }
    }

    if (failures == 0) {
        printf("\nAll cases match the oracle.\n");
        return 0;
    }
    printf("\n%d case(s) differ.\n", failures);
    return 1;
}
