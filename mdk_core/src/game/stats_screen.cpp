// stats_screen.cpp -- see stats_screen.h. C# Menu/StatsScreen.cs, operation for
// operation: the constructor (:144-170), Frame (:175-203), Update (:205-230),
// Start (:232-276), EndPhase/NextPhase (:280-305), PageRgb/LoadImage (:308-321),
// UpdatePage/Play (:324-342), UpdateTyping/DrawTyped (:347-416), StartScore,
// UpdateScore, UpdateRow, CountRow, UpdateHeads (:420-557), Draw/DrawScore
// (:561-636), ValueText (:639-644), Dispose (:646).

#include "game/stats_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "formats/bni.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "formats/wav.h"
#include "game/briefing.h"
#include "game/font_view.h"
#include "game/level_flow.h"

namespace mdk {
namespace {

// C# StatsScreen's constants.
const float kFadeSpeed = 2.0f;
const float kFast = 2.0f;
// Typing (0x4335c0): characters per second, and when fast.
const float kTypeRate = 15.0f;
const float kTypeRateFast = 60.0f;
const float kDebriefY[3] = {64.0f, 120.0f, 300.0f};
const float kBriefY = 32.0f;
// Debriefing texts by the town flags' bits 31-29 (0x57440f).
const char* const kDebriefResults[8] = {"S", "S", "F", "S", "SS", "SF", "FS", "FF"};
const int32_t kTownShift = 29;
const int32_t kTownMask = 7;
// Score-O-matic rows (tables 0x491b78 and 0x491bd8): label, start and end
// (x, y, bar width, scale).
struct Row {
    const char* label;
    float from[4];
    float to[4];
};
const Row kRowTable[StatsScreen::kRows] = {
    {"ST_SHF", {300.0f, 85.0f, 240.0f, 256.0f}, {180.0f, 75.0f, 120.0f, 128.0f}},
    {"ST_ACC", {300.0f, 155.0f, 240.0f, 256.0f}, {420.0f, 75.0f, 120.0f, 128.0f}},
    {"ST_SNF", {300.0f, 155.0f, 240.0f, 256.0f}, {180.0f, 125.0f, 120.0f, 128.0f}},
    {"ST_ACC", {300.0f, 225.0f, 240.0f, 256.0f}, {420.0f, 125.0f, 120.0f, 128.0f}},
    {"ST_KILL", {300.0f, 225.0f, 240.0f, 256.0f}, {300.0f, 175.0f, 120.0f, 128.0f}},
};
const int32_t kKillsRow = 4;
const float kFullScale = 256.0f;
const float kHeadLabelX = 300.0f;
const float kHeadLabelY = 255.0f;
const float kRowWait = 0.5f;
const int32_t kBarColour = 63;
const float kBarHeight = 16.0f;
const float kBarRise = 18.0f;
const float kValueGap = 8.0f;
const float kValueDrop = 14.0f;
const float kTitleY = 28.0f;
const float kNameY = 48.0f;
const float kCountRate = 0.5f;
const float kSlideSpeed = 2.0f;
const int32_t kRicochetChance = 8;
const int32_t kRicochets = 3;
const int32_t kCursorBlink = 31;
const int32_t kCursorOn = 15;
const float kTicksPerSecond = 30.0f;
const float kHidden = -1.0f;
// A row shows big (t = 0) until it slides.
const float kBig = -0.5f;
const int32_t kPaletteSize = 768;

const char* const kSoundNames[StatsScreen::kSounds] = {
    "CGUN", "SNIPER", "RICO1", "RICO2", "RICO3", "ALDIE", "XGHEAD1", "XGHEAD2", "TELETYPE",
};

float minf(float a, float b) { return a < b ? a : b; }
float maxf(float a, float b) { return a > b ? a : b; }

} // namespace

const char* StatsScreen::phaseName(Phase phase) {
    switch (phase) {
        case Phase::None: return "None";
        case Phase::Score: return "Score";
        case Phase::Intermission: return "Intermission";
        case Phase::Briefing: return "Briefing";
        case Phase::Debriefing: return "Debriefing";
    }
    return "?";
}

const char* StatsScreen::soundName(int32_t sound) {
    return sound >= 0 && sound < kSounds ? kSoundNames[sound] : "?";
}

const char* StatsScreen::soundOf(int32_t voice) const {
    const std::vector<audio::SoundDevice::Voice>& voices = device_.voices();
    for (size_t v = 0; v < voices.size(); v++) {
        if (voices[v].id != voice) {
            continue;
        }
        for (int32_t i = 0; i < kSounds; i++) {
            if (hasSound_[i] && voices[v].sound == &sounds_[i]) {
                return kSoundNames[i];
            }
        }
    }
    return "?";
}

std::string StatsScreen::valueText(int32_t row, int32_t value, int32_t full) {
    char text[32];
    if (row == kKillsRow) {
        snprintf(text, sizeof(text), "%d/%d", (int)value, (int)full);
    } else if (row == 1 || row == 3) {
        snprintf(text, sizeof(text), "%d%%", (int)value);
    } else {
        snprintf(text, sizeof(text), "%d", (int)value);
    }
    return text;
}

// C# constructor.
void StatsScreen::open(const Setup& setup) {
    close();
    fti_ = setup.fti;
    stats_ = setup.stats;
    memset(systemRgb_, 0, sizeof(systemRgb_));
    if (setup.systemRgb) {
        memcpy(systemRgb_, setup.systemRgb, sizeof(systemRgb_));
    }
    state_ = setup.state;
    saves_ = setup.saves;
    gore_ = setup.gore;
    random_ = setup.random;
    seed_ = setup.seed;
    print_ = setup.print;
    prompt_.reset();
    closedPrompt_.reset();
    drawPrompt_ = false;
    next_ = flow::Event::None;
    ticks_ = 0.0f;
    fast_ = false;
    skip_ = false;
    pressed_ = false;
    for (int32_t i = 0; i < kPlayers; i++) {
        voices_[i] = 0;
    }
    loopVoice_ = 0;
    teletypeVoice_ = 0;
    headsView_.forget();
    image_ = PalettedImage();
    imageLoaded_.clear();

    // The sounds: STATS.BNI's WAVs (C# Ui.SoundOf), CGUN looping.
    for (int32_t i = 0; i < kSounds; i++) {
        hasSound_[i] = false;
        entries_[i] = audio::SoundBank::Entry();
        const formats::ArchiveIndex::Entry* entry =
            stats_ ? stats_->index().find(kSoundNames[i]) : nullptr;
        if (!entry || entry->offset < 0 || entry->size <= 0 ||
            (int64_t)entry->offset + entry->size > (int64_t)stats_->bytes().size()) {
            continue;
        }
        formats::Wav wav;
        if (!formats::Wav::parse(&stats_->bytes()[(size_t)entry->offset], entry->size, &wav)) {
            continue;
        }
        const bool looping = i == Cgun;
        audio::soundFromPcm(wav.data(), wav.dataSize(), wav.channels(), wav.sampleRate(),
                            wav.bitsPerSample(), looping, &sounds_[i]);
        entries_[i].looping = looping;
        entries_[i].channels = wav.channels();
        entries_[i].sampleRate = wav.sampleRate();
        entries_[i].frames = wav.frames();
        entries_[i].sound = &sounds_[i];
        hasSound_[i] = true;
    }

    const int32_t index = state_ ? flow::indexOf(state_->level) : 0;
    index_ = index > 0 ? index : 0;
    phases_.clear();
    if (setup.pages == StatsPages::Briefing) {
        phases_.push_back(Phase::Briefing);
    } else {
        phases_.push_back(Phase::Intermission);
        phases_.push_back(Phase::Debriefing);
        phases_.push_back(Phase::Score);
        phases_.push_back(Phase::Briefing);
    }
    // `while (first is { } phase && _phases.Count > 1 && _phases.Peek() != phase)`.
    while (setup.first != Phase::None && phases_.size() > 1 && phases_.front() != setup.first) {
        phases_.erase(phases_.begin());
    }
    const Phase first = phases_.front();
    phases_.erase(phases_.begin());
    start(first);
}

void StatsScreen::attach(MdkRenderer renderer, const formats::MdkData& data) {
    if (stats_) {
        headsView_.load(renderer, *stats_, systemRgb_, data);
    }
}

void StatsScreen::close() {
    // C# Dispose: _ui.Audio.StopAll().
    device_.stopAll();
    headsView_.forget();
    image_ = PalettedImage();
    imageLoaded_.clear();
}

std::string StatsScreen::status() const {
    return prompt_ ? std::string("save prompt") : std::string("statistics: ") + phaseName(phase_);
}

void StatsScreen::say(const char* text) {
    if (print_.line) {
        print_.line(print_.context, text);
    }
}

int32_t StatsScreen::random(int32_t max) {
    if (random_.next) {
        return random_.next(random_.context, max);
    }
    // The screen's own generator (the C#'s unseeded Random picks only sounds).
    // An LCG's high 16 bits scaled into [0, max) (no divide).
    seed_ = seed_ * 1103515245u + 12345u;
    return max > 0 ? (int32_t)(((seed_ >> 16) * (uint32_t)max) >> 16) : 0;
}

std::vector<uint8_t> StatsScreen::text(const char* name) const {
    return fti_ ? fti_->getTextBytes(name) : std::vector<uint8_t>();
}

// C# Frame: the prompt's frame, or the keys and Update. The drawing half is draw().
flow::Event StatsScreen::frame(const Keys& keys, float elapsed) {
    closedPrompt_.reset();
    drawPrompt_ = prompt_ != nullptr;
    if (prompt_) {
        prompt_->update(keys.prompt, elapsed);
        if (prompt_->closed()) {
            if (prompt_->saved()) {
                char line[96];
                snprintf(line, sizeof(line), "Saved game %s: level %d", prompt_->name().c_str(),
                         (int)prompt_->save().level);
                say(line);
            }
            // C#: drawn and presented first, then `_prompt = null; NextPhase()`;
            // the closed prompt is kept for this frame's draw().
            closedPrompt_ = std::move(prompt_);
            nextPhase();
        }
        return next_;
    }
    if (keys.any) {
        skip_ = skip_ || keys.back;
        pressed_ = true;
    }
    update(elapsed, keys.fastHeld);
    return next_;
}

// C# Update.
void StatsScreen::update(float delta, bool fastHeld) {
    fast_ = skip_ || fastHeld;
    const float speed = fast_ ? kFast : 1.0f;
    ticks_ += delta * kTicksPerSecond;
    if (fadeOut_ >= 0.0f) {
        fadeOut_ += kFadeSpeed * speed * delta;
        if (fadeOut_ >= 1.0f) {
            endPhase();
        }
    } else if (fadeIn_ < 1.0f) {
        fadeIn_ = minf(fadeIn_ + kFadeSpeed * speed * delta, 1.0f);
    } else if (updatePage(delta, speed) && (pressed_ || fast_)) {
        fadeOut_ = 0.0f;
        device_.stop(loopVoice_);
    }
    skip_ = false;
    pressed_ = false;
}

// C# Start.
void StatsScreen::start(Phase phase) {
    char line[64];
    snprintf(line, sizeof(line), "Statistics: %s", phaseName(phase));
    say(line);
    phase_ = phase;
    fadeIn_ = 0.0f;
    fadeOut_ = -1.0f;
    fadeWhite_ = phase == Phase::Intermission;
    texts_.clear();
    textY_.clear();
    current_ = 0;
    count_ = 1.0f;
    shown_ = 0;
    imageName_.clear();
    char name[24];
    switch (phase) {
        case Phase::Intermission:
            imageName_ = "L1_INTRM";
            break;
        case Phase::Debriefing: {
            snprintf(name, sizeof(name), "L%d_MAP", (int)(index_ + 1));
            imageName_ = name;
            const int32_t flags = state_ ? state_->stats.townFlags : 0;
            const char* result = kDebriefResults[(flags >> kTownShift) & kTownMask];
            snprintf(name, sizeof(name), "DEB%d%s", (int)(index_ + 1), result);
            if (!fti_ || !fti_->has(name)) {
                snprintf(name, sizeof(name), "DEB%dS", (int)(index_ + 1));
            }
            texts_.push_back(text("DEBTOP"));
            texts_.push_back(text(name));
            texts_.push_back(text("DEBBOT"));
            for (int32_t i = 0; i < 3; i++) {
                textY_.push_back(kDebriefY[i]);
            }
            break;
        }
        case Phase::Score:
            startScore();
            break;
        case Phase::Briefing:
            // On entry the inventory is emptied and the health raised to 100
            // (every level starts with both): the level does that.
            snprintf(name, sizeof(name), "L%d_MAP", (int)(index_ + 1));
            imageName_ = name;
            snprintf(name, sizeof(name), "BRIEF%d", (int)(index_ + 1));
            texts_.push_back(text(name));
            textY_.push_back(kBriefY);
            break;
        case Phase::None:
            break;
    }
    // C# LoadImage: null when STATS.BNI has no such entry.
    if (!imageName_.empty() && (!stats_ || !stats_->has(imageName_))) {
        imageName_.clear();
    }
}

// C# EndPhase: after the Score-O-matic the index moves on to the next level,
// which can be saved (named after its number); after the briefing the level.
void StatsScreen::endPhase() {
    if (phase_ != Phase::Score) {
        nextPhase();
        return;
    }
    index_ = index_ + 1 < flow::kOrderCount - 1 ? index_ + 1 : flow::kOrderCount - 1;
    flow::SaveGame save;
    if (state_) {
        state_->level = flow::kOrder[index_];
        save = state_->save(flow::SaveKind::BeforeLevel);
    }
    char name[16];
    snprintf(name, sizeof(name), "%d",
             (int)flow::saveNameNumber(state_ ? state_->level : flow::kOrder[index_]));
    prompt_.reset(new flow::SavePrompt(saves_, save, name));
    phase_ = Phase::Briefing;
    imageName_.clear();
}

// C# NextPhase: the next page; after the last one (the briefing) the fall.
void StatsScreen::nextPhase() {
    if (phases_.empty()) {
        next_ = flow::Event::Fall;
        return;
    }
    const Phase phase = phases_.front();
    phases_.erase(phases_.begin());
    start(phase);
}

// C# UpdatePage: true once everything is shown.
bool StatsScreen::updatePage(float delta, float speed) {
    switch (phase_) {
        case Phase::Debriefing:
        case Phase::Briefing:
            return updateTyping(delta);
        case Phase::Score:
            return updateScore(delta, speed);
        case Phase::Intermission:
        case Phase::None:
            break;
    }
    return true;
}

// C# _ui.Play(sound): a voice, or 0 for a sound STATS.BNI lacks.
int32_t StatsScreen::playVoice(int32_t sound) {
    return hasSound_[sound] ? device_.play(entries_[sound], 1.0f, 1.0f, 0.0f,
                                           audio::SoundDevice::Effects)
                            : 0;
}

// C# Play: a sound on the first free voice of four (else the first's).
void StatsScreen::play(int32_t sound) {
    if (!hasSound_[sound]) {
        return;
    }
    int32_t slot = 0;
    for (int32_t i = 0; i < kPlayers; i++) {
        if (!device_.isPlaying(voices_[i])) {
            slot = i;
            break;
        }
    }
    device_.stop(voices_[slot]);
    voices_[slot] = playVoice(sound);
}

// --- Typing ---------------------------------------------------------------------

// C# UpdateTyping: the page's texts one after the other; Esc finishes the
// current one (the briefing: all).
bool StatsScreen::updateTyping(float delta) {
    if (current_ >= (int32_t)texts_.size()) {
        return true;
    }
    std::vector<TypesetLine> lines;
    const std::vector<uint8_t>& page = texts_[(size_t)current_];
    const int32_t total = typesetLayout(page.data(), (int32_t)page.size(), 0.0f, &lines);
    if (skip_) {
        if (phase_ == Phase::Briefing) {
            current_ = (int32_t)texts_.size();
            return false;
        }
        count_ = (float)total;
    } else {
        count_ += (fast_ ? kTypeRateFast : kTypeRate) * delta;
    }
    const int32_t rounded = (int32_t)roundEven(count_);
    const int32_t shown = rounded < total ? rounded : total;
    if (shown != shown_) {
        shown_ = shown;
        device_.stop(teletypeVoice_);
        teletypeVoice_ = playVoice(Teletype);
    }
    if (count_ < (float)total) {
        return false;
    }
    current_++;
    count_ = 0.0f;
    shown_ = 0;
    return current_ >= (int32_t)texts_.size();
}

// C# DrawTyped: a text with a budget of characters (-1: all), a cursor after the
// last one typed.
void StatsScreen::drawTyped(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                            const std::vector<uint8_t>& page, float y, int32_t budget) const {
    std::vector<TypesetLine> lines;
    const int32_t count = typesetLayout(page.data(), (int32_t)page.size(), y, &lines);
    const int32_t remaining = budget < 0 ? count : budget;
    for (size_t n = 0; n < lines.size(); n++) {
        const TypesetLine& line = lines[n];
        if (line.start > remaining) {
            break;
        }
        const int32_t take = remaining - line.start < (int32_t)line.text.size()
                                 ? remaining - line.start
                                 : (int32_t)line.text.size();
        std::vector<uint8_t> part(line.text.begin(), line.text.begin() + take);
        // The cursor follows the last line being typed.
        const bool last = n + 1 == lines.size() || lines[n + 1].start > remaining;
        if (last && remaining < count) {
            part.push_back(((int32_t)ticks_ & kCursorBlink) <= kCursorOn ? (uint8_t)'_'
                                                                          : (uint8_t)' ');
        }
        // A centred line is placed by its whole width, so it doesn't move while typed.
        const float left =
            line.centred
                ? line.x - (float)(big.width(line.text.data(), (int32_t)line.text.size()) >> 1)
                : line.x;
        view.text(renderer, big, part.data(), (int32_t)part.size(), left, line.y, 1.0f, 1.0f);
    }
}

// --- Score-O-matic -------------------------------------------------------------

// C# StartScore.
void StatsScreen::startScore() {
    scripts::GameStats s;
    if (state_) {
        s = state_->stats;
    }
    const int32_t targets[kRows] = {s.shots, scripts::GameStats::percent(s.shotHits, s.shots),
                                    s.sniperShots,
                                    scripts::GameStats::percent(s.sniperHits, s.sniperShots),
                                    s.kills};
    const int32_t fulls[kRows] = {s.shots, 100, s.sniperShots, 100, s.enemies};
    std::string line = "Score-O-matic: ";
    for (int32_t i = 0; i < kRows; i++) {
        targets_[i] = targets[i];
        fulls_[i] = fulls[i];
        values_[i] = 0.0f;
        slides_[i] = kHidden;
        char part[32];
        snprintf(part, sizeof(part), "%s%d/%d", i > 0 ? ", " : "", (int)targets_[i],
                 (int)fulls_[i]);
        line += part;
    }
    row_ = 0;
    rowStage_ = 0;
    rowTime_ = 0.0f;
    heads_ = 0;
    headTime_ = 0.0f;
    char heads[32];
    snprintf(heads, sizeof(heads), ", heads %d", (int)s.headShots);
    line += heads;
    say(line.c_str());
}

// C# UpdateScore: the rows one at a time (0x4328a4); then the heads.
bool StatsScreen::updateScore(float delta, float speed) {
    for (int32_t i = 0; i < kRows; i++) {
        if (slides_[i] >= 0.0f && slides_[i] < 1.0f) {
            slides_[i] = skip_ ? 1.0f : minf(slides_[i] + kSlideSpeed * speed * delta, 1.0f);
        }
    }
    if (row_ < kRows) {
        updateRow(delta, speed);
        return false;
    }
    return updateHeads(delta, speed);
}

// C# UpdateRow: wait, show big (SNIPER), wait, count up (about 2 s), slide to the
// final place.
void StatsScreen::updateRow(float delta, float speed) {
    rowTime_ += delta * speed;
    switch (rowStage_) {
        case 0:
            if (rowTime_ >= kRowWait || skip_) {
                rowStage_ = 1;
                rowTime_ = 0.0f;
                slides_[row_] = kBig;
                play(Sniper);
            }
            break;
        case 1:
            if (rowTime_ >= kRowWait || skip_) {
                rowStage_ = 2;
            }
            break;
        case 2:
            countRow(delta, speed);
            break;
        default:
            break;
    }
}

// C# CountRow.
void StatsScreen::countRow(float delta, float speed) {
    const int32_t target = targets_[row_];
    if (skip_) {
        values_[row_] = (float)target;
    } else {
        const float step = maxf(1.0f, roundEven((float)target * delta * kCountRate)) * speed;
        values_[row_] = minf(values_[row_] + step, (float)target);
        if (row_ == kKillsRow && !device_.isPlaying(voices_[0])) {
            play(Aldie);
        } else if (row_ != kKillsRow && !device_.isPlaying(loopVoice_)) {
            loopVoice_ = playVoice(Cgun);
        }
        if ((row_ == 1 || row_ == 3) && random(kRicochetChance) == 0) {
            play(Rico1 + random(kRicochets));
        }
    }
    if (values_[row_] < (float)target) {
        return;
    }
    device_.stop(loopVoice_);
    slides_[row_] = 0.0f;
    row_++;
    rowStage_ = 0;
    rowTime_ = 0.0f;
}

// C# UpdateHeads: head shots (0x433268), one a second (two when fast, all at once
// on Esc), spinning. Without gore the row is left out (0x43290b).
bool StatsScreen::updateHeads(float delta, float speed) {
    headsView_.update(heads_, delta);
    if (!gore_) {
        return true;
    }
    const int32_t count = state_ ? state_->stats.headShots : 0;
    if (row_ == kRows) {
        rowTime_ += delta * speed;
        if (rowStage_ == 0 && (rowTime_ >= kRowWait || skip_)) {
            play(Sniper);
            rowStage_ = 1;
            rowTime_ = 0.0f;
        } else if (rowStage_ == 1 && (rowTime_ >= kRowWait || skip_)) {
            row_++;
            headTime_ = 1.0f;
        }
        return false;
    }
    if (heads_ >= count) {
        return true;
    }
    if (skip_) {
        heads_ = count;
        return true;
    }
    headTime_ += delta * speed;
    if (headTime_ < 1.0f) {
        return false;
    }
    headTime_ = 0.0f;
    heads_++;
    play(Xghead1 + random(2));
    return heads_ >= count;
}

// --- Drawing ------------------------------------------------------------------

void StatsScreen::draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                       const FontView& small) {
    // The page's image (C# LoadImage, at Start): loaded when the page changed,
    // the last one released first (the C# keeps it until the screen's scope).
    if (imageName_ != imageLoaded_) {
        if (image_.ready()) {
            mdk_render_release_texture(renderer, image_.texture);
            mdk_render_release_palette(renderer, image_.palette);
        }
        image_ = PalettedImage();
        if (!imageName_.empty() && stats_) {
            loadPalettedImage(renderer, *stats_, imageName_.c_str(), systemRgb_, &image_);
        }
        imageLoaded_ = imageName_;
    }
    // The heads, under the canvas (C# HeadsView.Draw's InsetLayer.UnderCanvas):
    // only on the Score-O-matic's own frames, with gore.
    if (!drawPrompt_ && phase_ == Phase::Score && gore_) {
        headsView_.draw(renderer, heads_, state_ ? state_->stats.headShots : 0, view.toCanvasX(0.0f),
                        view.toCanvasY(0.0f), ScreenView::kWidth * view.scale,
                        ScreenView::kHeight * view.scale, view.canvasWidth);
    }
    mdk_render_canvas_begin(renderer);
    if (drawPrompt_) {
        drawPrompt(renderer, view, big);
    } else {
        drawPage(renderer, view, big, small);
    }
    mdk_render_canvas_end(renderer);
}

