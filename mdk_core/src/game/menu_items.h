#pragma once

// ============================================================================
// menu_items.h -- a list of menu items drawn like the original's menus
// (C# src/Mdk.Game/Menu/MenuItems.cs; menu_items.gd, menu_entry.gd, 0x42c374).
//
// FONTBIG in SYS_PAL, unselected items at 65 %, the selected one growing to
// full size at 2.1 a second (0.35 x 0.2 per tick). Rows of 36 from y 5 on the
// 600 x 360 view (baselines at 31 + 36 x row, 0x4265c0); longer pages get lower
// rows, and below 24 the small font. Also the options pages the main menu and
// the pause menu share.
//
//   main menu: a column at the view's left edge, items centred on its width
//   other pages: centred on x 300
//
// THE LAMBDAS ARE ACTIONS. The C# entries carry an Action for a press and an
// Action<int> for an option's step; this port has no std::function (AGENTS.md
// §10), so an entry carries an ACTION ID, and the owner (MainMenu, PauseMenu)
// acts on the Event that handle() returns -- at the point in the frame where
// the C# would have invoked the lambda (handle(), then the owner, then grow(),
// which is MenuItems.Update's own order: Layout, HandleInput, the growth loop).
// The options pages' own ids (kOptionsBase and up) are acted on inside
// handle(), as the C#'s lambdas inside MenuItems are.
//
// WHAT IS NOT PORTED, WITH CAUSE:
//   * The pointer: Hover and the clicks (the C#'s mouse). The 3DS has none and
//     the Linux frontend's mouse is the game's look; the keys are the menus'.
//   * The sounds: SND_PUSH on a move, OPTSONG under the options, OPTBUTT on a
//     change. The menus are silent here (docs/OPEN_ITEMS.md).
//   * The desktop's option pages whose settings no frontend of this port can
//     apply: Display (window, resolution, VSync, GPU), 3D Stereo, Keyboard &
//     mouse and Gamepad (bindings) and Mods. The C# filters its pages per host
//     itself (DisplayMenu.Items: a phone has no display mode); `Host::Console`
//     is this port's host. `Host::Oracle` lists the C#'s desktop pages, so the
//     golden can walk the C#'s own page list -- the five pages it has no
//     contents for open a page with Back alone.
//   * Esc's binding page waits (the controls pages) and the HD textures' job:
//     on the pages above.
//
// Cold code: the menus run between levels or while the game is paused. No
// virtuals, no templates, no exceptions, no std::function (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render.h"

namespace mdk {

class FontView;
struct GameSettings;
struct ScreenView;

namespace formats {
class Font;
}

namespace menu {

// One frame of the menu keys (C# MenuKey): pressed this frame, and Left/Right
// held (the sliders step again while held). `any` is the C#'s AnyPressed: any
// key or button at all this frame.
struct Keys {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool back = false;
    bool leftHeld = false;
    bool rightHeld = false;
    bool any = false;
};

// What a frame's keys did to the page, for its owner: an item pressed (or Esc
// on a page with a back action), or an option stepped.
struct Event {
    enum Kind { None, Press, Change };
    Kind kind = None;
    int32_t action = -1;
    int32_t step = 0;
};

// Which option pages and rows a frontend shows (see the header).
enum class Host { Console, Oracle };

// A line the menus print (the C#'s Console.WriteLine "Menu: ..."), and the
// settings changed (the C#'s Ui.ApplySettings: apply, then save).
typedef void (*PrintFn)(void* user, const char* line);
typedef void (*AppliedFn)(void* user);

class MenuItems {
public:
    // C# MenuItems.Align.
    enum class Align { Left, Centre };

    // The options pages' action ids; owners use ids below kOptionsBase.
    static const int32_t kOptionsBase = 1000;

