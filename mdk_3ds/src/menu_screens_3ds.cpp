// menu_screens_3ds.cpp -- see menu_screens_3ds.h. The C#'s Game.Run loop
// (Game.cs:284-364) for the screens the 3DS shows besides the level: the
// elapsed, the presses, the screen's frame, its event handled (Game.Handle), and
// the fall's sound device after it (Game.cs:334-337).

#include "menu_screens_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "audio/audio_3ds.h"
#include "audio/audio_dsp_3ds.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "fall/fall_screen.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "game/async_read.h"
#include "game/end_movie.h"
#include "game/font_view.h"
#include "game/input.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/pause_menu.h"
#include "game/save_prompt_screen.h"
#include "game/movie_view.h"
#include "game/viewer_saves.h"
#include "game/screen_view.h"
#include "game/stats_screen.h"
#include "platform/controls_3ds.h"
#include "platform/input_3ds.h"
#include "platform/log_3ds.h"
#include "render/render_types.h"

namespace mdk {
namespace {

void printLine(void*, const char* line) {
    log3dsPrintf("%s", line);
}

// C# Ui.ApplySettings: applied (the level reads them as it starts) and saved.
void applied(void* user) {
    FrontScreens3ds* front = static_cast<FrontScreens3ds*>(user);
    const bool saved = front->save ? front->save() : false;
    log3dsPrintf("menu: options changed: master=%d music=%d effects=%d filter=%d difficulty=%s "
                 "gore=%d (saved=%d)",
                 (int)front->settings->masterVolume, (int)front->settings->musicVolume,
                 (int)front->settings->effectsVolume, (int)front->settings->musicFilter,
                 GameSettings::difficultyName(front->settings->difficulty),
                 (int)front->settings->gore, (int)saved);
}

// MDKFONT.FTI's two fonts in SYS_PAL (C# Fonts), its texts, SYS_PAL's bytes, and
// the two archives the screens draw from. On the heap: each Font and FontView is
// ~11 KB, and the main thread's stack is not the place (AGENTS.md section 10).
struct UiAssets {
    formats::Fti fti;
    formats::Font bigFont;
    formats::Font smallFont;
    FontView big;
    FontView small;
    std::vector<uint8_t> systemRgb;
    formats::Bni options;
    formats::Bni stats;
    PalettedImage background;
};

bool loadAssets(MdkRenderer renderer, const formats::MdkData& data, UiAssets* ui) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    if (!data.read("MISC/MDKFONT.FTI", &bytes) ||
        !formats::Fti::parse(&ui->fti, std::move(bytes)) ||
        !ui->fti.getBytes("FONTBIG", &bigBytes) || !ui->fti.getBytes("FONTSML", &smallBytes) ||
        !ui->fti.getBytes("SYS_PAL", &ui->systemRgb) ||
        !formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, &ui->bigFont) ||
        !formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, &ui->smallFont)) {
        log3dsPrintf("menu: MDKFONT.FTI's fonts did not load");
        return false;
    }
    const formats::Palette palette =
        formats::Palette::fromRgb(ui->systemRgb.data(), (int32_t)ui->systemRgb.size());
    // C# StatsScreen: `_systemRgb = new byte[768]`, SYS_PAL copied in.
    ui->systemRgb.resize(768, 0);
    const int32_t paletteId = mdk_render_create_palette(renderer, palette.rgba());
    if (paletteId < 0 || !ui->big.load(renderer, ui->bigFont, paletteId, palette.rgba()) ||
        !ui->small.load(renderer, ui->smallFont, paletteId, palette.rgba())) {
        log3dsPrintf("menu: the font atlases did not upload");
        return false;
    }
    std::vector<uint8_t> optionsBytes;
    if (data.read("MISC/OPTIONS.BNI", &optionsBytes) &&
        formats::Bni::parse(&ui->options, std::move(optionsBytes))) {
        if (!loadPalettedImage(renderer, ui->options, "MDKOPT", nullptr, &ui->background)) {
            log3dsPrintf("menu: MDKOPT did not load; the menu draws on black");
        }
    }
    std::vector<uint8_t> statsBytes;
    if (data.read("MISC/STATS.BNI", &statsBytes)) {
        formats::Bni::parse(&ui->stats, std::move(statsBytes));
    }
    return true;
}

