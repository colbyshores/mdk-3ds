// ============================================================================
// object_view.cpp — see object_view.h.
// ============================================================================

#include "game/object_view.h"

#include <algorithm>
#include <math.h>
#include <string>
#include <vector>
#include <string.h>

#include "formats/arena.h"
#include "formats/cmi.h"
#include "formats/model.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "formats/vector_types.h"
#include "game/mod_models.h"

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

namespace {

// A swap's surfaces through a look, resolved once per layout (the C#'s
// SurfacesOf): image, else named material, else flat colour; plus white
// at the end for triangles with no material.
struct SwapSurfaces {
    std::vector<Surface> surfaces;
    std::vector<const HdImage*> images;
};

void resolveSwapSurfaces(const ModelSwap& swap, const formats::Model& model,
                         const ObjectLook& look, const MaterialResolver& resolver,
                         SwapSurfaces* out) {
    const std::vector<SwapSurface>& from = swap.surfaces();
    for (size_t i = 0; i < from.size(); i++) {
        const SwapSurface& s = from[i];
        Surface surface;
        const HdImage* image = nullptr;
        if (s.hasImage) {
            resolver.resolveFlat(s.colour, &surface, Pass::DoubleSided);
            image = &s.image;
        } else if (!resolver.resolve(0, std::vector<std::string>{s.name}, *look.palette,
                                     look.archives, &surface, Pass::DoubleSided)) {
            (void)model;
            resolver.resolveFlat(s.colour, &surface, Pass::DoubleSided);
        }
        out->surfaces.push_back(surface);
        out->images.push_back(image);
    }
    Surface white;
    resolver.resolveFlat(Colour{1.0f, 1.0f, 1.0f, 1.0f}, &white, Pass::DoubleSided);
    out->surfaces.push_back(white);
    out->images.push_back(nullptr);
}

} // namespace

// The model's material names, in its own order: what the resolver is handed
// (the C#'s model.Materials). One helper for the layout and the warm.
static void modelMaterialNames(const formats::Model& model, std::vector<std::string>* names) {
    names->clear();
    names->reserve(model.materials.size());
    for (size_t i = 0; i < model.materials.size(); i++) {
        names->push_back(model.materials[i].nameString());
    }
}

void objectModelSurfaces(const formats::Model& model, const ObjectLook& look,
                         const MaterialResolver& resolver, std::vector<Surface>* out) {
    out->clear();
    if (look.palette == nullptr) {
        return;
    }
    std::vector<std::string> names;
    modelMaterialNames(model, &names);
    // Each distinct triangle material once (a model has a handful).
    std::vector<int32_t> seen;
    for (size_t p = 0; p < model.partList.size(); p++) {
        const std::vector<int32_t>& materials = model.partList[p].triangleMaterials;
        for (size_t t = 0; t < materials.size(); t++) {
            if (std::find(seen.begin(), seen.end(), materials[t]) != seen.end()) {
                continue;
            }
            seen.push_back(materials[t]);
            Surface surface;
            if (resolver.resolve(materials[t], names, *look.palette, look.archives, &surface,
                                 Pass::DoubleSided)) {
                out->push_back(surface);
            }
        }
    }
}

bool warmObjectStep(const formats::Arena& arena, const formats::Cmi* cmi,
                    const ObjectLook& look, const MaterialResolver& resolver,
                    ObjectSurfaceFn start, void* user, size_t* cursor) {
    const size_t own = (size_t)arena.modelCount();
    const size_t global = cmi ? cmi->modelOffsets().size() : 0;
    if (start == nullptr || cursor == nullptr || *cursor >= own + global) {
        return true;
    }
    const size_t at = (*cursor)++;
    formats::Model parsed;
    const formats::Model* model = nullptr;
    if (at < own) {
        model = arena.model(arena.modelName((int32_t)at));
    } else {
        // A global model, parsed as Cmi::model parses it but not kept (that
        // cache, and the scripts' own, hold only the types actually spawned).
        const std::pair<std::string, int32_t>& entry = cmi->modelOffsets()[at - own];
        if (entry.second != 0 &&
            formats::Model::parse(&parsed, entry.first, cmi->bytes().data(),
                                  (int32_t)cmi->bytes().size(), entry.second)) {
            model = &parsed;
        }
    }
    if (model != nullptr) {
        std::vector<Surface> surfaces;
        objectModelSurfaces(*model, look, resolver, &surfaces);
        for (size_t i = 0; i < surfaces.size(); i++) {
            start(user, surfaces[i], look.palette);
        }
    }
    return *cursor >= own + global;
}