// C# SavePrompt.Draw: the prompt on black, in FONTBIG.
void StatsScreen::drawPrompt(MdkRenderer renderer, const ScreenView& view,
                             const FontView& big) const {
    const flow::SavePrompt* prompt = prompt_ ? prompt_.get() : closedPrompt_.get();
    view.fillCanvas(renderer, Colour{0.0f, 0.0f, 0.0f, 1.0f});
    if (!prompt) {
        return;
    }
    struct Measure {
        const StatsScreen* screen;
        const FontView* font;
        static float width(void* context, const std::string& text, bool key) {
            const Measure* m = static_cast<const Measure*>(context);
            if (key) {
                const std::vector<uint8_t> bytes = m->screen->text(text.c_str());
                return (float)m->font->width(bytes.data(), (int32_t)bytes.size());
            }
            return (float)m->font->width(text.c_str());
        }
    };
    Measure measure = {this, &big};
    std::vector<flow::SavePrompt::Run> runs;
    prompt->layout(Measure::width, &measure, &runs);
    for (size_t r = 0; r < runs.size(); r++) {
        const std::vector<uint8_t> bytes =
            runs[r].key ? text(runs[r].text.c_str())
                        : std::vector<uint8_t>(runs[r].text.begin(), runs[r].text.end());
        view.text(renderer, big, bytes.data(), (int32_t)bytes.size(), runs[r].x, runs[r].y,
                  runs[r].scale, 1.0f);
    }
}

