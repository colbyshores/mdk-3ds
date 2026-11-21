// stats_tests.cpp -- see stats_tests.h.

#include "stats_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "formats/bni.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "game/font_view.h"
#include "game/game_settings.h"
#include "game/heads_view.h"
#include "game/save_games.h"
#include "game/screen_view.h"
#include "game/slideshow.h"
#include "game/stats_screen.h"
#include "render/render.h"

// The canvas hook of the faked render seam (kurt_stream_tests.cpp).
extern void (*g_canvasQuadHook)(const mdk::CanvasQuad* quad);

// The seams the statistics reach that no other vm_tests source fakes. The heads'
// GPU half is never attached here (its logic is HeadsView::places/update).
void mdk_render_canvas_begin(MdkRenderer) {}
void mdk_render_canvas_end(MdkRenderer) {}
MdkMeshId mdk_render_create_mesh(MdkRenderer, const mdk::Vertex*, int32_t) { return -1; }
void mdk_render_set_camera(MdkRenderer, const mdk::Camera*) {}
void mdk_render_set_lighting(MdkRenderer, const mdk::Lighting*) {}
void mdk_render_set_point_lights(MdkRenderer, const mdk::PointLight*, int32_t) {}
void mdk_render_set_glow_quads(MdkRenderer, const mdk::GlowQuad*, int32_t) {}