    // The fonts the rows are measured in (C# Fonts.Big / Small), the settings
    // the options pages change, and where their lines and changes go.
    void setFonts(const formats::Font* big, const formats::Font* small) {
        big_ = big;
        small_ = small;
    }
    void setHost(Host host) { host_ = host; }
    void setSettings(GameSettings* settings, AppliedFn applied, void* user) {
        settings_ = settings;
        applied_ = applied;
        appliedUser_ = user;
    }
    void setPrint(PrintFn print, void* user) {
        print_ = print;
        printUser_ = user;
    }

    // C# Describe: the texts of the page joined by " | ".
    std::string describe() const;
    // C# Clear (the options' song stops; it is not ported) and ClearPage.
    void clear() { clearPage(); }

    void addItem(const std::string& text, int32_t action);
    // An item that can't be chosen (shown greyed).
    void addDisabled(const std::string& text);
    // A line at full size that can't be selected ("Really Quit?", list titles).
    void addTitle(const std::string& text);

    // C# Select: the item at `index`, or the first selectable one after it.
    void select(int32_t index);

    // Esc's page: the one above (the options' pages), or none (-1) when the
    // owner handles Esc. C# HandlesBack.
    bool handlesBack() const { return back_ >= 0; }

    // C# Print: "Menu: " and the page, to the print hook.
    void print() const;

    // ---- A frame (C# Update, split where the C# invokes the lambdas) ----
    // Layout, then the keys (when Visible): returns what the owner must act on.
    Event handle(const Keys& keys, float delta);
    // The items' growth toward their size (the tail of C# Update).
    void grow(float delta);

    // C# Draw (between mdk_render_canvas_begin and _end).
    void draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
              const FontView& small);

    // C# ShowOptions: the options' pages, Esc and Back acting `back` (an owner
    // action id).
    void showOptions(int32_t back);

    // C# RowFor, UsesBigFont, RowTop.
    static float rowFor(int32_t entries);
    static bool usesBigFont(float row);
    static float rowTop(float row, int32_t index);
    // C# VolumeStep.
    static int32_t volumeStep(int32_t value, int32_t step);

    // The page, for the goldens: entries, the selection, each one's growth and
    // the rows they stand in (after layout()).
    struct Entry {
        std::string text;
        bool title = false;
        bool disabled = false;
        int32_t action = -1;
        // An option (C# Entry.Change != null): its id, relabelled on change.
        int32_t option = -1;
        float grow = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        bool selectable() const { return !title && !disabled; }
    };
    const std::vector<Entry>& entries() const { return entries_; }
    int32_t selected() const { return selected_; }
    // C# Layout (0x4265c0): the rows on the view.
    void layout();

    Align alignment = Align::Centre;
    // The row the first item stands in.
    int32_t firstRow = 0;
    bool visible = true;

private:
    void clearPage();
    void add(const Entry& entry);
    void addOption(int32_t option);
    std::string optionLabel(int32_t option) const;
    void stepOption(int32_t option, int32_t step);
    // An options action, acted on (the C#'s lambdas inside MenuItems). False
    // for an owner's action.
    bool act(const Event& event, Event* forOwner);
    void move(int32_t step);
    void showPage(int32_t page, int32_t row);
    void endPage(int32_t back);
    int32_t textWidth(bool big, const std::string& text) const;

    std::vector<Entry> entries_;
    int32_t selected_ = -1;
    int32_t back_ = -1;
    // The owner's action the options page goes back to (C# ShowOptions' back).
    int32_t optionsBack_ = -1;
    // The time to the next step of an option whose Left/Right is held.
    float repeat_ = 0.0f;
    const formats::Font* big_ = nullptr;
    const formats::Font* small_ = nullptr;
    Host host_ = Host::Console;
    GameSettings* settings_ = nullptr;
    AppliedFn applied_ = nullptr;
    void* appliedUser_ = nullptr;
    PrintFn print_ = nullptr;
    void* printUser_ = nullptr;
};

} // namespace menu
} // namespace mdk
