// ============================================================================
// tick1.cpp — the level tick-1 golden-state check.
//
// The scripts' first end-to-end milestone: for each level, load it, place Kurt
// at the level's start, run one tick, and dump the object table. The arena
// controller (and, on some levels, the DTI's alien/static records) spawns the
// level's objects during that tick, and each runs its type's init script inside
// Spawn. The expected tables are the C# oracle's, byte for byte, in golden/.
//
// The oracle is /tmp/kilo/oracle (headless Mdk.Game). Its driver sets Option=0
// and seed=1; this matches it exactly.
//
// WHY TICK 1 ONLY. A spawned object runs its init script inside Spawn and gets
// no motion update on the first tick beyond path-following, so a non-path object
// stays at its spawn position. Gravity/footing/movement commands are later
// motion slices; the multi-tick golden states land with them.
//
// PATH-RIDING OBJECTS. Level 8's first object (XBSHIP) rides a path: follow_path
// puts it at the path start and Motion's path-following step (motionPath) advances
// it one frame and turns it along the path. That is the one motion that moves an
// object at tick 1 (gravity objects are footing-held at their spawn), so it is in
// scope here; the rest of ObjectMotion is not.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_support.h"
#include "audio/sound_bank.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "game/level_data.h"
#include "game/object_view.h"
#include "render/arena_scene.h"
#include "render/material_resolver.h"
#include "scripts/script_runtime.h"
#include "util/matrix.h"
#include "util/math_lut.h"

using namespace mdk;
using namespace mdk::formats;

namespace {

MdkData locateData() {
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const FileSystem* system = fileSystem();
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    const FileSystem* system = fileSystem();
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

// A whole file's text, read with plain stdio (a test fixture, not game data).
bool readText(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out->clear();
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out->append(buf, n);
    }
    fclose(f);
    return true;
}

// The object table in the oracle's format.
std::string dumpTable(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 currentArena=%s objects=%d\n", level,
             ticks, runtime.currentArena().c_str(), runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "%03d type=%s arena=%s pos=(%.4f,%.4f,%.4f) yaw=%.4f hp=%d flags=0x%08X dead=%d "
                 "move=%d/%d connects=%s path=%d/%.4f/%.4f/%d\n",
                 index, o.type_name.c_str(), o.arena.c_str(), o.position.x, o.position.y,
                 o.position.z, o.yaw, o.health, (unsigned)o.flags, o.dead ? 1 : 0, o.move_command,
                 o.move_parameter, o.connects.c_str(), o.path, o.path_time, o.path_speed,
                 o.path_stop);
        out += line;
    }
    return out;
}

// The identity line both object dumps share. `roll` is left out on purpose:
// it is a transform input, not a pose input, and the port does not write it
// yet (ObjectMotion.Bank is not ported), so including it would make the pose
// golden fail for a reason that has nothing to do with the pose.
std::string objectIdentity(int index, const scripts::MdkObject& o) {
    char line[512];
    snprintf(line, sizeof(line),
             "OBJ %03d type=%s arena=%s model=%s anim=%s frame=%d hidden=%d visible=%d "
             "dead=%d scale=%.4f pos=%.4f,%.4f,%.4f yaw=%.4f pitch=%.4f effect=%d texframe=%d\n",
             index, o.type_name.c_str(), o.arena.c_str(),
             o.model ? o.model->name.c_str() : "-",
             o.animation ? o.animation->name.c_str() : "-", o.animation_frame, o.hidden_parts,
             o.visible ? 1 : 0, o.dead ? 1 : 0, o.scale, o.position.x, o.position.y, o.position.z,
             o.yaw, o.pitch, o.effect_frames, o.texture_frame);
    return std::string(line);
}

// The object pass's pose: the C#'s PoseParts, per object in the C#'s draw order
// (Viewer.DrawObjects walks ScriptRuntime.Objects). The pose is the rest
// vertices or the baked animation frame, and it is what the draw stage copies
// into the renderer's vertex stream, so a wrong bit here is a moved vertex.
std::string dumpObjectPoses(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        out += objectIdentity(index, o);

        // Per part: how many vertices, their sum in order (which pins the
        // order as well as the values), and the first and last.
        const std::vector<std::vector<Vec3f> >& pose = o.pose_parts();
        snprintf(line, sizeof(line), "  PARTS %d\n", (int)pose.size());
        out += line;
        for (size_t p = 0; p < pose.size(); p++) {
            const std::vector<Vec3f>& v = pose[p];
            float sx = 0.0f, sy = 0.0f, sz = 0.0f;
            for (size_t i = 0; i < v.size(); i++) {
                sx += v[i].x;
                sy += v[i].y;
                sz += v[i].z;
            }
            char first[64];
            char last[64];
            if (v.empty()) {
                snprintf(first, sizeof(first), "-");
                snprintf(last, sizeof(last), "-");
            } else {
                snprintf(first, sizeof(first), "%.4f,%.4f,%.4f", v[0].x, v[0].y, v[0].z);
                snprintf(last, sizeof(last), "%.4f,%.4f,%.4f", v[v.size() - 1].x, v[v.size() - 1].y,
                         v[v.size() - 1].z);
            }
            snprintf(line, sizeof(line), "  P %d n=%d sum=%.4f,%.4f,%.4f first=%s last=%s\n",
                     (int)p, (int)v.size(), sx, sy, sz, first, last);
            out += line;
        }
    }
    return out;
}

// The object pass's place: the C#'s Transform (scale * orientation *
// translation), which is where the yaw/pitch/roll order and a rolling object's
// captured basis show up.
std::string dumpObjectTransforms(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        out += objectIdentity(index, o);
        const Mat4 t = o.transform();
        snprintf(line, sizeof(line),
                 "  T %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                 "%.6f %.6f\n",
                 t.m[0], t.m[1], t.m[2], t.m[3], t.m[4], t.m[5], t.m[6], t.m[7], t.m[8], t.m[9],
                 t.m[10], t.m[11], t.m[12], t.m[13], t.m[14], t.m[15]);
        out += line;
    }
    return out;
}

// Comma-join a list of ints, as the oracle's string.Join(",", list) does (empty when
// the list is empty).
std::string joinInts(const std::vector<int32_t>& v) {
    std::string s;
    char n[16];
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        snprintf(n, sizeof(n), "%d", (int)v[i]);
        s += n;
    }
    return s;
}

// The first line where two dumps differ, as one readable line. A whole object
// dump is hundreds of lines and only the first difference is a cause.
std::string firstDifference(const std::string& got, const std::string& expected) {
    size_t i = 0;
    while (i < got.size() && i < expected.size() && got[i] == expected[i]) {
        i++;
    }
    if (i == got.size() && i == expected.size()) {
        return "identical";
    }
    size_t lineStart = got.rfind('\n', i);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    size_t lineEnd = got.find('\n', i);
    if (lineEnd == std::string::npos) {
        lineEnd = got.size();
    }
    size_t expectedEnd = expected.find('\n', i);
    if (expectedEnd == std::string::npos) {
        expectedEnd = expected.size();
    }
    // The expected side can be SHORTER than the line being reported (got has text
    // the golden does not), and its line can start past the end of the golden. A
    // substr past size() throws, which would replace the report with a crash.
    size_t expectedStart = lineStart > expected.size() ? expected.size() : lineStart;
    return "at byte " + std::to_string((int)i) + "\n  got      " + got.substr(lineStart, lineEnd - lineStart) +
           "\n  expected " + expected.substr(expectedStart, expectedEnd - expectedStart);
}

