// stream_view.cpp — see stream_view.h. C# src/Mdk.Game/Stream/StreamView.cs,
// method by method.

#include "stream/stream_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "render/camera_builder.h"

namespace mdk {
namespace stream {

namespace {

// C# StreamView's constants.
constexpr float kViewHeight = 360.0f;
constexpr float kFocal = 250.0f;
constexpr float kNear = 0.05f;
constexpr float kFar = 1000.0f;
// ViewOf's vertical field of view, degrees: 2 * atan(kViewHeight / 2 / kFocal),
// the float 0x428F03FB -- glibc's atanf, which matches .NET's MathF.Atan, times
// 180 / pi as floats (the C# oracle's 10 s frame agrees with this value and not
// with the correctly rounded 0x428F03FC: 45 differing pixels against 68).
constexpr float kFieldOfView = 71.5077744f;
// The background quad stays this far ahead, behind everything in the tube.
constexpr float kBackgroundDistance = 900.0f;
// Sprites are drawn a quarter of their size (lights), the planet half.
constexpr float kLightScale = 0.25f;
constexpr float kPlanetScale = 0.5f;
constexpr int32_t kSpriteCapacity = 256;
constexpr int32_t kQuadVertices = 6;
constexpr int32_t kTubeCapacity = StreamTube::kRings * StreamTube::kPoints * 2 * 3;
// The health box (0x420830): SC_STAT at the bottom right, SNIP_TXT digits,
// blinking at 20 or less.
constexpr float kPanelRight = 16.0f;
constexpr float kPanelBottom = 10.0f;
constexpr int32_t kDigitWidth = 8;
constexpr int32_t kMaxNumber = 999;
constexpr int32_t kLowHealth = 20;
constexpr int32_t kBlinkOn = 16;
// The background quad cut at BG's edges: the canvas is at most two images wide
// (the widest aspect a frontend draws is well under 1200 / 360), so a row is at
// most three pieces, and the 360-high view two rows.
constexpr int32_t kBackPiecesX = 3;
constexpr int32_t kBackPiecesY = 2;
// The tube's colour table (no vertex colours): 64 ramp colours by 6 levels.
constexpr int32_t kTableWidth = StreamTube::kRampSize;
constexpr int32_t kTableHeight = 6;

// Vertex.Rgba: R in the low byte.
uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

mdk::Vec3 toRender(const formats::Vec3f& v) { return mdk::Vec3{v.x, v.y, v.z}; }

void fail(char* error, int32_t errorSize, const char* what, const char* name) {
    if (error && errorSize > 0) {
        snprintf(error, (size_t)errorSize, "%s %s", what, name);
    }
}

bool sameMaterial(const Material& a, const Material& b) {
    return a.texture == b.texture && a.palette == b.palette && a.frame_count == b.frame_count &&
           a.pass == b.pass && a.shading == b.shading && a.blend == b.blend && a.row_shift == b.row_shift &&
           a.colour.r == b.colour.r && a.colour.g == b.colour.g && a.colour.b == b.colour.b &&
           a.colour.a == b.colour.a;
}

} // namespace

// C# StreamView's constructor and Load.
bool StreamView::loadImage(const char* name, Image* out, char* error, int32_t errorSize) {
    formats::Texture texture;
    if (!bni_->image(name, &texture) || texture.width() <= 0 || texture.height() <= 0) {
        fail(error, errorSize, "STREAM.BNI has no image", name);
        return false;
    }
    out->texture = mdk_render_create_index_texture(renderer_, texture.width(), texture.height(),
                                                   texture.indices(), palette_->rgba());
    out->width = (float)texture.width();
    out->height = (float)texture.height();
    if (out->texture < 0) {
        fail(error, errorSize, "no GPU texture for", name);
        return false;
    }
    return true;
}

// C# ModelOf: Model.Parse(name, ..., Header.NoFlags, Parts.Named), and C# Build's
// triangle walk (the surfaces and UVs; the positions are the pose's, per frame).
bool StreamView::loadModel(const char* name, char* error, int32_t errorSize) {
    if (meshOf(name) != nullptr) {
        return true;
    }
    const formats::ArchiveIndex::Entry* entry = bni_->index().find(name);
    if (entry == nullptr) {
        fail(error, errorSize, "STREAM.BNI has no model", name);
        return false;
    }
    ModelMesh mesh;
    mesh.name = name;
    const std::vector<uint8_t>& bytes = bni_->bytes();
    if (!formats::Model::parse(&mesh.model, name, bytes.data(), (int32_t)bytes.size(), entry->offset,
                               formats::Model::Header::NoFlags, formats::Model::Parts::Named)) {
        fail(error, errorSize, "could not parse the model", name);
        return false;
    }
    std::vector<std::string> names;
    names.reserve(mesh.model.materials.size());
    for (size_t i = 0; i < mesh.model.materials.size(); i++) {
        names.push_back(mesh.model.materials[i].nameString());
    }
    for (int32_t p = 0; p < (int32_t)mesh.model.partList.size(); p++) {
        const formats::Model::Part& part = mesh.model.partList[(size_t)p];
        for (int32_t t = 0; t < (int32_t)part.triangleMaterials.size(); t++) {
            Surface surface;
            if (!resolver_.resolve(part.triangleMaterials[(size_t)t], names, *palette_, archives_, &surface,
                                   Pass::DoubleSided)) {
                continue;
            }
            // The UVs are in texels: scaled by the texture's size, zero for a flat colour.
            float scaleX = 0.0f;
            float scaleY = 0.0f;
            if (surface.texture != nullptr) {
                scaleX = 1.0f / (float)surface.texture->width();
                scaleY = 1.0f / (float)surface.texture->height();
            }
            const int32_t first = (int32_t)mesh.parts.size();
            for (int32_t k = 0; k < 3; k++) {
                const size_t corner = (size_t)(t * 3 + k);
                mesh.parts.push_back(p);
                mesh.indices.push_back(part.triangleIndices[corner]);
                mesh.uvs.push_back(Vec2{part.triangleUvs[corner].x * scaleX, part.triangleUvs[corner].y * scaleY});
            }

            // The material as object_draw.cpp makes it from a surface: the
            // texture (or the flat colour's one-pixel texture) through the
            // palette, white, the surface's pass and shading.
            Material material;
            material.texture = surfaces_.surfaceId(surface, palette_);
            material.palette = surfaces_.paletteId(palette_);
            material.frame_count = surface.frameCount;
            material.pass = surface.pass;
            material.shading = surface.shading;
            material.row_shift = surface.rowShift;
            material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};

            // Neighbouring triangles of one surface draw together.
            if (!mesh.batches.empty() && sameMaterial(mesh.batches.back().material, material)) {
                mesh.batches.back().count += 3;
                continue;
            }
            Batch batch;
            batch.first = first;
            batch.count = 3;
            batch.material = material;
            mesh.batches.push_back(batch);
        }
    }
    if ((int32_t)mesh.parts.size() > modelCapacity_) {
        modelCapacity_ = (int32_t)mesh.parts.size();
    }
    models_.push_back(mesh);
    return true;
}

// C# AnimationOf and Pose's Bake, for one (model, animation) pair.
bool StreamView::loadPose(const char* model, const char* animation, char* error, int32_t errorSize) {
    if (!loadModel(model, error, errorSize)) {
        return false;
    }
    if (poseOf(model, animation) != nullptr) {
        return true;
    }
    const formats::ModelAnimation* found = nullptr;
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].name == animation) {
            found = &animations_[i];
        }
    }
    if (found == nullptr) {
        const formats::ArchiveIndex::Entry* entry = bni_->index().find(animation);
        if (entry == nullptr) {
            fail(error, errorSize, "STREAM.BNI has no animation", animation);
            return false;
        }
        formats::ModelAnimation parsed;
        const std::vector<uint8_t>& bytes = bni_->bytes();
        if (!formats::ModelAnimation::parse(&parsed, animation, bytes.data(), (int32_t)bytes.size(),
                                            entry->offset)) {
            fail(error, errorSize, "could not parse the animation", animation);
            return false;
        }
        animations_.push_back(parsed);
        found = &animations_.back();
    }
    Pose pose;
    pose.model = model;
    pose.animation = animation;
    pose.frames = found->bake(meshOf(model)->model);
    if (pose.frames.empty()) {
        fail(error, errorSize, "the animation has no frames:", animation);
        return false;
    }
    poses_.push_back(pose);
    return true;
}

