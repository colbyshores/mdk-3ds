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
#include "fall/fall_screen.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "game/briefing.h"
#include "game/font_view.h"
#include "game/input.h"
#include "game/main_menu.h"
#include "game/viewer_saves.h"
#include "game/menu_presses.h"
#include "game/screen_view.h"
#include "platform/controls_3ds.h"
#include "platform/harness_3ds.h"
#include "platform/input_3ds.h"
#include "platform/log_3ds.h"

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

// The harness's menu keys (MENU=..., game/menu_presses.h), parsed once; the
// clock is the screens' own game time, across every call.
MenuPresses& harnessPresses() {
    static MenuPresses presses;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_MENU)
        std::string error;
        if (presses.parse(MDK_HW_MENU, &error)) {
            log3dsPrintf("menu: harness presses MENU=%s (%d)", MDK_HW_MENU,
                         (int)presses.presses.size());
        } else {
            log3dsPrintf("menu: harness presses MENU=%s REFUSED: %s", MDK_HW_MENU, error.c_str());
            presses.presses.clear();
        }
#endif
    }
    return presses;
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
    MenuPresses& presses = harnessPresses();
    static float pressTime = 0.0f;

    std::unique_ptr<menu::MainMenu> mainMenu(new menu::MainMenu());
    Briefing briefing;
    PalettedImage map;
    // THE FALL (package #17b): made when the flow shows it; closed -- the DSP path
    // first, while the bank it binds lives, then the screen's voices and renderer
    // scope -- when the flow leaves it.
    std::unique_ptr<fall::FallScreen> fall(new fall::FallScreen());
    bool fallAudio = false;
    auto closeFall = [&]() {
        if (fallAudio) {
            if (kDspAudio) {
                audioDspClose();
            } else {
                audioClose();
            }
            fallAudio = false;
        }
        fall->close(front->renderer);
    };
    // C# Show for the fall: a new FallScreen and its output. False when its data
    // did not load: the caller skips it the C#'s way.
    auto openFall = [&]() -> bool {
        fall::FallViewOptions options;
        options.lean = true;
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_FIXED)
        const int32_t seed = 1;   // FIXED=1 replays one fall, as the Linux --fixed-step
#else
        const int32_t seed = (int32_t)(uint32_t)svcGetSystemTick();   // the C#'s `new Random()`
