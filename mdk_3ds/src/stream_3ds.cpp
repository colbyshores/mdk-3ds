// stream_3ds.cpp -- see stream_3ds.h.

#include "stream_3ds.h"

#include <3ds.h>
#include <stdio.h>

#include <memory>

#include "audio/audio_dsp_3ds.h"
#include "audio/sound_mixer.h"
#include "platform/input_3ds.h"
#include "platform/log_3ds.h"
#include "stream/stream_screen.h"

namespace mdk {

namespace {

// The console's monotonic clock, in seconds (main_3ds.cpp's nowSeconds).
double streamNow() {
    return (double)svcGetSystemTick() / (double)SYSCLOCK_ARM11;
}

} // namespace

// The stream screen's report lines (C# StreamScreen's Console.WriteLine) go to the log.
static void logStreamLine(void*, const char* line) {
    log3dsPrintf("%s", line);
}

// THE STREAM SCREEN (package #18b; stream/stream_screen.h): one stream, from its
// first frame to its Event (C# Game.Run's loop over a StreamScreen), on this
// frontend's clock, input and sound. Returns Event::Quit when the RUN ends inside
// it (aptMainLoop false, the frame budget) with
// `exitReason` set; otherwise the screen's Event and, in `endHealth`, Kurt's health
// at its end. The caller marks and releases the renderer's scope around it.
//
// P1: aptMainLoop() every frame, the frame clock rebased on resume and clamped.
// P2: the camera is set before begin_frame (StreamScreen::update), the draws
// between begin and end; every GPU state is the backend's per draw.
// The sound: the screen's own bank and mixer (the BNI's WAVs), on the DSP path
// (audio_dsp_3ds.h) -- opened here for the stream's bank and closed before the
// screen, and with it the bank, goes.
flow::Event runStream3ds(const Stream3dsFrontend& f, const formats::MdkData& data,
                         int32_t level, int32_t health, uint32_t seed, int32_t* endHealth,
                         const char** exitReason) {
    MdkRenderer renderer = f.renderer;
    uint32_t* frame = f.frame;
    *endHealth = health;
    f.heapProbe("stream-start");
    // The screen and its StreamRun on the heap: main's stack is small.
    std::unique_ptr<stream::StreamScreen> screen(new stream::StreamScreen());
    stream::StreamScreen::Setup setup;
    setup.renderer = renderer;
    setup.data = &data;
    setup.level = level;
    setup.health = health;
    setup.difficulty = f.difficulty;
    setup.seed = seed;
    setup.log = logStreamLine;
    log3dsPrintf("stream: after level %d, health %d, seed %lu", (int)level, (int)health,
                      (unsigned long)seed);
    if (!screen->open(setup)) {
        log3dsPrintf("stream: could not open; it ends at once with health %d", (int)health);
    }
    f.heapProbe("stream-open");
    audio::SoundMixer& mixer = screen->mixer();
    audioDspOpen();
    mixer.device().setSampleMix(false);

    // The circle pad steers too (its range is about +-156; a third of it is a push).
    const int32_t kPadDead = 52;
    flow::Event result = flow::Event::None;
    double last = streamNow();
    u32 prevHeld = 0;
    while (result == flow::Event::None) {
        if (!aptMainLoop()) {
            *exitReason = "aptMainLoop returned false";
            result = flow::Event::Quit;
            break;
        }
        const double now = streamNow();
        if (*f.resumePending) {
            *f.resumePending = false;
            last = now;
            log3dsPrintf("lifecycle: woke; frame clock rebased (stream)");
        }
        double delta = now - last;
        last = now;
        if (delta < 0.0) delta = 0.0;
        if (delta > 0.25) delta = 0.25;
        const float elapsed = (float)delta;

        // ONE HID scan for the frame (input3dsPoll); the pad is read off the same scan.
        Input input;
        const u32 held = input3dsPoll(&input, elapsed, *f.controls);
        circlePosition pad;
        hidCircleRead(&pad);
        const u32 pressed = held & ~prevHeld;
        prevHeld = held;
        // C# MenuKey.Back: START.
        const bool back = (pressed & KEY_START) != 0;
        const stream::Steering steering =
            stream::steeringWithPad(stream::steeringOf(input), pad.dx, pad.dy, kPadDead);

        const flow::Event next = screen->update(elapsed, steering, back);

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = *frame;
        params.clear_colour = 0xFF000000u;   // C# Ui.Black
        params.present = true;
        params.screenshot_path = nullptr;
        mdk_render_begin_frame(renderer, &params);
        screen->draw();
        // C# Game.Run: the device after the screen, with the same elapsed; the DSP
        // path's frame between the mixer and the device (voice_binding.h).
        mixer.update(elapsed);
        audioDspFrame(mixer);
        mixer.device().update(elapsed);
        mdk_render_end_frame(renderer);
        (*frame)++;
        if ((*frame % 300u) == 0u) {
            char status[128];
            screen->status(status, (int32_t)sizeof(status));
            log3dsPrintf("stream: frame %lu: %s", (unsigned long)*frame, status);
        }
        if (f.frameBudget != 0 && *frame >= f.frameBudget) {
            *exitReason = "frame budget reached";
            result = flow::Event::Quit;
            break;
        }
        if (next != flow::Event::None) {
            *endHealth = screen->health();
            result = next;
        }
    }
    // The output before the bank it plays from: the DSP worker joined and its
    // pins given back while the screen's bank lives (audio_dsp_3ds.h).
    audioDspClose();
    screen.reset();
    f.heapProbe("stream-end");
    return result;
}

void streamBlankFrames3ds(MdkRenderer renderer, uint32_t frame, int count) {
    for (int i = 0; i < count; i++) {
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

} // namespace mdk
