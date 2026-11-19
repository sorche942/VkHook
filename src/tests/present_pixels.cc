// present_pixels: a game-shaped Vulkan app that checks what actually reaches the screen.
//
// Regression for C2 (AGENTS.md section 7): when the layer swaps the swapchain images
// for fakes, every present path has to present what the game drew into them. A layer
// that presents the real images instead leaves the window frozen or showing garbage,
// and nothing inside the process can tell, because the game's own commands all succeed.
//
// So this app asks the X server. It renders a known, per-frame-unique colour through
// an ordinary render pass (no depth attachment, like the apps that hid C2), presents,
// and reads the window back with XGetImage. The X server's view is the ground truth:
// it only shows what the WSI actually delivered, whatever the layer did in between.
//
// The run is a sequence of swapchain generations created with oldSwapchain: the first
// one, then a present-mode change (different image count), then a surface-format
// change. Variants the surface does not offer are skipped and logged.
//
// usage: present-pixels [--expect identity|invert] [--frames N] [--sleep-ms N]
// exit:  0 pass, 1 fail, 77 skip (no X11 display or no usable device)
// stdout: machine-readable lines, see the driver.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kExitSkip = 77;
constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;
// how many of the most recent frames a captured colour may belong to; covers the
// swapchain depth plus the layer's own queued frames
constexpr int kStaleWindow = 12;
constexpr int kTolerance = 5;
// how long the effect chain may take to come up before the run is failed
constexpr int kEffectBudgetMs = 10000;
// frames after a swapchain recreation in which plain output is still accepted (chain rebuild)
constexpr int kRecreateGrace = 30;
// frames at the very start that may show the window background before the first present lands
constexpr int kStartFrames = 3;
// consecutive inverted frames after which the effect counts as live
constexpr int kSettleRun = 10;

struct Rgb {
    int r, g, b;
};

[[noreturn]] void skip(const char* why) {
    std::printf("SKIP %s\n", why);
    std::fflush(stdout);
    std::exit(kExitSkip);
}

[[noreturn]] void fail(const char* what, VkResult r = VK_SUCCESS) {
    std::printf("FAIL setup %s result=%d\n", what, static_cast<int>(r));
    std::fflush(stdout);
    std::exit(1);
}

#define VK(call)                                                           \
    do {                                                                   \
        VkResult vkr_ = (call);                                            \
        if (vkr_ != VK_SUCCESS) fail(#call, vkr_);                         \
    } while (0)

// the colour the app wants on screen for a frame; distinct for 256 frames and spread
// over all three channels so a stale or garbage image cannot match by accident
Rgb wanted(int frame) {
    return {(frame * 37 + 20) % 200 + 28, (frame * 91 + 60) % 200 + 28, (frame * 53 + 10) % 200 + 28};
}

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool isSrgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
}

const char* formatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        default: return "other";
    }
}

const char* modeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
    }
}

struct Generation {
    VkPresentModeKHR mode;
    VkFormat format;
    uint32_t minImages;
};

struct Frame {
    int index;
    int generation;
    bool strict;  // the layer has had time to settle, only the expected colour is acceptable
    Rgb captured;
    int matchedFrame;  // frame index the capture belongs to, -1 when none
    bool inverted;
    bool consistent;  // the sample points agree
};

class App {
public:
    void run(bool invert, int framesPerGeneration, int sleepMs);

private:
    void openWindow();
    void createInstanceAndDevice();
    void createSwapchain(const Generation& g);
    void destroySwapchainResources();
    bool renderAndPresent(int frame);  // true when the present reported SUBOPTIMAL
    Rgb pixel(int x, int y);
    std::vector<Generation> plan();

    Display* dpy_ = nullptr;
    Window win_ = 0;
    XVisualInfo vi_{};
    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> fbs_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore acquire_ = VK_NULL_HANDLE;
    VkSemaphore render_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<VkPresentModeKHR> modes_;
    std::vector<VkSurfaceFormatKHR> formats_;
    VkSurfaceCapabilitiesKHR caps_{};
    bool presentFailed_ = false;
};

