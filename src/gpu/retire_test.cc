// Tests for Timeline + RetireQueue against formal/VkHookFormal/RetireQueue.lean.
// Seeded random action sequences mirror the model's actions over two devices;
// after every step the model's safety clauses are checked against the harness's
// own ground truth of which submissions the "GPU" has finished. A deliberately
// buggy flush (old panicLayer) must be caught by the same checks.

#include "gpu/retire.hh"
#include "gpu/timeline.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace VkHook;

namespace
{
    int g_failures = 0;
    int g_checks   = 0;

    void expect(bool condition, const std::string& what)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            std::printf("  FAILED: %s\n", what.c_str());
        }
    }

    VkFence fenceOf(uint64_t id) { return u64ToHandle<VkFence>(id); }

    // ---- ground-truth harness ------------------------------------------------

    struct Obj
    {
        int          dev       = 0;
        VkObjectType type      = VK_OBJECT_TYPE_IMAGE;
        uint64_t     handle    = 0;
        bool         retired   = false;
        bool         destroyed = false;
        uint64_t     tag       = 0;
    };

    struct Sub
    {
        int              dev    = 0;
        uint64_t         serial = 0;
        uint64_t         fence  = 0;  // 0 = unfenced
        std::vector<int> refs;
        bool             complete = false;
    };

    struct Harness;

    struct Dev
    {
        Harness*             owner = nullptr;
        int                  index = 0;
        Timeline             timeline;
        RetireQueue          queue{timeline};
        std::vector<TimelineLane>        lanes;
        std::vector<std::deque<int>>     outstanding;  // per lane, sub indices in order
        std::set<uint64_t>   signalled;                // ground truth: fence signalled
        std::vector<uint64_t> pool;                    // reset fences available for reuse
        bool                 drained       = false;
        bool                 idledSinceSubmit = false;
        uint64_t             prevCompleted = 0;
    };

    VkResult fenceStatus(void* ctx, VkFence fence)
    {
        auto* dev = static_cast<Dev*>(ctx);
        return dev->signalled.count(handleToU64(fence)) ? VK_SUCCESS : VK_NOT_READY;
    }

    void destroyCb(void* ctx, const RetiredObject& object);

    struct Harness
    {
        std::vector<std::unique_ptr<Dev>> devs;
        std::vector<Obj>                  objs;
        std::vector<Sub>                  subs;
        uint64_t                          nextHandle = 1;
        uint64_t                          nextFence  = 1;
        std::string                       error;  // first fault found by the hooks

        explicit Harness(int devices, int lanesPerDev)
        {
            for (int i = 0; i < devices; ++i)
            {
                auto dev   = std::make_unique<Dev>();
                dev->owner = this;
                dev->index = i;
                for (int l = 0; l < lanesPerDev; ++l)
                {
                    dev->lanes.push_back(dev->timeline.addLane());
                    dev->outstanding.emplace_back();
                }
                dev->queue.bind(destroyCb, dev.get());
                devs.push_back(std::move(dev));
            }
        }

        int create(int d, VkObjectType type)
        {
            Obj obj;
            obj.dev    = d;
            obj.type   = type;
            obj.handle = nextHandle++;
            objs.push_back(obj);
            return static_cast<int>(objs.size()) - 1;
        }

        std::vector<int> liveObjects(int d) const
        {
            std::vector<int> out;
            for (size_t i = 0; i < objs.size(); ++i)
                if (objs[i].dev == d && !objs[i].retired && !objs[i].destroyed)
                    out.push_back(static_cast<int>(i));
            return out;
        }

        // submit with a fence (referencing `object`, or -1) or unfenced (no refs)
        void submit(int d, size_t lane, int object, bool fenced)
        {
            Dev& dev = *devs[d];
            Sub  sub;
            sub.dev    = d;
            sub.serial = dev.timeline.nextSerial(dev.lanes[lane]);
            if (object >= 0)
                sub.refs.push_back(object);
            if (fenced)
            {
                if (!dev.pool.empty())
                {
                    sub.fence = dev.pool.back();
                    dev.pool.pop_back();
                }
                else
                    sub.fence = nextFence++;
                dev.timeline.addProbe(dev.lanes[lane], sub.serial, fenceOf(sub.fence));
            }
            subs.push_back(sub);
            dev.outstanding[lane].push_back(static_cast<int>(subs.size()) - 1);
            dev.idledSinceSubmit = false;
        }

        // the GPU finishes the earliest outstanding submission on a lane
        bool gpuComplete(int d, size_t lane)
        {
            Dev& dev = *devs[d];
            if (dev.outstanding[lane].empty())
                return false;
            Sub& sub = subs[dev.outstanding[lane].front()];
            dev.outstanding[lane].pop_front();
            sub.complete = true;
            if (sub.fence)
                dev.signalled.insert(sub.fence);
            return true;
        }

        void retire(int o)
        {
            Obj& obj = objs[o];
            Dev& dev = *devs[obj.dev];
            obj.retired = true;
            obj.tag     = dev.timeline.submittedSerial();
            dev.queue.retire(obj.type, obj.handle);
        }

        size_t collect(int d)
        {
            Dev&                  dev = *devs[d];
            std::vector<VkFence>  sigs;
            dev.timeline.pollProbes({fenceStatus, &dev}, &sigs);
            for (VkFence fence : sigs)
            {
                const uint64_t id = handleToU64(fence);
                if (!dev.signalled.count(id))
                    error = "pollProbes reported a fence the GPU has not signalled";
                dev.signalled.erase(id);  // owner resets it
                dev.pool.push_back(id);
            }
            return dev.queue.collect(dev.timeline.completedSerial());
        }

        void waitIdle(int d)
        {
            Dev& dev = *devs[d];
            dev.timeline.markIdle();
            for (auto& lane : dev.outstanding)
            {
                while (!lane.empty())
                {
                    Sub& sub = subs[lane.front()];
                    lane.pop_front();
                    sub.complete = true;
                    if (sub.fence)
                    {
                        dev.signalled.erase(sub.fence);
                        dev.pool.push_back(sub.fence);  // dropped probes: free to reset
                    }
                }
            }
            dev.idledSinceSubmit = true;
        }

        bool canDrain(int d) const
        {
            const Dev& dev = *devs[d];
            return !dev.drained && dev.timeline.completedSerial() == dev.timeline.submittedSerial();
        }

        void drainAll(int d)
        {
            Dev& dev = *devs[d];
            dev.queue.drainAll();
            dev.drained = true;
            for (Obj& obj : objs)
                if (obj.dev == d)
                    obj.destroyed = true;  // remaining layer objects are destroyed by the caller
        }

        // old panicLayer: flush everything retired without waiting for the GPU
        void panicFlush(int d) { devs[d]->queue.drainAll(); }

        // Model safety clauses. Returns "" when all hold.
        std::string check()
        {
            if (!error.empty())
                return error;
            for (auto& devp : devs)
            {
                Dev&           dev       = *devp;
                const uint64_t completed = dev.timeline.completedSerial();
                const uint64_t submitted = dev.timeline.submittedSerial();
                if (completed > submitted)
                    return "counters_ordered";
                if (completed < dev.prevCompleted)
                    return "completed not monotone";
                dev.prevCompleted = completed;
                if (dev.drained && completed != submitted)
                    return "drained_idle";
                // completed is sound for every submission that references layer objects
                for (const Sub& sub : subs)
                    if (sub.dev == dev.index && !sub.refs.empty() && sub.serial <= completed && !sub.complete)
                        return "completed claims an in-flight submission";
            }
            uint64_t oldest[8] = {};
            bool     have[8]   = {};
            for (const Obj& obj : objs)
            {
                Dev& dev = *devs[obj.dev];
                if (obj.retired)
                {
                    if (obj.tag > dev.timeline.submittedSerial())
                        return "retire_tag_bound";
                    for (const Sub& sub : subs)
                        if (sub.dev == obj.dev && std::find(sub.refs.begin(), sub.refs.end(), &obj - objs.data()) != sub.refs.end() &&
                            sub.serial > obj.tag)
                            return "retire_tag_covers_users";
                    if (!obj.destroyed)
                    {
                        oldest[obj.dev] = have[obj.dev] ? std::min(oldest[obj.dev], obj.tag) : obj.tag;
                        have[obj.dev]   = true;
                    }
                }
                if (obj.destroyed)
                {
                    if (!dev.drained && !(obj.retired && obj.tag <= dev.timeline.completedSerial()))
                        return "destroyed_means_safe";
                    for (const Sub& sub : subs)
                        if (sub.dev == obj.dev && !sub.complete &&
                            std::find(sub.refs.begin(), sub.refs.end(), &obj - objs.data()) != sub.refs.end())
                            return "no_destroyed_in_flight";
                }
            }
            for (size_t d = 0; d < devs.size(); ++d)
            {
                const Dev& dev = *devs[d];
                if (have[d] != (dev.queue.pending() != 0))
                    return "queue pending count disagrees with retired objects";
                if (have[d] && dev.queue.oldestPendingSerial() != oldest[d] && oldest[d] != 0)
                    return "oldestPendingSerial";
            }
            return "";
        }
    };

    void destroyCb(void* ctx, const RetiredObject& object)
    {
        auto* dev = static_cast<Dev*>(ctx);
        Harness& h = *dev->owner;
        for (Obj& obj : h.objs)
        {
            if (obj.dev == dev->index && obj.handle == object.handle)
            {
                if (obj.destroyed)
                    h.error = "object destroyed twice";
                if (obj.type != object.type)
                    h.error = "destroy callback got the wrong type";
                obj.destroyed = true;
                return;
            }
        }
        h.error = "destroy callback for an unknown object";
    }

    constexpr VkObjectType kTypes[] = {
        VK_OBJECT_TYPE_IMAGE,       VK_OBJECT_TYPE_IMAGE_VIEW,  VK_OBJECT_TYPE_DEVICE_MEMORY,
        VK_OBJECT_TYPE_FRAMEBUFFER, VK_OBJECT_TYPE_RENDER_PASS, VK_OBJECT_TYPE_BUFFER,
        VK_OBJECT_TYPE_SAMPLER,     VK_OBJECT_TYPE_PIPELINE,
    };

    // Runs one random sequence; returns "" or the first violated clause.
    std::string runSequence(uint64_t seed, int steps, bool buggy)
    {
        std::mt19937_64 rng(seed);
        Harness         h(2, 2);
        auto            pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };

        for (int step = 0; step < steps; ++step)
        {
            const int d = static_cast<int>(pick(2));
            Dev&      dev = *h.devs[d];
            if (dev.drained)
                continue;
            const size_t lane = pick(dev.lanes.size());
            switch (pick(buggy ? 8 : 7))
            {
            case 0:
                if (h.liveObjects(d).size() < 6)
                    h.create(d, kTypes[pick(std::size(kTypes))]);
                break;
            case 1:
            case 2:
            {
                const bool fenced = pick(5) != 0;
                int        object = -1;
                const auto live   = h.liveObjects(d);
                if (fenced && !live.empty())
                    object = live[pick(live.size())];
                h.submit(d, lane, object, fenced);
                break;
            }
            case 3:
                h.gpuComplete(d, lane);
                break;
            case 4:
            {
                const auto live = h.liveObjects(d);
                if (!live.empty())
                    h.retire(live[pick(live.size())]);
                break;
            }
            case 5:
                h.collect(d);
                break;
            case 6:
                if (pick(4) == 0)
                {
                    h.waitIdle(d);
                    if (pick(2) == 0 && h.canDrain(d))
                        h.drainAll(d);
                }
                break;
            case 7:
                h.panicFlush(d);
                break;
            }
            const std::string fault = h.check();
            if (!fault.empty())
                return fault;
        }
        return "";
    }

    // ---- focused tests -------------------------------------------------------

    struct Log
    {
        std::vector<RetiredObject> destroyed;
    };

    void logCb(void* ctx, const RetiredObject& object) { static_cast<Log*>(ctx)->destroyed.push_back(object); }

    VkResult statusOf(void* ctx, VkFence fence)
    {
        auto* set = static_cast<std::set<uint64_t>*>(ctx);
        return set->count(handleToU64(fence)) ? VK_SUCCESS : VK_NOT_READY;
    }

    void testRandomSequences()
    {
        std::printf("random sequences\n");
        int bad = 0;
        for (uint64_t seed = 1; seed <= 200; ++seed)
        {
            const std::string fault = runSequence(seed, 400, false);
            if (!fault.empty())
            {
                bad++;
                std::printf("  seed %llu: %s\n", static_cast<unsigned long long>(seed), fault.c_str());
            }
        }
        expect(bad == 0, "no safety clause violated in 200 seeds x 400 steps");
    }

    void testBuggyFlushIsCaught()
    {
        std::printf("buggy flush is detected\n");
        int caught = 0;
        for (uint64_t seed = 1; seed <= 200; ++seed)
            if (!runSequence(seed, 400, true).empty())
                caught++;
        expect(caught > 0, "flush without waiting violates the invariant in the same harness");
    }

    void testDeviceIsolation()
    {
        std::printf("device isolation\n");
        Timeline    ta, tb;
        RetireQueue qa(ta), qb(tb);
        Log         la, lb;
        qa.bind(logCb, &la);
        qb.bind(logCb, &lb);
        qa.retire(VK_OBJECT_TYPE_IMAGE, 10);
        qb.retire(VK_OBJECT_TYPE_IMAGE, 20);
        expect(qa.collect(~0ull) == 1, "A collects one entry");
        expect(la.destroyed.size() == 1 && la.destroyed[0].handle == 10, "A destroyed only its own");
        expect(lb.destroyed.empty() && qb.pending() == 1, "B untouched");
        expect(qb.drainAll() == 1 && lb.destroyed[0].handle == 20, "B drains its own");
    }

    void testOrdering()
    {
        std::printf("dependents-first order\n");
        Timeline    t;
        RetireQueue q(t);
        Log         log;
        q.bind(logCb, &log);
        q.retire(VK_OBJECT_TYPE_DEVICE_MEMORY, 1);
        q.retire(VK_OBJECT_TYPE_RENDER_PASS, 2);
        q.retire(VK_OBJECT_TYPE_IMAGE, 3);
        q.retire(VK_OBJECT_TYPE_FRAMEBUFFER, 4);
        q.retire(VK_OBJECT_TYPE_IMAGE_VIEW, 5);
        q.retire(VK_OBJECT_TYPE_IMAGE_VIEW, 6);
        q.retire(VK_OBJECT_TYPE_UNKNOWN, 7);
        expect(q.collect(0) == 7, "all entries tagged 0 are collected");
        std::vector<uint64_t> order;
        for (const auto& o : log.destroyed)
            order.push_back(o.handle);
        const std::vector<uint64_t> expected = {4, 2, 6, 5, 7, 3, 1};
        expect(order == expected, "framebuffer, render pass, views (newest first), image, memory");
        expect(retireRank(VK_OBJECT_TYPE_COMMAND_BUFFER) < retireRank(VK_OBJECT_TYPE_COMMAND_POOL), "cmd buffer before pool");
        expect(retireRank(VK_OBJECT_TYPE_DESCRIPTOR_SET) < retireRank(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT), "set before set layout");
        expect(retireRank(VK_OBJECT_TYPE_PIPELINE) < retireRank(VK_OBJECT_TYPE_PIPELINE_LAYOUT), "pipeline before layout");

        // tag filter: entries newer than completed stay
        Timeline    t2;
        RetireQueue q2(t2);
        Log         log2;
        q2.bind(logCb, &log2);
        const TimelineLane lane = t2.addLane();
        q2.retire(VK_OBJECT_TYPE_IMAGE, 1);   // tag 0
        t2.nextSerial(lane);
        q2.retire(VK_OBJECT_TYPE_IMAGE, 2);   // tag 1
        q2.retire(VK_OBJECT_TYPE_IMAGE, 0);   // ignored
        expect(q2.pending() == 2, "null handle ignored");
        expect(q2.collect(0) == 1 && log2.destroyed[0].handle == 1, "only tag <= completed destroyed");
        expect(q2.oldestPendingSerial() == 1, "oldest pending serial is the remaining tag");
        expect(q2.collect(1) == 1 && q2.pending() == 0, "remaining destroyed once completed");
    }

    void testDrainAll()
    {
        std::printf("drainAll\n");
        Timeline    t;
        RetireQueue q(t);
        Log         log;
        q.bind(logCb, &log);
        const TimelineLane lane = t.addLane();
        for (uint64_t i = 1; i <= 5; ++i)
        {
            t.nextSerial(lane);
            q.retire(VK_OBJECT_TYPE_BUFFER, i, 77);
        }
        expect(q.collect(0) == 0, "nothing completed yet");
        expect(q.drainAll() == 5 && q.pending() == 0, "drainAll destroys everything");
        expect(log.destroyed.size() == 5 && log.destroyed[0].extra == 77, "entries keep their parent handle");

        Timeline    t2;
        RetireQueue q2(t2);  // no destroy fn bound
        q2.retire(VK_OBJECT_TYPE_BUFFER, 1);
        expect(q2.drainAll() == 1 && q2.pending() == 0, "unbound queue still drops and counts entries");
    }

    void testProbedAndUnprobedLanes()
    {
        std::printf("probed / unprobed lanes\n");
        std::set<uint64_t> signalled;
        FenceStatusFn      status{statusOf, &signalled};

        Timeline t;
        expect(t.completedSerial() == 0, "empty timeline");
        const TimelineLane a = t.addLane();
        const TimelineLane b = t.addLane();
        const uint64_t     s1 = t.nextSerial(a);
        const uint64_t     s2 = t.nextSerial(b);
        expect(s1 == 1 && s2 == 2 && t.submittedSerial() == 2, "serials are device-wide");
        expect(t.completedSerial() == 0, "no probe proves anything: only markIdle advances");
        t.pollProbes(status);
        expect(t.completedSerial() == 0, "polling without probes proves nothing");

        t.addProbe(a, s1, fenceOf(1));
        expect(t.completedSerial() == 0, "probed lane with outstanding work holds completion");
        t.pollProbes(status);
        expect(t.completedSerial() == 0, "pending fence still holds");
        signalled.insert(1);
        std::vector<VkFence> sigs;
        t.pollProbes(status, &sigs);
        expect(sigs.size() == 1 && sigs[0] == fenceOf(1), "signalled fence reported");
        expect(t.completedSerial() == 2, "unprobed lane with outstanding work is ignored");
        expect(t.probeCount() == 0, "completed probe dropped");

        // a probed lane with fresh outstanding work holds, completed never drops
        const uint64_t s3 = t.nextSerial(a);
        t.addProbe(a, s3, fenceOf(2));
        expect(t.completedSerial() == 2, "completed is monotone");
        expect(t.probeCovering(2) == fenceOf(2), "probe covers a later serial");
        expect(t.probeCovering(4) == VK_NULL_HANDLE, "no probe covers a serial past the newest");

        // an unfenced batch between two fenced ones is covered by the later fence
        const uint64_t s4 = t.nextSerial(a);
        const uint64_t s5 = t.nextSerial(a);
        t.addProbe(a, s5, fenceOf(3));
        expect(s4 == 4 && s5 == 5, "serials continue");
        expect(t.probeCovering(4) == fenceOf(3), "oldest covering probe is chosen");
        expect(t.probeCovering(3) == fenceOf(2), "oldest probe with serial >= requested");
        signalled.insert(3);  // fence 3 signalled, fence 2 not polled as signalled
        sigs.clear();
        t.pollProbes(status, &sigs);
        expect(sigs.size() == 2 && sigs[0] == fenceOf(2) && sigs[1] == fenceOf(3), "a fence completes older probes of its lane");
        expect(t.completedSerial() == 5, "lane caught up");

        t.nextSerial(b);
        t.markIdle();
        expect(t.completedSerial() == t.submittedSerial() && t.probeCount() == 0, "markIdle completes everything");

        // lane-order min: slowest probed lane decides
        Timeline u;
        signalled.clear();
        const TimelineLane x = u.addLane();
        const TimelineLane y = u.addLane();
        const uint64_t     x1 = u.nextSerial(x);
        const uint64_t     y2 = u.nextSerial(y);
        const uint64_t     x3 = u.nextSerial(x);
        u.addProbe(x, x1, fenceOf(10));
        u.addProbe(y, y2, fenceOf(11));
        u.addProbe(x, x3, fenceOf(12));
        signalled = {10, 12};
        u.pollProbes(status);
        expect(u.completedSerial() == 0, "lane y still outstanding holds everything at its watermark");
        signalled.insert(11);
        u.pollProbes(status);
        expect(u.completedSerial() == 3, "all lanes caught up");
    }

    void testFenceReuse()
    {
        std::printf("fence reuse\n");
        std::set<uint64_t> signalled;
        FenceStatusFn      status{statusOf, &signalled};
        Timeline           t;
        const TimelineLane lane = t.addLane();
        const uint64_t     s1   = t.nextSerial(lane);
        t.addProbe(lane, s1, fenceOf(5));
        signalled.insert(5);  // signalled, not yet polled; the owner resets and reuses it
        signalled.erase(5);
        const uint64_t s2 = t.nextSerial(lane);
        t.addProbe(lane, s2, fenceOf(5));
        expect(t.probeCount() == 1, "reused handle replaces its old probe");
        expect(t.completedSerial() == 1, "older probe was completed before the replacement");
        expect(t.probeCovering(1) == fenceOf(5), "new probe is registered");
        t.pollProbes(status);
        expect(t.completedSerial() == 1, "new fence still pending");
        signalled.insert(5);
        t.pollProbes(status);
        expect(t.completedSerial() == 2, "new fence completes serial 2");
    }
} // namespace

int main()
{
    testRandomSequences();
    testBuggyFlushIsCaught();
    testDeviceIsolation();
    testOrdering();
    testDrainAll();
    testProbedAndUnprobedLanes();
    testFenceReuse();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
