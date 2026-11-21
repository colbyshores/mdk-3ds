// movie_tests.cpp -- see movie_tests.h. Each section prints what
// tests/scripts/movie_probe/Program.cs prints, in its order and spelling.

#include "movie_tests.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "formats/bni.h"
#include "formats/flc.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/gif.h"
#include "formats/mdk_data.h"
#include "formats/mve.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "game/end_movie.h"
#include "game/game_settings.h"
#include "game/intro_splash.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/screen_view.h"
#include "game/video_player.h"

namespace mdk {
namespace test {
namespace {

int failures = 0;

// The game's fixed step (C# `1f / 60f`).
const float kStep = 1.0f / 60.0f;

std::string f(float value) {
    return formatShortestFloat(value);
}

std::string num(long value) {
    char text[32];
    snprintf(text, sizeof(text), "%ld", value);
    return text;
}

// FNV-1a, 32 bits, as the probe's Fnv.
std::string fnv(const uint8_t* bytes, size_t count) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    char text[16];
    snprintf(text, sizeof(text), "%08x", (unsigned)h);
    return text;
}

std::string fnv(const std::vector<uint8_t>& bytes) {
    return fnv(bytes.data(), bytes.size());
}

// The probe's FnvAudio: each sample's two floats' bits, little-endian.
std::string fnvAudio(const std::vector<formats::Mve::AudioSample>& audio) {
    std::vector<uint8_t> bytes(audio.size() * 8);
    for (size_t i = 0; i < audio.size(); i++) {
        uint32_t left;
        uint32_t right;
        memcpy(&left, &audio[i].left, 4);
        memcpy(&right, &audio[i].right, 4);
        for (int b = 0; b < 4; b++) {
            bytes[i * 8 + (size_t)b] = (uint8_t)(left >> (8 * b));
            bytes[i * 8 + 4 + (size_t)b] = (uint8_t)(right >> (8 * b));
        }
    }
    return fnv(bytes);
}

bool endsWithMve(const char* name) {
    const size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".MVE") == 0;
}

// --- decode ----------------------------------------------------------------------
std::string dumpDecode(const formats::MdkData& data) {
    const char* movies[] = {"MISC/FLIC/MDK12.FLC", "MISC/FLIC/MDKEND.FLC", "MISC/FLIC/pie.flc",
                            "MISC/FLIC/shiny.flc", "MISC/FLIC/MDKBZK.MVE"};
    std::string out;
    for (size_t m = 0; m < sizeof(movies) / sizeof(movies[0]); m++) {
        const char* name = movies[m];
        std::vector<uint8_t> bytes;
        if (!data.read(name, &bytes)) {
            out += std::string("== ") + name + ": missing\n";
            continue;
        }
        if (endsWithMve(name)) {
            formats::Mve mve;
            if (!formats::Mve::load(bytes.data(), (int32_t)bytes.size(), &mve)) {
                out += std::string("== ") + name + ": not an MVE\n";
                continue;
            }
            out += std::string("== ") + name + ": mve\n";
            while (mve.nextFrame() == formats::Mve::FrameStatus::Decoded) {
                out += "frame " + num(mve.frame()) + " " + num(mve.width()) + "x" +
                       num(mve.height()) + " time=" + num(mve.frameTime()) +
                       " rate=" + num(mve.sampleRate()) + " channels=" + num(mve.channels()) +
                       " indices=" + fnv(mve.indices(), (size_t)mve.indexCount()) +
                       " palette=" + fnv(mve.palette(), 768) +
                       " audio=" + num((long)mve.audio().size()) + " sound=" +
                       fnvAudio(mve.audio()) + "\n";
            }
            out += "end frame=" + num(mve.frame()) + "\n";
            continue;
        }
        formats::Flc flc;
        if (!formats::Flc::load(bytes.data(), (int32_t)bytes.size(), &flc)) {
            out += std::string("== ") + name + ": not an FLC\n";
            continue;
        }
        out += std::string("== ") + name + ": flc " + num(flc.width()) + "x" + num(flc.height()) +
               " frames=" + num(flc.frameCount()) + " speed=" + num(flc.speed()) + "\n";
        while (flc.nextFrame() == formats::Flc::FrameStatus::Decoded) {
            out += "frame " + num(flc.frame()) + " indices=" +
                   fnv(flc.indices(), (size_t)flc.indexCount()) +
                   " palette=" + fnv(flc.palette(), 768) + "\n";
        }
        out += "end frame=" + num(flc.frame()) + "\n";
        flc.rewind();
        out += "rewind frame=" + num(flc.frame()) +
               " indices=" + fnv(flc.indices(), (size_t)flc.indexCount()) + "\n";
        flc.nextFrame();
        out += "again frame=" + num(flc.frame()) +
               " indices=" + fnv(flc.indices(), (size_t)flc.indexCount()) +
               " palette=" + fnv(flc.palette(), 768) + "\n";
    }
    return out;
}

bool loadOptions(const formats::MdkData& data, formats::Bni* options) {
    std::vector<uint8_t> bytes;
    return data.read("MISC/OPTIONS.BNI", &bytes) && formats::Bni::parse(options, std::move(bytes));
}

// --- splash ----------------------------------------------------------------------
std::string dumpSplash(const formats::MdkData& data) {
    std::string out;
    formats::Bni options;
    if (!loadOptions(data, &options)) {
        return "OPTIONS.BNI did not load\n";
    }
    const formats::ArchiveIndex::Entry* entry = options.index().find("INTRO1A");
    if (!entry) {
        return "no INTRO1A\n";
    }
    const uint8_t* bytes = options.bytes().data() + entry->offset;
    const uint8_t* one = bytes;
    const uint8_t* two = bytes + 768;
    uint8_t black[768] = {};
    const std::vector<uint8_t> picture =
        menu::IntroSplash::unpack(bytes, entry->size, 1536, 600 * 360);
    out += "== INTRO1A: " + num(entry->size) + " bytes palette1=" + fnv(one, 768) +
           " palette2=" + fnv(two, 768) + " picture=" + fnv(picture) + "\n";
    const uint8_t crafted[] = {3, 7, 254, 1, 2, 2, 9, 253, 4, 5, 6, 0, 5, 5};
    const std::vector<uint8_t> full = menu::IntroSplash::unpack(crafted, sizeof(crafted), 0, 16);
    const std::vector<uint8_t> shortened =
        menu::IntroSplash::unpack(crafted, sizeof(crafted), 0, 4);
    std::string hex;
    for (size_t i = 0; i < full.size(); i++) {
        char pair[4];
        snprintf(pair, sizeof(pair), "%02X", full[i]);
        hex += pair;
    }
    out += "crafted " + fnv(full) + " short " + fnv(shortened) + " " + hex + "\n";
    const float ts[] = {0.0f,  0.001f, 0.003f, 0.25f, 0.5f,
                        0.5019531f, 0.6f, 0.75f, 0.998f, 1.0f};
    for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
        uint8_t a[768];
        uint8_t b[768];
        uint8_t c[768];
        menu::IntroSplash::blend(black, one, ts[i], 768, a);
        menu::IntroSplash::blend(one, two, ts[i], 768, b);
        menu::IntroSplash::blend(two, black, ts[i], 768, c);
        out += "blend t=" + f(ts[i]) + " black-one=" + fnv(a, 768) + " one-two=" + fnv(b, 768) +
               " two-black=" + fnv(c, 768) + "\n";
    }
    for (int32_t frame = 200; frame <= 265; frame++) {
        out += "flash " + num(frame) + " " + f(menu::EndMovie::flash(frame)) + "\n";
    }
    return out;
}