void App::openWindow() {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) skip("cannot open X11 display");
    int screen = DefaultScreen(dpy_);
    if (!XMatchVisualInfo(dpy_, screen, 24, TrueColor, &vi_) && !XMatchVisualInfo(dpy_, screen, 32, TrueColor, &vi_))
        skip("no TrueColor visual");
    XSetWindowAttributes a{};
    a.colormap = XCreateColormap(dpy_, RootWindow(dpy_, screen), vi_.visual, AllocNone);
    a.event_mask = ExposureMask | StructureNotifyMask;
    a.border_pixel = 0;
    a.background_pixel = 0;
    win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0, kWidth, kHeight, 0, vi_.depth, InputOutput, vi_.visual,
                         CWColormap | CWEventMask | CWBorderPixel | CWBackPixel, &a);
    XStoreName(dpy_, win_, "VkHook present pixels");
    // fixed size so the WM (if any) cannot resize the window between frames
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = kWidth;
    hints.min_height = hints.max_height = kHeight;
    XSetWMNormalHints(dpy_, win_, &hints);
    XMapRaised(dpy_, win_);
    for (;;) {
        XEvent e;
        XNextEvent(dpy_, &e);
        if (e.type == Expose) break;
    }
    XSync(dpy_, False);
}

void App::createInstanceAndDevice() {
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "present-pixels";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst_);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_EXTENSION_NOT_PRESENT) skip("no Vulkan driver with Xlib WSI");
    if (r != VK_SUCCESS) fail("vkCreateInstance", r);

    VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = dpy_;
    sci.window = win_;
    VK(vkCreateXlibSurfaceKHR(inst_, &sci, nullptr, &surface_));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst_, &n, devs.data());
    // prefer the GPU the user has over a software rasteriser when both are present,
    // but only one that can actually present to this window
    int bestScore = -1;
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &ok);
            if (!ok || !(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                phys_ = d;
                family_ = i;
                std::printf("DEVICE %s\n", p.deviceName);
            }
            break;
        }
    }
    if (!phys_) skip("no device can present to the window");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VK(vkCreateDevice(phys_, &dci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    VK(vkCreateCommandPool(dev_, &pci, nullptr, &pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev_, &cai, &cmd_));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK(vkCreateSemaphore(dev_, &si, nullptr, &acquire_));
    VK(vkCreateSemaphore(dev_, &si, nullptr, &render_));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK(vkCreateFence(dev_, &fi, nullptr, &fence_));

    n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
    formats_.resize(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats_.data());
    n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
    modes_.resize(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes_.data());
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps_));
}

// generation 0 is the baseline; later ones each change one thing the layer has to
// re-derive: present mode (and with it the image count), then surface format
std::vector<Generation> App::plan() {
    auto hasMode = [&](VkPresentModeKHR m) { return std::find(modes_.begin(), modes_.end(), m) != modes_.end(); };
    auto hasFormat = [&](VkFormat f) {
        return std::any_of(formats_.begin(), formats_.end(), [&](const VkSurfaceFormatKHR& s) { return s.format == f; });
    };
    VkFormat base = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
        if (hasFormat(f)) {
            base = f;
            break;
        }
    if (base == VK_FORMAT_UNDEFINED) skip("surface offers no 8-bit RGBA/BGRA format");

    std::vector<Generation> out;
    out.push_back({VK_PRESENT_MODE_FIFO_KHR, base, caps_.minImageCount});
    VkPresentModeKHR alt = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        if (hasMode(m)) {
            alt = m;
            break;
        }
    if (alt != VK_PRESENT_MODE_FIFO_KHR) {
        uint32_t images = std::max(caps_.minImageCount + 1, 4u);
        if (caps_.maxImageCount) images = std::min(images, caps_.maxImageCount);
        out.push_back({alt, base, images});
    } else {
        std::printf("NOTE present-mode variant skipped: surface only offers FIFO\n");
    }
    VkFormat altFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB})
        if (f != base && hasFormat(f)) {
            altFormat = f;
            break;
        }
    if (altFormat != VK_FORMAT_UNDEFINED)
        out.push_back({out.back().mode, altFormat, out.back().minImages});
    else
        std::printf("NOTE format variant skipped: surface offers a single format\n");
    // and back to FIFO with the first format, so a recreate that has to undo state is covered
    if (out.size() > 1) out.push_back(out.front());
    return out;
}

