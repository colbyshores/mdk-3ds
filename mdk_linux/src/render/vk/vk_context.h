#pragma once

// ============================================================================
// vk_context.h — the Vulkan device, swapchain and per-frame resources.
//
// This is the Linux backend's half of the renderer, split the way t2k's
// t2k_pc splits it (vk_context / vk_resources / vk_pipeline / vk_scene /
// vk_post). The seam implementation in renderer_vk.cpp owns one of these.
//
// VULKAN HEADERS ARE VENDORED under third_party/vulkan, and the entry points
// are loaded through VOLK (third_party/volk) rather than linked against
// libvulkan. That is deliberate: volk dlopens the loader, so a machine with
// no ICD gets a MESSAGE, not a missing shared object.
//
// The Vulkan 1.3 core is what t2k uses and what the shaders are compiled
// against (glslang -V --target-env vulkan1.3).
// ============================================================================

#include <vulkan/vulkan.h>

#include <stdint.h>

namespace mdk {
namespace vk {

// The number of frames in flight. Two is the usual minimum: the CPU builds
// frame N+1 while the GPU draws frame N, and a fence stops the CPU from
// overwriting a frame the GPU is still reading.
static constexpr uint32_t kFramesInFlight = 2;

// The per-frame resources. One set per frame in flight.
struct FrameContext {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    // The fence the CPU waits on before reusing this frame's resources.
    VkFence inFlight = VK_NULL_HANDLE;

    // The semaphores. `renderFinished` is signalled when the frame's draws
    // are done; `presentWait` is waited on before presenting.
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkSemaphore presentWait = VK_NULL_HANDLE;

    // The command pool this frame's command buffer comes from.
    VkCommandPool commandPool = VK_NULL_HANDLE;
};

// The whole Vulkan context. One per renderer.
struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    // The swapchain images and their views.
    VkImage images[16] = {};
    VkImageView imageViews[16] = {};
    VkFramebuffer framebuffers[16] = {};
    uint32_t imageCount = 0;

    FrameContext frames[kFramesInFlight];
    uint32_t currentFrame = 0;

    // The image currently being recorded.
    uint32_t currentImage = 0;

    // The memory the swapchain images are bound to.
    VkDeviceMemory imageMemory[16] = {};

    // THE DEPTH BUFFER. The render pass declares a depth attachment, so
    // every framebuffer MUST have one: a framebuffer with fewer attachments
    // than its render pass is INVALID, and a driver is entitled to
    // dereference the missing view's null handle. (Lavapipe segfaults
    // inside vkCmdBeginRenderPass on exactly that.)
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // THE SCENE TARGET (P6 foundation). The scene renders HERE, into an
    // offscreen colour image, and endFrame blits it into the acquired
    // swapchain image for present. Before this split the scene drew straight
    // into the swapchain image, so nothing downstream -- no post pass, no
    // stereo composite -- could ever read the finished frame. The offscreen
    // image uses the swapchain format and the same render pass and depth
    // view, so existing pipelines are unaffected; with no consumer yet the
    // blit is an exact copy and the presented bytes are unchanged.
    VkImage sceneImage = VK_NULL_HANDLE;
    VkDeviceMemory sceneMemory = VK_NULL_HANDLE;
    VkImageView sceneView = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;

    // THE STEREO EYES AND THE COMPOSITE (P6). When a stereo mode is on, the
    // scene renders once per eye into eyeImage[0] (left) and eyeImage[1]
    // (right), and a composite pass samples both into the scene target,
    // which then blits to present exactly as the mono frame does. The
    // composite pass is colour-only over the scene view; the eyes share the
    // scene render pass and depth view. Created lazily by ensureEyeTargets
    // and dropped on every swapchain rebuild.
    VkImage eyeImage[2] = {};
    VkDeviceMemory eyeMemory[2] = {};
    VkImageView eyeView[2] = {};
    VkFramebuffer eyeFramebuffer[2] = {};
    VkRenderPass compositePass = VK_NULL_HANDLE;
    VkFramebuffer compositeFramebuffer = VK_NULL_HANDLE;
    bool eyesReady = false;

    // The framebuffer beginFrame's render pass targets: the scene
    // framebuffer normally, an eye framebuffer when the renderer is drawing
    // that eye. VK_NULL_HANDLE means the scene framebuffer.
    VkFramebuffer sceneOverride = VK_NULL_HANDLE;

