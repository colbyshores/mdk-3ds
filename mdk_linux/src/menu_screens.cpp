// menu_screens.cpp -- see menu_screens.h. The C#'s Game.Run loop (Game.cs:284-364)
// for the two screens this frontend shows besides the level: the elapsed, the
// presses, the screen's frame, its event handled (Game.Handle) -- with the
// screen's own Frame order inside (MainMenu.Frame, StatsScreen.Frame).

#include "menu_screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <memory>

#include "formats/bni.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/file_system.h"
#include "formats/palette.h"
#include "game/briefing.h"
#include "game/font_view.h"
#include "game/input.h"
#include "game/main_menu.h"
#include "game/viewer_saves.h"
#include "game/screen_view.h"

namespace mdk {
namespace {

void printLine(void*, const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);
}

// C# Ui.ApplySettings: applied (the level reads them as it starts) and saved.
void applied(void* user) {
    FrontScreens* front = static_cast<FrontScreens*>(user);
    saveSettings(*front->settings, front->settingsFile);
}

menu::Keys menuKeysOf(const MenuKeyState& state) {
    menu::Keys keys;
    keys.up = state.up;
    keys.down = state.down;
    keys.left = state.left;
    keys.right = state.right;
    keys.accept = state.accept;
    keys.back = state.back;
    keys.leftHeld = state.leftHeld;
    keys.rightHeld = state.rightHeld;
    keys.any = state.any;
    return keys;
}

// MDKFONT.FTI's two fonts in SYS_PAL (C# Fonts), the texts, and SYS_PAL's bytes.
struct UiAssets {
    formats::Fti fti;
    std::unique_ptr<formats::Font> bigFont{new formats::Font()};
    std::unique_ptr<formats::Font> smallFont{new formats::Font()};
    std::unique_ptr<FontView> big{new FontView()};
    std::unique_ptr<FontView> small{new FontView()};
    std::vector<uint8_t> systemRgb;
    formats::Bni options;
    formats::Bni stats;
    PalettedImage background;
    bool ready = false;
};

bool loadAssets(MdkRenderer renderer, const formats::MdkData& data, UiAssets* ui) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    if (!data.read("MISC/MDKFONT.FTI", &bytes) ||
        !formats::Fti::parse(&ui->fti, bytes.data(), (int32_t)bytes.size()) ||
        !ui->fti.getBytes("FONTBIG", &bigBytes) || !ui->fti.getBytes("FONTSML", &smallBytes) ||
        !ui->fti.getBytes("SYS_PAL", &ui->systemRgb) ||
        !formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, ui->bigFont.get()) ||
        !formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4,
                              ui->smallFont.get())) {
        std::printf("[menu] MDKFONT.FTI's fonts did not load\n");
        return false;
    }
    const formats::Palette palette =
        formats::Palette::fromRgb(ui->systemRgb.data(), (int32_t)ui->systemRgb.size());
    // C# StatsScreen: `_systemRgb = new byte[768]`, SYS_PAL copied in (it may be
    // shorter), so the page palettes' system colours are always 192 bytes.
    ui->systemRgb.resize(768, 0);
    const int32_t paletteId = mdk_render_create_palette(renderer, palette.rgba());
    if (paletteId < 0 || !ui->big->load(renderer, *ui->bigFont, paletteId, palette.rgba()) ||
        !ui->small->load(renderer, *ui->smallFont, paletteId, palette.rgba())) {
        std::printf("[menu] the font atlases did not upload\n");
        return false;
    }
    // MDKOPT: the main menu's background when the video is missing (the C#'s
    // fallback, and the only picture behind it here: MDK12.FLC is #20c's).
    std::vector<uint8_t> optionsBytes;
    if (data.read("MISC/OPTIONS.BNI", &optionsBytes) &&
        formats::Bni::parse(&ui->options, std::move(optionsBytes))) {
        loadPalettedImage(renderer, ui->options, "MDKOPT", nullptr, &ui->background);
    }
    std::vector<uint8_t> statsBytes;
    if (data.read("MISC/STATS.BNI", &statsBytes)) {
        formats::Bni::parse(&ui->stats, std::move(statsBytes));
    }
    ui->ready = true;
    return true;
}

} // namespace

std::string settingsPath() {
    const char* user = getenv("MDK_USER_DIR");
    if (user && user[0] != '\0') {
        return std::string(user) + "/settings.cfg";
    }
    return "settings.cfg";
}

GameSettings loadSettings(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return GameSettings();
    }
    std::string text;
    char chunk[4096];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text.append(chunk, got);
    }
    fclose(file);
    std::printf("[menu] settings: %s loaded\n", path.c_str());
    return GameSettings::parse(text.c_str());
}

bool saveSettings(const GameSettings& settings, const std::string& path) {
    const std::string text = settings.format();
    FILE* file = fopen(path.c_str(), "wb");
    bool written = file != nullptr;
    if (file) {
        written = fwrite(text.data(), 1, text.size(), file) == text.size();
        written = fclose(file) == 0 && written;
    }
    if (!written) {
        std::fprintf(stderr, "Settings not saved: %s\n", path.c_str());
    }
    return written;
}

// The main menu's view of the saves (package #19): the C#'s SaveGames.In(user)
// behind MainMenu's List/Read/Load.
static menu::MenuSaves menuSavesOf(const flow::SaveGames* saves) {
    menu::MenuSaves hooks;
    if (saves != nullptr) {
        hooks.list = flow::menuListSaves;
        hooks.peek = flow::menuPeekSave;
        hooks.load = flow::menuLoadSave;
        hooks.user = const_cast<flow::SaveGames*>(saves);
    }
    return hooks;
}

