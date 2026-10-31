// ============================================================================
// object_view.cpp — see object_view.h.
// ============================================================================

#include "game/object_view.h"

#include <algorithm>
#include <string>
#include <vector>

#include "formats/model.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "formats/vector_types.h"

namespace mdk {

namespace {

// The C#'s TriangleVertices.
const int32_t kTriangleVertices = 3;

// Whether two surfaces are the same batch. The C# keys its batches by the
// Material record, whose fields are the texture, the palette, the colour, the
// frame count, the pass, the row shift, the shading, the blend and the depth
// layer. The layout is per palette, and the blend and depth layer are the
// defaults for every surface a resolver produces, so what is left to compare is
// everything the C++ Surface carries.
bool sameSurface(const Surface& a, const Surface& b) {
    return a.texture == b.texture && a.frameCount == b.frameCount && a.pass == b.pass &&
           a.shading == b.shading && a.rowShift == b.rowShift && a.colour.r == b.colour.r &&
           a.colour.g == b.colour.g && a.colour.b == b.colour.b && a.colour.a == b.colour.a;
}

} // namespace

bool objectPartHidden(int32_t hidden, int32_t part) {
    return (hidden & (int32_t)(1u << ((uint32_t)part & 31u))) != 0;
}

int32_t arrangeObjectLayout(ObjectLayout* layout, int32_t hidden) {
    if (layout == nullptr) {
        return 0;
    }
    int32_t count = 0;
    for (size_t b = 0; b < layout->batches.size(); b++) {
        int32_t first = kObjectUnseen;
        const std::vector<int32_t>& starts = layout->batches[b].firstTriangle;
        for (size_t p = 0; p < starts.size(); p++) {
            if (!objectPartHidden(hidden, (int32_t)p)) {
                if (starts[p] < first) {
                    first = starts[p];
                }
            }
        }
        if (first == kObjectUnseen) {
            continue;
        }

        // Insertion sort: a model has a few surfaces.
        int32_t at = count++;
        while (at > 0 && layout->keys[(size_t)(at - 1)] > first) {
            layout->keys[(size_t)at] = layout->keys[(size_t)(at - 1)];
            layout->order[(size_t)at] = layout->order[(size_t)(at - 1)];
            at--;
        }
        layout->keys[(size_t)at] = first;
        layout->order[(size_t)at] = (int32_t)b;
    }
    return count;
}

int32_t objectBatchCorners(const ObjectBatch& batch, int32_t hidden) {
    int32_t corners = 0;
    for (size_t p = 0; p < batch.partCorners.size(); p++) {
        if (!objectPartHidden(hidden, (int32_t)p)) {
            corners += batch.partCorners[p];
        }
    }
    return corners;
}

Surface shineSurface(const Surface& surface, bool shines) {
    if (shines && surface.shading == Shading::Lit) {
        Surface unlit = surface;
        unlit.shading = Shading::Sprite;
        return unlit;
    }
    return surface;
}

bool buildObjectLayout(const formats::Model& model, const ObjectLook& look,
                       const MaterialResolver& resolver, ObjectLayout* out) {
    if (out == nullptr || look.palette == nullptr) {
        return false;
    }
    out->batches.clear();
    out->order.clear();
    out->keys.clear();

    const int32_t parts = (int32_t)model.partList.size();

    // The model's material names, which is what the C# passes as
    // model.Materials.
    std::vector<std::string> names;
    names.reserve(model.materials.size());
    for (size_t i = 0; i < model.materials.size(); i++) {
        names.push_back(model.materials[i].nameString());
    }

    int32_t sequence = 0;
    for (int32_t p = 0; p < parts; p++) {
        const formats::Model::Part& part = model.partList[(size_t)p];
        for (int32_t t = 0; t < (int32_t)part.triangleMaterials.size(); t++, sequence++) {
            Surface surface;
            if (!resolver.resolve(part.triangleMaterials[(size_t)t], names, *look.palette,
                                  look.archives, &surface, Pass::DoubleSided)) {
                continue;
            }

            // The batch of this surface, or a new one at the end (the C#'s
            // bySurface dictionary: batches are in the order their surface was
            // first seen).
            size_t index = 0;
            for (; index < out->batches.size(); index++) {
                if (sameSurface(out->batches[index].surface, surface)) {
                    break;
                }
            }
            if (index == out->batches.size()) {
                ObjectBatch batch;
                batch.surface = surface;
                batch.partCorners.assign((size_t)parts, 0);
                batch.firstTriangle.assign((size_t)parts, kObjectUnseen);
                out->batches.push_back(batch);
            }
            ObjectBatch& batch = out->batches[index];

            // UVs are in texels of one frame: the C# scales them by the
            // texture's own size, and a flat surface -- which has no texture to
            // count texels in -- gets zero for both, so its UVs are all (0,0).
            float scaleX = 0.0f;
            float scaleY = 0.0f;
            if (surface.texture != nullptr) {
                scaleX = 1.0f / (float)surface.texture->width();
                scaleY = 1.0f / (float)surface.texture->height();
            }

            if (sequence < batch.firstTriangle[(size_t)p]) {
                batch.firstTriangle[(size_t)p] = sequence;
            }
            batch.partCorners[(size_t)p] += kTriangleVertices;
            for (int32_t k = 0; k < kTriangleVertices; k++) {
                const size_t corner = (size_t)(t * kTriangleVertices + k);
                batch.parts.push_back(p);
                batch.indices.push_back(part.triangleIndices[corner]);
                batch.uvs.push_back(Vec2{part.triangleUvs[corner].x * scaleX,
                                         part.triangleUvs[corner].y * scaleY});
            }
        }
    }

    if (out->batches.empty()) {
        return false;
    }
    // The arrange scratch, sized once (the C#'s Layout constructor).
    out->order.assign(out->batches.size(), 0);
    out->keys.assign(out->batches.size(), 0);
    return true;
}

ObjectLayout* ObjectView::layoutOf(const formats::Model* model, const ObjectLook& look) {
    if (model == nullptr || look.palette == nullptr) {
        return nullptr;
    }
    const std::pair<const formats::Model*, const formats::Palette*> key(model, look.palette);
    std::map<decltype(key), std::unique_ptr<ObjectLayout> >::iterator found = layouts_.find(key);
    if (found != layouts_.end()) {
        return found->second.get();
    }
    std::unique_ptr<ObjectLayout> made(new ObjectLayout());
    if (!buildObjectLayout(*model, look, *resolver_, made.get())) {
        // The C# caches the null too, so a model with nothing to draw is not
        // rebuilt every draw. The empty layout is dropped; the map keeps a null.
        made.reset();
    }
    return layouts_.emplace(key, std::move(made)).first->second.get();
}

// The C#'s ObjectView.Draw, with the two renderer seam calls replaced by the
// frame's stream: `renderer.Stream(corners, out mesh, out first)` is the cursor
// bump at the end of `stream->vertices`, and `renderer.Draw(mesh, first,
// corners, material, frame, transform)` is the queued ObjectStreamDraw. The
// guards, PoseParts, Arrange, Visible, the per-part copy loop, Shine and the
// frame selection are the oracle's, in the oracle's order.
bool ObjectView::addObject(const scripts::MdkObject& obj, const ObjectLook& look,
                           const Mat4& transform, ObjectStream* stream) {
    if (obj.model == nullptr || obj.dead || stream == nullptr) {
        return false;
    }
    ObjectLayout* layout = layoutOf(obj.model, look);
    if (layout == nullptr) {
        return false;
    }

    const std::vector<std::vector<formats::Vec3f> >& pose = obj.pose_parts();
    const int32_t hidden = obj.hidden_parts;

    // A pose that is not this model's is a stale pose cache (see
    // scripts/pose_cache.h and LevelData::load), and the C# would take an
    // IndexOutOfRange there. C++ has no exception to throw, so the object draws
    // nothing instead of reading past the pose; the layout is keyed by the same
    // model, so a part count that disagrees means the pose is not this model's.
    if (pose.size() != obj.model->partList.size()) {
        return false;
    }

    // The C#'s `TextureFrame >= 0 ? TextureFrame : 0` and its Glow.Shines test.
    const int32_t frame = obj.texture_frame >= 0 ? obj.texture_frame : 0;
    const bool shines = obj.effect_frames > 0;
    const int32_t count = arrangeObjectLayout(layout, hidden);

    // The frame's stream grows by this object's corners in ONE step. The C#
    // asks for room per batch; the answer is the same cursor, and one resize
    // keeps the value-initialisation that resize does (Vec3 has default member
    // initializers, so the new corners are zeroed before the writes below
    // overwrite them) to a single contiguous range instead of one per batch.
    // The vectors keep their capacity between frames, so a frame after the first
    // does not allocate at all (AGENTS.md 10).
    int32_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        total += objectBatchCorners(layout->batches[(size_t)layout->order[i]], hidden);
    }
    if (total == 0) {
        return true;
    }
    size_t at = stream->vertices.size();
    stream->vertices.resize(at + (size_t)total);

