#include "skydive_dump.h"

// THE FALL'S C++ TWIN of tests/scripts/fall_probe/Program.cs, line for line: the
// same setup (fall::fallSetup, FallScreen's), the same seeded Random, the same
// schedule grammar, the same per-step line, hash and full block, and the same
// carry at the end (FallSim::carry, fall::carryIntoLevel). Read the probe's
// header for what each part pins; this file only has to print the same bytes.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

#include "fall/fall_ground.h"
#include "fall/fall_layout.h"
#include "fall/fall_palette.h"
#include "fall/fall_sim.h"
#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "game/kurt.h"

namespace mdk {
namespace test {

namespace {

using fall::FallSim;
using formats::Vec3f;

uint32_t bitsOf(float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

std::string hexOf(float v) {
    char text[16];
    snprintf(text, sizeof(text), "%08X", (unsigned)bitsOf(v));
    return text;
}

std::string v3(const Vec3f& v) {
    return hexOf(v.x) + "," + hexOf(v.y) + "," + hexOf(v.z);
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

struct Hold {
    std::string key;
    int from;
    int to;
};

bool held(const std::vector<Hold>& plan, const char* key, int s) {
    for (size_t i = 0; i < plan.size(); i++) {
        if (plan[i].key == key && s >= plan[i].from && s < plan[i].to) {
            return true;
        }
    }
    return false;
}

bool parseSchedule(const std::string& schedule, std::vector<Hold>* plan, std::string* why) {
    if (schedule == "-") {
        return true;
    }
    size_t at = 0;
    while (at <= schedule.size()) {
        size_t comma = schedule.find(',', at);
        if (comma == std::string::npos) {
            comma = schedule.size();
        }
        const std::string part = schedule.substr(at, comma - at);
        const size_t colon = part.find(':');
        if (colon == std::string::npos) {
            *why = "bad schedule part " + part;
            return false;
        }
        Hold hold;
        hold.key = part.substr(0, colon);
        const std::string rest = part.substr(colon + 1);
        if (hold.key == "skip") {
            hold.from = atoi(rest.c_str());
            hold.to = hold.from + 1;
        } else {
            const size_t dash = rest.find('-');
            if (dash == std::string::npos) {
                *why = "bad range " + part;
                return false;
            }
            hold.from = atoi(rest.substr(0, dash).c_str());
            hold.to = atoi(rest.substr(dash + 1).c_str());
            if (hold.key != "left" && hold.key != "right" && hold.key != "up" && hold.key != "down" &&
                hold.key != "chase") {
                *why = "unknown key " + hold.key;
                return false;
            }
        }
        plan->push_back(hold);
        at = comma + 1;
    }
    return true;
}

void recordSound(void* context, const char* name) {
    static_cast<std::vector<std::string>*>(context)->push_back(name);
}

void recordMessage(void* context, const char* name) {
    static_cast<std::vector<std::string>*>(context)->push_back(std::string("msg:") + name);
}

std::string inventoryOf(const Kurt& kurt) {
    std::string text;
    if (kurt.slotCount() == 0) {
        text = "-";
    }
    for (int32_t i = 0; i < kurt.slotCount(); i++) {
        if (i != 0) {
            text += ";";
        }
        text += format("%d:%d", (int)kurt.slots()[i].item, (int)kurt.slots()[i].count);
    }
    text += format(" sel=%d ammo=%d:%d:%d:%d:%d/%d sgun=%d", (int)kurt.selected(), (int)kurt.ammo(1),
                   (int)kurt.ammo(2), (int)kurt.ammo(3), (int)kurt.ammo(4), (int)kurt.ammo(5),
                   (int)kurt.selectedAmmo(), (int)kurt.superChainGun());
    return text;
}

std::string joined(const std::vector<std::string>& names, const char* separator) {
    if (names.empty()) {
        return "-";
    }
    std::string text;
    for (size_t i = 0; i < names.size(); i++) {
        if (i != 0) {
            text += separator;
        }
        text += names[i];
    }
    return text;
}

// The probe's FNV-1a over 32-bit words, a byte at a time, low byte first.
struct Hash {
    uint32_t value = 2166136261u;
    void word(uint32_t v) {
        for (int i = 0; i < 4; i++) {
            value ^= (v >> (8 * i)) & 0xFFu;
            value *= 16777619u;
        }
    }
    void f(float v) { word(bitsOf(v)); }
    void vec(const Vec3f& v) {
        f(v.x);
        f(v.y);
        f(v.z);
    }
    void text(const std::string& s) {
        for (size_t i = 0; i < s.size(); i++) {
            word((uint32_t)(unsigned char)s[i]);
        }
        word(0xFFFFFFFFu);
    }
};

uint32_t fnv(const uint8_t* bytes, size_t count) {
    uint32_t v = 2166136261u;
    for (size_t i = 0; i < count; i++) {
        v ^= bytes[i];
        v *= 16777619u;
    }
    return v;
}

std::string rect(const fall::FallRect& r) {
    return hexOf(r.x) + "," + hexOf(r.y) + "," + hexOf(r.width) + "," + hexOf(r.height);
}

// The view's parts the probe's view mode builds: FALLPn's effects and the ground.
struct ViewParts {
    formats::TextureArchive mti;
    std::unique_ptr<fall::FallPalette> effects;
    fall::FallGround ground;
};

bool loadView(const formats::Bni& bni, int32_t index, const formats::MdkData& data, ViewParts* parts,
              std::string* why) {
    const int n = index + 1;
    std::vector<uint8_t> bytes;
    const std::string path = format("FALL3D/FALL3D_%d.MTI", n);
    if (!data.read(path, &bytes)) {
        *why = path + " did not load";
        return false;
    }
    parts->mti = formats::TextureArchive::parse(bytes.data(), (int32_t)bytes.size(), formats::TextureArchive::kFileBase,
                                                formats::TextureArchive::kHeader,
                                                formats::TextureArchive::Names::AsStored,
                                                formats::TextureArchive::Zero::SeeThrough);
    const formats::ArchiveIndex::Entry* entry = bni.index().find(format("FALLP%d", n));
    const formats::Texture* ground = parts->mti.findTexture(format("LEVEL%d", n));
    const formats::Texture* pod = parts->mti.findTexture(format("POD%d", n));
    if (entry == nullptr || ground == nullptr || pod == nullptr || !parts->ground.load(*ground, *pod, true)) {
        *why = "the fall's palette or ground did not load";
        return false;
    }
    const formats::Palette palette =
        formats::Palette::fromRgb(bni.bytes().data() + entry->offset, formats::Palette::kSize * 3);
    parts->effects.reset(new fall::FallPalette(palette.rgba()));
    return true;
}

// The probe's View(s): FallView.SetEffects, QueueGround, QueueModels, QueueRadar and
// DrawEffects' inputs, on a 600-wide view.
std::string viewLine(int s, const FallSim& sim, ViewParts* parts) {
    const float viewWidth = 600.0f;
    std::string line;
    const float brightness = sim.inIntro() ? 1.0f : sim.whiten();
    const fall::FallPalette::Zero zero = sim.time() < 1.0f ? fall::FallPalette::Whitened : fall::FallPalette::Kept;
    const bool changed = parts->effects->set(brightness, sim.red(), zero);
    line += format("v s=%d pal=%d:%d:%d", s, (int)fall::FallPalette::level(brightness),
                   (int)rintf(sim.red() * 255.0f), (int)zero);
    if (changed) {
        line += format(":%08X", (unsigned)fnv(parts->effects->rgba(), 256 * 4));
    }
    const Colour c0 = parts->effects->colour(0);
    line += format(" back=%s,%s,%s", hexOf(c0.r).c_str(), hexOf(c0.g).c_str(), hexOf(c0.b).c_str());
    const Vec3f camera = sim.cameraPosition();
    if (sim.inIntro()) {
        line += format(" intro=%s", hexOf(1.0f - (float)sim.introLeft() / (float)FallSim::kIntroTicks).c_str());
    } else {
        const float centreRow = fall::FallGround::centreRow(sim.time());
        const int32_t oldRow = parts->ground.trackRow();
        const bool track = parts->ground.updateTrack(centreRow);
        const int32_t row = parts->ground.trackRow();
        line += format(" row=%s track=%d", hexOf(centreRow).c_str(), (int)row);
        if (track) {
            uint32_t hash = 2166136261u;
            const int32_t end = oldRow + 24 < fall::FallGround::kSize ? oldRow + 24 : fall::FallGround::kSize;
            for (int32_t y = row; y < end; y++) {
                const uint32_t h = fnv(parts->ground.indices().data() + (size_t)y * fall::FallGround::kSize + 512 - 32, 64);
                hash = (hash ^ h) * 16777619u;
            }
            line += format(":%08X", (unsigned)hash);
        }
        fall::FallRect screen;
        fall::FallRect texels;
        fall::FallGround::visible(camera, centreRow, viewWidth, &screen, &texels);
        line += " vis=" + rect(screen) + "/" + rect(texels);
        float x = 0.0f;
        float y = 0.0f;
        float scale = 0.0f;
        fall::FallGround::crawler(camera, viewWidth, &x, &y, &scale);
        line += format(" crawler=%s,%s,%s:%d", hexOf(x).c_str(), hexOf(y).c_str(), hexOf(scale).c_str(),
                       (int)sim.crawlerFrame() % FallSim::kCrawlerFrames);
        const int32_t level = sim.wind() >> FallSim::kWindShift;
        line += format(" haze=%d", (int)sim.hazeFrame());
        for (int32_t b = 1; b <= fall::kHazeLevels; b++) {
            const float alpha = fall::hazeAlpha(level, b);
            if (alpha > 0.0f) {
                line += format(",%d:%s", (int)b, hexOf(alpha).c_str());
            }
        }
    }

    if (sim.kurtVisible()) {
        line += format(" kurt=%s:%d", sim.kurtPose() == FallSim::AnimHit ? "KURT_HIT" : "KURTANIM",
                       (int)sim.kurtFrame());
    }
    if (sim.hasBones()) {
        line += format(" bones=%d", (int)sim.bonesFrame());
    }
    for (size_t i = 0; i < sim.missiles().size(); i++) {
        const fall::FallMissile& missile = sim.missiles()[i];
        if (fall::nearCamera(missile.position, camera)) {
            line += " m=near";
            continue;
        }
        const Mat4 m = fall::orient(missile.velocity);
        line += format(" m=%s,%s,%s,%s,%s,%s,%s,%s,%s", hexOf(m.m[0]).c_str(), hexOf(m.m[1]).c_str(),
                       hexOf(m.m[2]).c_str(), hexOf(m.m[4]).c_str(), hexOf(m.m[5]).c_str(), hexOf(m.m[6]).c_str(),
                       hexOf(m.m[8]).c_str(), hexOf(m.m[9]).c_str(), hexOf(m.m[10]).c_str());
    }
    for (size_t i = 0; i < sim.pickups().size(); i++) {
        const FallSim::Pickup& pickup = sim.pickups()[i];
        if (fall::nearCamera(pickup.position, camera)) {
            line += " p=near";
            continue;
        }
        Vec3f chute = pickup.position;
        chute.z += fall::kChuteAbove;
        line += format(" p=%s:%s:%d", pickup.name.c_str(), hexOf(fall::degreesToRadians(pickup.yaw)).c_str(),
                       pickup.chute && !fall::nearCamera(chute, camera) ? 1 : 0);
    }
    for (size_t i = 0; i < sim.explosions().size(); i++) {
        const int32_t frame = (int32_t)sim.explosions()[i].ticks;
        line += format(" e=%d:%s", (int)frame, hexOf(fall::explosionScale(frame)).c_str());
    }
    if (sim.radarActive()) {
        Vec3f basePoint;
        basePoint.x = -4.0f * camera.x;
        basePoint.y = -4.0f * camera.y;
        line += " radar";
        for (int32_t k = 0; k < fall::kRadarRings; k++) {
            Vec3f centre;
            float radius = 0.0f;
            fall::radarRing(basePoint, sim.radarSpot(), sim.radarSpotRadius(), k, &centre, &radius);
            line += format("=%s:%s", v3(centre).c_str(), hexOf(radius).c_str());
        }
    }
    if (sim.inIntro() && sim.whiten() < 1.0f) {
        line += format(" white=%s", hexOf(1.0f - sim.whiten()).c_str());
    }
    if (sim.dark() < 1.0f) {
        line += format(" dark=%s", hexOf(1.0f - sim.dark()).c_str());
    }
    return line + "\n";
}

bool run(int level, int difficulty, int seed, int steps, const std::string& deltaText, const std::string& schedule,
         bool full, bool view, const formats::MdkData& data, std::string* out, std::string* why) {
    std::vector<Hold> plan;
    if (!parseSchedule(schedule, &plan, why)) {
        return false;
    }
    const float delta = strtof(deltaText.c_str(), nullptr);

    std::vector<uint8_t> bytes;
    formats::Bni bni;
    if (!data.read("FALL3D/FALL3D.BNI", &bytes) || !formats::Bni::parse(&bni, bytes.data(), (int32_t)bytes.size())) {
        *why = "FALL3D/FALL3D.BNI did not load";
        return false;
    }
    FallSim::Setup setup;
    if (!fall::fallSetup(bni, level, (Kurt::Difficulty)difficulty, &setup)) {
        *why = "the fall's setup did not load";
        return false;
    }

    FallSim sim(setup, seed);
    ViewParts parts;
    if (view && !loadView(bni, setup.index, data, &parts, why)) {
        return false;
    }
    std::vector<std::string> events;
    FallSim::Hooks hooks;
    hooks.context = &events;
    hooks.sound = recordSound;
    hooks.message = recordMessage;
    sim.setHooks(hooks);

    std::string& o = *out;
    o += format("fall level=%d index=%d difficulty=%d seed=%d steps=%d delta=%s schedule=%s\n", level,
                (int)setup.index, difficulty, seed, steps, hexOf(delta).c_str(), schedule.c_str());
    o += format("setup beam=%s missiles=%d spread=%s mgap=%d rgap=%d pickups=%s kurt=%s:%d hit=%s:%d bones=%s:%d\n",
                hexOf(sim.beamSpeed()).c_str(), (int)sim.missilesPerDetection(), hexOf(sim.spread()).c_str(),
                (int)sim.missileGap(), (int)sim.radarGap(),
                setup.pickups.empty() ? "" : joined(setup.pickups, ",").c_str(), hexOf(setup.kurt.speed).c_str(),
                (int)setup.kurt.frameCount, hexOf(setup.kurtHit.speed).c_str(), (int)setup.kurtHit.frameCount,
                hexOf(setup.bones.speed).c_str(), (int)setup.bones.frameCount);

    for (int s = 1; s <= steps && sim.state() == FallSim::Running; s++) {
        events.clear();
        FallSim::Input input;
        input.skip = held(plan, "skip", s);
        input.steerX = (held(plan, "right", s) ? 1 : 0) - (held(plan, "left", s) ? 1 : 0);
        input.steerY = (held(plan, "up", s) ? 1 : 0) - (held(plan, "down", s) ? 1 : 0);
        if (held(plan, "chase", s)) {
            const Vec3f& k = sim.kurtPosition();
            for (size_t i = 0; i < sim.pickups().size(); i++) {
                const FallSim::Pickup& pickup = sim.pickups()[i];
                if (!pickup.chute) {
                    continue;
                }
                const float dx = pickup.position.x - k.x;
                const float dy = pickup.position.y - k.y;
                input.steerX = fabsf(dx) < 1.0f ? 0 : (dx > 0.0f ? 1 : -1);
                input.steerY = fabsf(dy) < 1.0f ? 0 : (dy > 0.0f ? 1 : -1);
                break;
            }
        }

        sim.update(delta, input);
        if (view) {
            o += viewLine(s, sim, &parts);
            continue;
        }

        Hash h;
        h.word((uint32_t)sim.state());
        h.word((uint32_t)sim.ticks());
        h.word((uint32_t)sim.introLeft());
        h.f(sim.time());
        h.word(sim.dying() ? 1u : 0u);
        h.word((uint32_t)sim.health());
        h.vec(sim.kurtPosition());
        h.word(sim.kurtVisible() ? 1u : 0u);
        h.word((uint32_t)sim.kurtPose());
        h.f(sim.kurtFrame());
        h.vec(sim.cameraPosition());
        h.word(sim.hasBones() ? 1u : 0u);
        if (sim.hasBones()) {
            h.vec(sim.bonesPosition());
        }
        h.f(sim.bonesFrame());
        h.word((uint32_t)sim.wind());
        h.word((uint32_t)sim.hazeFrame());
        h.f(sim.crawlerFrame());
        h.f(sim.whiten());
        h.f(sim.dark());
        h.f(sim.red());
        h.word(sim.radarActive() ? 1u : 0u);
        h.word((uint32_t)sim.radar());
        h.vec(sim.radarSpot());
        h.f(sim.radarSpotRadius());
        h.word((uint32_t)sim.missiles().size());
        for (size_t i = 0; i < sim.missiles().size(); i++) {
            const fall::FallMissile& m = sim.missiles()[i];
            h.vec(m.position);
            h.vec(m.velocity);
            h.word((uint32_t)m.trailCount());
            for (int32_t t = 0; t < m.trailCount(); t++) {
                h.vec(m.trail()[t]);
            }
        }
        h.word((uint32_t)sim.pickups().size());
        for (size_t i = 0; i < sim.pickups().size(); i++) {
            const FallSim::Pickup& p = sim.pickups()[i];
            h.text(p.name);
            h.vec(p.position);
            h.vec(p.velocity);
            h.f(p.yaw);
            h.word(p.chute ? 1u : 0u);
            h.word((uint32_t)p.fallTicks);
            h.word((uint32_t)p.ticks);
        }
        h.word((uint32_t)sim.explosions().size());
        for (size_t i = 0; i < sim.explosions().size(); i++) {
            const FallSim::Explosion& e = sim.explosions()[i];
            h.vec(e.position);
            h.f(e.yaw);
            h.f(e.ticks);
        }
        h.word((uint32_t)sim.collected().size());
        for (size_t i = 0; i < sim.collected().size(); i++) {
            h.text(sim.collected()[i]);
        }
        const Kurt& inv = sim.inventory();
        h.word((uint32_t)inv.slotCount());
        for (int32_t i = 0; i < inv.slotCount(); i++) {
            h.word((uint32_t)inv.slots()[i].item);
            h.word((uint32_t)inv.slots()[i].count);
        }
        h.word((uint32_t)inv.selected());
        for (int32_t a = 1; a <= Kurt::kAmmoTypes; a++) {
            h.word((uint32_t)inv.ammo(a));
        }
        h.word((uint32_t)inv.selectedAmmo());
        h.word((uint32_t)inv.superChainGun());

        o += format("s=%d st=%d tk=%d hp=%d m=%d p=%d e=%d c=%d ev=%s h=%08X\n", s, (int)sim.state(),
                    (int)sim.ticks(), (int)sim.health(), (int)sim.missiles().size(), (int)sim.pickups().size(),
                    (int)sim.explosions().size(), (int)sim.collected().size(), joined(events, ";").c_str(),
                    (unsigned)h.value);
        if (full || s % 10 == 0 || !events.empty() || sim.state() != FallSim::Running) {
            const std::string bones =
                sim.hasBones() ? v3(sim.bonesPosition()) + ":" + hexOf(sim.bonesFrame()) : std::string("-");
            o += format("  intro=%d t=%s dying=%d kurt=%s vis=%d pose=%d:%s cam=%s bones=%s\n", (int)sim.introLeft(),
                        hexOf(sim.time()).c_str(), sim.dying() ? 1 : 0, v3(sim.kurtPosition()).c_str(),
                        sim.kurtVisible() ? 1 : 0, (int)sim.kurtPose(), hexOf(sim.kurtFrame()).c_str(),
                        v3(sim.cameraPosition()).c_str(), bones.c_str());
            o += format("  wind=%d haze=%d crawler=%s screen=%s,%s,%s radar=%d:%d:%s:%s wv=%s gv=%s\n",
                        (int)sim.wind(), (int)sim.hazeFrame(), hexOf(sim.crawlerFrame()).c_str(),
                        hexOf(sim.whiten()).c_str(), hexOf(sim.dark()).c_str(), hexOf(sim.red()).c_str(),
                        sim.radarActive() ? 1 : 0, (int)sim.radar(), v3(sim.radarSpot()).c_str(),
                        hexOf(sim.radarSpotRadius()).c_str(), hexOf(sim.windVolume()).c_str(),
                        hexOf(sim.grindVolume()).c_str());
            for (size_t i = 0; i < sim.missiles().size(); i++) {
                const fall::FallMissile& m = sim.missiles()[i];
                const std::string last =
                    m.trailCount() == 0 ? std::string("-") : v3(m.trail()[m.trailCount() - 1]);
                o += format("  missile %s v=%s trail=%d:%s\n", v3(m.position).c_str(), v3(m.velocity).c_str(),
                            (int)m.trailCount(), last.c_str());
            }
            for (size_t i = 0; i < sim.pickups().size(); i++) {
                const FallSim::Pickup& p = sim.pickups()[i];
                o += format("  pickup %s %s v=%s yaw=%s chute=%d ticks=%d/%d\n", p.name.c_str(),
                            v3(p.position).c_str(), v3(p.velocity).c_str(), hexOf(p.yaw).c_str(), p.chute ? 1 : 0,
                            (int)p.ticks, (int)p.fallTicks);
            }
            for (size_t i = 0; i < sim.explosions().size(); i++) {
                const FallSim::Explosion& e = sim.explosions()[i];
                o += format("  explosion %s yaw=%s ticks=%s\n", v3(e.position).c_str(), hexOf(e.yaw).c_str(),
                            hexOf(e.ticks).c_str());
            }
            o += format("  collected=%s inv=%s\n", joined(sim.collected(), ",").c_str(), inventoryOf(inv).c_str());
        }
    }

    // FallScreen.Next and Viewer.CarryFromFall, through the API the flow calls.
    o += format("end state=%d time=%s health=%d\n", (int)sim.state(), hexOf(sim.time()).c_str(), (int)sim.health());
    flow::FallCarry carry;
    if (sim.carry(&carry)) {
        Kurt levelKurt;
        levelKurt.setDifficulty((Kurt::Difficulty)difficulty);
        fall::carryIntoLevel(carry, &levelKurt);
        o += format("carry health=%d pickups=%s\n", (int)carry.health, joined(carry.pickups, ",").c_str());
        o += format("level health=%d inv=%s\n", (int)levelKurt.health(), inventoryOf(levelKurt).c_str());
    } else {
        o += "carry none\n";
    }
    return true;
}

}  // namespace

bool skydiveDump(int level, int difficulty, int seed, int steps, const std::string& deltaText,
                 const std::string& schedule, bool full, const formats::MdkData& data, std::string* out,
                 std::string* why) {
    return run(level, difficulty, seed, steps, deltaText, schedule, full, false, data, out, why);
}

bool fallViewDump(int level, int difficulty, int seed, int steps, const std::string& deltaText,
                  const std::string& schedule, const formats::MdkData& data, std::string* out, std::string* why) {
    return run(level, difficulty, seed, steps, deltaText, schedule, false, true, data, out, why);
}

}  // namespace test
}  // namespace mdk