const StreamView::ModelMesh* StreamView::meshOf(const std::string& name) const {
    for (size_t i = 0; i < models_.size(); i++) {
        if (models_[i].name == name) {
            return &models_[i];
        }
    }
    return nullptr;
}

const StreamView::Pose* StreamView::poseOf(const std::string& model, const std::string& animation) const {
    for (size_t i = 0; i < poses_.size(); i++) {
        if (poses_[i].model == model && poses_[i].animation == animation) {
            return &poses_[i];
        }
    }
    return nullptr;
}

bool StreamView::load(MdkRenderer renderer, const formats::Bni* bni, const formats::Palette* palette,
                      const formats::TextureArchive* archive, StreamTube::Kind kind, char* error,
                      int32_t errorSize) {
    renderer_ = renderer;
    bni_ = bni;
    palette_ = palette;
    archives_.assign(1, archive);
    // C# `new MaterialResolver(renderer, new Dti())`: no level, so no glass.
    dti_ = formats::Dti();
    resolver_ = MaterialResolver(&dti_);
    surfaces_ = SurfaceCache(renderer);
    paletteId_ = surfaces_.paletteId(palette_);
    vertexColours_ = mdk_render_capabilities(renderer).vertex_colours;
    // C# StreamScreen: (GUNTA, GUNTANIM) after LEVEL8, else (SWH150, SWHANM).
    otherModel_ = kind == StreamTube::Gunter ? "GUNTA" : "SWH150";
    otherAnimation_ = kind == StreamTube::Gunter ? "GUNTANIM" : "SWHANM";

    if (!loadImage("BG", &background_, error, errorSize) || !loadImage("LIGHT", &light_, error, errorSize) ||
        !loadImage("PLANET", &planet_, error, errorSize) || !loadImage("SC_STAT", &panel_, error, errorSize) ||
        !loadImage("SNIP_TXT", &digits_, error, errorSize)) {
        return false;
    }

    // Every pose Draw can ask for: Kurt flying (KURTANIM) and rescued
    // (BONESANIM), Bones (BONESANIM), and the other.
    models_.reserve(3);
    poses_.reserve(4);
    animations_.reserve(3);
    if (!loadPose("KURT", "KURTANIM", error, errorSize) || !loadPose("KURT", "BONESANIM", error, errorSize) ||
        !loadPose("BONES", "BONESANIM", error, errorSize) ||
        !loadPose(otherModel_, otherAnimation_, error, errorSize)) {
        return false;
    }

    tubeMesh_ = mdk_render_create_dynamic_mesh(renderer, kTubeCapacity);
    spriteMesh_ = mdk_render_create_dynamic_mesh(renderer, kSpriteCapacity * kQuadVertices);
    backMesh_ = mdk_render_create_dynamic_mesh(renderer, kBackPiecesX * kBackPiecesY * kQuadVertices);
    for (int32_t s = 0; s < kSlotCount; s++) {
        modelMeshes_[s] = mdk_render_create_dynamic_mesh(renderer, modelCapacity_);
    }
    if (tubeMesh_ < 0 || spriteMesh_ < 0 || backMesh_ < 0 || modelMeshes_[KurtSlot] < 0 ||
        modelMeshes_[BonesSlot] < 0 || modelMeshes_[OtherSlot] < 0) {
        fail(error, errorSize, "no GPU memory for the", "stream's meshes");
        return false;
    }

    // The frame's scratch, at its largest: the tube's capacity covers every
    // vertex list a frame builds (sprites 1536, the models' corners).
    size_t largest = (size_t)kTubeCapacity;
    if ((size_t)modelCapacity_ > largest) {
        largest = (size_t)modelCapacity_;
    }
    vertices_.reserve(largest);
    tubeVertices_.reserve((size_t)StreamTube::kPlanesPerSegment * 3);
    return true;
}