// The pass and shading names, spelled as the C# enum names print them.
const char* passName(Pass pass) {
    switch (pass) {
        case Pass::Solid: return "Solid";
        case Pass::DoubleSided: return "DoubleSided";
        case Pass::Mirror: return "Mirror";
        case Pass::Blended: return "Blended";
        case Pass::OnTop: return "OnTop";
        case Pass::Overlay: return "Overlay";
    }
    return "?";
}

const char* shadingName(Shading shading) {
    switch (shading) {
        case Shading::Original: return "Original";
        case Shading::Lit: return "Lit";
        case Shading::Sprite: return "Sprite";
    }
    return "?";
}

// The C#'s Viewer.ObjectLook: an arena's palette and archives, the level's own
// outside the arenas. The C++ LevelData caches the palette per arena, exactly as
// the C# does, so asking twice hands back the same palette (which is what makes
// the layout cache key the same across objects of one arena).
ObjectLook lookOf(LevelData& level, const std::string& arenaName) {
    ObjectLook look;
    const formats::Arena* arena = level.arenaNamed(arenaName);
    if (arena != nullptr) {
        look.palette = &level.paletteOf(arena);
        look.archives = level.archivesOf(arena);
    } else {
        look.palette = &level.dti().palette();
        look.archives.push_back(&level.levelTextures());
    }
    return look;
}

// The object pass's layout: what ObjectView.Build makes once per (model,
// palette), and what Arrange/Visible make of it per draw. The objects are walked
// in the draw stage's order (Viewer.DrawObjects: skip invisible, skip dead, skip
// no model) and each layout is printed the first time its pair is laid out,
// which is the order the C#'s layout cache fills.
//
// This is the golden that pins the layout stage at all: the surface grouping, the
// corner and first-triangle tables, the UV scale, the arrangement under hidden
// masks and the per-batch corner count. The pose goldens cannot see any of it.
std::string dumpObjectLayout(int level, int ticks, LevelData& levelData,
                             scripts::ScriptRuntime& runtime) {
    MaterialResolver resolver(&levelData.dti());
    ObjectView view(resolver);
    std::vector<std::pair<const formats::Model*, const formats::Palette*> > seen;

    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;
            continue;
        }
        if (!o.visible || o.dead || o.model == nullptr) {
            continue;
        }
        ObjectLook look = lookOf(levelData, o.arena);
        std::pair<const formats::Model*, const formats::Palette*> key(o.model, look.palette);
        bool done = false;
        for (size_t i = 0; i < seen.size(); i++) {
            done = done || seen[i] == key;
        }
        if (done) {
            continue;
        }
        seen.push_back(key);

        ObjectLayout* layout = view.layoutOf(o.model, look);
        if (layout == nullptr) {
            snprintf(line, sizeof(line), "LAYOUT obj=%d model=%s arena=%s batches=0\n", index,
                     o.model->name.c_str(), o.arena.c_str());
            out += line;
            continue;
        }
        snprintf(line, sizeof(line), "LAYOUT obj=%d model=%s arena=%s batches=%d\n", index,
                 o.model->name.c_str(), o.arena.c_str(), (int)layout->batches.size());
        out += line;

        for (size_t b = 0; b < layout->batches.size(); b++) {
            const ObjectBatch& batch = layout->batches[b];
            const Surface& s = batch.surface;
            snprintf(line, sizeof(line),
                     "  B%d tex=%s colour=%.4f,%.4f,%.4f,%.4f frames=%d pass=%s shading=%s "
                     "rowshift=%.4f corners=%d\n",
                     (int)b, s.texture ? s.texture->name().c_str() : "flat", s.colour.r, s.colour.g,
                     s.colour.b, s.colour.a, (int)s.frameCount, passName(s.pass), shadingName(s.shading),
                     s.rowShift, (int)batch.parts.size());
            out += line;
            out += "    parts=" + joinInts(batch.parts) + "\n";
            out += "    indices=" + joinInts(batch.indices) + "\n";
            for (size_t p = 0; p < batch.partCorners.size(); p++) {
                if (batch.partCorners[p] == 0) {
                    continue;
                }
                float su = 0.0f, sv = 0.0f;
                int n = 0;
                char first[64], last[64];
                snprintf(first, sizeof(first), "-");
                snprintf(last, sizeof(last), "-");
                for (size_t c = 0; c < batch.parts.size(); c++) {
                    if (batch.parts[c] != (int32_t)p) {
                        continue;
                    }
                    su += batch.uvs[c].x;
                    sv += batch.uvs[c].y;
                    char text[64];
                    snprintf(text, sizeof(text), "%.4f,%.4f", batch.uvs[c].x, batch.uvs[c].y);
                    if (n == 0) {
                        snprintf(first, sizeof(first), "%s", text);
                    }
                    snprintf(last, sizeof(last), "%s", text);
                    n++;
                }
                snprintf(line, sizeof(line), "    uv p=%d n=%d sum=%.4f,%.4f first=%s last=%s\n",
                         (int)p, n, su, sv, first, last);
                out += line;
            }
        }

        const int32_t masks[] = {0, 1, 3, o.hidden_parts, -1};
        for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
            int32_t hidden = masks[m];
            int32_t count = arrangeObjectLayout(layout, hidden);
            std::vector<int32_t> order, visible;
            for (int32_t i = 0; i < count; i++) {
                order.push_back(layout->order[i]);
                visible.push_back(objectBatchCorners(layout->batches[layout->order[i]], hidden));
            }
            snprintf(line, sizeof(line), "  A hidden=%d count=%d order=%s visible=%s\n",
                     (int)hidden, (int)count, joinInts(order).c_str(), joinInts(visible).c_str());
            out += line;
        }
    }
    return out;
}

// The rolling basis, driven directly (the C#'s ObjectMotion.Roll through the
// reflection the probe used). No level the tests tick has a rolling object in a
// live state, so the motion is synthesized here: the basis after each move, and
// the transform that draws with it -- which is the rolling branch of
// MdkObject::transform(), the one that reads the basis instead of the yaw.
//
// The basis is printed as raw bits (the transform golden's %.6f would not see a
// last-bit difference) and the transform at %.6f, because the transform's
// translation is the object's position and its rotation is the basis scaled.
// The draw stage's digest of one batch's corners: FNV-1a over the raw bits of
// the five floats of each vertex, in stream order. The sums and the end vertices
// below say what a batch is; this says it is EXACTLY the oracle's, to the bit,
// including the order the corners were written in (a rolling hash is not
// order-independent the way a plain XOR would be).
uint32_t hashCorners(const std::vector<Vertex>& vertices, int32_t first, int32_t corners) {
    uint32_t hash = 2166136261u;
    for (int32_t i = 0; i < corners; i++) {
        const Vertex& v = vertices[(size_t)(first + i)];
        const float values[5] = {v.pos.x, v.pos.y, v.pos.z, v.uv.x, v.uv.y};
        for (int k = 0; k < 5; k++) {
            uint32_t bits = 0;
            memcpy(&bits, &values[k], sizeof(bits));
            hash ^= bits;
            hash *= 16777619u;
        }
    }
    return hash;
}