namespace mdk {
namespace test {
namespace {

int failures = 0;

#define STATS_CHECK(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// The game's fixed step (C# Viewer.Step), as the probe's `1f / 60f`.
const float kStep = 1.0f / 60.0f;

// The probe's scenarios (menu_probe/Program.cs), field for field.
struct Scenario {
    const char* name;
    int32_t level;
    StatsScreen::Phase first;
    int32_t counts[7];
    int32_t towns;
    bool gore;
    const char* schedule;
};
const Scenario kScenarios[] = {
    {"stats-all7", 7, StatsScreen::Phase::None, {57, 31, 9, 4, 12, 20, 3}, 0, true,
     "-:59,accept:2,-:150,back:2,-:60,fire:60,-:200,fire:100,-:450,accept:2,-:45,down:3,"
     "accept:3,-:60,back:2,-:5,accept:40"},
    {"stats-score4", 4, StatsScreen::Phase::Score, {0, 0, 5, 5, 7, 7, 20}, 5, true,
     "-:40,back:2,-:30,back:2,-:30,back:2,-:30,back:2,-:20,back:2,-:20,back:2,-:20,back:2,"
     "-:20,fire:200,back:2,-:35,accept:3,-:5,accept:3,-:10,fire:200"},
    {"stats-nogore6", 6, StatsScreen::Phase::Debriefing, {10, 10, 0, 0, 0, 0, 2}, 6, false,
     "fire:500,-:10,back:3,-:4,fire:200"},
};

std::string f(float value) {
    return formatShortestFloat(value);
}

// The schedule's frames: the key on a step's first frame (a held one on each),
// then nothing (the probe's Schedule).
std::vector<std::string> schedule(const char* text) {
    std::vector<std::string> frames;
    const std::string all(text);
    size_t at = 0;
    while (at <= all.size()) {
        size_t comma = all.find(',', at);
        if (comma == std::string::npos) {
            comma = all.size();
        }
        const std::string step = all.substr(at, comma - at);
        const size_t colon = step.find(':');
        const std::string key = step.substr(0, colon);
        const int count = atoi(step.c_str() + colon + 1);
        const bool held = key == "rheld" || key == "lheld" || key == "fire";
        for (int i = 0; i < count; i++) {
            frames.push_back(i == 0 || held ? key : "-");
        }
        at = comma + 1;
    }
    return frames;
}

// The probe's Keys(key) as the screen reads it.
StatsScreen::Keys keysOf(const std::string& key) {
    StatsScreen::Keys keys;
    keys.prompt.up = key == "up";
    keys.prompt.down = key == "down";
    keys.prompt.left = key == "left";
    keys.prompt.right = key == "right";
    keys.prompt.accept = key == "accept";
    keys.prompt.back = key == "back";
    keys.back = keys.prompt.back;
    keys.any = keys.prompt.up || keys.prompt.down || keys.prompt.left || keys.prompt.right ||
               keys.prompt.accept || keys.prompt.back;
    keys.fastHeld = key == "fire";
    return keys;
}

const char* eventName(flow::Event event) {
    return event == flow::Event::Fall ? "Fall" : event == flow::Event::None ? "None" : "?";
}

// The scripted Random of the probe: (5n + 1) % max.
struct Scripted {
    int32_t n = 0;
    static int32_t next(void* context, int32_t max) {
        Scripted* self = static_cast<Scripted*>(context);
        return (self->n++ * 5 + 1) % max;
    }
};

// The screen's printed lines (the probe keeps Statistics:, Score-O-matic:, Saved game).
void collect(void* user, const char* line) {
    std::string* out = static_cast<std::string*>(user);
    *out += line;
    *out += "\n";
}

// The canvas, recorded as the probe's Overlay hook formats it.
struct Canvas {
    int32_t big = -1;
    int32_t small = -1;
    std::vector<std::string> quads;
};
Canvas* g_canvas = nullptr;
void record(const CanvasQuad* q) {
    if (!g_canvas) {
        return;
    }
    const char* kind = q->texture == g_canvas->big     ? "B"
                       : q->texture == g_canvas->small ? "S"
                       : q->texture < 0                ? "F"
                                                       : "I";
    g_canvas->quads.push_back(std::string(kind) + " " + f(q->x0) + "," + f(q->y0) + "," +
                              f(q->x2) + "," + f(q->y2) + " " + f(q->colour.r) + "," +
                              f(q->colour.g) + "," + f(q->colour.b) + "," + f(q->colour.a));
}

uint64_t hashOf(const std::vector<std::string>& lines) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string line = lines[i] + "\n";
        for (size_t k = 0; k < line.size(); k++) {
            hash = (hash ^ (uint8_t)line[k]) * 1099511628211ULL;
        }
    }
    return hash;
}

std::vector<int32_t> ids(const audio::SoundDevice& device) {
    std::vector<int32_t> out;
    for (size_t i = 0; i < device.voices().size(); i++) {
        out.push_back(device.voices()[i].id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool contains(const std::vector<int32_t>& list, int32_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

// MDKFONT.FTI (texts, fonts, SYS_PAL) and STATS.BNI, the fonts' atlases through
// the faked seam.
struct Assets {
    formats::Fti fti;
    formats::Font bigFont;
    formats::Font smallFont;
    FontView big;
    FontView small;
    std::vector<uint8_t> systemRgb;
    formats::Bni stats;
    bool ready = false;
};

bool loadAssets(const formats::MdkData& data, Assets* out) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    std::vector<uint8_t> statsBytes;
    if (!data.read("MISC/MDKFONT.FTI", &bytes) ||
        !formats::Fti::parse(&out->fti, bytes.data(), (int32_t)bytes.size()) ||
        !out->fti.getBytes("FONTBIG", &bigBytes) || !out->fti.getBytes("FONTSML", &smallBytes) ||
        !out->fti.getBytes("SYS_PAL", &out->systemRgb) ||
        !formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, &out->bigFont) ||
        !formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, &out->smallFont) ||
        !data.read("MISC/STATS.BNI", &statsBytes) ||
        !formats::Bni::parse(&out->stats, std::move(statsBytes))) {
        return false;
    }
    const formats::Palette palette =
        formats::Palette::fromRgb(out->systemRgb.data(), (int32_t)out->systemRgb.size());
    out->systemRgb.resize(768, 0);
    out->ready = out->big.load(nullptr, out->bigFont, 1, palette.rgba()) &&
                 out->small.load(nullptr, out->smallFont, 1, palette.rgba());
    return out->ready;
}

std::string scratchDir() {
    const char* tmp = getenv("TMPDIR");
    std::string dir = (tmp && tmp[0]) ? std::string(tmp) : std::string("build");
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "/mdk-stats-tests-%d", (int)getpid());
    return dir + leaf;
}

void removeDir(const std::string& dir) {
    const flow::SaveGames saves(dir + "/saves");
    const std::vector<std::string> names = saves.list();
    for (size_t i = 0; i < names.size(); i++) {
        saves.remove(names[i]);
    }
    rmdir((dir + "/saves").c_str());
    rmdir(dir.c_str());
}

std::string dumpScenario(Assets* assets, const Scenario& scenario) {
    std::string out;
    char line[1024];
    std::string counts;
    for (int32_t i = 0; i < 7; i++) {
        snprintf(line, sizeof(line), "%s%d", i > 0 ? "," : "", (int)scenario.counts[i]);
        counts += line;
    }
    snprintf(line, sizeof(line), "== %s: level %d, first %s, counts %s, towns %d, gore %d, canvas %s\n",
             scenario.name, (int)scenario.level,
             scenario.first == StatsScreen::Phase::None ? "-" : StatsScreen::phaseName(scenario.first),
             counts.c_str(), (int)scenario.towns, scenario.gore ? 1 : 0, f(480.0f).c_str());
    out += line;

    flow::GameState state;
    state.level = scenario.level;
    state.stats.shots = scenario.counts[0];
    state.stats.shotHits = scenario.counts[1];
    state.stats.sniperShots = scenario.counts[2];
    state.stats.sniperHits = scenario.counts[3];
    state.stats.kills = scenario.counts[4];
    state.stats.enemies = scenario.counts[5];
    state.stats.headShots = scenario.counts[6];
    state.stats.townFlags = (int32_t)((uint32_t)scenario.towns << 29);
    const std::string user = scratchDir();
    mkdir(user.c_str(), 0755);
    const flow::SaveGames saves = flow::SaveGames::in(user);

    Scripted scripted;
    std::string printed;
    StatsScreen::Setup setup;
    setup.fti = &assets->fti;
    setup.stats = &assets->stats;
    setup.systemRgb = assets->systemRgb.data();
    setup.state = &state;
    setup.saves = saves;
    setup.pages = StatsPages::All;
    setup.first = scenario.first;
    setup.gore = scenario.gore;
    setup.random.context = &scripted;
    setup.random.next = Scripted::next;
    setup.print.context = &printed;
    setup.print.line = collect;
    std::unique_ptr<StatsScreen> screen(new StatsScreen());
    screen->open(setup);
    out += printed;
    printed.clear();

    Canvas canvas;
    canvas.big = assets->big.texture();
    canvas.small = assets->small.texture();
    g_canvas = &canvas;
    g_canvasQuadHook = record;
    ScreenView view;
    view.layoutInside(480.0f, kCanvasHeight);

    const std::vector<std::string> frames = schedule(scenario.schedule);
    std::string lastStatus;
    for (size_t frame = 0; frame < frames.size(); frame++) {
        const std::string& key = frames[frame];
        const std::vector<int32_t> before = ids(screen->device());
        const flow::Event next = screen->frame(keysOf(key), kStep);
        canvas.quads.clear();
        screen->draw(nullptr, view, assets->big, assets->small);
        const std::vector<int32_t> after = ids(screen->device());
        screen->device().update(kStep);
        const std::vector<int32_t> mixed = ids(screen->device());
        std::string sound;
        for (size_t i = 0; i < after.size(); i++) {
            if (!contains(before, after[i])) {
                sound += " +" + std::string(screen->soundOf(after[i])) + "#" +
                         std::to_string(after[i]);
            }
        }
        for (size_t i = 0; i < before.size(); i++) {
            if (!contains(after, before[i])) {
                sound += " -#" + std::to_string(before[i]);
            }
        }
        for (size_t i = 0; i < after.size(); i++) {
            if (!contains(mixed, after[i])) {
                sound += " ~#" + std::to_string(after[i]);
            }
        }
        out += printed;
        printed.clear();

        const std::string status = screen->status();
        std::string prompt;
        if (const flow::SavePrompt* p = screen->prompt()) {
            snprintf(line, sizeof(line), " prompt=%s/%d/%s/%d",
                     p->stage() == flow::SavePrompt::Stage::Ask ? "Ask" : "Name",
                     (int)p->selected(), p->name().c_str(), (int)p->cursor());
            prompt = line;
        }
        std::string values;
        std::string slides;
        for (int32_t i = 0; i < StatsScreen::kRows; i++) {
            values += (i > 0 ? "," : "") + f(screen->value(i));
            slides += (i > 0 ? "," : "") + f(screen->slide(i));
        }
        snprintf(line, sizeof(line),
                 "f%d %s %s%s in=%s out=%s cur=%d count=%s shown=%d ticks=%s row=%d/%d/%s v=%s "
                 "s=%s heads=%d/%s next=%s\n",
                 (int)frame, key.c_str(), status.c_str(), prompt.c_str(),
                 f(screen->fadeIn()).c_str(), f(screen->fadeOut()).c_str(), (int)screen->current(),
                 f(screen->count()).c_str(), (int)screen->shown(), f(screen->ticks()).c_str(),
                 (int)screen->row(), (int)screen->rowStage(), f(screen->rowTime()).c_str(),
                 values.c_str(), slides.c_str(), (int)screen->heads(),
                 f(screen->headTime()).c_str(), eventName(next));
        out += line;
        if (!sound.empty()) {
            out += "  snd" + sound + "\n";
        }
        const int32_t heads = screen->heads();
        if (heads > 0) {
            std::string angles;
            const int32_t n = heads < HeadsView::kMaxHeads ? heads : HeadsView::kMaxHeads;
            for (int32_t i = 0; i < n; i++) {
                angles += (i > 0 ? "," : "") + f(screen->headsView().angle(i));
            }
            Vec3 at[HeadsView::kMaxHeads];
            const int32_t placed = HeadsView::places(heads, state.stats.headShots, at);
            std::string places;
            for (int32_t i = 0; i < placed; i++) {
                places += (i > 0 ? "," : "") + f(at[i].x) + ":" + f(at[i].z);
            }
            out += "  heads a=" + angles + " p=" + places + "\n";
        }
        snprintf(line, sizeof(line), "  quads %d %016llx\n", (int)canvas.quads.size(),
                 (unsigned long long)hashOf(canvas.quads));
        out += line;
        if (status != lastStatus || frame % 120 == 0) {
            for (size_t q = 0; q < canvas.quads.size(); q++) {
                out += "    " + canvas.quads[q] + "\n";
            }
        }
        lastStatus = status;
        if (next != flow::Event::None) {
            break;
        }
    }
    g_canvasQuadHook = nullptr;
    g_canvas = nullptr;

    const std::vector<std::string> names = saves.list();
    std::string list;
    for (size_t i = 0; i < names.size(); i++) {
        list += (i > 0 ? "," : "") + names[i];
    }
    snprintf(line, sizeof(line), "== end: level %d, saves %s\n", (int)state.level, list.c_str());
    out += line;
    screen->close();
    removeDir(user);
    return out;
}

// The probe's SlideshowSchedule: Update each frame, then Reset on a key.
const char kSlideshowSchedule[] = "-:200,key:1,-:400,key:1,-:300,key:1,-:1500";

uint64_t fnv(const uint8_t* bytes, int32_t count) {
    uint64_t hash = 14695981039346656037ULL;
    for (int32_t i = 0; i < count; i++) {
        hash = (hash ^ bytes[i]) * 1099511628211ULL;
    }
    return hash;
}

std::string dumpSlideshow(const formats::MdkData& data) {
    std::string out;
    char line[256];
    std::unique_ptr<Slideshow> show(new Slideshow());
    show->start(data);
    snprintf(line, sizeof(line), "== slideshow: %d slides\n", (int)show->slideCount());
    out += line;
    for (int32_t i = 1; i <= show->slideCount(); i++) {
        const formats::Gif* gif = show->gif(i);
        snprintf(line, sizeof(line), "  slide %d: %dx%d indices %016llx palette %016llx\n", (int)i,
                 (int)gif->width(), (int)gif->height(),
                 (unsigned long long)fnv(gif->indices(), gif->indexCount()),
                 (unsigned long long)fnv(gif->palette(), 768));
        out += line;
    }
    const std::vector<std::string> frames = schedule(kSlideshowSchedule);
    int32_t last = -1;
    for (size_t frame = 0; frame < frames.size(); frame++) {
        const std::string& key = frames[frame];
        show->update(kStep);
        if (key != "-") {
            show->reset();
        }
        if (key != "-" || show->image() != last) {
            snprintf(line, sizeof(line), "f%d %s image=%d items=%d time=%s\n", (int)frame,
                     key.c_str(), (int)show->image(), show->itemsShown() ? 1 : 0,
                     f(show->time()).c_str());
            out += line;
            last = show->image();
        }
    }
    return out;
}

bool readGolden(const char* path, std::string* out) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out->append(chunk, n);
    }
    fclose(file);
    return true;
}