// The tube's colour table (no vertex colours): texel (colour, level) is
// StreamTube::rgba's answer for that pair -- the ramp colour with the level's
// alpha, both fixed tables of the tube.
bool StreamView::loadTubeTable(const StreamTube& tube, char* error, int32_t errorSize) {
    if (vertexColours_) {
        return true;
    }
    uint8_t table[kTableWidth * kTableHeight * 4];
    for (int32_t level = 0; level < kTableHeight; level++) {
        for (int32_t colour = 0; colour < kTableWidth; colour++) {
            TubeVertex v;
            v.colour = colour;
            v.level = level;
            const Rgba8 c = tube.rgba(v);
            uint8_t* texel = table + (level * kTableWidth + colour) * 4;
            texel[0] = c.r;
            texel[1] = c.g;
            texel[2] = c.b;
            texel[3] = c.a;
        }
    }
    tubeTable_ = mdk_render_create_rgba_texture(renderer_, kTableWidth, kTableHeight, table);
    if (tubeTable_ < 0) {
        fail(error, errorSize, "no GPU texture for the", "tube's colours");
        return false;
    }
    return true;
}

bool StreamView::clipOf(const char* animation, Clip* out) const {
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].name == animation) {
            out->frameCount = animations_[i].frameCount;
            out->speed = animations_[i].speed;
            return true;
        }
    }
    return false;
}

