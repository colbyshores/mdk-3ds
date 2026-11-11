#pragma once

// ============================================================================
// hud_view.h — the in-game HUD, a 1:1 port of src/Mdk.Game/Hud/HudView.cs.
//
// It draws on the 360-high canvas whose width follows the frame's aspect
// (render.h's canvas): the health in the
// SC_STAT panel (bottom right, SNIP_TXT digits, blinking at 20 or less), the
// inventory outside sniper mode (5 PICKUPS slots at the bottom left), the
// health bar of the object Kurt shoots at (top left), the red and white
// flashes, and the skull when he is dead.
//
// THE SPRITES come from TRAVERSE/TRAVSPRT.BNI (the same Bni the Kurt sprite
// and the scripts use) and are indexed through the level's palette, exactly as
// the C# HudView does. The sniper overlay, the bomber sight and the message
// stream are separate subsystems and are not part of this port yet.
// ============================================================================

#include <stdint.h>

#include <utility>
#include <vector>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

namespace formats {
class Bni;
class Palette;
} // namespace formats

// What the HUD shows this frame (the C# HudState record).
struct HudState {
    int health = 100;
    float hurtFlash = 0.0f;
    float whiteFlash = 0.0f;
    bool dead = false;
    // The inventory: (item number, count), 1-based item numbers.
    std::vector<std::pair<int32_t, int32_t>> slots;
    int32_t selected = 0;
    int32_t superChainGun = 0;
    int32_t barHealth = 0;
    int32_t barMax = 0;
};

class HudView {
public:
    // The super chain gun's item number: its slot shows the ticks left.
    static const int32_t kSuperChainGun = 6;

    // Uploads the HUD's sprites as index textures through `palette`. Returns
    // false when a required sprite is missing.
    bool load(MdkRenderer renderer, const formats::Bni& sprites, const formats::Palette& palette);

    // Advances the blink once a tick (the C# Tick).
    void tick();

    // Draws the HUD between mdk_render_canvas_begin/end.
    void draw(MdkRenderer renderer, const HudState& state);

private:
    struct Image {
        int32_t texture = -1;
        float w = 0.0f;
        float h = 0.0f;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
    };

    // The palette is a parameter and not just the `palette_` id, because the two
    // backends need different things: the seam's index-texture call takes the
    // palette BYTES as well as the id, the Vulkan backend ignores the bytes and
    // samples the palette at draw time through the id, and the Citro3D backend
    // cannot make the texture at all without the bytes (it expands the indices on
    // the CPU, renderer_c3d.cpp). Passing nullptr here used to work on Linux and
    // refused every HUD image on the 3DS.
    void loadImage(const formats::Bni& sprites, const char* name, Image* out,
                   const formats::Palette& palette);
    void drawImage(const Image& image, float x, float y, const Colour& tint);
    void drawNumber(int32_t value, float centreX, float y);
    void drawInventory(const HudState& state);
    void drawBar(int32_t health, int32_t max);
    void fillRect(float x, float y, float w, float h, const Colour& colour);
    void frameRect(float x, float y, float w, float h, const Colour& colour);

    MdkRenderer renderer_ = nullptr;
    int32_t palette_ = -1;
    Image panel_;
    Image skull_;
    Image digits_;
    std::vector<Image> icons_;
    Colour barFill_ = {0.0f, 0.0f, 0.0f, 1.0f};
    Colour barFrame_ = {0.0f, 0.0f, 0.0f, 1.0f};
    int32_t blink_ = 0;
};

} // namespace mdk