bool buildObjectLayout(const formats::Model& model, const ObjectLook& look,
                       const MaterialResolver& resolver, ObjectLayout* out,
                       const ModelSwap* swap) {
    if (out == nullptr || look.palette == nullptr) {
        return false;
    }
    out->batches.clear();
    out->order.clear();
    out->keys.clear();

    const int32_t parts = (int32_t)model.partList.size();

    // The largest part's bounds diagonal, for the object cull's sweep
    // inflation (a posed part can swing that far past the rest bounds).
    out->partSweep = 0.0f;
    for (int32_t p = 0; p < parts; p++) {
        const formats::Box& partBounds = model.partList[(size_t)p].bounds;
        const float pdx = partBounds.max.x - partBounds.min.x;
        const float pdy = partBounds.max.y - partBounds.min.y;
        const float pdz = partBounds.max.z - partBounds.min.z;
        const float diagonal = sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
        if (diagonal > out->partSweep) {
            out->partSweep = diagonal;
        }
    }

    // The model's material names, which is what the C# passes as
    // model.Materials.
    std::vector<std::string> names;
    modelMaterialNames(model, &names);

    SwapSurfaces swapSurfaces;
    const bool hasSwapSurfaces = swap != nullptr;
    if (hasSwapSurfaces) {
        resolveSwapSurfaces(*swap, model, look, resolver, &swapSurfaces);
    }

    int32_t sequence = 0;
    for (int32_t p = 0; p < parts; p++) {
        const formats::Model::Part& part = model.partList[(size_t)p];
        // A replaced part draws the swap's mesh in its place (the C#'s
        // AddSwapped): batches keyed by resolved surface + image, in the
        // part's own place among the model's triangles.
        if (swap != nullptr && swap->part(p) != nullptr) {
            const SwapMesh& mesh = *swap->part(p);
            for (size_t t = 0; t < mesh.materials.size(); t++) {
                const int32_t index = mesh.materials[t];
                const size_t pick =
                    (index >= 0 && (size_t)index < swapSurfaces.surfaces.size() - 1)
                        ? (size_t)index
                        : swapSurfaces.surfaces.size() - 1;
                const Surface& surface = swapSurfaces.surfaces[pick];
                const HdImage* image = swapSurfaces.images[pick];

                size_t batchIndex = 0;
                for (; batchIndex < out->batches.size(); batchIndex++) {
                    const ObjectBatch& existing = out->batches[batchIndex];
                    if (existing.swapped && existing.image == image &&
                        sameSurface(existing.surface, surface)) {
                        break;
                    }
                }
                if (batchIndex == out->batches.size()) {
                    ObjectBatch batch;
                    batch.surface = surface;
                    batch.swapped = true;
                    batch.image = image;
                    batch.partCorners.assign((size_t)parts, 0);
                    batch.firstTriangle.assign((size_t)parts, kObjectUnseen);
                    out->batches.push_back(batch);
                }
                ObjectBatch& batch = out->batches[batchIndex];
                if (sequence < batch.firstTriangle[(size_t)p]) {
                    batch.firstTriangle[(size_t)p] = sequence;
                }
                batch.partCorners[(size_t)p] += kTriangleVertices;
                for (int32_t k = 0; k < kTriangleVertices; k++) {
                    const int32_t vertex = mesh.triangles[t * (size_t)kTriangleVertices + (size_t)k];
                    batch.parts.push_back(p);
                    batch.indices.push_back(vertex);
                    batch.uvs.push_back(mesh.uvs[(size_t)vertex]);
                }
            }
            sequence += (int32_t)part.triangleMaterials.size();
            continue;
        }
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

void ObjectView::layoutSphere(const scripts::MdkObject& obj, const ObjectLayout& layout,
                              const Mat4& transform, Vec3* center, float* radius) {
    const formats::Box& bounds = obj.model->bounds;
    const float local[3] = {(bounds.min.x + bounds.max.x) * 0.5f,
                            (bounds.min.y + bounds.max.y) * 0.5f,
                            (bounds.min.z + bounds.max.z) * 0.5f};
    const float dx = bounds.max.x - bounds.min.x;
    const float dy = bounds.max.y - bounds.min.y;
    const float dz = bounds.max.z - bounds.min.z;
    const float localRadius = sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
    const float* m = transform.m;
    // The Frobenius norm of the linear part: an upper bound on the
    // transform's largest scale for any composition, where the max
    // row length is only exact for a uniform scale after rotation.
    float frobenius = 0.0f;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            frobenius += m[i * 4 + j] * m[i * 4 + j];
        }
    }
    const float scale = sqrtf(frobenius);
    *center = Vec3{local[0] * m[0] + local[1] * m[4] + local[2] * m[8] + m[12],
                   local[0] * m[1] + local[1] * m[5] + local[2] * m[9] + m[13],
                   local[0] * m[2] + local[1] * m[6] + local[2] * m[10] + m[14]};
    *radius = (localRadius + layout.partSweep) * scale;
}