// C# ViewOf: CameraMath.View(eye, forward, up, fov, aspect, Near, Far).
void StreamView::camera(const Flight& flight, Camera* out) const {
    // C# `float.RadiansToDegrees(2f * MathF.Atan(ViewHeight / 2f / Focal))`, a
    // constant: 2 * atan(0.72) in degrees is the float 0x428F03FB (no libm call per
    // frame, AGENTS.md R6).
    const float fieldOfView = kFieldOfView;
    const float aspect = mdk_render_canvas_width(renderer_) / kCanvasHeight;
    const Vec3 eye = flight.eye();
    const Vec3 forward = flight.forward();
    buildLookAtCamera(out, toRender(eye), mdk::Vec3{eye.x + forward.x, eye.y + forward.y, eye.z + forward.z},
                      toRender(flight.up()), fieldOfView, aspect, kNear, kFar);
}

// C# Draw.
void StreamView::draw(const Flight& flight) {
    drawBackground(flight);
    // C# StreamScreen.Frame: `_flight.Bones == null ? KurtAnimation : RescueAnimation`.
    drawModel(KurtSlot, "KURT", flight.hasBones() ? "BONESANIM" : "KURTANIM", flight.kurtFrame(),
              flight.kurtTransform());
    if (flight.hasBones()) {
        drawModel(BonesSlot, "BONES", "BONESANIM", flight.bonesFrame(), flight.kurtTransform());
    }
    if (flight.hasOther()) {
        drawModel(OtherSlot, otherModel_, otherAnimation_, flight.otherFrame(flight.other()),
                  flight.transform(flight.other()));
    }
    drawTube(flight);
    drawSprites(flight);
    mdk_render_canvas_begin(renderer_);
    drawHealth(flight.health(), flight.blink());
    drawFade(flight.fade());
    mdk_render_canvas_end(renderer_);
}