// The probe's Video(): the player's state and its image.
std::string video(const menu::VideoPlayer& player) {
    std::string image = "none";
    if (player.hasStill()) {
        image = num(player.width()) + "x" + num(player.height()) + " " + fnv(player.indices()) +
                " " + fnv(player.shownPaletteRgb(), 768);
    }
    const int32_t frame = player.flcFrame();
    return std::string("playing=") + (player.playing() ? "1" : "0") +
           " flc=" + (frame < 0 ? std::string("-") : num(frame)) + " time=" + f(player.time()) +
           " hold=" + f(player.holdTime()) + " bright=" + f(player.brightness()) +
           " image=" + image;
}

// "key:frames,...": the key on the first frame, then "-" (the probe's Schedule).
std::vector<std::string> schedule(const char* text) {
    std::vector<std::string> keys;
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
        for (int i = 0; i < count; i++) {
            keys.push_back(i == 0 ? key : std::string("-"));
        }
        at = comma + 1;
    }
    return keys;
}

menu::Keys keysOf(const std::string& key) {
    menu::Keys keys;
    if (key == "down") {
        keys.down = true;
        keys.any = true;
    } else if (key == "accept") {
        keys.accept = true;
        keys.any = true;
    } else if (key == "back") {
        keys.back = true;
        keys.any = true;
    }
    return keys;
}

