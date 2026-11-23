// ============================================================================
// harness_3ds.cpp -- see harness_3ds.h. The whole file is the harness
// build's: without MDK_AUTOQUIT it compiles to the two no-op answers, so
// the release binary carries no trace of the cycle (no deadline, no
// chain-load, no hb:ldr traffic).
// ============================================================================

#include "platform/harness_3ds.h"

#ifdef MDK_AUTOQUIT

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "platform/log_3ds.h"
#ifdef MDK_HW_WALK
#include "game/input.h"
#include "game/walk_script.h"
#endif

namespace mdk {

bool harnessAutoquitDue() {
    static u64 quitAtTick = 0;
    const u64 now = svcGetSystemTick();
    if (quitAtTick == 0) {
        // Armed on the first frame, not at init (the runbook: an init-time
        // arm measures the boot, not the game).
        quitAtTick = now + (u64)(MDK_AUTOQUIT) * (u64)SYSCLOCK_ARM11;
        return false;
    }
    return now >= quitAtTick;
}

#ifdef MDK_HW_WALK
namespace {
WalkScript g_walk;
bool g_walkReady = false;
#ifdef MDK_HW_PROFILE
// A PROFILE build keeps each segment's start here and prints it at exit.
struct WalkStart {
    uint32_t frame;
    int32_t segment;
    float x, y, z, yaw;
    char arena[16];
};
WalkStart g_walkStarts[WalkScript::kMaxSegments];
int32_t g_walkStartCount = 0;
#endif

void walkSegmentText(int32_t segment, char* out, size_t size) {
    char keys[96];
    formatWalkKeys(g_walk.segments[segment].keys, keys, sizeof(keys));
    snprintf(out, size, "%s@%lu-%lu", keys, (unsigned long)g_walk.segments[segment].first,
             (unsigned long)g_walk.segments[segment].end);
}
} // namespace
#endif

bool harnessWalkInit() {
#ifdef MDK_HW_WALK
    // Cold: once, at boot, before the first frame. The script is the Makefile's
    // WALK, a string literal (-DMDK_HW_WALK='"..."').
    char error[256];
    mdk::log3dsPrintf("main: harness walk WALK=%s", MDK_HW_WALK);
    if (!parseWalkScript(MDK_HW_WALK, &g_walk, error, sizeof(error))) {
        mdk::log3dsPrintf("main: harness walk REFUSED: %s", error);
        return false;
    }
    for (int32_t i = 0; i < g_walk.count; i++) {
        char text[128];
        walkSegmentText(i, text, sizeof(text));
        mdk::log3dsPrintf("walk: segment %d of %d: %s (%lu frames)", (int)(i + 1),
                          (int)g_walk.count, text,
                          (unsigned long)(g_walk.segments[i].end - g_walk.segments[i].first));
    }
    g_walkReady = true;
#endif
    return true;
}

void harnessWalkApply(uint32_t frame, Input* input, float x, float y, float z, float yaw,
                      const char* arena) {
#ifdef MDK_HW_WALK
    if (!g_walkReady) {
        return;
    }
    applyWalk(g_walk, frame, input);
    const int32_t started = walkSegmentStartingAt(g_walk, frame);
    if (started < 0) {
        return;
    }
#ifdef MDK_HW_PROFILE
    if (g_walkStartCount < WalkScript::kMaxSegments) {
        WalkStart& s = g_walkStarts[g_walkStartCount++];
        s.frame = frame;
        s.segment = started;
        s.x = x;
        s.y = y;
        s.z = z;
        s.yaw = yaw;
        snprintf(s.arena, sizeof(s.arena), "%s", arena ? arena : "");
    }
#else
    char text[128];
    walkSegmentText(started, text, sizeof(text));
    mdk::log3dsPrintf("walk: frame %lu: segment %d %s starts, feet (%.1f,%.1f,%.1f) yaw %.1f "
                      "arena %s",
                      (unsigned long)frame, (int)(started + 1), text, (double)x, (double)y,
                      (double)z, (double)yaw, arena && arena[0] ? arena : "-");
#endif
#else
    (void)frame; (void)input; (void)x; (void)y; (void)z; (void)yaw; (void)arena;
#endif
}

#ifdef MDK_HW_PROFILE
namespace {
// 0.1 ms buckets to 100 ms, one overflow bucket. Frame periods on this
// target sit at 16.7/33.3/50 ms (vsync-quantized), so 0.1 ms resolves the
// quantum and its jitter; anything over 100 ms is a stall, counted, with
// its true maximum kept separately.
constexpr uint32_t kBucketUs = 100;
constexpr uint32_t kBuckets = 1000;
uint32_t g_hist[kBuckets + 1];
uint32_t g_count = 0;
uint64_t g_sumUs = 0;
uint32_t g_maxUs = 0;
uint32_t g_maxAt = 0;    // the profiled frame (1-based) that took g_maxUs
// THE STEADY STATE. The frame period is quantised by the 60 Hz refresh (a
// frame takes 2 or 3 vblanks: 33.3 or 50 ms), so the p50 flips between those
// with no real change (measured 2026-11-19: 35.1 against 49.5 ms for builds
// whose means differ by 1.2 ms); and the level's first frames carry the
// first-use work (seconds). So the summary also gives the mean from the third
// frame on, and how many frames took 1, 2, 3 and 4+ vblanks.
constexpr uint32_t kSteadyFrom = 3;
uint64_t g_steadySumUs = 0;
uint32_t g_steadyCount = 0;
uint32_t g_vblanks[5] = {0, 0, 0, 0, 0};   // [1], [2], [3], [4+]; [0] unused
// THE SPIKES, NAMED. A frame over kSpikeUs (60 ms: four vblanks or more) is
// logged with the previous frame's spans -- the spans of frame N are reported
// at its end, its period measured at the start of N+1 -- so a spike says what
// it was made of (simulation steps, the draw build, the GPU waits). The first
// kSpikeLogs only: the log is an SD write.
constexpr uint32_t kSpikeUs = 60000u;
constexpr uint32_t kSpikeLogs = 40u;
uint32_t g_spikesLogged = 0;
uint64_t g_lastSimT = 0, g_lastBeginT = 0, g_lastDrawT = 0, g_lastEndT = 0, g_lastAudioT = 0;
uint32_t g_lastSteps = 0;
uint32_t g_over17 = 0;   // > 16.667 ms: missed a 60 Hz vblank
uint32_t g_over34 = 0;   // > 33.333 ms: missed a 30 Hz vblank
bool g_stereoSeen = false;
uint64_t g_simT = 0, g_beginT = 0, g_drawT = 0, g_endT = 0, g_audioT = 0;
uint64_t g_simSteps = 0;
uint32_t g_spanFrames = 0;
uint64_t g_playerT = 0, g_runtimeT = 0, g_cameraT = 0, g_solidT = 0;

// THE ARENA-ENTRY TABLE (harness_3ds.h, HarnessArenaFrame). Fixed storage: the
// last kArenaBefore+1 frames in a ring, and the kept windows' rows in a flat
// table filled in frame order. Raw ticks while measuring; converted at exit.
constexpr uint32_t kArenaBefore = 3;
constexpr uint32_t kArenaAfter = 60;
constexpr uint32_t kArenaRing = kArenaBefore + 1;
constexpr uint32_t kArenaRows = 640;
constexpr uint32_t kArenaEvents = 32;
constexpr uint32_t kArenaName = 16;
enum : uint8_t {
    kArenaFlagArena = 1,    // A: Kurt's (the scripts') current arena changed
    kArenaFlagSecond = 2,   // 2: the second arena, or its activation, changed
    kArenaFlagSwitch = 4,   // W: the arena stream switched (an MTO arena began streaming)
    kArenaFlagViews = 8,    // V: arena views were built this frame
    kArenaFlagStall = 16,   // T: an arena was shown before its stream was done
    kArenaFlagEvict = 32,   // E: the GPU texture LRU evicted (a thrash shows as a run of E)
};
struct ArenaRow {
    uint32_t frame;
    uint32_t periodUs;      // 0 until the next frame's start fills it in
    uint32_t simT, beginT, drawT, endT, audioT;   // ticks (clamped to 32 bits)
    uint32_t steps;
    uint32_t streamBytes;   // read by the arena stream this frame
    float streamMs, viewsMs;
    uint16_t viewsBuilt, textures, hd, kurtReads, kurtHolds, logLines;
    uint16_t texEvicted, hdEvicted;
    float texMs, hdMs;
    uint32_t linearFreeKB;
    uint8_t flags;
};
struct ArenaEvent {
    uint32_t frame;
    uint8_t flags;
    bool secondActive;
    char from[kArenaName];
    char to[kArenaName];
    char second[kArenaName];
};
ArenaRow g_arenaRing[kArenaRing];
uint32_t g_arenaRingNext = 0;     // the slot the next frame goes in
uint32_t g_arenaRingFill = 0;     // how many slots hold a frame
int32_t g_arenaRingNewest = -1;   // the newest frame's slot (its period comes next)
ArenaRow g_arenaRows[kArenaRows];
uint32_t g_arenaRowCount = 0;
uint32_t g_arenaRowsDropped = 0;
int32_t g_arenaRowNewest = -1;    // the newest frame's table row, if it was kept
ArenaEvent g_arenaEvents[kArenaEvents];
uint32_t g_arenaEventCount = 0;
uint32_t g_arenaEventsDropped = 0;
bool g_arenaWindowOpen = false;
uint32_t g_arenaWindowEnd = 0;
bool g_arenaSeen = false;          // the first frame sets the baseline
char g_arenaName[kArenaName];
char g_arenaSecond[kArenaName];
bool g_arenaSecondActive = false;
int32_t g_arenaPrevSwitches = 0, g_arenaPrevStalls = 0, g_arenaPrevTextures = 0;
int32_t g_arenaPrevHd = 0, g_arenaPrevKurtReads = 0, g_arenaPrevKurtHolds = 0;
int64_t g_arenaPrevBytes = 0;
uint32_t g_arenaPrevLog = 0;
int32_t g_arenaPrevTexEvicted = 0, g_arenaPrevHdEvicted = 0;
uint64_t g_arenaPrevTexTicks = 0, g_arenaPrevHdTicks = 0;
// The whole run's first-use cost (the table shows the windows; this is the total).
uint64_t g_runTexTicks = 0, g_runHdTicks = 0;
uint32_t g_runTexEvicted = 0, g_runHdEvicted = 0, g_runTextures = 0, g_runHd = 0;
uint32_t g_runLinearMinKB = 0xFFFFFFFFu;
float g_runTexWorstMs = 0.0f, g_runHdWorstMs = 0.0f;
uint32_t g_runTexWorstFrame = 0, g_runHdWorstFrame = 0;

uint32_t clampTicks(uint64_t ticks) {
    return ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ticks;
}

uint16_t clamp16(int64_t value) {
    return value < 0 ? (uint16_t)0 : (value > 0xFFFF ? (uint16_t)0xFFFF : (uint16_t)value);
}

void copyName(char* out, const char* name) {
    snprintf(out, kArenaName, "%s", name ? name : "");
}

uint32_t percentileUs(uint32_t pct) {
    // The bucket's upper edge: an estimate, stated as one in the log line.
    const uint32_t want = (uint32_t)(((uint64_t)g_count * pct + 99) / 100);
    uint32_t seen = 0;
    for (uint32_t b = 0; b <= kBuckets; b++) {
        seen += g_hist[b];
        if (seen >= want && want > 0) {
            return b == kBuckets ? g_maxUs : (b + 1) * kBucketUs;
        }
    }
    return g_maxUs;
}
} // namespace
#endif

void harnessProfileFrame(double periodSeconds) {
#ifdef MDK_HW_PROFILE
    // Once per frame, outside any loop over the frame's work: one double
    // compare and one conversion, never in a hot loop (AGENTS.md R2 scope).
    if (periodSeconds < 0.0) periodSeconds = 0.0;
    if (periodSeconds > 4000.0) periodSeconds = 4000.0;
    const uint32_t us = (uint32_t)(periodSeconds * 1000000.0);
    // A literal divisor (kBucketUs), so the compiler emits a multiply rather
    // than __aeabi_uidiv (AGENTS.md R11; this runs once per frame).
    const uint32_t b = us / 100u;
    static_assert(kBucketUs == 100u, "the bucket divisor above is kBucketUs");
    g_hist[b < kBuckets ? b : kBuckets]++;
    g_count++;
    g_sumUs += us;
    if (us > g_maxUs) {
        g_maxUs = us;
        g_maxAt = g_count;
    }
    // A STALL is named when it happens (frame number, length), not only folded
    // into the summary's max: the summary cannot say which frame, and the frame
    // log's `elapsed` is the game's clamped step, not the wall clock. Rare by
    // definition, so the log write is not a per-frame cost.
    if (us > kSpikeUs && us <= 500000u && g_count >= kSteadyFrom && g_spikesLogged < kSpikeLogs) {
        g_spikesLogged++;
        const double usPerTick = 1000000.0 / (double)SYSCLOCK_ARM11;
        mdk::log3dsPrintf("profile: spike %lu.%01lu ms at profiled frame %lu: sim %.2f ms (%lu steps) "
                          "begin %.2f draw %.2f end %.2f audio %.2f",
                          (unsigned long)(us / 1000u), (unsigned long)(us % 1000u / 100u),
                          (unsigned long)g_count, (double)g_lastSimT * usPerTick / 1000.0,
                          (unsigned long)g_lastSteps, (double)g_lastBeginT * usPerTick / 1000.0,
                          (double)g_lastDrawT * usPerTick / 1000.0,
                          (double)g_lastEndT * usPerTick / 1000.0,
                          (double)g_lastAudioT * usPerTick / 1000.0);
    }
    if (us > 500000u) {
        mdk::log3dsPrintf("profile: STALL %lu.%03lu ms at profiled frame %lu",
                          (unsigned long)(us / 1000u), (unsigned long)(us % 1000u),
                          (unsigned long)g_count);
    }
    if (g_count >= kSteadyFrom) {
        g_steadySumUs += us;
        g_steadyCount++;
        // Nearest whole vblank (16.667 ms), by integer arithmetic: the divisor
        // is a literal (AGENTS.md R11).
        const uint32_t vb = (us + 8333u) / 16667u;
        g_vblanks[vb < 1u ? 1u : (vb > 4u ? 4u : vb)]++;
    }
    if (us > 16667) g_over17++;
    if (us > 33333) g_over34++;
#ifndef MDK_HW_MONO
    // (MONO=1 forces one eye whatever the slider says: the report says off.)
    if (osGet3DSliderState() > 0.0f) g_stereoSeen = true;
#endif
    // The arena-entry table's newest frame (the one that just ended) gets its
    // period now: in the ring, and in the table if its window kept it.
    if (g_arenaRingNewest >= 0) {
        g_arenaRing[g_arenaRingNewest].periodUs = us;
    }
    if (g_arenaRowNewest >= 0) {
        g_arenaRows[g_arenaRowNewest].periodUs = us;
    }
#else
    (void)periodSeconds;
#endif
}

uint64_t harnessProfileTick() {
#ifdef MDK_HW_PROFILE
    return svcGetSystemTick();
#else
    return 0;
#endif
}

void harnessProfileStepSpans(uint64_t playerTicks, uint64_t runtimeTicks,
                             uint64_t cameraTicks, uint64_t solidTicks) {
#ifdef MDK_HW_PROFILE
    g_playerT += playerTicks;
    g_runtimeT += runtimeTicks;
    g_cameraT += cameraTicks;
    g_solidT += solidTicks;
#else
    (void)playerTicks; (void)runtimeTicks; (void)cameraTicks; (void)solidTicks;
#endif
}

void harnessProfileSpans(uint64_t simTicks, uint32_t simSteps, uint64_t beginTicks,
                         uint64_t drawTicks, uint64_t endTicks, uint64_t audioTicks) {
#ifdef MDK_HW_PROFILE
    g_simT += simTicks;
    g_simSteps += simSteps;
    g_beginT += beginTicks;
    g_drawT += drawTicks;
    g_endT += endTicks;
    g_audioT += audioTicks;
    g_spanFrames++;
    g_lastSimT = simTicks;
    g_lastSteps = simSteps;
    g_lastBeginT = beginTicks;
    g_lastDrawT = drawTicks;
    g_lastEndT = endTicks;
    g_lastAudioT = audioTicks;
#else
    (void)simTicks; (void)simSteps; (void)beginTicks; (void)drawTicks; (void)endTicks;
    (void)audioTicks;
#endif
}

void harnessArenaFrame(uint32_t frame, const HarnessArenaFrame& s) {
#ifdef MDK_HW_PROFILE
    // Once per frame, at its end (after harnessProfileSpans, so g_last* are this
    // frame's spans): integer differences and a few short string compares.
    const char* arena = s.arena ? s.arena : "";
    const char* second = s.second ? s.second : "";
    if (!g_arenaSeen) {
        // The baseline: the counters as the level left them, and the start pair.
        // The start itself is not an event (the level load is not a room entry).
        g_arenaSeen = true;
        copyName(g_arenaName, arena);
        copyName(g_arenaSecond, second);
        g_arenaSecondActive = s.secondActive;
        g_arenaPrevSwitches = s.streamSwitches;
        g_arenaPrevStalls = s.streamStalls;
        g_arenaPrevBytes = s.streamBytes;
        g_arenaPrevTextures = s.texturesMade;
        g_arenaPrevHd = s.hdImported;
        g_arenaPrevKurtReads = s.kurtReads;
        g_arenaPrevKurtHolds = s.kurtHolds;
        g_arenaPrevLog = s.logLines;
        g_arenaPrevTexEvicted = s.texturesEvicted;
        g_arenaPrevHdEvicted = s.hdEvicted;
        g_arenaPrevTexTicks = s.textureMakeTicks;
        g_arenaPrevHdTicks = s.hdImportTicks;
    }
    ArenaRow row;
    memset(&row, 0, sizeof(row));
    row.frame = frame;
    row.simT = clampTicks(g_lastSimT);
    row.beginT = clampTicks(g_lastBeginT);
    row.drawT = clampTicks(g_lastDrawT);
    row.endT = clampTicks(g_lastEndT);
    row.audioT = clampTicks(g_lastAudioT);
    row.steps = g_lastSteps;
    row.streamBytes = (uint32_t)(s.streamBytes - g_arenaPrevBytes);
    row.streamMs = s.streamMs;
    row.viewsMs = s.viewsMs;
    row.viewsBuilt = clamp16(s.viewsBuilt);
    row.textures = clamp16((int64_t)s.texturesMade - g_arenaPrevTextures);
    row.hd = clamp16((int64_t)s.hdImported - g_arenaPrevHd);
    row.kurtReads = clamp16((int64_t)s.kurtReads - g_arenaPrevKurtReads);
    row.kurtHolds = clamp16((int64_t)s.kurtHolds - g_arenaPrevKurtHolds);
    row.logLines = clamp16((int64_t)s.logLines - (int64_t)g_arenaPrevLog);
    row.texEvicted = clamp16((int64_t)s.texturesEvicted - g_arenaPrevTexEvicted);
    row.hdEvicted = clamp16((int64_t)s.hdEvicted - g_arenaPrevHdEvicted);
    {
        const double msPerTick = 1000.0 / (double)SYSCLOCK_ARM11;
        const uint64_t texTicks = s.textureMakeTicks - g_arenaPrevTexTicks;
        const uint64_t hdTicks = s.hdImportTicks - g_arenaPrevHdTicks;
        row.texMs = (float)((double)texTicks * msPerTick);
        row.hdMs = (float)((double)hdTicks * msPerTick);
        g_runTexTicks += texTicks;
        g_runHdTicks += hdTicks;
        if (row.texMs > g_runTexWorstMs) {
            g_runTexWorstMs = row.texMs;
            g_runTexWorstFrame = frame;
        }
        if (row.hdMs > g_runHdWorstMs) {
            g_runHdWorstMs = row.hdMs;
            g_runHdWorstFrame = frame;
        }
    }
    row.linearFreeKB = s.linearFreeKB;
    if (s.linearFreeKB < g_runLinearMinKB) g_runLinearMinKB = s.linearFreeKB;
    g_runTexEvicted += row.texEvicted;
    g_runHdEvicted += row.hdEvicted;
    g_runTextures += row.textures;
    g_runHd += row.hd;
    const bool arenaChanged = strncmp(g_arenaName, arena, kArenaName - 1) != 0;
    const bool secondChanged = strncmp(g_arenaSecond, second, kArenaName - 1) != 0 ||
                               g_arenaSecondActive != s.secondActive;
    if (arenaChanged) row.flags |= kArenaFlagArena;
    if (secondChanged) row.flags |= kArenaFlagSecond;
    if (s.streamSwitches != g_arenaPrevSwitches) row.flags |= kArenaFlagSwitch;
    if (s.viewsBuilt > 0) row.flags |= kArenaFlagViews;
    if (s.streamStalls != g_arenaPrevStalls) row.flags |= kArenaFlagStall;
    if (row.texEvicted != 0) row.flags |= kArenaFlagEvict;
    g_arenaPrevSwitches = s.streamSwitches;
    g_arenaPrevStalls = s.streamStalls;
    g_arenaPrevBytes = s.streamBytes;
    g_arenaPrevTextures = s.texturesMade;
    g_arenaPrevHd = s.hdImported;
    g_arenaPrevKurtReads = s.kurtReads;
    g_arenaPrevKurtHolds = s.kurtHolds;
    g_arenaPrevLog = s.logLines;
    g_arenaPrevTexEvicted = s.texturesEvicted;
    g_arenaPrevHdEvicted = s.hdEvicted;
    g_arenaPrevTexTicks = s.textureMakeTicks;
    g_arenaPrevHdTicks = s.hdImportTicks;

    if (row.flags != 0) {
        if (g_arenaEventCount < kArenaEvents) {
            ArenaEvent& event = g_arenaEvents[g_arenaEventCount++];
            event.frame = frame;
            event.flags = row.flags;
            event.secondActive = s.secondActive;
            copyName(event.from, g_arenaName);
            copyName(event.to, arena);
            copyName(event.second, second);
        } else {
            g_arenaEventsDropped++;
        }
        if (!g_arenaWindowOpen) {
            // The frames before it, oldest first, out of the ring.
            const uint32_t before = g_arenaRingFill < kArenaBefore ? g_arenaRingFill : kArenaBefore;
            for (uint32_t i = before; i > 0; i--) {
                const uint32_t slot = (g_arenaRingNext + kArenaRing - i) % kArenaRing;
                if (g_arenaRowCount > 0 &&
                    g_arenaRing[slot].frame <= g_arenaRows[g_arenaRowCount - 1].frame) {
                    continue;   // the previous window already kept it
                }
                if (g_arenaRowCount < kArenaRows) {
                    g_arenaRows[g_arenaRowCount++] = g_arenaRing[slot];
                } else {
                    g_arenaRowsDropped++;
                }
            }
        }
        g_arenaWindowOpen = true;
        g_arenaWindowEnd = frame + kArenaAfter;
        copyName(g_arenaName, arena);
        copyName(g_arenaSecond, second);
        g_arenaSecondActive = s.secondActive;
    }
    g_arenaRing[g_arenaRingNext] = row;
    g_arenaRingNewest = (int32_t)g_arenaRingNext;
    g_arenaRingNext = (g_arenaRingNext + 1) % kArenaRing;
    if (g_arenaRingFill < kArenaRing) g_arenaRingFill++;
    g_arenaRowNewest = -1;
    if (g_arenaWindowOpen && frame <= g_arenaWindowEnd) {
        if (g_arenaRowCount < kArenaRows) {
            g_arenaRowNewest = (int32_t)g_arenaRowCount;
            g_arenaRows[g_arenaRowCount++] = row;
        } else {
            g_arenaRowsDropped++;
        }
    } else {
        g_arenaWindowOpen = false;
    }
#else
    (void)frame;
    (void)s;
#endif
}

#ifdef MDK_HW_PROFILE
namespace {
void flagText(uint8_t flags, char* out) {
    // Always six characters plus the terminator: "AW-V--" style.
    out[0] = (flags & kArenaFlagArena) ? 'A' : '-';
    out[1] = (flags & kArenaFlagSecond) ? '2' : '-';
    out[2] = (flags & kArenaFlagSwitch) ? 'W' : '-';
    out[3] = (flags & kArenaFlagViews) ? 'V' : '-';
    out[4] = (flags & kArenaFlagStall) ? 'T' : '-';
    out[5] = (flags & kArenaFlagEvict) ? 'E' : '-';
    out[6] = '\0';
}

// At exit, never inside a measured frame: one line per event, then one per kept
// row that is flagged, slow (over 1.25x the steady mean) or did first-use work.
void arenaReport(uint32_t steadyUs) {
    const uint32_t slowUs = steadyUs > 0 ? steadyUs + steadyUs / 4u : 0u;
    mdk::log3dsPrintf("profile: arena-entry table: %lu events (%lu dropped), %lu frames kept "
                      "(%lu dropped); window -%lu..+%lu frames of each; a row is shown when "
                      "flagged, over %lu.%01lums (1.25x the steady mean) or doing first-use "
                      "work",
                      (unsigned long)g_arenaEventCount, (unsigned long)g_arenaEventsDropped,
                      (unsigned long)g_arenaRowCount, (unsigned long)g_arenaRowsDropped,
                      (unsigned long)kArenaBefore, (unsigned long)kArenaAfter,
                      (unsigned long)(slowUs / 1000u), (unsigned long)(slowUs % 1000u / 100u));
    char flags[7];
    for (uint32_t i = 0; i < g_arenaEventCount; i++) {
        const ArenaEvent& e = g_arenaEvents[i];
        flagText(e.flags, flags);
        const bool moved = (e.flags & kArenaFlagArena) != 0;
        mdk::log3dsPrintf("profile: arena event %lu frame %lu [%s]: %s %s%s%s, second %s%s",
                          (unsigned long)(i + 1u), (unsigned long)e.frame, flags,
                          moved ? "entered" : "in", e.to[0] ? e.to : "-",
                          moved ? " from " : "", moved ? (e.from[0] ? e.from : "-") : "",
                          e.second[0] ? e.second : "-",
                          e.second[0] ? (e.secondActive ? " (active)" : " (inactive)") : "");
    }
    mdk::log3dsPrintf("profile: arena  frame period    sim(st)  begin   draw    end  "
                      "strmKB strmMs views viewMs  tex  texMs tEv  hd  hdMs hEv linKB kRd kSI "
                      "log flags (ms; period = this frame's start to the next's; tex/texMs/tEv "
                      "= textures made, ms making them, textures the LRU evicted; hd/hdMs/hEv "
                      "= HD imports, ms importing, HD evicted; linKB = linear free; kRd/kSI/log "
                      "= Kurt animations read, Kurt held draws, SD log lines, this frame)");
    const double msPerTick = 1000.0 / (double)SYSCLOCK_ARM11;
    uint32_t hidden = 0;
    for (uint32_t i = 0; i < g_arenaRowCount; i++) {
        const ArenaRow& r = g_arenaRows[i];
        const bool work = r.streamBytes != 0 || r.viewsBuilt != 0 || r.textures != 0 ||
                          r.hd != 0 || r.kurtReads != 0 || r.kurtHolds != 0 ||
                          r.logLines != 0 || r.texEvicted != 0 || r.hdEvicted != 0;
        const bool slow = slowUs == 0u || r.periodUs == 0u || r.periodUs > slowUs;
        if (r.flags == 0 && !work && !slow) {
            hidden++;
            continue;
        }
        flagText(r.flags, flags);
        mdk::log3dsPrintf("profile: arena %6lu %6lu.%01lu %5.1f(%lu) %6.1f %6.1f %6.1f %7lu "
                          "%6.1f %5u %6.1f %4u %6.1f %3u %3u %5.1f %3u %5lu %3u %3u %3u %s",
                          (unsigned long)r.frame, (unsigned long)(r.periodUs / 1000u),
                          (unsigned long)(r.periodUs % 1000u / 100u),
                          (double)r.simT * msPerTick, (unsigned long)r.steps,
                          (double)r.beginT * msPerTick, (double)r.drawT * msPerTick,
                          (double)r.endT * msPerTick, (unsigned long)(r.streamBytes / 1024u),
                          (double)r.streamMs, (unsigned)r.viewsBuilt, (double)r.viewsMs,
                          (unsigned)r.textures, (double)r.texMs, (unsigned)r.texEvicted,
                          (unsigned)r.hd, (double)r.hdMs, (unsigned)r.hdEvicted,
                          (unsigned long)r.linearFreeKB, (unsigned)r.kurtReads,
                          (unsigned)r.kurtHolds, (unsigned)r.logLines, flags);
    }
    mdk::log3dsPrintf("profile: arena table end (%lu kept rows hidden: unflagged, not slow, "
                      "no first-use work)",
                      (unsigned long)hidden);
    mdk::log3dsPrintf("profile: first-use run totals: %lu textures made in %.1f ms (worst frame "
                      "%lu: %.1f ms), %lu evicted; %lu HD imports in %.1f ms (worst frame %lu: "
                      "%.1f ms), %lu HD evicted; linear free min %lu KiB",
                      (unsigned long)g_runTextures, (double)g_runTexTicks * msPerTick,
                      (unsigned long)g_runTexWorstFrame, (double)g_runTexWorstMs,
                      (unsigned long)g_runTexEvicted, (unsigned long)g_runHd,
                      (double)g_runHdTicks * msPerTick, (unsigned long)g_runHdWorstFrame,
                      (double)g_runHdWorstMs, (unsigned long)g_runHdEvicted,
                      (unsigned long)(g_runLinearMinKB == 0xFFFFFFFFu ? 0u : g_runLinearMinKB));
}
} // namespace
#endif

void harnessProfileReport() {
#ifdef MDK_HW_PROFILE
    if (g_spanFrames > 0) {
        // Ticks -> microseconds once, here (the runbook's rule: raw ticks in the
        // bracket, convert once per report). Per frame, and per step for sim.
        const double usPerTick = 1000000.0 / (double)SYSCLOCK_ARM11;
        const double f = (double)g_spanFrames;
        const double simUs = (double)g_simT * usPerTick;
        mdk::log3dsPrintf("profile: spans frames=%lu sim=%.2fms/frame steps=%.2f/frame "
                          "step=%.2fms begin(vblank wait)=%.2fms draw=%.2fms end=%.2fms "
                          "audio=%.2fms",
                          (unsigned long)g_spanFrames, simUs / f / 1000.0,
                          (double)g_simSteps / f,
                          g_simSteps ? simUs / (double)g_simSteps / 1000.0 : 0.0,
                          (double)g_beginT * usPerTick / f / 1000.0,
                          (double)g_drawT * usPerTick / f / 1000.0,
                          (double)g_endT * usPerTick / f / 1000.0,
                          (double)g_audioT * usPerTick / f / 1000.0);
        // The step's parts, per STEP, and the residual so the accounting is
        // seen to close (the runbook's rule: an unexplained residual is where
        // the next phantom hides).
        const double steps = g_simSteps ? (double)g_simSteps : 1.0;
        const double parts = (double)(g_playerT + g_runtimeT + g_cameraT + g_solidT);
        mdk::log3dsPrintf("profile: step player=%.2fms runtime=%.2fms camera=%.2fms "
                          "solid=%.2fms residual=%.2fms (per step)",
                          (double)g_playerT * usPerTick / steps / 1000.0,
                          (double)g_runtimeT * usPerTick / steps / 1000.0,
                          (double)g_cameraT * usPerTick / steps / 1000.0,
                          (double)g_solidT * usPerTick / steps / 1000.0,
                          ((double)g_simT - parts) * usPerTick / steps / 1000.0);
    }
    u8 model = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }
    // 0 = 3DS, 1 = 3DS XL, 3 = 2DS: the Old-3DS family, 268 MHz, no L2.
    // 2 = New 3DS, 4 = New 3DS XL, 5 = New 2DS XL: NOT an OG result even when
    // run at 268 MHz (AGENTS.md: a New-3DS clocked run is not an OG pass).
    const bool oldFamily = model == 0 || model == 1 || model == 3;
    // Once per process, at exit: a double divide, not a 64-bit integer helper.
    const uint32_t meanUs = g_count ? (uint32_t)((double)g_sumUs / (double)g_count) : 0;
    mdk::log3dsPrintf("profile: frames=%lu mean=%lu.%03lums p50<=%lu.%01lums p95<=%lu.%01lums "
                      "p99<=%lu.%01lums max=%lu.%03lums (frame %lu) over16.7ms=%lu over33.3ms=%lu",
                      (unsigned long)g_count,
                      (unsigned long)(meanUs / 1000), (unsigned long)(meanUs % 1000),
                      (unsigned long)(percentileUs(50) / 1000), (unsigned long)(percentileUs(50) % 1000 / 100),
                      (unsigned long)(percentileUs(95) / 1000), (unsigned long)(percentileUs(95) % 1000 / 100),
                      (unsigned long)(percentileUs(99) / 1000), (unsigned long)(percentileUs(99) % 1000 / 100),
                      (unsigned long)(g_maxUs / 1000), (unsigned long)(g_maxUs % 1000),
                      (unsigned long)g_maxAt, (unsigned long)g_over17, (unsigned long)g_over34);
    const uint32_t steadyUs =
        g_steadyCount ? (uint32_t)((double)g_steadySumUs / (double)g_steadyCount) : 0;
    mdk::log3dsPrintf("profile: steady (frame %lu on) frames=%lu mean=%lu.%03lums; vblanks 1:%lu "
                      "2:%lu 3:%lu 4+:%lu",
                      (unsigned long)kSteadyFrom, (unsigned long)g_steadyCount,
                      (unsigned long)(steadyUs / 1000), (unsigned long)(steadyUs % 1000),
                      (unsigned long)g_vblanks[1], (unsigned long)g_vblanks[2],
                      (unsigned long)g_vblanks[3], (unsigned long)g_vblanks[4]);
    mdk::log3dsPrintf("profile: console model=%u family=%s clock=%s l2=%s stereo=%s "
                      "(frame period = wall clock between frames, first frame excluded; "
                      "percentiles are 0.1 ms bucket upper edges)",
                      (unsigned)model, oldFamily ? "old3ds" : (model == 0xFF ? "unknown" : "new3ds"),
                      oldFamily ? "268MHz(by-model)" : "unverified", oldFamily ? "none(by-model)" : "unverified",
#ifdef MDK_HW_MONO
                      "off(MONO=1, slider ignored)");
#else
                      g_stereoSeen ? "on" : "off");