// C# DrawBackground: one BG pixel per canvas unit, as a quad far ahead filling
// the view -- cut at BG's edges (see the header), each piece's corners where
// the uncut quad's interpolation puts them.
void StreamView::drawBackground(const Flight& flight) {
    const float width = mdk_render_canvas_width(renderer_);
    const float unit = kBackgroundDistance / kFocal;
    const Vec3 eye = flight.eye();
    const Vec3 forward = flight.forward();
    const Vec3 centre = nAdd(eye, nScale(forward, kBackgroundDistance));
    const Vec3 right = nScale(flight.right(), width / 2.0f * unit);
    const Vec3 up = nScale(flight.up(), kViewHeight / 2.0f * unit);
    const Vec2 offset = flight.backgroundOffset();
    // BG's size in texels, as floats (the divides below are float divides).
    const float imageWidth = background_.width;
    const float imageHeight = background_.height;

    vertices_.clear();
    // The pieces: texel spans [x0, x1) x [y0, y1) of the scrolled view, cut where
    // a whole image ends. Which images the span touches is counted in integers
    // first (AGENTS.md T2: no float in a loop's condition).
    const float xEnd = offset.x + width;
    const float yEnd = offset.y + kViewHeight;
    const int32_t firstColumn = (int32_t)floorf(offset.x / imageWidth);
    const int32_t firstRow = (int32_t)floorf(offset.y / imageHeight);
    int32_t columns = (int32_t)ceilf(xEnd / imageWidth) - firstColumn;
    int32_t rows = (int32_t)ceilf(yEnd / imageHeight) - firstRow;
    columns = columns < 1 ? 1 : (columns > kBackPiecesX ? kBackPiecesX : columns);
    rows = rows < 1 ? 1 : (rows > kBackPiecesY ? kBackPiecesY : rows);
    for (int32_t row = 0; row < rows; row++) {
        const float imageTop = (float)(firstRow + row) * imageHeight;
        const float y0 = row == 0 ? offset.y : imageTop;
        const float y1 = imageTop + imageHeight < yEnd ? imageTop + imageHeight : yEnd;
        for (int32_t column = 0; column < columns; column++) {
            const float imageLeft = (float)(firstColumn + column) * imageWidth;
            const float x0 = column == 0 ? offset.x : imageLeft;
            const float x1 = imageLeft + imageWidth < xEnd ? imageLeft + imageWidth : xEnd;
            // The corners: -1..1 across the view, top (+up) to bottom (-up).
            const float left = (x0 - offset.x) / width * 2.0f - 1.0f;
            const float rightEdge = (x1 - offset.x) / width * 2.0f - 1.0f;
            const float top = 1.0f - (y0 - offset.y) / kViewHeight * 2.0f;
            const float bottom = 1.0f - (y1 - offset.y) / kViewHeight * 2.0f;
            const Vec3 tl = nAdd(nAdd(centre, nScale(right, left)), nScale(up, top));
            const Vec3 tr = nAdd(nAdd(centre, nScale(right, rightEdge)), nScale(up, top));
            const Vec3 br = nAdd(nAdd(centre, nScale(right, rightEdge)), nScale(up, bottom));
            const Vec3 bl = nAdd(nAdd(centre, nScale(right, left)), nScale(up, bottom));
            const Vec2 uv0{(x0 - imageLeft) / imageWidth, (y0 - imageTop) / imageHeight};
            const Vec2 uv1{(x1 - imageLeft) / imageWidth, (y1 - imageTop) / imageHeight};
            addQuad(tl, tr, br, bl, uv0, uv1, kVertexWhite);
        }
    }
    if (vertices_.empty()) {
        return;
    }
    mdk_render_update_mesh(renderer_, backMesh_, vertices_.data(), (int32_t)vertices_.size());
    MeshDraw draw;
    draw.mesh = backMesh_;
    draw.first_vertex = 0;
    draw.vertex_count = (int32_t)vertices_.size();
    draw.material.texture = background_.texture;
    draw.material.palette = paletteId_;
    draw.material.pass = Pass::DoubleSided;
    const Mat4 identity = Mat4::identity();
    memcpy(draw.world, identity.m, sizeof(draw.world));
    mdk_render_draw_mesh(renderer_, &draw);
}

