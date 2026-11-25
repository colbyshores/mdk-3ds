// ============================================================================
// vk_context.cpp — the Vulkan device, swapchain and per-frame resources.
//
// See vk_context.h for why the headers are vendored and the entry points
// come through volk.
//
// THIS FILE IS ONE TRANSLATION UNIT ASSEMBLED FROM vk_context/*.inc, the
// renderer_vk.cpp pattern (split 2026-11-25 so it reads in pieces). The .inc
// files are NOT headers: no guards, no includes of their own, each pasted at
// its #include below in the original file order. The split moved code and
// nothing else: the object code is unchanged.
// ============================================================================

#include "render/vk/vk_context.h"

#include <volk.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mdk {
namespace vk {

namespace {

// A FAILURE THAT SPEAKS.
//
// Every result-bearing Vulkan call in this file has its result read, and every
// init-path one that can fail says so: a device that refuses to build a swapchain
// and prints nothing is a black window with a green build, which is the same
// invisible-refusal defect `renderer_vk.cpp`'s `refused()` exists for.
//
// WHY THIS IS NOT THAT HELPER. `refused()` remembers every distinct request so a
// per-draw refusal prints once; this file has no such memory and must not
// duplicate it. That is fine because the split is by how often a site can repeat:
// everything here runs once per process (instance, device, swapchain, depth,
// frame resources), so a plain line cannot flood. The FRAME path is the one that
// repeats, and it is reported by the seam instead — `beginFrame`/`endFrame`
// return false quietly and `mdk_render_begin_frame`/`mdk_render_end_frame` print
// through `refused()`, which is the layer that owns refusal reporting.
//
// It flushes for the reason the seam's helper flushes: a refusal is exactly the
// moment the process may die with the line that explains it still in the buffer.
void vkFailed(const char* what, VkResult result) {
    std::printf("[vk] %s failed (VkResult %d)\n", what, (int)result);
    std::fflush(stdout);
}

// A refusal with no VkResult to report: the call succeeded and the ANSWER is the
// problem (a swapchain that reports zero images, say).
void vkRefused(const char* what) {
    std::printf("[vk] REFUSED %s\n", what);
    std::fflush(stdout);
}

// CONSUMING AN ACQUIRE THAT NO FRAME WILL USE.
//
// `presentWait` is a BINARY semaphore. It is created with no type info, so it has two
// states — signalled and unsignalled — and no count. A successful acquire posts it and
// the only thing that clears it is a submit that waits on it. So a frame that acquired
// an image and then refused left the semaphore signalled, and the NEXT frame's acquire
// would pass an already-signalled binary semaphore to `vkAcquireNextImageKHR`, which is
// invalid use (VUID-vkAcquireNextImageKHR-semaphore-01779) with no specified answer.
//
// The comment this replaces called the leftover "a stale +1 on a semaphore, not a leak
// and not a hang". That reasoned about an arithmetic the object cannot represent: a
// binary semaphore is never +1 (review-1 F4). It is also not unfixable. Vulkan cannot
// WITHDRAW a signal from the CPU, but it can CONSUME one: an empty submit — no command
// buffers, no signal, just this wait — retires the signal as soon as the queue reaches
// it, and the queue reaches it at once because the signal is already posted.
//
// Call this ONLY where the acquire SUCCEEDED. On a failed acquire the semaphore's state
// is not known (the spec does not say it is untouched), and waiting on a signal that may
// never arrive would put the queue to sleep instead of repairing anything.
void drainPresentWait(Context* ctx, FrameContext& frame) {
    VkSubmitInfo drain = {};
    drain.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &frame.presentWait;
    // A stage mask is required whenever there is a wait semaphore
    // (VUID-VkSubmitInfo-pWaitDstStageMask-parameter). TOP_OF_PIPE asks the least of
    // anything: the wait is already satisfied, and no stage has to be flushed for it.
    const VkPipelineStageFlags top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    drain.pWaitDstStageMask = &top;
    const VkResult drained = vkQueueSubmit(ctx->queue, 1, &drain, VK_NULL_HANDLE);
    if (drained != VK_SUCCESS) {
        vkFailed("vkQueueSubmit(consuming the image acquire of a refused frame)", drained);
    }
}

// The validation layer, when it is asked for.
static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool hasValidationLayer() {
    // BOTH CALLS, BOTH RESULTS. The count call and the fill call can each fail,
    // and the fill call may legitimately answer VK_INCOMPLETE (a layer appeared
    // between the two). A discarded result here makes "I could not read the list"
    // and "there is no validation layer" the same return value, and the one thing
    // that turns validation off does so in silence.
    uint32_t count = 0;
    VkResult result = vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumerateInstanceLayerProperties (the count; validation stays off "
                 "because the list could not be read)", result);
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (count > 0) {
        result = vkEnumerateInstanceLayerProperties(&count, layers.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkEnumerateInstanceLayerProperties (the list; validation stays off)",
                     result);
            return false;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(layers[i].layerName, kValidationLayer) == 0) {
            return true;
        }
    }
    return false;
}

#include "vk_context/01_swapchain.inc"

} // namespace