flow::Screen runFrontScreens(FrontScreens* front) {
    flow::GameFlow& flow = *front->flow;
    flow::Screen screen = flow.screen();
    const MdkScope scope = mdk_render_mark(front->renderer);
    const formats::MdkData data = formats::MdkData::find(formats::fileSystem());
    std::unique_ptr<UiAssets> ui(new UiAssets());
    if (!data.found() || !loadAssets(front->renderer, data, ui.get())) {
        mdk_render_release(front->renderer, scope);
        front->exitReason = "the menus' assets did not load";
        return flow::Screen::None;
    }

    menu::MainMenu mainMenu;
    Briefing briefing;
    PalettedImage map;
    // Opens the screen the flow shows (C# Show: a new MainMenu / StatsScreen).
    auto open = [&](flow::Screen shown) {
        if (shown == flow::Screen::Menu) {
            mainMenu = menu::MainMenu();
            menu::MenuItems& items = mainMenu.items();
            items.setFonts(ui->bigFont.get(), ui->smallFont.get());
            items.setHost(menu::Host::Console);
            items.setSettings(front->settings, applied, front);
            items.setPrint(printLine, nullptr);
            std::printf("[main] flow: the main menu\n");
            mainMenu.open(&ui->fti, &flow.state(), menuSavesOf(front->saves), false,
                          menu::MenuPage::Main);
        } else if (shown == flow::Screen::Briefing) {
            briefing.open(&ui->fti, flow.state().level);
            char name[24];
            snprintf(name, sizeof(name), "L%d_MAP", (int)(briefing.index() + 1));
            if (map.ready()) {
                mdk_render_release_texture(front->renderer, map.texture);
                mdk_render_release_palette(front->renderer, map.palette);
            }
            loadPalettedImage(front->renderer, ui->stats, name, ui->systemRgb.data(), &map);
            std::printf("[main] flow: the briefing of level %d (%s)\n", (int)flow.state().level,
                        name);
        }
    };
    open(screen);

    auto last = std::chrono::steady_clock::now();
    Input input;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed =
            front->fixedStep ? 1.0f / 60.0f : std::chrono::duration<float>(now - last).count();
        last = now;
        *front->time += elapsed;
        if (!front->headless && !windowPump(front->window, &input)) {
            front->exitReason = "window closed";
            screen = flow::Screen::None;
            break;
        }
        MenuKeyState state;
        windowTakeMenuKeys(&state);
        menu::Keys keys = menuKeysOf(state);
        // The text typed this frame (C# Input.Typed): the main menu's Y/N on "Really
        // Quit?" and SEETHEWHOLEGAME's debug keys (MainMenu.HandleKeys, package #21).
        // Taken on every screen, so nothing typed here reaches the level's cheats.
        uint32_t typingPresses = 0;
        windowTakeTyping(&keys.typed, &typingPresses);
        if (front->presses) {
            front->presses->apply(*front->time, &keys);
        }

        flow::Event next = flow::Event::None;
        if (screen == flow::Screen::Menu) {
            next = mainMenu.frame(keys, elapsed);
        } else {
            next = briefing.frame(keys.any, keys.back,
                                  input.isDown(kKeyFire) || input.isDown(kKeyJump), elapsed);
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *front->frame;
        params.clear_colour = 0xFF000000u;
        params.present = true;
        const bool capture =
            front->captureFrame > 0 && (int)*front->frame + 1 == front->captureFrame;
        params.screenshot_path = capture ? front->capturePath : nullptr;
        mdk_render_begin_frame(front->renderer, &params);
        mdk_render_canvas_begin(front->renderer);
        ScreenView view;
        view.layoutInside(mdk_render_canvas_width(front->renderer), kCanvasHeight);
        if (screen == flow::Screen::Menu) {
            view.image(front->renderer, ui->background.texture, ui->background.palette,
                       ui->background.width, ui->background.height, 0.0f, 0.0f);
            mainMenu.items().draw(front->renderer, view, *ui->big, *ui->small);
        } else {
            briefing.draw(front->renderer, view, *ui->big, map.texture, map.palette, map.width,
                          map.height);
        }
        mdk_render_canvas_end(front->renderer);
        mdk_render_end_frame(front->renderer);
        (*front->frame)++;
        if (capture) {
            front->exitReason = "capture taken";
            screen = flow::Screen::None;
            break;
        }
        if (front->maxFrames > 0 && (int)*front->frame >= front->maxFrames) {
            front->exitReason = "frame budget reached";
            screen = flow::Screen::None;
            break;
        }
        if (next == flow::Event::None) {
            continue;
        }
        // C# Game.Handle, then the screens this port lacks skipped (the fall).
        flow::Screen shown = flow.handle(next);
        flow::GameFlow::SkipTrail trail;
        shown = flow.skipUnported(&trail);
        for (int32_t i = 0; i < trail.count; i++) {
            std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                        flow::GameFlow::screenName(trail.skipped[i]),
                        (int)flow::GameFlow::screenPackage(trail.skipped[i]));
        }
        if (shown == flow::Screen::Menu || shown == flow::Screen::Briefing) {
            screen = shown;
            open(screen);
            continue;
        }
        if (shown == flow::Screen::None) {
            front->exitReason = "menu Quit";
        } else if (shown != flow::Screen::Level) {
            std::printf("[main] flow: %s follows, which this frontend cannot show; exiting\n",
                        flow::GameFlow::screenName(shown));
            front->exitReason = "the flow asked for a screen this frontend cannot show";
            shown = flow::Screen::None;
        }
        screen = shown;
        break;
    }
    mdk_render_release(front->renderer, scope);
    return screen;
}

} // namespace mdk