ObjectLayout* ObjectView::layoutOf(const formats::Model* model, const ObjectLook& look) {
    if (model == nullptr || look.palette == nullptr) {
        return nullptr;
    }
    // The archives' first entry is the arena's own archive, unique per arena:
    // it separates a corridor's look from the owner arena's now that they share
    // one palette object (LevelData::paletteOf). See the layouts_ comment.
    typedef std::pair<const formats::Model*,
                      std::pair<const formats::Palette*, const formats::TextureArchive*> >
        LayoutKey;
    const LayoutKey key(
        model, std::make_pair(look.palette,
                              look.archives.empty() ? nullptr : look.archives[0]));
    std::map<LayoutKey, std::unique_ptr<ObjectLayout> >::iterator found = layouts_.find(key);
    if (found != layouts_.end()) {
        return found->second.get();
    }
    std::unique_ptr<ObjectLayout> made(new ObjectLayout());
    {
        const ModelSwap* swap = swaps_ ? swaps_->of(*model) : nullptr;
        made->swap = swap;
        if (!buildObjectLayout(*model, look, *resolver_, made.get(), swap)) {
            // The C# caches the null too, so a model with nothing to draw is not
            // rebuilt every draw. The empty layout is dropped; the map keeps a null.
            made.reset();
        }
    }
    return layouts_.emplace(key, std::move(made)).first->second.get();
}