void checkGolden(const formats::MdkData& data, const char* section) {
    char path[128];
    // "stats-all7" -> golden/stats_all7.txt, "slideshow" -> golden/stats_slideshow.txt.
    const char* leaf = strncmp(section, "stats-", 6) == 0 ? section + 6 : section;
    snprintf(path, sizeof(path), "golden/stats_%s.txt", leaf);
    std::string expected;
    if (!readGolden(path, &expected)) {
        printf("  FAIL: no golden file %s\n", path);
        failures++;
        return;
    }
    const std::string got = statsDump(data, section);
    if (got == expected) {
        return;
    }
    size_t at = 0;
    while (at < got.size() && at < expected.size() && got[at] == expected[at]) {
        at++;
    }
    size_t lineStart = at;
    while (lineStart > 0 && expected[lineStart - 1] != '\n') {
        lineStart--;
    }
    printf("  FAIL: %s differs from the C# probe at byte %lu: oracle \"%.100s\", port \"%.100s\"\n",
           path, (unsigned long)at, expected.c_str() + lineStart,
           lineStart < got.size() ? got.c_str() + lineStart : "");
    failures++;
}

// C# ValueText and HeadsView.Places, the cases the scenarios do not reach.
void testUnits() {
    STATS_CHECK(StatsScreen::valueText(0, 12, 57) == "12");
    STATS_CHECK(StatsScreen::valueText(1, 54, 100) == "54%");
    STATS_CHECK(StatsScreen::valueText(4, 7, 20) == "7/20");
    Vec3 at[HeadsView::kMaxHeads];
    // Four heads: one row of four at z -3.
    STATS_CHECK(HeadsView::places(4, 4, at) == 4 && at[0].z == -3.0f && at[3].z == -3.0f &&
                at[0].x == -6.0f && at[3].x == 6.0f);
    // Five: rows of three, the second row's two centred.
    STATS_CHECK(HeadsView::places(5, 5, at) == 5 && at[3].z == -4.5f && at[3].x == 20.0f / 3.0f - 10.0f);
    // Twenty: rows of eight, at most sixteen shown.
    STATS_CHECK(HeadsView::places(20, 20, at) == 16 && at[15].z == -4.5f);
}

} // namespace

std::string statsDump(const formats::MdkData& data, const char* section) {
    if (strcmp(section, "slideshow") == 0) {
        return dumpSlideshow(data);
    }
    std::unique_ptr<Assets> assets(new Assets());
    if (!loadAssets(data, assets.get())) {
        return std::string();
    }
    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); i++) {
        if (strcmp(kScenarios[i].name, section) == 0) {
            return dumpScenario(assets.get(), kScenarios[i]);
        }
    }
    return std::string();
}

int runStatsTests(const formats::MdkData& data) {
    failures = 0;
    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); i++) {
        checkGolden(data, kScenarios[i].name);
    }
    checkGolden(data, "slideshow");
    testUnits();
    return failures;
}

} // namespace test
} // namespace mdk
