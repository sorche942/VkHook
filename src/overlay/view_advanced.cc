#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "vkhook_internal.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    // window-sized first (exact or titlebar-inset), then the swapchain-linked
    // candidate; the busiest buffer can be the wrong size
    static VkImageView defaultPinCandidateForSwapchain(const DepthInfo& info)
    {
        constexpr int64_t kChromeTolerancePx = 128;
        auto windowSized = [&](const DepthCandidateInfo& c) {
            const int64_t dw = static_cast<int64_t>(c.extent.width)  - static_cast<int64_t>(info.swapchainExtent.width);
            const int64_t dh = static_cast<int64_t>(c.extent.height) - static_cast<int64_t>(info.swapchainExtent.height);
            return dw >= -kChromeTolerancePx && dw <= kChromeTolerancePx
                && dh >= -kChromeTolerancePx && dh <= kChromeTolerancePx;
        };

        for (const auto& c : info.candidates)
            if (c.extent.width == info.swapchainExtent.width
                && c.extent.height == info.swapchainExtent.height)
                return c.imageView;
        for (const auto& c : info.candidates)
            if (windowSized(c))
                return c.imageView;
        for (const auto& c : info.candidates)
            if (c.hasPresentableSnapshotTarget)
                return c.imageView;
        return info.candidates.empty() ? VK_NULL_HANDLE : info.candidates.front().imageView;
    }

    // Convert a VkFormat to a short human-readable string for the UI.
    static const char* depthFormatName(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D16_UNORM:           return "D16_UNORM";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8_D24_PACK32";
        case VK_FORMAT_D32_SFLOAT:          return "D32_SFLOAT";
        case VK_FORMAT_D16_UNORM_S8_UINT:   return "D16_UNORM_S8_UINT";
        case VK_FORMAT_D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
        case VK_FORMAT_R32_SFLOAT:          return "R32_SFLOAT (resolved)";
        case VK_FORMAT_UNDEFINED:           return "none";
        default:                            return "other";
        }
    }

    static const char* layoutName(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:         return "D_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DS_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:          return "D_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "undefined";
        default:                                               return "other";
        }
    }

    static const char* sampleName(VkSampleCountFlagBits samples)
    {
        switch (samples)
        {
        case VK_SAMPLE_COUNT_1_BIT:  return "1x";
        case VK_SAMPLE_COUNT_2_BIT:  return "2x";
        case VK_SAMPLE_COUNT_4_BIT:  return "4x";
        case VK_SAMPLE_COUNT_8_BIT:  return "8x";
        case VK_SAMPLE_COUNT_16_BIT: return "16x";
        case VK_SAMPLE_COUNT_32_BIT: return "32x";
        case VK_SAMPLE_COUNT_64_BIT: return "64x";
        default:                     return "?";
        }
    }

    void ImGuiOverlay::gatherDepthInfo()
    {
        if (!logicalDevice)
            return;

        std::lock_guard<std::mutex> l(globalLock);

        depthInfo.supportedResolveModes = logicalDevice->supportedDepthResolveModes;
        depthInfo.depthCaptureEnabled   = settingsManager.getDepthCapture();
        depthInfo.swapchainExtent       = currentSwapchainExtentLocked(logicalDevice);

        deviceInfo.gpuName      = logicalDevice->gpuName;
        deviceInfo.gpuDriverInfo = logicalDevice->gpuDriverInfo;
        deviceInfo.gpuPciSlot   = logicalDevice->gpuPciSlot;
        deviceInfo.gpuApiVersion = logicalDevice->gpuApiVersion;
        deviceInfo.gpuVendorId  = logicalDevice->gpuVendorId;

        const int modePref = settingsManager.getDepthResolveMode();
        const bool avgSupported = (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        if (modePref == 2 && avgSupported)
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        else
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

        // Snapshot pin state from the device.  If the pinned view was destroyed
        // by the render thread since last frame, clear the stale pin here so the
        // UI immediately falls back to auto instead of showing a ghost pin.
        depthInfo.pinnedView = logicalDevice->pinnedDepthImageView;
        depthInfo.depthIsPinned = (depthInfo.pinnedView != VK_NULL_HANDLE);

        if (depthInfo.depthIsPinned)
        {
            auto it = logicalDevice->depthViewStates.find(depthInfo.pinnedView);
            if (it != logicalDevice->depthViewStates.end())
            {
                // Pinned view is still valid — show it as the active buffer.
                const DepthState& pinned = it->second;
                depthInfo.active.imageView       = pinned.imageView;
                depthInfo.active.format          = pinned.format;
                depthInfo.active.extent          = pinned.extent;
                depthInfo.active.samples         = pinned.samples;
                depthInfo.active.observedLayout  = pinned.observedLayout;
                depthInfo.active.transient       = pinned.transient;
                depthInfo.active.drawCount = logicalDevice->bestDepthCandidate.valid
                                              && logicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                                              ? logicalDevice->bestDepthCandidate.drawCount : 0;
                depthInfo.active.hasPresentableSnapshotTarget =
                    logicalDevice->bestDepthCandidate.valid
                    && logicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                    ? logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget : false;
            }
            else
            {
                // Pinned view was destroyed since last frame — clear the stale pin
                // so we fall back to auto.  getDepthState() in vkhook.cpp does the
                // same check, but we need it here too so the UI is consistent.
                Logger::debug("gatherDepthInfo: pinned view no longer tracked, clearing stale pin");
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                depthInfo.depthIsPinned = false;
                depthInfo.pinnedView    = VK_NULL_HANDLE;
                // Don't set active here — fall through to the auto path below.
            }
        }

        // Auto path (also handles the case where the pin was just cleared above).
        if (!depthInfo.depthIsPinned)
        {
            const DepthState& active = logicalDevice->activeDepthState;
            depthInfo.active.imageView       = active.imageView;
            depthInfo.active.format          = active.format;
            depthInfo.active.extent          = active.extent;
            depthInfo.active.samples         = active.samples;
            depthInfo.active.observedLayout  = active.observedLayout;
            depthInfo.active.transient       = active.transient;
            depthInfo.active.drawCount       = logicalDevice->bestDepthCandidate.valid
                                                 ? logicalDevice->bestDepthCandidate.drawCount : 0;
            depthInfo.active.hasPresentableSnapshotTarget =
                logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
        }
        depthInfo.depthResolveIsMsaa = depthInfo.active.samples != VK_SAMPLE_COUNT_1_BIT;

        // Populate candidate list from all tracked depth views.
        depthInfo.candidates.clear();
        for (const auto& [view, ds] : logicalDevice->depthViewStates)
        {
            DepthCandidateInfo c;
            c.imageView      = view;
            c.format         = ds.format;
            c.extent         = ds.extent;
            c.samples        = ds.samples;
            c.observedLayout = ds.observedLayout;
            c.transient      = ds.transient;
            if (logicalDevice->bestDepthCandidate.valid &&
                logicalDevice->bestDepthCandidate.depthState.imageView == view)
            {
                c.drawCount = logicalDevice->bestDepthCandidate.drawCount;
                c.hasPresentableSnapshotTarget =
                    logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
            }
            depthInfo.candidates.push_back(c);
        }
        // Sort by drawCount descending so the best candidate appears first.
        std::sort(depthInfo.candidates.begin(), depthInfo.candidates.end(),
                  [](const DepthCandidateInfo& a, const DepthCandidateInfo& b){
                      return a.drawCount > b.drawCount;
                  });
    }

    void ImGuiOverlay::applyDepthPinRequests()
    {
        if (!logicalDevice)
            return;

        if (depthPinPendingView != VK_NULL_HANDLE)
        {
            std::lock_guard<std::mutex> l(globalLock);
            auto it = logicalDevice->depthViewStates.find(depthPinPendingView);
            if (it != logicalDevice->depthViewStates.end())
            {
                logicalDevice->pinnedDepthImageView = depthPinPendingView;
                Logger::info("depth manual pin set to view 0x" +
                    std::to_string((uint64_t)depthPinPendingView));
                depthPinChanged = true;
                // Force deferred realloc so descriptor sets + MSAA framebuffers
                // get rebuilt for the new depth source view.
                logicalDevice->depthReallocPending = true;
            }
            else
            {
                Logger::warn("depth pin requested but view not found (may have been destroyed)");
            }
            depthPinPendingView = VK_NULL_HANDLE;
        }

        if (depthPinPendingClear)
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinPendingClear = false;
            depthPinChanged = true;
            // Same: force realloc so we stop using the just-unpinned view.
            logicalDevice->depthReallocPending = true;
            Logger::info("depth manual pin cleared (back to auto-promotion)");
        }

        if (depthTransformPending)
        {
            std::lock_guard<std::mutex> l(globalLock);
            // the resolve owns transforms now (macros no longer carry them),
            // so re-recording it is all a toggle needs
            logicalDevice->depthReallocPending = true;
            depthTransformPending = false;
        }
    }

    // The one-line describer under the card's hero header. Reads as a sentence
    // instead of the old column dump: 1920x1080 (D16_UNORM, MSAA 1x, ...).
    static std::string describeActiveDepth(const DepthInfo& info)
    {
        if (info.active.imageView == VK_NULL_HANDLE)
            return "No depth buffer detected yet";

        std::string s = std::to_string(info.active.extent.width) + "x" +
                        std::to_string(info.active.extent.height);
        s += " (";
        s += depthFormatName(info.active.format);
        s += ", MSAA ";
        s += sampleName(info.active.samples);
        s += info.depthIsPinned ? ", pinned manually" : ", picked automatically";
        s += ")";
        return s;
    }

    namespace
    {
        struct DepthRowAction
        {
            VkImageView pin = VK_NULL_HANDLE;
            bool        unpin = false;
        };

        DepthRowAction drawDepthBufferRow(int index, const DepthCandidateInfo& c,
                                          bool isActive, bool isPinned)
        {
            DepthRowAction action;
            ImGui::PushID(index);
            char label[128];
            snprintf(label, sizeof(label), "%d: %ux%u%s%s", index, c.extent.width, c.extent.height,
                     isActive ? " [active]" : "", isPinned ? " [pinned]" : "");
            if (ImGui::Selectable(label, isPinned))
            {
                if (isPinned)
                    action.unpin = true;
                else
                    action.pin = c.imageView;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("View: 0x%llx", reinterpret_cast<unsigned long long>(c.imageView));
                ImGui::Text("Layout: %s", layoutName(c.observedLayout));
                ImGui::TextUnformatted("Click to pin or unpin this buffer.");
                ImGui::EndTooltip();
            }
            ImGui::TextDisabled("%s | MSAA %s | %u draws%s%s", depthFormatName(c.format),
                                sampleName(c.samples), c.drawCount,
                                c.transient ? " | transient" : "",
                                c.hasPresentableSnapshotTarget ? " | swapchain" : "");
            ImGui::Separator();
            ImGui::PopID();
            return action;
        }
    } // namespace

    void ImGuiOverlay::renderAdvancedView()
    {
        const float d = ImGui::GetFontSize() / 16.0f;
        ImGui::SeparatorText("Depth buffer");
        // Capture hooks are armed at startup, so this setting requires a restart.
                bool depthCapture = settingsManager.getDepthCapture();
        if (ImGui::Checkbox("Capture depth buffer (requires restart)", &depthCapture))
        {
            settingsManager.setDepthCapture(depthCapture);
            settingsDirty = true;
            Logger::info(std::string("depth capture ") + (depthCapture ? "enabled" : "disabled") +
                         " (saved; takes effect on restart)");
            pushToast(LogLevel::Info, depthCapture
                ? "Depth buffer capture on. Restart to apply."
                : "Depth buffer capture off. Restart to apply.");
        }
        ImGui::TextDisabled("%s", describeActiveDepth(depthInfo).c_str());
        if (depthCapture != depthCaptureAtStartup)
        {
            ImGui::TextDisabled("Requires restart");
        }

        ImGui::Spacing();
        // --- Auto / Manual pin ---
        {
            const char* modes[] = { "Auto", "Manual pin" };
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            if (ImGui::Combo("Selection mode", &mode, modes, 2))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                {
                    // unbiassed: the first reported buffer, so the switch is
                    // immediate even before anything has been drawn this frame.
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                }
                else
                {
                    Logger::info("manual pin requested but no depth buffers are tracked yet");
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[mode]);
        }

        ImGui::Spacing();
        // --- Reported buffers live on their own page, pushed from here ---
        {
            if (ImGui::Button("Reported depth buffers"))
                openSubView = SubView_DepthBuffers;
        }

        ImGui::Spacing();
        // --- Encoding: the two modes that actually behave differently ---
        // Luminance/Red and every other single-channel mask decode depth the same
        // way, so they collapse to "Red"; View-space Z is the only transform with
        // distinct behaviour.
        {
            static const int kChannels[] = { 0, 4 };   // depth source channel indices
            const char* labels[] = { "Luminance/Red", "View-space Z" };
            const char* hints[] = {
                "Read depth from the red / luminance channel. The standard Vulkan depth layout.",
                "Convert raw view-space Z into [0,1]. Use when the red channel reads flat or inverted."
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Encoding");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(240.0f * d);
            ImGui::PushID("##depth_encoding");
            int current = settingsManager.getDepthSourceChannel();
            int index = (current == kChannels[1]) ? 1 : 0;
            if (ImGui::Combo("##depth_channel", &index, labels, 2))
            {
                settingsManager.setDepthSourceChannel(kChannels[index]);
                depthTransformPending = true;
                settingsDirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[index]);
            ImGui::PopID();
        }

        // --- Transforms: one compact row ---
        ImGui::Spacing();
        {
            bool depthInvert = settingsManager.getDepthInvert();
            bool depthFlip   = settingsManager.getDepthFlip();
            bool depthFlop   = settingsManager.getDepthFlop();
            bool changed = false;

            if (ImGui::Checkbox("Invert", &depthInvert))
            {
                settingsManager.setDepthInvert(depthInvert);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Invert: depth = 1.0 - depth. Flips the near/far interpretation.");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Flip", &depthFlip))
            {
                settingsManager.setDepthFlip(depthFlip);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image vertically (uv.y -> 1 - uv.y).");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Mirror", &depthFlop))
            {
                settingsManager.setDepthFlop(depthFlop);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image horizontally (uv.x -> 1 - uv.x).");

            if (changed)
            {
                depthTransformPending = true;
                settingsDirty = true;
            }
        }

        // --- Rebuild timing: what the refresh button below and every transform use ---
        ImGui::Spacing();
        {
            const char* labels[] = { "Safe", "Fast" };
            const char* hints[] = {
                "Idle the GPU before rebuilding the depth resolve path, so nothing in flight is still reading what gets freed.",
                "Rebuild behind the layer's own submit fences without idling. Reacts a frame sooner; a driver can fault if the app still references an old resolve target."
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Rebuild");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(240.0f * d);
            ImGui::PushID("##depth_rebuild");
            int index = (settingsManager.getDepthRebuildMode() == 0) ? 0 : 1;
            if (ImGui::Combo("##depth_rebuild_mode", &index, labels, 2))
            {
                settingsManager.setDepthRebuildMode(index);
                settingsDirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[index]);
            ImGui::PopID();
        }

        // --- Footer ---
        ImGui::Spacing();
        if (ImGui::Button("Refresh depth buffers"))
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->activeDepthState    = DepthState{};
            logicalDevice->bestDepthCandidate  = LogicalDevice::DepthCandidateTrackingState{};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinChanged = true;
            Logger::info("depth re-detect requested from Advanced UI");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear the active buffer, the best candidate and any pin.\nThe layer re-evaluates every render pass next frame.");

        // --- Deferred save ---
        flushSettingsSave();
    }

    // --- Pushed page: every tracked depth buffer -------------------------------
    void ImGuiOverlay::renderDepthBuffersView()
    {
        if (ImGui::Button("Back to settings"))
        {
            openSubView = SubView_None;
            return;
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Reported depth buffers");
        ImGui::TextDisabled("%s", describeActiveDepth(depthInfo).c_str());
        ImGui::Spacing();

        // mode control, so the page stands on its own
        {
            const char* modes[] = { "Auto", "Manual pin" };
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            if (ImGui::Combo("Selection mode", &mode, modes, 2))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                {
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[mode]);
        }

        ImGui::Spacing();
        if (depthInfo.candidates.empty())
        {
            UI::TextSupport("No depth views tracked yet. Render a frame with depth.");
            flushSettingsSave();
            return;
        }

        ImGui::BeginChild("##depth_page_list", ImVec2(0, 0), false);
        for (size_t i = 0; i < depthInfo.candidates.size(); ++i)
        {
            const DepthCandidateInfo& c = depthInfo.candidates[i];
            const bool isActive = (c.imageView == depthInfo.active.imageView);
            const bool isPinned = depthInfo.depthIsPinned && (c.imageView == depthInfo.pinnedView);
            const DepthRowAction action = drawDepthBufferRow(static_cast<int>(i), c, isActive, isPinned);
            if (action.pin != VK_NULL_HANDLE)
                depthPinPendingView = action.pin;
            else if (action.unpin)
                depthPinPendingClear = true;
        }
        ImGui::EndChild();

        flushSettingsSave();
    }

} // namespace VkHook
