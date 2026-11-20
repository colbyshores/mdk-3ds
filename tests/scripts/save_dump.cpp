// save_dump.cpp -- see save_dump.h. The C++ twin of tests/scripts/save_probe.

#include "save_dump.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <vector>

#include "audio/sound_bank.h"
#include "formats/cmi.h"
#include "game/arena_space.h"
#include "game/game_state.h"
#include "game/input.h"
#include "game/level_data.h"
#include "game/player.h"
#include "game/save_games.h"
#include "scripts/script_runtime.h"

#include "formats/sprite_animation.h"

namespace mdk {
namespace test {

namespace {

// tick1.cpp's two helpers (private to it there): Kurt's state names as the oracle
// prints them, and his frame counts from TRAVSPRT.BNI as KurtSprite.FrameCount reads
// them.
const char* kurtStateName(Kurt::State state) {
    static const char* const kNames[] = {
        "Still", "Idle", "Run", "Side", "Turn", "Jump", "RunJump", "Fall", "Chute", "Land",
        "Shot", "RunFire", "Throw", "Knocked", "GetUp", "Dead", "Slip", "Slide", "SlideFast",
        "SlideBrake", "Hang", "HardLand", "RollLeft", "RollRight", "HelmetOn", "HelmetOff"};
    const int index = (int)state;
    return index >= 0 && index < (int)(sizeof(kNames) / sizeof(kNames[0])) ? kNames[index] : "?";
}

Player::FrameCounts kurtFrameCounts(const formats::Bni& sprites) {
    static const char* const kAnimations[] = {
        "K_STILL", "K_IDLE", "K_RUN", "K_SIDE", "K_TRN45", "K_JUMP", "K_RJMP", "K_FALL",
        "K_CHUTE", "K_LAND", "K_SHOT", "K_RUNFIR", "K_SPWEP", "K_BANG", "K_BFLIP", "K_BANG",
        "K_SLIP", "K_SLIDE", "K_FSLIDE", "K_BSLIDE", "K_HANG", "K_CRASHL", "K_ROLLL", "K_ROLLR",
        "K_HELM", "K_HELM"};
    Player::FrameCounts counts;
    for (int i = 0; i < Player::kStateCount; i++) {
        formats::SpriteAnimation animation;
        counts.values[i] = sprites.has(kAnimations[i]) &&
                                   sprites.animation(kAnimations[i], &animation)
                               ? animation.frameCount()
                               : 0;
    }
    return counts;
}

// The probe's FixedTime: the save's time, fixed so the bytes repeat.
const char kFixedTime[] = "2026-11-20T12:00:00";

struct Hold {
    Key key;
    int from;
    int to;
};

struct Give {
    int step;
    std::string model;
};

// `tp:step:ARENA:x:y:z` and `run:step:MODEL:pc:dx:dy:dz` (hazards_probe's).
struct Teleport {
    int step;
    std::string arena;
    Vec3 position;
};

struct Run {
    int step;
    std::string model;
    int32_t pc;
    Vec3 offset;
};

struct Schedule {
    std::vector<Hold> holds;
    std::vector<Give> gives;
    std::vector<Teleport> teleports;
    std::vector<Run> runs;
    bool hasYaw = false;
    float yaw = 0.0f;
    bool hasClip = false;
    int32_t clipRounds = 0;
    float clipTime = 0.0f;
};

// A token's fields after its head, split at ':'.
std::vector<std::string> fieldsOf(const std::string& rest) {
    std::vector<std::string> fields;
    size_t at = 0;
    for (;;) {
        const size_t next = rest.find(':', at);
        fields.push_back(rest.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) {
            return fields;
        }
        at = next + 1;
    }
}

// items_probe's grammar, the kinds the save probe takes: `key:from-to`,
// `give:step:MODEL` and `yaw:DEG` (Kurt's yaw before the first step).
bool parseSchedule(const std::string& schedule, Schedule* out) {
    std::vector<Hold>* holds = &out->holds;
    std::vector<Give>* gives = &out->gives;
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
        if (head == "yaw") {
            out->hasYaw = true;
            out->yaw = strtof(rest.c_str(), nullptr);
            continue;
        }
        if (head == "clip") {
            const std::vector<std::string> f = fieldsOf(rest);
            if (f.size() != 2) {
                return false;
            }
            out->hasClip = true;
            out->clipRounds = atoi(f[0].c_str());
            out->clipTime = strtof(f[1].c_str(), nullptr);
            continue;
        }
        if (head == "tp") {
            const std::vector<std::string> f = fieldsOf(rest);
            if (f.size() != 5) {
                return false;
            }
            Teleport t;
            t.step = atoi(f[0].c_str());
            t.arena = f[1];
            t.position = Vec3{strtof(f[2].c_str(), nullptr), strtof(f[3].c_str(), nullptr),
                              strtof(f[4].c_str(), nullptr)};
            out->teleports.push_back(t);
            continue;
        }
        if (head == "run") {
            const std::vector<std::string> f = fieldsOf(rest);
            if (f.size() != 6) {
                return false;
            }
            Run r;
            r.step = atoi(f[0].c_str());
            r.model = f[1];
            r.pc = atoi(f[2].c_str());
            r.offset = Vec3{strtof(f[3].c_str(), nullptr), strtof(f[4].c_str(), nullptr),
                            strtof(f[5].c_str(), nullptr)};
            out->runs.push_back(r);
            continue;
        }
        if (head == "give") {
            const size_t second = rest.find(':');
            if (second == std::string::npos) {
                return false;
            }
            Give give;
            give.step = atoi(rest.substr(0, second).c_str());
            give.model = rest.substr(second + 1);
            gives->push_back(give);
            continue;
        }
        bool found = false;
        for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
            if (head == kKeys[k].name) {
                Hold hold;
                hold.key = kKeys[k].key;
                if (sscanf(rest.c_str(), "%d-%d", &hold.from, &hold.to) != 2) {
                    return false;
                }
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

// The probe's World(level): a fresh level, the reachable arenas in Kurt's space,
// Kurt at the DTI's start, a runtime seeded 1 with Option 1. Members in the order
// they borrow each other.
struct World {
    LevelData level;
    std::vector<uint8_t> cmiBytes;
    formats::Cmi cmi;
    audio::SoundBank bank;
    ArenaSpace space;
    Player player;
    std::unique_ptr<scripts::ScriptRuntime> runtime;

    bool make(const formats::MdkData& data, const formats::Bni* sprites, int number,
              std::string* why) {
        if (!LevelData::load(&level, &data, number)) {
            *why = "the level did not load";
            return false;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", number, number);
        if (!data.read(cmiPath, &cmiBytes) ||
            !formats::Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            *why = "the CMI did not parse";
            return false;
        }
        if (!audio::SoundBank::forLevel(&bank, data, number)) {
            *why = "the sound bank did not load";
            return false;
        }
        bank.preload();
        const std::vector<const formats::Arena*>& arenas = level.arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            if (level.isReachable(arenas[i]->name())) {
                space.add(arenas[i]);
            }
        }
        const float* start = level.dti().startPosition();
        player.feet = Vec3{start[0], start[1], start[2]};
        player.yaw = level.dti().startAngle();
        player.setSpace(&space);
        player.setFrameCounts(kurtFrameCounts(*sprites));
        runtime.reset(new scripts::ScriptRuntime(&level, &cmi, 1, &bank, &player, sprites));
        runtime->option = 1;
        player.setMixer(&runtime->mixer());
        return true;
    }

    // The probe's Step: the teleports, the gives, the runs, the keys, Kurt then the
    // scripts at 1/60 s.
    void step(int at, const Schedule& schedule) {
        for (size_t i = 0; i < schedule.teleports.size(); i++) {
            const Teleport& t = schedule.teleports[i];
            if (t.step == at) {
                runtime->teleportKurt(t.arena, t.position, player.yaw);
                // The C#'s EnterArena adds the arena to its ONE space, Kurt's too.
                const formats::Arena* arena = level.arenaNamed(t.arena);
                if (arena != nullptr) {
                    space.add(arena);
                }
            }
        }
        for (size_t i = 0; i < schedule.gives.size(); i++) {
            if (schedule.gives[i].step == at) {
                player.collect(schedule.gives[i].model);
            }
        }
        for (size_t i = 0; i < schedule.runs.size(); i++) {
            const Run& r = schedule.runs[i];
            if (r.step == at) {
                runtime->spawn(&runtime->getArenaState(runtime->currentArena()).controller, r.model,
                               Vec3{player.feet.x + r.offset.x, player.feet.y + r.offset.y,
                                    player.feet.z + r.offset.z},
                               0.0f, -1, r.pc, false);
            }
        }
        Input input;
        for (size_t i = 0; i < schedule.holds.size(); i++) {
            if (at >= schedule.holds[i].from && at < schedule.holds[i].to) {
                input.down[schedule.holds[i].key] = true;
            }
        }
        player.update(input, 1.0f / 60.0f);
        runtime->update(1.0f / 60.0f);
    }
};

// The probe's Restore(level, file, after): parse the file, load it into a fresh
// world, run on.
bool restore(const formats::MdkData& data, const formats::Bni* sprites, int level,
             const std::string& file, int after, std::string* out, std::string* why) {
    flow::SaveGame save;
    if (!flow::SaveGame::parse(file, &save)) {
        *why = "the save did not parse";
        return false;
    }
    flow::GameState state;
    state.load(save);
    char line[512];
    snprintf(line, sizeof(line),
             "parsed kind=%d level=%d health=%d deaths=%d strike=%s time=%s state=%d\n",
             (int)save.kind, (int)save.level, (int)save.health, (int)save.deaths,
             save.strikeUsed ? "True" : "False", save.time.c_str(),
             state.snapshot.empty() ? -1 : (int)state.snapshot.size());
    *out += line;
    World world;
    if (!world.make(data, sprites, level, why)) {
        return false;
    }
    if (!world.runtime->load(state.snapshot, why)) {
        return false;
    }
    // The other half of the C#'s ONE ArenaSpace: what the restore entered, Kurt's
    // space has too (ScriptRuntime::enteredArenas).
    const std::vector<std::string> entered = world.runtime->enteredArenas();
    for (size_t i = 0; i < entered.size(); i++) {
        const formats::Arena* arena = world.level.arenaNamed(entered[i]);
        if (arena != nullptr) {
            world.space.add(arena);
        }
    }
    const std::string restored = world.runtime->capture();
    snprintf(line, sizeof(line), "restored %s %d objects %d\n",
             scripts::ScriptRuntime::snapshotHash(restored).c_str(), (int)restored.size(),
             (int)world.runtime->objectCount());
    *out += line;
    if (restored != state.snapshot) {
        *out += restored + "\n";
    }
    const Schedule none;
    const auto p64 = [](float v) { return (int)floorf(v * 64.0f); };
    for (int step = 1; step <= after; step++) {
        world.step(step, none);
        const std::string json = world.runtime->capture();
        const Player& kurt = world.player;
        snprintf(line, sizeof(line),
                 "s=%d state=%s feet64=%d,%d,%d yaw=%.4f hp=%d objs=%d arena=%s hash=%s\n", step,
                 kurtStateName(kurt.state()), p64(kurt.feet.x), p64(kurt.feet.y),
                 p64(kurt.feet.z), kurt.yaw, kurt.health(), (int)world.runtime->objectCount(),
                 world.runtime->currentArena().c_str(),
                 scripts::ScriptRuntime::snapshotHash(json).c_str());
        *out += line;
    }
    *out += "final\n";
    *out += world.runtime->capture() + "\n";
    return true;
}

// The first world: played to the save, then its save file's text.
bool play(const formats::MdkData& data, const formats::Bni* sprites, int level, int steps,
          const std::string& schedule, World* world, std::string* capture, std::string* file,
          std::string* why) {
    Schedule parsed;
    if (!parseSchedule(schedule, &parsed)) {
        *why = "the schedule did not parse: " + schedule;
        return false;
    }
    if (!world->make(data, sprites, level, why)) {
        return false;
    }
    if (parsed.hasYaw) {
        world->player.yaw = parsed.yaw;
    }
    if (parsed.hasClip) {
        world->player.scope().clipRounds = parsed.clipRounds;
        world->player.scope().clipTime = parsed.clipTime;
    }
    for (int step = 1; step <= steps; step++) {
        world->step(step, parsed);
    }
    *capture = world->runtime->capture();
    // Viewer.SnapshotSave: GameState.Save(Snapshot) with Kurt's health and the state.
    flow::GameState game;
    game.level = level;
    flow::SaveGame saved = game.saveAt(flow::SaveKind::Snapshot, kFixedTime);
    saved.health = world->player.health();
    saved.hasState = true;
    saved.state = *capture;
    *file = saved.toJson();
    return true;
}

} // namespace

bool saveDump(const formats::MdkData& data, const formats::Bni* sprites, int level, int steps,
              int after, const std::string& schedule, std::string* out, std::string* why) {
    if (sprites == nullptr) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    char line[256];
    snprintf(line, sizeof(line), "save level=%d steps=%d after=%d seed=1 schedule=%s\n", level,
             steps, after, schedule.c_str());
    *out = line;
    World first;
    std::string capture;
    std::string file;
    if (!play(data, sprites, level, steps, schedule, &first, &capture, &file, why)) {
        return false;
    }
    snprintf(line, sizeof(line), "capture %s %d objects %d can=%s\n",
             scripts::ScriptRuntime::snapshotHash(capture).c_str(), (int)capture.size(),
             (int)first.runtime->objectCount(), first.runtime->canSnapshot() ? "True" : "False");
    *out += line;
    snprintf(line, sizeof(line), "file %d\n", (int)file.size());
    *out += line;
    *out += file + "\n";
    return restore(data, sprites, level, file, after, out, why);
}

bool saveLoadDump(const formats::MdkData& data, const formats::Bni* sprites,
                  const std::string& fileText, int level, int after, std::string* out,
                  std::string* why) {
    if (sprites == nullptr) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    char line[128];
    snprintf(line, sizeof(line), "save load level=%d after=%d\n", level, after);
    *out = line;
    return restore(data, sprites, level, fileText, after, out, why);
}

bool saveFileText(const formats::MdkData& data, const formats::Bni* sprites, int level,
                  int steps, const std::string& schedule, std::string* out, std::string* why) {
    if (sprites == nullptr) {
        *why = "no TRAVSPRT.BNI to take the frame counts from";
        return false;
    }
    World world;
    std::string capture;
    return play(data, sprites, level, steps, schedule, &world, &capture, out, why);
}

} // namespace test
} // namespace mdk
