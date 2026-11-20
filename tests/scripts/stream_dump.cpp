// ============================================================================
// stream_dump.cpp — see stream_dump.h. Every line here has a twin in
// tests/scripts/stream_probe/Program.cs; keep the two in step.
// ============================================================================

#include "stream_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

#include "formats/bni.h"
#include "formats/model_animation.h"
#include "stream/stream_flight.h"

namespace mdk {
namespace test {

namespace {

using stream::Clip;
using stream::Flight;
using stream::StreamTube;
using stream::Thing;
using stream::TubeBasis;
using stream::Vec3;

uint32_t bitsOf(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

uint64_t bitsOf(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

std::string H(float v)
{
    char s[16];
    snprintf(s, sizeof(s), "%08X", (unsigned)bitsOf(v));
    return s;
}

std::string HD(double v)
{
    char s[24];
    snprintf(s, sizeof(s), "%016llX", (unsigned long long)bitsOf(v));
    return s;
}

std::string V(Vec3 v) { return H(v.x) + "," + H(v.y) + "," + H(v.z); }

std::string I(long long v) { return std::to_string(v); }

std::string X8(uint32_t v)
{
    char s[16];
    snprintf(s, sizeof(s), "%08X", (unsigned)v);
    return s;
}

std::string X2(int v)
{
    char s[8];
    snprintf(s, sizeof(s), "%02X", v);
    return s;
}

// FNV-1a over 32-bit words, little-endian bytes (the probe's Fnv).
const uint32_t kFnvStart = 2166136261u;
uint32_t fnv(uint32_t h, uint32_t word)
{
    for (int b = 0; b < 4; b++) {
        h ^= (word >> (8 * b)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}
uint32_t fnvF(uint32_t h, float v) { return fnv(h, bitsOf(v)); }
uint32_t fnvV(uint32_t h, Vec3 v) { return fnvF(fnvF(fnvF(h, v.x), v.y), v.z); }

const char* kindName(StreamTube::Kind k) { return k == StreamTube::Gunter ? "Gunter" : "Normal"; }
const char* spawnName(StreamTube::SpawnKind k) { return k == StreamTube::Planet ? "Planet" : "Light"; }
const char* eventName(stream::FlightEvent e)
{
    switch (e) {
        case stream::BonesCame:
            return "BonesCame";
        case stream::KurtDied:
            return "KurtDied";
        case stream::WallHit:
            return "WallHit";
        case stream::BonusTaken:
            return "BonusTaken";
        default:
            return "Ended";
    }
}

std::string thingOf(bool present, const Thing& t)
{
    if (!present) {
        return "-";
    }
    return H(t.t) + "," + H(t.x) + "," + H(t.z) + "," + H(t.speed) + "," + H(t.yaw) + "," + H(t.frame);
}

// The probe's M: M11 M12 M13 M21 M22 M23 M31 M32 M33 M41 M42 M43.
const int kMatrixCells[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
std::string M(const Mat4& m)
{
    std::string s;
    for (int i = 0; i < 12; i++) {
        if (i) {
            s += ",";
        }
        s += H(m.m[kMatrixCells[i]]);
    }
    return s;
}

struct Key {
    std::string name;
    int from = 0;
    int to = 0;
};

bool parseSchedule(const std::string& text, std::vector<Key>* keys, std::string* why)
{
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        if (item.empty()) {
            continue;
        }
        const size_t colon = item.find(':');
        if (colon == std::string::npos) {
            *why = "bad schedule item " + item;
            return false;
        }
        Key k;
        k.name = item.substr(0, colon);
        const std::string range = item.substr(colon + 1);
        const size_t dash = range.find('-');
        k.from = atoi(range.substr(0, dash).c_str());
        k.to = dash == std::string::npos ? k.from : atoi(range.substr(dash + 1).c_str());
        if (k.name != "right" && k.name != "left" && k.name != "up" && k.name != "down" && k.name != "stop" &&
            k.name != "auto") {
            *why = "unknown schedule key " + k.name;
            return false;
        }
        keys->push_back(k);
    }
    return true;
}

bool held(const std::vector<Key>& keys, const char* name, int step)
{
    for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i].name == name && step >= keys[i].from && step <= keys[i].to) {
            return true;
        }
    }
    return false;
}

bool clipOf(const formats::Bni& bni, const char* name, Clip* out, std::string* why)
{
    const formats::ArchiveIndex::Entry* entry = bni.index().find(name);
    formats::ModelAnimation a;
    if (entry == nullptr ||
        !formats::ModelAnimation::parse(&a, name, bni.bytes().data(), (int32_t)bni.bytes().size(), entry->offset)) {
        *why = std::string("STREAM/STREAM.BNI has no animation ") + name;
        return false;
    }
    out->frameCount = a.frameCount;
    out->speed = a.speed;
    return true;
}

struct Printer {
    std::string* out;
    const StreamTube* tube;
    const Flight* flight;
    const stream::WatcomRandom* random;
    int32_t seen = 0;
    std::vector<stream::TubeVertex> triangles;

    void line(const std::string& s)
    {
        *out += s;
        *out += "\n";
    }

    void ring(int32_t n)
    {
        const TubeBasis& b = tube->basis(n);
        uint32_t points = kFnvStart;
        for (int j = 0; j < StreamTube::kPoints; j++) {
            points = fnvV(points, tube->point(n, j));
        }
        std::string s = "R n=" + I(n) + " o=" + V(tube->origin(n)) + " b=" + V(b.x) + ";" + V(b.y) + ";" + V(b.z) +
                        " pts=" + X8(points);
        if (n > tube->tail()) {
            s += " col=";
            for (int j = 0; j < StreamTube::kPoints; j++) {
                s += X2(tube->colour(n - 1, j));
            }
            uint32_t planes = kFnvStart;
            for (int i = 0; i < StreamTube::kPlanesPerSegment; i++) {
                planes = fnvF(fnvV(planes, tube->planeNormal(n - 1, i)), tube->planeDistance(n - 1, i));
            }
            s += " pl=" + X8(planes);
        }
        line(s);
    }

    void walk()
    {
        line("W head=" + I(tube->head()) + " ang=" + V(tube->angles()) + " rad=" + HD(tube->radiusExact()) +
             " shade=" + HD(tube->shade()) + " " + I(tube->shadeFrom()) + " " + I(tube->shadeTo()));
    }

    void spawns()
    {
        const std::vector<stream::Sprite>& sprites = flight->sprites();
        int32_t fresh = flight->spawnedTotal() - seen;
        seen = flight->spawnedTotal();
        if (fresh > (int32_t)sprites.size()) {
            fresh = (int32_t)sprites.size();
        }
        for (size_t i = sprites.size() - fresh; i < sprites.size(); i++) {
            const StreamTube::Spawn& p = sprites[i].spawn;
            line(std::string("P ") + spawnName(p.kind) + " t=" + H(p.t) + " x=" + H(p.x) + " z=" + H(p.z) +
                 " size=" + H(p.size) + " speed=" + H(p.speed));
        }
    }

    void view(int step)
    {
        triangles.clear();
        for (int32_t n = tube->firstDrawn(); n >= tube->tail(); n--) {
            tube->addTriangles(n, flight->eye(), &triangles);
        }
        uint32_t tri = kFnvStart;
        for (size_t i = 0; i < triangles.size(); i++) {
            const stream::TubeVertex& v = triangles[i];
            const stream::Rgba8 c = tube->rgba(v);
            tri = fnv(fnvV(tri, v.position), (uint32_t)(v.colour | (v.level << 8) | (c.r << 16)));
            tri = fnv(tri, (uint32_t)(c.g | (c.b << 8) | (c.a << 16)));
        }
        uint32_t spr = kFnvStart;
        const std::vector<stream::Sprite>& sprites = flight->sprites();
        for (size_t i = 0; i < sprites.size(); i++) {
            const Mat4 m = flight->transform(sprites[i].thing);
            for (int c = 0; c < 12; c++) {
                spr = fnvF(spr, m.m[kMatrixCells[c]]);
            }
            spr = fnv(spr, (uint32_t)flight->otherFrame(sprites[i].thing));
        }
        const std::string other = flight->hasOther()
                                      ? M(flight->transform(flight->other())) + " of=" + I(flight->otherFrame(flight->other()))
                                      : std::string("-");
        line("V s=" + I(step) + " kurt=" + M(flight->kurtTransform()) + " kf=" + I(flight->kurtFrame()) +
             " bf=" + I(flight->bonesFrame()) + " other=" + other + " verts=" + I((long long)triangles.size()) +
             " tri=" + X8(tri) + " spr=" + X8(spr));
    }

    void step(int s)
    {
        const Thing& k = flight->kurt();
        const stream::Fade f = flight->fade();
        const stream::Vec2 bg = flight->backgroundOffset();
        std::string ev;
        for (int i = 0; i < flight->eventCount(); i++) {
            ev += (i ? "+" : "") + std::string(eventName(flight->event(i)));
        }
        std::string snd;
        for (int i = 0; i < flight->soundCount(); i++) {
            snd += (i ? "+" : "") + std::string(flight->sound(i));
        }
        std::string l = "s=" + I(s) + " seg=" + I(tube->tail()) + "/" + I(tube->head()) + " rng=" +
                        X8(random->state()) + " k=" + thingOf(true, k) + " p=" + H(flight->pitch()) + " at=" +
                        V(flight->kurtPosition()) + " dir=" + V(flight->direction()) + " hp=" + I(flight->health()) +
                        " blink=" + I(flight->blink()) + " tf=" + H(flight->tickFraction()) + " fade=" +
                        H(flight->fadeLevel()) + (flight->ending() ? "!" : "") + " f=" + H(f.whiten) + "," +
                        H(f.dark) + "," + H(f.red) + " time=" + H(flight->time()) + " eye=" + V(flight->eye()) +
                        " fwd=" + V(flight->forward()) + " right=" + V(flight->right()) + " up=" + V(flight->up()) +
                        " bg=" + H(bg.x) + "," + H(bg.y) + " other=" + thingOf(flight->hasOther(), flight->other()) +
                        " bones=" + thingOf(flight->hasBones(), flight->bones()) +
                        " spr=" + I((long long)flight->sprites().size()) + " ev=" + (ev.empty() ? "-" : ev) +
                        " snd=" + (snd.empty() ? "-" : snd);
        if (flight->ended()) {
            l += flight->outcome() == stream::Died ? " END Died" : " END Survived";
        }
        line(l);
    }
};

int32_t clampKey(int32_t v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }

float dtOf(const std::string& mode, int step)
{
    if (mode == "30") {
        return 1.0f / 30.0f;
    }
    if (mode == "60") {
        return 1.0f / 60.0f;
    }
    if (mode == "fast") {
        static const float kFast[4] = {0.1f, 0.07f, 0.15f, 1.0f / 30.0f};
        return kFast[(step - 1) % 4];
    }
    static const float kMix[5] = {1.0f / 60.0f, 1.0f / 30.0f, 0.05f, 0.15f, 1.0f / 45.0f};
    return kMix[(step - 1) % 5];
}

} // namespace

bool streamRandDump(const std::string& seeds, int count, std::string* out, std::string* why)
{
    out->clear();
    size_t at = 0;
    while (at <= seeds.size()) {
        size_t end = seeds.find(',', at);
        if (end == std::string::npos) {
            end = seeds.size();
        }
        const std::string text = seeds.substr(at, end - at);
        at = end + 1;
        if (text.empty()) {
            *why = "empty seed";
            return false;
        }
        const uint32_t seed = (uint32_t)strtoul(text.c_str(), nullptr, 10);
        stream::WatcomRandom random(seed);
        *out += "seed=" + std::to_string(seed) + " count=" + std::to_string(count) + "\n";
        std::string row;
        for (int i = 0; i < count; i++) {
            char v[8];
            snprintf(v, sizeof(v), "%04X", (unsigned)random.next());
            row += (i % 16 == 0 ? "" : " ");
            row += v;
            if (i % 16 == 15 || i == count - 1) {
                *out += row + "\n";
                row.clear();
            }
        }
    }
    return true;
}

bool streamDump(const formats::MdkData& data, int level, int difficulty, unsigned seed, int health, int maxSteps,
                const std::string& dt, const std::string& schedule, std::string* out, std::string* why)
{
    out->clear();
    if (difficulty < 0 || difficulty > 2) {
        *why = "difficulty is 0-2";
        return false;
    }
    if (dt != "30" && dt != "60" && dt != "mix" && dt != "fast") {
        *why = "dt is 30, 60, mix or fast";
        return false;
    }
    std::vector<Key> keys;
    if (!parseSchedule(schedule, &keys, why)) {
        return false;
    }

    std::vector<uint8_t> bytes;
    formats::Bni bni;
    if (!data.read("STREAM/STREAM.BNI", &bytes) || !formats::Bni::parse(&bni, std::move(bytes))) {
        *why = "STREAM/STREAM.BNI did not load";
        return false;
    }

    const int32_t index = stream::streamIndexOf(level);
    const StreamTube::Kind kind = stream::streamKindOf(index);
    Clip kurt;
    Clip rescue;
    Clip other;
    if (!clipOf(bni, "KURTANIM", &kurt, why) || !clipOf(bni, "BONESANIM", &rescue, why) ||
        !clipOf(bni, kind == StreamTube::Gunter ? "GUNTANIM" : "SWHANM", &other, why)) {
        return false;
    }

    // The run is ~25 KB of fixed arrays: on the heap, not the stack.
    std::unique_ptr<stream::StreamRun> run(
        new stream::StreamRun(level, (Kurt::Difficulty)difficulty, seed, health, kurt, rescue, other));
    StreamTube& tube = run->tube;
    Flight& flight = run->flight;

    Printer p;
    p.out = out;
    p.tube = &tube;
    p.flight = &flight;
    p.random = &run->random;

    p.line("stream level=" + I(level) + " index=" + I(index) + " kind=" + kindName(kind) + " difficulty=" +
           I(difficulty) + " seed=" + std::to_string(seed) + " health=" + I(health) + " steps=" + I(maxSteps) +
           " dt=" + dt + " schedule=" + (schedule.empty() ? std::string("-") : schedule));
    p.line("clips kurt=" + I(kurt.frameCount) + "/" + H(kurt.speed) + " rescue=" + I(rescue.frameCount) + "/" +
           H(rescue.speed) + " other=" + I(other.frameCount) + "/" + H(other.speed));
    std::string ramp;
    for (int i = 0; i < StreamTube::kRampSize; i++) {
        ramp += X2(tube.ramp()[i].r) + X2(tube.ramp()[i].g) + X2(tube.ramp()[i].b);
    }
    p.line("tube turn=" + H(tube.maxTurn()) + " radius=" + H(tube.minRadius()) + "-" + H(tube.maxRadius()) +
           " lights=" + I(flight.lightCount()) + " ramp=" + ramp);
    for (int32_t n = tube.tail(); n < tube.head(); n++) {
        p.ring(n);
    }
    p.walk();
    p.spawns();
    p.view(0);

    for (int step = 1; step <= maxSteps; step++) {
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i].name == "stop" && keys[i].from == step) {
                flight.stop();
                break;
            }
        }

        stream::Steering steering;
        steering.right = (held(keys, "right", step) ? 1 : 0) - (held(keys, "left", step) ? 1 : 0);
        steering.up = (held(keys, "up", step) ? 1 : 0) - (held(keys, "down", step) ? 1 : 0);
        if (held(keys, "auto", step)) {
            // The probe's pilot: right moves Kurt to +x and up to +z (Flight::steer),
            // so steer against the offset, added to any key held and clamped.
            const Thing& k = flight.kurt();
            steering.right = clampKey(steering.right + (k.x > 0.5f ? -1 : (k.x < -0.5f ? 1 : 0)));
            steering.up = clampKey(steering.up + (k.z > 0.5f ? -1 : (k.z < -0.5f ? 1 : 0)));
        }
        const int32_t head = tube.head();
        const int32_t tail = tube.tail();
        flight.update(dtOf(dt, step), steering);

        for (int32_t n = head; n < tube.head(); n++) {
            p.ring(n);
        }
        if (tube.tail() != tail) {
            p.walk();
        }
        p.spawns();
        p.step(step);
        if (step % 15 == 0 || flight.ended()) {
            p.view(step);
        }
        if (flight.ended()) {
            break;
        }
    }
    return true;
}

} // namespace test
} // namespace mdk