const char* eventName(flow::Event event) {
    switch (event) {
        case flow::Event::None: return "None";
        case flow::Event::Quit: return "Quit";
        case flow::Event::Menu: return "Menu";
        case flow::Event::Play: return "Play";
        case flow::Event::Briefing: return "Briefing";
        case flow::Event::Statistics: return "Statistics";
        case flow::Event::LevelEnded: return "LevelEnded";
        case flow::Event::GameOver: return "GameOver";
        case flow::Event::KurtDied: return "KurtDied";
        case flow::Event::GameFinished: return "GameFinished";
        case flow::Event::Fall: return "Fall";
        case flow::Event::Stream: return "Stream";
        case flow::Event::StreamEnded: return "StreamEnded";
    }
    return "?";
}

void noApply(void*) {}
void noPrint(void*, const char*) {}

// --- menu ------------------------------------------------------------------------
std::string dumpMenu(const formats::MdkData& data) {
    std::string out;
    formats::Bni options;
    loadOptions(data, &options);
    std::vector<uint8_t> ftiBytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    formats::Fti* fti = new formats::Fti();
    formats::Font* big = new formats::Font();
    formats::Font* small = new formats::Font();
    if (!data.read("MISC/MDKFONT.FTI", &ftiBytes) ||
        !formats::Fti::parse(fti, ftiBytes.data(), (int32_t)ftiBytes.size()) ||
        !fti->getBytes("FONTBIG", &bigBytes) || !fti->getBytes("FONTSML", &smallBytes) ||
        !formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, big) ||
        !formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, small)) {
        delete fti;
        delete big;
        delete small;
        return "MDKFONT.FTI did not load\n";
    }
    struct Run {
        const char* label;
        const char* schedule;
    };
    const Run runs[] = {{"no key", "-:800"},
                        {"a key skips the splash", "-:30,accept:20,down:60,-:100"}};
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        out += std::string("== menu: ") + runs[r].label + "\n";
        flow::GameState state;
        state.splash = true;
        GameSettings settings;
        menu::MainMenu* menu = new menu::MainMenu();
        menu->items().setFonts(big, small);
        menu->items().setHost(menu::Host::Oracle);
        menu->items().setSettings(&settings, noApply, nullptr);
        menu->items().setPrint(noPrint, nullptr);
        menu->setMedia(&options, &data);
        menu->open(fti, &state, menu::MenuSaves(), false, menu::MenuPage::Main);
        const std::vector<std::string> keys = schedule(runs[r].schedule);
        for (size_t frame = 0; frame < keys.size(); frame++) {
            const flow::Event next = menu->frame(keysOf(keys[frame]), kStep);
            menu::IntroSplash& splash = menu->splash();
            const uint8_t* palette = splash.palette();
            const std::string splashText =
                std::string(splash.done() ? "1" : "0") + " step=" + num(splash.step()) +
                " time=" + f(splash.time()) +
                " pal=" + (palette ? fnv(palette, 768) : std::string("-"));
            out += "f" + num((long)frame) + " " + keys[frame] + " status=\"" + menu->status() +
                   "\" next=" + eventName(next) + " splash=" + splashText +
                   " sel=" + num(menu->items().selected()) + " " + video(menu->video()) + "\n";
        }
        delete menu;
    }
    delete fti;
    delete big;
    delete small;
    return out;
}

// --- endmovie --------------------------------------------------------------------
// FINISH.BNI's sounds as a recorder: each play a new voice id (the C#'s muted
// device numbers its voices), each call kept for the frame's line.
struct SoundLog {
    int32_t nextVoice = 1;
    std::vector<std::string> played;
    std::vector<int32_t> stopped;
};

