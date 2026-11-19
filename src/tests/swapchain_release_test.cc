// SwapchainShared::release picks the destroy call of each handle by its VkObjectType.
//
// Regression: it used to pick by the handle's C++ type (std::is_same_v<Handle, VkFence>
// ...). On i686 every non-dispatchable handle is the same uint64_t, so the first branch
// matched everything and vkDestroyFence was called on image views, images and memory:
// the 32-bit layer crashed in the driver at the first swapchain recreation (the app
// destroys the old swapchain after creating the new one). The test runs in the i686 build.

#include "gpu/retire.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool ok, const char* what)
    {
        if (ok)
            return;
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }

    struct Call
    {
        const char* fn;
        uint64_t    handle;
        bool operator==(const Call& other) const { return fn == other.fn && handle == other.handle; }
    };
    std::vector<Call> calls;

    VKAPI_ATTR void VKAPI_CALL fakeDestroyFence(VkDevice, VkFence h, const VkAllocationCallbacks*)
    {
        calls.push_back({"fence", handleToU64(h)});
    }
    VKAPI_ATTR void VKAPI_CALL fakeDestroySemaphore(VkDevice, VkSemaphore h, const VkAllocationCallbacks*)
    {
        calls.push_back({"semaphore", handleToU64(h)});
    }
    VKAPI_ATTR void VKAPI_CALL fakeDestroyImageView(VkDevice, VkImageView h, const VkAllocationCallbacks*)
    {
        calls.push_back({"view", handleToU64(h)});
    }
    VKAPI_ATTR void VKAPI_CALL fakeDestroyImage(VkDevice, VkImage h, const VkAllocationCallbacks*)
    {
        calls.push_back({"image", handleToU64(h)});
    }
    VKAPI_ATTR void VKAPI_CALL fakeFreeMemory(VkDevice, VkDeviceMemory h, const VkAllocationCallbacks*)
    {
        calls.push_back({"memory", handleToU64(h)});
    }

    void fill(SwapchainShared& shared)
    {
        shared.effectFences      = {u64ToHandle<VkFence>(0x101), u64ToHandle<VkFence>(0x102)};
        shared.semaphores        = {u64ToHandle<VkSemaphore>(0x201)};
        shared.overlaySemaphores = {u64ToHandle<VkSemaphore>(0x301)};
        shared.imageViews        = {u64ToHandle<VkImageView>(0x401), u64ToHandle<VkImageView>(0x402)};
        shared.fakeImages        = {u64ToHandle<VkImage>(0x501)};
        shared.fakeImageMemories = {u64ToHandle<VkDeviceMemory>(0x601)};
    }
} // namespace

int main()
{
    LogicalDevice device;
    device.vkd.DestroyFence     = &fakeDestroyFence;
    device.vkd.DestroySemaphore = &fakeDestroySemaphore;
    device.vkd.DestroyImageView = &fakeDestroyImageView;
    device.vkd.DestroyImage     = &fakeDestroyImage;
    device.vkd.FreeMemory       = &fakeFreeMemory;

    {
        // the GPU is idle: each object goes to the call of its own type
        SwapchainShared shared(&device);
        fill(shared);
        shared.release(true);
        const std::vector<Call> want = {{"fence", 0x101},     {"fence", 0x102}, {"semaphore", 0x201},
                                        {"semaphore", 0x301}, {"view", 0x401},  {"view", 0x402},
                                        {"image", 0x501},     {"memory", 0x601}};
        expect(calls == want, "release(true) destroys every object with the call of its type");
        expect(device.retireQueue.pending() == 0, "nothing is retired when the GPU is idle");
        calls.clear();
        shared.release(true);
        expect(calls.empty(), "release is idempotent");
    }

    {
        // not proven idle: everything is retired, nothing destroyed inline
        SwapchainShared shared(&device);
        fill(shared);
        shared.release(false);
        expect(calls.empty(), "release(false) destroys nothing inline");
        expect(device.retireQueue.pending() == 8, "release(false) retires all eight objects");
    }

    if (failures == 0)
        std::printf("swapchain release tests passed\n");
    return failures == 0 ? 0 : 1;
}