    for (int32_t i = 0; i < count; i++) {
        const ObjectBatch& batch = layout->batches[(size_t)layout->order[i]];
        const int32_t corners = objectBatchCorners(batch, hidden);
        if (corners == 0) {
            // The C#'s `if (vertices.IsEmpty) continue;` -- a batch whose every
            // part is hidden draws nothing, and the stream does not move.
            continue;
        }

        const int32_t first = (int32_t)at;
        size_t slot = 0;
        for (size_t c = 0; c < batch.parts.size(); c++) {
            const int32_t part = batch.parts[c];
            if (objectPartHidden(hidden, part)) {
                continue;
            }
            const std::vector<formats::Vec3f>& vertices = pose[(size_t)part];
            const int32_t index = batch.indices[c];
            // The slot advances for every corner the batch keeps, exactly as the
            // C#'s `at++` does; only the write is conditional, and only for the
            // stale-pose case the guards above exist for.
            if (index >= 0 && (size_t)index < vertices.size()) {
                const formats::Vec3f& position = vertices[(size_t)index];
                stream->vertices[(size_t)(first + slot)] =
                    Vertex{Vec3{position.x, position.y, position.z}, batch.uvs[c], kVertexWhite};
            }
            slot++;
        }
        at += (size_t)corners;

        ObjectStreamDraw draw;
        draw.first = first;
        draw.corners = corners;
        draw.surface = shineSurface(batch.surface, shines);
        draw.frame = frame;
        draw.world = transform;
        stream->draws.push_back(draw);
    }

    return true;
}

} // namespace mdk