int32_t logPlay(void* user, const char* name, bool) {
    SoundLog* log = static_cast<SoundLog*>(user);
    log->played.push_back(name);
    return log->nextVoice++;
}

void logStop(void* user, int32_t voice) {
    static_cast<SoundLog*>(user)->stopped.push_back(voice);
}

std::string sounds(SoundLog* log) {
    std::sort(log->played.begin(), log->played.end());
    std::string text = "[";
    for (size_t i = 0; i < log->played.size(); i++) {
        text += (i ? " " : "") + log->played[i];
    }
    log->played.clear();
    return text + "]";
}

std::string dumpEndMovie(const formats::MdkData& data) {
    std::string out = "== end movie: the data's\n";
    {
        SoundLog log;
        menu::EndSounds hooks;
        hooks.play = logPlay;
        hooks.stop = logStop;
        hooks.user = &log;
        menu::EndMovie* movie = new menu::EndMovie();
        movie->player().setData(&data);
        movie->open(hooks);
        const char* keys[] = {"-", "-", "back"};
        for (size_t i = 0; i < 3; i++) {
            const flow::Event next = movie->frame(strcmp(keys[i], "back") == 0, kStep);
            out += std::string(keys[i]) + " status=\"" + movie->status() + "\" next=" +
                   eventName(next) + " " + video(movie->player()) + "\n";
        }
        movie->close();
        delete movie;
    }
    struct Run {
        const char* label;
        const char* schedule;
        bool stopOnEvent;
    };
    const Run runs[] = {{"pie.flc as MDKEND.FLC", "-:740", false},
                        {"Back gives up", "-:100,back:3", true}};
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        out += std::string("== end movie: ") + runs[r].label + "\n";
        SoundLog log;
        menu::EndSounds hooks;
        hooks.play = logPlay;
        hooks.stop = logStop;
        hooks.user = &log;
        menu::EndMovie* movie = new menu::EndMovie();
        movie->player().setData(&data);
        movie->open(hooks);
        movie->playForTest("MISC/FLIC/pie.flc");
        out += "start sounds=" + sounds(&log) + " " + video(movie->player()) + "\n";
        log.stopped.clear();
        const std::vector<std::string> keys = schedule(runs[r].schedule);
        for (size_t frame = 0; frame < keys.size(); frame++) {
            const flow::Event next = movie->frame(keys[frame] == "back", kStep);
            std::string stops;
            for (size_t s = 0; s < log.stopped.size(); s++) {
                if (movie->hasVoice(0) && log.stopped[s] == movie->voice(0)) {
                    stops = " stop=DOGSHIP";
                }
            }
            log.stopped.clear();
            out += "f" + num((long)frame) + " " + keys[frame] + " next=" + eventName(next) +
                   " status=\"" + movie->status() + "\" sounds=" + sounds(&log) + stops + " " +
                   video(movie->player()) + "\n";
            if (next != flow::Event::None && runs[r].stopOnEvent) {
                break;
            }
        }
        movie->close();
        delete movie;
    }
    return out;
}