void App::destroySwapchainResources() {
    for (VkFramebuffer f : fbs_) vkDestroyFramebuffer(dev_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    fbs_.clear();
    views_.clear();
    if (pass_) vkDestroyRenderPass(dev_, pass_, nullptr);
    pass_ = VK_NULL_HANDLE;
}

void App::createSwapchain(const Generation& g) {
    vkDeviceWaitIdle(dev_);
    destroySwapchainResources();
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps_));

    VkSurfaceFormatKHR sf{};
    for (const auto& s : formats_)
        if (s.format == g.format) sf = s;
    format_ = g.format;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    uint32_t count = std::max(g.minImages, caps_.minImageCount);
    if (caps_.maxImageCount) count = std::min(count, caps_.maxImageCount);
    sci.minImageCount = count;
    sci.imageFormat = sf.format;
    sci.imageColorSpace = sf.colorSpace;
    sci.imageExtent = caps_.currentExtent.width == 0xFFFFFFFFu ? VkExtent2D{kWidth, kHeight} : caps_.currentExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps_.currentTransform;
    sci.compositeAlpha = (caps_.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = g.mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VK(vkCreateSwapchainKHR(dev_, &sci, nullptr, &fresh));
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = fresh;

    VkAttachmentDescription att{};
    att.format = sf.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK(vkCreateRenderPass(dev_, &rpi, nullptr, &pass_));

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, images.data());
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sf.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK(vkCreateImageView(dev_, &vi, nullptr, &view));
        views_.push_back(view);
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = pass_;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &view;
        fbi.width = sci.imageExtent.width;
        fbi.height = sci.imageExtent.height;
        fbi.layers = 1;
        VkFramebuffer fb;
        VK(vkCreateFramebuffer(dev_, &fbi, nullptr, &fb));
        fbs_.push_back(fb);
    }
    std::printf("GENERATION mode=%s format=%s images=%u\n", modeName(g.mode), formatName(g.format), n);
}

bool App::renderAndPresent(int frame) {
    VK(vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX));
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, acquire_, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) fail("vkAcquireNextImageKHR", r);
    VK(vkResetFences(dev_, 1, &fence_));

    // the colour is specified as what should be *displayed*; an sRGB target encodes
    // on write, so hand it the linear value that encodes back to the wanted one
    Rgb w = wanted(frame);
    float c[3] = {w.r / 255.0f, w.g / 255.0f, w.b / 255.0f};
    if (isSrgb(format_))
        for (float& v : c) v = srgbToLinear(v);
    VkClearValue clear{};
    // the BGRA/RGBA swizzle is handled by the format itself, the clear is always r,g,b
    clear.color.float32[0] = c[0];
    clear.color.float32[1] = c[1];
    clear.color.float32[2] = c[2];
    clear.color.float32[3] = 1.0f;

    VK(vkResetCommandBuffer(cmd_, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cmd_, &bi));
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass_;
    rbi.framebuffer = fbs_[idx];
    rbi.renderArea = {{0, 0}, {kWidth, kHeight}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd_);
    VK(vkEndCommandBuffer(cmd_));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquire_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_;
    VK(vkQueueSubmit(queue_, 1, &si, fence_));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::printf("FAIL present frame=%d result=%d\n", frame, static_cast<int>(r));
        presentFailed_ = true;
    }
    return r == VK_SUBOPTIMAL_KHR;
}