    // The image submitFrame blits to present: the scene image normally, or
    // a post pass's output (the gamma grade) when one ran. VK_NULL_HANDLE
    // means the scene image. Reset by the renderer every frame.
    VkImage presentImage = VK_NULL_HANDLE;

    bool validation = false;

    // The surface creator, set by the platform layer. It creates the
    // platform's surface for the window against the given instance.
    // Keeping it a function pointer is what lets this file stay free of
    // SDL while the seam still owns the whole create sequence.
    bool (*surfaceCreator)(VkInstance instance, void* window, VkSurfaceKHR* outSurface) = nullptr;

    // The platform's REQUIRED INSTANCE EXTENSIONS, if any. The windowing
    // system knows which surface extension its backend needs (xcb, xlib,
    // wayland), and that extension MUST be enabled in the instance or the
    // surface creation fails. The provider returns the count and fills the
    // array; the array is owned by the provider and must stay valid until
    // the instance is created.
    uint32_t (*extensionProvider)(const char* const** outNames) = nullptr;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// The context is created in THREE STEPS, and the order is forced by Vulkan:
// the instance must exist before a surface can be created against it, and
// the surface must exist before the device can be chosen (the queue family
// is chosen by whether it can PRESENT to that surface).
//
// 1. createInstance  -- the VkInstance.
// 2. createSurface   -- the platform's surface for the window.
// 3. createDevice    -- the device, the queue, the swapchain, the render
//                       pass, the framebuffers and the per-frame resources.
//
// Each returns false on failure, and the caller must not assume any member is
// valid. destroyContext cleans up whatever was created, in any order.
bool createInstance(Context* ctx, bool validation);

// The surface for the platform's window. `window` is the platform's window
// handle (an SDL_Window* on Linux). The surface is created against the
// instance createInstance made.
bool createSurface(Context* ctx, void* window);

// A surface with NO window behind it (VK_EXT_headless_surface). The
// renderer draws into an image and reads it back. This is what the
// golden-frame harness uses, and it is the only way to run on a machine
// whose display server cannot present a Vulkan swapchain.
bool createHeadlessSurface(Context* ctx);

// The device, the queue, the swapchain and the per-frame resources.
bool createDevice(Context* ctx, uint32_t width, uint32_t height, bool vsync);

// Destroy everything the context owns, in dependency order.
void destroyContext(Context* ctx);

// Recreate the swapchain when the window changed size. The old swapchain's
// resources are destroyed first; the images are not owned by the swapchain
// and must be released explicitly.
bool recreateSwapchain(Context* ctx, uint32_t width, uint32_t height, bool vsync);

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// Begin recording the next frame: acquire the next swapchain image, reset
// and record the command buffer. Returns false when no image was acquired.
// `clearColour` is 0xAARRGGBB, the frame's clear colour.
bool beginFrame(Context* ctx, uint32_t width, uint32_t height, bool vsync,
                uint32_t clearColour = 0xFF000000u);

// End recording: submit the command buffer and present. Returns false when
// the frame could not be submitted.
bool endFrame(Context* ctx);

// The stereo eye targets and composite pass (see Context's eye fields).
// ensureEyeTargets creates them at the current extent if needed;
// destroyEyeTargets releases them (called on rebuild and teardown).
bool ensureEyeTargets(Context* ctx);
void destroyEyeTargets(Context* ctx);

// Which framebuffer the next beginFrame render pass targets (an eye, or
// VK_NULL_HANDLE for the scene target).
void setSceneOverride(Context* ctx, VkFramebuffer framebuffer);

// Which image submitFrame blits to present (a post output, or
// VK_NULL_HANDLE for the scene image).
void setPresentImage(Context* ctx, VkImage image);

// End the currently open render pass, and begin one on the given
// framebuffer/pass. The stereo path uses these to draw the second eye and
// the composite between beginFrame and submitFrame.
void endPass(Context* ctx);
bool beginPass(Context* ctx, VkFramebuffer framebuffer, VkRenderPass pass,
               uint32_t clearColour);

// Submit and present the frame whose render passes are all closed: the
// scene blit, the command-buffer end, the submit and the present. endFrame
// is endPass + submitFrame; the stereo path closes its own passes and calls
// submitFrame directly.
bool submitFrame(Context* ctx);

// The frame's command buffer, for the renderer's draws.
VkCommandBuffer commandBuffer(Context* ctx);

} // namespace vk
} // namespace mdk