// The object pass's DRAW stage (ObjectView.Draw): the batches arrange orders,
// the posed corners each writes, and where in the frame's stream it is drawn
// from. This is the golden that pins any of it: the layout golden stops at the
// corner table, and the pose golden stops at the posed vertices -- what neither
// can see is which corner goes where, in what order, with what surface and what
// animated-texture frame, and which transform the draw carries.
//
// The objects are walked exactly as Viewer.DrawObjects walks them (skip the
// invisible ones; the dead, the model-less and the layout-less are refused inside
// the draw stage itself), and the stream is the one the whole frame shares, so
// the `first` offsets are the oracle's stream offsets.
std::string dumpObjectDraw(int level, int ticks, LevelData& levelData,
                           scripts::ScriptRuntime& runtime) {
    MaterialResolver resolver(&levelData.dti());
    ObjectView view(resolver);
    ObjectStream stream;
    stream.reset();   // the frame's start, as the GPU half will do

    std::string out;
    char line[640];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 objects=%d\n", level, ticks,
             runtime.objectCount());
    out += line;

    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;
            continue;
        }
        if (!o.visible) {
            continue;
        }

        ObjectLook look = lookOf(levelData, o.arena);
        const Mat4 transform = o.transform();
        const size_t before = stream.draws.size();
        if (!view.addObject(o, look, transform, &stream)) {
            continue;
        }

        ObjectLayout* layout = view.layoutOf(o.model, look);
        const int32_t count = arrangeObjectLayout(layout, o.hidden_parts);
        snprintf(line, sizeof(line), "DRAW obj=%d model=%s arena=%s hidden=%d frame=%d shines=%d count=%d\n",
                 index, o.model->name.c_str(), o.arena.c_str(), (int)o.hidden_parts,
                 (int)(o.texture_frame >= 0 ? o.texture_frame : 0), o.effect_frames > 0 ? 1 : 0,
                 (int)count);
        out += line;

        snprintf(line, sizeof(line),
                 "  T %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
                 transform.m[0], transform.m[1], transform.m[2], transform.m[3], transform.m[4],
                 transform.m[5], transform.m[6], transform.m[7], transform.m[8], transform.m[9],
                 transform.m[10], transform.m[11], transform.m[12], transform.m[13], transform.m[14],
                 transform.m[15]);
        out += line;

        for (size_t d = before; d < stream.draws.size(); d++) {
            const ObjectStreamDraw& draw = stream.draws[d];
            const Surface& s = draw.surface;
            const int32_t first = draw.first;
            const int32_t corners = draw.corners;
            const Vertex& head = stream.vertices[(size_t)first];
            const Vertex& tail = stream.vertices[(size_t)(first + corners - 1)];
            float sx = 0.0f, sy = 0.0f, sz = 0.0f, su = 0.0f, sv = 0.0f;
            for (int32_t i = 0; i < corners; i++) {
                const Vertex& v = stream.vertices[(size_t)(first + i)];
                sx += v.pos.x;
                sy += v.pos.y;
                sz += v.pos.z;
                su += v.uv.x;
                sv += v.uv.y;
            }
            snprintf(line, sizeof(line),
                     "  B%d first=%d corners=%d tex=%s colour=%.4f,%.4f,%.4f,%.4f frames=%d "
                     "pass=%s shading=%s rowshift=%.4f sum=%.4f,%.4f,%.4f uvsum=%.4f,%.4f "
                     "first=%.4f,%.4f,%.4f,%.4f,%.4f last=%.4f,%.4f,%.4f,%.4f,%.4f hash=%08X\n",
                     (int)(d - before), (int)first, (int)corners,
                     s.texture ? s.texture->name().c_str() : "flat", s.colour.r, s.colour.g,
                     s.colour.b, s.colour.a, (int)s.frameCount, passName(s.pass), shadingName(s.shading),
                     s.rowShift, sx, sy, sz, su, sv, head.pos.x, head.pos.y, head.pos.z, head.uv.x,
                     head.uv.y, tail.pos.x, tail.pos.y, tail.pos.z, tail.uv.x, tail.uv.y,
                     hashCorners(stream.vertices, first, corners));
            out += line;
        }
    }

    snprintf(line, sizeof(line), "STREAM vertices=%d\n", (int)stream.vertices.size());
    out += line;
    return out;
}

std::string dumpRolling() {
    struct Step {
        float x, y, z;
    };
    struct Case {
        float yaw, pitch, roll, scale, radius, lift;
        int steps;
        Step move[7];
    };
    const Case cases[] = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 7,
         {{0.0f, 0.0f, 0.0f}, {1.5f, -0.25f, 0.0f}, {0.0f, 4.0f, 0.0f}, {-2.75f, 1.25f, 3.0f},
          {0.0f, 0.0f, 3.0f}, {12.0f, 9.0f, 3.0f}, {-3.0f, -3.0f, 3.0f}}},
        {37.0f, 11.0f, -23.0f, 2.0f, 5.15f, 5.0f, 5,
         {{0.0f, 0.0f, 0.0f}, {1.5f, -0.25f, 0.0f}, {12.0f, 9.0f, 0.0f}, {-3.0f, -3.0f, 0.0f},
          {0.5f, 0.25f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}},
        {200.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f, 6,
         {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {-1.0f, -1.0f, 0.0f}, {0.25f, 0.0f, 0.0f},
          {0.0f, 0.0f, 0.0f}, {7.0f, -7.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}},
    };

    std::string out;
    char line[512];
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const Case& k = cases[c];
        scripts::MdkObject obj;
        obj.yaw = k.yaw;
        obj.pitch = k.pitch;
        obj.roll = k.roll;
        obj.scale = k.scale;
        obj.roll_radius = k.radius;
        obj.height_offset = k.lift;
        obj.set_rolling(true);
        snprintf(line, sizeof(line),
                 "ROLL case=%d yaw=%.4f pitch=%.4f roll=%.4f scale=%.4f radius=%.4f lift=%.4f\n", (int)c,
                 k.yaw, k.pitch, k.roll, k.scale, k.radius, k.lift);
        out += line;
        for (int s = 0; s < k.steps; s++) {
            obj.position.x = k.move[s].x;
            obj.position.y = k.move[s].y;
            obj.position.z = k.move[s].z;
            obj.update_rolling();
            snprintf(line, sizeof(line), " R%d pos=%.4f,%.4f,%.4f\n", s, k.move[s].x, k.move[s].y,
                     k.move[s].z);
            out += line;

            const Mat4 b = obj.rolling_basis;
            out += "  B";
            for (int i = 0; i < 16; i++) {
                uint32_t bits;
                memcpy(&bits, &b.m[i], sizeof(bits));
                snprintf(line, sizeof(line), " %08X", bits);
                out += line;
            }
            out += "\n";

            const Mat4 t = obj.transform();
            snprintf(line, sizeof(line),
                     "  T %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                     "%.6f %.6f\n",
                     t.m[0], t.m[1], t.m[2], t.m[3], t.m[4], t.m[5], t.m[6], t.m[7], t.m[8], t.m[9],
                     t.m[10], t.m[11], t.m[12], t.m[13], t.m[14], t.m[15]);
            out += line;
            obj.previous_position = obj.position;
        }
    }
    return out;
}