// --- loading ---------------------------------------------------------------------
std::string dumpLoading(const formats::MdkData& data) {
    // The probe's renderer: a 1280 x 960 window, so a 480 x 360 canvas.
    const float canvasWidth = 480.0f;
    ScreenView view;
    view.layout(canvasWidth, 360.0f, ScreenView::kWidth, ScreenView::kHeight, true);
    std::string out = "== loading: canvas " + f(canvasWidth) + "x360 scale=" + f(view.scale) +
                      " origin=" + f(view.originX) + "," + f(view.originY) + "\n";
    std::vector<uint8_t> ftiBytes;
    std::vector<uint8_t> bigBytes;
    formats::Fti* fti = new formats::Fti();
    formats::Font* big = new formats::Font();
    if (data.read("MISC/MDKFONT.FTI", &ftiBytes) &&
        formats::Fti::parse(fti, ftiBytes.data(), (int32_t)ftiBytes.size()) &&
        fti->getBytes("FONTBIG", &bigBytes) &&
        formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, big)) {
        const std::vector<uint8_t> text = fti->getTextBytes("LOAD_MSG");
        const int32_t width = big->width(text.data(), (int32_t)text.size());
        out += "text \"" + std::string(text.begin(), text.end()) + "\" width=" + num(width) +
               " x=" + f((ScreenView::kWidth - (float)width) / 2.0f) +
               " y=" + f(LoadingScreen::kTextY) + "\n";
    }
    delete fti;
    delete big;
    // LoadingScreen's own constants: the bar, its frame's width (the view's scale)
    // and the picture's place (its size is the LBB's, 200 x 200).
    out += "bar " + f(view.toCanvasX(LoadingScreen::kBarX)) + "," +
           f(view.toCanvasY(LoadingScreen::kBarY)) + " " +
           f(LoadingScreen::kBarWidth * view.scale) + "x" +
           f(LoadingScreen::kBarHeight * view.scale) + " frame=" + f(view.scale) + "\n";
    out += "image " + f(view.toCanvasX(LoadingScreen::kImageX)) + "," +
           f(view.toCanvasY(LoadingScreen::kImageY)) + " " + f(200.0f * view.scale) + "x" +
           f(200.0f * view.scale) + "\n";
    for (int32_t level = 1; level <= 8; level++) {
        char path[32];
        snprintf(path, sizeof(path), "MISC/LOAD_%d.LBB", (int)level);
        std::vector<uint8_t> bytes;
        if (!data.read(path, &bytes)) {
            out += "LOAD_" + num(level) + ": missing\n";
            continue;
        }
        char name[16];
        snprintf(name, sizeof(name), "LOAD_%d", (int)level);
        const formats::Texture texture =
            formats::Texture::parse(name, bytes.data(), (int32_t)bytes.size(), 768);
        const formats::Palette palette = formats::Palette::fromRgb(bytes.data(), 768);
        const uint8_t* rgba = palette.rgba();
        auto colour = [rgba](int i) {
            return num(rgba[i * 4]) + "," + num(rgba[i * 4 + 1]) + "," + num(rgba[i * 4 + 2]);
        };
        out += "LOAD_" + num(level) + ": " + num((long)bytes.size()) + " bytes " +
               num(texture.width()) + "x" + num(texture.height()) + " indices=" +
               fnv(texture.indices(), (size_t)texture.width() * (size_t)texture.height()) +
               " palette=" + fnv(rgba, 1024) + " fill=" + colour(LoadingScreen::kBarFill) +
               " frame=" + colour(LoadingScreen::kBarFrame) + "\n";
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
    snprintf(path, sizeof(path), "golden/movie_%s.txt", section);
    std::string expected;
    if (!readGolden(path, &expected)) {
        printf("  FAIL: no golden file %s\n", path);
        failures++;
        return;
    }
    const std::string got = movieDump(data, section);
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
    printf("  FAIL: %s differs from the C# probe at byte %lu: oracle \"%.120s\", port \"%.120s\"\n",
           path, (unsigned long)at, expected.c_str() + lineStart,
           lineStart < got.size() ? got.c_str() + lineStart : "");
    failures++;
}

// --- the slideshow's start (the integration of #20b and #20c) ----------------------
// C# MainMenu: `_video.Finished += _slideshow.Start`. No C# probe prints it (the
// movie probe's menu runs end before the slideshow's first 5 s), so this pins the
// hookup against Slideshow.cs's own rules: MDK12.FLC ends -> the slideshow runs on
// its last frame (image 0); 5 s later the bare MDKS_001 is on the player and the
// items hide ("main menu: slideshow"); a key there gives way to the next slide on
// the next frame, the items back, and the key chooses nothing.
void expectTrue(const char* what, bool ok) {
    if (!ok) {
        printf("  FAIL: slideshow: %s\n", what);
        failures++;
    }
}