// THE LENT FILE READER (game/async_read.h): the renderer's file thread
// (render.h's mdk_render_read_file_async, platform/async_file_reader.h's
// kChannelFile). The main menu reads MDK12.FLC and the slideshow's eight GIFs
// (~632 KB and ~650 KB of files, ~1.7 MiB decoded) through it instead of on the
// main thread in one frame (the owner's rule: no main-thread stalls), and the
// GIFs are decoded there too (1.4-2.1 ms each on the host, so ~70-120 ms each on
// an Old 3DS by the usual 50-60x ratio: one long hitch per slide on the main
// thread even spread one a frame).
bool lentRequest(void* user, int32_t tag, const char* path, AsyncWork work) {
    return mdk_render_read_file_async(static_cast<MdkRenderer>(user), tag, path, work);
}

bool lentPoll(void* user, int32_t tag, uint8_t** bytes, uint32_t* size, bool* ok) {
    return mdk_render_poll_file_read(static_cast<MdkRenderer>(user), tag, bytes, size, ok);
}

void lentCancel(void* user, int32_t tag) {
    mdk_render_cancel_file_read(static_cast<MdkRenderer>(user), tag);
}

} // namespace

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

flow::Screen runFrontScreens3ds(FrontScreens3ds* front) {
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
    // THE MAIN MENU'S SONGS (MainMenu::song: MAINSONG, OPTSONG under the options),
    // on the DSP path exactly as a level's music: a bank that knows only the two
    // streamed tracks (romfs:/music/menu, mdk_3ds/music/menu), a mixer whose music
    // voice the VoiceBinder streams on ndsp 23/22, the settings' master and music
    // volumes and the music filter. Opened with the menu screen and closed -- the
    // worker joined, its linear buffers freed -- when the flow leaves it (C#
    // Game.Show's StopAll), so nothing of it is allocated once a level loads.
    std::unique_ptr<audio::SoundBank> menuBank;
    std::unique_ptr<audio::SoundMixer> menuMixer;
    bool menuAudio = false;
    int32_t menuVoice = 0;
    std::string menuSong;
    uint32_t menuStarts = 0;
    auto closeMenuMusic = [&]() {
        if (menuAudio) {
            audioDspClose();
            menuAudio = false;
        }
        menuMixer.reset();
        menuBank.reset();
        menuVoice = 0;
        menuSong.clear();
    };
    auto openMenuMusic = [&]() {
        closeMenuMusic();
        menuBank.reset(new audio::SoundBank());
        const int32_t tracks = audioDspMusicRegisterFolder(menuBank.get(), "romfs:/music/menu");
        menuMixer.reset(new audio::SoundMixer(menuBank.get()));
        menuAudio = tracks > 0 && audioDspOpen();
        menuMixer->device().setSampleMix(false);
        front->settings->applyAudio(&menuMixer->device());
        menuStarts = mainMenu->songStarts() - 1u;   // the song the menu opened with starts
    };
    // One frame of it (the fall's order: mixer, the DSP path, the device). A new
    // song, or the same one started again, stops the old voice and plays the
    // song from its start, looping, at full volume on the music bus (C#
    // `ui.Play(song, Bus.Music)`). The settings are applied every frame: the
    // options' volume and filter changes are heard at once, as in the C#.
    auto menuMusicFrame = [&](float elapsed) {
        if (!menuMixer) {
            return;
        }
        const char* want = mainMenu->song();
        const uint32_t starts = mainMenu->songStarts();
        if (std::string(want ? want : "") != menuSong || starts != menuStarts) {
            if (menuVoice != 0) {
                menuMixer->stopVoice(menuVoice);
            }
            menuVoice = 0;
            menuSong = want ? want : "";
            menuStarts = starts;
            if (want) {
                menuVoice = menuMixer->playMusic(want);
                menuMixer->setVolume(menuVoice, audio::SoundMixer::kFullVolume);
            }
            log3dsPrintf("menu: song %s", want ? want : "none");
        }
        front->settings->applyAudio(&menuMixer->device());
        menuMixer->update(elapsed);
        audioDspFrame(*menuMixer);
        menuMixer->device().update(elapsed);
    };
    // THE STATISTICS AND THE BRIEFING (package #20b): the C#'s StatsScreen, all its
    // pages after a level, the briefing alone for a new game. Its renderer objects
    // (the pages' images, the heads' model) are this function's scope's; its sound
    // device mixes on the ARM11 into audio_3ds.cpp's one ndsp channel (a handful
    // of voices on a screen between levels; the DSP path, audio_dsp_3ds.h, binds
    // a level's SoundMixer and bank, which this screen has neither of), opened
    // with the screen and closed when the flow leaves it.
    std::unique_ptr<StatsScreen> stats(new StatsScreen());
    bool statsOpen = false;
    bool statsAudio = false;
    bool promptOpen = false;
    auto closeStats = [&]() {
        if (!statsOpen) {
            return;
        }
        // C# Dispose: every voice stopped; then the sink's channel.
        stats->close();
        if (statsAudio) {
            audioClose();
            statsAudio = false;
        }
        statsOpen = false;
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
        setup.gore = front->settings->gore;
        // The C#'s `new Random()` picks only sounds: the system tick.
        setup.seed = (uint32_t)svcGetSystemTick();
        setup.print.line = printLine;
        stats->open(setup);
        if (all) {
            stats->attach(front->renderer, data);
        }
        audio::SoundDevice& device = stats->device();
        statsAudio = audioOpen();
        device.setSampleMix(true);
        if (statsAudio) {
            device.setSink(audioSink, nullptr);
        }
        front->settings->applyAudio(&device);
        statsOpen = true;
        if (all) {
            log3dsPrintf("flow: the statistics after level %d (from %s, heads %d, audio %d)",
                         (int)flow.state().level, StatsScreen::phaseName(stats->phase()),
                         stats->headsView().ready() ? 1 : 0, statsAudio ? 1 : 0);
        } else {
            log3dsPrintf("flow: the briefing of level %d (L%d_MAP)", (int)flow.state().level,
                         (int)(stats->index() + 1));
        }
    };
    // THE FALL (package #17b): made when the flow shows it; closed -- the DSP path
    // first, while the bank it binds lives, then the screen's voices and renderer
    // scope -- when the flow leaves it.
    std::unique_ptr<fall::FallScreen> fall(new fall::FallScreen());
    // START in the fall opens the pause menu, as in a level (upstream 75ec50e2; the
    // original's abort prompt, 0x403cf8): the fall waits under it.
    menu::PauseMenu fallPause;
    bool fallAudio = false;
    auto closeFall = [&]() {
        if (fallAudio) {
            audioDspClose();
            fallAudio = false;
        }
        fall->close(front->renderer);
    };
    // C# Show for the fall: a new FallScreen and its output. False when its data
    // did not load: the caller skips it the C#'s way.
    auto openFall = [&]() -> bool {
        // The C#'s new FallScreen comes with a new PauseMenu. This one outlives the
        // fall, and its "Main menu" leaves it open with its answer (Menu) still
        // set: kept, the next fall's first frame would take that answer and go
        // straight back to the main menu.
        fallPause = menu::PauseMenu();
        fall::FallViewOptions options;
        options.lean = true;
        const int32_t seed = (int32_t)(uint32_t)svcGetSystemTick();   // the C#'s `new Random()`
        fall::FallScreen::Print print;
        print.line = printLine;
        std::string why;
        if (!fall->open(front->renderer, data, ui->fti, ui->bigFont, ui->smallFont, flow.state().level,
                        (Kurt::Difficulty)front->settings->difficulty, seed, options, print, &why)) {
            log3dsPrintf("flow: the fall of level %d did not load (%s)", (int)flow.state().level,
                         why.c_str());
            return false;
        }
        audio::SoundDevice& device = fall->mixer().device();
        fallAudio = audioDspOpen();
        device.setSampleMix(false);
        front->settings->applyAudio(&device);
        log3dsPrintf("flow: the fall before level %d (seed %d, audio %d)", (int)flow.state().level,
                     (int)seed, fallAudio ? 1 : 0);
        return true;
    };
    // The flow's screen after an event, past a fall that did not load (skipped
    // the C#'s Esc way: FallSim.Skip's carry, then the level).
    auto settle = [&](flow::Screen shown) -> flow::Screen {
        for (int32_t i = 0; i < flow::GameFlow::kMaxSkips && shown == flow::Screen::Fall; i++) {
            if (openFall()) {
                break;
            }
            log3dsPrintf("flow: skipped Fall (it did not load)");
            shown = flow.handle(flow.skipScreen());
        }
        return shown;
    };
    // THE SAVE PROMPT AFTER LEVEL8 (C# SavePromptScreen, game/save_prompt_screen.h):
    // the Gunter stream's AfterStream shows it before LEVEL5. The D-pad chooses, A
    // answers (yes: saved under the offered name, "6" -- there is no keyboard to
    // type another), B says no; then LEVEL5.
    SavePromptScreen savePrompt;
    // Package #20c: the end movies, and the pictures of the splash and the movies
    // (game/movie_view.h). The splash is the GPU cross-fade of its two pictures
    // (no per-frame expansion); a movie frame is one streamed RGB565 upload at a
    // frame boundary, every second one with the MOVIES HALF RATE toggle.
    std::unique_ptr<menu::EndMovie> endMovie;
    MovieView movieView;
    SplashView splashView;
    auto open = [&](flow::Screen shown) {
        if (movieView.uploads > 0) {
            log3dsPrintf("movie: %lu uploads", (unsigned long)movieView.uploads);
        }
        movieView.release(front->renderer);
        splashView.release(front->renderer);
        if (endMovie) {
            endMovie->close();
            endMovie.reset();
        }
        if (shown == flow::Screen::Menu) {
            mainMenu.reset(new menu::MainMenu());
            menu::MenuItems& items = mainMenu->items();
            items.setFonts(&ui->bigFont, &ui->smallFont);
            items.setHost(menu::Host::Console);
            items.setSettings(front->settings, applied, front);
            items.setPrint(printLine, nullptr);
            mainMenu->setMedia(&ui->options, &data);
            // Without a running file thread the menu reads at once, as the C#.
            const bool lent = mdk_render_has_file_reader(front->renderer);
            if (lent) {
                AsyncRead reader;
                reader.request = lentRequest;
                reader.poll = lentPoll;
                reader.cancel = lentCancel;
                reader.user = front->renderer;
                mainMenu->setReader(reader);
            }
            log3dsPrintf("flow: the main menu%s%s",
                         flow.state().splash ? " (the INTRO1A splash first)" : "",
                         lent ? "" : " (no file thread: the video and slides read at once)");
            mainMenu->open(&ui->fti, &flow.state(), menuSavesOf(front->saves), false,
                           menu::MenuPage::Main);
            openMenuMusic();
            splashView.open(front->renderer, mainMenu->splash(), true);
        } else if (shown == flow::Screen::EndMovie) {
            // C# EndMovie: MDKEND.FLC, then MDKBZK.MVE, then the menu. FINISH.BNI's
            // sounds and the MVE's stream voice are not wired (end_movie.h): the
            // GOG data has neither movie, so the menu follows at once.
            endMovie.reset(new menu::EndMovie());
            endMovie->player().setData(&data);
            endMovie->open(menu::EndSounds());
            log3dsPrintf("flow: the end movies (%s)", endMovie->status());
        } else if (shown == flow::Screen::Briefing || shown == flow::Screen::Statistics) {
            openStats(shown);
        } else if (shown == flow::Screen::SavePrompt) {
            savePrompt.open(front->saves ? *front->saves : flow::SaveGames(), flow.state());
            log3dsPrintf("flow: the save prompt before level %d (offering \"%s\")",
                         (int)flow.state().level, savePrompt.prompt()->name().c_str());
        }
    };
    if (screen == flow::Screen::Fall) {
        screen = settle(screen);
        if (screen == flow::Screen::Level) {
            mdk_render_release(front->renderer, scope);
            return screen;
        }
    }
    open(screen);

    double last = front->now();
    bool slidesLoading = false;
    // Every button counts as held on the first frame: the press that left the
    // last screen (A on the START menu's MAIN MENU) must not choose here too.
    u32 prevHeld = 0xFFFFFFFFu;
    front->exitReason = "aptMainLoop returned false";
    flow::Screen result = flow::Screen::None;
    while (aptMainLoop()) {
        const double now = front->now();
        if (*front->resumePending) {
            *front->resumePending = false;
            last = now;
            log3dsPrintf("lifecycle: woke; frame clock rebased");
        }
        double delta = now - last;
        last = now;
        if (delta < 0.0) delta = 0.0;
        if (delta > 0.25) delta = 0.25;
        const float elapsed = (float)delta;

        // ONE HID scan for the frame (input_3ds.h's trap), its edges.
        Input input;
        const u32 held = input3dsPoll(&input, elapsed, *front->controls);
        const u32 pressed = held & ~prevHeld;
        prevHeld = held;
        menu::Keys keys;
        keys.up = (pressed & KEY_DUP) != 0;
        keys.down = (pressed & KEY_DDOWN) != 0;
        keys.left = (pressed & KEY_DLEFT) != 0;
        keys.right = (pressed & KEY_DRIGHT) != 0;
        keys.accept = (pressed & (KEY_A | KEY_START)) != 0;
        keys.back = (pressed & KEY_B) != 0;
        keys.leftHeld = (held & KEY_DLEFT) != 0;
        keys.rightHeld = (held & KEY_DRIGHT) != 0;
        keys.any = pressed != 0;

        flow::Event next = flow::Event::None;
        const bool halfRate = front->toggles != nullptr && front->toggles->movieHalfRate;
        if (screen == flow::Screen::Fall) {
            // C# FallScreen.Frame: START (the C#'s Esc) opens the pause menu and the
            // fall waits under it (upstream 75ec50e2); otherwise the movement
            // actions, the circle pad and the D-pad steer.
            if (fallPause.open()) {
                next = fallPause.update(keys, elapsed);
                if (!fallPause.open()) {
                    log3dsPrintf("fall: the pause menu closes at %s", fall->status().c_str());
                }
            } else if ((pressed & KEY_START) != 0) {
                fallPause.show();
                log3dsPrintf("fall: the pause menu opens at %s", fall->status().c_str());
            } else {
                fall::FallSim::Input steer = fall::FallScreen::steering(input, false);
                const int32_t padX = ((held & (KEY_CPAD_RIGHT | KEY_DRIGHT)) != 0 ? 1 : 0) -
                                     ((held & (KEY_CPAD_LEFT | KEY_DLEFT)) != 0 ? 1 : 0);
                const int32_t padY = ((held & (KEY_CPAD_UP | KEY_DUP)) != 0 ? 1 : 0) -
                                     ((held & (KEY_CPAD_DOWN | KEY_DDOWN)) != 0 ? 1 : 0);
                const int32_t x = steer.steerX + padX;
                const int32_t y = steer.steerY + padY;
                steer.steerX = x > 0 ? 1 : (x < 0 ? -1 : 0);
                steer.steerY = y > 0 ? 1 : (y < 0 ? -1 : 0);
                fall->frame(steer, elapsed);
            }
        } else if (screen == flow::Screen::Menu) {
            next = mainMenu->frame(keys, elapsed);
            menuMusicFrame(elapsed);
            movieView.sync(front->renderer, mainMenu->video(), halfRate);
            // The lent reader's slide load, for the log: its start and its end.
            if (mainMenu->slideshow().loading() != slidesLoading) {
                slidesLoading = mainMenu->slideshow().loading();
                log3dsPrintf("menu: slides %s at frame %lu (%d loaded)",
                             slidesLoading ? "loading off the main thread" : "loaded",
                             (unsigned long)*front->frame,
                             (int)mainMenu->slideshow().slideCount());
            }
        } else if (screen == flow::Screen::EndMovie) {
            // C# EndMovie.Frame's Back: B, and START or A as the console's skip.
            next = endMovie->frame(keys.back || keys.accept, elapsed);
            movieView.sync(front->renderer, endMovie->player(), halfRate);
        } else if (screen == flow::Screen::SavePrompt) {
            // C# SavePromptScreen.Frame: the D-pad, A (yes, then save under the
            // offered name) and B (no / give up), as the statistics' prompt.
            flow::SavePrompt::Input prompt;
            prompt.up = keys.up;
            prompt.down = keys.down;
            prompt.left = keys.left;
            prompt.right = keys.right;
            prompt.accept = keys.accept;
            prompt.back = keys.back;
            next = savePrompt.frame(prompt, elapsed);
            if (next != flow::Event::None) {
                // C# SavePrompt.Save's line, or why it closed without one.
                log3dsPrintf("flow: the save prompt closes at frame %lu (%s%s)",
                             (unsigned long)*front->frame,
                             savePrompt.prompt()->saved() ? "saved game " : "no save",
                             savePrompt.prompt()->saved() ? savePrompt.prompt()->name().c_str()
                                                          : "");
            }
        } else {
            // C# StatsScreen.Frame: any button, B (Esc) skips, Fire or Jump held
            // runs it fast. The save prompt between its pages takes the D-pad, A
            // (yes; then save under the offered name, the next level's number --
            // there is no keyboard to type another) and B (no / give up).
            StatsScreen::Keys statsKeys;
            statsKeys.any = keys.any;
            statsKeys.back = keys.back;
            statsKeys.fastHeld = input.isDown(kKeyFire) || input.isDown(kKeyJump);
            statsKeys.prompt.up = keys.up;
            statsKeys.prompt.down = keys.down;
            statsKeys.prompt.left = keys.left;
            statsKeys.prompt.right = keys.right;
            statsKeys.prompt.accept = keys.accept;
            statsKeys.prompt.back = keys.back;
            next = stats->frame(statsKeys, elapsed);
            if ((stats->prompt() != nullptr) != promptOpen) {
                promptOpen = stats->prompt() != nullptr;
                log3dsPrintf("statistics: the save prompt %s at frame %lu",
                             promptOpen ? "opens" : "closes", (unsigned long)*front->frame);
            }
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *front->frame;
        params.clear_colour = screen == flow::Screen::Fall ? fall->backdrop() : 0xFF000000u;
        params.present = true;
        params.screenshot_path = nullptr;
        mdk_render_begin_frame(front->renderer, &params);
        if (screen == flow::Screen::Fall) {
            fall->draw(front->renderer);
            if (fallPause.open()) {
                // C# FallScreen.Paused: the fall as it stopped, the menu over it.
                ScreenView pauseView;
                pauseView.layoutInside(mdk_render_canvas_width(front->renderer), kCanvasHeight);
                mdk_render_canvas_begin(front->renderer);
                fallPause.draw(front->renderer, pauseView, ui->big, ui->small);
                mdk_render_canvas_end(front->renderer);
            }
            mdk_render_end_frame(front->renderer);
            // Game.Run: the device after the screen's frame (the DSP frame between
            // the mixer and the device, voice_binding.h); then FallScreen.Next.
            audio::SoundMixer& mixer = fall->mixer();
            mixer.update(elapsed);
            audioDspFrame(mixer);
            mixer.device().update(elapsed);
            if (next == flow::Event::None) {
                next = fall->next(&flow.state());
            }
        } else {
        ScreenView view;
        view.layoutInside(mdk_render_canvas_width(front->renderer), kCanvasHeight);
        const float canvasWidth = mdk_render_canvas_width(front->renderer);
        if (screen == flow::Screen::SavePrompt) {
            // C# SavePromptScreen.Frame: Layout(Fit.Inside), then SavePrompt.Draw.
            mdk_render_canvas_begin(front->renderer);
            savePrompt.draw(front->renderer, view, ui->big, &ui->fti);
            mdk_render_canvas_end(front->renderer);
        } else if (screen == flow::Screen::Menu || screen == flow::Screen::EndMovie) {
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
                mainMenu->items().draw(front->renderer, view, ui->big, ui->small);
            } else {
                movieView.draw(front->renderer, endMovie->player(), canvasWidth);
            }
            mdk_render_canvas_end(front->renderer);
        } else {
            // C# StatsScreen.Draw: the heads (meshes, queued for both eyes' replay
            // with the camera, lighting and glow state they read set by the screen
            // this frame), then the page on the canvas.
            stats->draw(front->renderer, view, ui->big, ui->small);
        }
        mdk_render_end_frame(front->renderer);
        if (screen == flow::Screen::Statistics || screen == flow::Screen::Briefing) {
            // Game.Run: the audio device after the screen's frame.
            stats->device().update(elapsed);
        }
        }
        (*front->frame)++;
        if (front->frameBudget != 0 && *front->frame >= front->frameBudget) {
            front->exitReason = "frame budget reached";
            break;
        }
        if (next == flow::Event::None) {
            continue;
        }
        // C# Game.Handle (Show closes the screen that ends: the fall's output,
        // voices and scope; the statistics' voices and sink), then the screens this
        // port lacks skipped.
        if (screen == flow::Screen::Fall) {
            closeFall();
        } else if (screen == flow::Screen::Menu) {
            closeMenuMusic();
        } else if (screen == flow::Screen::SavePrompt) {
            savePrompt.close();
        } else if (screen != flow::Screen::Menu) {
            closeStats();
        }
        flow::Screen shown = flow.handle(next);
        flow::GameFlow::SkipTrail trail;
        shown = flow.skipUnported(&trail);
        for (int32_t i = 0; i < trail.count; i++) {
            log3dsPrintf("flow: skipped %s (package #%d not ported)",
                         flow::GameFlow::screenName(trail.skipped[i]),
                         (int)flow::GameFlow::screenPackage(trail.skipped[i]));
        }
        shown = settle(shown);
        if (shown == flow::Screen::Fall) {
            screen = shown;
            prevHeld = 0xFFFFFFFFu;
            continue;
        }
        if (shown == flow::Screen::Menu || shown == flow::Screen::Briefing ||
            shown == flow::Screen::Statistics || shown == flow::Screen::EndMovie ||
            shown == flow::Screen::SavePrompt) {
            screen = shown;
            open(screen);
            prevHeld = 0xFFFFFFFFu;
            continue;
        }
        if (shown == flow::Screen::Level) {
            log3dsPrintf("flow: level %d at frame %lu", (int)flow.state().level,
                         (unsigned long)*front->frame);
            result = flow::Screen::Level;
        } else if (shown == flow::Screen::None) {
            front->exitReason = "menu Quit";
        } else {
            log3dsPrintf("flow: %s follows, which this frontend cannot show; exiting",
                         flow::GameFlow::screenName(shown));
            front->exitReason = "the flow asked for a screen this frontend cannot show";
        }
        break;
    }
    closeFall();
    closeStats();
    closeMenuMusic();
    if (endMovie) {
        endMovie->close();
    }
    mdk_render_release(front->renderer, scope);
    return result;
}

