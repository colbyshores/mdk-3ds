// stream_screen.cpp — see stream_screen.h. C# src/Mdk.Game/Stream/StreamScreen.cs,
// member by member.

#include "stream/stream_screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "formats/fti.h"
#include "formats/sni.h"

namespace mdk {
namespace stream {

namespace {

// C# StreamScreen's constants.
const char* const kArchive = "STREAM/STREAM.BNI";
const char* const kTextures = "STREAM/STREAM.MTI";
const char* const kSystemFile = "MISC/MDKFONT.FTI";
const char* const kSystemPalette = "SYS_PAL";
const char* const kStreamPalette = "PAL";
constexpr int32_t kRgbSize = 3;
const char* const kKurtAnimation = "KURTANIM";
const char* const kRescueAnimation = "BONESANIM";
const char* const kHitSound = "HITSIDE";
const char* const kWindSound = "WIND";

// The in-memory SNI the bank reads the BNI's WAVs from (formats/sni.h): the
// retail layout, its directory at 0x14 (`u32 count`, then per entry char[12]
// name, u16 flags, u16 volume, u32 offset from 4, u32 length).
constexpr int32_t kSniDirectory = 0x14;
constexpr int32_t kSniEntryBytes = 24;
constexpr int32_t kSniNameBytes = 12;
constexpr int32_t kFullVolume = 0x7FFF;

void putU16(std::vector<uint8_t>* out, size_t at, uint32_t v) {
    (*out)[at] = (uint8_t)(v & 0xFFu);
    (*out)[at + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

void putU32(std::vector<uint8_t>* out, size_t at, uint32_t v) {
    putU16(out, at, v & 0xFFFFu);
    putU16(out, at + 2, v >> 16);
}

// The BNI's RIFF entries as an SNI: every one at full volume, none flagged to
// loop (WIND loops because it is played looped, as the C# plays it with
// Looping.Forever).
bool soundArchiveOf(const formats::Bni& bni, formats::Sni* out) {
    const std::vector<uint8_t>& bytes = bni.bytes();
    std::vector<const std::pair<std::string, formats::ArchiveIndex::Entry>*> sounds;
    for (size_t i = 0; i < bni.entries().size(); i++) {
        const std::pair<std::string, formats::ArchiveIndex::Entry>& e = bni.entries()[i];
        if (e.first.size() <= (size_t)kSniNameBytes && e.second.offset >= 0 && e.second.size >= 4 &&
            e.second.offset + e.second.size <= (int32_t)bytes.size() &&
            memcmp(bytes.data() + e.second.offset, "RIFF", 4) == 0) {
            sounds.push_back(&e);
        }
    }
    size_t total = (size_t)kSniDirectory + 4 + sounds.size() * (size_t)kSniEntryBytes;
    for (size_t i = 0; i < sounds.size(); i++) {
        total += (size_t)sounds[i]->second.size;
    }
    std::vector<uint8_t> sni(total, 0);
    putU32(&sni, (size_t)kSniDirectory, (uint32_t)sounds.size());
    size_t data = (size_t)kSniDirectory + 4 + sounds.size() * (size_t)kSniEntryBytes;
    for (size_t i = 0; i < sounds.size(); i++) {
        const size_t at = (size_t)kSniDirectory + 4 + i * (size_t)kSniEntryBytes;
        memcpy(sni.data() + at, sounds[i]->first.c_str(), sounds[i]->first.size());
        putU16(&sni, at + 12, 0);
        putU16(&sni, at + 14, (uint32_t)kFullVolume);
        putU32(&sni, at + 16, (uint32_t)(data - 4));
        putU32(&sni, at + 20, (uint32_t)sounds[i]->second.size);
        memcpy(sni.data() + data, bytes.data() + sounds[i]->second.offset, (size_t)sounds[i]->second.size);
        data += (size_t)sounds[i]->second.size;
    }
    return formats::Sni::parse(out, std::move(sni));
}

const char* kindName(StreamTube::Kind kind) { return kind == StreamTube::Gunter ? "Gunter" : "Normal"; }

const char* outcomeName(Outcome outcome) { return outcome == Died ? "Died" : "Survived"; }

} // namespace

Steering steeringOf(const Input& input) {
    const int32_t right = (input.isDown(kKeyTurnRight) ? 1 : 0) + (input.isDown(kKeyStrafeRight) ? 1 : 0) -
                          (input.isDown(kKeyTurnLeft) ? 1 : 0) - (input.isDown(kKeyStrafeLeft) ? 1 : 0);
    const int32_t up = (input.isDown(kKeyForward) ? 1 : 0) - (input.isDown(kKeyBack) ? 1 : 0);
    Steering s;
    s.right = right > 0 ? 1 : (right < 0 ? -1 : 0);
    s.up = up > 0 ? 1 : (up < 0 ? -1 : 0);
    return s;
}

Steering steeringWithPad(Steering keys, int32_t padX, int32_t padY, int32_t dead) {
    const int32_t right = keys.right + (padX > dead ? 1 : 0) - (padX < -dead ? 1 : 0);
    const int32_t up = keys.up + (padY > dead ? 1 : 0) - (padY < -dead ? 1 : 0);
    Steering s;
    s.right = right > 0 ? 1 : (right < 0 ? -1 : 0);
    s.up = up > 0 ? 1 : (up < 0 ? -1 : 0);
    return s;
}

StreamScreen::StreamScreen() : mixer_(&bank_) {
    for (int32_t i = 0; i < kPlayers; i++) {
        voices_[i] = 0;
    }
}

StreamScreen::~StreamScreen() {
    // The voices first (the mixer gives the bank its pins back as it forgets
    // them), then the flight.
    mixer_.stop(kWindSound);
    delete run_;
    run_ = nullptr;
}

void StreamScreen::say(const char* format, ...) {
    if (setup_.log == nullptr) {
        return;
    }
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    setup_.log(setup_.logUser, line);
}

// C# StreamScreen's constructor.
bool StreamScreen::open(const Setup& setup) {
    setup_ = setup;
    if (setup.renderer == nullptr || setup.data == nullptr || !setup.data->found()) {
        say("stream: no data folder");
        return false;
    }
    const int32_t index = streamIndexOf(setup.level);
    const StreamTube::Kind kind = streamKindOf(index);

    // _bni = Bni.Load(STREAM/STREAM.BNI).
    std::vector<uint8_t> bytes;
    if (!setup.data->read(kArchive, &bytes) || !formats::Bni::parse(&bni_, std::move(bytes))) {
        say("stream: %s could not be read", kArchive);
        return false;
    }

    // LoadPalette: colours 0-63 are the system palette, 64-255 the stream's PAL.
    {
        const int32_t systemSize = formats::Palette::kArenaFirstIndex * kRgbSize;
        uint8_t rgb[formats::Palette::kSize * kRgbSize];
        memset(rgb, 0, sizeof(rgb));
        std::vector<uint8_t> ftiBytes;
        formats::Fti fti;
        std::vector<uint8_t> system;
        if (!setup.data->read(kSystemFile, &ftiBytes) || !formats::Fti::parse(&fti, std::move(ftiBytes)) ||
            !fti.getBytes(kSystemPalette, &system) || (int32_t)system.size() < systemSize) {
            say("stream: %s has no %s", kSystemFile, kSystemPalette);
            return false;
        }
        memcpy(rgb, system.data(), (size_t)systemSize);
        const formats::ArchiveIndex::Entry* entry = bni_.index().find(kStreamPalette);
        const int32_t restSize = (int32_t)sizeof(rgb) - systemSize;
        if (entry == nullptr || entry->offset + systemSize + restSize > (int32_t)bni_.bytes().size()) {
            say("stream: STREAM.BNI has no %s", kStreamPalette);
            return false;
        }
        memcpy(rgb + systemSize, bni_.bytes().data() + entry->offset + systemSize, (size_t)restSize);
        palette_ = formats::Palette::fromRgb(rgb, (int32_t)sizeof(rgb));
    }

    // TextureArchive.Load(STREAM/STREAM.MTI): FileBase, Header, names as stored,
    // index 0 see-through.
    bytes.clear();
    if (!setup.data->read(kTextures, &bytes)) {
        say("stream: %s could not be read", kTextures);
        return false;
    }
    archive_ = formats::TextureArchive::parse(bytes.data(), (int32_t)bytes.size(),
                                              formats::TextureArchive::kFileBase,
                                              formats::TextureArchive::kHeader,
                                              formats::TextureArchive::Names::AsStored,
                                              formats::TextureArchive::Zero::SeeThrough);

    // The view (the images, the meshes, the models and their poses).
    char error[128] = {0};
    if (!view_.load(setup.renderer, &bni_, &palette_, &archive_, kind, error, (int32_t)sizeof(error))) {
        say("stream: %s", error);
        return false;
    }
    Clip kurt;
    Clip rescue;
    Clip other;
    if (!view_.clipOf(kKurtAnimation, &kurt) || !view_.clipOf(kRescueAnimation, &rescue) ||
        !view_.clipOf(view_.otherAnimation(), &other)) {
        say("stream: STREAM.BNI is missing an animation");
        return false;
    }

    // The tube, then the Flight, from the same random (StreamRun keeps the C#'s
    // order of draws). On the heap: ~25 KB of fixed arrays.
    run_ = new StreamRun(setup.level, setup.difficulty, setup.seed, setup.health, kurt, rescue, other);
    if (!view_.loadTubeTable(run_->tube, error, (int32_t)sizeof(error))) {
        say("stream: %s", error);
        delete run_;
        run_ = nullptr;
        return false;
    }

    // The sounds: the BNI's WAVs through the shared mixer.
    std::vector<formats::Sni> archives(1);
    if (!soundArchiveOf(bni_, &archives[0]) || !audio::SoundBank::withArchives(&bank_, std::move(archives))) {
        say("stream: the sounds did not load; the stream is silent");
    }

    // _ui.Play(Ui.SoundOf(_bni, WindSound, Looping.Forever)).
    mixer_.playLooped(kWindSound);

    // There is no sky in the stream: the background is a quad of its own.
    const Panorama none;
    mdk_render_set_panorama(setup.renderer, &none);

    const StreamTube& tube = run_->tube;
    const int32_t last = tube.head() - 1;
    const Vec3 ring = tube.origin(last);
    say("Stream after LEVEL%d: %s, turn %g, radius %g-%g, %d lights, ring %d at %.3f %.3f %.3f",
        (int)setup.level, kindName(kind), (double)tube.maxTurn(), (double)tube.minRadius(),
        (double)tube.maxRadius(), (int)run_->flight.lightCount(), (int)last, (double)ring.x, (double)ring.y,
        (double)ring.z);
    return true;
}

int32_t StreamScreen::health() const { return run_ != nullptr ? run_->flight.health() : setup_.health; }

const Flight* StreamScreen::flight() const { return run_ != nullptr ? &run_->flight : nullptr; }

// C# Frame, its first half: Stop on Back, Flight.Update, PlaySounds, Report,
// and the frame's renderer state. Before begin_frame: the Vulkan backend takes
// the camera the frame is drawn with at begin_frame (07_frame.inc's
// `activeCamera = camera`), so a camera set after it would draw this frame
// from the last one's eye -- measured against the C# oracle's frame at 10 s:
// Kurt, one unit of flight nearer than the eye that drew him, came out 12%
// small. The 3DS keeps the camera until end_frame, so before is right on both.
flow::Event StreamScreen::update(float elapsed, Steering steering, bool back) {
    if (run_ == nullptr) {
        // A stream that could not open ends at once with the health it was given
        // (the flow's own skip, GameFlow::skipScreen).
        return setup_.health < flow::kStreamLowHealth ? flow::Event::Menu : flow::Event::StreamEnded;
    }
    Flight& flight = run_->flight;
    if (back) {
        flight.stop();
    }

    flight.update(elapsed, steering);
    playSounds();
    report();

    // The frame's state, every frame (AGENTS.md P2): no fade, no lighting rig,
    // no glow or point lights -- the level's are gone with it.
    MdkRenderer renderer = setup_.renderer;
    mdk_render_set_fade(renderer, 1.0f);
    const Lighting unlit;
    mdk_render_set_lighting(renderer, &unlit);
    mdk_render_set_point_lights(renderer, nullptr, 0);
    mdk_render_set_glow_quads(renderer, nullptr, 0);
    Camera camera;
    view_.camera(flight, &camera);
    mdk_render_set_camera(renderer, &camera);

    if (!flight.ended()) {
        return flow::Event::None;
    }
    return flight.outcome() == Died ? flow::Event::Menu : flow::Event::StreamEnded;
}

// C# Frame, its second half: StreamView.Draw (Present is the frontend's end_frame).
void StreamScreen::draw() {
    if (run_ != nullptr) {
        view_.draw(run_->flight);
    }
}

// C# PlaySounds: the flight's sounds; a wall hit's thud doesn't start again
// while it plays. IsPlaying is the DEVICE's answer, as the C#'s AudioDevice's.
void StreamScreen::playSounds() {
    const Flight& flight = run_->flight;
    const audio::SoundDevice& device = mixer_.device();
    if (flight.hasEvent(WallHit) && !device.isPlaying(hitVoice_)) {
        hitVoice_ = mixer_.play(kHitSound);
    }

    int32_t kept = 0;
    for (int32_t i = 0; i < voiceCount_; i++) {
        if (device.isPlaying(voices_[i])) {
            voices_[kept++] = voices_[i];
        }
    }
    voiceCount_ = kept;
    for (int32_t i = 0; i < flight.soundCount(); i++) {
        if (voiceCount_ >= kPlayers) {
            return;
        }
        voices_[voiceCount_++] = mixer_.play(flight.sound(i));
    }
}

// C# Report: the stream's moments (tests compare them).
void StreamScreen::report() {
    const Flight& flight = run_->flight;
    const StreamTube& tube = flight.tube();
    const Thing& kurt = flight.kurt();
    for (int32_t i = 0; i < flight.eventCount(); i++) {
        switch (flight.event(i)) {
            case WallHit:
                if (wallHits_++ == 0) {
                    say("Stream: first wall hit at segment %d, t %.3f x %.3f z %.3f yaw %.2f speed %.3f health %d",
                        (int)tube.tail(), (double)kurt.t, (double)kurt.x, (double)kurt.z, (double)kurt.yaw,
                        (double)kurt.speed, (int)flight.health());
                }
                break;
            case BonesCame:
                say("Stream: Bones comes at segment %d, health %d, %.1f s", (int)tube.tail(), (int)flight.health(),
                    (double)flight.time());
                break;
            case KurtDied:
                say("Stream: Kurt died at segment %d, %.1f s", (int)tube.tail(), (double)flight.time());
                break;
            case BonusTaken:
                say("Stream: health bonus, health %d", (int)flight.health());
                break;
            case Ended:
                say("Stream ended after LEVEL%d: %s, health %d, segment %d, %.1f s", (int)setup_.level,
                    outcomeName(flight.outcome()), (int)flight.health(), (int)tube.tail(), (double)flight.time());
                break;
        }
    }
}

void StreamScreen::status(char* out, int32_t size) const {
    if (out == nullptr || size <= 0) {
        return;
    }
    if (run_ == nullptr) {
        snprintf(out, (size_t)size, "stream after LEVEL%d (not open)", (int)setup_.level);
        return;
    }
    const Flight& flight = run_->flight;
    snprintf(out, (size_t)size, "stream after LEVEL%d segment %d t %.1f speed %.3f health %d", (int)setup_.level,
             (int)flight.tube().tail(), (double)flight.time(), (double)flight.kurt().speed, (int)flight.health());
}

} // namespace stream
} // namespace mdk
