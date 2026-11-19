#ifndef VKHOOK_OVERLAY_CORE_SNAPSHOT_HH_INCLUDED
#define VKHOOK_OVERLAY_CORE_SNAPSHOT_HH_INCLUDED

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cow_slot.hh"
#include "effects/effect_config.hh"
#include "settings_schema.hh"
#include "vulkan_include.hh"

namespace VkHook
{
    // Everything the overlay's views may know about the layer. Core builds one
    // of these, immutable from then on, and publishes it through a
    // CoreSnapshotSlot; the views only ever read it (invariant 8). Changing
    // anything in the layer is a Command (commands.hh), never a write here.

    struct DepthCandidateInfo
    {
        VkImageView imageView = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent = {0, 0, 1};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageLayout observedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool transient = false;
        uint32_t drawCount = 0;
        bool hasPresentableSnapshotTarget = false;
    };

    struct DepthInfo
    {
        DepthCandidateInfo active;  // the effective buffer: pinned or automatic
        std::vector<DepthCandidateInfo> candidates;  // every tracked buffer, busiest first
        VkExtent2D swapchainExtent = {0, 0};  // window size, for the pin default
        VkResolveModeFlags supportedResolveModes = 0;
        bool depthCaptureEnabled = false;
        bool depthResolveIsMsaa = false;
        VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        bool depthIsPinned = false;
        VkImageView pinnedView = VK_NULL_HANDLE;
    };

    // identity of the VkPhysicalDevice the game is actually using, reported
    // verbatim because the sysfs DRM scan can't tell hybrid GPUs apart
    struct DeviceInfo
    {
        std::string gpuName;
        std::string gpuDriverInfo;
        std::string gpuPciSlot;
        uint32_t gpuApiVersion = 0;
        uint32_t gpuVendorId = 0;
    };

    // One effect instance of the chain, with its parameters as the layer holds
    // them right now. Parameter objects are copies: editing one changes nothing.
    struct EffectView
    {
        std::string name;      // instance name: "Clarity", "CAS.2"
        std::string type;      // base type the shader is found by
        std::string filePath;
        bool enabled = true;
        bool failed = false;
        std::string error;     // compile error when failed
        std::vector<std::shared_ptr<const EffectParam>> params;
        std::vector<PreprocessorDefinition> macros;
        std::vector<std::string> techniques;
    };

    struct EffectCatalog
    {
        std::vector<EffectView> chain;  // selected effects, in chain order
        bool effectsEnabled = true;     // the global on/off

        // What the add-effects page can offer, split by where each came from.
        std::vector<std::string> currentConfigEffects;
        std::vector<std::string> defaultConfigEffects;
        std::map<std::string, std::string> effectPaths;  // effect name -> .fx path
        // Bumped whenever the three above change, so the page can cache its list.
        uint64_t availableVersion = 0;

        const EffectView* find(const std::string& name) const
        {
            for (const EffectView& effect : chain)
                if (effect.name == name)
                    return &effect;
            return nullptr;
        }
    };

    struct ProfileInfo
    {
        std::string gameName;       // detected executable; empty for an unknown one
        std::string activeName;     // active shader preset
        std::string activePath;
        std::vector<std::string> names;  // presets selectable for this game
        std::string configName;     // the .conf the layer runs on
    };

    struct CoreSnapshot
    {
        uint64_t generation = 0;
        std::shared_ptr<const AppSettings> settings = std::make_shared<const AppSettings>();
        EffectCatalog effects;
        ProfileInfo profiles;
        DepthInfo depth;
        DeviceInfo device;
    };

    using CoreSnapshotPtr = std::shared_ptr<const CoreSnapshot>;

    // The hand-off cell. Core publishes from the present thread; the overlay
    // loads once per frame. Neither side ever blocks.
    class CoreSnapshotSlot
    {
    public:
        CoreSnapshotSlot() { current.store(std::make_shared<const CoreSnapshot>()); }
        CoreSnapshotPtr load() const { return current.load(); }
        void publish(CoreSnapshotPtr snapshot) { current.store(std::move(snapshot)); }

    private:
        // CowSlot rather than std::atomic<shared_ptr>: libc++ before 21 lacks the latter.
        CowSlot<CoreSnapshot> current;
    };
} // namespace VkHook

#endif // VKHOOK_OVERLAY_CORE_SNAPSHOT_HH_INCLUDED