// C# Draw.
void StatsScreen::drawPage(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                           const FontView& small) const {
    if (image_.ready()) {
        view.image(renderer, image_.texture, image_.palette, image_.width, image_.height, 0.0f,
                   0.0f);
    }
    const bool shown = fadeIn_ >= 1.0f;
    if ((phase_ == Phase::Debriefing || phase_ == Phase::Briefing) && shown) {
        const int32_t end =
            current_ + 1 < (int32_t)texts_.size() ? current_ + 1 : (int32_t)texts_.size();
        for (int32_t i = 0; i < end; i++) {
            drawTyped(renderer, view, big, texts_[(size_t)i], textY_[(size_t)i],
                      i < current_ ? -1 : (int32_t)roundEven(count_));
        }
    } else if (phase_ == Phase::Score) {
        drawScore(renderer, view, big, small, shown);
    }
    // The fades: the page blended with white or black.
    const float fade = fadeOut_ < 0.0f ? 1.0f - fadeIn_ : fadeOut_;
    if (fade > 0.0f) {
        const float level = fadeOut_ < 0.0f && fadeWhite_ ? 1.0f : 0.0f;
        const Colour colour{level, level, level, minf(fade, 1.0f)};
        view.fill(renderer, 0.0f, 0.0f, ScreenView::kWidth, ScreenView::kHeight, colour);
    }
}

