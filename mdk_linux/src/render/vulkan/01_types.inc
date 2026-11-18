// 01_types.inc -- the push blocks, the resource records, the renderer state.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.


// The push constants. The layout is the GLSL push block's, in order:
// mat4 (64 bytes) + vec4 (16) + four ints (16) = 96 bytes, within the
// 128-byte minimum every implementation guarantees.
struct PushConstants {
    float mvp[16];
    float colour[4];
    int32_t textured;     // 0 flat, 1 index+palette, 2 RGBA
    int32_t frame_count;
    int32_t frame;
    int32_t unused_;
};

// The sky pipeline's push block (the same 96-byte range as PushConstants):
// the clip-to-direction matrix (column-vector form, the transpose of the
// row-vector view matrices) then the panorama uniforms.
struct SkyPush {
    float clip_to_direction[16];
    float wrap_width;
    float horizon_row;
    float offset;
    float height;
    int32_t top_colour;
    int32_t bottom_colour;
    float row_shift;
    int32_t sampling;
};

struct TextureVk {
    bool used = false;
    bool isIndex = false;
    int32_t width = 0;
    int32_t height = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    // The palette indices, kept for the enhanced look's colour expansion
    // (the C# keeps them in _indexPixels the same way). Bumped version on
    // every rewrite (bullet holes), so the colour cache can tell.
    std::vector<uint8_t> indices;
    uint32_t version = 0;
    // A 2D-array view of an RGBA texture's image, for the enhanced
    // sampler (which is an array sampler even for one-layer textures).
    VkImageView arrayView = VK_NULL_HANDLE;
};

struct PaletteVk {
    bool used = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* mapped = nullptr;
    // The palette's bytes, kept for the colour expansion.
    uint8_t rgba[1024] = {};
};

// One expanded colour texture (render/colour_mips): RGBA8, a mip chain,
// one array layer per frame. Built on demand for a (texture, palette,
// frames) triple and rebuilt when the index texture's version moves.
struct ColourVk {
    bool used = false;
    int32_t tex = -1;
    int32_t pal = -1;
    int32_t frames = 1;
    uint32_t version = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// A mod's whole colour image given instead of the palette expansion
// (mdk_render_replace_colour / _replace_image): premultiplied RGBA8, frames
// stacked downwards, kept until the index texture's version moves -- a
// changed texture shows its expansion again, exactly as the C#'s Changed
// drops _replaced. Canvas images (image=true) survive texture changes.
struct ReplacementVk {
    bool used = false;
    bool image = false;
    int32_t tex = -1;
    int32_t pal = -1;
    int32_t frames = 1;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t textureVersion = 0;
    std::vector<uint8_t> rgba;
};

// THE VERTEX BUFFER SLOTS, and why a dynamic mesh needs more than one.
//
// A STATIC mesh is written once, when it is made, before any frame has drawn
// with it, so one buffer is enough.
//
// A DYNAMIC mesh is rewritten every frame -- the object stream, Kurt's sprite,
// the canvas -- and the frame being recorded is not the only one the GPU may be
// working on. beginFrame waits the fence of the slot it is about to record, not
// the fence of the slot the previous frame used, so with two frames in flight
// the previous submission can still be reading a vertex buffer while this
// frame's CPU code is writing it. AGENTS.md P3 puts the completion boundary
// BEFORE CPU reuse, so a dynamic mesh has one buffer per frame in flight and
// writes the slot its own frame will draw from: the slot whose fence beginFrame
// just waited, which is the frame two back and never the one in front.
struct MeshVk {
    bool used = false;
    bool dynamic = false;
    VkBuffer buffer[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory memory[mdk::vk::kFramesInFlight] = {};
    void* mapped[mdk::vk::kFramesInFlight] = {};
    VkDeviceSize capacity = 0;   // bytes, per slot
};

// Which slot of a mesh this frame writes and draws. A static mesh always uses
// slot 0, which no frame is rewriting.
int meshSlot(const MeshVk& mesh, const mdk::vk::Context* ctx) {
    return mesh.dynamic ? (int)ctx->currentFrame : 0;
}

// A buffer whose last GPU use may still be running. It is destroyed at the
// frame boundary that proves the reading frame has finished; see `retiring`.
struct RetiredBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

// A descriptor set the cache has forgotten but a recorded frame may still name.
// It goes back to the pool at the same boundary a retired buffer waits for;
// see `retiringSets`.
struct RetiredSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

// An image, its view and its memory that a recorded frame may still be sampling.
// A descriptor set names a VIEW, so deferring only the set is half a fix: the
// handle would wait two frames while the resource it names was destroyed on the
// spot, and AGENTS.md P3 puts the completion boundary on the RESOURCE, not on
// the handle that refers to it. Settled on the same boundary as `retiringSets`;
// see `retiringImages`.
struct RetiredImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

void destroyImage(mdk::vk::Context* ctx, VkImage* image, VkDeviceMemory* memory,
                  VkImageView* view, void** mapped);

void destroyBufferPair(VkDevice device, VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
}

// A pipeline, keyed by (pass, blend, primitive).
struct PipelineVk {
    int key;
    VkPipeline pipeline;
};

// A descriptor set for one (texture, palette, wrap) triple.
struct DescPair {
    int32_t texture;
    int32_t palette;
    bool repeat;
    VkDescriptorSet set;
};

// Which resource a scope entry refers to, for mark/release.
struct ResRef {
    int kind;  // 0 texture, 1 palette, 2 mesh
    int32_t id;
};

struct RendererVk {
    mdk::vk::Context ctx;
    bool ready = false;
    bool inFrame = false;

