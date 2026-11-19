#include "overlay_ui.hh"
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
            if (c.matchesWindowSize)
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
                                c.matchesWindowSize ? " | swapchain" : "");
            ImGui::Separator();
            ImGui::PopID();
            return action;
        }
    } // namespace

    void OverlayUi::renderAdvancedView()
    {
        const AppSettings& cfg = settings();
        const DepthInfo& depthInfo = snapshot->depth;
        const float d = ImGui::GetFontSize() / 16.0f;
        ImGui::SeparatorText("Depth buffer");
        // Capture hooks are armed at startup, so this setting requires a restart.
        bool depthCapture = cfg.depthCapture;
        if (ImGui::Checkbox("Capture depth buffer (requires restart)", &depthCapture))
        {
            emitSetting("depthCapture", depthCapture);
            emit(SettingsChanged{});
            Logger::info(std::string("depth capture ") + (depthCapture ? "enabled" : "disabled") +
                         " (saved; takes effect on restart)");
            pushToast(LogLevel::Info, depthCapture
                ? "Depth buffer capture on. Restart to apply."
                : "Depth buffer capture off. Restart to apply.");
        }
        ImGui::TextDisabled("%s", describeActiveDepth(depthInfo).c_str());
        if (cfg.depthCapture != depthCaptureAtStartup)
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
                        emit(ClearDepthPin{});
                }
                else if (!depthInfo.candidates.empty())
                {
                    // unbiassed: the first reported buffer, so the switch is
                    // immediate even before anything has been drawn this frame.
                    emit(PinDepth{defaultPinCandidateForSwapchain(depthInfo)});
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
        // --- MSAA resolve: how a multisampled depth source is reduced to one sample ---
        {
            const char* labels[] = { "Average", "Sample zero" };
            const char* hints[] = {
                "Average all samples of each pixel. Smoother, the default.",
                "Take sample 0 of each pixel. Exact depth values, but edges are not averaged."
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("MSAA resolve");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(240.0f * d);
            int index = cfg.depthResolveMode == 1 ? 1 : 0;
            if (ImGui::Combo("##depth_resolve", &index, labels, 2))
            {
                emitSetting("depthResolveMode", index);
                emit(SettingsChanged{});
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[index]);
        }

        // --- Transforms: one compact row ---
        ImGui::Spacing();
        {
            bool depthInvert = cfg.depthInvert;
            bool depthFlip = cfg.depthFlip;
            bool depthFlop = cfg.depthFlop;
            bool changed = false;

            if (ImGui::Checkbox("Invert", &depthInvert))
            {
                emitSetting("depthInvert", depthInvert);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Invert: depth = 1.0 - depth. Flips the near/far interpretation.");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Flip", &depthFlip))
            {
                emitSetting("depthFlip", depthFlip);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image vertically (uv.y -> 1 - uv.y).");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Mirror", &depthFlop))
            {
                emitSetting("depthFlop", depthFlop);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image horizontally (uv.x -> 1 - uv.x).");

            if (changed)
            {
                emit(SettingsChanged{});
            }
        }

        // --- Footer ---
        ImGui::Spacing();
        if (ImGui::Button("Refresh depth buffers"))
        {
            emit(RefreshDepth{});
            Logger::info("depth re-detect requested from Advanced UI");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear the active buffer, the best candidate and any pin.\nThe layer re-evaluates every render pass next frame.");
    }

    // --- Pushed page: every tracked depth buffer -------------------------------
    void OverlayUi::renderDepthBuffersView()
    {
        const DepthInfo& depthInfo = snapshot->depth;
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
                        emit(ClearDepthPin{});
                }
                else if (!depthInfo.candidates.empty())
                {
                    emit(PinDepth{defaultPinCandidateForSwapchain(depthInfo)});
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hints[mode]);
        }

        ImGui::Spacing();
        if (depthInfo.candidates.empty())
        {
            UI::TextSupport("No depth views tracked yet. Render a frame with depth.");
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
                emit(PinDepth{action.pin});
            else if (action.unpin)
                emit(ClearDepthPin{});
        }
        ImGui::EndChild();
    }

} // namespace VkHook
