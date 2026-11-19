#include "gpu/timeline.hh"

#include <algorithm>

namespace VkHook
{
    TimelineLane Timeline::addLane()
    {
        std::lock_guard guard(lock);
        lanes.emplace_back();
        return static_cast<TimelineLane>(lanes.size() - 1);
    }

    uint64_t Timeline::nextSerial(TimelineLane lane)
    {
        std::lock_guard guard(lock);
        const uint64_t serial = submitted.fetch_add(1, std::memory_order_acq_rel) + 1;
        lanes.at(lane).last   = serial;
        return serial;
    }

    void Timeline::completeLocked(const Probe& probe)
    {
        Lane& lane     = lanes[probe.lane];
        lane.watermark = std::max(lane.watermark, probe.serial);
    }

    uint64_t Timeline::computeLocked() const
    {
        const uint64_t sub = submitted.load(std::memory_order_acquire);

        bool     anyProbed = false;
        bool     held      = false;
        uint64_t minMark   = 0;
        for (const Lane& lane : lanes)
        {
            if (!lane.probed)
                continue;
            anyProbed = true;
            if (lane.last > lane.watermark)
            {
                minMark = held ? std::min(minMark, lane.watermark) : lane.watermark;
                held    = true;
            }
        }

        uint64_t computed = completedFloor;
        if (anyProbed)
            computed = held ? minMark : sub;
        return std::min(std::max(completedFloor, computed), sub);
    }

    uint64_t Timeline::completedSerial() const
    {
        std::lock_guard guard(lock);
        completedFloor = computeLocked();
        return completedFloor;
    }

    void Timeline::addProbe(TimelineLane lane, uint64_t serial, VkFence fence)
    {
        std::lock_guard guard(lock);
        for (auto it = probes.begin(); it != probes.end(); ++it)
        {
            if (it->fence == fence)
            {
                completeLocked(*it);
                probes.erase(it);
                break;
            }
        }
        lanes.at(lane).probed = true;
        probes.push_back({lane, serial, fence});
    }

    void Timeline::pollProbes(const FenceStatusFn& status, std::vector<VkFence>* signalled)
    {
        std::lock_guard guard(lock);

        std::vector<uint64_t> covered(lanes.size(), 0);
        for (const Probe& probe : probes)
        {
            if (status.fn(status.ctx, probe.fence) == VK_SUCCESS)
                covered[probe.lane] = std::max(covered[probe.lane], probe.serial);
        }

        // A signalled fence covers every earlier submission on its queue, so
        // older probes of the lane complete with it.
        auto keep = std::stable_partition(probes.begin(), probes.end(),
            [&](const Probe& probe) { return probe.serial > covered[probe.lane]; });
        std::sort(keep, probes.end(), [](const Probe& a, const Probe& b) {
            return a.lane != b.lane ? a.lane < b.lane : a.serial < b.serial;
        });
        for (auto it = keep; it != probes.end(); ++it)
        {
            completeLocked(*it);
            if (signalled)
                signalled->push_back(it->fence);
        }
        probes.erase(keep, probes.end());

        completedFloor = computeLocked();
    }

    VkFence Timeline::probeCovering(uint64_t serial) const
    {
        std::lock_guard guard(lock);
        const Probe*    best = nullptr;
        for (const Probe& probe : probes)
        {
            if (probe.serial >= serial && (!best || probe.serial < best->serial))
                best = &probe;
        }
        return best ? best->fence : VK_NULL_HANDLE;
    }

    void Timeline::markIdle()
    {
        std::lock_guard guard(lock);
        for (Lane& lane : lanes)
            lane.watermark = lane.last;
        probes.clear();
        completedFloor = submitted.load(std::memory_order_acquire);
    }

    size_t Timeline::probeCount() const
    {
        std::lock_guard guard(lock);
        return probes.size();
    }
} // namespace VkHook