#include "vk_context/02_eye_targets.inc"


bool createInstance(Context* ctx, bool validation) {
    ctx->validation = validation && hasValidationLayer();

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MDK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "MDK";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const char* layers[1] = {kValidationLayer};

    // The instance extensions. The base list is VK_KHR_surface; the windowing
    // system adds the platform's own (xcb, xlib, wayland), because THAT is
    // the extension its surface creation needs and it MUST be enabled in the
    // instance.
    //
    // The list is then FILTERED against the loader's available extensions.
    // Enabling an extension the loader does not have makes
    // vkCreateInstance fail outright, and which platform extensions exist
    // depends on how the loader was started -- a loader in single-ICD
    // "direct" mode exposes none of them, for instance.
    std::vector<const char*> wanted;
    wanted.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    // The headless surface. It is OPTIONAL: the list is filtered against
    // the loader's available extensions, so a loader without it still
    // works -- it just cannot run headless.
    wanted.push_back("VK_EXT_headless_surface");
    if (ctx->extensionProvider) {
        uint32_t platformCount = 0;
        const char* const* platformNames = nullptr;
        platformCount = ctx->extensionProvider(&platformNames);
        for (uint32_t i = 0; i < platformCount && platformNames != nullptr; i++) {
            wanted.push_back(platformNames[i]);
        }
    }

    uint32_t availableCount = 0;
    // Read, and a failure leaves the list EMPTY on purpose: the check below then
    // refuses the instance with its own line. What must not happen is a failed
    // enumeration that looks like "the loader has no extensions".
    VkResult result =
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumerateInstanceExtensionProperties (the count)", result);
        availableCount = 0;
    }
    std::vector<VkExtensionProperties> available(availableCount);
    if (availableCount > 0) {
        result =
            vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkEnumerateInstanceExtensionProperties (the list)", result);
            availableCount = 0;
            available.clear();
        }
    }
    auto isAvailable = [&](const char* name) -> bool {
        for (const VkExtensionProperties& property : available) {
            if (strcmp(property.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    std::vector<const char*> extensions;
    for (const char* name : wanted) {
        if (isAvailable(name)) {
            extensions.push_back(name);
        }
    }
    if (extensions.empty()) {
        std::printf("[vk] no usable instance extensions (the loader exposed none)\n");
        return false;
    }

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (ctx->validation) {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = layers;
    }

    result = vkCreateInstance(&instanceInfo, nullptr, &ctx->instance);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateInstance failed (%d)\n", (int)result);
        return false;
    }

    // LOAD THE INSTANCE-LEVEL ENTRY POINTS. volk loads only the GLOBAL
    // functions in volkInitialize(); every instance-level function (the
    // surface queries, vkCreateDevice, vkDestroySurfaceKHR) is a NULL
    // pointer until volkLoadInstance is called. Calling one before this is
    // a jump to address zero, which is exactly what it looks like.
    volkLoadInstance(ctx->instance);
    return true;
}

bool createHeadlessSurface(Context* ctx) {
    // VK_EXT_headless_surface. A surface with no window behind it, so the
    // renderer can draw into an image and read it back -- exactly what the
    // golden-frame harness needs, and the only way to run on a machine with
    // no display server (or a software X server without DRI3, which the
    // lavapipe WSI needs for presentation).
    PFN_vkCreateHeadlessSurfaceEXT create =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(ctx->instance, "vkCreateHeadlessSurfaceEXT");
    if (!create) {
        std::printf("[vk] VK_EXT_headless_surface is not available\n");
        return false;
    }
    VkHeadlessSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkResult result = create(ctx->instance, &info, nullptr, &ctx->surface);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateHeadlessSurfaceEXT failed (%d)\n", (int)result);
        return false;
    }
    return true;
}

bool createSurface(Context* ctx, void* window) {
    // The surface is platform-specific, so the call goes through a function
    // pointer the platform layer sets. That keeps this file free of SDL
    // (and of any other windowing library) while still letting the seam's
    // single mdk_render_create entry point own the whole sequence.
    if (!ctx->surfaceCreator) {
        std::printf("[vk] no surface creator was set\n");
        return false;
    }
    return ctx->surfaceCreator(ctx->instance, window, &ctx->surface);
}

bool createDevice(Context* ctx, uint32_t width, uint32_t height, bool vsync) {
    uint32_t deviceCount = 0;
    // The count query and the fill are both read. A failed count is NOT "zero
    // devices": the message below about a missing ICD is the right answer for a
    // successful query that found nothing, and the wrong one for a loader that
    // could not be asked.
    VkResult result = vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumeratePhysicalDevices (the count)", result);
        return false;
    }
    if (deviceCount == 0) {
        std::printf("[vk] no Vulkan physical device. (Is an ICD installed? "
                    "On a headless box, try "
                    "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json)\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        vkFailed("vkEnumeratePhysicalDevices (the list)", result);
        return false;
    }
    if (deviceCount == 0) {
        vkRefused("the device: the physical device list came back empty");
        return false;
    }

    bool found = false;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t family = 0;
        if (!findQueueFamily(candidate, ctx->surface, &family)) {
            continue;
        }
        ctx->physicalDevice = candidate;
        ctx->queueFamily = family;
        found = true;
        break;
    }
    if (!found) {
        std::printf("[vk] no physical device with a presentable graphics queue\n");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = ctx->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // THE FEATURES, asked for only where the device has them. The original look
    // needs fillModeNonSolid alone. The enhanced look's colour sampler turns on
    // anisotropy when the device supports it (10_enhanced.inc), which is only
    // legal if the DEVICE enabled samplerAnisotropy; and glslang targeting
    // vulkan1.3 compiles `discard` to OpDemoteToHelperInvocation, which needs
    // shaderDemoteToHelperInvocation. Both were missing (validation errors
    // VUID-VkSamplerCreateInfo-anisotropyEnable-01070 and
    // VUID-VkShaderModuleCreateInfo-pCode-08740 on every --enhanced run).
    VkPhysicalDeviceVulkan13Features have13 = {};
    have13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 have = {};
    have.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(ctx->physicalDevice, &have);

    VkPhysicalDeviceVulkan13Features want13 = {};
    want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    want13.shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation;
    VkPhysicalDeviceFeatures2 want = {};
    want.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want.pNext = &want13;
    VkPhysicalDeviceFeatures& features = want.features;
    features.fillModeNonSolid = VK_TRUE;
    features.samplerAnisotropy = have.features.samplerAnisotropy;

    // THE DEVICE EXTENSIONS. VK_KHR_swapchain is NOT optional: without it
    // vkGetDeviceProcAddr returns a NULL pointer for vkCreateSwapchainKHR,
    // and calling it jumps to address zero. (In Vulkan 1.1 and later the
    // swapchain is core, but the EXTENSION still has to be enabled for the
    // entry points to be present.)
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = (uint32_t)(sizeof(deviceExtensions) / sizeof(deviceExtensions[0]));
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pNext = &want;   // the features travel in the chain, so pEnabledFeatures stays null

    result = vkCreateDevice(ctx->physicalDevice, &deviceInfo, nullptr, &ctx->device);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateDevice failed (%d)\n", (int)result);
        return false;
    }
    // The device-level entry points, likewise.
    volkLoadDevice(ctx->device);
    vkGetDeviceQueue(ctx->device, ctx->queueFamily, 0, &ctx->queue);

    if (!createSwapchain(ctx, width, height, vsync)) {
        return false;
    }
    if (!createRenderPass(ctx)) {
        return false;
    }
    if (!createDepthImage(ctx) || !createFramebuffers(ctx) || !createFrameResources(ctx)) {
        return false;
    }
    return true;
}

