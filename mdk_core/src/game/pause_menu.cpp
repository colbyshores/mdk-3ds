// pause_menu.cpp -- see pause_menu.h. C# src/Mdk.Game/Menu/PauseMenu.cs.

#include "game/pause_menu.h"

#include "game/screen_view.h"

namespace mdk {
namespace menu {
namespace {

// C# PauseMenu.Dim.
const Colour kDim{0.0f, 0.0f, 0.0f, 0.6f};

} // namespace

// C# Show.
void PauseMenu::show() {
    // A new C# PauseMenu comes with each Viewer, so its _next starts at None;
    // this one outlives a level, so a show starts it there again.
    next_ = flow::Event::None;
    open_ = true;
    optionsOnly_ = false;
    showMain();
}

void PauseMenu::openOptions() {
    next_ = flow::Event::None;
    open_ = true;
    optionsOnly_ = true;
    items_.showOptions(ActOptionsBack);
}

// C# Update: Esc resumes; on the options' pages it goes up a page.
flow::Event PauseMenu::update(const Keys& keys, float delta) {
    if (!open_) {
        return next_;
    }
    if (keys.back && !items_.handlesBack()) {
        resume();
        return next_;
    }
    const Event event = items_.handle(keys, delta);
    act(event);
    items_.grow(delta);
    return next_;
}

void PauseMenu::draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                     const FontView& small) {
    if (!open_) {
        return;
    }
    view.fillCanvas(renderer, kDim);
    items_.draw(renderer, view, big, small);
}

void PauseMenu::act(const Event& event) {
    if (event.kind == Event::None) {
        return;
    }
    switch (event.action) {
        case ActResume:
            resume();
            break;
        case ActOptions:
            items_.showOptions(ActOptionsBack);
            break;
        case ActOptionsBack:
            if (optionsOnly_) {
                resume();
            } else {
                showMain();
            }
            break;
        case ActMainMenu:
            next_ = flow::Event::Menu;
            break;
        case ActQuit:
            next_ = flow::Event::Quit;
            break;
        default:
            break;
    }
}

// C# ShowMain (the keys' hint is #19's: pause_menu.h).
void PauseMenu::showMain() {
    items_.clear();
    items_.addItem("Resume", ActResume);
    items_.addItem("Options", ActOptions);
    items_.addItem("Main menu", ActMainMenu);
    items_.addItem("Quit", ActQuit);
    items_.print();
}

// C# Resume.
void PauseMenu::resume() {
    items_.clear();
    open_ = false;
}

} // namespace menu
} // namespace mdk