// The C#'s ObjectView.Draw, with the two renderer seam calls replaced by the
// frame's stream: `renderer.Stream(corners, out mesh, out first)` is the cursor
// bump at the end of `stream->vertices` -- including its refusal of a batch that
// does not fit in kObjectStreamCapacity, which leaves the cursor where it was --
// and `renderer.Draw(mesh, first, corners, material, frame, transform)` is the
// queued ObjectStreamDraw. The guards, PoseParts, Arrange, Visible, the per-part
// copy loop, Shine and the frame selection are the oracle's, in the oracle's order.
//
// The queued draw carries the surface, the look's PALETTE, the frame and the
// transform, which is all the C#'s Draw call is given: nothing a backend needs
// may be left in the caller.
bool ObjectView::addObject(const scripts::MdkObject& obj, const ObjectLook& look,
                           const Mat4& transform, ObjectStream* stream, uint8_t* tinyHidden,
                           float poseAlpha) {
    if (obj.model == nullptr || obj.dead || stream == nullptr) {
        return false;
    }
    ObjectLayout* layout = layoutOf(obj.model, look);
    if (layout == nullptr) {
        return false;
    }

    // The object cull (setCullFrustum): the model's bounds through this
    // object's transform, as a world sphere. One test per object, not
    // per corner: six plane distances and a comparison each -- the same
    // test the backend's per-eye batch cull makes per draw, here at
    // object granularity so the corners are never streamed.
    const bool tiny = tinyHidden != nullptr && tinyCamera_ != nullptr;
    if ((hasCullFrustum_ || tiny) && layout->swap == nullptr) {
        // (A swapped model's drawn geometry is the swap's, whose bounds
        // the model's do not describe: never culled.)
        Vec3 center;
        float radius = 0.0f;
        layoutSphere(obj, *layout, transform, &center, &radius);
        if (hasCullFrustum_ && frustumSphereOutside(cullFrustum_, center, radius + cullMargin_)) {
            return true;
        }
        // The tiny cull (setTinyCull), on the same sphere, with its hysteresis.
        if (tiny) {
            const float px = projectedPixelSize(*tinyCamera_, 2.0f * radius,
                                                sphereNearDepth(*tinyCamera_, center, radius),
                                                tinyWidth_, tinyHeight_);
            if (*tinyHidden != 0) {
                if (px > tinyShow_) {
                    *tinyHidden = 0;
                }
            } else if (px <= tinyHide_) {
                *tinyHidden = 1;
            }
            if (*tinyHidden != 0) {
                return true;
            }
        }
    }

    const std::vector<std::vector<formats::Vec3f> >* posed = &obj.pose_parts();
    // Between two frames (upstream ae746b5d, FrameBlend.PoseOf): each vertex
    // `share` of the way from the earlier frame to the later. Drawing only, so
    // the plain lerp (no fused multiply-add: VFPv2 has none, fmaf is a call).
    const std::vector<std::vector<formats::Vec3f> >* from = nullptr;
    const std::vector<std::vector<formats::Vec3f> >* to = nullptr;
    float share = 0.0f;
    if (poseAlpha < 1.0f && obj.frames_at(poseAlpha, &from, &to, &share)) {
        blendPose_.resize(to->size());
        for (size_t p = 0; p < to->size(); p++) {
            const std::vector<formats::Vec3f>& a = (*from)[p];
            const std::vector<formats::Vec3f>& b = (*to)[p];
            std::vector<formats::Vec3f>& out = blendPose_[p];
            const size_t n = a.size() < b.size() ? a.size() : b.size();
            out.resize(b.size());
            for (size_t v = 0; v < n; v++) {
                out[v].x = a[v].x + (b[v].x - a[v].x) * share;
                out[v].y = a[v].y + (b[v].y - a[v].y) * share;
                out[v].z = a[v].z + (b[v].z - a[v].z) * share;
            }
            for (size_t v = n; v < b.size(); v++) {
                out[v] = b[v];
            }
        }
        posed = &blendPose_;
    }
    const std::vector<std::vector<formats::Vec3f> >& pose = *posed;
    const int32_t hidden = obj.hidden_parts;

    // A pose that is not this model's is a stale pose cache (see
    // scripts/pose_cache.h and LevelData::load), and the C# would take an
    // IndexOutOfRange there. C++ has no exception to throw, so the object draws
    // nothing instead of reading past the pose; the layout is keyed by the same
    // model, so a part count that disagrees means the pose is not this model's.
    if (pose.size() != obj.model->partList.size()) {
        return false;
    }

    // A swapped model's replaced parts follow this pose (the C#'s
    // swap.Pose(pose, hidden)): fit them before any corner is streamed.
    if (layout->swap != nullptr) {
        // Only replaced parts are converted (the fit reads nothing else);
        // this is the modded path, never the shipped frame's hot loop.
        std::vector<std::vector<Vec3>> posed(pose.size());
        for (size_t p = 0; p < pose.size(); p++) {
            if (layout->swap->part((int32_t)p) == nullptr) {
                continue;
            }
            posed[p].reserve(pose[p].size());
            for (size_t v = 0; v < pose[p].size(); v++) {
                posed[p].push_back(Vec3{pose[p][v].x, pose[p][v].y, pose[p][v].z});
            }
        }
        layout->swap->pose(posed, hidden);
    }

    // The C#'s `TextureFrame >= 0 ? TextureFrame : 0` and its Glow.Shines test.
    const int32_t frame = obj.texture_frame >= 0 ? obj.texture_frame : 0;
    const bool shines = obj.effect_frames > 0;
    const int32_t count = arrangeObjectLayout(layout, hidden);

    // The frame's stream grows by this object's ACCEPTED corners in ONE step.
    // The C# asks for room per batch, and a batch that does not fit in the frame's
    // remaining stream is refused with the cursor unmoved -- so the accepted set
    // has to be walked per batch, and is walked here once to find the size and once
    // below to write it, with the same predicate both times. One resize keeps the
    // value-initialisation that resize does (Vec3 has default member initializers,
    // so the new corners are zeroed before the writes below overwrite them) to a
    // single contiguous range instead of one per batch. The vectors keep their
    // capacity between frames, so a frame after the first does not allocate at all
    // (AGENTS.md 10).
    const size_t start = stream->vertices.size();
    size_t room = start;
    int32_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        const int32_t corners =
            objectBatchCorners(layout->batches[(size_t)layout->order[i]], hidden);
        // The C#'s two refusals: a full stream (first + count > StreamVertices) and
        // an empty span (a batch whose every part is hidden). Both leave the cursor.
        if (corners == 0 || room + (size_t)corners > (size_t)kObjectStreamCapacity) {
            continue;
        }
        room += (size_t)corners;
        total += corners;
    }
    if (total == 0) {
        return true;
    }
    size_t at = start;
    stream->vertices.resize(at + (size_t)total);

    for (int32_t i = 0; i < count; i++) {
        const ObjectBatch& batch = layout->batches[(size_t)layout->order[i]];
        const int32_t corners = objectBatchCorners(batch, hidden);
        if (corners == 0 || at + (size_t)corners > (size_t)kObjectStreamCapacity) {
            // The C#'s empty span and its full-stream refusal: the batch draws
            // nothing and the stream does not move, so a later smaller batch fits.
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
            if (batch.swapped && layout->swap != nullptr && layout->swap->part(part) != nullptr) {
                const SwapMesh& mesh = *layout->swap->part(part);
                if (index >= 0 && (size_t)index < mesh.vertices.size()) {
                    stream->vertices[(size_t)(first + slot)] =
                        Vertex{layout->swap->vertex(part, index), batch.uvs[c], kVertexWhite};
                }
            } else if (index >= 0 && (size_t)index < vertices.size()) {
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
        draw.image = batch.image;
        draw.palette = look.palette;
        draw.frame = frame;
        draw.world = transform;
        draw.back = stream->back;
        stream->draws.push_back(draw);
    }

    return true;
}

} // namespace mdk