void destroyContext(Context* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->device != VK_NULL_HANDLE) {
        // Teardown cannot DO anything with a failed wait except say so: the destroys
        // below have to happen or the device itself cannot be released. What the
        // line buys is the record that the resources were released without a proven
        // completion boundary (AGENTS.md P3) — the difference between a clean exit
        // and one whose last frame was still in flight when the device went away.
        const VkResult idle = vkDeviceWaitIdle(ctx->device);
        if (idle != VK_SUCCESS) {
            vkFailed("vkDeviceWaitIdle(teardown; the resources below are released without "
                     "a proven completion)", idle);
        }

        destroyEyeTargets(ctx);
        destroySceneTarget(ctx);
        for (uint32_t i = 0; i < ctx->imageCount; i++) {
            if (ctx->framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(ctx->device, ctx->framebuffers[i], nullptr);
            }
            if (ctx->imageViews[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(ctx->device, ctx->imageViews[i], nullptr);
            }
        }
        if (ctx->depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, ctx->depthView, nullptr);
            ctx->depthView = VK_NULL_HANDLE;
        }
        if (ctx->depthImage != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, ctx->depthImage, nullptr);
            ctx->depthImage = VK_NULL_HANDLE;
        }
        if (ctx->depthMemory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, ctx->depthMemory, nullptr);
            ctx->depthMemory = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kFramesInFlight; i++) {
            FrameContext& frame = ctx->frames[i];
            if (frame.inFlight != VK_NULL_HANDLE) {
                vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            }
            if (frame.renderFinished != VK_NULL_HANDLE) {
                vkDestroySemaphore(ctx->device, frame.renderFinished, nullptr);
            }
            if (frame.presentWait != VK_NULL_HANDLE) {
                vkDestroySemaphore(ctx->device, frame.presentWait, nullptr);
            }
            if (frame.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(ctx->device, frame.commandPool, nullptr);
            }
        }
        if (ctx->renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(ctx->device, ctx->renderPass, nullptr);
        }
        if (ctx->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
        }
        vkDestroyDevice(ctx->device, nullptr);
        ctx->device = VK_NULL_HANDLE;
    }
    if (ctx->instance != VK_NULL_HANDLE) {
        if (ctx->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
            ctx->surface = VK_NULL_HANDLE;
        }
        vkDestroyInstance(ctx->instance, nullptr);
        ctx->instance = VK_NULL_HANDLE;
    }
}