// C# DrawScore (the heads are draw()'s, under the canvas).
void StatsScreen::drawScore(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                            const FontView& small, bool shown) const {
    const std::vector<uint8_t> title = text("ST_SCR");
    view.text(renderer, big, title.data(), (int32_t)title.size(),
              300.0f - (float)(big.width(title.data(), (int32_t)title.size()) >> 1), kTitleY,
              1.0f, 1.0f);
    const std::vector<uint8_t> name = text("ST_DAMP");
    view.text(renderer, small, name.data(), (int32_t)name.size(),
              300.0f - (float)(small.width(name.data(), (int32_t)name.size()) >> 1), kNameY,
              1.0f, 1.0f);
    if (!shown) {
        return;
    }
    const formats::Palette palette = formats::Palette::fromRgb(systemRgb_, kPaletteSize);
    const uint8_t* rgba = palette.rgba();
    const Colour bar{(float)rgba[kBarColour * 4] / 255.0f, (float)rgba[kBarColour * 4 + 1] / 255.0f,
                     (float)rgba[kBarColour * 4 + 2] / 255.0f, 1.0f};
    for (int32_t i = 0; i < kRows; i++) {
        if (slides_[i] == kHidden) {
            continue;
        }
        const float t = slides_[i] < 0.0f ? 0.0f : slides_[i] > 1.0f ? 1.0f : slides_[i];
        // C# Vector4.Lerp, then MathF.Round of each component.
        float place[4];
        for (int32_t k = 0; k < 4; k++) {
            const float a = kRowTable[i].from[k];
            const float b = kRowTable[i].to[k];
            place[k] = roundEven(a + (b - a) * t);
        }
        const float scale = place[3] / kFullScale;
        const std::vector<uint8_t> label = text(kRowTable[i].label);
        const float labelWidth = (float)big.width(label.data(), (int32_t)label.size());
        view.text(renderer, big, label.data(), (int32_t)label.size(),
                  place[0] - roundEven(labelWidth * scale * 0.5f), place[1], scale, 1.0f);
        const float barX = place[0] - place[2] / 2.0f;
        const float barY = roundEven(place[1] + kBarRise * scale);
        const int32_t value = (int32_t)values_[i];
        if (fulls_[i] > 0 && value > 0) {
            view.fill(renderer, barX, barY, floorf(place[2] * (float)value / (float)fulls_[i]),
                      kBarHeight, bar);
        }
        const std::string valueString = valueText(i, value, fulls_[i]);
        view.text(renderer, small, (const uint8_t*)valueString.data(), (int32_t)valueString.size(),
                  barX - (float)small.width(valueString.c_str()) - kValueGap, barY + kValueDrop,
                  1.0f, 1.0f);
    }
    if (row_ >= kRows && gore_) {
        const std::vector<uint8_t> label = text("ST_HEAD");
        view.text(renderer, big, label.data(), (int32_t)label.size(),
                  kHeadLabelX - (float)(big.width(label.data(), (int32_t)label.size()) >> 1),
                  kHeadLabelY, 1.0f, 1.0f);
    }
}

} // namespace mdk