#endif
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
        fallAudio = kDspAudio ? audioDspOpen(fall->bank()) : audioOpen();
        device.setSampleMix(!kDspAudio);
        if (fallAudio) {
            device.setSink(kDspAudio ? nullptr : audioSink, nullptr);
        }
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
    auto open = [&](flow::Screen shown) {
        if (shown == flow::Screen::Menu) {
            mainMenu.reset(new menu::MainMenu());
            menu::MenuItems& items = mainMenu->items();
            items.setFonts(&ui->bigFont, &ui->smallFont);
            items.setHost(menu::Host::Console);
            items.setSettings(front->settings, applied, front);
            items.setPrint(printLine, nullptr);
            log3dsPrintf("flow: the main menu");
            mainMenu->open(&ui->fti, &flow.state(), menuSavesOf(front->saves), false,
                           menu::MenuPage::Main);
        } else if (shown == flow::Screen::Briefing) {
            briefing.open(&ui->fti, flow.state().level);
            char name[24];
            snprintf(name, sizeof(name), "L%d_MAP", (int)(briefing.index() + 1));
            if (map.ready()) {
                mdk_render_release_texture(front->renderer, map.texture);
                mdk_render_release_palette(front->renderer, map.palette);
                map = PalettedImage();
            }
            if (!loadPalettedImage(front->renderer, ui->stats, name, ui->systemRgb.data(), &map)) {
                log3dsPrintf("menu: %s did not load; the briefing types on black", name);
            }
            log3dsPrintf("flow: the briefing of level %d (%s)", (int)flow.state().level, name);
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
    // Every button counts as held on the first frame: the press that left the
    // last screen (A on the START menu's MAIN MENU) must not choose here too.
    u32 prevHeld = 0xFFFFFFFFu;
    front->exitReason = "aptMainLoop returned false";
    flow::Screen result = flow::Screen::None;
    while (aptMainLoop()) {
        if (harnessAutoquitDue()) {
            front->exitReason = "harness autoquit (MDK_AUTOQUIT)";
            break;
        }
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
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_FIXED)
        delta = 1.0 / 60.0;
#endif
        const float elapsed = (float)delta;
        pressTime += elapsed;

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
        presses.apply(pressTime, &keys);

        flow::Event next = flow::Event::None;
        if (screen == flow::Screen::Fall) {
            // C# FallScreen.Frame: START skips (the C#'s Esc; a harness Menu.Back
            // too -- `keys.back` with no B pressed, B being the back movement here),
            // the movement actions, the circle pad and the D-pad steer.
            const bool harnessBack = keys.back && (pressed & KEY_B) == 0;
            fall::FallSim::Input steer = fall::FallScreen::steering(
                input, (pressed & KEY_START) != 0 || harnessBack);
            const int32_t padX = ((held & (KEY_CPAD_RIGHT | KEY_DRIGHT)) != 0 ? 1 : 0) -
                                 ((held & (KEY_CPAD_LEFT | KEY_DLEFT)) != 0 ? 1 : 0);
            const int32_t padY = ((held & (KEY_CPAD_UP | KEY_DUP)) != 0 ? 1 : 0) -
                                 ((held & (KEY_CPAD_DOWN | KEY_DDOWN)) != 0 ? 1 : 0);
            const int32_t x = steer.steerX + padX;
            const int32_t y = steer.steerY + padY;
            steer.steerX = x > 0 ? 1 : (x < 0 ? -1 : 0);
            steer.steerY = y > 0 ? 1 : (y < 0 ? -1 : 0);
            fall->frame(steer, elapsed);
        } else if (screen == flow::Screen::Menu) {
            next = mainMenu->frame(keys, elapsed);
        } else {
            next = briefing.frame(keys.any, keys.back,
                                  input.isDown(kKeyFire) || input.isDown(kKeyJump), elapsed);
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *front->frame;
        params.clear_colour = screen == flow::Screen::Fall ? fall->backdrop() : 0xFF000000u;
        params.present = true;
        params.screenshot_path = harnessCapturePath(*front->frame);
        mdk_render_begin_frame(front->renderer, &params);
        if (screen == flow::Screen::Fall) {
            fall->draw(front->renderer);
            mdk_render_end_frame(front->renderer);
            // Game.Run: the device after the screen's frame (the DSP frame between
            // the mixer and the device, voice_binding.h); then FallScreen.Next.
            audio::SoundMixer& mixer = fall->mixer();
            mixer.update(elapsed);
            audioDspFrame(mixer);
            mixer.device().update(elapsed);
            next = fall->next(&flow.state());
        } else {
        mdk_render_canvas_begin(front->renderer);
        ScreenView view;
        view.layoutInside(mdk_render_canvas_width(front->renderer), kCanvasHeight);
        if (screen == flow::Screen::Menu) {
            view.image(front->renderer, ui->background.texture, ui->background.palette,
                       ui->background.width, ui->background.height, 0.0f, 0.0f);
            mainMenu->items().draw(front->renderer, view, ui->big, ui->small);
        } else {
            briefing.draw(front->renderer, view, ui->big, map.texture, map.palette, map.width,
                          map.height);
        }
        mdk_render_canvas_end(front->renderer);
        mdk_render_end_frame(front->renderer);
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
        // voices and scope), then the screens this port lacks skipped.
        if (screen == flow::Screen::Fall) {
            closeFall();
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
        if (shown == flow::Screen::Menu || shown == flow::Screen::Briefing) {
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
    mdk_render_release(front->renderer, scope);
    return result;
}

} // namespace mdk
