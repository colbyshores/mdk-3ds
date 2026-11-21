// ============================================================================
// main.cpp — the Linux frontend.
//
// It finds the data, loads level 3, builds the arena view, and drives Kurt and
// the follow camera (input, movement, collision, vertical physics, camera pitch
// and clearance) every frame. The game logic is live on Linux only for now: the
// 3DS frontend (mdk_3ds/src/main_3ds.cpp) still frames the arena with the
// standstill camera until its input layer lands, so the two frontends are
// deliberately at different phases rather than mirroring each other.
// ============================================================================

#include <SDL3/SDL.h>

#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <climits>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>

#include "audio/audio_sdl.h"
#include "audio/level_music.h"
#include "audio/sound_bank.h"
#include "formats/cmi.h"
#include "render/render.h"
#include "render/camera_builder.h"
#include "game/enhanced_config.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "game/mod_catalog.h"
#include "game/mod_images.h"
#include "game/mod_models.h"
#include "game/level_view.h"
#include "game/cutscene_camera.h"
#include "game/strike_view.h"
#include "game/follow_camera.h"
#include "fall/fall_sim.h"
#include "game/game_flow.h"
#include "game/game_settings.h"
#include "game/cheats.h"
#include "game/pause_menu.h"
#include "game/screen_view.h"
#include "game/font_view.h"
#include "game/hud_view.h"
#include "game/kurt_sprite.h"
#include "game/effects_draw.h"
#include "game/effects_view.h"
#include "game/object_draw.h"
#include "game/object_view.h"
#include "game/player.h"
#include "game/sniper_view.h"
#include "game/input.h"
#include "game/arena_space.h"
#include "game/arena_views.h"
#include "game/sky_modes.h"
#include "game/walk_script.h"
#include "formats/mdk_data.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/palette.h"
#include "platform/filesystem_posix.h"
#include "platform/window_sdl.h"
#include "render/material_resolver.h"
#include "render/surface_cache.h"
#include "scripts/script_runtime.h"
#include "game/save_games.h"
#include "game/save_prompt.h"
#include "game/viewer_saves.h"
#include "stream/stream_screen.h"
#include "util/math_lut.h"
#include "menu_screens.h"