bool recreateSwapchain(Context* ctx, uint32_t width, uint32_t height, bool vsync) {
    // The wait is the ONLY thing that makes the destroys below safe: a framebuffer
    // a submission is still reading must not be destroyed, and AGENTS.md P3 puts the
    // completion boundary before the release. A failed wait is a boundary that was
    // NOT established, so this refuses before it touches anything: the old swapchain
    // stays intact and this frame is simply dropped.
    const VkResult idle = vkDeviceWaitIdle(ctx->device);
    if (idle != VK_SUCCESS) {
        vkFailed("vkDeviceWaitIdle(before the swapchain rebuild; nothing was destroyed)",
                 idle);
        return false;
    }

    destroyEyeTargets(ctx);
    destroySceneTarget(ctx);
    for (uint32_t i = 0; i < ctx->imageCount; i++) {
        if (ctx->framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(ctx->device, ctx->framebuffers[i], nullptr);
            ctx->framebuffers[i] = VK_NULL_HANDLE;
        }
        if (ctx->imageViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, ctx->imageViews[i], nullptr);
            ctx->imageViews[i] = VK_NULL_HANDLE;
        }
    }
    if (ctx->renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx->device, ctx->renderPass, nullptr);
        ctx->renderPass = VK_NULL_HANDLE;
    }
    if (ctx->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
        ctx->swapchain = VK_NULL_HANDLE;
    }

    if (!createSwapchain(ctx, width, height, vsync)) {
        return false;
    }
    if (!createRenderPass(ctx)) {
        return false;
    }
    if (ctx->depthImage != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->depthView, nullptr);
        vkDestroyImage(ctx->device, ctx->depthImage, nullptr);
        vkFreeMemory(ctx->device, ctx->depthMemory, nullptr);
        ctx->depthImage = VK_NULL_HANDLE;
        ctx->depthView = VK_NULL_HANDLE;
        ctx->depthMemory = VK_NULL_HANDLE;
    }
    if (!createDepthImage(ctx) || !createFramebuffers(ctx)) {
        return false;
    }
    return true;
}

#include "vk_context/03_frame.inc"

} // namespace vk
} // namespace mdk
