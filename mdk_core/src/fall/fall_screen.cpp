// fall_screen.cpp — see fall_screen.h. The C#'s FallScreen.cs, method for method.

#include "fall/fall_screen.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/sni.h"

namespace mdk {
namespace fall {

namespace {

// The original mixes the fall's sounds on 8 voices at most; more are dropped.
constexpr size_t kMaxVoices = 8;
constexpr float kMaxDelta = 0.1f;
constexpr float kMessageSeconds = 3.0f;

int32_t signOf(int32_t v) {
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

int32_t held(const Input& input, Key key) {
    return input.isDown(key) ? 1 : 0;
}

} // namespace

void FallScreen::say(const char* format, ...) {
    if (print_.line == nullptr) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    print_.line(print_.context, text);
}

bool FallScreen::open(MdkRenderer renderer, const formats::MdkData& data, const formats::Fti& fti,
                      const formats::Font& big, const formats::Font& small, int32_t level,
                      Kurt::Difficulty difficulty, int32_t seed, const FallViewOptions& view, const Print& print,
                      std::string* why) {
    close(renderer);
    print_ = print;
    scope_ = mdk_render_mark(renderer);
    open_ = true;

    // C# FallScreen: `index = Math.Clamp(GameState.IndexOf(state.Level), 0, LastIndex)`.
    const int32_t index = fallIndex(level);
    char name[48];
    std::vector<uint8_t> bytes;
    if (!data.read("FALL3D/FALL3D.BNI", &bytes) || !formats::Bni::parse(&bni_, std::move(bytes))) {
        *why = "FALL3D/FALL3D.BNI did not load";
        close(renderer);
        return false;
    }
    snprintf(name, sizeof(name), "FALL3D/FALL3D_%d.MTI", (int)(index + 1));
    bytes.clear();
    if (!data.read(name, &bytes)) {
        *why = std::string(name) + " did not load";
        close(renderer);
        return false;
    }
    // C# TextureArchive.Load: the file's base and header, index 0 see-through.
    mti_ = formats::TextureArchive::parse(bytes.data(), (int32_t)bytes.size(), formats::TextureArchive::kFileBase,
                                          formats::TextureArchive::kHeader, formats::TextureArchive::Names::AsStored,
                                          formats::TextureArchive::Zero::SeeThrough);

    FallSim::Setup setup;
    if (!fallSetup(bni_, level, difficulty, &setup)) {
        *why = "the fall's animations did not parse";
        close(renderer);
        return false;
    }
    view_.reset(new FallView());
    if (!view_->load(renderer, bni_, mti_, index, fti, big, small, setup.pickups, view, why)) {
        close(renderer);
        return false;
    }

    // C# `new SoundBank([Sni.Load(FALL3D.SNI)])`, every sound decoded now so the
    // fall never decodes on its frame (the C# decodes on first play).
    std::vector<formats::Sni> archives(1);
    bytes.clear();
    if (!data.read("FALL3D/FALL3D.SNI", &bytes) || !formats::Sni::parse(&archives[0], std::move(bytes))) {
        archives.clear();
        say("Fall: FALL3D/FALL3D.SNI did not load; the fall is silent");
    }
    bank_.reset(new audio::SoundBank());
    audio::SoundBank::withArchives(bank_.get(), std::move(archives));
    bank_->preload();
    mixer_.reset(new audio::SoundMixer(bank_.get()));
    voices_.reserve(kMaxVoices);

    sim_.reset(new FallSim(setup, seed));
    FallSim::Hooks hooks;
    hooks.context = this;
    hooks.sound = soundHook;
    hooks.message = messageHook;
    sim_->setHooks(hooks);

    // The wind plays from the start, silent until the intro's end.
    windVoice_ = loop("WINDLOOP");
    std::string names;
    for (size_t i = 0; i < setup.pickups.size(); i++) {
        names += (i == 0 ? "" : ",") + setup.pickups[i];
    }
    say("Fall: index %d difficulty %d beam %.2f missiles %d spread %.2f missile gap %d radar gap %d "
           "pickups %s seed %d",
           (int)index, (int)setup.difficulty, (double)sim_->beamSpeed(), (int)sim_->missilesPerDetection(),
           (double)sim_->spread(), (int)sim_->missileGap(), (int)sim_->radarGap(), names.c_str(), (int)seed);
    return true;
}

FallSim::Input FallScreen::steering(const Input& input, bool back) {
    // C# Steering: right = TurnRight + StrafeRight − TurnLeft − StrafeLeft, up =
    // Forward − Back, each Math.Sign'd.
    FallSim::Input out;
    const int32_t right = held(input, kKeyTurnRight) + held(input, kKeyStrafeRight) - held(input, kKeyTurnLeft) -
                          held(input, kKeyStrafeLeft);
    const int32_t up = held(input, kKeyForward) - held(input, kKeyBack);
    out.steerX = signOf(right);
    out.steerY = signOf(up);
    out.skip = back;
    return out;
}

void FallScreen::chase(const FallSim& sim, FallSim::Input* input) {
    const formats::Vec3f& k = sim.kurtPosition();
    const std::vector<FallSim::Pickup>& pickups = sim.pickups();
    for (size_t i = 0; i < pickups.size(); i++) {
        if (!pickups[i].chute) {
            continue;
        }
        const float dx = pickups[i].position.x - k.x;
        const float dy = pickups[i].position.y - k.y;
        input->steerX = fabsf(dx) < 1.0f ? 0 : (dx > 0.0f ? 1 : -1);
        input->steerY = fabsf(dy) < 1.0f ? 0 : (dy > 0.0f ? 1 : -1);
        return;
    }
}

void FallScreen::frame(const FallSim::Input& input, float elapsed) {
    if (!sim_) {
        return;
    }
    // C# Frame: Esc skips, then the update (FallSim::update(delta, Input) does both,
    // in that order), the sounds, the HUD's clocks.
    sim_->update(elapsed, input);
    updateSounds();
    if (!sim_->inIntro()) {
        view_->hud().messages().update(elapsed < kMaxDelta ? elapsed : kMaxDelta);
        view_->hud().tick(sim_->ticks());
    }
}

void FallScreen::draw(MdkRenderer renderer) {
    if (sim_) {
        view_->draw(renderer, *sim_);
    }
}

flow::Event FallScreen::next(flow::GameState* state) {
    if (!sim_) {
        return flow::Event::None;
    }
    switch (sim_->state()) {
        case FallSim::Landed:
            report("Fall ended");
            // C# `_state.Carry = new FallCarry(_sim.Health, [.. _sim.Collected])`.
            state->carry = flow::FallCarry();
            sim_->carry(&state->carry);
            state->hasCarry = true;
            return flow::Event::Play;
        case FallSim::GameOver:
            report("Fall: Kurt died");
            return flow::Event::Menu;
        case FallSim::Running:
            break;
    }
    return flow::Event::None;
}

std::string FallScreen::status() const {
    if (!sim_) {
        return std::string();
    }
    char text[160];
    snprintf(text, sizeof(text), "fall%s t %.1f kurt z %.1f health %d pickups %d", sim_->inIntro() ? " intro" : "",
             (double)sim_->time(), (double)sim_->kurtPosition().z, (int)sim_->health(),
             (int)sim_->collected().size());
    return text;
}

void FallScreen::report(const char* what) {
    // C# Report.
    const formats::Vec3f& k = sim_->kurtPosition();
    std::string names;
    const std::vector<std::string>& collected = sim_->collected();
    for (size_t i = 0; i < collected.size(); i++) {
        names += (i == 0 ? "" : ",") + collected[i];
    }
    say("%s t %.1f kurt %.2f %.2f %.1f camera %.1f wind %d health %d pickups %s", what, (double)sim_->time(),
           (double)k.x, (double)k.y, (double)k.z, (double)sim_->cameraPosition().z,
           (int)(sim_->wind() >> FallSim::kWindShift), (int)sim_->health(), names.c_str());
}

void FallScreen::soundHook(void* context, const char* name) {
    static_cast<FallScreen*>(context)->play(name);
}

void FallScreen::messageHook(void* context, const char* name) {
    // C# `_view.Hud.Messages.Push(name, Messages.FlagZoom, MessageSeconds)`.
    static_cast<FallScreen*>(context)->view_->hud().messages().push(name, Messages::kFlagZoom, kMessageSeconds);
}

void FallScreen::play(const char* name) {
    // C# Play: the finished voices forgotten, then a voice if there are fewer than
    // 8 and the bank has the sound, at full gain.
    audio::SoundDevice& device = mixer_->device();
    size_t kept = 0;
    for (size_t i = 0; i < voices_.size(); i++) {
        if (device.isPlaying(voices_[i])) {
            voices_[kept++] = voices_[i];
        }
    }
    voices_.resize(kept);
    audio::SoundBank::Entry entry;
    if (voices_.size() >= kMaxVoices || !bank_->get(name, &entry)) {
        return;
    }
    voices_.push_back(device.play(entry, 1.0f));
}

int32_t FallScreen::loop(const char* name) {
    audio::SoundBank::Entry entry;
    return bank_->get(name, &entry) ? mixer_->device().play(entry, 0.0f) : 0;
}

void FallScreen::updateSounds() {
    // C# UpdateSounds.
    audio::SoundDevice& device = mixer_->device();
    device.set(windVoice_, sim_->windVolume(), 1.0f, 0.0f);
    if (sim_->inIntro()) {
        return;
    }
    // The minecrawler grinds from the start of the fall.
    if (grindVoice_ == 0) {
        grindVoice_ = loop("C_GRIND");
    }
    device.set(grindVoice_, sim_->grindVolume(), 1.0f, 0.0f);
}

void FallScreen::close(MdkRenderer renderer) {
    if (mixer_) {
        mixer_->device().stopAll();
    }
    sim_.reset();
    mixer_.reset();
    bank_.reset();
    view_.reset();
    voices_.clear();
    windVoice_ = 0;
    grindVoice_ = 0;
    mti_ = formats::TextureArchive();
    bni_ = formats::Bni();
    if (open_) {
        mdk_render_release(renderer, scope_);
        open_ = false;
    }
}

} // namespace fall
} // namespace mdk
