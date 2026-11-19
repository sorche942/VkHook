// Present modes (formal/VkHookFormal/PresentModes.lean) and semaphore
// accounting. The mode chooser is checked exhaustively; the accounting runs the
// production submitPresentBatches against a fake queue dispatch and checks every
// application wait semaphore is consumed exactly once.

#include "present/present_mode.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void check(bool value, const char* what, int line)
    {
        if (!value)
        {
            std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
            ++failures;
        }
    }
#define CHECK(expr) check((expr), #expr, __LINE__)

    VkSemaphore sem(uintptr_t n) { return reinterpret_cast<VkSemaphore>(n); }
    VkCommandBuffer cmd(uintptr_t n) { return reinterpret_cast<VkCommandBuffer>(n); }
    VkQueue         queueHandle() { return reinterpret_cast<VkQueue>(uintptr_t{0x1000}); }

    struct RecordedSubmit
    {
        std::vector<VkSemaphore>     waits;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore>     signals;
        VkFence                      fence = VK_NULL_HANDLE;
    };

    std::vector<RecordedSubmit> recorded;
    VkResult                    failSubmitNumber = VK_SUCCESS;  // result for the submit counted by failAt
    int                         failAt           = -1;

    VKAPI_ATTR VkResult VKAPI_CALL fakeSubmit(VkQueue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            RecordedSubmit r;
            r.waits.assign(submits[i].pWaitSemaphores, submits[i].pWaitSemaphores + submits[i].waitSemaphoreCount);
            r.commandBuffers.assign(submits[i].pCommandBuffers, submits[i].pCommandBuffers + submits[i].commandBufferCount);
            r.signals.assign(submits[i].pSignalSemaphores, submits[i].pSignalSemaphores + submits[i].signalSemaphoreCount);
            r.fence = fence;
            recorded.push_back(std::move(r));
        }
        if (failAt >= 0 && static_cast<int>(recorded.size()) - 1 == failAt)
            return failSubmitNumber;
        return VK_SUCCESS;
    }

    struct Fixture
    {
        Timeline   timeline;
        QueueTable queues;
        Fixture()
        {
            recorded.clear();
            failAt = -1;
            QueueDispatch dispatch;
            dispatch.submit = &fakeSubmit;
            queues.init(VK_NULL_HANDLE, dispatch, timeline);
            queues.add(queueHandle(), 0, 0);
        }
    };

    // every app wait appears in exactly one layer batch, and in no other
    void checkWaitsConsumedOnce(const std::vector<VkSemaphore>& appWaits, int line)
    {
        for (VkSemaphore wait : appWaits)
        {
            int consumed = 0;
            for (const RecordedSubmit& r : recorded)
                consumed += static_cast<int>(std::count(r.waits.begin(), r.waits.end(), wait));
            check(consumed == 1, "app wait consumed exactly once", line);
        }
    }

    void testChooser()
    {
        for (int bits = 0; bits < 32; ++bits)
        {
            ModeInputs in;
            in.faked             = bits & 1;
            in.effectsOn         = bits & 2;
            in.softDisabled      = bits & 4;
            in.depthHold         = bits & 8;
            in.generationPending = bits & 16;
            const bool bypass = !in.effectsOn || in.softDisabled || in.depthHold || in.generationPending;
            const PresentMode mode = choosePresentMode(in);
            if (!in.faked)
                CHECK(mode == PresentMode::Passthrough);
            else
                CHECK(mode == (bypass ? PresentMode::CopyOnly : PresentMode::Effects));
            // a faked swapchain never passes through, whatever the bypass reason
            if (in.faked)
                CHECK(mode != PresentMode::Passthrough);
        }
    }

    void testAccounting()
    {
        const PresentMode pass[]  = {PresentMode::Passthrough, PresentMode::Passthrough};
        const PresentMode copy[]  = {PresentMode::CopyOnly, PresentMode::Effects};
        const PresentMode mixed[] = {PresentMode::Effects, PresentMode::Passthrough};

        WaitAccounting a = accountPresentWaits(pass, 2, 3);
        CHECK(a.forwardUnchanged && a.layerWaitCount == 0 && !a.needsRelay);
        a = accountPresentWaits(copy, 2, 3);
        CHECK(!a.forwardUnchanged && a.layerWaitCount == 3 && !a.needsRelay);
        a = accountPresentWaits(mixed, 2, 3);
        CHECK(!a.forwardUnchanged && a.layerWaitCount == 3 && a.needsRelay);
        a = accountPresentWaits(copy, 1, 0);
        CHECK(!a.forwardUnchanged && a.layerWaitCount == 0 && !a.needsRelay);
        a = accountPresentWaits(nullptr, 0, 2);
        CHECK(a.forwardUnchanged);
    }

    void testSingleSwapchain(PresentMode mode, uint32_t waitCount)
    {
        Fixture f;
        std::vector<VkSemaphore> appWaits;
        for (uint32_t i = 0; i < waitCount; ++i)
            appWaits.push_back(sem(10 + i));

        PresentTarget target;
        target.mode            = mode;
        target.commandBuffer   = cmd(1);
        target.signalSemaphore = sem(100);

        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits.data(), waitCount,
                                                           &target, 1, presentWaits);
        CHECK(r.result == VK_SUCCESS);
        CHECK(recorded.size() == 1);
        CHECK(recorded[0].commandBuffers.size() == 1 && recorded[0].commandBuffers[0] == cmd(1));
        CHECK(recorded[0].waits.size() == waitCount);
        checkWaitsConsumedOnce(appWaits, __LINE__);
        // the present waits on what the layer signalled, never on an app semaphore
        CHECK(presentWaits.size() == 1 && presentWaits[0] == sem(100));
        CHECK(target.serial != 0);
    }

    int overlayFenceCalls = 0;
    VkFence overlayFence(void*, uint32_t)
    {
        // the overlay's fence is requested right before its submit: nothing of the overlay batch exists yet
        ++overlayFenceCalls;
        return reinterpret_cast<VkFence>(uintptr_t{0x77});
    }

    void testOverlayFollows()
    {
        Fixture f;
        overlayFenceCalls = 0;
        VkSemaphore appWaits[] = {sem(10)};

        PresentTarget target;
        target.mode                 = PresentMode::CopyOnly;
        target.commandBuffer        = cmd(1);
        target.signalSemaphore      = sem(100);
        target.overlayCommandBuffer = cmd(2);
        target.overlaySemaphore     = sem(200);
        target.overlayFence         = &overlayFence;

        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits, 1, &target, 1, presentWaits);
        CHECK(r.result == VK_SUCCESS);
        CHECK(recorded.size() == 2);
        CHECK(recorded[1].waits.size() == 1 && recorded[1].waits[0] == sem(100));
        CHECK(recorded[1].signals.size() == 1 && recorded[1].signals[0] == sem(200));
        CHECK(recorded[1].fence == reinterpret_cast<VkFence>(uintptr_t{0x77}));
        CHECK(overlayFenceCalls == 1);
        CHECK(presentWaits.size() == 1 && presentWaits[0] == sem(200));
        checkWaitsConsumedOnce({sem(10)}, __LINE__);
    }

    void testTwoFaked()
    {
        Fixture f;
        VkSemaphore appWaits[] = {sem(10), sem(11)};
        PresentTarget targets[2];
        targets[0].mode = PresentMode::Effects;
        targets[0].commandBuffer = cmd(1);
        targets[0].signalSemaphore = sem(100);
        targets[1].mode = PresentMode::CopyOnly;
        targets[1].commandBuffer = cmd(2);
        targets[1].signalSemaphore = sem(101);

        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits, 2, targets, 2, presentWaits);
        CHECK(r.result == VK_SUCCESS);
        CHECK(recorded.size() == 2);
        CHECK(recorded[0].waits.size() == 2 && recorded[1].waits.empty());
        checkWaitsConsumedOnce({sem(10), sem(11)}, __LINE__);
        CHECK(presentWaits.size() == 2 && presentWaits[0] == sem(100) && presentWaits[1] == sem(101));
    }

    void testMixedRelay()
    {
        Fixture f;
        VkSemaphore appWaits[] = {sem(10)};
        PresentTarget targets[2];
        targets[0].mode = PresentMode::Passthrough;
        targets[0].relaySemaphore = sem(300);
        targets[1].mode = PresentMode::CopyOnly;
        targets[1].commandBuffer = cmd(2);
        targets[1].signalSemaphore = sem(101);

        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits, 1, targets, 2, presentWaits);
        CHECK(r.result == VK_SUCCESS);
        // the layer batch first (it consumes the wait), the relay after it
        CHECK(recorded.size() == 2);
        CHECK(recorded[0].commandBuffers.size() == 1 && recorded[0].waits.size() == 1);
        CHECK(recorded[1].commandBuffers.empty() && recorded[1].waits.empty());
        CHECK(recorded[1].signals.size() == 1 && recorded[1].signals[0] == sem(300));
        checkWaitsConsumedOnce({sem(10)}, __LINE__);
        CHECK(presentWaits.size() == 2 && presentWaits[0] == sem(300) && presentWaits[1] == sem(101));
    }

    VkResult prepareAddsWait(void*, uint32_t, VkSubmitInfo& submit)
    {
        static VkSemaphore aux[2];
        aux[0] = sem(500);
        static VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores    = aux;
        submit.pWaitDstStageMask  = &stage;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    void testPrepareFailure()
    {
        Fixture f;
        VkSemaphore appWaits[] = {sem(10)};
        PresentTarget target;
        target.mode = PresentMode::Effects;
        target.commandBuffer = cmd(1);
        target.signalSemaphore = sem(100);
        target.prepare = &prepareAddsWait;

        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits, 1, &target, 1, presentWaits);
        CHECK(r.result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(r.auxiliaryQueued);  // the effect replaced the wait list: the layer must stop, not retry the app's present
        CHECK(recorded.empty());
    }

    void testSubmitFailure()
    {
        Fixture f;
        failAt           = 0;
        failSubmitNumber = VK_ERROR_DEVICE_LOST;
        VkSemaphore appWaits[] = {sem(10)};
        PresentTarget target;
        target.mode = PresentMode::CopyOnly;
        target.commandBuffer = cmd(1);
        target.signalSemaphore = sem(100);
        std::vector<VkSemaphore> presentWaits;
        const PresentSubmitResult r = submitPresentBatches(f.queues, queueHandle(), appWaits, 1, &target, 1, presentWaits);
        CHECK(r.result == VK_ERROR_DEVICE_LOST);
        CHECK(r.failedTarget == 0);
    }
} // namespace

int main()
{
    testChooser();
    testAccounting();
    for (PresentMode mode : {PresentMode::CopyOnly, PresentMode::Effects})
        for (uint32_t waits : {0u, 1u, 3u})
            testSingleSwapchain(mode, waits);
    testOverlayFollows();
    testTwoFaked();
    testMixedRelay();
    testPrepareFailure();
    testSubmitFailure();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("present mode tests passed");
    return 0;
}