// The script register file + per-object VM state, in the oracle's "script" dump format.
std::string dumpScript(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    // Runtime target-tracking state (the C# resets it to Kurt each tick and SelectTarget
    // may override). alien_target is the alien-target object's index in this dump's own
    // enumeration (removed objects skipped), or -1 when none — the same index the OBJ
    // lines use, so the two views agree.
    int alienIndex = -1;
    {
        int ai = 0;
        for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
             it != runtime.objects().end(); ++it) {
            if (runtime.isRemoved(&(*it))) {
                continue;
            }
            if (&(*it) == runtime.alienTarget()) {
                alienIndex = ai;
                break;
            }
            ai++;
        }
    }
    const Vec3& tp = runtime.targetPosition();
    snprintf(line, sizeof(line),
             "SCRIPT level=%d ticks=%d seed=1 currentArena=%s objects=%d target_pos=%.4f,%.4f,%.4f "
             "target_yaw=%.4f alien_target=%d\n",
             level, ticks, runtime.currentArena().c_str(), runtime.objectCount(), tp.x, tp.y, tp.z,
             runtime.targetYaw(), alienIndex);
    out += line;
    const std::array<float, 4>& g = runtime.globalVariables;
    snprintf(line, sizeof(line), "GVAR %.4f %.4f %.4f %.4f\n", g[0], g[1], g[2], g[3]);
    out += line;
    // Kurt's combat/inventory state (game/kurt.h): hurt_kurt (109) writes
    // health/flash/knock, if_inventory (175) reads the slots.
    const Kurt& k = runtime.kurt();
    snprintf(line, sizeof(line),
             "KURT health=%d hurt_flash=%.4f invulnerable=%.4f knock=%.4f state=%d slots=%d\n",
             k.health(), k.hurtFlash(), k.invulnerable(), k.knockDamage(), (int)k.state(),
             (int)k.slotCount());
    out += line;
    // The mixer's live voices: the ids the scripts were handed, in play order.
    // This is what if_own_sound (249) and if_sound_playing (233) ask about.
    const audio::SoundMixer& mixer = runtime.mixer();
    snprintf(line, sizeof(line), "AUDIO voices=%d", (int)mixer.voices().size());
    out += line;
    out += " list=";
    for (size_t v = 0; v < mixer.voices().size(); v++) {
        if (v) {
            out += ",";
        }
        snprintf(line, sizeof(line), "%d:", (int)mixer.voices()[v].id);
        out += line;
        out += mixer.voices()[v].name;
    }
    out += "\n";
    // The debris pieces in flight: shattered groups and sparks (Debris.Pieces).
    {
        const scripts::Debris& debris = runtime.debris();
        const std::vector<scripts::Debris::Piece*>& pieces = debris.pieces();
        snprintf(line, sizeof(line), "DEBRIS pieces=%d list=", (int)pieces.size());
        out += line;
        for (size_t pi = 0; pi < pieces.size(); pi++) {
            const scripts::Debris::Piece* p = pieces[pi];
            if (pi) {
                out += ",";
            }
            snprintf(line, sizeof(line), "%s@%.4f,%.4f,%.4fv%.4f,%.4f,%.4ft%d", p->arena.c_str(),
                     p->center.x, p->center.y, p->center.z, p->velocity.x, p->velocity.y,
                     p->velocity.z, (int)p->ticks);
            out += line;
        }
        out += "\n";
    }
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "OBJ %03d type=%s sflags=0x%08X vars=%.4f,%.4f,%.4f,%.4f restart=%d wait=%.4f "
                 "wresume=%d death=%d cmdtgt=%d gret=%s gres=%s timers=%.4f,%.4f,%.4f,%.4f,%.4f "
                 "tsnd=%s tvoice=%d tplay=%d lsnd=%s lvoice=%d lplay=%d\n",
                 index, o.type_name.c_str(), (unsigned)o.script_flags, o.variables[0], o.variables[1],
                 o.variables[2], o.variables[3], o.restart, o.wait_time, o.wait_resume, o.death_script,
                 o.command_target, joinInts(o.gosub_returns).c_str(), joinInts(o.gosub_restarts).c_str(),
                 o.level_timers[0], o.level_timers[1], o.level_timers[2], o.level_timers[3],
                 o.level_timers[4], o.tracked_sound.c_str(), (int)o.tracked_voice,
                 mixer.isVoicePlaying(o.tracked_voice) ? 1 : 0, o.loop_sound_name.c_str(),
                 (int)o.loop_sound, mixer.isVoicePlaying(o.loop_sound) ? 1 : 0);
        out += line;
    }
    return out;
}

// The DTI records in the oracle's "dti" dump format. The floats are printed as
// raw IEEE bits (not %.4f) so a one-ULP parser difference is caught, not rounded
// away. This locks the record layout -- including the boxEnd/name rewind quirk --
// against the C# Dti.ParseRecords.
std::string floatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    char buf[12];
    snprintf(buf, sizeof(buf), "%08X", u);
    return std::string(buf);
}

