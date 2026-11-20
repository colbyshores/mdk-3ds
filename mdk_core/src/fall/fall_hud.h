#pragma once

// ============================================================================
// fall_hud.h — the HUD over the fall, as in a level (0x420830, 0x46cce4) but from
// FALL3D.BNI's copies of the images.
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallHud.cs, ported operation for
// operation: the messages, the health panel (blinking at 20 or less), the
// inventory (always: 0x46cce4 resets its timer every frame outside sniper mode),
// and the skull growing while Kurt dies.
//
//   ┌─────────────────────────────┐
//   │          message            │
//   │          (skull)            │
//   │ [i][i][i][i][i]   [health]  │
//   └─────────────────────────────┘
//
// The fonts and the images use the fall's palette (fall.gd), whose effects the
// view draws over the whole frame (fall_palette.h). The C#'s mod images for the
// HUD (CanvasImages.Replace through ModImages) are not wired here: the fall has
// no mod hook in this port yet (docs/OPEN_ITEMS.md).
//
// Cold: AGENTS.md §2. No virtuals, no templates (AGENTS.md §10). Its FontViews
// are ~11 KB each: the screen that owns this is on the heap.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "game/font_view.h"
#include "game/messages.h"
#include "render/render.h"

namespace mdk {

class Kurt;

namespace formats {
class Bni;
class Font;
class Fti;
}

namespace fall {

class FallHud {
public:
    // FallHud.Life.
    enum Life { Alive, Dying };

    // C# FallHud(renderer, bni, palette, fti, ...): the images of the BNI, the
    // fonts in the fall's palette, the messages' texts. False when the panel,
    // the digits or a font did not load.
    bool load(MdkRenderer renderer, const formats::Bni& bni, int32_t palette, const uint8_t* paletteRgba,
              const formats::Fti& fti, const formats::Font& big, const formats::Font& small);

    // FallHud.Messages.
    Messages& messages() { return messages_; }

    // FallHud.Tick: some ticks passed (the blink).
    void tick(int32_t ticks);

    // FallHud.Draw, into the open canvas batch; `red` (0-1) sizes the skull while
    // Kurt dies.
    void draw(MdkRenderer renderer, int32_t health, const Kurt& inventory, Life life, float red);

    int32_t blink() const { return blink_; }

private:
    struct Image {
        int32_t texture = -1;
        float width = 0.0f;
        float height = 0.0f;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
    };

    void loadImage(const formats::Bni& bni, const char* name, Image* out);
    void drawImage(MdkRenderer renderer, const Image& image, float x, float y, float width, float height);
    void drawNumber(MdkRenderer renderer, int32_t value, float x, float y);

    MdkRenderer renderer_ = nullptr;
    int32_t palette_ = -1;
    const uint8_t* paletteRgba_ = nullptr;
    Image panel_;
    Image skull_;
    Image digits_;
    std::vector<Image> icons_;
    int32_t blink_ = 0;
    FontView big_;
    FontView small_;
    Messages messages_;
};

} // namespace fall
} // namespace mdk