// C# AddQuad: a quad from its top-left corner clockwise, textured from uv0 (top
// left) to uv1.
void StreamView::addQuad(Vec3 topLeft, Vec3 topRight, Vec3 bottomRight, Vec3 bottomLeft, Vec2 uv0, Vec2 uv1,
                         uint32_t colour) {
    const mdk::Vec2 a{uv0.x, uv0.y};
    const mdk::Vec2 b{uv1.x, uv0.y};
    const mdk::Vec2 c{uv1.x, uv1.y};
    const mdk::Vec2 d{uv0.x, uv1.y};
    vertices_.push_back(Vertex{toRender(topLeft), a, colour});
    vertices_.push_back(Vertex{toRender(topRight), b, colour});
    vertices_.push_back(Vertex{toRender(bottomRight), c, colour});
    vertices_.push_back(Vertex{toRender(topLeft), a, colour});
    vertices_.push_back(Vertex{toRender(bottomRight), c, colour});
    vertices_.push_back(Vertex{toRender(bottomLeft), d, colour});
}

// C# DrawTube: the segments far to near in one draw, each vertex with its RGBA
// (or, without vertex colours, its texel of the colour table).
void StreamView::drawTube(const Flight& flight) {
    const StreamTube& tube = flight.tube();
    const Vec3 eye = flight.eye();
    vertices_.clear();
    for (int32_t n = tube.firstDrawn(); n >= tube.tail(); n--) {
        tubeVertices_.clear();
        tube.addTriangles(n, eye, &tubeVertices_);
        for (size_t i = 0; i < tubeVertices_.size(); i++) {
            const TubeVertex& v = tubeVertices_[i];
            if ((int32_t)vertices_.size() >= kTubeCapacity) {
                break;
            }
            Vertex out;
            out.pos = toRender(v.position);
            if (vertexColours_) {
                const Rgba8 c = tube.rgba(v);
                out.uv = mdk::Vec2{0.0f, 0.0f};
                out.colour = packRgba(c.r, c.g, c.b, c.a);
            } else {
                out.uv = mdk::Vec2{((float)v.colour + 0.5f) / (float)kTableWidth,
                                   ((float)v.level + 0.5f) / (float)kTableHeight};
                out.colour = kVertexWhite;
            }
            vertices_.push_back(out);
        }
    }
    if (vertices_.empty()) {
        return;
    }
    mdk_render_update_mesh(renderer_, tubeMesh_, vertices_.data(), (int32_t)vertices_.size());
    MeshDraw draw;
    draw.mesh = tubeMesh_;
    draw.first_vertex = 0;
    draw.vertex_count = (int32_t)vertices_.size();
    // C# Material.Flat(Vector4.One, Pass.Blended).
    draw.material.texture = vertexColours_ ? -1 : tubeTable_;
    draw.material.palette = -1;
    draw.material.pass = Pass::Blended;
    const Mat4 identity = Mat4::identity();
    memcpy(draw.world, identity.m, sizeof(draw.world));
    mdk_render_draw_mesh(renderer_, &draw);
}