    // The camera, kept so the draw path can use it.
    mdk::Camera camera;

    // The lighting rig. SHARED DATA: the Vulkan backend CONSUMES it (the
    // enhanced look); the 3DS backend ignores it (SPEC.md 5.3).
    mdk::Lighting lighting;
    // The Enhanced Mode feature toggles (SPEC.md 7.9): read once per
    // pass at setup (shadow pass, haze term, light buffer, post glow,
    // mirror draws), never per vertex or per pixel on the CPU.
    mdk::EnhancedToggles toggles;
    mdk::PointLight pointLights[64];
    int32_t pointLightCount = 0;

    mdk::Panorama panorama;
    float fade = 1.0f;
    float gamma = 1.0f;
    bool enhancedLook = false;

    // The gamma grade's objects (P6 post): a target the graded frame is
    // written into, sampled from the scene target, then presented instead
    // of it. Created on the first graded frame.
    VkImage gammaImage = VK_NULL_HANDLE;
    VkDeviceMemory gammaMemory = VK_NULL_HANDLE;
    VkImageView gammaView = VK_NULL_HANDLE;
    VkFramebuffer gammaFramebuffer = VK_NULL_HANDLE;
    uint32_t gammaWidth = 0;
    uint32_t gammaHeight = 0;
    VkShaderModule gammaVertModule = VK_NULL_HANDLE;
    VkShaderModule gammaFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout gammaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout gammaPipeLayout = VK_NULL_HANDLE;
    VkPipeline gammaPipeline = VK_NULL_HANDLE;
    VkDescriptorPool gammaPool = VK_NULL_HANDLE;
    VkDescriptorSet gammaSets[mdk::vk::kFramesInFlight] = {};
    bool gammaReady = false;

    int32_t drawCalls = 0;
    int32_t triangles = 0;

    // The palette pipeline's objects.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    // Descriptor sets that a failed `vkFreeDescriptorSets` lost for good (see
    // `settleRetired`). The handle is dropped rather than retried, so without a
    // count the only trace of one is a refusal that prints once per sentence.
    uint32_t lostDescriptorSets = 0;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipelineCache pipeCache = VK_NULL_HANDLE;
    // The C#'s two samplers (Renderer.cs:249-250): the REPEAT one for a
    // surface's index texture and the CLAMP one for the palette and the
    // sky's panorama.
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler samplerRepeat = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkShaderModule skyVertModule = VK_NULL_HANDLE;
    VkShaderModule skyFragModule = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;

    // The flat-stereo state (P6). `stereo` is the requested mode from the
    // render description; `activeCamera` is the camera the draw path
    // consumes (the centre camera, or the eye being drawn); the composite
    // objects are created on the first stereo frame.
    mdk::StereoSettings stereo;
    mdk::Camera activeCamera;
    uint32_t clearColour = 0xFF000000u;
    bool replayingEyes = false;
    VkShaderModule stereoVertModule = VK_NULL_HANDLE;
    VkShaderModule stereoFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout stereoSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout stereoPipeLayout = VK_NULL_HANDLE;
    VkPipeline stereoPipeline = VK_NULL_HANDLE;
    VkDescriptorPool stereoPool = VK_NULL_HANDLE;
    VkDescriptorSet stereoSets[mdk::vk::kFramesInFlight] = {};
    VkSampler eyeSampler = VK_NULL_HANDLE;
    bool stereoReady = false;