// C# Play's LoadingScreen.Show (Game.cs:478-483): the screen loaded and its first
// frame, the bar empty. It stays on the screen through the level's load; the
// caller moves the bar (drawLoadingScreen3ds) and frees it before play.
void showLoadingScreen3ds(MdkRenderer renderer, int32_t level, uint32_t frameIndex,
                          LoadingScreen* loading) {
    const formats::MdkData data = formats::MdkData::find(formats::fileSystem());
    const bool ready = data.found() && loading->load(renderer, data, level);
    drawLoadingScreen3ds(renderer, *loading, 0.0f, frameIndex);
    log3dsPrintf("flow: the loading screen of level %d%s", (int)level,
                 ready ? "" : " (no LOAD_n.LBB: black)");
}

// Without the LBB the frame is black, as the first one was.
void drawLoadingScreen3ds(MdkRenderer renderer, const LoadingScreen& loading, float progress,
                          uint32_t frameIndex) {
    MdkFrameParams params;
    params.elapsed = 0.0f;
    params.frame_index = frameIndex;
    params.clear_colour = 0xFF000000u;
    params.present = true;
    params.screenshot_path = nullptr;
    params.loading = true;
    mdk_render_begin_frame(renderer, &params);
    mdk_render_canvas_begin(renderer);
    loading.draw(renderer, mdk_render_canvas_width(renderer), progress);
    mdk_render_canvas_end(renderer);
    mdk_render_end_frame(renderer);
}

} // namespace mdk