namespace {

// A fresh 32-bit seed for an ordinary run. The C# uses its modern unseeded
// Random; the C++ runtime currently exposes the oracle-compatible seeded
// legacy generator, so each construction gets fresh entropy instead.
int32_t freshScriptSeed() {
    std::random_device source;
    const uint32_t bits = static_cast<uint32_t>(source());
    int32_t seed = 0;
    std::memcpy(&seed, &bits, sizeof(seed));
    return seed;
}

// Remove a picture that is already sitting at the path this run was told to
// write, so that a capture which fails cannot leave it there.
//
// `--capture` names a file, and the run only ever REPORTED a capture it could
// not write (batch 171): the exit code says "no picture from this run", and it
// does not remove the picture the PREVIOUS run left at that path. A harness that
// keys on the file -- "is the picture there yet?" -- then commits another run's
// bytes as this build's frame, which is the exact hazard review-2 F1 named and
// batch 171's record repeats verbatim. Reporting the failure and clearing the
// destination are two different obligations and only one of them was met.
//
// So the destination is cleared when the run COMMITS to honouring the request,
// before anything that can fail: renderer creation, level load, the readback and
// the write all happen after this point, and a failure in any of them now leaves
// no picture at the path instead of a stale one. This is the rule the oracle-side
// capture harness already applies to its own output
// (`tests/frames/capture_oracle.sh` runs `rm -f "$OUT"` before it runs the
// oracle at all), and it belongs to the frontend that made the request rather
// than to whichever backend is asked to fill it.
//
// Only a REGULAR file is removed, and only after `stat` followed the link. A
// directory, a fifo or a device node at that path is left exactly where it is:
// `--capture=/dev/full` is how this host forces a write failure, and a cleanup
// written with `std::remove` would turn that into a way to delete a device node.
// An invocation that the argument parser refuses never reaches this line, so a
// refusal cannot touch anyone's file.
//
// A removal that fails is reported and the run continues: the write may still
// succeed, and if it does not, the exit code already says the run has no picture.
bool clearCaptureDestination(const char* path) {
    struct stat info;
    if (::stat(path, &info) != 0) {
        return true;   // nothing there, or nothing reachable: nothing to clear
    }
    if (!S_ISREG(info.st_mode)) {
        return true;   // not a picture this run would overwrite: leave it alone
    }
    if (::unlink(path) != 0) {
        std::fprintf(stderr,
                     "[main] --capture=%s already holds a file that could not be "
                     "removed (%s); if this run's capture fails, those bytes are "
                     "still there\n", path, std::strerror(errno));
        return false;
    }
    return true;
}

// An option value that names a count this frontend can act on: a non-empty run
// of decimal digits, nothing after them, and a value in [1, INT_MAX].
//
// This exists because `atoi` answers 0 for "abc", for "0" and for "" alike, and
// every reader of these counts takes 0 to mean "no limit" or "never":
// `maxFrames > 0` gates the frame bound and `captureFrame > 0` gates the
// capture. So a value that failed to parse did not fail — it silently became the
// OPPOSITE of what was asked. `--frames abc` ran unbounded, and
// `--capture-frame=abc` exited 0 having written no picture, which is a capture
// harness reporting a frame it never took.
//
// The leading-digit test is not decoration. `strtol` skips leading space and
// accepts a sign, so " 5", "+5" and "-5" all parse without it, and a contract
// that says "a positive whole number" should mean exactly that.
// ERANGE is checked and not folded into the range test, because on a target whose
// `long` is 32 bits (any -m32 or ILP32 build) LONG_MAX *is* INT_MAX: the value
// clamps to INT_MAX, `value > INT_MAX` cannot fire, and an out-of-range count is
// silently accepted as the largest legal one. That is the fail-open this
// predicate exists to remove, reappearing through the type instead of the parse.
// On this x86-64 host the two tests agree, so the ERANGE arm is a portability
// guard and is NOT observable here -- stated rather than implied by a green run.
bool positiveCount(const char* name, const char* text, int* out) {
    char* end = nullptr;
    const bool digits = text != nullptr && *text >= '0' && *text <= '9';
    errno = 0;
    const long value = digits ? std::strtol(text, &end, 10) : 0;
    if (!digits || end == text || *end != '\0' || errno == ERANGE || value < 1 ||
        value > INT_MAX) {
        std::fprintf(stderr, "[main] %s must be a positive whole number: %s\n",
                     name, text != nullptr ? text : "");
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

// The mods' replacement source for the shared surface cache: the level's
// ModImages plus the image being handed over (the backend copies its bytes
// inside the call, so the one buffer serves every lookup in turn).
struct ModReplacementSource {
    mdk::ModImages* mods = nullptr;
    mdk::HdImage image;
};

bool modReplacementLookup(void* user, const mdk::Surface& surface,
                          const mdk::formats::Palette* palette,
                          mdk::ColourReplacement* out) {
    ModReplacementSource* source = static_cast<ModReplacementSource*>(user);
    if (!source->mods || !surface.texture || !palette) {
        return false;
    }
    source->image = mdk::HdImage();
    const std::string& name =
        surface.name.empty() ? surface.texture->name() : surface.name;
    if (!source->mods->texture(name, *surface.texture, *palette, &source->image)) {
        return false;
    }
    out->width = source->image.width;
    out->height = source->image.height;
    out->frames = source->image.frames;
    out->rgba = source->image.rgba.data();
    return true;
}

// THE LEVEL CYCLE'S MEMORY (package 2, the next-level transition): what a level
// leaves behind once it is torn down. `heap` is the C heap's bytes in use
// (mallinfo2's uordblks: freed memory counts as free even when glibc keeps it),
// `fds` the open file descriptors (the archives, the Vulkan device's handles),
// and `logged` the renderer's creation log (mdk_render_mark), which a level's
// mdk_render_release pops back to the level's mark. A level-to-level run prints
// one line before the first level and one after each teardown, and a leak is a
// figure that climbs along them.
struct MemorySample {
    long long heap = 0;
    int fds = 0;
    int logged = 0;
};

MemorySample memorySample(MdkRenderer renderer) {
    MemorySample sample;
    const struct mallinfo2 info = mallinfo2();
    sample.heap = (long long)info.uordblks;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (const struct dirent* entry = readdir(dir)) {
            if (entry->d_name[0] != '.') {
                sample.fds++;
            }
        }
        closedir(dir);
        sample.fds--;   // the directory stream's own descriptor
    }
    sample.logged = mdk_render_mark(renderer).textures;
    return sample;
}

// The empty frames between two levels (see the level loop's teardown): more than the
// Vulkan backend's frames in flight, so every retired resource reaches its boundary.
constexpr int kBetweenLevelFrames = 4;

void printMemory(const char* tag, int level, const MemorySample& sample) {
    std::printf("[main] mem[%s]: level %d heap=%lld fds=%d renderer-log=%d\n", tag, level,
                sample.heap, sample.fds, sample.logged);
}

// The stream screen's report lines (C# StreamScreen's Console.WriteLine), printed
// as the C# prints them so the two runs' lines compare as text.
void printStreamLine(void*, const char* line) {
    std::printf("%s\n", line);
}

// THE STREAM SCREEN (package #18b; stream/stream_screen.h). Everything a stream
// needs from this frontend: the window's input, the frame, the run's frame count
// with its --frames bound and --capture frame, and the sound's output.
struct StreamFrontend {
    MdkRenderer renderer = nullptr;
    mdk::Window* window = nullptr;
    bool headless = false;
    bool fixedStep = false;
    float step = 1.0f / 60.0f;
    const mdk::WalkScript* walk = nullptr;   // --walk steers the stream too
    uint32_t* frame = nullptr;
    int maxFrames = -1;
    int captureFrame = 0;
    const std::string* capturePath = nullptr;
    // The settings' difficulty (C# StreamScreen: ui.Settings.Difficulty).
    mdk::Kurt::Difficulty difficulty = mdk::Kurt::Normal;
};

// One stream, from its first frame to its Event (C# Game.Run's loop over a
// StreamScreen). Returns Event::Quit when the RUN ends inside it -- the window
// closed, --frames reached, the --capture frame taken -- with `exitReason` set;
// otherwise the screen's Event and, in `health`, Kurt's health at its end (the
// C#'s StreamScreen.Health, which AfterStream reads).
mdk::flow::Event runStreamScreen(const StreamFrontend& f, const mdk::formats::MdkData& data,
                                 int32_t level, int32_t health, uint32_t seed, int32_t* endHealth,
                                 const char** exitReason) {
    *endHealth = health;
    // The screen and its StreamRun on the heap (stream_screen.h: MEMORY).
    std::unique_ptr<mdk::stream::StreamScreen> screen(new mdk::stream::StreamScreen());
    mdk::stream::StreamScreen::Setup setup;
    setup.renderer = f.renderer;
    setup.data = &data;
    setup.level = level;
    setup.health = health;
    setup.difficulty = f.difficulty;
    setup.seed = seed;
    setup.log = printStreamLine;
    std::printf("[main] stream: after level %d, health %d, seed %u\n", (int)level, (int)health,
                (unsigned)seed);
    if (!screen->open(setup)) {
        std::printf("[main] stream: could not open; it ends at once with health %d\n", (int)health);
    }
    // The sound to the speakers when there is a window (a headless test run stays
    // silent, as the level's mix does here).
    if (!f.headless && mdk::audioIsOpen()) {
        screen->mixer().device().setSink(mdk::audioSink, nullptr, mdk::audioQueued);
    }

    mdk::Input input;
    bool escapeWasDown = false;
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = f.fixedStep ? f.step : std::chrono::duration<float>(now - last).count();
        last = now;
        if (!f.headless && !mdk::windowPump(f.window, &input)) {
            *exitReason = "window closed";
            return mdk::flow::Event::Quit;
        }
        if (f.walk != nullptr) {
            mdk::applyWalk(*f.walk, *f.frame, &input);
        }
        // C# input.WasPressed(MenuKey.Back): Esc, on its press.
        const bool escapeDown = input.isDown(mdk::kKeyEscape);
        const bool back = escapeDown && !escapeWasDown;
        escapeWasDown = escapeDown;

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *f.frame;
        params.clear_colour = 0xFF000000u;   // C# Ui.Black
        params.present = true;
        const bool captureThisFrame =
            f.captureFrame > 0 && static_cast<int>(*f.frame) + 1 == f.captureFrame;
        params.screenshot_path = captureThisFrame && f.capturePath && !f.capturePath->empty()
                                     ? f.capturePath->c_str()
                                     : nullptr;
        // The screen's update first (it sets the camera the frame is drawn with),
        // then the frame.
        const mdk::flow::Event next = screen->update(elapsed, mdk::stream::steeringOf(input), back);
        mdk_render_begin_frame(f.renderer, &params);
        screen->draw();
        // C# Game.Run: the device after the screen, with the same elapsed.
        screen->mixer().update(elapsed);
        screen->mixer().device().update(elapsed);
        mdk_render_end_frame(f.renderer);
        (*f.frame)++;
        if ((*f.frame % 600u) == 0u) {
            char status[128];
            screen->status(status, (int32_t)sizeof(status));
            std::printf("[main] stream: frame %u: %s\n", *f.frame, status);
        }

        if (captureThisFrame) {
            *exitReason = "capture";
            return mdk::flow::Event::Quit;
        }
        if (f.maxFrames > 0 && (int)*f.frame >= f.maxFrames) {
            *exitReason = "frame budget reached";
            return mdk::flow::Event::Quit;
        }
        if (next != mdk::flow::Event::None) {
            *endHealth = screen->health();
            return next;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // --headless: render with no window. The golden-frame harness needs it,
    // and it is the only way to run where the display server cannot present
    // a Vulkan swapchain.
    bool headless = false;
    // --mute: no sound (the C#'s --mute, Program.cs:186 -> SoundMode.Muted ->
    // AudioDevice(Output.Muted)). --headless implies it: a headless run is a test,
    // and a real sink's queue drains at the wall clock's pace, so the frames each
    // update mixes -- and with them which voices are still playing when a script
    // asks -- would depend on the host. Muted, the device mixes `delta`'s frames,
    // the golden's accounting (audio/sound_device.h, the `owed` branch).
    bool mute = false;
    int maxFrames = -1;
    bool scriptSeedSet = false;
    int32_t scriptSeed = 0;
    std::string capturePath;
    int captureFrame = 0;
    int hiddenWidth = 1280;
    // --true-aspect: project at the output size's own width/height instead of
    // the original's fixed 4:3. The 3DS shows its panel's true 5:3 (owner
    // decision 2026-11-17), and a frame-exact 3DS-vs-Linux comparison
    // (mdk_3ds/tools/compare_linux.sh) needs Linux at that same aspect. Off by
    // default: the Linux game keeps the original's 4:3.
    bool trueAspect = false;
    int hiddenHeight = 960;
    int levelNumber = 3;
    // --level names the first level: the run starts in it (the C#'s Start.Level).
    // Without it the run starts at the main menu (the C#'s Start.Menu, package
    // #20a), so every test and harness run that passes --level is unchanged.
    bool levelGiven = false;
    // --fall=N: the fall before level N, then the level (the C#'s --fall, Start.Fall;
    // package #17b). --fall-lean draws it the 3DS's way (fall/fall_view.h).
    bool fallGiven = false;
    bool fallLean = false;
    // --skip-falls: every fall skipped the C#'s Esc way before its first frame (a
    // test aid, as --event-every-level: the level chain and the menu tests time
    // their frames without the 38-second fall).
    bool skipFalls = false;
    // --fall-chase: the fall steers itself onto the first pickup under its chute
    // (the fall probe's `chase` schedule; a test aid, so a headless run carries
    // pickups into the level).
    bool fallChase = false;
    // --press: menu keys at game times (the C#'s TestPresses, game/menu_presses.h).
    mdk::MenuPresses presses;
    // --stream=N / --health=N (package #18b): start in the stream after level N.
    bool startStream = false;
    bool hasStreamHealth = false;
    int streamHealthOption = 0;
    bool fixedStep = false;
    bool hasStartOverride = false;
    mdk::Vec3 startOverride{};
    bool hasYawOverride = false;
    float yawOverride = 0.0f;
    mdk::StereoSettings stereo;
    bool enhancedLook = false;
    float gammaValue = 1.0f;
    std::vector<std::string> onlyMods;
    bool onlyModsSet = false;
    // --walk=<script>: the hardware harness's scripted walk (game/walk_script.h,
    // the 3DS's `make AUTOQUIT=.. WALK=..`), here to FIND and VERIFY a route on
    // the shared simulation: with --fixed-step frame f is step f on both
    // frontends. A walk also logs Kurt's arena on every change.
    bool hasWalk = false;
    mdk::WalkScript walk;
    // --event=N: the C#'s test option (Program.cs:53, Viewer.RunTests at Viewer.cs:664-680):
    // special_event N for the arena controller of Kurt's arena after 1 second of game time.
    // 50 and below end the level (scripts/end_level.h); it is how a run reaches the end
    // of a level without walking to its trigger.
    bool hasEvent = false;
    int32_t eventValue = 0;
    // --event-every-level: the event fires one second into EVERY level, not only the
    // first. The C# hands the command line's test options to the first level alone
    // (Game.cs:487, `_firstLevel ? _options.Level : new ViewerOptions(...)`), so
    // without this a chained run plays the second level for real; with it a
    // headless run chains level to level (3 -> 4 -> 8 -> 5), which is how the
    // teardown and reload are tested (tests/scripts/level_chain_test.sh). A test
    // aid, not the C#'s.
    bool eventEveryLevel = false;
    // --replay-level: when a level ends and the flow names the next one, load the
    // level that ended AGAIN instead. A leak test, not the C#'s flow: two teardowns
    // of the same level must leave the same memory behind, which two different
    // levels cannot show (tests/scripts/level_chain_test.sh).
    bool replayLevel = false;
    // THE SAVES (package #19). --save-dir=DIR: where the saves go (the C#'s
    // `<user folder>/saves`); without it $MDK_USER_DIR/saves (the C#'s override), else
    // the C#'s own Linux user folder, ~/.local/share/mdk-sdl/saves ($XDG_DATA_HOME
    // first, as .NET's LocalApplicationData), so a C# install and this one share them.
    // --load=NAME starts from a save; --save=NAME writes a light save of the first
    // level as it starts (the C#'s GameOptions.Load / Save).
    std::string saveDir;
    std::string loadName;
    std::string saveName;
    // THE CONSOLE'S CHEATS (gap package #21). --god and --noclip are the C# console's
    // `god` and `noclip` (DevTools/ConsoleCommands.cs:37-38), which DevSession holds for
    // the session and the Viewer hands to every level's Kurt (Viewer.cs:286-289); this
    // port has no console, so the command line is where they are turned on. The C#'s
    // own test spelling is `--console="god;noclip"`.
    bool godCheat = false;
    bool noclipCheat = false;
    // --type=TEXT@SECONDS: TEXT typed in the first level at that much game time, as if
    // from the keyboard (the cheats: TOOSCARYFORME, SEETHEWHOLEGAME). A test aid, not
    // the C#'s: a headless run has no keyboard.
    bool hasTypeTest = false;
    std::string typeTestText;
    float typeTestTime = 0.0f;
    // --die: Kurt is hurt to death at the test start (the C#'s ViewerOptions.Die,
    // Viewer.RunTests with FatalDamage 1000), the first level only -- how a death's
    // LASTGAME and the level's reload are run without a console.
    bool dieTest = false;
    // --arena=NAME (with --at): Kurt starts in that arena the way a script's teleport
    // puts him there (ScriptRuntime::teleportKurt; the C# console's map,
    // Viewer.EnterStartArena), so an arena only a teleport reaches (LEVEL3's HMO_7)
    // is entered and solid. --at alone only moves his feet.
    std::string startArenaOverride;
    // THE STATISTICS (package #20b), the C#'s test options (Program.cs:22-23,
    // Game.cs:405-420): --stats=N starts at the screens after level N, --phase=1-4
    // at one of their pages (1 Score-O-matic, 2 intermission, 3 briefing, 4
    // debriefing), --counts=shots,hits,sniper,sniper hits,kills,enemies,heads and
    // --towns=bits (the town flags' bits 31-29) are what they show. --bloodyes /
    // --nobloodno: gore on or off instead of the settings' (the heads' row).
    // --skip-statistics: a level's end takes the C#'s skip of them (GameFlow::
    // skipScreen; the level chain's tests), a test aid, not the C#'s.
    bool statsGiven = false;
    int statsPhase = 0;
    std::vector<int32_t> statsCounts;
    bool townsGiven = false;
    int32_t towns = 0;
    int goreOverride = -1;
    bool skipStatistics = false;

    // The C#'s sniper tests (Program.cs:56-58, SniperTest.cs): --sniper[=zoom[,pitch]]
    // enters sniper mode once Kurt stands, at that zoom and pitch; --zoom=seconds holds
    // "zoom in" that long from then; --sniper-fire fires one round once the clip is
    // loaded (1 s after entering, for 0.1 s). With --fixed-step and --capture-frame
    // they frame the scope as the oracle's --sniper and --screenshot do.
    bool sniperTest = false;
    float sniperTestZoom = 1.0f;
    float sniperTestPitch = 0.0f;
    float sniperTestHold = 0.0f;
    bool sniperTestFire = false;

    auto optionValue = [](const char* arg, const char* name) -> const char* {
        const size_t length = std::strlen(name);
        if (std::strncmp(arg, name, length) != 0 || arg[length] != '=') {
            return nullptr;
        }
        return arg + length + 1;
    };

    for (int i = 1; i < argc; i++) {
        const char* value = nullptr;
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--mute") == 0) {
            mute = true;
        } else if (std::strcmp(argv[i], "--fixed-step") == 0) {
            fixedStep = true;
        } else if (std::strcmp(argv[i], "--true-aspect") == 0) {
            trueAspect = true;
        } else if ((value = optionValue(argv[i], "--capture")) != nullptr) {
            if (*value == '\0') {
                std::fprintf(stderr, "[main] --capture must name a file to write\n");
                return 1;
            }
            capturePath = value;
            if (captureFrame == 0) {
                captureFrame = 1;
            }
        } else if ((value = optionValue(argv[i], "--capture-frame")) != nullptr) {
            if (!positiveCount("--capture-frame", value, &captureFrame)) {
                return 1;
            }
        } else if ((value = optionValue(argv[i], "--size")) != nullptr) {
            int w = 0;
            int h = 0;
            if (std::sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                std::fprintf(stderr, "[main] invalid --size: %s (want WxH)\n", value);
                return 1;
            }
            hiddenWidth = w;
            hiddenHeight = h;
        } else if ((value = optionValue(argv[i], "--stream")) != nullptr) {
            // The C#'s --stream=N (Program.cs:28): the stream after level N, started
            // directly, its tube the WatcomRandom's default seed's.
            if (!positiveCount("--stream", value, &levelNumber)) {
                return 1;
            }
            if (levelNumber < 3 || levelNumber > 8) {
                std::fprintf(stderr, "[main] --stream must be 3-8\n");
                return 1;
            }
            startStream = true;
        } else if ((value = optionValue(argv[i], "--health")) != nullptr) {
            // The C#'s --health=N: Kurt's health in a stream started directly.
            if (!positiveCount("--health", value, &streamHealthOption)) {
                return 1;
            }
            hasStreamHealth = true;
        } else if ((value = optionValue(argv[i], "--level")) != nullptr) {
            // Parse first, then bound. Before this, atoi turned "abc" into 0 and
            // the range check rejected it with a message about the range for a
            // value that was never a number.
            if (!positiveCount("--level", value, &levelNumber)) {
                return 1;
            }
            if (levelNumber < 3 || levelNumber > 8) {
                std::fprintf(stderr, "[main] --level must be 3-8\n");
                return 1;
            }
            levelGiven = true;
        } else if ((value = optionValue(argv[i], "--fall")) != nullptr) {
            if (!positiveCount("--fall", value, &levelNumber)) {
                return 1;
            }
            if (levelNumber < 3 || levelNumber > 8) {
                std::fprintf(stderr, "[main] --fall must be 3-8\n");
                return 1;
            }
            fallGiven = true;
        } else if (std::strcmp(argv[i], "--fall-lean") == 0) {
            fallLean = true;
        } else if (std::strcmp(argv[i], "--skip-falls") == 0) {
            skipFalls = true;
        } else if (std::strcmp(argv[i], "--fall-chase") == 0) {
            fallChase = true;
        } else if ((value = optionValue(argv[i], "--stats")) != nullptr) {
            if (!positiveCount("--stats", value, &levelNumber)) {
                return 1;
            }
            if (levelNumber < 3 || levelNumber > 8) {
                std::fprintf(stderr, "[main] --stats must be 3-8\n");
                return 1;
            }
            statsGiven = true;
        } else if ((value = optionValue(argv[i], "--phase")) != nullptr) {
            if (!positiveCount("--phase", value, &statsPhase) || statsPhase > 4) {
                std::fprintf(stderr, "[main] --phase must be 1-4\n");
                return 1;
            }
        } else if ((value = optionValue(argv[i], "--counts")) != nullptr) {
            // C# `counts.Split(',').Select(int.Parse)`.
            statsCounts.clear();
            const char* at = value;
            for (;;) {
                char* end = nullptr;
                errno = 0;
                const long count = std::strtol(at, &end, 10);
                if (end == at || errno == ERANGE || count < INT_MIN || count > INT_MAX ||
                    (*end != ',' && *end != '\0')) {
                    std::fprintf(stderr, "[main] invalid --counts: %s\n", value);
                    return 1;
                }
                statsCounts.push_back((int32_t)count);
                if (*end == '\0') {
                    break;
                }
                at = end + 1;
            }
        } else if ((value = optionValue(argv[i], "--towns")) != nullptr) {
            char* end = nullptr;
            errno = 0;
            const long bits = std::strtol(value, &end, 10);
            if (end == value || *end != '\0' || errno == ERANGE || bits < 0 || bits > 7) {
                std::fprintf(stderr, "[main] --towns must be 0-7\n");
                return 1;
            }
            towns = (int32_t)bits;
            townsGiven = true;
        } else if (std::strcmp(argv[i], "--bloodyes") == 0) {
            goreOverride = 1;
        } else if (std::strcmp(argv[i], "--nobloodno") == 0) {
            goreOverride = 0;
        } else if (std::strcmp(argv[i], "--skip-statistics") == 0) {
            skipStatistics = true;
        } else if ((value = optionValue(argv[i], "--press")) != nullptr) {
            std::string error;
            if (!presses.parse(value, &error)) {
                std::fprintf(stderr, "[main] invalid --press: %s\n", error.c_str());
                return 1;
            }
        } else if ((value = optionValue(argv[i], "--at")) != nullptr) {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            float yaw = 0.0f;
            const int fields = std::sscanf(value, "%f,%f,%f,%f", &x, &y, &z, &yaw);
            if (fields < 3) {
                std::fprintf(stderr, "[main] invalid --at: %s (want x,y,z[,yaw])\n", value);
                return 1;
            }
            startOverride = mdk::Vec3{x, y, z};
            hasStartOverride = true;
            hasYawOverride = fields == 4;
            yawOverride = yaw;
        } else if ((value = optionValue(argv[i], "--walk")) != nullptr) {
            char error[256];
            if (!mdk::parseWalkScript(value, &walk, error, sizeof(error))) {
                std::fprintf(stderr, "[main] invalid --walk: %s\n", error);
                return 1;
            }
            hasWalk = true;
        } else if ((value = optionValue(argv[i], "--event")) != nullptr) {
            // The opcode's operand is a byte (special_event's U8), so 0-255.
            char* end = nullptr;
            const bool digits = *value >= '0' && *value <= '9';
            const long parsed = digits ? std::strtol(value, &end, 10) : -1;
            if (!digits || end == value || *end != '\0' || parsed < 0 || parsed > 255) {
                std::fprintf(stderr, "[main] --event must be a whole number 0-255: %s\n", value);
                return 1;
            }
            hasEvent = true;
            eventValue = (int32_t)parsed;
        } else if (std::strcmp(argv[i], "--event-every-level") == 0) {
            eventEveryLevel = true;
        } else if (std::strcmp(argv[i], "--sniper") == 0) {
            sniperTest = true;
        } else if ((value = optionValue(argv[i], "--sniper")) != nullptr) {
            // SniperTest.Setup: "zoom[,pitch]", both optional.
            float zoom = 1.0f;
            float pitch = 0.0f;
            const int fields = std::sscanf(value, "%f,%f", &zoom, &pitch);
            if (fields < 1 || !(zoom > 0.0f) || zoom > 1.0f) {
                std::fprintf(stderr, "[main] invalid --sniper: %s (want zoom[,pitch], zoom 0-1)\n",
                             value);
                return 1;
            }
            sniperTest = true;
            sniperTestZoom = zoom;
            sniperTestPitch = fields > 1 ? pitch : 0.0f;
        } else if ((value = optionValue(argv[i], "--zoom")) != nullptr) {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || *end != '\0' || parsed < 0.0f) {
                std::fprintf(stderr, "[main] invalid --zoom: %s (want seconds)\n", value);
                return 1;
            }
            sniperTestHold = parsed;
        } else if (std::strcmp(argv[i], "--sniper-fire") == 0) {
            sniperTestFire = true;
        } else if (std::strcmp(argv[i], "--replay-level") == 0) {
            replayLevel = true;
        } else if (std::strcmp(argv[i], "--enhanced") == 0) {
            enhancedLook = true;
        } else if (std::strcmp(argv[i], "--original") == 0) {
            enhancedLook = false;
        } else if ((value = optionValue(argv[i], "--mod")) != nullptr) {
            // --mod=a,b: only these mods (folders of mods/) on, for this
            // run (the C# GameOptions.Mods; tests). Empty entries dropped.
            onlyModsSet = true;
            std::string list = value;
            size_t at = 0;
            while (at <= list.size()) {
                const size_t comma = list.find(',', at);
                const std::string one =
                    list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) {
                    onlyMods.push_back(one);
                }
                if (comma == std::string::npos) {
                    break;
                }
                at = comma + 1;
            }
        } else if ((value = optionValue(argv[i], "--gamma")) != nullptr) {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || *end != '\0' || parsed < 0.5f || parsed > 2.0f) {
                std::fprintf(stderr, "[main] invalid --gamma: %s (want 0.5-2)\n", value);
                return 1;
            }
            gammaValue = parsed;
        } else if ((value = optionValue(argv[i], "--stereo")) != nullptr) {
            // The oracle's --stereo spellings (StereoModes.TryParse): off,
            // sbs, sbsfull, crossview, int, intr, tab, tabr, vr. An unknown
            // mode is a refusal, never a silent mono run -- a harness that
            // asked for a stereo frame and got a mono one would measure the
            // wrong picture and call it a pass.
            bool parsed = true;
            if (std::strcmp(value, "off") == 0) {
                stereo.mode = mdk::StereoMode::Off;
            } else if (std::strcmp(value, "sbs") == 0) {
                stereo.mode = mdk::StereoMode::Sbs;
            } else if (std::strcmp(value, "sbsfull") == 0) {
                stereo.mode = mdk::StereoMode::SbsFull;
            } else if (std::strcmp(value, "crossview") == 0 || std::strcmp(value, "cross") == 0) {
                stereo.mode = mdk::StereoMode::CrossView;
            } else if (std::strcmp(value, "int") == 0) {
                stereo.mode = mdk::StereoMode::Interlaced;
            } else if (std::strcmp(value, "intr") == 0) {
                stereo.mode = mdk::StereoMode::InterlacedReversed;
            } else if (std::strcmp(value, "tab") == 0) {
                stereo.mode = mdk::StereoMode::Tab;
            } else if (std::strcmp(value, "tabr") == 0) {
                stereo.mode = mdk::StereoMode::TabReversed;
            } else if (std::strcmp(value, "vr") == 0) {
                stereo.mode = mdk::StereoMode::Vr;
            } else {
                parsed = false;
            }
            if (!parsed) {
                std::fprintf(stderr, "[main] --stereo: off, sbs, sbsfull, crossview, int, intr, tab, tabr or vr\n");
                return 1;
            }
        } else if ((value = optionValue(argv[i], "--stereo-separation")) != nullptr) {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || *end != '\0') {
                std::fprintf(stderr, "[main] invalid --stereo-separation: %s\n", value);
                return 1;
            }
            stereo.separation = parsed;
        } else if ((value = optionValue(argv[i], "--stereo-convergence")) != nullptr) {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || *end != '\0') {
                std::fprintf(stderr, "[main] invalid --stereo-convergence: %s\n", value);
                return 1;
            }
            stereo.convergence = parsed;
        } else if ((value = optionValue(argv[i], "--arena")) != nullptr) {
            startArenaOverride = value;
        } else if (std::strcmp(argv[i], "--die") == 0) {
            dieTest = true;
        } else if (std::strcmp(argv[i], "--god") == 0) {
            godCheat = true;
        } else if (std::strcmp(argv[i], "--noclip") == 0) {
            noclipCheat = true;
        } else if ((value = optionValue(argv[i], "--type")) != nullptr) {
            const char* at = std::strrchr(value, '@');
            char* end = nullptr;
            const float seconds = at ? std::strtof(at + 1, &end) : -1.0f;
            if (!at || at == value || end == at + 1 || *end != '\0' || seconds < 0.0f) {
                std::fprintf(stderr, "[main] invalid --type: %s (want TEXT@SECONDS)\n", value);
                return 1;
            }
            hasTypeTest = true;
            typeTestText.assign(value, (size_t)(at - value));
            typeTestTime = seconds;
        } else if ((value = optionValue(argv[i], "--save-dir")) != nullptr) {
            saveDir = value;
        } else if ((value = optionValue(argv[i], "--load")) != nullptr) {
            loadName = value;
        } else if ((value = optionValue(argv[i], "--save")) != nullptr) {
            saveName = value;
        } else if (std::strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[main] --frames needs a value\n");
                return 1;
            }
            if (!positiveCount("--frames", argv[++i], &maxFrames)) {
                return 1;
            }
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[main] --seed needs a value\n");
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(argv[++i], &end, 10);
            // ERANGE for the same reason `positiveCount` needs it: where `long` is
            // 32 bits the value clamps to LONG_MAX == INT_MAX and the bound test
            // below cannot fire, so an out-of-range seed is accepted as INT_MAX.
            if (end == argv[i] || *end != '\0' || errno == ERANGE ||
                value < INT_MIN || value > INT_MAX) {
                std::fprintf(stderr, "[main] invalid --seed: %s\n", argv[i]);
                return 1;
            }
            scriptSeed = static_cast<int32_t>(value);
            scriptSeedSet = true;
        } else {
            // An argument the chain does not know used to fall through and the run
            // continued on its defaults, which is how a "levels 1-7 sweep" made by
            // passing `--level 3` with a space measured level 3 seven times and
            // printed identical draw counts. A harness that reports having run
            // what it did not run is worse than one that fails.
            std::fprintf(stderr, "[main] unrecognized argument: %s\n", argv[i]);
            std::fprintf(stderr,
                         "[main] options take their value with an equals "
                         "(--level=3, --size=320x240, --capture=frame.bmp) except "
                         "--frames and --seed, which take it as a separate word "
                         "(--frames 3, --seed 1)\n");
            return 1;
        }
    }

    // --true-aspect projects at --size's own ratio, which is the picture only when
    // the hidden target IS the picture. A window is always 1280x960 (4:3) and the
    // canvas follows the window, so a 5:3 projection there would be stretched.
    if (eventEveryLevel && !hasEvent) {
        std::fprintf(stderr, "[main] --event-every-level needs --event=N: there is no event to repeat\n");
        return 1;
    }
    if (trueAspect && !headless) {
        std::fprintf(stderr, "[main] --true-aspect needs --headless (the window is 4:3)\n");
        return 1;
    }

    // A capture the run can never reach is a refusal, not a default.
    //
    // Both checks sit HERE rather than in their own option branch because the two
    // options may arrive in either order: `--capture-frame=99 --frames 2` and
    // `--frames 2 --capture-frame=99` are the same request, and a branch-local
    // check would only catch one of them.
    //
    // 1. A capture frame with no file to write. The loop breaks on the capture
    //    frame whether or not a path was given, so `--capture-frame=1` alone ran
    //    one frame, wrote nothing and exited 0.
    // 2. A capture frame past the frame bound. The bound breaks first, so
    //    `--capture=f.bmp --capture-frame=200 --frames 100` exits 0 having written
    //    no picture: a capture harness reporting a frame it never took.
    //
    // This is the same defect as an unparseable count, one step further along: the
    // value parsed, and nothing asked whether the run could still do what it says.
    // It also makes the loop's own `break` on `captureThisFrame` safe, because a
    // non-zero capture frame now always has a path behind it.
    if (captureFrame > 0 && capturePath.empty()) {
        std::fprintf(stderr,
                     "[main] --capture-frame must be used with --capture: no file was named\n");
        return 1;
    }
    if (captureFrame > 0 && maxFrames > 0 && captureFrame > maxFrames) {
        std::fprintf(stderr,
                     "[main] --capture-frame must be within --frames: frame %d is past "
                     "the %d-frame bound and would write no picture\n",
                     captureFrame, maxFrames);
        return 1;
    }

    // A non-zero capture frame now always has a path behind it, so this is the
    // whole of the request: one picture, at this path, or the run fails. Clearing
    // it HERE -- after every refusal and before the first thing that can fail -- is
    // what makes "the file is there" mean "THIS run put it there". See
    // `clearCaptureDestination`.
    if (captureFrame > 0) {
        clearCaptureDestination(capturePath.c_str());
    }

    // The C# Viewer's ordinary runtime uses an unseeded modern Random. The
    // C++ port currently has only the oracle-compatible seeded legacy
    // generator, so an ordinary run gets a fresh entropy seed and a test can
    // request a reproducible one with --seed.

    // The trig tables MUST be initialised before any fast* call. An
    // uninitialised table is all zeroes, so every trig call returns 0 --
    // which looks like a game that renders but never moves.
    mdk::mathLutInit();

    // The formats library reaches the filesystem through this seam, and the
    // case-insensitive path resolution needs it BEFORE any data file is
    // read. So it is installed first.
    mdk::formats::setFileSystem(mdk::platformFileSystem());

    mdk::Window window;
    if (!headless) {
        mdk::WindowDesc windowDesc;
        windowDesc.title = "MDK";
        windowDesc.width = 1280;
        windowDesc.height = 960;
        if (!mdk::windowCreate(&window, &windowDesc)) {
            return 1;
        }
    }

    // The Enhanced Mode toggles (SPEC.md 7.9): the same config.ini the
    // 3DS build reads from sdmc:/3ds/mdk/, here from $MDK_CONFIG_INI or
    // ./config.ini when one exists. Absent file: all on (the defaults).
    mdk::EnhancedToggles enhancedToggles;
    {
        const char* configPath = std::getenv("MDK_CONFIG_INI");
        if (!configPath || configPath[0] == '\0') {
            configPath = "config.ini";
        }
        if (FILE* config = std::fopen(configPath, "rb")) {
            char configText[1024];
            const size_t got = std::fread(configText, 1, sizeof(configText) - 1, config);
            configText[got] = '\0';
            std::fclose(config);
            if (mdk::parseEnhancedConfig(configText, &enhancedToggles)) {
                std::printf("[main] config: %s loaded (shadows=%d lights=%d haze=%d "
                            "mirrors=%d glow=%d filtering=%d mipmaps=%d hd=%d lighting=%d "
                            "smoothing=%d)\n",
                            configPath, (int)enhancedToggles.enableSkyShadows,
                            (int)enhancedToggles.enableFragmentLights,
                            (int)enhancedToggles.enableHaze, (int)enhancedToggles.enableMirrors,
                            (int)enhancedToggles.enableGlowBillboards,
                            (int)enhancedToggles.enableFiltering,
                            (int)enhancedToggles.enableMipmaps,
                            (int)enhancedToggles.enableHdTextures,
                            (int)enhancedToggles.enableLighting,
                            (int)enhancedToggles.enableEdgeSmoothing);
            }
        }
    }

    MdkRenderDesc desc;
    desc.width = window.width;
    desc.height = window.height;
    desc.surface = window.handle;   // the SDL_Window, for the Vulkan surface
    desc.headless = headless;
    desc.vsync = true;
    desc.validation = true;
    desc.enhanced_look = enhancedLook;
    desc.enhanced_toggles = enhancedToggles;
    desc.stereo = stereo;
    desc.hidden_width = hiddenWidth;
    desc.hidden_height = hiddenHeight;

    MdkRenderer renderer = mdk_render_create(&desc);
    if (!renderer) {
        std::printf("[main] renderer creation FAILED\n");
        mdk::windowDestroy(&window);
        return 1;
    }
    std::printf("[main] backend: %s\n", mdk_render_backend_name(renderer));
    mdk_render_set_gamma(renderer, gammaValue);

    // The output stream (the C#'s AudioDevice(Output.Speakers); Game.cs:206 picks
    // Output.Muted for --mute instead). No audio device is not an error: the device
    // then mixes and drops, which is the headless oracle's own shape. --headless is
    // muted too (see `mute` above), so every test and capture runs as before.
    if (mute || headless) {
        std::printf("[audio] muted (%s): the mix runs and is dropped, as the C#'s "
                    "Output.Muted\n", mute ? "--mute" : "--headless");
    } else {
        mdk::audioOpen();
    }

    // THE SETTINGS (C# Flow/Settings.cs): the C#'s settings.cfg in the user
    // folder (menu_screens.h). The volumes reach the level's mixer, the difficulty
    // Kurt and the gore the scripts as each level starts (Viewer.cs:231-235); the
    // menus' options change them and save the file.
    const std::string settingsFile = mdk::settingsPath();
    mdk::GameSettings settings = mdk::loadSettings(settingsFile);
    // --bloodyes / --nobloodno (C# GameOptions.Gore): gore instead of the settings'.
    if (goreOverride >= 0) {
        settings.gore = goreOverride == 1;
    }

    // ---------------------------------------------------------------------
    // THE FLOW (game/game_flow.h, the C#'s Game): it decides which level plays,
    // and after a level's end which screen follows. The run starts in a level
    // with --level (Start::Level), as the C#'s `--level` does, and at the main
    // menu without it (Start::Menu, package #20a).
    // ---------------------------------------------------------------------
    mdk::flow::FlowOptions flowOptions;
    flowOptions.start = fallGiven     ? mdk::flow::Start::Fall
                        : startStream ? mdk::flow::Start::Stream
                        : levelGiven  ? mdk::flow::Start::Level
                                      : mdk::flow::Start::Menu;
    // Without --level (or --stream, --fall) the C#'s default first level
    // (ViewerOptions, the order's first: LEVEL7), which is where the menu's "Level: 1"
    // starts.
    if (levelGiven || startStream || fallGiven) {
        flowOptions.level = levelNumber;
    }
    // --stats=N (C# Start.Statistics): the screens after level N, with --counts
    // and --towns (FlowOptions, the C#'s ApplyTestCounts).
    if (statsGiven && !levelGiven) {
        flowOptions.start = mdk::flow::Start::Statistics;
        flowOptions.level = levelNumber;
    }
    if (!statsCounts.empty()) {
        flowOptions.setCounts(statsCounts.data(), (int32_t)statsCounts.size());
    }
    flowOptions.hasTowns = townsGiven;
    flowOptions.towns = towns;
    flowOptions.load = loadName;
    flowOptions.save = saveName;
    flowOptions.hasHealth = hasStreamHealth;
    flowOptions.health = streamHealthOption;
    mdk::flow::GameFlow flow(flowOptions);
    if (saveDir.empty()) {
        const char* user = std::getenv("MDK_USER_DIR");
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        if (user && user[0]) {
            saveDir = mdk::flow::SaveGames::in(user).directory();
        } else if (xdg && xdg[0]) {
            saveDir = std::string(xdg) + "/mdk-sdl/saves";
        } else if (home && home[0]) {
            saveDir = std::string(home) + "/.local/share/mdk-sdl/saves";
        }
    }
    // C# Game's constructor: the saves, and LASTGAME deleted ("the original deletes
    // LASTGAME.SAV when it quits").
    const mdk::flow::SaveGames saves(saveDir);
    saves.remove(mdk::flow::SaveGames::kLastGame);
    flow.setSaves(saves);
    std::printf("[main] saves: %s\n", saveDir.empty() ? "(none: no home folder)" : saveDir.c_str());
    // Prints what the flow's own save work said (C#'s Console lines), once.
    auto printFlowReport = [&flow]() {
        const std::string report = flow.takeReport();
        if (!report.empty()) {
            std::printf("[main] %s\n", report.c_str());
        }
    };
    mdk::flow::Screen screen = flow.handle(flow.firstEvent());
    printFlowReport();
    // --load of a before-level save starts at its briefing, --stats at the
    // statistics (the menus' screens); --stream=N starts in the stream (package
    // #18b), --fall=N in the fall (#17b).
    if (screen != mdk::flow::Screen::Level && screen != mdk::flow::Screen::Menu &&
        screen != mdk::flow::Screen::Briefing && screen != mdk::flow::Screen::Stream &&
        screen != mdk::flow::Screen::Fall && screen != mdk::flow::Screen::Statistics) {
        std::fprintf(stderr, "[main] the flow did not start in a level, the menu, the briefing, the statistics, the stream or the fall\n");
        mdk_render_destroy(renderer);
        mdk::audioClose();
        mdk::windowDestroy(&window);
        return 1;
    }
    // The menus' screens and the presses' clock (the C#'s Game.Run `time`).
    float pressTime = 0.0f;
    mdk::FrontScreens front;
    front.renderer = renderer;
    front.window = &window;
    front.headless = headless;
    front.fixedStep = fixedStep;
    front.flow = &flow;
    front.settings = &settings;
    front.settingsFile = settingsFile;
    front.presses = &presses;
    // The main menu's Continue and Saved Game page read the saves (package #19).
    front.saves = &saves;
    front.time = &pressTime;
    front.maxFrames = maxFrames;
    front.captureFrame = captureFrame;
    front.capturePath = capturePath.empty() ? nullptr : capturePath.c_str();
    front.fallLean = fallLean;
    front.skipFalls = skipFalls;
    front.fallChase = fallChase;
    front.fallSeedSet = scriptSeedSet || fixedStep;
    front.fallSeed = scriptSeedSet ? scriptSeed : 1;
    // --phase: the first statistics' page (C# `_firstLevel ? _options.Phase : null`);
    // --skip-statistics: a level's end skips them.
    front.statsPhase = statsGiven ? statsPhase : 0;
    front.skipStatistics = skipStatistics;
    // THE PAUSE MENU (C# PauseMenu: Esc in a level). It outlives the levels; its
    // options change `settings` and save them, and the volumes reach the live
    // mixer at once (C# Settings.Apply), through `pauseDevice`.
    mdk::menu::PauseMenu pause;
    struct PauseApply {
        mdk::GameSettings* settings;
        const std::string* file;
        mdk::audio::SoundDevice* device;
        static void applied(void* user) {
            PauseApply* self = static_cast<PauseApply*>(user);
            self->settings->applyAudio(self->device);
            mdk::saveSettings(*self->settings, *self->file);
        }
    };
    PauseApply pauseApply{&settings, &settingsFile, nullptr};
    // The run's frame count and its end: run-scoped, so --frames, --capture-frame
    // and the log count frames across every level the run plays.
    uint32_t frame = 0;
    const char* exitReason = nullptr;
    // C# Game._firstLevel: the first level gets the command line's test options
    // (--event; --at), later ones do not (Game.cs:487).
    bool firstLevel = true;
    printMemory("run-start", levelNumber, memorySample(renderer));
    // A run that starts at the menu hands no test option to a level (C#
    // Handle(Menu) clears _firstLevel before any level is played).
    if (screen == mdk::flow::Screen::Menu || screen == mdk::flow::Screen::Fall ||
        screen == mdk::flow::Screen::Statistics) {
        firstLevel = false;
    }

    // =====================================================================
    // THE LEVEL LOOP (package 2, the next-level transition). One pass per level:
    // load, play, and -- when the level ends and the flow names another level
    // (GameFlow::skipUnported, the screens this port lacks skipped) -- tear the
    // level down and go round again. The C#'s Game.Show does the same between
    // screens: the old screen disposed, every voice stopped, the renderer's scope
    // released (Game.cs:539-547), then the new Viewer built.
    //
    // The body is NOT re-indented (a 1000-line whitespace diff in a file other
    // packages edit). Its two blocks: the loop, and inside it the level's own
    // block, whose closing brace destroys every level object in reverse order of
    // declaration before the renderer's scope is released. Both closing braces are
    // marked "END OF THE LEVEL".
    // =====================================================================
    for (;;) {
    // THE STREAM (package #18b), when the flow shows one: after a level's end (the
    // level-end hook below goes round the loop for it) or first, from --stream=N.
    // Its own renderer scope, released with the same empty frames a level's
    // teardown gets; then its Event is handled, the screens this port still lacks
    // are skipped, and a level that follows is loaded below.
    if (flow.screen() == mdk::flow::Screen::Stream) {
        const MdkScope streamScope = mdk_render_mark(renderer);
        StreamFrontend streamFrontend;
        streamFrontend.renderer = renderer;
        streamFrontend.window = &window;
        streamFrontend.headless = headless;
        streamFrontend.fixedStep = fixedStep;
        streamFrontend.walk = hasWalk ? &walk : nullptr;
        streamFrontend.frame = &frame;
        streamFrontend.maxFrames = maxFrames;
        streamFrontend.captureFrame = captureFrame;
        streamFrontend.capturePath = &capturePath;
        streamFrontend.difficulty = (mdk::Kurt::Difficulty)settings.difficulty;
        // C# ShowStream's seed: DefaultSeed for --stream, else Environment.TickCount --
        // this frontend's clock, except in a --fixed-step run, whose every stream takes
        // the default seed so the run is reproducible.
        const uint32_t clockSeed =
            fixedStep ? mdk::flow::kStreamDefaultSeed
                      : (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        const int32_t streamLevel = flow.state().level;
        int32_t streamEndHealth = flow.streamHealth();
        const char* streamExit = nullptr;
        mdk::flow::Event streamEvent = mdk::flow::Event::Quit;
        {
            const mdk::formats::MdkData streamData =
                mdk::formats::MdkData::find(mdk::formats::fileSystem());
            streamEvent = runStreamScreen(streamFrontend, streamData, streamLevel,
                                          flow.streamHealth(), flow.streamSeed(clockSeed),
                                          &streamEndHealth, &streamExit);
        }
        mdk_render_release(renderer, streamScope);
        for (int i = 0; i < kBetweenLevelFrames; i++) {
            MdkFrameParams blank;
            blank.elapsed = 0.0f;
            blank.frame_index = frame;
            blank.clear_colour = 0xFF000000u;
            blank.present = true;
            blank.screenshot_path = nullptr;
            mdk_render_begin_frame(renderer, &blank);
            mdk_render_end_frame(renderer);
        }
        printMemory("stream-teardown", streamLevel, memorySample(renderer));
        if (streamEvent == mdk::flow::Event::Quit) {
            exitReason = streamExit;
            break;
        }
        // C# AfterStream reads the stream screen's Health before it shows the next.
        flow.setStreamHealth(streamEndHealth);
        mdk::flow::Screen after = flow.handle(streamEvent);
        std::printf("[main] flow: the stream after level %d ended (health %d); the flow shows "
                    "%s\n",
                    (int)streamLevel, (int)streamEndHealth,
                    mdk::flow::GameFlow::screenName(after));
        mdk::flow::GameFlow::SkipTrail trail;
        after = flow.skipUnported(&trail);
        for (int32_t i = 0; i < trail.count; i++) {
            std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                        mdk::flow::GameFlow::screenName(trail.skipped[i]),
                        (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
        }
        // The statistics after the stream (C# AfterStream -> Event.Statistics, package
        // #20b): the front screens show them, or --skip-statistics takes the C#'s skip
        // of them, as the level-end hook below does.
        if (after == mdk::flow::Screen::Statistics && skipStatistics) {
            std::printf("[main] flow: skipped Statistics (--skip-statistics)\n");
            flow.handle(flow.skipScreen());
            after = flow.skipUnported(&trail);
            for (int32_t i = 0; i < trail.count; i++) {
                std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                            mdk::flow::GameFlow::screenName(trail.skipped[i]),
                            (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
            }
        }
        if (after == mdk::flow::Screen::Fall && skipFalls) {
            // --skip-falls (package #17b): the next level's fall skipped the C#'s Esc way,
            // as the level-end hook below does.
            std::printf("[main] flow: skipped Fall (--skip-falls)\n");
            after = flow.handle(flow.skipScreen());
        }
        if (after == mdk::flow::Screen::Level && replayLevel && !firstLevel) {
            std::printf("[main] flow: --replay-level: level %d again instead of %d\n",
                        (int)streamLevel, (int)flow.state().level);
            flow.state().level = streamLevel;
        }
        screen = after;
        if (after == mdk::flow::Screen::Level) {
            std::printf("[main] flow: next level %d at frame %u, entered at health %d\n",
                        (int)flow.state().level, frame, (int)flow.levelHealth());
        } else if (after == mdk::flow::Screen::Statistics) {
            // The statistics (package #20b): the front screens show them, then the
            // fall and the level.
            std::printf("[main] flow: the statistics follow\n");
        } else if (after == mdk::flow::Screen::Menu || after == mdk::flow::Screen::Fall) {
            // Kurt died in the Gunter stream: the main menu (package #20a), below. Or
            // the next level's fall (#17b, after the statistics): the front screens
            // show it, then the level.
            std::printf("[main] flow: the %s follows\n",
                        after == mdk::flow::Screen::Fall ? "fall" : "menu");
        } else {
            std::printf("[main] flow: %s follows, which this frontend cannot show; exiting\n",
                        mdk::flow::GameFlow::screenName(after));
            exitReason = "stream ended";
            break;
        }
        // C# Handle clears _firstLevel after the stream's event: the level that
        // follows is not the first level (its test options are the first level's).
        firstLevel = false;
    }
    // THE MENU AND THE BRIEFING (package #20a): the screens before a level, until
    // the flow names one (menu_screens.h). The first pass of a --level run, and
    // every level that ends into a level, skip straight past.
    if (screen != mdk::flow::Screen::Level) {
        front.frame = &frame;
        screen = mdk::runFrontScreens(&front);
        if (screen != mdk::flow::Screen::Level) {
            exitReason = front.exitReason;
            break;
        }
        if (maxFrames > 0 && (int)frame >= maxFrames) {
            break;
        }
    }
    levelNumber = flow.state().level;
    // Set when the level is left for the main menu (the pause menu's "Main
    // menu", or a level that ends into the menu).
    bool toMenu = false;
    // Everything the renderer makes from here on is the level's.
    const MdkScope levelScope = mdk_render_mark(renderer);
    bool nextLevel = false;
    {
    // ---------------------------------------------------------------------
    // The level. The same sequence the 3DS frontend runs.
    // ---------------------------------------------------------------------
    mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    mdk::LevelData level;
    // The mods (p6 slice g; the C# Viewer's ScanMods + ModImages): the
    // enhanced look's HD textures and image replacements, from the user
    // folder's mods/ ($MDK_USER_DIR, the oracle's override; ./mods here,
    // where this frontend's config.ini already lives). The original look
    // never sees them, exactly as the C# only builds ModImages for the
    // enhanced graphics.
    mdk::ModCatalog modCatalog;
    mdk::ModImages modImages;
    mdk::ModModels modModels;
    ModReplacementSource modSource;
    bool modsReady = false;
    if (enhancedLook) {
        const char* userDir = std::getenv("MDK_USER_DIR");
        const std::string modsRoot =
            (userDir && userDir[0]) ? mdk::ModCatalog::folderIn(userDir) : "mods";
        modCatalog = mdk::ModCatalog::scan(modsRoot);
        if (onlyModsSet) {
            for (size_t i = 0; i < onlyMods.size(); i++) {
                if (!modCatalog.has(onlyMods[i])) {
                    std::fprintf(stderr, "No mod %s in %s\n", onlyMods[i].c_str(),
                                 modsRoot.c_str());
                }
            }
        }
        // The settings file's mod states (settings.cfg beside the mods:
        // `mod.<folder>=Off` switches a mod off between runs, the
        // oracle's Settings). --mod still wins for its run.
        std::vector<std::pair<std::string, mdk::ModState>> modStates;
        {
            const std::string settingsPath =
                (userDir && userDir[0]) ? std::string(userDir) + "/settings.cfg"
                                        : "settings.cfg";
            if (std::FILE* file = std::fopen(settingsPath.c_str(), "rb")) {
                std::string text;
                char chunk[1024];
                size_t got;
                while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                    text.append(chunk, got);
                }
                std::fclose(file);
                modStates = mdk::modStatesFromSettingsText(text);
            }
        }
        const std::vector<std::string> folders =
            onlyModsSet ? modCatalog.onlyFolders(onlyMods)
                        : modCatalog.enabledFolders(modStates);
        modImages = mdk::ModImages::open(folders, true, levelNumber);
        modModels = mdk::ModModels(folders, levelNumber);
        modSource.mods = &modImages;
        modsReady = true;
        std::printf("[main] mods: %d found, %d enabled in %s\n",
                    (int)modCatalog.mods().size(), (int)folders.size(), modsRoot.c_str());
    }

    // The GPU textures and palettes a resolved surface needs, shared by the arena
    // view and the object pass exactly as the C# shares ONE MaterialResolver
    // between them, so a texture an arena and an object both use is one texture
    // (render/surface_cache.h).
    mdk::SurfaceCache surfaces(renderer);
    if (modsReady) {
        surfaces.setColourReplacementLookup(modReplacementLookup, &modSource);
    }
    // The arenas on the GPU: a LevelView per arena the scripts have loaded,
    // following the arena stream (game/arena_views.h), exactly as the 3DS
    // frontend does -- one path on both targets, so this build exercises the
    // stream the console depends on. (Until 2026-11-18: one view, the start
    // arena's, and nothing past the first door was drawn.)
    mdk::ArenaViews arenaViews;
    std::string startArenaName;
    std::vector<std::string> startDrawn;
    const std::vector<std::string>* frameDrawn = nullptr;   // this frame's drawn list
    int32_t lastDemandLoads = 0;
    mdk::HudView hud;
    // The HUD messages' fonts (the C# HudView's pair: MDKFONT.FTI's FONTBIG and
    // FONTSML through SYS_PAL). On the heap: each carries a whole Font.
    std::unique_ptr<mdk::FontView> hudBig(new mdk::FontView());
    std::unique_ptr<mdk::FontView> hudSmall(new mdk::FontView());
    // The save prompt's texts (MDKFONT.FTI's SV_ASK, ABORT2, ABORT3, SV_TITLE), drawn
    // in FONTBIG (Menu/SavePrompt.cs Draw).
    std::vector<std::pair<std::string, std::vector<uint8_t>>> promptTexts;
    mdk::KurtSprite kurtSprite;
    mdk::formats::Cmi cmi;
    mdk::audio::SoundBank bank;
    // The sprites archive is a level asset too, and it is a constructor argument of
    // the runtime for a reason: the built-in behaviours play their animations out of
    // it (SW_H150's H150_R / H150_I, ObjectBehaviors.UpdateRunner). So it lives out
    // here, not inside the HUD block that first reads it: the runtime below borrows
    // it and is rebuilt on every respawn.
    mdk::formats::Bni sprites;
    bool spritesReady = false;
    bool scriptAssetsReady = false;
    bool hudReady = false;
    bool kurtSpriteReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        std::printf("[main] data: %s\n", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, levelNumber)) {
            if (level.corridorReadFailures() != 0) {
                std::printf("[main] level: %d corridor read(s) FAILED -- the level has holes "
                            "where those corridors are\n",
                            (int)level.corridorReadFailures());
            }
            // The scripts and their sounds are level assets, just as in the
            // C# Viewer. They are kept alive for the runtime below.
            char cmiPath[128];
            std::snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI",
                          levelNumber, levelNumber);
            std::vector<uint8_t> cmiBytes;
            if (!data.read(cmiPath, &cmiBytes)) {
                std::printf("[main] scripts: CMI did not read\n");
            } else if (!mdk::formats::Cmi::parse(&cmi, cmiBytes.data(),
                                                static_cast<int32_t>(cmiBytes.size()))) {
                std::printf("[main] scripts: CMI did not parse\n");
            } else if (!mdk::audio::SoundBank::forLevel(&bank, data, levelNumber)) {
                std::printf("[main] scripts: sound bank did not load\n");
            } else {
                bank.preload();
                scriptAssetsReady = true;
                std::printf("[main] scripts: assets loaded\n");
            }

            const int32_t start = level.dti().startArena();
            if (start >= 0 && start < (int32_t)level.dti().arenas().size()) {
                arena = level.arenaNamed(level.dti().arenas()[start].name);
            }
            if (!arena && !level.arenas().empty()) {
                arena = level.arenas()[0];
            }
            if (arena) {
                std::printf("[main] arena: %s (%d arenas)\n", arena->name().c_str(),
                            (int)level.arenas().size());
                startArenaName = arena->name();
                arenaViews.start(renderer, &level, &surfaces,
                                 enhancedLook ? mdk::Shading::Lit : mdk::Shading::Original);
                // The level's start is a SHOW of Kurt's arena (0x41ba68): its
                // textures are read in whole, now, and its view built.
                level.stream().update(startArenaName, std::string(), false);
                arenaViews.sync(startArenaName, std::string());
                if (const mdk::LevelView* startView = arenaViews.find(startArenaName)) {
                    std::printf("[main] view: %d batches, %d vertices, %d triangles\n",
                                startView->batchCount(), startView->vertexCount(),
                                arena->triangleCount());
                    // The sky panorama (C# Viewer.CreatePanorama): the DTI's
                    // sky index texture through the base palette.
                    const mdk::formats::Texture& sky = level.dti().sky();
                    const uint8_t* pal = level.dti().palette().rgba();
                    mdk::Panorama pano;
                    pano.sky = mdk_render_create_index_texture(renderer, sky.width(), sky.height(),
                                                               sky.indices(), pal);
                    pano.mirror_sky = pano.sky;
                    pano.palette = mdk_render_create_palette(renderer, pal);
                    pano.wrap_width = (float)level.dti().skyWrapWidth();
                    pano.horizon_row = (float)level.dti().skyHorizonRow();
                    pano.offset = (float)level.dti().skyOffset();
                    pano.height = (float)sky.height();
                    pano.top_colour = (uint32_t)level.dti().skyTopColor();
                    pano.bottom_colour = (uint32_t)level.dti().skyBottomColor();
                    pano.linear = enhancedLook;
                    mdk_render_set_panorama(renderer, &pano);
                    std::printf("[main] sky: %dx%d wrap=%d horizon=%d offset=%d top=%d bottom=%d\n",
                                sky.width(), sky.height(), level.dti().skyWrapWidth(),
                                level.dti().skyHorizonRow(), level.dti().skyOffset(),
                                level.dti().skyTopColor(), level.dti().skyBottomColor());

                    // The HUD and Kurt's sprites (C# Viewer:
                    // TRAVERSE/TRAVSPRT.BNI), indexed through the level's
                    // palette like the C# HudView and KurtSprite.
                    std::vector<uint8_t> spriteBytes;
                    if (data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) &&
                        mdk::formats::Bni::parse(&sprites, std::move(spriteBytes))) {
                        spritesReady = true;
                        if (hud.load(renderer, sprites, level.dti().palette(),
                                     modsReady ? &modImages : nullptr)) {
                            hudReady = true;
                            std::printf("[main] hud: loaded\n");
                        } else {
                            std::printf("[main] hud: FAILED\n");
                        }
                        // The messages' fonts and texts (HudView's constructor).
                        // Without them the messages are kept but not drawn.
                        {
                            std::vector<uint8_t> ftiBytes;
                            mdk::formats::Fti fti;
                            std::vector<uint8_t> bigBytes;
                            std::vector<uint8_t> smallBytes;
                            std::vector<uint8_t> sysPal;
                            std::unique_ptr<mdk::formats::Font> bigFont(new mdk::formats::Font());
                            std::unique_ptr<mdk::formats::Font> smallFont(new mdk::formats::Font());
                            if (data.read("MISC/MDKFONT.FTI", &ftiBytes) &&
                                mdk::formats::Fti::parse(&fti, std::move(ftiBytes)) &&
                                fti.getBytes("FONTBIG", &bigBytes) &&
                                fti.getBytes("FONTSML", &smallBytes) &&
                                fti.getBytes("SYS_PAL", &sysPal) &&
                                mdk::formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(),
                                                          6, bigFont.get()) &&
                                mdk::formats::Font::parse(smallBytes.data(),
                                                          (int32_t)smallBytes.size(), 4,
                                                          smallFont.get())) {
                                const mdk::formats::Palette system = mdk::formats::Palette::fromRgb(
                                    sysPal.data(), (int32_t)sysPal.size());
                                const int32_t palette =
                                    mdk_render_create_palette(renderer, system.rgba());
                                if (palette >= 0 &&
                                    hudBig->load(renderer, *bigFont, palette, system.rgba()) &&
                                    hudSmall->load(renderer, *smallFont, palette, system.rgba())) {
                                    hud.setFonts(hudBig.get(), hudSmall.get());
                                    std::printf("[main] hud: message fonts loaded\n");
                                }
                                hud.messages().setTexts(fti);
                                static const char* const kPromptKeys[] = {"SV_ASK", "ABORT2",
                                                                          "ABORT3", "SV_TITLE"};
                                for (size_t k = 0; k < 4; k++) {
                                    promptTexts.push_back(std::make_pair(
                                        std::string(kPromptKeys[k]),
                                        fti.getTextBytes(kPromptKeys[k])));
                                }
                            } else {
                                std::printf("[main] hud: MDKFONT.FTI fonts did not load -- no "
                                            "messages\n");
                            }
                        }
                        kurtSprite.setShading(enhancedLook ? mdk::Shading::Sprite
                                                            : mdk::Shading::Original);
                        if (modsReady) {
                            kurtSprite.setMods(&modImages);
                        }
                        if (kurtSprite.load(renderer, sprites, level.dti().palette())) {
                            kurtSpriteReady = true;
                            // The level's SNI archives carry the slide and
                            // snowboard frames (C# Viewer: KurtSprite.Add over
                            // SoundBank.Animation).
                            for (int i = 0; i < mdk::KurtSprite::levelAnimationCount(); i++) {
                                const char* name = mdk::KurtSprite::levelAnimationName(i);
                                mdk::formats::SpriteAnimation animation;
                                if (bank.animation(name, &animation)) {
                                    kurtSprite.add(renderer, name, animation);
                                }
                            }
                            std::printf("[main] kurt sprite: loaded\n");
                        } else {
                            std::printf("[main] kurt sprite: FAILED\n");
                        }
                    } else {
                        std::printf("[main] sprites: FAILED — the HUD, Kurt's sprite and the "
                                    "scripts all need TRAVERSE/TRAVSPRT.BNI\n");
                    }
                } else {
                    std::printf("[main] view: FAILED\n");
                    arena = nullptr;
                }
            }
        } else {
            std::printf("[main] level %d: FAILED to load\n", levelNumber);
        }
    } else {
        std::printf("[main] data: NOT FOUND\n");
    }

    // The BSP world Kurt moves through: the arenas reachable from the start
    // (the C# Viewer adds only IsReachable ones -- a hidden arena whose bounds
    // overlap the spawn column would otherwise steal the arena selection).
    mdk::ArenaSpace space;
    for (const mdk::formats::Arena* a : level.arenas()) {
        if (level.isReachable(a->name())) {
            space.add(a);
        }
    }

    // ---------------------------------------------------------------------
    // The object pass: the C#'s Viewer._objects, Viewer._looks and DrawObjects.
    //
    // WHY IT SITS HERE, BEFORE spawnPlayer. The layout cache is keyed by
    // (model, palette), and the models are NOT all the level's: ScriptRuntime
    // parses the CMI's global models into its own modelCache_ and holds them BY
    // VALUE (script_runtime.h:580), so a respawn -- which rebuilds the runtime --
    // frees every one of those Model addresses. Both caches that key on a Model
    // address (this layout cache, and the shared pose caches scripts/pose_cache.h
    // exists to drop) would then be keyed by freed memory, and the new runtime's
    // models land on recycled addresses: same level, same parse order, so the
    // address is likely to reappear holding a DIFFERENT model, and the cache hands
    // it the previous model's geometry and pose. The C# cannot have that: a respawn
    // builds a fresh Viewer, whose constructor calls MdkObject.ForgetPoses()
    // (Viewer.cs:185) and constructs a fresh ObjectView, so both caches die with
    // the runtime that owned the models. So spawnPlayer drops both, and that is why
    // these declarations are above it and not beside the draw below.
    //
    // The LOOKS are keyed by arena NAME and point into LevelData, which is not
    // rebuilt, so they survive a respawn and are not cleared by one.
    //
    // The shading follows the look, as LevelView's does.
    // ---------------------------------------------------------------------
    mdk::MaterialResolver objectResolver(&level.dti(),
                                         enhancedLook ? mdk::Shading::Lit : mdk::Shading::Original);
    mdk::ObjectView objectView(objectResolver);
    if (modsReady) {
        objectView.setSwaps(&modModels);
    }
    mdk::ObjectDraw objectDraw;
    // Sniper mode's view besides the world through the scope: the overlay's state,
    // the round cameras' and the clip's insets (game/sniper_view.h).
    mdk::SniperView sniperView;
    bool sniperClipFailureLogged = false;
    mdk::EffectsView effectsView(objectResolver, &level,
                                 enhancedLook ? mdk::Shading::Sprite : mdk::Shading::Original);
    mdk::EffectsDraw effectsDraw;
    mdk::EffectsFrame effectsFrame;
    mdk::ObjectStream objectStream;
    std::map<std::string, mdk::ObjectLook> objectLooks;

    // An arena's palette and textures for its objects (C# Viewer.ObjectLook):
    // the arena's own when it has one, the level's for an object outside every
    // arena. Cached by arena name, exactly as the C# caches its looks.
    auto objectLook = [&](const std::string& name) -> const mdk::ObjectLook& {
        std::map<std::string, mdk::ObjectLook>::iterator found = objectLooks.find(name);
        if (found != objectLooks.end()) {
            return found->second;
        }
        mdk::ObjectLook look;
        const mdk::formats::Arena* owner = level.arenaNamed(name);
        if (owner) {
            look.palette = &level.paletteOf(owner);
            look.archives = level.archivesOf(owner);
        } else {
            look.palette = &level.dti().palette();
            look.archives.push_back(&level.levelTextures());
        }
        return objectLooks.emplace(name, look).first->second;
    };

    // Kurt at the level's spawn, RAISED 3 units above the DTI's start position
    // (the C# Viewer's `StartPosition + (0,0,StartDrop)`, Viewer.cs:220 — the
    // original lands Kurt by chute, so the pad is below him), facing the start
    // angle. The follow camera owns its own
    // pitch smoothing, starting at the game's default (4 degrees). Used for the
    // first spawn and every respawn after a death: the C# reloads the level on
    // death, and with no scripts the geometry is static, so resetting Kurt and the
    // camera to the start is the same fresh start.
    mdk::Input input;
    mdk::Player player;
    // C# Viewer._cheats (Viewer.cs:171, `new Cheats(ui.Settings, state)`): one per level,
    // so a half-typed cheat does not carry into the next. It flips the run's settings
    // (gore, not saved by itself) and the flow's state (the debug keys).
    mdk::flow::Cheats cheats(&settings, &flow.state());
    bool typeTestDone = false;
    mdk::FollowCamera follow;
    // The scripts' cutscene camera (package 13, game/cutscene_camera.h): the C# Viewer's
    // _cutscene (Viewer.cs:142), the view instead of `follow` while a cutscene holds it.
    mdk::CutsceneCamera cutsceneCamera;
    std::unique_ptr<mdk::scripts::ScriptRuntime> runtime;
    // THE LEVEL'S MUSIC (audio/level_music.h, the C#'s LevelMusic): the track of
    // Kurt's arena, played through the runtime's mixer -- so it lives and dies
    // with the runtime.
    std::unique_ptr<mdk::audio::LevelMusic> music;
    // The arena whose track the bank was last asked to prefetch (the frame loop).
    std::string musicPrefetchedFor;
    bool playerReady = false;
    // The health the level is ENTERED with (the 3DS frontend's entryHealth): the
    // fall's carry, which a skipped fall makes 100, or LEVEL8's end health into
    // LEVEL5 (GameFlow::levelHealth, Viewer.CarryFromFall). The level's first spawn
    // takes it; a respawn after a death is a fresh Kurt.
    const int32_t entryHealth = flow.levelHealth();
    // ...and the pickups the fall carried (package #17b, Viewer.CarryFromFall: the
    // pickups taken again, then the health; fall::carryIntoLevel).
    const mdk::flow::FallCarry entryCarry = flow.levelCarry();
    bool firstSpawn = true;
    auto spawnPlayer = [&]() {
        const float* start = level.dti().startPosition();
        player = mdk::Player();
        // The difficulty decides the town's timer: before the scripts (Viewer.cs:231,
        // `_kurt.Inventory.Difficulty = ui.Settings.Difficulty`).
        player.combat().setDifficulty((mdk::Kurt::Difficulty)settings.difficulty);
        if (firstSpawn) {
            mdk::fall::carryIntoLevel(entryCarry, &player.combat());
            if (!entryCarry.pickups.empty()) {
                std::string slots;
                const mdk::Kurt& kurt = player.combat();
                for (int32_t s = 0; s < kurt.slotCount(); s++) {
                    char slot[24];
                    std::snprintf(slot, sizeof(slot), "%s%d:%d", s == 0 ? "" : ";",
                                  (int)kurt.slots()[s].item, (int)kurt.slots()[s].count);
                    slots += slot;
                }
                std::string names;
                for (size_t p = 0; p < entryCarry.pickups.size(); p++) {
                    names += (p == 0 ? "" : ",") + entryCarry.pickups[p];
                }
                std::printf("[main] level: the fall's carry: health %d, pickups %s; inventory "
                            "slots %s ammo %d:%d:%d:%d:%d sgun %d\n",
                            (int)entryHealth, names.c_str(), slots.empty() ? "-" : slots.c_str(),
                            (int)kurt.ammo(1), (int)kurt.ammo(2), (int)kurt.ammo(3),
                            (int)kurt.ammo(4), (int)kurt.ammo(5), (int)kurt.superChainGun());
            }
            firstSpawn = false;
        }
        // --at is a test option, so the first level's alone (Game.cs:487).
        if (hasStartOverride && firstLevel) {
            player.feet = startOverride;
            player.yaw = hasYawOverride ? yawOverride : level.dti().startAngle();
        } else {
            player.feet = mdk::Vec3{start[0], start[1], start[2] + 3.0f};
            player.yaw = level.dti().startAngle();
        }
        player.setSpace(&space);
        if (kurtSpriteReady) {
            player.setFrameCounts(kurtSprite.frameCounts());
        }
        // The session's cheats hold in every level (Viewer.cs:286-288, SetGod/SetNoclip
        // from DevSession), and in a respawn's fresh Kurt.
        player.setMortality(godCheat ? mdk::Kurt::God : mdk::Kurt::Mortal);
        player.setClipping(noclipCheat ? mdk::Player::ClipOff : mdk::Player::ClipOn);
        // A respawn is a fresh C# Viewer, and with it a fresh Cheats (an empty window).
        cheats = mdk::flow::Cheats(&settings, &flow.state());
        if (godCheat || noclipCheat) {
            std::printf("[main] cheats: god %s, noclip %s\n", godCheat ? "on" : "off",
                        noclipCheat ? "on" : "off");
        }
        // Pickups show their names (Viewer.cs:249). A respawn is a fresh C# Viewer,
        // so its HUD's messages start empty.
        hud.messages().reset();
        {
            mdk::Kurt::PickupHook pickupHook;
            pickupHook.context = &hud.messages();
            pickupHook.pickedUp = mdk::Messages::pickedUp;
            player.combat().setPickupHook(pickupHook);
        }
        follow = mdk::FollowCamera();
        cutsceneCamera = mdk::CutsceneCamera();
        // Pin Kurt's arena to the one around the spawn (the C# SetSolid of the
        // scripts' drawn arenas). The step loop refreshes it from the runtime's drawn
        // list from here on, or from the arena under his feet when there is no
        // runtime; this first pin is what covers a Kurt standing outside every arena
        // and a runtime that has not ticked yet, where the lookup finds nothing and
        // bottom() -- and so the fall-out death -- would otherwise resolve nothing at
        // all.
        const std::string* here =
            space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
        if (here) {
            space.setSolid({*here});
        }
        // And it is the arena the frame shows until the scripts' first tick names
        // one (the stream step's fallback): Kurt's own, which is the level's start
        // arena unless he was placed elsewhere (--at, a respawn).
        if (here) {
            startArenaName = *here;
        }

        // The C# reloads the level and constructs a fresh ScriptRuntime when
        // Kurt respawns. Recreate the runtime here too, so its object list,
        // tick count and random stream start over with the player.
        //
        // The sprites archive is one of the runtime's assets, not an optional
        // decoration, so it gates the runtime exactly as the CMI and the sound
        // bank do. The C# loads TRAVERSE/TRAVSPRT.BNI in Viewer before the
        // runtime exists and Bni.Load throws when it is not there: the oracle has
        // no run without it. A runtime built with a null archive is a world where
        // H150_R and H150_I do not exist — and because the Runner draws its idle
        // chance only while idle or done, that world consumes the random stream
        // differently from tick one. So the nullptr is not passed: the runtime is
        // not constructed at all, and the level runs without scripts instead of
        // running a different game.
        if (scriptAssetsReady && spritesReady) {
            // THE CACHES THAT KEY ON A MODEL ADDRESS DIE WITH THE RUNTIME, and this
            // is the moment they have to be dropped. The reset below frees
            // ScriptRuntime::modelCache_, so every Model* the layout cache and the
            // pose caches are keyed by becomes a freed address, and the new runtime
            // parses its models into whatever the allocator hands back -- very
            // likely the same addresses, holding different models. The C# drops both
            // here, because a respawn is a fresh Viewer (Viewer.cs:185). Dropping
            // derived data is never wrong: the next layout and pose are rebuilt from
            // the model, so this is safe on the first spawn too.
            mdk::scripts::MdkObject::forget_poses();
            objectView.clear();
            music.reset();   // its voices are the old runtime's mixer's
            const int32_t runtimeSeed = scriptSeedSet ? scriptSeed : freshScriptSeed();
            runtime.reset(new mdk::scripts::ScriptRuntime(&level, &cmi, runtimeSeed, &bank, &player,
                                                          &sprites));
            // The gore setting (Viewer.cs:234, `Option = ui.Settings.Gore ? 1 : 0`),
            // on by default as the oracle's (ScriptRuntime.cs:214 initialises Option to 1
            // and Settings.Gore defaults to true, Settings.cs:75). Review-6 F2: this used
            // to be 0 under a comment calling that "the oracle's Option", which is the
            // opposite of the oracle's default. The menus' Gore option sets it since
            // package #20a, for the next level as in the C#.
            runtime->option = settings.gore ? 1 : 0;
            // The one air strike of the last two levels survives a death (Viewer.cs:236).
            runtime->airStrike().usedUp = flow.state().strikeUsed;
            // The volumes and the music filter (C# Settings.Apply onto its AudioDevice;
            // here the runtime's mixer owns the device), and the pause menu's changes
            // reach this one live.
            settings.applyAudio(&runtime->mixer().device());
            pauseApply.device = &runtime->mixer().device();
            // THE LEVEL'S SOUND REACHES THE SPEAKERS. The C#'s Viewer mixes into the
            // game's one AudioDevice (Game.cs:206, handed to the Viewer through the Ui),
            // whose Update pushes the mix to its SDL stream (AudioDevice.cs:214-266);
            // here each runtime's mixer owns its device, so the level's device is given
            // the process's one stream -- the music and the effects alike, through the
            // volumes and the filter applied just above. Until 2026-11-20 nothing on
            // this target called setSink: the stream was opened, and every level mixed
            // into the muted branch. A respawn's new runtime takes the same line. The
            // queue hook is the C#'s `SDL_GetAudioStreamQueued` arm, so the device keeps
            // the C#'s 60 ms ahead (kQueueSeconds) instead of mixing the frame's delta.
            if (mdk::audioIsOpen()) {
                runtime->mixer().device().setSink(mdk::audioSink, nullptr, mdk::audioQueued);
                std::printf("[audio] the level's mixer -> the SDL stream (master %.2f, music "
                            "%.2f, effects %.2f)\n",
                            runtime->mixer().device().masterGain(),
                            runtime->mixer().device().busGain(mdk::audio::SoundDevice::Music),
                            runtime->mixer().device().busGain(mdk::audio::SoundDevice::Effects));
            }
            // hud_message reaches the HUD (Viewer.cs:245, `_scripts.Messages`).
            runtime->hudMessages = &hud.messages();
            // Kurt's own sounds (footsteps, the chute, the guns) go through his
            // mixer, which is the runtime's: C# Kurt gets the SoundMixer as a
            // constructor argument (Kurt.cs, the `SoundMixer` field) and the
            // viewer builds both over one bank. Without this the Player's play
            // calls all go to a null mixer and are dropped.
            // The camera's line of sight against the objects (Viewer.cs:237: ClipView =
            // _scripts.ClipView, package #3).
            follow.clipView.context = runtime.get();
            follow.clipView.clip = &mdk::scripts::ScriptRuntime::clipViewTrampoline;
            // The scripts shake the camera (Viewer.cs:238, `ShakeRaised +=
            // _follow.RaiseShake`; package #15). The C#'s camera draws its offsets from a
            // time-seeded Random; this one takes the scripts' seed, so --seed replays a
            // shake as it replays the scripts.
            runtime->setShakeRaisedHook(&mdk::FollowCamera::raiseShakeHook, &follow);
            follow.seedShake(runtimeSeed);
            player.setMixer(&runtime->mixer());
            std::printf("[main] scripts: runtime ready (seed %d)\n", runtimeSeed);
            // The level starts in Kurt's arena, its music at once (C#
            // Viewer.cs:225-228, the original's 0x41ba68): the DTI's start arena,
            // or where he was put.
            music.reset(new mdk::audio::LevelMusic(&runtime->mixer(), &cmi.arenaMusic(),
                                                   &cmi.arenaAmbience()));
            musicPrefetchedFor.clear();
            music->enter(startArenaName);
            std::printf("[main] music: %s for %s\n", music->playing().c_str(),
                        startArenaName.c_str());
        } else if (scriptAssetsReady) {
            std::printf("[main] scripts: NOT started — TRAVERSE/TRAVSPRT.BNI did not load, and a "
                        "runtime without it is a different random stream, not the oracle's\n");
        }
    };
    if (arena) {
        spawnPlayer();
        playerReady = true;
        mdk::windowCaptureMouse(&window, true);
        if (!startArenaOverride.empty() && hasStartOverride && firstLevel && runtime) {
            const mdk::formats::Arena* into = level.arenaNamed(startArenaOverride);
            if (into == nullptr) {
                std::printf("[main] --arena=%s: no such arena; --at alone\n",
                            startArenaOverride.c_str());
            } else {
                runtime->teleportKurt(startArenaOverride, player.feet, player.yaw);
                space.add(into);
                space.setSolid({startArenaOverride});
                startArenaName = startArenaOverride;
                std::printf("[main] --arena=%s: Kurt teleported in at (%.1f,%.1f,%.1f)\n",
                            startArenaOverride.c_str(), player.feet.x, player.feet.y,
                            player.feet.z);
            }
        }
        // C# Viewer's constructor: RestoreSnapshot (a full save being loaded), then
        // ShowNotice ("Game loaded"). The restore enters the save's arenas into the
        // scripts' space; Kurt's is this frontend's, so they go into it too (the C#
        // has ONE ArenaSpace), and Kurt's arena is pinned again where he now stands.
        bool restored = false;
        std::string restoreLog;
        if (!mdk::flow::restoreSnapshot(&flow.state(), runtime.get(), &restored, &restoreLog)) {
            std::printf("[main] save: %s; the level starts plain\n", restoreLog.c_str());
            spawnPlayer();
        } else if (restored) {
            std::printf("[main] save: %s\n", restoreLog.c_str());
            const std::vector<std::string> entered = runtime->enteredArenas();
            for (size_t i = 0; i < entered.size(); i++) {
                const mdk::formats::Arena* into = level.arenaNamed(entered[i]);
                if (into != nullptr) {
                    space.add(into);
                }
            }
            const std::string* here =
                space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
            if (here) {
                space.setSolid({*here});
                startArenaName = *here;
            }
        }
        const std::string notice = mdk::flow::takeNotice(&flow.state());
        if (!notice.empty()) {
            hud.messages().pushText((const uint8_t*)notice.data(), (int32_t)notice.size(),
                                    mdk::Messages::kFlagZoom | mdk::Messages::kFlagFront,
                                    mdk::flow::kNoticeSeconds);
            std::printf("[main] %s\n", notice.c_str());
        }
    }

    const float kStep = 1.0f / 60.0f; // the C# fixed simulation step
    const float kHudStep = 1.0f / 30.0f; // the C# HUD blink's 30 Hz boundary
    const float kAspect = trueAspect ? (float)hiddenWidth / (float)hiddenHeight : 1280.0f / 960.0f;
    float hudPending = 0.0f;
    float pending = 0.0f;
    auto last = std::chrono::steady_clock::now();

    // Edge trigger for the object pass's refusal line below. Declared with the
    // loop-carried state because it lives as long as the frame loop.
    bool streamFailureLogged = false;
    bool effectsFailureLogged = false;
    // F2's prompt while it is open (C# Viewer._snapshotPrompt): the game waits.
    std::unique_ptr<mdk::flow::SavePrompt> prompt;
    // A death or F9 leaves the level for the flow's next screen, as the level's end
    // does (C# Viewer._next = KurtDied / Play).
    bool leaveLevel = false;
    mdk::flow::Event leaveEvent = mdk::flow::Event::None;
    // C# Viewer.Notify: the port's own HUD message, also printed.
    auto notify = [&hud](const char* text) {
        hud.messages().pushText((const uint8_t*)text, (int32_t)std::strlen(text),
                                mdk::Messages::kFlagZoom | mdk::Messages::kFlagFront,
                                mdk::flow::kNoticeSeconds);
        std::printf("[main] %s\n", text);
    };
    // Build the camera once up front so the very first frame has a valid matrix
    // even if no simulation step fits in it (a zero camera would collapse every
    // vertex to the clip origin and draw nothing).
    mdk::Camera camera;
    if (playerReady) {
        // Seed the matrix with the NON-clearing placement: the C# places no
        // camera before its first step (FollowCamera.Update, which runs the
        // clearance, is step 1), so seeding with the cleared build would shove
        // Kurt mid-air at spawn before any physics has run. The roll is 0 because
        // this Player has not stepped yet: C# CameraRoll starts level.
        mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                               mdk::follow_camera::kDefaultPitch, mdk::follow_camera::kDefaultPivot,
                               kAspect, 0.0f);
    }
    // THE END OF A LEVEL (package 1). The C#'s Viewer subscribes to the scripts'
    // LevelEnded (Viewer.cs:240) and leaves EndDelay (0.5 s, :112) later through
    // Game.cs's Event.LevelEnded (:442, AfterLevel). The runtime's levelEnded() is that
    // event, latched; `endingDelay` is the C#'s _ending countdown, started on the frame
    // it is first seen and spent by the same frame's elapsed (Viewer.cs:450-457), and
    // `levelEndedExit` is its `_next` -- acted on after the frame is drawn.
    const float kEndDelay = 0.5f;
    float endingDelay = 0.0f;
    bool endingStarted = false;
    bool levelEndedExit = false;
    // C# Event.GameFinished (Viewer.cs:241): special_event 81 latched it (package 13).
    bool gameFinishedExit = false;
    bool endSeen = false;
    bool tearRefusedLogged = false;
    // C# Viewer._time and TestStart (Viewer.cs:109): the game time the --event fires at.
    const float kTestStart = 1.0f;
    float testTime = 0.0f;
    // SniperTest's _entered (the game time sniper mode was entered; < 0: not yet).
    float sniperTestEntered = -1.0f;
    // THE FRAME'S VIEW (C# Viewer.SceneView): the follow camera's, or in sniper mode
    // the scope's from Kurt's eye (FollowCamera.SniperView). `camera` stays the
    // follow camera's state -- it is what follow.update places every step, sniping
    // or not -- and this is what the frame is drawn and heard through.
    mdk::Camera sceneCamera = camera;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        // A golden capture MUST NOT depend on how long the host took between
        // frames. --fixed-step advances exactly the C#'s 1/60-second test step
        // every loop, so the same capture frame is the same simulation state.
        const float elapsed = fixedStep ? kStep
                                        : std::chrono::duration<float>(now - last).count();
        last = now;

        if (!headless && !mdk::windowPump(&window, &input)) {
            break;
        }
        // The scripted walk OWNS the actions (game/walk_script.h): after the
        // platform's poll, before the game reads the input.
        if (hasWalk) {
            mdk::applyWalk(walk, frame, &input);
            const int32_t started = mdk::walkSegmentStartingAt(walk, frame);
            if (started >= 0) {
                char keys[96];
                mdk::formatWalkKeys(walk.segments[started].keys, keys, sizeof(keys));
                std::printf("[walk] frame %u: segment %d %s@%u-%u starts at feet (%.1f,%.1f,%.1f) "
                            "yaw %.1f\n",
                            frame, (int)started + 1, keys, walk.segments[started].first,
                            walk.segments[started].end, player.feet.x, player.feet.y,
                            player.feet.z, player.yaw);
            }
        }

        // THE PAUSE MENU (C# Viewer.Frame, Viewer.cs:367-394): while it is open the
        // game waits (no simulation, no look), and Esc opens it -- that frame waits
        // too. Its keys are the menus' (menu_screens.h); --press reaches it as well.
        mdk::MenuKeyState menuKeyState;
        mdk::windowTakeMenuKeys(&menuKeyState);
        mdk::menu::Keys keys;
        keys.up = menuKeyState.up;
        keys.down = menuKeyState.down;
        keys.left = menuKeyState.left;
        keys.right = menuKeyState.right;
        keys.accept = menuKeyState.accept;
        keys.back = menuKeyState.back;
        keys.leftHeld = menuKeyState.leftHeld;
        keys.rightHeld = menuKeyState.rightHeld;
        keys.any = menuKeyState.any;
        pressTime += elapsed;
        presses.apply(pressTime, &keys);
        bool pausedFrame = false;
        mdk::flow::Event pauseNext = mdk::flow::Event::None;
        // THE FULL-SCREEN STRIKE (package 14, game/strike_view.h): while it plays, Esc
        // skips it instead of opening the pause menu (Viewer.cs:389-396).
        const bool striking = runtime && runtime->strikeScene() != nullptr &&
                              runtime->strikeScene()->active();
        if (pause.open()) {
            pauseNext = pause.update(keys, elapsed);
            pausedFrame = true;
            input.takeMouseX();
            input.takeMouseY();
            if (!pause.open() && !headless) {
                mdk::windowCaptureMouse(&window, true);
            }
        } else if (keys.back && !prompt && !striking && playerReady && hudReady && hudBig->ready()) {
            mdk::menu::MenuItems& items = pause.items();
            items.setFonts(&hudBig->font(), &hudSmall->font());
            items.setHost(mdk::menu::Host::Console);
            items.setSettings(&settings, PauseApply::applied, &pauseApply);
            items.setPrint([](void*, const char* line) { std::printf("%s\n", line); }, nullptr);
            pause.show();
            pausedFrame = true;
            input.takeMouseX();
            input.takeMouseY();
            if (!headless) {
                mdk::windowCaptureMouse(&window, false);
            }
        }
        if (pauseNext == mdk::flow::Event::Quit) {
            flow.handle(pauseNext);
            exitReason = "pause menu Quit";
            break;
        }
        if (pauseNext == mdk::flow::Event::Menu) {
            // C# Game.Handle(Event.Menu): the level is left for a new main menu.
            screen = flow.handle(pauseNext);
            std::printf("[main] flow: pause menu: the main menu\n");
            toMenu = true;
            break;
        }
        // THE SAVE KEYS (package #19; C# Viewer.Frame :369-445). While F2's prompt is
        // open the game waits and the prompt takes the keys; otherwise F2 opens it
        // (OpenSnapshot) and F9 loads (QuickLoad).
        {
            std::string typed;
            uint32_t typing = 0;
            mdk::windowTakeTyping(&typed, &typing);
            if (hasTypeTest && firstLevel && !typeTestDone && testTime >= typeTestTime) {
                typed += typeTestText;
                typeTestDone = true;
                std::printf("[main] --type: \"%s\" typed at %.2f s\n", typeTestText.c_str(),
                            testTime);
            }
            // C# Viewer.TypeCheats (Viewer.cs:431, 862-882): the letters typed this frame,
            // unless the prompt, the pause menu or the full-screen strike holds the game
            // (Viewer.Frame returns before it on those frames, and on the one Esc opens
            // the menu).
            if (!prompt && !pausedFrame && !striking) {
                const std::string letters = mdk::flow::cheatLetters(typed);
                for (size_t c = 0; c < letters.size(); c++) {
                    const mdk::flow::Cheats::Cheat cheat = cheats.type(letters[c]);
                    if (cheat == mdk::flow::Cheats::Gore) {
                        // `_scripts.Option = _ui.Settings.Gore ? 1 : 0`, then the line.
                        if (runtime) {
                            runtime->option = settings.gore ? 1 : 0;
                        }
                        std::printf("Gore %s\n", settings.gore ? "on" : "off");
                    } else if (cheat == mdk::flow::Cheats::DebugKeys) {
                        std::printf("Debug keys %s\n", flow.state().debugKeys ? "on" : "off");
                    }
                }
            }
            if (prompt) {
                mdk::flow::SavePrompt::Input promptKeys;
                promptKeys.accept = (typing & (mdk::kPressEnter | mdk::kPressSpace)) != 0;
                promptKeys.back = (typing & mdk::kPressEscape) != 0;
                promptKeys.up = (typing & mdk::kPressUp) != 0;
                promptKeys.down = (typing & mdk::kPressDown) != 0;
                promptKeys.left = (typing & mdk::kPressLeft) != 0;
                promptKeys.right = (typing & mdk::kPressRight) != 0;
                promptKeys.home = (typing & mdk::kPressHome) != 0;
                promptKeys.end = (typing & mdk::kPressEnd) != 0;
                promptKeys.backspace = (typing & mdk::kPressBackspace) != 0;
                promptKeys.del = (typing & mdk::kPressDelete) != 0;
                promptKeys.typed = typed;
                prompt->update(promptKeys, elapsed);
                if (!prompt->error().empty() && promptKeys.accept) {
                    std::printf("[main] save: not written: %s\n", prompt->error().c_str());
                }
                if (prompt->closed()) {
                    // AskSnapshot: a save made is this session's quick slot.
                    if (prompt->saved()) {
                        std::printf("[main] Saved game %s: level %d\n", prompt->name().c_str(),
                                    (int)prompt->save().level);
                        flow.state().quickSlot = prompt->name();
                        notify("Game saved");
                    }
                    prompt.reset();
                    // The text input stays on: the C# starts it with the window and never
                    // stops it (Window.cs:71-75), because the cheats are typed in play.
                    mdk::windowCaptureMouse(&window, true);
                    pending = 0.0f;
                }
            } else if (playerReady && !pausedFrame && (typing & mdk::kPressQuickSave)) {
                // OpenSnapshot (:763-776): the game stops and the name is asked, the
                // level's number offered; the level is captured NOW.
                if (!runtime || !runtime->canSnapshot() || player.sniping()) {
                    notify("Can't save now");
                } else {
                    const std::string json = runtime->capture();
                    std::printf("[main] snapshot hash %s, objects %d\n",
                                mdk::scripts::ScriptRuntime::snapshotHash(json).c_str(),
                                (int)runtime->objectCount());
                    prompt.reset(new mdk::flow::SavePrompt(
                        saves,
                        mdk::flow::snapshotSave(&flow.state(), levelNumber, player.health(), json),
                        mdk::flow::snapshotName(levelNumber)));
                    mdk::windowCaptureMouse(&window, false);
                    mdk::windowTextInput(&window, true);
                }
            } else if (playerReady && !pausedFrame && (typing & mdk::kPressQuickLoad)) {
                mdk::flow::Event next = mdk::flow::Event::None;
                std::string log;
                if (!mdk::flow::quickLoad(saves, &flow.state(), &next, &log)) {
                    notify("Nothing to load");
                } else {
                    std::printf("[main] %s\n", log.c_str());
                    leaveLevel = true;
                    leaveEvent = next;
                }
            }
        }

        // The strike advances (Esc skips it) and, while it plays, the game waits and the
        // frame is the sky and its model alone (SniperView.DrawStrike, Viewer.cs:396-410).
        const bool strikeFrame =
            !pausedFrame && !prompt &&
            mdk::advanceStrikeScene(runtime.get(), striking && keys.back, elapsed);
        if (strikeFrame) {
            mdk::buildStrikeCamera(*runtime->strikeScene(), kAspect, &camera);
            mdk_render_set_camera(renderer, &camera);
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF0F1218u;
        params.present = true;
        const bool captureThisFrame = captureFrame > 0 && static_cast<int>(frame) + 1 == captureFrame;
        params.screenshot_path = captureThisFrame && !capturePath.empty()
                                     ? capturePath.c_str()
                                     : nullptr;

        if (playerReady && !pausedFrame && !prompt && !leaveLevel && !strikeFrame) {
            // The C# frame order (Viewer.Frame): the mouse look is per frame
            // (Kurt's yaw and the camera's look offset), then the simulation runs
            // in fixed steps, and EACH step updates Kurt and then the camera
            // (UpdateKurt then UpdateCamera) — so the pitch smoothing and the
            // clearance (C# Clear/Sight) run once per step, not once per frame.
            // A frame faster than the step runs no step and reuses the camera the
            // last step placed (the C# blends between steps; that is not ported).
            player.mouseLook(input);
            follow.mouseLook(player, input);
            pending += elapsed;
            if (pending > kStep * 10.0f) {
                pending = kStep * 10.0f; // never spiral after a stall
            }
            while (pending >= kStep) {
                // C# Viewer.RunTests (Viewer.cs:664-680), ahead of Kurt's step: the
                // --event fires on the step whose game time first reaches TestStart.
                testTime += kStep;
                if (dieTest && firstLevel &&
                    !(testTime < kTestStart || testTime - kStep >= kTestStart)) {
                    std::printf("[main] --die: Kurt hurt by 1000 at frame %u\n", frame);
                    player.combat().hurt(1000);  // C# Viewer.FatalDamage
                }
                if (hasEvent && (firstLevel || eventEveryLevel) && runtime &&
                    !(testTime < kTestStart || testTime - kStep >= kTestStart)) {
                    std::printf("[main] --event: special_event %d at frame %u\n", (int)eventValue,
                                frame);
                    runtime->specialEvent(
                        &runtime->getArenaState(runtime->currentArena()).controller, eventValue);
                }
                // C# SniperTest.Step (Viewer.cs:507), before Kurt's step: sniper mode
                // once he stands (after SettleTime 0.5 s), then the zoom key and the shot.
                if (sniperTest && (firstLevel || eventEveryLevel)) {
                    if (sniperTestEntered < 0.0f && player.onFloor && testTime >= 0.5f) {
                        player.enterSniper();
                        player.scope().zoom = sniperTestZoom;
                        player.scope().pitch = sniperTestPitch;
                        sniperTestEntered = testTime;
                    }
                    if (sniperTestEntered >= 0.0f) {
                        const float since = testTime - sniperTestEntered;
                        input.down[mdk::kKeyZoomIn] = since < sniperTestHold;
                        input.down[mdk::kKeyFire] =
                            sniperTestFire && since >= 1.0f && since < 1.0f + 0.1f;
                    }
                }
                player.update(input, kStep);
                // The C# Viewer order is Kurt, then the scripts, then the
                // camera (Viewer.cs:505-535). ScriptRuntime keeps its own
                // 1/30-second clock, so this 1/60-second call completes a
                // script tick every second step.
                if (runtime) {
                    runtime->update(kStep);
                }
                if (runtime) {
                    // The bullet holes the step stamped (special_130, package #16): the C#'s
                    // TextureStamped -> MaterialResolver.Refresh (Viewer.cs:263), each changed
                    // texture's GPU copies given its pixels again.
                    const std::vector<const mdk::formats::Texture*>& stamped =
                        runtime->stampedTextures();
                    for (size_t i = 0; i < stamped.size(); i++) {
                        surfaces.refresh(stamped[i]);
                    }
                    runtime->clearStampedTextures();
                }
                // The pitch the camera eases to (Viewer.cs:746-747): a live
                // camera_track request wins; otherwise it is the scripts'
                // current arena's rest pitch. Without the runtime, retain the
                // older geometric-arena stand-in.
                float target = mdk::follow_camera::kDefaultPitch;
                if (runtime) {
                    target = runtime->cameraTrackTicks > 0
                                 ? runtime->cameraTrackPitch
                                 : level.arenaPitch(runtime->currentArena());
                } else {
                    const std::string* here =
                        space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                    if (here) {
                        target = level.arenaPitch(*here);
                    }
                }
                follow.update(&camera, &space, player, target, kStep, kAspect);
                // C# input.ClearMouse() after UpdateCamera (Viewer.cs:518): the wheel's
                // notches and the keys they pressed were this step's.
                input.wheel = 0.0f;
                input.wheelKeys = 0;
                // The music follows Kurt's arena, which the scripts set (C#
                // Viewer.cs:521-525: Enter, then Update(Step), every step, after
                // the camera).
                if (music) {
                    music->enter(runtime->currentArena());
                    music->update(kStep);
                }
                // The cutscene's shot of this step (Viewer.cs:528-531, after the music).
                if (runtime && mdk::CutsceneCamera::active(*runtime)) {
                    cutsceneCamera.update(*runtime);
                }
                // The collision world follows him EVERY step, at the point the C#
                // does it (`_space.SetSolid(_scripts.SolidArenas)`, after
                // UpdateCamera, Viewer.cs:533). The pin is sticky: while `_solid` is
                // non-empty `move()` uses only it (arena_space.cpp:111-124) and
                // `kurts()` returns only it, so a pin left behind is Kurt colliding
                // with a room he has already left.
                //
                // WITH A RUNTIME THIS IS THE ORACLE'S OWN VALUE, not a stand-in:
                // `ScriptRuntime::solidArenas()` is the scripts' DRAWN list — the
                // current arena plus the active second (ScriptRuntime.cs:265-282,
                // UpdateArenas :658-664) — which is what Viewer.cs:533 hands
                // `SetSolid`. It moves when a move crosses a doorway CONNECTION
                // (`CrossedArena`, :791-803) and it does NOT follow the feet, so it
                // holds while Kurt falls out of the world instead of re-pinning below
                // the arena he left. This is the frontend that takes the pinned frame,
                // which is why both shapes this line used to have are gone: batch 99
                // pinned `runtime->currentArena()` — ONE name where the oracle passes
                // two — so walking out through an open doorway dropped the room behind
                // the door, `bottom()` resolved the neighbour holding his feet, and the
                // fall-out death fired into a respawn loop (review-6 F1, named here by
                // review-7 F1). One name that is the wrong one is no better than a stale
                // one; the oracle's value is the LIST, and `arena_space.h` carries the
                // account of what the pin is and why, gated for both frontends by
                // `tests/scripts/frontend_solid_pin_test.sh`.
                //
                // The branch with NO runtime is the asset gate: a level whose CMI, bank
                // or sprites archive is missing runs without scripts, so there is no
                // drawn list to read and the geometric arena under his feet is the
                // stand-in — wrong in both directions (it can re-pin during a fall-out
                // and it carries one name) but better than no pin at all. It is probed
                // AGAIN rather than reusing the pitch's `here`, because the C# evaluates
                // SetSolid after UpdateCamera and the camera's clearance can move Kurt
                // into another arena. It is deliberately NOT cleared when he is outside
                // every arena: that is the fall-out case, and `bottom()` still has to
                // resolve an arena for the death to fire at all.
                if (runtime) {
                    space.setSolid(runtime->solidArenas());
                } else {
                    const std::string* solid =
                        space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                    if (solid) {
                        space.setSolid({*solid});
                    }
                }
                // The C# advances the HUD blink once per 30 Hz boundary
                // (Viewer.cs:535-538), not once per 60 Hz frame.
                hudPending += kStep;
                if (hudPending >= kHudStep) {
                    hud.tick();
                    hudPending -= kHudStep;
                }
                pending -= kStep;
            }
            // Death: Player::update stays in updateDeath until the skull has faded in,
            // and when the beat ends the level is left for the flow (below).
            //
            // Scripts keep running during the beat HERE AND IN THE ORACLE: the C#
            // play tick calls _scripts.Update(Step) with no Dead guard
            // (Viewer.cs:509-512), so this is not a divergence. What does differ is what
            // happens when the beat ENDS. In the oracle the skull fade is Kurt's own
            // HurtFlash, rising at SkullFade = 2*Ticks = 60/s from the zero the death
            // transition itself set (Kurt.Combat.cs:238-253 and :375), and when it passes
            // SkullEnd the Died event fires: Game.cs:448-456 writes SaveGames.LastGame and
            // leaves the level for the menu. player.died()
            // is that event, not a copy of its clock: the beat is the oracle's own count,
            // 255 steps of simulation to reach SkullEnd and the next rising step to pass it
            // (256 rising steps total), not a wall-clock timer of the frontend's, so a stalled or a
            // faster-than-real-time frame cannot make it longer or shorter than the C#'s.
            // Package #19 ported that flow: the death is counted (Viewer.OnDied), the
            // flow writes LASTGAME and goes to the menu, whose "Continue" -- taken for
            // this frontend, which draws no menu (GameFlow::skipScreen) -- reloads the
            // level from its start through the level loop, as the C#'s Play does.
            if (player.died() && !leaveLevel) {
                std::printf("[main] Kurt died\n");
                mdk::flow::countDeath(&flow.state(), levelNumber,
                                      runtime ? runtime->airStrike().usedUp : flow.state().strikeUsed);
                leaveLevel = true;
                leaveEvent = mdk::flow::Event::KurtDied;
            }
            // The end of a level: special_event <= 50 started it (the log says where),
            // EndLevel finished it (levelEnded), and the frontend leaves EndDelay later.
            if (runtime && runtime->endLevel() && !endSeen) {
                endSeen = true;
                std::printf("[main] level: special_event ended the level in %s at frame %u; "
                            "the arena breaks up and Kurt rises\n",
                            runtime->endLevel()->arena().c_str(), frame);
            }
            if (runtime && runtime->levelEnded() && !endingStarted) {
                endingStarted = true;
                endingDelay = kEndDelay;
                std::printf("[main] level: ended (LevelEnded) at frame %u; leaving in %.1f s\n",
                            frame, (double)kEndDelay);
            }
            if (endingStarted && !levelEndedExit) {
                endingDelay -= elapsed;
                levelEndedExit = endingDelay <= 0.0f;
            }
            // The end of the game (special_event 81): the C#'s GameFinished, at once
            // (Viewer.cs:241, no delay), acted on after the frame like the level's end.
            if (runtime && runtime->gameFinished() && !gameFinishedExit) {
                gameFinishedExit = true;
                std::printf("[main] game: finished (special_event 81, GameFinished) at frame "
                            "%u\n",
                            frame);
            }
            // A CUTSCENE HOLDS THE CAMERA (package 13): the C# Viewer draws from its
            // CutsceneCamera while CutsceneCamera.Active (Viewer.SceneView/Eye, :728/:739),
            // blended between the scripts' ticks (Viewer.BlendFrame, :614-617; a fixed step
            // shows the tick's own shot, as it does the objects). The follow camera keeps its
            // own state and rewrites `camera` on the next step, so nothing is left behind
            // when the cutscene ends.
            if (runtime && mdk::CutsceneCamera::active(*runtime)) {
                cutsceneCamera.blend(fixedStep ? 1.0f : runtime->tick_blend(pending));
                cutsceneCamera.build(&camera, kAspect);
            }
            // Sniper mode before the cutscene (C# Viewer.SceneView: flying, then
            // Sniping, then CutsceneCamera.Active).
            sceneCamera = camera;
            if (player.sniping()) {
                mdk::buildSniperCamera(&sceneCamera, player, kAspect);
            }
            mdk_render_set_camera(renderer, &sceneCamera);
            // The walk's route evidence: Kurt's arena (the scripts' current one,
            // which the 3DS's arena-entry table keys on) on every change, and his
            // feet every 30 frames.
            if (hasWalk) {
                static std::string walkArena;
                static std::string walkSecond;
                const std::string current = runtime ? runtime->currentArena() : std::string();
                const std::string second = runtime ? runtime->secondArena() : std::string();
                if (current != walkArena || second != walkSecond) {
                    std::printf("[walk] frame %u: arena %s -> %s (second %s%s) feet "
                                "(%.1f,%.1f,%.1f)\n",
                                frame, walkArena.empty() ? "-" : walkArena.c_str(),
                                current.empty() ? "-" : current.c_str(),
                                second.empty() ? "-" : second.c_str(),
                                runtime && runtime->secondActive() ? " active" : "",
                                player.feet.x, player.feet.y, player.feet.z);
                    walkArena = current;
                    walkSecond = second;
                }
                if (frame % 30u == 0u) {
                    std::printf("[walk] frame %u: feet (%.1f,%.1f,%.1f) yaw %.1f arena %s\n", frame,
                                player.feet.x, player.feet.y, player.feet.z, player.yaw,
                                current.empty() ? "-" : current.c_str());
                }
            }
        }

        // The frame's light (LevelLight.Update): the scripts' current
        // arena's rig, or the start arena's before the runtime exists.
        // Point lights gather with the LightSources slice; none yet.
        if (enhancedLook) {
            const mdk::formats::Arena* lightArena = arena;
            if (runtime) {
                if (const mdk::formats::Arena* current =
                        level.arenaNamed(runtime->currentArena())) {
                    lightArena = current;
                }
            }
            if (lightArena) {
                const mdk::Lighting rig = mdk::enhancedLighting(level.dti(), *lightArena);
                mdk_render_set_lighting(renderer, &rig);
            }
        }

        // THE MIX, once per frame, after the steps that moved the listener. The C#
        // does it in two places that are one frame apart: the mixer's own Update
        // runs in DrawScene right after the listener is taken from the camera
        // (Viewer.cs:686-696), and the device's Update runs after the screen's
        // frame (Game.cs:334-337). Both get the frame's elapsed, not the step's.
        if (runtime) {
            mdk::audio::SoundMixer& mixer = runtime->mixer();
            // C# Viewer.DrawScene (Viewer.cs:686-693): the listener stands at the
            // frame's view (the scope's eye in sniper mode) with the follow camera's
            // right, and in sniper mode the sounds are heard through the scope:
            // ScopeZoom, along Kurt's line of sight with its up.
            mixer.listenFromCamera(camera);
            mixer.listenerPosition = sceneCamera.position;
            mixer.scopeZoom = player.sniping() ? player.scope().zoom : 0.0f;
            mixer.listenerForward = player.sniperForward();
            mixer.listenerUp = mdk::followCameraUpOf(mixer.listenerForward);
            mixer.update(elapsed);
            mixer.device().update(elapsed);
        }

        // THE ARENAS IN MEMORY AND ON THE GPU FOLLOW THE SCRIPTS, once per frame,
        // after the steps and before anything is drawn (game/arena_stream.h,
        // game/arena_views.h): the stream first, so an arena drawn this frame has
        // its textures' pixels, then the views.
        if (arena) {
            const std::string& streamCurrent =
                (runtime && !runtime->currentArena().empty()) ? runtime->currentArena()
                                                              : startArenaName;
            const std::string streamSecond = runtime ? runtime->secondArena() : std::string();
            const bool streamActive = runtime && runtime->secondActive();
            // WHAT THIS FRAME DRAWS: the scripts' drawn list (C# Viewer.cs:702,
            // LevelView.Draw(DrawnArenas)), or Kurt's arena until their first
            // tick has named one. ONE list for the stream, the views and the
            // draw below: the list is the middle of the scripts' tick and the
            // pair above is its end, and after a scripted teleport they differ
            // for a frame.
            const bool scriptsDrew = runtime && !runtime->drawnArenas().empty() &&
                                     !runtime->drawnArenas()[0].empty();
            if (!scriptsDrew) {
                startDrawn.assign(1, startArenaName);
            }
            frameDrawn = scriptsDrew ? &runtime->drawnArenas() : &startDrawn;
            mdk::ArenaStream& stream = level.stream();
            const mdk::ArenaStream::Stats before = stream.stats();
            if (before.demandLoads != lastDemandLoads) {
                std::printf("[main] stream: %d demand load(s) last frame, %lld bytes in all\n",
                            (int)(before.demandLoads - lastDemandLoads),
                            (long long)before.demandBytes);
                lastDemandLoads = before.demandLoads;
            }
            stream.update(streamCurrent, streamSecond, streamActive, *frameDrawn);
            const int32_t releasedBefore = arenaViews.stats().released;
            const int32_t builtViews = arenaViews.sync(streamCurrent, streamSecond, *frameDrawn);
            const mdk::ArenaStream::Stats& after = stream.stats();
            if (after.switches != before.switches) {
                std::printf("[main] stream: switch to %s (cur=%s second=%s%s)\n",
                            stream.resident().c_str(), streamCurrent.c_str(),
                            streamSecond.c_str(), streamActive ? " active" : "");
            }
            if (after.stalls != before.stalls) {
                std::printf("[main] stream: STALL %s shown before its stream was done: %lld bytes\n",
                            stream.resident().c_str(),
                            (long long)(after.stallBytes - before.stallBytes));
            }
            if (after.readFailures != before.readFailures) {
                std::printf("[main] stream: read FAILED for %s (%d failures)\n",
                            stream.resident().c_str(), (int)after.readFailures);
            }
            if (builtViews > 0 || arenaViews.stats().released != releasedBefore) {
                std::printf("[main] views: %d built (last %s), %d released, %d kept\n",
                            (int)builtViews, arenaViews.lastBuilt().c_str(),
                            (int)(arenaViews.stats().released - releasedBefore),
                            (int)arenaViews.count());
            }
            // The groups the steps retextured (group_set_texture, opcode 140; package
            // #15): the C#'s TriangleGroups.Changed -> LevelView.Rebuild, each arena's
            // view built again from its groups' materials.
            if (runtime) {
                const std::vector<std::string>& retextured = runtime->retexturedArenas();
                for (size_t i = 0; i < retextured.size(); i++) {
                    const bool rebuilt = arenaViews.refresh(retextured[i]);
                    std::printf("[main] views: %s retextured, %s\n", retextured[i].c_str(),
                                rebuilt ? "rebuilt" : "its mesh was REFUSED");
                }
                runtime->clearRetexturedArenas();
            }

            // THE MUSIC OF THE ARENA THAT IS LOADING (docs/ARENA_STREAMING_SPEC.md
            // Phase 5; the original's arena_load -> 0x4192c4, a step a frame at
            // 0x4193a4): the pair's other arena is where a door or a corridor
            // leads, so its track is asked for when that arena changes and read
            // a step per frame. The same calls as the 3DS frontend's; this
            // frontend preloads its bank (as the C# does), so here they find the
            // track decoded and do nothing.
            if (music) {
                if (streamSecond != musicPrefetchedFor) {
                    musicPrefetchedFor = streamSecond;
                    if (!streamSecond.empty()) {
                        const std::string track = music->trackOf(streamSecond);
                        if (bank.prefetch(track)) {
                            std::printf("[main] music: prefetch %s for %s\n", track.c_str(),
                                        streamSecond.c_str());
                        }
                    }
                }
                bank.prefetchStep();
            }
        }

        // The background the scripts want (C# Viewer.Background, Viewer.cs:1105; package
        // #15): the sky mode (opcode 202) picks the backdrop, and the clear colour is the
        // DTI's sky colour, black in mode 1. Without scripts the mode is the C#'s 0.
        if (arena) {
            const int32_t skyMode = runtime ? runtime->skyMode : mdk::sky_modes::kShown;
            params.backdrop = mdk::sky_modes::backdropOf(skyMode);
            params.clear_colour =
                mdk::sky_modes::clearOf(skyMode, mdk::sky_modes::skyColour(level.dti()));
            // Said when it changes (the C# prints `sky {mode} ({backdrop})`, Viewer.cs:543):
            // the retail arenas that black or hide the sky are closed rooms, so the log is
            // where a run shows it.
            static int32_t loggedSkyMode = mdk::sky_modes::kShown;
            if (skyMode != loggedSkyMode) {
                static const char* const kBackdropNames[] = {"Sky", "Clear", "Keep"};
                std::printf("[main] sky: mode %d (%s) at frame %u\n", (int)skyMode,
                            kBackdropNames[(int)params.backdrop], frame);
                loggedSkyMode = skyMode;
            }
        }
        mdk_render_begin_frame(renderer, &params);
        // The arenas the scripts say are drawn: Kurt's and the active second
        // (C# Viewer.cs:702, LevelView.Draw(DrawnArenas)); the start arena until
        // the runtime's first tick has named one.
        if (arena && frameDrawn && !strikeFrame) {
            // The end of a level's torn triangles leave the arena's mesh first (the C#'s
            // LevelView.DrawEnd, Viewer.cs:703): its view swaps to a dynamic mesh and
            // rewrites it in this frame. The flying pieces are drawn by the effects pass.
            if (runtime && runtime->endLevel() &&
                !arenaViews.tear(runtime->endLevel()->arena(), runtime->endLevel()->torn())) {
                if (!tearRefusedLogged) {
                    tearRefusedLogged = true;
                    std::printf("[main] level: the torn arena's dynamic mesh was REFUSED (%s); "
                                "it is drawn whole, the pieces still fly\n",
                                runtime->endLevel()->arena().c_str());
                }
            }
            arenaViews.draw(*frameDrawn);
        }
        if (runtime) {
            // C# Viewer.DrawScene draws the objects after the arena and before
            // Kurt's sprite: every object the scripts hold, in the order of its
            // list, skipping the invisible ones -- and the ones the scripts have
            // REMOVED, which the C# has taken out of its Objects list and the C++
            // runtime keeps in its deque because other objects hold pointers
            // into it.
            //
            // The transform is the blended one. The scripts tick at 1/30 s and this
            // loop presents as fast as it can, so most frames draw an object part
            // of the way from its last tick to its current one: the C# hands
            // ObjectView.Draw `blend.TransformOf(obj)` (Viewer.cs:959), which is
            // `obj.TransformAt(tick)` for every object but the ride that follows
            // Kurt, and `tick` is the frame's leftover time over a tick
            // (ScriptRuntime.TickBlend, Viewer.cs:607). A fixed-step capture has no
            // leftover and the oracle turns blending off there (Viewer.cs:280-282),
            // so alpha 1 -- which transform_at answers with the raw transform -- is
            // the capture's own picture, byte for byte.
            //
            // The other half of TransformOf: `obj == _driven` (the XD2 Kurt rides,
            // package #9) is drawn at its own transform shifted by Kurt's lag between
            // two steps. This port draws Kurt at Player::feet (FrameBlend's Kurt pose is
            // not ported), so his lag is zero and the driven object is drawn at its
            // current transform -- alpha 1 -- where Rides::follow put it after the
            // step, under the camera that follows the same feet. The XE Kurt flies
            // (package #11) is the C#'s Carrier (Viewer.cs:588): the oracle draws Kurt
            // and its camera blended with the XE, and here Rides::follow puts his feet
            // -- and so LookDown's camera -- on the XE's current place each step, so the
            // XE is drawn there too (alpha 1), or it would lag under the camera.
            const float tickAlpha = fixedStep ? 1.0f : runtime->tick_blend(pending);
            objectStream.reset();
            for (const mdk::scripts::MdkObject& obj : runtime->objects()) {
                if (strikeFrame) {
                    break;  // the strike's frame shows its model alone (below)
                }
                if (!obj.visible || runtime->isRemoved(&obj)) {
                    continue;
                }
                const bool carried = runtime->rides().bomber() && &obj == runtime->rides().ridden();
                const float alpha =
                    &obj == runtime->rides().driven() || carried ? 1.0f : tickAlpha;
                objectView.addObject(obj, objectLook(obj.arena), obj.transform_at(alpha),
                                     &objectStream);
            }
            if (strikeFrame) {
                // THE FULL-SCREEN STRIKE (package 14): the scene's model alone over the sky
                // (SniperView.DrawStrike: objects.Draw of its Subject, posed by its frame and
                // turned by its yaw), through this same pass -- which sets every GPU state
                // it needs per draw.
                const mdk::scripts::MdkObject& subject = runtime->strikeScene()->subject();
                objectView.addObject(subject, objectLook(subject.arena), subject.transform(),
                                     &objectStream);
            }
            // C# SniperView.Draw's first loop (Viewer.cs:706; not in the strike's
            // frame, which DrawStrike draws instead): the sniper rounds in
            // flight (SniperRounds.Visuals), each in its arena's look, at its own
            // transform -- they are not in the object list and do not blend.
            for (int32_t i = 0; i < mdk::scripts::SniperRounds::kSlots && !strikeFrame; i++) {
                if (runtime->sniperRounds().visualFlying(i)) {
                    const mdk::scripts::MdkObject& round = runtime->sniperRounds().visual(i);
                    objectView.addObject(round, objectLook(round.arena), round.transform(),
                                         &objectStream);
                }
            }
            objectDraw.draw(renderer, &surfaces, objectStream);
            // A REFUSED STREAM MESH IS A SILENTLY EMPTY OBJECT PASS UNLESS SOMEONE
            // PRINTS IT (review-11 F4, the half `object_draw.h` records as owed to
            // this frontend). `ObjectDraw` cannot print: `mdk_core` has no log, and
            // inventing one there would put a frontend's stderr inside the shared
            // game code. So the pass RECORDS the refusal and the frontend that owns
            // the log prints it — which is what `main_3ds.cpp` has done since
            // review-10 F3 and what this frontend did NOT do, so on Linux a level
            // whose stream mesh was refused lost every scripted object and said
            // nothing at all.
            //
            // Edge-triggered and RE-ARMED, in the 3DS frontend's shape: the flag
            // clears when a later attempt succeeds, so a second refusal after a
            // recovery still prints (review-11 F7), and the clear sits in the
            // `else` sibling of the `allocationFailed()` test so it runs on every
            // frame that did not refuse rather than once at the end of the branch.
            //
            // THE CAPACITY THAT WAS REFUSED, not the frame's corner count: `ensure`
            // rounds up to a power of two and that rounded capacity is what the
            // backend was asked for, so the frame's own count would report a
            // smaller refusal than the one that happened (review-11 F1). The
            // backend's own `[vk] REFUSED` line now prints the same request in
            // bytes, so the two lines agree by construction.
            if (objectDraw.allocationFailed()) {
                if (!streamFailureLogged) {
                    streamFailureLogged = true;
                    const int32_t attempted = objectDraw.attemptedCapacity();
                    std::printf("[main] objects: stream mesh of %d corners = %d KB "
                                "REFUSED by the backend — no objects drawn this frame\n",
                                (int)attempted,
                                (int)((int64_t)attempted * (int64_t)sizeof(mdk::Vertex) /
                                      1024));
                }
            } else {
                streamFailureLogged = false;
            }
        }
        if (playerReady && !strikeFrame) {
            // The rest of C# SniperView.Draw (Viewer.cs:706): the overlay's state, the
            // round cameras' insets and the loaded rounds' inset, between the objects
            // and the effects.
            sniperView.draw(renderer, hud.sniper(), player, runtime.get(), elapsed, true,
                            objectView, &surfaces,
                            objectLook(runtime ? runtime->currentArena() : std::string()));
            if (sniperView.allocationFailed()) {
                if (!sniperClipFailureLogged) {
                    sniperClipFailureLogged = true;
                    std::printf("[main] sniper: the clip's stream mesh was REFUSED by the "
                                "backend -- the loaded rounds are not drawn\n");
                }
            } else {
                sniperClipFailureLogged = false;
            }
        }
        if (runtime && !strikeFrame) {
            // The pieces and sprite effects, where the C# draws them:
            // after the objects, before Kurt (Viewer.cs:707).
            effectsView.build(*runtime, sceneCamera, &effectsFrame);
            effectsDraw.draw(renderer, &surfaces, effectsFrame);
            if (effectsDraw.allocationFailed()) {
                if (!effectsFailureLogged) {
                    effectsFailureLogged = true;
                    std::printf("[main] effects: mesh of %d corners REFUSED by the "
                                "backend — no pieces or effects drawn this frame\n",
                                (int)effectsDraw.attemptedCapacity());
                }
            } else {
                effectsFailureLogged = false;
            }
        }
        if (kurtSpriteReady && playerReady && !strikeFrame) {
            // C# Viewer.DrawScene draws Kurt after the level, objects and
            // effects, and before the HUD.
            kurtSprite.draw(renderer, player, player.feet, camera.position, camera.forward,
                            camera.up, mdk::follow_camera::kFieldOfView, &space);
        }
        if (runtime && !strikeFrame) {
            // Viewer.cs:699: the scripts' Eye is the camera (an explosion faces it) --
            // the frame's view, the scope's in sniper mode; the strike's frames are
            // not DrawScene's, so they leave it alone.
            runtime->eye = sceneCamera.position;
        }
        if (hudReady && !strikeFrame) {
            // The health, skull and its fade are Kurt's now, and the target bar is the
            // chain gun's (the scripts' GetBar). The C# starts with an empty
            // inventory; its writer lands with the pickup subsystem, so the HUD must
            // not invent it.
            //
            // Both flashes are read off the combat half, exactly as Viewer.HudStateOf
            // passes kurt.HurtFlash and kurt.WhiteFlash (Viewer.cs:913). The frontend
            // used to fake the skull with a timer of its own because nothing else rose
            // or faded it; that copy is gone, so the skull's rate and its end are the
            // oracle's constants in mdk::Kurt and there is no second beat to keep in
            // step, and the red damage flash and opcode 215's white one now reach the
            // screen at all -- which is what the fade in Player::update is for.
            mdk::HudState state;
            state.health = player.health();
            state.dead = player.dead;
            state.hurtFlash = player.combat().hurtFlash();
            state.whiteFlash = player.combat().whiteFlash();
            // The inventory (Viewer.HudStateOf, Viewer.cs:905-914): the slots as (item, count),
            // the selected slot and the super chain gun's ticks left.
            const mdk::Kurt& inventory = player.combat();
            for (int32_t i = 0; i < inventory.slotCount() && i < mdk::HudState::kMaxSlots; i++) {
                state.slots[i] = std::make_pair((int32_t)inventory.slots()[i].item,
                                                inventory.slots()[i].count);
                state.slotCount = i + 1;
            }
            state.selected = inventory.selected();
            state.superChainGun = inventory.superChainGun();
            // The target's health bar while the chain gun hits it (Viewer.cs:912, GetBar).
            if (runtime) {
                const mdk::scripts::ScriptRuntime::BarValues bar = runtime->bar();
                state.barHealth = bar.health;
                state.barMax = bar.max;
                // The XE bomber's sight (Viewer.cs:914, Rides.Bomber?.Shown; package #11).
                mdk::scripts::Bomber::Sight sight;
                const mdk::scripts::Bomber* bomber = runtime->rides().bomberRide();
                if (bomber != nullptr && bomber->shown(&sight)) {
                    state.bomber = true;
                    state.bomberCursorX = sight.cursorX;
                    state.bomberCursorY = sight.cursorY;
                    state.bomberBombs = sight.bombs;
                }
            }
            // Viewer.DrawScene: the messages advance by the frame's elapsed time, then
            // the HUD draws them last.
            hud.messages().update(elapsed);
            mdk_render_canvas_begin(renderer);
            hud.draw(renderer, state);
            if (pause.open()) {
                // The menu over the dimmed game (C# PauseMenu.Update's drawing).
                mdk::ScreenView view;
                view.layoutInside(mdk_render_canvas_width(renderer), mdk::kCanvasHeight);
                pause.draw(renderer, view, *hudBig, *hudSmall);
            }
            if (prompt && hudBig->ready()) {
                // AskSnapshot: F2's prompt on black (SavePrompt.Draw through the
                // prompt's layout), on the 600 x 360 view centred on the canvas.
                const float canvasWidth = mdk_render_canvas_width(renderer);
                mdk::CanvasQuad black;
                black.x0 = 0.0f;
                black.y0 = 0.0f;
                black.x1 = canvasWidth;
                black.y1 = 0.0f;
                black.x2 = canvasWidth;
                black.y2 = 360.0f;
                black.x3 = 0.0f;
                black.y3 = 360.0f;
                black.colour = mdk::Colour{0.0f, 0.0f, 0.0f, 1.0f};
                mdk_render_canvas_quad(renderer, &black);
                struct PromptFont {
                    const mdk::FontView* font;
                    const std::vector<std::pair<std::string, std::vector<uint8_t>>>* texts;
                    const std::vector<uint8_t>* text(const std::string& key) const {
                        for (size_t k = 0; k < texts->size(); k++) {
                            if ((*texts)[k].first == key) {
                                return &(*texts)[k].second;
                            }
                        }
                        return nullptr;
                    }
                };
                PromptFont font = {hudBig.get(), &promptTexts};
                std::vector<mdk::flow::SavePrompt::Run> runs;
                prompt->layout(
                    [](void* context, const std::string& text, bool key) -> float {
                        const PromptFont* f = static_cast<const PromptFont*>(context);
                        const std::vector<uint8_t>* bytes = key ? f->text(text) : nullptr;
                        return key ? (bytes ? (float)f->font->width(bytes->data(),
                                                                    (int32_t)bytes->size())
                                            : 0.0f)
                                   : (float)f->font->width(text.c_str());
                    },
                    &font, &runs);
                const float originX = (canvasWidth - mdk::flow::SavePrompt::kViewWidth) * 0.5f;
                const mdk::Colour white{1.0f, 1.0f, 1.0f, 1.0f};
                for (size_t r = 0; r < runs.size(); r++) {
                    const std::vector<uint8_t>* bytes = runs[r].key ? font.text(runs[r].text) : nullptr;
                    if (runs[r].key && bytes) {
                        hudBig->draw(renderer, bytes->data(), (int32_t)bytes->size(),
                                     originX + runs[r].x, runs[r].y, runs[r].scale, white);
                    } else if (!runs[r].key) {
                        hudBig->draw(renderer, runs[r].text.c_str(), originX + runs[r].x,
                                     runs[r].y, runs[r].scale, white);
                    }
                }
            }
            mdk_render_canvas_end(renderer);
        }
        mdk_render_end_frame(renderer);

        frame++;
        if (captureThisFrame) {
            break;
        }
        if (maxFrames > 0 && (int)frame >= maxFrames) {
            break;
        }
        // THE LEVEL-END HOOK (package 2). Game.cs takes Event.LevelEnded to AfterLevel
        // (:442, :499-512): the level's counts and Kurt's health are kept (the C#
        // reads them off `_viewer`), and the flow says what follows. The stream is
        // played (package #18b: the loop's top, after this level's teardown); the
        // statistics, the fall and the save prompt are not ported, so they are skipped
        // the way the C# skips them (GameFlow::skipScreen, pinned against the C# by
        // golden/flow_next_level.txt), each one logged. A level at the end of that
        // walk is loaded by the level loop; anything else (the menu) ends the run.
        if (leaveLevel) {
            // C# Game.Handle(KurtDied) -- LASTGAME written, then the main menu with
            // its splash, whose first item is "Continue" (Game.cs:448-456,
            // MainMenu.ShowMain) -- or the Play/Briefing a quick load set. The menu
            // and the briefing are the menus' screens (package #20a); the screens
            // this port still lacks are skipped the C#'s way.
            mdk::flow::Screen after = flow.handle(leaveEvent);
            printFlowReport();
            mdk::flow::GameFlow::SkipTrail trail;
            if (after != mdk::flow::Screen::Level && after != mdk::flow::Screen::Menu &&
                after != mdk::flow::Screen::Briefing) {
                after = flow.skipUnported(&trail);
                printFlowReport();
            }
            for (int32_t i = 0; i < trail.count; i++) {
                std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                            mdk::flow::GameFlow::screenName(trail.skipped[i]),
                            (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
            }
            if (after == mdk::flow::Screen::Level) {
                nextLevel = true;
                std::printf("[main] flow: level %d (again) at frame %u, deaths %d\n",
                            (int)flow.state().level, frame, (int)flow.state().deaths);
            } else if (after == mdk::flow::Screen::Menu ||
                       after == mdk::flow::Screen::Briefing) {
                std::printf("[main] flow: the %s follows (deaths %d)\n",
                            after == mdk::flow::Screen::Menu ? "menu" : "briefing",
                            (int)flow.state().deaths);
                screen = after;
                toMenu = true;
            } else {
                std::printf("[main] flow: %s follows, which this frontend cannot show; "
                            "exiting\n",
                            mdk::flow::GameFlow::screenName(after));
                exitReason = leaveEvent == mdk::flow::Event::KurtDied ? "Kurt died" : "load";
            }
            break;
        }
        if (levelEndedExit) {
            mdk::flow::LevelResult result;
            if (runtime) {
                result.stats = runtime->stats;
            }
            result.health = player.health();
            flow.reportLevel(result);
            mdk::flow::Screen after = flow.handle(mdk::flow::Event::LevelEnded);
            std::printf("[main] flow: level %d ended (health %d); the flow shows %s\n",
                        levelNumber, (int)result.health,
                        mdk::flow::GameFlow::screenName(after));
            if (after == mdk::flow::Screen::Stream) {
                // The stream is shown (package #18b): the level is torn down first,
                // and the level loop's top plays it.
                nextLevel = true;
                break;
            }
            mdk::flow::GameFlow::SkipTrail trail;
            after = flow.skipUnported(&trail);
            for (int32_t i = 0; i < trail.count; i++) {
                std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                            mdk::flow::GameFlow::screenName(trail.skipped[i]),
                            (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
            }
            // THE STATISTICS (package #20b) are the menus' screens (menu_screens.h);
            // a run made with --skip-statistics takes the C#'s skip of them instead
            // (GameFlow::skipScreen), as the level chain's tests do.
            if (after == mdk::flow::Screen::Statistics && skipStatistics) {
                std::printf("[main] flow: skipped Statistics (--skip-statistics)\n");
                flow.handle(flow.skipScreen());
                after = flow.skipUnported(&trail);
                for (int32_t i = 0; i < trail.count; i++) {
                    std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                                mdk::flow::GameFlow::screenName(trail.skipped[i]),
                                (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
                }
            }
            if (after == mdk::flow::Screen::Statistics) {
                std::printf("[main] flow: the statistics follow\n");
                screen = after;
                toMenu = true;
                break;
            }
            if (after == mdk::flow::Screen::Fall && skipFalls) {
                // --skip-falls: the fall skipped here, the C#'s Esc way, so the run
                // goes on to the level exactly as it did before package #17b.
                std::printf("[main] flow: skipped Fall (--skip-falls)\n");
                after = flow.handle(flow.skipScreen());
            }
            if (after == mdk::flow::Screen::Level && replayLevel) {
                std::printf("[main] flow: --replay-level: level %d again instead of %d\n",
                            levelNumber, (int)flow.state().level);
                flow.state().level = levelNumber;
            }
            if (after == mdk::flow::Screen::Level) {
                nextLevel = true;
                std::printf("[main] flow: next level %d at frame %u, entered at health %d\n",
                            (int)flow.state().level, frame, (int)flow.levelHealth());
            } else if (after == mdk::flow::Screen::Menu || after == mdk::flow::Screen::Fall) {
                // The main menu follows (package #20a): after LEVEL5, a game over, a
                // level outside the order. Or the next level's fall (#17b): the
                // front screens show it, then the level.
                std::printf("[main] flow: the %s follows\n",
                            after == mdk::flow::Screen::Fall ? "fall" : "menu");
                screen = after;
                toMenu = true;
            } else {
                std::printf("[main] flow: %s follows, which this frontend cannot show; "
                            "exiting\n",
                            mdk::flow::GameFlow::screenName(after));
                exitReason = "level ended";
            }
            break;
        }
        // THE GAME-FINISHED HOOK (beside package 2's): the C#'s Game takes
        // Event.GameFinished to the end movie (Game.cs:458, MDKEND.FLC and MDKBZK.MVE, then
        // the menu with the splash). The movies are not this package's, so the run ends
        // through the ordinary teardown.
        if (gameFinishedExit) {
            std::printf("[main] game: finished; the flow shows %s (the end movie is not "
                        "played; exiting)\n",
                        mdk::flow::GameFlow::screenName(
                            flow.handle(mdk::flow::Event::GameFinished)));
            exitReason = "game finished";
            break;
        }
    }

    // THE LEVEL'S TEARDOWN, between frames (the last frame has ended; the next level's
    // first frame has not begun). The arenas' GPU objects first, while their views
    // still know them; then the level block's closing brace destroys the rest in
    // reverse order of declaration -- the music and the runtime (its mixer and every
    // voice), the object and effects passes, Kurt's sprite, the HUD, the bank, the
    // level data; then the renderer's scope releases what the level made through it
    // that no object released (the sky, the HUD and font atlases, the surface cache's
    // textures and palettes), newest first. The Vulkan backend retires each one to
    // its frame-completion boundary (AGENTS.md P3).
    arenaViews.clear();
    if (runtime) {
        // The level's sound, said once: how many voices its mixer started (ids are
        // never reused, so the next id less one) and what reached the stream.
        const mdk::AudioStats sent = mdk::audioStats();
        std::printf("[audio] level %d: %d voices started; %lld frames sent to the stream "
                    "(peak %.3f)\n",
                    levelNumber, (int)(runtime->mixer().device().nextId() - 1),
                    (long long)sent.frames, sent.peak);
    }
    music.reset();
    pauseApply.device = nullptr;   // the runtime's mixer goes next
    pause = mdk::menu::PauseMenu();
    runtime.reset();
    // The shared pose caches key on the runtime's Model addresses, which the reset
    // above freed (scripts/pose_cache.h; the C#'s next Viewer calls ForgetPoses,
    // Viewer.cs:185). Dropped here, not only at the next spawn, so they do not hold
    // the finished level's poses through the load.
    mdk::scripts::MdkObject::forget_poses();
    }   // END OF THE LEVEL: its objects are destroyed here
    mdk_render_release(renderer, levelScope);
    // The sky the renderer was handed is the level's and was just released.
    {
        const mdk::Panorama none;
        mdk_render_set_panorama(renderer, &none);
    }
    // THE LOADING SCREEN'S FRAMES (C# Play: Show(null), LoadingScreen.Show, Show(null),
    // Game.cs:480-483): a few empty frames between the levels -- the clear colour, not
    // game frames, so `frame` does not count them. They are also the backend's
    // completion boundaries: what the release above retired is destroyed once the
    // frames that could still read it have finished, so the memory line after them
    // is the level's teardown complete, not half of it.
    if (nextLevel) {
        for (int i = 0; i < kBetweenLevelFrames; i++) {
            MdkFrameParams blank;
            blank.elapsed = 0.0f;
            blank.frame_index = frame;
            blank.clear_colour = 0xFF000000u;
            blank.present = true;
            blank.screenshot_path = nullptr;
            mdk_render_begin_frame(renderer, &blank);
            mdk_render_end_frame(renderer);
        }
    }
    if (toMenu) {
        // The menu's own frames are the completion boundaries for what the level
        // retired (menu_screens.cpp); the next pass of the loop runs it.
        printMemory("level-teardown", levelNumber, memorySample(renderer));
        firstLevel = false;
        continue;
    }
    if (!nextLevel) {
        // The last level: no frames follow, so what it retired is still waiting for
        // its boundary and the figure is not a settled one (the renderer is destroyed
        // next anyway).
        printMemory("run-end, unsettled", levelNumber, memorySample(renderer));
        break;
    }
    printMemory("level-teardown", levelNumber, memorySample(renderer));
    firstLevel = false;
    }   // END OF THE LEVEL LOOP

    int32_t draws = 0;
    int32_t tris = 0;
    mdk_render_stats(renderer, &draws, &tris);
    std::printf("[main] exiting after %u frames (last frame: %d draws, %d triangles)\n",
                frame, draws, tris);
    if (exitReason != nullptr) {
        std::printf("[main] exit reason: %s\n", exitReason);
    }

    // THE PICTURE THIS RUN WAS ASKED TO LEAVE BEHIND. `--capture` names a file, and
    // the run used to return 0 whether or not that file was ever written: the backend
    // printed `[vk] screenshot: could not open ...`, the bool it returned was
    // discarded at the frame seam, and a capture harness whose output directory did
    // not exist reported a frame it had not taken (review-2 F1). What makes that
    // worse than a crash is that the harness upstream then finds the BMP the PREVIOUS
    // run left at that path, and commits it as this build's frame.
    //
    // The rule is stated against what THIS RUN ASKED FOR and not against what the
    // backend counted, because "the write failed" and "the frame was never reached"
    // are both the same failure from here: no picture. A backend that reports zero
    // writes for a request it cannot honour -- the 3DS has no readback at all --
    // fails the same test, which is the point of asking the renderer instead of
    // trusting a log line nobody is reading.
    int32_t captureRequested = 0;
    int32_t captureWritten = 0;
    mdk_render_capture_stats(renderer, &captureRequested, &captureWritten);
    int status = 0;
    if (captureFrame > 0 && captureWritten < 1) {
        std::fprintf(stderr,
                     "[main] capture FAILED: --capture=%s asked for frame %d and %d of %d "
                     "requested pictures reached the disk; the run cannot report a frame it "
                     "did not take\n",
                     capturePath.c_str(), captureFrame, (int)captureWritten,
                     (int)captureRequested);
        status = 1;
    }

    // The arenas' GPU objects went with the last level, while the renderer was
    // still there (the level loop's teardown).
    mdk_render_destroy(renderer);
    mdk::audioClose();
    mdk::windowDestroy(&window);
    return status;
}
