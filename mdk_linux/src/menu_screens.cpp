// menu_screens.cpp -- see menu_screens.h. The C#'s Game.Run loop (Game.cs:284-364)
// for the screens this frontend shows besides the level: the elapsed, the
// presses, the screen's frame, its event handled (Game.Handle) -- with the
// screen's own Frame order inside (MainMenu.Frame, StatsScreen.Frame,
// FallScreen.Frame), and the sound device's update after it (Game.cs:334-337).

#include "menu_screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <memory>
#include <string>

#include "audio/audio_sdl.h"
#include "fall/fall_screen.h"
#include "formats/bni.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/file_system.h"
#include "formats/palette.h"
#include "game/end_movie.h"
#include "game/font_view.h"
#include "game/input.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/movie_view.h"
#include "game/viewer_saves.h"
#include "game/screen_view.h"
#include "game/stats_screen.h"

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
    // fallback; MDK12.FLC plays over it, package #20c). OPTIONS.BNI also holds
    // the INTRO1A splash the menu shows first.
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

    std::unique_ptr<menu::MainMenu> mainMenu(new menu::MainMenu());
    // THE STATISTICS AND THE BRIEFING (package #20b): the C#'s StatsScreen, all
    // its pages after a level (StatsPages.All), the briefing alone for a new game
    // or a save made before a level (StatsPages.Briefing). Made when the flow
    // shows it; closed (its voices stopped) when the flow leaves it. Its renderer
    // objects (the pages' images, the heads) are this function's scope's.
    std::unique_ptr<StatsScreen> stats(new StatsScreen());
    bool statsOpen = false;
    bool textInput = false;
    bool promptOpen = false;
    auto closeStats = [&]() {
        if (statsOpen) {
            stats->close();
            statsOpen = false;
        }
    };
    auto openStats = [&](flow::Screen shown) {
        closeStats();
        const bool all = shown == flow::Screen::Statistics;
        StatsScreen::Setup setup;
        setup.fti = &ui->fti;
        setup.stats = &ui->stats;
        setup.systemRgb = ui->systemRgb.data();
        setup.state = &flow.state();
        if (front->saves) {
            setup.saves = *front->saves;
        }
        setup.pages = all ? StatsPages::All : StatsPages::Briefing;
        setup.first = all && front->statsPhase > 0 ? (StatsScreen::Phase)front->statsPhase
                                                   : StatsScreen::Phase::None;
        setup.gore = front->settings->gore;
        // The C#'s `new Random()` picks only sounds: the clock, or 1 in a test.
        setup.seed = front->fixedStep
                         ? 1u
                         : (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
        setup.print.line = printLine;
        stats->open(setup);
        if (all) {
            stats->attach(front->renderer, data);
        }
        // The screen plays on its own device (C# _ui.Audio); a headless run keeps
        // the muted device's accounting, as the C#'s hidden tests do.
        audio::SoundDevice& device = stats->device();
        if (!front->headless && audioIsOpen()) {
            device.setSink(audioSink, nullptr, audioQueued);
        }
        front->settings->applyAudio(&device);
        statsOpen = true;
        if (all) {
            std::printf("[main] flow: the statistics after level %d (from %s)\n",
                        (int)flow.state().level, StatsScreen::phaseName(stats->phase()));
        } else {
            std::printf("[main] flow: the briefing of level %d (L%d_MAP)\n",
                        (int)flow.state().level, (int)(stats->index() + 1));
        }
    };
    // THE FALL (package #17b): made when the flow shows it, closed (its voices
    // stopped, its renderer scope released) when the flow leaves it.
    std::unique_ptr<fall::FallScreen> fall(new fall::FallScreen());
    fall::FallScreen::Print fallPrint;
    fallPrint.line = printLine;
    // C# Show for the fall: a new FallScreen. False when it cannot be shown (its
    // data did not load): the caller skips it the C#'s way.
    auto openFall = [&]() -> bool {
        fall::FallViewOptions options;
        options.lean = front->fallLean;
        // The C#'s `new Random()` is the clock; a test's --seed or --fixed-step is 1.
        const int32_t seed =
            front->fallSeedSet
                ? front->fallSeed
                : (int32_t)std::chrono::steady_clock::now().time_since_epoch().count();
        std::string why;
        if (!fall->open(front->renderer, data, ui->fti, *ui->bigFont, *ui->smallFont,
                        flow.state().level, (Kurt::Difficulty)front->settings->difficulty, seed,
                        options, fallPrint, &why)) {
            std::printf("[main] flow: the fall of level %d did not load (%s)\n",
                        (int)flow.state().level, why.c_str());
            return false;
        }
        std::printf("[main] flow: the fall before level %d%s\n", (int)flow.state().level,
                    front->fallLean ? " (the 3DS's lean way)" : "");
        // The fall's sound to the speakers when the stream is open (a headless or
        // --mute run stays silent), as the level's and the stream's mixers are.
        if (!front->headless && audioIsOpen()) {
            fall->mixer().device().setSink(audioSink, nullptr, audioQueued);
        }
        return true;
    };
    // The flow's screen after an event, past the screens a run skips: the
    // statistics under --skip-statistics (the C#'s skip of them, GameFlow::
    // skipScreen, then the screens this port lacks skipped), and the falls under
    // --skip-falls or one that did not load (FallSim.Skip's carry, then the level).
    auto settle = [&](flow::Screen shown) -> flow::Screen {
        for (int32_t i = 0; i < flow::GameFlow::kMaxSkips; i++) {
            if (shown == flow::Screen::Statistics && front->skipStatistics) {
                std::printf("[main] flow: skipped Statistics (--skip-statistics)\n");
                flow.handle(flow.skipScreen());
                flow::GameFlow::SkipTrail trail;
                shown = flow.skipUnported(&trail);
                for (int32_t k = 0; k < trail.count; k++) {
                    std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                                flow::GameFlow::screenName(trail.skipped[k]),
                                (int)flow::GameFlow::screenPackage(trail.skipped[k]));
                }
                continue;
            }
            if (shown != flow::Screen::Fall) {
                break;
            }
            if (!front->skipFalls && openFall()) {
                break;
            }
            std::printf("[main] flow: skipped Fall (%s)\n",
                        front->skipFalls ? "--skip-falls" : "it did not load");
            shown = flow.handle(flow.skipScreen());
        }
        return shown;
    };
    // Package #20c: the end movies, and the pictures of the splash and the movies
    // (the C#'s PalettedImages; game/movie_view.h -- one picture whose palette
    // follows the blend, as the C# draws it).
    std::unique_ptr<menu::EndMovie> endMovie;
    MovieView movieView;
    SplashView splashView;
    // Opens the screen the flow shows (C# Show: a new MainMenu / StatsScreen /
    // EndMovie). The previous screen's pictures go first (C# Dispose).
    auto open = [&](flow::Screen shown) {
        movieView.release(front->renderer);
        splashView.release(front->renderer);
        if (endMovie) {
            endMovie->close();
            endMovie.reset();
        }
        if (shown == flow::Screen::Menu) {
            mainMenu.reset(new menu::MainMenu());
            menu::MenuItems& items = mainMenu->items();
            items.setFonts(ui->bigFont.get(), ui->smallFont.get());
            items.setHost(menu::Host::Console);
            items.setSettings(front->settings, applied, front);
            items.setPrint(printLine, nullptr);
            mainMenu->setMedia(&ui->options, &data);
            const bool splash = flow.state().splash;
            std::printf("[main] flow: the main menu%s\n",
                        splash ? " (the INTRO1A splash first)" : "");
            mainMenu->open(&ui->fti, &flow.state(), menuSavesOf(front->saves), false,
                           menu::MenuPage::Main);
            splashView.open(front->renderer, mainMenu->splash(), false);
        } else if (shown == flow::Screen::EndMovie) {
            // C# EndMovie: MDKEND.FLC, then MDKBZK.MVE, then the menu. FINISH.BNI's
            // sounds and the MVE's stream voice are not wired (end_movie.h,
            // video_player.h): the GOG data has neither movie.
            endMovie.reset(new menu::EndMovie());
            endMovie->player().setData(&data);
            endMovie->open(menu::EndSounds());
            std::printf("[main] flow: the end movies (%s)\n", endMovie->status());
        } else if (shown == flow::Screen::Briefing || shown == flow::Screen::Statistics) {
            openStats(shown);
        }
        // --phase is the first screen's (C# `_firstLevel ? _options.Phase : null`).
        front->statsPhase = 0;
    };
    screen = settle(screen);
    if (screen != flow::Screen::Menu && screen != flow::Screen::Briefing &&
        screen != flow::Screen::Statistics && screen != flow::Screen::Fall &&
        screen != flow::Screen::EndMovie) {
        mdk_render_release(front->renderer, scope);
        return screen;
    }
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
        if (screen == flow::Screen::Fall) {
            // C# FallScreen.Frame: Esc (MenuKey.Back) skips, the keys steer.
            fall::FallSim::Input steer = fall::FallScreen::steering(input, keys.back);
            if (front->fallChase) {
                fall::FallScreen::chase(fall->sim(), &steer);
            }
            fall->frame(steer, elapsed);
        } else if (screen == flow::Screen::Menu) {
            next = mainMenu->frame(keys, elapsed);
            movieView.sync(front->renderer, mainMenu->video(), false);
        } else if (screen == flow::Screen::EndMovie) {
            // C# EndMovie.Frame: Esc gives up.
            next = endMovie->frame(keys.back, elapsed);
            movieView.sync(front->renderer, endMovie->player(), false);
        } else {
            // C# StatsScreen.Frame: any key, Esc skips, Fire or Jump held runs it
            // fast; the save prompt between its pages reads the menu keys and
            // what is typed (SavePrompt.Update).
            // The typing was taken once above (package #21: every screen takes it).
            const std::string& typed = keys.typed;
            const uint32_t typing = typingPresses;
            StatsScreen::Keys statsKeys;
            statsKeys.any = keys.any;
            statsKeys.back = keys.back;
            statsKeys.fastHeld = input.isDown(kKeyFire) || input.isDown(kKeyJump);
            flow::SavePrompt::Input& prompt = statsKeys.prompt;
            prompt.up = keys.up;
            prompt.down = keys.down;
            prompt.left = keys.left;
            prompt.right = keys.right;
            prompt.accept = keys.accept;
            prompt.back = keys.back;
            prompt.home = (typing & kPressHome) != 0;
            prompt.end = (typing & kPressEnd) != 0;
            prompt.backspace = (typing & kPressBackspace) != 0;
            prompt.del = (typing & kPressDelete) != 0;
            prompt.typed = typed;
            next = stats->frame(statsKeys, elapsed);
            if ((stats->prompt() != nullptr) != promptOpen) {
                promptOpen = stats->prompt() != nullptr;
                std::printf("[main] statistics: the save prompt %s at %.2f s\n",
                            promptOpen ? "opens" : "closes", *front->time);
            }
            // The name is typed while the prompt is open.
            const bool wantText = !front->headless && stats->prompt() != nullptr;
            if (wantText != textInput) {
                windowTextInput(front->window, wantText);
                textInput = wantText;
            }
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *front->frame;
        params.clear_colour = screen == flow::Screen::Fall ? fall->backdrop() : 0xFF000000u;
        params.present = true;
        const bool capture =
            front->captureFrame > 0 && (int)*front->frame + 1 == front->captureFrame;
        params.screenshot_path = capture ? front->capturePath : nullptr;
        mdk_render_begin_frame(front->renderer, &params);
        if (screen == flow::Screen::Fall) {
            fall->draw(front->renderer);
            mdk_render_end_frame(front->renderer);
            // Game.Run: the device after the screen's frame; then FallScreen.Next.
            fall->mixer().device().update(elapsed);
            next = fall->next(&flow.state());
        } else {
        ScreenView view;
        view.layoutInside(mdk_render_canvas_width(front->renderer), kCanvasHeight);
        const float canvasWidth = mdk_render_canvas_width(front->renderer);
        if (screen == flow::Screen::Menu || screen == flow::Screen::EndMovie) {
            mdk_render_canvas_begin(front->renderer);
            if (screen == flow::Screen::Menu && mainMenu->splashFrame()) {
                // C# MainMenu.Frame's early return: the splash alone (black once done).
                splashView.draw(front->renderer, mainMenu->splash(), canvasWidth);
            } else if (screen == flow::Screen::Menu) {
                // C#: MDKOPT only while the player has no picture, then the picture,
                // then the items.
                if (!mainMenu->video().playing() && !mainMenu->video().hasStill()) {
                    view.image(front->renderer, ui->background.texture, ui->background.palette,
                               ui->background.width, ui->background.height, 0.0f, 0.0f);
                }
                movieView.draw(front->renderer, mainMenu->video(), canvasWidth);
                mainMenu->items().draw(front->renderer, view, *ui->big, *ui->small);
            } else {
                movieView.draw(front->renderer, endMovie->player(), canvasWidth);
            }
            mdk_render_canvas_end(front->renderer);
        } else {
            // C# StatsScreen.Draw: the heads under the canvas, then the page (the
            // screen opens and closes the canvas itself).
            stats->draw(front->renderer, view, *ui->big, *ui->small);
        }
        mdk_render_end_frame(front->renderer);
        if (screen == flow::Screen::Statistics || screen == flow::Screen::Briefing) {
            // Game.Run: the audio device after the screen's frame.
            stats->device().update(elapsed);
        }
        }
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
        // C# Game.Handle (Show closes the screen that ends: the fall's voices and
        // scope, the statistics' voices), then the screens this port lacks skipped.
        if (screen == flow::Screen::Fall) {
            fall->close(front->renderer);
        } else if (screen != flow::Screen::Menu) {
            closeStats();
        }
        flow::Screen shown = flow.handle(next);
        flow::GameFlow::SkipTrail trail;
        shown = flow.skipUnported(&trail);
        for (int32_t i = 0; i < trail.count; i++) {
            std::printf("[main] flow: skipped %s (package #%d not ported)\n",
                        flow::GameFlow::screenName(trail.skipped[i]),
                        (int)flow::GameFlow::screenPackage(trail.skipped[i]));
        }
        shown = settle(shown);
        if (shown == flow::Screen::Fall) {
            screen = shown;
            continue;
        }
        if (shown == flow::Screen::Menu || shown == flow::Screen::Briefing ||
            shown == flow::Screen::Statistics || shown == flow::Screen::EndMovie) {
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
    fall->close(front->renderer);
    closeStats();
    if (textInput) {
        windowTextInput(front->window, false);
    }
    if (endMovie) {
        endMovie->close();
    }
    mdk_render_release(front->renderer, scope);
    return screen;
}

// C# Play's LoadingScreen.Show (Game.cs:478-483): one frame, drawn and
// presented, its resources released before the level loads.
void showLoadingScreen(MdkRenderer renderer, int32_t level, uint32_t frameIndex,
                       const char* capturePath) {
    const MdkScope scope = mdk_render_mark(renderer);
    const formats::MdkData data = formats::MdkData::find(formats::fileSystem());
    std::unique_ptr<LoadingScreen> loading(new LoadingScreen());
    const bool ready = data.found() && loading->load(renderer, data, level);
    MdkFrameParams params;
    params.elapsed = 0.0f;
    params.frame_index = frameIndex;
    params.clear_colour = 0xFF000000u;
    params.present = true;
    params.screenshot_path = capturePath;
    mdk_render_begin_frame(renderer, &params);
    mdk_render_canvas_begin(renderer);
    loading->draw(renderer, mdk_render_canvas_width(renderer), 0.0f);
    mdk_render_canvas_end(renderer);
    mdk_render_end_frame(renderer);
    std::printf("[main] flow: the loading screen of level %d%s\n", (int)level,
                ready ? "" : " (no LOAD_n.LBB: black)");
    loading.reset();
    mdk_render_release(renderer, scope);
}

} // namespace mdk