std::string dumpDti(int level, const Dti& dti) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "DTI level=%d arenas=%d\n", level, (int)dti.arenas().size());
    out += line;
    for (size_t i = 0; i < dti.arenas().size(); i++) {
        const Dti::ArenaEntry& a = dti.arenas()[i];
        snprintf(line, sizeof(line), "ARENA name=%s pitch=%s records=%d\n", a.name.c_str(),
                 floatBits(a.pitch).c_str(), (int)a.records.size());
        out += line;
        for (size_t j = 0; j < a.records.size(); j++) {
            const Dti::Record& r = a.records[j];
            snprintf(line, sizeof(line),
                     "REC type=%u id=%d angle=%s pos=%s,%s,%s box=%s,%s,%s name=%s\n",
                     r.type, r.id, floatBits(r.angle).c_str(),
                     floatBits(r.position[0]).c_str(), floatBits(r.position[1]).c_str(),
                     floatBits(r.position[2]).c_str(), floatBits(r.boxEnd[0]).c_str(),
                     floatBits(r.boxEnd[1]).c_str(), floatBits(r.boxEnd[2]).c_str(), r.name);
            out += line;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    // The shared camera/geometry trig path must be initialized before any
    // runtime can call fastAtan2 (opcode 203's camera-track calculation).
    mdk::mathLutInit();

    // The formats library reaches the filesystem through this seam.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    MdkData data = locateData();
    if (!data.found()) {
        printf("SKIP: no MDK data (set MDK_DATA_DIR)\n");
        return 0;
    }

    // Golden dumps (`tick1 dump <pose|transform|layout|draw|rolling> [level] [ticks]`):
    // print one dump to stdout, which is how a golden is written and how a failing
    // golden is diffed against the C# probe (the oracle twin of each of these
    // dumps; see docs/PROGRESS_LOG.md for the probe's recipe).
    if (argc > 1 && std::string(argv[1]) == "dump") {
        std::string what = argc > 2 ? argv[2] : "rolling";
        int level = argc > 3 ? atoi(argv[3]) : 3;
        int ticks = argc > 4 ? atoi(argv[4]) : 1;
        if (what == "rolling") {
            printf("%s", dumpRolling().c_str());
            return 0;
        }
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("the level did not load\n");
            return 1;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        Cmi cmi;
        if (!data.read(cmiPath, &cmiBytes) ||
            !Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("the CMI did not parse\n");
            return 1;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("the sound bank did not load\n");
            return 1;
        }
        bank.preload();
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }
        if (what == "pose") {
            printf("%s", dumpObjectPoses(level, ticks, runtime).c_str());
        } else if (what == "transform") {
            printf("%s", dumpObjectTransforms(level, ticks, runtime).c_str());
        } else if (what == "layout") {
            printf("%s", dumpObjectLayout(level, ticks, levelData, runtime).c_str());
        } else if (what == "draw") {
            printf("%s", dumpObjectDraw(level, ticks, levelData, runtime).c_str());
        } else {
            printf("unknown dump\n");
            return 1;
        }
        return 0;
    }

    // Reachability probe (`tick1 probe [ticks]`): run each level and list the
    // opcodes the scripts reached that the VM does not implement yet, with
    // hit counts. This is how the next opcode slice is chosen (evidence, not
    // memory — see docs/OPCODE_COVERAGE.md).
    if (argc > 1 && std::string(argv[1]) == "probe") {
        int probeTicks = argc > 2 ? atoi(argv[2]) : 10000;
        for (int level = 3; level <= 8; level++) {
            LevelData levelData;
            if (!LevelData::load(&levelData, &data, level)) {
                continue;
            }
            char cmiPath[128];
            snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
            std::vector<uint8_t> cmiBytes;
            if (!data.read(cmiPath, &cmiBytes)) {
                continue;
            }
            Cmi cmi;
            if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                continue;
            }
            audio::SoundBank bank;
            if (!audio::SoundBank::forLevel(&bank, data, level)) {
                continue;
            }
            bank.preload();
            scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
            runtime.option = 0;
            for (int t = 0; t < probeTicks; t++) {
                runtime.update(1.0f / 30.0f);
            }
            printf("PROBE L%d ticks=%d unimplemented:", level, probeTicks);
            const std::vector<std::pair<int32_t, int32_t> >& un = runtime.unimplemented();
            for (size_t i = 0; i < un.size(); i++) {
                printf(" %d(x%d)", (int)un[i].first, (int)un[i].second);
            }
            printf("\n");
        }
        return 0;
    }

    int failures = 0;
    // (level, ticks) cases. Tick 1 covers every shipped level; the multi-tick cases
    // exercise the motion slices that move an object after its spawn tick:
    // path-following, gravity free-fall, the BSP footing sweep, and the
    // face_target / turn_to_yaw turning opcodes. A level/tick is added here only
    // once its whole table matches the oracle byte for byte. The deeper ticks still
    // differ and are deferred to their own slices: the walking/chasing enemies'
    // stuck detection and replanning (DetectStuck/HandleStuck), the combat and
    // death paths, and the transient object effects (projectiles, explosions).
    struct Case {
        int level;
        int ticks;
    };
    const Case cases[] = {
        {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
        {3, 2}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {5, 10}, {6, 2}, {6, 3}, {6, 4}, {6, 5}, {6, 6}, {6, 10}, {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}, {7, 7}, {7, 10},
        {4, 15}, {5, 15}, {6, 15}, {7, 15}, {4, 20}, {5, 20}, {6, 20}, {7, 20},
        {5, 25}, {6, 25}, {7, 25}, {5, 30}, {6, 30}, {7, 30},
        {5, 40}, {6, 40}, {7, 40}, {5, 60}, {6, 60}, {7, 60},
        // The XGEN spawner's aliens: the countdown (add_var_dt) opens the spawn at
        // ~t119, and the spawned XG runs its spawn script, lands (footing), and walks
        // off with set_move_x (command 88) — the tables at t240/t300 keep matching
        // while it moves.
        {5, 120}, {5, 240}, {5, 300},
        // Stuck detection/replan (DetectStuck: 16 ticks walking, 9 with 197;
        // HandleStuck: replan once per stuck flag, slide, give up): L8's walkers
        // wander GUNT for hundreds of ticks — the stuck block is what keeps the
        // long walks deterministic between the walkers.
        {6, 300}, {8, 40}, {8, 60}, {8, 120},
        // With .NET's degree/radian conversions spelled left-associatively (the
        // constant-folded forms are 1 ULP off at many inputs) the yaws are
        // bit-exact, so the previously-excluded cases match: L5_t180 and L3_t3
        // (their 4th-decimal yaw shifts), L8_t2+ (the walk planner's detour picked
        // a different waypoint off the ULP), and every level runs green to t300
        // except where the walk's stuck/replan slice begins to matter (L8_t120's
        // table, L3_t300's XS, and past t300).
        {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 10}, {3, 15}, {3, 30}, {3, 60}, {3, 120},
        // The XS controller fires the BOLT (fire, opcode 61): the projectile
        // spawns at the reference point, flies straight, and dies on its lifetime.
        {3, 300},
        {8, 300},
        {4, 25}, {4, 30}, {4, 40}, {4, 60}, {4, 120}, {4, 300},
        {5, 180},
        {6, 120}, {6, 300},
        {7, 120}, {7, 300},
        {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 6}, {8, 10}, {8, 15}, {8, 20}, {8, 25}, {8, 30},
        {8, 40}, {8, 60},
        // Deeper ticks that stay byte-exact with the oracle: the spawner/walker
        // scripts run past t300 without a new slice (L3 and L8 diverge earlier).
        {4, 360}, {4, 420}, {4, 480}, {4, 540}, {4, 600}, {5, 360}, {5, 420}, {5, 480}, {5, 540}, {6, 360}, {6, 420}, {6, 480}, {6, 540}, {6, 600}, {7, 360}, {7, 420}, {7, 480}, {7, 540}, {7, 600}, {8, 360},
        // L5 past t540: the XGEN spawner's 5th XG opens (~t585) and dies (~t595), and
        // object 004 takes a move_near_target (opcode 43) at ~t577. The old "XG[004]
        // divergence ~t577" was the same degree/radian constant-fold ULP class the
        // left-associative spelling resolves (see the note above), so the whole region
        // now matches the oracle byte for byte (table and script state).
        {5, 577}, {5, 585}, {5, 590}, {5, 595}, {5, 600},
        // Deeper ticks that stay byte-exact with the oracle, generated from it. Each
        // level stops at the first tick where an unported slice diverges (the pickup/
        // chute/death lifecycle and the transient effects), so these are the honest
        // frontier per level: L3 to t420 (objects drop off as pickups/chutes resolve),
        // L5 to t660 (the XGEN region past t600), L6 to t900 (the XD keeps walking —
        // the table genuinely cycles, not a frozen steady state), L7 t660/t900 bracket
        // a steady arena to prove no slow drift. L4 empties out past t600 (0 objects)
        // and L8 diverges at t420 (SW_H01/SW_CHUTE removal), so neither extends.
        {3, 360}, {3, 420},
        {5, 660},
        {6, 660}, {6, 720}, {6, 780}, {6, 840}, {6, 900},
        {7, 660}, {7, 900},
        // The pickup/chute/death lifecycle slice (faithful Kill death-script branch +
        // Remove taking a live Attached with it) moved the frontier from ~t400-900 to
        // the end of the simulated corpus: every level now matches the oracle byte for
        // byte (table AND script) far past the old boundary. L3/L6/L7/L8 match to at
        // least t30000 with live objects (11/5/7/10 — a genuinely cycling steady
        // state, not an empty arena); L4 empties its arena (0 objects, matching the
        // oracle's empty at the same ticks) and matches to t10000. L5's t2303 BOLT
        // spawn was the unported if_wall (opcode 115): the XG's approach script
        // probes 42 units ahead for a wall before committing to the walk, and the
        // missing case fell through to the no-wall branch, skipping the fire that
        // spawned the 16th BOLT. With it ported, L5 matches the oracle byte for
        // byte (table AND script) to t30000 like the other full levels.
        {3, 480}, {3, 600}, {3, 1200}, {3, 2400}, {3, 5000}, {3, 10000}, {3, 30000},
        {4, 660}, {4, 900}, {4, 1200}, {4, 2400}, {4, 5000}, {4, 10000}, {4, 30000},
        {5, 720}, {5, 900}, {5, 1200}, {5, 1800}, {5, 2300}, {5, 2302},
        {5, 2303}, {5, 2350}, {5, 2400}, {5, 3000}, {5, 5000}, {5, 10000}, {5, 30000},
        {6, 960}, {6, 1200}, {6, 2400}, {6, 5000}, {6, 10000}, {6, 30000},
        {7, 960}, {7, 1200}, {7, 2400}, {7, 5000}, {7, 10000}, {7, 30000},
        {8, 420}, {8, 600}, {8, 1200}, {8, 2400}, {8, 5000}, {8, 10000}, {8, 30000},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("FAIL L%d: did not load\n", level);
            failures++;
            continue;
        }

        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            printf("FAIL L%d: CMI did not read\n", level);
            failures++;
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("FAIL L%d: CMI did not parse\n", level);
            failures++;
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d: sound bank did not load\n", level);
            failures++;
            continue;
        }
        bank.preload();

        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
        runtime.option = 0;  // the oracle's Option
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }

        std::string got = dumpTable(level, ticks, runtime);

        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d: no golden file %s\n", level, ticks, goldenPath);
            failures++;
            continue;
        }

        if (got == expected) {
            printf("PASS L%d_t%d: table matches the oracle (%d objects)\n", level, ticks,
                   runtime.objectCount());
        } else {
            printf("FAIL L%d_t%d: table differs\n--- got ---\n%s--- expected ---\n%s", level, ticks,
                   got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Script register file + per-object VM state. The object table can match while a
    // script variable or the VM's per-object state (restart/wait/timers) diverges, so
    // this is a second, independent byte-for-byte check over the same cases.
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            continue;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d_t%d_script: sound bank did not load\n", level, ticks);
            failures++;
            continue;
        }
        bank.preload();
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }
        std::string got = dumpScript(level, ticks, runtime);
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_script.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            // No script golden yet: the script state diverges on a later slice (the
            // door's wait_anim_frame parks restart on an animation frame the advance
            // slice has not landed). Skip, not fail — the object table above still
            // covers this case. A golden is added here once the state matches.
            printf("SKIP L%d_t%d_script: no script golden (later slice)\n", level, ticks);
            continue;
        }
        if (got == expected) {
            printf("PASS L%d_t%d_script: register file + VM state match the oracle\n", level, ticks);
        } else {
            printf("FAIL L%d_t%d_script: script state differs\n--- got ---\n%s--- expected ---\n%s",
                   level, ticks, got.c_str(), expected.c_str());
            failures++;
        }
    }

    // The object pass's pose and place. The pose (the rest vertices or the
    // baked animation frame) is pinned at ticks where the scripts have an alien
    // walking, so the bake is exercised. The place (the transform chain) is
    // pinned at tick 1 only, because the port does not write MdkObject::roll
    // yet -- ObjectMotion.Bank (the automatic lean into a turn) is not ported,
    // so by tick 120 the oracle's XF has roll -6.3411 where the port still has
    // 0, and every transform that turns by roll differs. See OPEN_ITEMS.md.
    const Case poseCases[] = {{3, 1}, {3, 120}, {3, 1200}};
    const Case transformCases[] = {{3, 1}};
    const Case layoutCases[] = {{3, 1}};
    for (size_t c = 0; c < sizeof(poseCases) / sizeof(poseCases[0]); c++) {
        int level = poseCases[c].level;
        int ticks = poseCases[c].ticks;
        // These are fixed acceptance cases, not optional probes: a level that will
        // not load, a CMI that will not parse or a sound bank that will not open
        // is a regression in the port, so it FAILS here rather than skipping the
        // goldens below (a skipped golden reads like a passing one).
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("FAIL L%d_t%d_object: the level did not load\n", level, ticks);
            failures++;
            continue;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            printf("FAIL L%d_t%d_object: %s did not read\n", level, ticks, cmiPath);
            failures++;
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("FAIL L%d_t%d_object: the CMI did not parse\n", level, ticks);
            failures++;
            continue;
        }
        audio::SoundBank bank;
        if (!audio::SoundBank::forLevel(&bank, data, level)) {
            printf("FAIL L%d_t%d_object: the sound bank did not load\n", level, ticks);
            failures++;
            continue;
        }
        bank.preload();
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }

        std::string got = dumpObjectPoses(level, ticks, runtime);
        char goldenPath[160];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_pose.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d_pose: no golden file %s\n", level, ticks, goldenPath);
            failures++;
        } else if (got == expected) {
            printf("PASS L%d_t%d_pose: every object's posed vertices match the oracle\n", level,
                   ticks);
        } else {
            printf("FAIL L%d_t%d_pose: %s\n", level, ticks, firstDifference(got, expected).c_str());
            failures++;
        }

        got = dumpObjectTransforms(level, ticks, runtime);
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_transform.txt", level, ticks);
        bool wanted = false;
        for (size_t i = 0; i < sizeof(transformCases) / sizeof(transformCases[0]); i++) {
            wanted = wanted || (transformCases[i].level == level && transformCases[i].ticks == ticks);
        }
        if (!wanted) {
            continue;
        }
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d_transform: no golden file %s\n", level, ticks, goldenPath);
            failures++;
        } else if (got == expected) {
            printf("PASS L%d_t%d_transform: every object's transform matches the oracle\n", level,
                   ticks);
        } else {
            printf("FAIL L%d_t%d_transform: %s\n", level, ticks,
                   firstDifference(got, expected).c_str());
            failures++;
        }

        // The layout stage (ObjectView.Build/Arrange/Visible), over the same tick.
        wanted = false;
        for (size_t i = 0; i < sizeof(layoutCases) / sizeof(layoutCases[0]); i++) {
            wanted = wanted || (layoutCases[i].level == level && layoutCases[i].ticks == ticks);
        }
        if (!wanted) {
            continue;
        }
        got = dumpObjectLayout(level, ticks, levelData, runtime);
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_layout.txt", level, ticks);
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d_layout: no golden file %s\n", level, ticks, goldenPath);
            failures++;
        } else if (got == expected) {
            printf("PASS L%d_t%d_layout: every model's batches, corners, UVs and order match the "
                   "oracle\n", level, ticks);
        } else {
            printf("FAIL L%d_t%d_layout: %s\n", level, ticks,
                   firstDifference(got, expected).c_str());
            failures++;
        }

        // The draw stage (ObjectView.Draw), over the same tick: the frame's
        // stream, the offset and corner count of every batch, and the surface,
        // animated frame and transform each is drawn with.
        got = dumpObjectDraw(level, ticks, levelData, runtime);
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_draw.txt", level, ticks);
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d_draw: no golden file %s\n", level, ticks, goldenPath);
            failures++;
        } else if (got == expected) {
            printf("PASS L%d_t%d_draw: every object's streamed corners, batch order, surfaces and "
                   "transform match the oracle\n", level, ticks);
        } else {
            printf("FAIL L%d_t%d_draw: %s\n", level, ticks,
                   firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // The rolling basis (ObjectMotion.Roll), driven directly: see dumpRolling.
    {
        std::string got = dumpRolling();
        std::string expected;
        if (!readText("golden/rolling_basis.txt", &expected)) {
            printf("FAIL rolling_basis: no golden file golden/rolling_basis.txt\n");
            failures++;
        } else if (got == expected) {
            printf("PASS rolling_basis: the rolling basis and the transform that draws with it "
                   "match the oracle\n");
        } else {
            printf("FAIL rolling_basis: %s\n", firstDifference(got, expected).c_str());
            failures++;
        }
    }

    // Footing (the BSP sweep that stops a gravity object on a floor). The L3_t3
    // table is now a whole-table golden; the footing probes below pin the clean
    // landings directly: each XG rests on its arena floor exactly as the oracle,
    // with velocity.Z zeroed so it stays put on later ticks.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 3)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The two XG spawn at z=127.0 and fall; the oracle lands them at
                    // 126.9500 and 126.9644 (their arena floors), which free-fall alone
                    // would overshoot (126.8933).
                    int checked = 0;
                    const float expectedZ[2] = {126.9500f, 126.9644f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        if (xi < 2) {
                            float z = it->position.z;
                            bool ok = z > expectedZ[xi] - 0.0001f && z < expectedZ[xi] + 0.0001f;
                            printf("%s footing XG[%d] landed z=%.4f (oracle %.4f)\n", ok ? "PASS" : "FAIL",
                                   xi, z, expectedZ[xi]);
                            if (!ok) {
                                failures++;
                            }
                            checked++;
                            xi++;
                        }
                    }
                    if (checked != 2) {
                        printf("FAIL footing: expected 2 XG objects, found %d\n", checked);
                        failures++;
                    }
                }
            }
        }
    }

    // Walk command (move_to opcode 78 -> StartMove -> PlanMove -> Walk). The commanded
    // XGs prove the turn math the oracle agrees with byte for byte: opcode 78 records
    // the command, PlanMove finds a clear line (waypoint = destination), and Walk turns
    // the yaw toward it (90 -> 84.5768 and 90 -> 84.0) via the atan2f/wrap/clamp turn;
    // they have not translated yet (speed still ramping from 0). The first XD proves the
    // FULL chain including translation: its script gates the move behind if_sees_kurt
    // and if_flag_set (both now ported), so it walks to the oracle's exact position and
    // yaw. The second XD is a near-miss (yaw 356.3 vs the oracle's 354.0): its direct
    // line is blocked and the detour waypoint differs, a BSP-raycast/detour fidelity
    // slice separate from this one, so the whole L8_t2 table is not yet green.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 8)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL8/LEVEL8.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 8)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
                    runtime.option = 0;
                    for (int t = 0; t < 2; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The first two XGs (in table order) are commanded to move; the
                    // oracle turns them to these yaws with move command 78 active.
                    const float expectedYaw[2] = {84.5768f, 84.0f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end() && xi < 2; ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        float yaw = it->yaw;
                        bool yawOk = yaw > expectedYaw[xi] - 0.0001f && yaw < expectedYaw[xi] + 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XG[%d] yaw=%.4f move=%d (oracle yaw %.4f move 78)\n",
                               (yawOk && cmdOk) ? "PASS" : "FAIL", xi, yaw, it->move_command,
                               expectedYaw[xi]);
                        if (!yawOk || !cmdOk) {
                            failures++;
                        }
                        xi++;
                    }
                    if (xi != 2) {
                        printf("FAIL walk: expected 2 commanded XG objects, found %d\n", xi);
                        failures++;
                    }
                    // The first XD (x > 0) is commanded via if_sees_kurt + if_flag_set
                    // and walks to the oracle's exact position and yaw (full translation).
                    const float xdPos[3] = {18.0663f, 115.0070f, 11.9600f};
                    const float xdYaw = 6.0f;
                    bool xdFound = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XD" || it->position.x <= 0.0f) {
                            continue;
                        }
                        xdFound = true;
                        bool posOk = fabsf(it->position.x - xdPos[0]) < 0.0001f &&
                                     fabsf(it->position.y - xdPos[1]) < 0.0001f &&
                                     fabsf(it->position.z - xdPos[2]) < 0.0001f;
                        bool yawOk = fabsf(it->yaw - xdYaw) < 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XD pos=(%.4f,%.4f,%.4f) yaw=%.4f move=%d (oracle "
                               "(18.0663,115.0070,11.9600) yaw 6.0000 move 78)\n",
                               (posOk && yawOk && cmdOk) ? "PASS" : "FAIL", it->position.x,
                               it->position.y, it->position.z, it->yaw, it->move_command);
                        if (!posOk || !yawOk || !cmdOk) {
                            failures++;
                        }
                        break;
                    }
                    if (!xdFound) {
                        printf("FAIL walk: expected a commanded XD object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    // Pickup landing (UpdatePickup -> Fall). L7_t3 proves the Runner (SW_H150) lands
    // with hover 0, but that does not exercise the Z += hover arithmetic. SW_H01 is a
    // normal pickup that lands on the floor at tick 3 with hover 1.5 (z 103.95 ->
    // 105.45) and FlagLanded set, so it verifies the non-Runner landing path.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    audio::SoundBank bank;
                    if (audio::SoundBank::forLevel(&bank, data, 3)) {
                        bank.preload();
                    }
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    bool found = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "SW_H01") {
                            continue;
                        }
                        found = true;
                        bool zOk = fabsf(it->position.z - 105.4500f) < 0.0001f;
                        bool landed = (it->flags & 0x20000) != 0;  // FlagLanded
                        printf("%s pickup SW_H01 z=%.4f landed=%d (oracle z 105.4500 landed)\n",
                               (zOk && landed) ? "PASS" : "FAIL", it->position.z, landed ? 1 : 0);
                        if (!zOk || !landed) {
                            failures++;
                        }
                        break;
                    }
                    if (!found) {
                        printf("FAIL pickup: expected an SW_H01 object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    // DTI record layout. The object-table goldens above prove the DTI-derived
    // SPAWNED objects match, but they do not observe every record field (the
    // boxEnd far-corner and the name bytes a fan/wind zone keeps in them). This
    // dumps every arena's records with the floats as raw bits and compares to the
    // oracle's, closing the "DTI record discrepancy" claim with a reproduction:
    // the C++ Dti::parseRecords is byte-identical to the C# Dti.ParseRecords.
    for (int level = 3; level <= 8; level++) {
        char dtiPath[128];
        snprintf(dtiPath, sizeof(dtiPath), "TRAVERSE/LEVEL%d/LEVEL%d.DTI", level, level);
        std::vector<uint8_t> dtiBytes;
        if (!data.read(dtiPath, &dtiBytes)) {
            printf("FAIL L%d_dti: DTI did not read\n", level);
            failures++;
            continue;
        }
        Dti dti = Dti::parse(dtiBytes.data(), (int32_t)dtiBytes.size());
        std::string got = dumpDti(level, dti);
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_dti.txt", level);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_dti: no golden file %s\n", level, goldenPath);
            failures++;
            continue;
        }
        if (got == expected) {
            printf("PASS L%d_dti: %d arenas, records match the oracle byte for byte\n", level,
                   (int)dti.arenas().size());
        } else {
            printf("FAIL L%d_dti: records differ\n--- got ---\n%s--- expected ---\n%s", level,
                   got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Dynamic scene updates (open item 1). The scene is built once but the
    // scripts change group state (group_set_state) and group material
    // (group_set_texture) mid-play. This checks the three things that make that
    // work, against real arena geometry:
    //   (a) tagging  -- every batch belongs to exactly one group, and the
    //                   batches partition the drawn triangles by group (so a
    //                   per-batch hidden check skips exactly the hidden group);
    //   (b) hide     -- a group hidden after the build is not visible, while the
    //                   other groups still are (the C# draw-time Hidden check,
    //                   LevelView.cs:126), with no rebuild;
    //   (c) retexture-- a group given a new material shows it on a rebuild (the
    //                   C# LevelView.Rebuild path, LevelView.cs:251).
    {
        int arenasChecked = 0;
        int crossAdjTotal = 0;  // adjacent drawn triangles in different groups sharing a material
        bool testedHide = false, testedRetexture = false;
        for (int level = 3; level <= 8; level++) {
            LevelData levelData;
            if (!LevelData::load(&levelData, &data, level)) {
                continue;
            }
            for (size_t ai = 0; ai < levelData.arenas().size(); ai++) {
                const Arena* arenaC = levelData.arenas()[ai];
                Arena* arena = levelData.arenaNamedMutable(arenaC->name());
                if (!arena) {
                    continue;
                }
                Palette& palette = levelData.paletteOf(arena);
                const std::vector<const TextureArchive*> archives = levelData.archivesOf(arena);
                MaterialResolver resolver(&levelData.dti(), Shading::Original);
                ArenaScene scene;
                if (!buildArenaScene(arena, resolver, palette, archives, &scene) ||
                    scene.batches.empty()) {
                    continue;
                }
                arenasChecked++;

                // (a) Tagging, on EVERY arena: per group, the batch triangles must
                // equal the drawn triangles -- a triangle is drawn when it is not
                // clip/disabled and its material (the group's, else its own) is
                // drawn. A batch that spanned two groups would misattribute its
                // triangles and break this equality, so this proves the build
                // breaks runs at group boundaries (the run-break) and that the
                // batches partition the drawn triangles by group exactly once.
                const std::vector<uint32_t>& flags = arena->triangleFlags();
                const std::vector<int32_t>& mats = arena->triangleMaterials();
                const std::vector<std::string>& names = arena->materials();
                std::map<int32_t, int32_t> expected, got;
                int32_t prevGroup = -1, prevMat = -999;
                bool prevDrawn = false;
                for (size_t t = 0; t < flags.size(); t++) {
                    // A clip/disabled triangle is skipped without breaking the run
                    // (buildArenaScene `continue`s before the run logic), so it does
                    // not reset the previous-drawn tracking either.
                    if ((flags[t] & (arena->clipFlag() | Arena::kGroupDisabled)) != 0) {
                        continue;
                    }
                    const int32_t g = (int32_t)(flags[t] >> 24);
                    int32_t gm = 0;
                    const int32_t mat = arena->groupMaterial(g, &gm) ? gm : mats[t];
                    if (MaterialResolver::isDrawn(mat, names, archives)) {
                        expected[g]++;
                        // Two consecutive drawn triangles in different groups that
                        // share a material: only the run-break keeps them in
                        // separate batches. Count them so the test proves it is
                        // exercised, not just present.
                        if (prevDrawn && g != prevGroup && mat == prevMat) {
                            crossAdjTotal++;
                        }
                        prevDrawn = true;
                        prevGroup = g;
                        prevMat = mat;
                    } else {
                        prevDrawn = false;  // a not-drawn material breaks the run
                    }
                }
                for (size_t i = 0; i < scene.batches.size(); i++) {
                    got[scene.batches[i].group] += scene.batches[i].count / 3;
                }
                if (!(expected == got)) {
                    printf("FAIL scene tagging L%d %s: batches do not partition drawn triangles by group\n",
                           level, arena->name().c_str());
                    failures++;
                }

                // (b) Hide: a non-zero group with batches, hidden after the build.
                if (!testedHide) {
                    int32_t g = 0;
                    for (size_t i = 0; i < scene.batches.size(); i++) {
                        if (scene.batches[i].group != 0) { g = scene.batches[i].group; break; }
                    }
                    if (g != 0) {
                        arena->setGroupState(g, Arena::kGroupHidden);
                        bool anyOfG = false, hiddenAllHidden = true, othersVisible = true;
                        for (size_t i = 0; i < scene.batches.size(); i++) {
                            const bool vis = arenaBatchVisible(arena, scene.batches[i]);
                            if (scene.batches[i].group == g) {
                                anyOfG = true;
                                if (vis) hiddenAllHidden = false;
                            } else if (!vis) {
                                othersVisible = false;
                            }
                        }
                        const bool ok = anyOfG && hiddenAllHidden && othersVisible;
                        printf("%s scene hide L%d %s group %d: hidden group's batches not drawn, others still drawn\n",
                               ok ? "PASS" : "FAIL", level, arena->name().c_str(), g);
                        if (!ok) failures++;
                        testedHide = true;
                        arena->setGroupState(g, 0);  // restore for any later check
                    }
                }

                // (c) Retexture: a non-zero group with a textured batch, given a
                // flat colour; on a rebuild its batches must all be flat (the new
                // material took effect), proving the scene reflects group_set_texture.
                if (!testedRetexture) {
                    int32_t g = 0;
                    for (size_t i = 0; i < scene.batches.size(); i++) {
                        if (scene.batches[i].group != 0 && scene.batches[i].surface.texture != nullptr) {
                            g = scene.batches[i].group;
                            break;
                        }
                    }
                    if (g != 0) {
                        arena->setGroupMaterial(g, -1);  // flat palette colour 1
                        ArenaScene rebuilt;
                        buildArenaScene(arena, resolver, palette, archives, &rebuilt);
                        bool anyOfG = false, allFlat = true;
                        for (size_t i = 0; i < rebuilt.batches.size(); i++) {
                            if (rebuilt.batches[i].group == g) {
                                anyOfG = true;
                                if (rebuilt.batches[i].surface.texture != nullptr) allFlat = false;
                            }
                        }
                        const bool ok = anyOfG && allFlat;
                        printf("%s scene retexture L%d %s group %d: after group_set_texture(-1) its batches are flat on rebuild\n",
                               ok ? "PASS" : "FAIL", level, arena->name().c_str(), g);
                        if (!ok) failures++;
                        testedRetexture = true;
                    }
                }
            }
        }
        printf("PASS scene tagging: %d arenas partition the drawn triangles by group (%d cross-group same-material adjacencies exercised the run-break)\n",
               arenasChecked, crossAdjTotal);
        if (arenasChecked == 0 || crossAdjTotal == 0) {
            printf("FAIL scene dynamic: tagging did not exercise the group run-break (arenas=%d crossAdj=%d)\n",
                   arenasChecked, crossAdjTotal);
            failures++;
        }
        if (!testedHide || !testedRetexture) {
            printf("FAIL scene dynamic: did not exercise hide/retexture (hide=%d retexture=%d)\n",
                   (int)testedHide, (int)testedRetexture);
            failures++;
        }
    }

    if (failures == 0) {
        printf("\nAll cases match the oracle.\n");
        return 0;
    }
    printf("\n%d case(s) differ.\n", failures);
    return 1;
}
