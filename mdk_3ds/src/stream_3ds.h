#pragma once

// ============================================================================
// stream_3ds.h -- the 3DS frontend's stream screen (package #18b): one stream
// (stream/stream_screen.h, the C#'s StreamScreen) played on the console's clock,
// input and sound, from its first frame to its Event. main_3ds.cpp's level loop
// calls it when the flow shows the stream, inside a renderer scope of its own.
//
// Its own file, beside main_3ds.cpp and not in it: the stream's loop has its own
// mixer and its own frame clock, and the source gates over main_3ds.cpp
// (ndsp_sink_test.sh, main_3ds_flow_test.sh) pin the LEVEL's single clock and
// mix; a second loop in that file would be read as a second feed of the level's.
// ============================================================================

#include <stdint.h>

#include "formats/mdk_data.h"
#include "game/game_flow.h"
#include "game/kurt.h"
#include "platform/controls_3ds.h"
#include "render/render.h"

namespace mdk {

// What the stream needs from main_3ds.cpp: the renderer, the controls, the run's
// frame count and budget, the lifecycle hook's resume flag (P1: the frame clock is
// rebased on resume) and the heap probe for the log.
struct Stream3dsFrontend {
    MdkRenderer renderer = nullptr;
    const ControlBindings* controls = nullptr;
    uint32_t* frame = nullptr;
    uint32_t frameBudget = 0;
    volatile bool* resumePending = nullptr;
    void (*heapProbe)(const char* tag) = nullptr;
    // The settings' difficulty (C# StreamScreen: ui.Settings.Difficulty).
    Kurt::Difficulty difficulty = Kurt::Normal;
};

// Returns Event::Quit when the RUN ends inside the stream (aptMainLoop false, the
// frame budget) with `exitReason` set; otherwise the
// screen's Event and, in `endHealth`, Kurt's health at its end (the C#'s
// StreamScreen.Health, which AfterStream reads).
flow::Event runStream3ds(const Stream3dsFrontend& f, const formats::MdkData& data, int32_t level,
                         int32_t health, uint32_t seed, int32_t* endHealth, const char** exitReason);

// `count` empty frames (the clear colour) after the stream's scope is released:
// C3D_FrameBegin waits for the last queue, so the deferred releases are freed
// before the next level allocates (AGENTS.md P3). Here and not in main_3ds.cpp so
// that file's first frame end stays the level frame's (main_3ds_flow_test.sh
// places its object-pass arm before it).
void streamBlankFrames3ds(MdkRenderer renderer, uint32_t frame, int count);

} // namespace mdk