void checkSlideshowStart(const formats::MdkData& data) {
    formats::Bni options;
    loadOptions(data, &options);
    std::vector<uint8_t> ftiBytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    formats::Fti* fti = new formats::Fti();
    formats::Font* big = new formats::Font();
    formats::Font* small = new formats::Font();
    if (!data.read("MISC/MDKFONT.FTI", &ftiBytes) ||
        !formats::Fti::parse(fti, ftiBytes.data(), (int32_t)ftiBytes.size()) ||
        !fti->getBytes("FONTBIG", &bigBytes) || !fti->getBytes("FONTSML", &smallBytes) ||
        !formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, big) ||
        !formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, small)) {
        printf("  FAIL: slideshow: MDKFONT.FTI did not load\n");
        failures++;
        delete fti;
        delete big;
        delete small;
        return;
    }
    flow::GameState state;
    state.splash = false;
    GameSettings settings;
    menu::MainMenu* menu = new menu::MainMenu();
    menu->items().setFonts(big, small);
    menu->items().setHost(menu::Host::Oracle);
    menu->items().setSettings(&settings, noApply, nullptr);
    menu->items().setPrint(noPrint, nullptr);
    menu->setMedia(&options, &data);
    menu->open(fti, &state, menu::MenuSaves(), false, menu::MenuPage::Main);
    const menu::Keys none;
    int frames = 0;
    menu->frame(none, kStep);
    expectTrue("MDK12.FLC plays behind the menu", menu->video().playing());
    expectTrue("no slideshow while it plays", !menu->slideshow().running());
    while (menu->video().playing() && frames < 2000) {
        menu->frame(none, kStep);
        frames++;
    }
    expectTrue("the video ends", !menu->video().playing());
    expectTrue("its end starts the slideshow", menu->slideshow().running());
    expectTrue("on the video's last frame (image 0)", menu->slideshow().image() == 0);
    expectTrue("the slides are loaded", menu->slideshow().slideCount() >= 2);
    expectTrue("the items show over the last frame", menu->items().visible);
    const std::vector<uint8_t> lastFrame = menu->video().indices();
    int waited = 0;
    while (menu->slideshow().image() == 0 && waited < 400) {
        menu->frame(none, kStep);
        waited++;
    }
    expectTrue("5 s on the last frame (FirstDelay)", waited >= 298 && waited <= 301);
    expectTrue("then the bare first slide", menu->slideshow().image() == 1);
    const formats::Gif* first = menu->slideshow().gif(1);
    expectTrue("shown on the player (ShowStill)",
               first != nullptr && menu->video().hasStill() &&
                   menu->video().width() == first->width() &&
                   menu->video().height() == first->height() &&
                   menu->video().indices().size() == (size_t)first->indexCount() &&
                   memcmp(menu->video().indices().data(), first->indices(),
                          (size_t)first->indexCount()) == 0);
    expectTrue("without the items", !menu->items().visible);
    expectTrue("its status", menu->status() == "main menu: slideshow");
    menu::Keys key;
    key.accept = true;
    key.any = true;
    const flow::Event chose = menu->frame(key, kStep);
    expectTrue("the key on the bare slide chooses nothing", chose == flow::Event::None);
    menu->frame(none, kStep);
    expectTrue("and gives way to the next slide at once (Reset)",
               menu->slideshow().image() == 2);
    expectTrue("the items show again", menu->items().visible);
    // Past the last slide the video's last frame comes back (image 0).
    const int32_t count = menu->slideshow().slideCount();
    for (int i = 0; i < (count + 1) * 2 * 60 + 60 && menu->slideshow().image() != 0; i++) {
        menu->frame(none, kStep);
    }
    expectTrue("past the last slide, the video's last frame again",
               menu->slideshow().image() == 0 && menu->video().indices() == lastFrame);
    delete menu;
    delete fti;
    delete big;
    delete small;
}

} // namespace

std::string movieDump(const formats::MdkData& data, const char* section) {
    if (strcmp(section, "decode") == 0) return dumpDecode(data);
    if (strcmp(section, "splash") == 0) return dumpSplash(data);
    if (strcmp(section, "menu") == 0) return dumpMenu(data);
    if (strcmp(section, "endmovie") == 0) return dumpEndMovie(data);
    if (strcmp(section, "loading") == 0) return dumpLoading(data);
    return std::string();
}

int runMovieTests(const formats::MdkData& data) {
    failures = 0;
    const char* sections[] = {"decode", "splash", "menu", "endmovie", "loading"};
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        checkGolden(data, sections[i]);
    }
    checkSlideshowStart(data);
    return failures;
}

} // namespace test
} // namespace mdk