// C# DrawSprites: the planet and the lights, billboards facing the camera.
void StreamView::drawSprites(const Flight& flight) {
    vertices_.clear();
    const int32_t planets = addSprites(flight, StreamTube::Planet, kPlanetScale);
    const int32_t lights = addSprites(flight, StreamTube::Light, kLightScale);
    if (vertices_.empty()) {
        return;
    }
    mdk_render_update_mesh(renderer_, spriteMesh_, vertices_.data(), (int32_t)vertices_.size());
    const Mat4 identity = Mat4::identity();
    if (planets > 0) {
        MeshDraw draw;
        draw.mesh = spriteMesh_;
        draw.first_vertex = 0;
        draw.vertex_count = planets;
        draw.material.texture = planet_.texture;
        draw.material.palette = paletteId_;
        draw.material.pass = Pass::DoubleSided;
        memcpy(draw.world, identity.m, sizeof(draw.world));
        mdk_render_draw_mesh(renderer_, &draw);
    }
    if (lights > 0) {
        // The lights glow, added to what's behind (as stream.gd; the original's
        // blend isn't known).
        MeshDraw draw;
        draw.mesh = spriteMesh_;
        draw.first_vertex = planets;
        draw.vertex_count = lights;
        draw.material.texture = light_.texture;
        draw.material.palette = paletteId_;
        draw.material.pass = Pass::Blended;
        draw.material.blend = Blend::Add;
        memcpy(draw.world, identity.m, sizeof(draw.world));
        mdk_render_draw_mesh(renderer_, &draw);
    }
}

// C# AddSprites: the sprites of a kind; returns their vertex count.
int32_t StreamView::addSprites(const Flight& flight, StreamTube::SpawnKind kind, float scale) {
    const int32_t first = (int32_t)vertices_.size();
    const std::vector<Sprite>& sprites = flight.sprites();
    for (size_t i = 0; i < sprites.size(); i++) {
        const Sprite& sprite = sprites[i];
        if (sprite.spawn.kind != kind || (int32_t)vertices_.size() >= kSpriteCapacity * kQuadVertices) {
            continue;
        }
        const Vec3 centre = flight.tube().place(sprite.thing.t, sprite.thing.x, sprite.thing.z);
        const float half = sprite.spawn.size * scale / 2.0f;
        const Vec3 right = nScale(flight.right(), half);
        const Vec3 up = nScale(flight.up(), half);
        addQuad(nAdd(nSub(centre, right), up), nAdd(nAdd(centre, right), up), nSub(nAdd(centre, right), up),
                nSub(nSub(centre, right), up), Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, kVertexWhite);
    }
    return (int32_t)vertices_.size() - first;
}

// C# DrawModel: a model in a pose (frame clamped to the baked frames, C# Pose),
// its slot's mesh rewritten with the pose's corners, then one draw per batch.
void StreamView::drawModel(Slot slot, const char* name, const char* animation, int32_t frame,
                           const Mat4& transform) {
    const ModelMesh* mesh = meshOf(name);
    const Pose* pose = poseOf(name, animation);
    if (mesh == nullptr || pose == nullptr || mesh->parts.empty()) {
        return;
    }
    const int32_t last = (int32_t)pose->frames.size() - 1;
    const int32_t at = frame < 0 ? 0 : (frame > last ? last : frame);
    const std::vector<std::vector<formats::Vec3f>>& parts = pose->frames[(size_t)at];
    vertices_.clear();
    for (size_t i = 0; i < mesh->parts.size(); i++) {
        const std::vector<formats::Vec3f>& part = parts[(size_t)mesh->parts[i]];
        const int32_t index = mesh->indices[i];
        Vertex v;
        v.pos = (index >= 0 && index < (int32_t)part.size()) ? toRender(part[(size_t)index]) : mdk::Vec3{};
        v.uv = mdk::Vec2{mesh->uvs[i].x, mesh->uvs[i].y};
        v.colour = kVertexWhite;
        vertices_.push_back(v);
    }
    mdk_render_update_mesh(renderer_, modelMeshes_[slot], vertices_.data(), (int32_t)vertices_.size());
    for (size_t b = 0; b < mesh->batches.size(); b++) {
        MeshDraw draw;
        draw.mesh = modelMeshes_[slot];
        draw.first_vertex = mesh->batches[b].first;
        draw.vertex_count = mesh->batches[b].count;
        draw.material = mesh->batches[b].material;
        draw.frame = 0;
        memcpy(draw.world, transform.m, sizeof(draw.world));
        mdk_render_draw_mesh(renderer_, &draw);
    }
}