#endif
#ifdef MDK_HW_WALK
    // The walk's segment starts, held back from the measured frames.
    for (int32_t i = 0; i < g_walkStartCount; i++) {
        const WalkStart& s = g_walkStarts[i];
        char text[128];
        walkSegmentText(s.segment, text, sizeof(text));
        mdk::log3dsPrintf("walk: frame %lu: segment %d %s started, feet (%.1f,%.1f,%.1f) yaw "
                          "%.1f arena %s",
                          (unsigned long)s.frame, (int)(s.segment + 1), text, (double)s.x,
                          (double)s.y, (double)s.z, (double)s.yaw, s.arena[0] ? s.arena : "-");
    }
#endif
    arenaReport(steadyUs);
#endif
}

const char* harnessCapturePath(uint32_t frame) {
#ifdef MDK_HW_CAPTURE
    if (frame == (uint32_t)(MDK_HW_CAPTURE)) {
        return "sdmc:/3ds/mdk/hwcap.bmp.log";
    }
#else
    (void)frame;
#endif
    return nullptr;
}

void harnessChainloadStub() {
    // Game-side exit recipe (hbmenu pattern): SetTarget + FULL argv, then
    // the caller's exit(0) is the transfer. svcConnectToPort("hb:ldr")
    // fails without Luma's loader; then this is simply a normal exit.
    static char argvBuffer[0x400];
    const char* path = "/3ds/netload_stub.3dsx";  // sdmc-relative, no prefix
    Handle hbldr = 0;
    if (R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) {
        return;
    }
    u32* cmdbuf = getThreadCommandBuffer();
    const u32 pathLength = (u32)strlen(path) + 1;
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(pathLength, 0);
    cmdbuf[2] = (u32)path;
    Result rc = svcSendSyncRequest(hbldr);
    if (R_SUCCEEDED(rc)) {
        rc = (Result)cmdbuf[1];
    }
    if (R_FAILED(rc)) {
        svcCloseHandle(hbldr);
        return;  // target not registered: stay a normal exit
    }
    ((u32*)argvBuffer)[0] = 1;
    snprintf(argvBuffer + 4, sizeof(argvBuffer) - 4, "sdmc:%s", path);
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(sizeof(argvBuffer), 1);
    cmdbuf[2] = (u32)argvBuffer;
    rc = svcSendSyncRequest(hbldr);
    svcCloseHandle(hbldr);
}

} // namespace mdk

#else  // MDK_AUTOQUIT

namespace mdk {

bool harnessAutoquitDue() {
    return false;
}

const char* harnessCapturePath(uint32_t) {
    return nullptr;
}

uint64_t harnessProfileTick() {
    return 0;
}

void harnessProfileStepSpans(uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileSpans(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileFrame(double) {}

void harnessProfileReport() {}

void harnessArenaFrame(uint32_t, const HarnessArenaFrame&) {}

bool harnessWalkInit() {
    return true;
}

void harnessWalkApply(uint32_t, Input*, float, float, float, float, const char*) {}

void harnessChainloadStub() {}

} // namespace mdk

#endif  // MDK_AUTOQUIT