Rgb App::pixel(int x, int y) {
    XImage* img = XGetImage(dpy_, win_, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!img) return {-1, -1, -1};
    unsigned long p = XGetPixel(img, 0, 0);
    auto channel = [](unsigned long v, unsigned long mask) {
        int shift = 0;
        while (mask && !(mask & 1)) mask >>= 1, ++shift;
        unsigned long bits = mask;  // contiguous run of ones
        int width = 0;
        while (bits) bits >>= 1, ++width;
        unsigned long field = (v >> shift) & mask;
        return width >= 8 ? static_cast<int>(field >> (width - 8)) : static_cast<int>(field << (8 - width));
    };
    Rgb out{channel(p, img->red_mask), channel(p, img->green_mask), channel(p, img->blue_mask)};
    XDestroyImage(img);
    return out;
}

bool near(const Rgb& a, const Rgb& b) {
    return std::abs(a.r - b.r) <= kTolerance && std::abs(a.g - b.g) <= kTolerance && std::abs(a.b - b.b) <= kTolerance;
}

void App::run(bool invert, int framesPerGeneration, int sleepMs) {
    openWindow();
    createInstanceAndDevice();
    const std::vector<Generation> gens = plan();

    std::vector<Frame> log;
    int frame = 0;
    int lastMatched = -1;
    int failures = 0;
    std::vector<int> distinct;
    auto note = [&](const char* fmt, auto... args) {
        std::printf(fmt, args...);
        std::printf("\n");
    };
    // For expect=invert the effect chain (shader compile, pipelines) may legitimately take
    // a while to come up on a real GPU, so generation 0 keeps rendering until the first
    // run of inverted frames is seen, for at most kEffectBudgetMs. From then on every frame must be
    // inverted, except the first kRecreateGrace frames after a swapchain recreation, where
    // the chain rebuilds and plain frames are accepted. A stale or unmatched colour is never
    // accepted (apart from the first kStartFrames, before anything was presented).
    bool settled = !invert;
    int invertedRun = 0;
    const auto start = std::chrono::steady_clock::now();

    for (size_t gi = 0; gi < gens.size(); ++gi) {
        createSwapchain(gens[gi]);
        // frames since the swapchain was (re)created; -1 for the very first one, where the
        // plain-until-settled rule already covers startup
        int sinceRecreate = gi > 0 ? 0 : -1;
        int suboptimalRebuilds = 0;
        for (int k = 0;; ++k, ++frame) {
            if (k >= framesPerGeneration) {
                const bool waiting = gi == 0 && !settled;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start).count();
                if (!waiting) break;
                if (ms > kEffectBudgetMs) {
                    note("NOEFFECT inverted colour never appeared within %d ms (%d frames)", kEffectBudgetMs, frame);
                    ++failures;
                    break;
                }
            }
            const bool suboptimal = renderAndPresent(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            XSync(dpy_, False);

            Frame f{};
            f.index = frame;
            f.generation = static_cast<int>(gi);
            const bool acceptPlain = !invert || !settled || (sinceRecreate >= 0 && sinceRecreate < kRecreateGrace);
            f.strict = !acceptPlain;
            f.captured = pixel(kWidth / 2, kHeight / 2);
            const Rgb a = pixel(kWidth / 8, kHeight / 8);
            const Rgb b = pixel(kWidth - kWidth / 8, kHeight - kHeight / 8);
            f.consistent = near(f.captured, a) && near(f.captured, b);
            f.matchedFrame = -1;
            f.inverted = false;
            // newest first, so a colour that repeats within the window resolves to the
            // freshest frame instead of looking stale
            for (int back = 0; back < kStaleWindow && frame - back >= 0 && f.matchedFrame < 0; ++back) {
                const Rgb w = wanted(frame - back);
                const Rgb inv{255 - w.r, 255 - w.g, 255 - w.b};
                if (invert && near(f.captured, inv)) {
                    f.matchedFrame = frame - back;
                    f.inverted = true;
                } else if (acceptPlain && near(f.captured, w)) {
                    f.matchedFrame = frame - back;
                }
            }
            // the layer drops to pass-through while it debounces window resizes, so a few early
            // inverted frames prove nothing; settled needs an unbroken run of them
            invertedRun = f.inverted ? invertedRun + 1 : 0;
            if (invertedRun >= kSettleRun) settled = true;
            const char* verdict = "ok";
            if (f.matchedFrame < 0) {
                verdict = frame < kStartFrames ? "unmatched-start" : "UNMATCHED";
                if (frame >= kStartFrames) ++failures;
            } else if (f.matchedFrame < lastMatched) {
                verdict = "WENT-BACKWARDS";
                ++failures;
            } else if (!f.consistent) {
                verdict = "INCONSISTENT";
                ++failures;
            }
            if (f.matchedFrame >= 0) {
                if (f.matchedFrame > lastMatched) lastMatched = f.matchedFrame;
                if (std::find(distinct.begin(), distinct.end(), f.matchedFrame) == distinct.end())
                    distinct.push_back(f.matchedFrame);
            }
            note("FRAME %d gen=%zu rgb=%d,%d,%d want=%d,%d,%d matched=%d inverted=%d %s", frame, gi, f.captured.r,
                 f.captured.g, f.captured.b, invert ? 255 - wanted(frame).r : wanted(frame).r,
                 invert ? 255 - wanted(frame).g : wanted(frame).g, invert ? 255 - wanted(frame).b : wanted(frame).b,
                 f.matchedFrame, f.inverted ? 1 : 0, verdict);
            log.push_back(f);
            if (sinceRecreate >= 0) ++sinceRecreate;
            // a real app recreates on SUBOPTIMAL (the compositor wants a different surface
            // configuration); do the same, a bounded number of times per generation
            if (suboptimal && suboptimalRebuilds < 3) {
                ++suboptimalRebuilds;
                note("SUBOPTIMAL after frame %d: recreating swapchain", frame);
                createSwapchain(gens[gi]);
                sinceRecreate = 0;
            }
        }
        // staleness at the end of a generation: the last frame must be (nearly) the newest
        const Frame& last = log.back();
        if (last.matchedFrame < 0 || last.index - last.matchedFrame > 3) {
            note("STALE generation %zu ends showing frame %d, newest is %d", gi, last.matchedFrame, last.index);
            ++failures;
        }
    }

    // a frozen window passes every other check by matching one old frame forever
    const int total = frame;
    const int minDistinct = total / 4;
    if (static_cast<int>(distinct.size()) < minDistinct) {
        note("FROZEN only %zu distinct frames seen in %d presented (need %d)", distinct.size(), total, minDistinct);
        ++failures;
    }
    if (presentFailed_) ++failures;

    vkDeviceWaitIdle(dev_);
    note("SUMMARY frames=%d distinct=%zu generations=%zu failures=%d expect=%s", total, distinct.size(), gens.size(),
         failures, invert ? "invert" : "identity");
    std::printf(failures ? "RESULT FAIL\n" : "RESULT PASS\n");
    std::fflush(stdout);
    // the process is a short-lived test; the OS reclaims the rest, and tearing the
    // layer's resources down is covered by the smoke test, not by this one
    std::_Exit(failures ? 1 : 0);
}

}  // namespace

int main(int argc, char** argv) {
    bool invert = false;
    int frames = 60;
    int sleepMs = 12;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--expect" && i + 1 < argc)
            invert = std::string(argv[++i]) == "invert";
        else if (a == "--frames" && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (a == "--sleep-ms" && i + 1 < argc)
            sleepMs = std::atoi(argv[++i]);
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    App app;
    app.run(invert, frames, sleepMs);
    return 0;
}