// C# DrawHealth: the health box at the bottom right; the number blinks at 20 or less.
void StreamView::drawHealth(int32_t health, int32_t blink) {
    const float width = mdk_render_canvas_width(renderer_);
    const float atX = width - (panel_.width + kPanelRight);
    const float atY = kCanvasHeight - (panel_.height + kPanelBottom);
    drawImage(panel_, 0.0f, 0.0f, panel_.width, panel_.height, atX, atY, panel_.width, panel_.height);
    if (health <= kLowHealth && blink >= kBlinkOn) {
        return;
    }

    // A number centred on the panel, 8-pixel digits (0x420bd0).
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)(health < kMaxNumber ? health : kMaxNumber));
    const int32_t length = (int32_t)strlen(text);
    float x = atX + (float)((int32_t)panel_.width >> 1) - (float)kDigitWidth / 2.0f * (float)length;
    const float y = atY + (float)(((int32_t)panel_.height - (int32_t)digits_.height) >> 1);
    for (int32_t i = 0; i < length; i++) {
        const float sourceX = (float)((text[i] - '0') * kDigitWidth);
        drawImage(digits_, sourceX, 0.0f, (float)kDigitWidth, digits_.height, x, y, (float)kDigitWidth,
                  digits_.height);
        x += (float)kDigitWidth;
    }
}

// C# Renderer.DrawImage: part of an index texture (source in its pixels) on the canvas.
void StreamView::drawImage(const Image& image, float sx, float sy, float sw, float sh, float tx, float ty,
                           float tw, float th) {
    const float u0 = sx / image.width;
    const float v0 = sy / image.height;
    const float u1 = (sx + sw) / image.width;
    const float v1 = (sy + sh) / image.height;
    CanvasQuad q;
    q.x0 = tx;      q.y0 = ty;
    q.x1 = tx + tw; q.y1 = ty;
    q.x2 = tx + tw; q.y2 = ty + th;
    q.x3 = tx;      q.y3 = ty + th;
    q.u0 = u0; q.v0 = v0;
    q.u1 = u1; q.v1 = v0;
    q.u2 = u1; q.v2 = v1;
    q.u3 = u0; q.v3 = v1;
    q.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
    q.texture = image.texture;
    q.palette = paletteId_;
    mdk_render_canvas_quad(renderer_, &q);
}

// C# Renderer.FillRect / AddRect.
void StreamView::fillRect(float x, float y, float w, float h, Colour colour, bool additive) {
    CanvasQuad q;
    q.x0 = x;     q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w; q.y2 = y + h;
    q.x3 = x;     q.y3 = y + h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = colour;
    q.texture = -1;
    q.palette = -1;
    q.additive = additive;
    mdk_render_canvas_quad(renderer_, &q);
}

// C# DrawFade: the fade over everything -- red added (0x4352ac: R + red, green
// and blue kept), then towards white, then darkened.
void StreamView::drawFade(Fade fade) {
    const float width = mdk_render_canvas_width(renderer_);
    if (fade.red > 0.0f) {
        fillRect(0.0f, 0.0f, width, kCanvasHeight, Colour{1.0f, 0.0f, 0.0f, fade.red}, true);
    }
    if (fade.whiten < 1.0f) {
        fillRect(0.0f, 0.0f, width, kCanvasHeight, Colour{1.0f, 1.0f, 1.0f, 1.0f - fade.whiten}, false);
    }
    if (fade.dark < 1.0f) {
        fillRect(0.0f, 0.0f, width, kCanvasHeight, Colour{0.0f, 0.0f, 0.0f, 1.0f - fade.dark}, false);
    }
}

} // namespace stream
} // namespace mdk