    // The enhanced look's objects (P6 lit pipeline): the colour-texture
    // cache, the trilinear sampler, the per-frame descriptor pools and
    // uniform rings, and the pipelines keyed like the palette ones.
    std::vector<ColourVk> colours;
    std::vector<ReplacementVk> replacements;
    VkSampler colourSampler = VK_NULL_HANDLE;
    VkShaderModule enhancedVertModule = VK_NULL_HANDLE;
    VkShaderModule enhancedFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout enhancedSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout enhancedPipeLayout = VK_NULL_HANDLE;
    std::vector<PipelineVk> enhancedPipelines;
    VkDescriptorPool enhancedPools[mdk::vk::kFramesInFlight] = {};
    VkBuffer uniformRing[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory uniformRingMemory[mdk::vk::kFramesInFlight] = {};
    void* uniformRingMapped[mdk::vk::kFramesInFlight] = {};
    uint32_t uniformCursor = 0;
    VkBuffer lightBuffers[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory lightMemory[mdk::vk::kFramesInFlight] = {};
    void* lightMapped[mdk::vk::kFramesInFlight] = {};
    VkImage dummyColourImage = VK_NULL_HANDLE;
    VkDeviceMemory dummyColourMemory = VK_NULL_HANDLE;
    VkImageView dummyColourView = VK_NULL_HANDLE;
    // The same white texel as a plain 2D view, for the bindings enhanced.frag
    // declares sampler2D (3 shadow, 4 index, 5 palette). dummyColourView is a
    // 2D_ARRAY view for binding 0; handing it to a 2D binding is
    // VUID-vkCmdDraw-viewType-07752 (measured: twice per enhanced draw).
    VkImageView dummyFlatView = VK_NULL_HANDLE;
    bool enhancedReady = false;

    // The sun shadow (P6 shadow slice): a 2048x2048 depth target, its
    // depth-only pass, and the depth pipelines (Solid-culled and not).
    // The scene is deferred to end_frame under the enhanced look so the
    // shadow pass can read the whole queue before the scene renders.
    VkImage shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    VkImageView shadowView = VK_NULL_HANDLE;
    VkRenderPass shadowPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
    VkShaderModule depthFragModule = VK_NULL_HANDLE;
    VkPipeline depthPipelines[2] = {};
    bool shadowReady = false;
    float sunMatrix[16] = {};
    float postClipToView[16] = {};
    float postViewToClip[16] = {};
    bool deferScene = false;

    // The post pass (P6 occlusion/post slice): the camera's depth, the
    // enhanced scene with its mip chain, the occlusion target, and the
    // two screen pipelines (occlusion, post) over the composite pass.
    // The canvas then replays into the scene target through a LOAD pass,
    // over the post's output, as the oracle draws canvas after post.
    VkImage postSceneImage = VK_NULL_HANDLE;
    VkDeviceMemory postSceneMemory = VK_NULL_HANDLE;
    VkImageView postSceneView = VK_NULL_HANDLE;      // all mips, sampled
    VkImageView postSceneLevel0 = VK_NULL_HANDLE;    // mip 0, attached
    VkFramebuffer postSceneFramebuffer = VK_NULL_HANDLE;
    VkImage viewDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory viewDepthMemory = VK_NULL_HANDLE;
    VkImageView viewDepthView = VK_NULL_HANDLE;
    VkFramebuffer viewDepthFramebuffer = VK_NULL_HANDLE;
    VkImage occlusionImage = VK_NULL_HANDLE;
    VkDeviceMemory occlusionMemory = VK_NULL_HANDLE;
    VkImageView occlusionView = VK_NULL_HANDLE;
    VkFramebuffer occlusionFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer postFramebuffer = VK_NULL_HANDLE;  // over the scene view
    VkRenderPass canvasLoadPass = VK_NULL_HANDLE;
    VkFramebuffer canvasFramebuffer = VK_NULL_HANDLE;
    VkShaderModule postVertModule = VK_NULL_HANDLE;
    VkShaderModule occlusionFragModule = VK_NULL_HANDLE;
    VkShaderModule postFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout postPipeLayout = VK_NULL_HANDLE;
    VkPipeline occlusionPipeline = VK_NULL_HANDLE;
    VkPipeline postPipeline = VK_NULL_HANDLE;
    VkDescriptorPool postPool = VK_NULL_HANDLE;
    VkDescriptorSet postSets[mdk::vk::kFramesInFlight] = {};
    VkBuffer postUniformBuffers[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory postUniformMemory[mdk::vk::kFramesInFlight] = {};
    void* postUniformMapped[mdk::vk::kFramesInFlight] = {};
    VkSampler postMipSampler = VK_NULL_HANDLE;
    VkSampler postClampSampler = VK_NULL_HANDLE;
    uint32_t postWidth = 0;
    uint32_t postHeight = 0;
    uint32_t postMips = 0;
    bool postReady = false;

    // Canvas quads deferred under the enhanced look: the vertices are in
    // the canvas ring as usual; these say where and with what to draw.
    struct CanvasRecord {
        VkDeviceSize offset = 0;
        int32_t texture = -1;
        int32_t palette = -1;
        int32_t frameCount = 1;
        float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::vector<CanvasRecord> canvasRecords;
    std::vector<PipelineVk> pipelines;
    std::vector<DescPair> descs;

    // Resource pools. The id IS the index; a negative id is "none".
    std::vector<TextureVk> textures;
    std::vector<PaletteVk> palettes;
    std::vector<MeshVk> meshes;
    std::vector<ResRef> creationLog;

    // The buffers that were released while a frame that reads them may still be
    // running. Each one names the frame index at which its reading frame is
    // provably finished; `settleRetired` destroys them there, which is the
    // CPU/GPU completion boundary AGENTS.md P3 asks for.
    std::vector<RetiredBuffer> retiring;
    // The descriptor sets the cache dropped because the image view they name is
    // going away. Dropping the CACHE ENTRY is what makes a recreated id safe
    // (review-25); returning the SET is what keeps the pool alive, and the two
    // are different acts. Settled beside `retiring`, on the same boundary.
    std::vector<RetiredSet> retiringSets;
    // The images whose view a retired descriptor set names. Destroying the view
    // at the call that retired the set left a frame on the queue sampling a
    // destroyed view, which is the half of the resource AGENTS.md P3 asks about
    // and review-9 F2 named. Settled on the same boundary as `retiringSets`.
    std::vector<RetiredImage> retiringImages;
    // Counts the frames this backend has begun. The retire deadline is
    // `frameIndex + kFramesInFlight`; see `retireBuffer`.
    uint32_t frameIndex = 0;

    // The frame's queued draws, submitted in the oracle's pass order. The
    // caller queues a draw; the backend decides when the frame's 3D scene ends
    // and draws the buckets then (render.h's "the backend sorts and batches",
    // and draw_queue.h for the order).
    mdk::DrawQueue queue;

    // The 2D canvas: quads accumulated between begin/end, drawn in one
    // orthographic Overlay pass. A ring of dynamic vertex buffers -- one per
    // frame in flight, for the reason MeshVk spells out -- resized as needed.
    VkBuffer canvasBuffer[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory canvasMemory[mdk::vk::kFramesInFlight] = {};
    void* canvasMapped[mdk::vk::kFramesInFlight] = {};
    VkDeviceSize canvasCapacity = 0;
    // The write cursor into the canvas buffer. Each quad is uploaded at its own
    // offset and drawn from there, because the persistent map is written for
    // the whole frame at record time -- reusing one slot would make every
    // draw read the last quad's vertices.
    VkDeviceSize canvasOffset = 0;

    // The frame-capture path requested for the frame now being drawn. The
    // path is copied because MdkFrameParams owns only a borrowed C string.
    std::string screenshotPath;

    // The capture ACCOUNTING: how many frames asked for a picture, and how many
    // pictures reached the disk. Every failure below already returned a bool and
    // the frame seam threw it away, so the run exited 0 with no file and the
    // harness upstream read the BMP the previous run left there (review-2 F1).
    // `capturesWritten` is incremented ONLY on the path where the bytes landed.
    int32_t capturesRequested = 0;
    int32_t capturesWritten = 0;
};

// Defined with the drawing code below; the frame and the canvas need them and
