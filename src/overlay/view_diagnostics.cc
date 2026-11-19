#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "input_blocker.hh"
#include "wayland_display.hh"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <memory>
#include <mutex>

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    namespace
    {
        // Ring buffer for storing history
        template<typename T, size_t N>
        class RingBuffer
        {
        public:
            void push(T value)
            {
                data[writeIndex] = value;
                writeIndex = (writeIndex + 1) % N;
                if (count < N)
                    count++;
            }

            T get(size_t i) const
            {
                if (i >= count)
                    return T{};
                size_t idx = (writeIndex + N - count + i) % N;
                return data[idx];
            }

            size_t size() const { return count; }
            static constexpr size_t capacity() { return N; }

            T min() const
            {
                if (count == 0) return T{};
                T m = get(0);
                for (size_t i = 1; i < count; i++)
                    m = std::min(m, get(i));
                return m;
            }

            T max() const
            {
                if (count == 0) return T{};
                T m = get(0);
                for (size_t i = 1; i < count; i++)
                    m = std::max(m, get(i));
                return m;
            }

            T avg() const
            {
                if (count == 0) return T{};
                T sum = T{};
                for (size_t i = 0; i < count; i++)
                    sum += get(i);
                return sum / static_cast<T>(count);
            }

            // value at proportion p of the sorted samples (p=0.99 -> 99th
            // percentile). used for frametime lows.
            T percentile(float p) const
            {
                if (count == 0) return T{};
                T sorted[N];
                for (size_t i = 0; i < count; i++)
                    sorted[i] = get(i);
                std::sort(sorted, sorted + count);
                const int last = static_cast<int>(count) - 1;
                const int idx = std::clamp(static_cast<int>(last * p + 0.5f), 0, last);
                return sorted[idx];
            }

            // Get data as contiguous array for ImGui plotting
            void copyTo(float* out) const
            {
                for (size_t i = 0; i < count; i++)
                    out[i] = static_cast<float>(get(i));
            }

        private:
            T data[N] = {};
            size_t writeIndex = 0;
            size_t count = 0;
        };

        // ── GPU vendor detection ────────────────────────────────────────────

        enum class GpuVendor { Unknown, AMD, Intel, NVIDIA };

        struct GpuInfo
        {
            GpuVendor vendor = GpuVendor::Unknown;
            std::string drmCardPath;      // /sys/class/drm/cardN
            std::string vendorName;       // Display name
            bool hasGpuUsage = false;
            bool hasVram = false;
            bool hasGtt = false;
        };

        // ── NVIDIA NVML (runtime dlopen) ────────────────────────────────────

        // NVML types (from nvml.h, but we don't require the header)
        using nvmlReturn_t = unsigned int;
        using nvmlDevice_t = void*;
        struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
        struct nvmlMemory_t { unsigned long long total; unsigned long long free; unsigned long long used; };

        static constexpr nvmlReturn_t NVML_SUCCESS = 0;

        struct NvmlState
        {
            void* lib = nullptr;
            nvmlDevice_t device = nullptr;
            bool initialized = false;

            // Function pointers
            nvmlReturn_t (*Init)() = nullptr;
            nvmlReturn_t (*Shutdown)() = nullptr;
            nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*) = nullptr;
            nvmlReturn_t (*DeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
            nvmlReturn_t (*DeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
            nvmlReturn_t (*DeviceGetCount)(unsigned int*) = nullptr;
        };

        static NvmlState nvml;

        static bool initNvml()
        {
            // NVML state is process-wide; two overlays may race to load it
            static std::mutex nvmlMutex;
            std::lock_guard<std::mutex> lock(nvmlMutex);
            if (nvml.initialized)
                return nvml.lib != nullptr;
            nvml.initialized = true;

            nvml.lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
            if (!nvml.lib)
                nvml.lib = dlopen("libnvidia-ml.so", RTLD_LAZY);
            if (!nvml.lib)
            {
                Logger::debug("NVML not available: " + std::string(dlerror()));
                return false;
            }

            nvml.Init = (decltype(nvml.Init))dlsym(nvml.lib, "nvmlInit_v2");
            if (!nvml.Init)
                nvml.Init = (decltype(nvml.Init))dlsym(nvml.lib, "nvmlInit");
            nvml.Shutdown = (decltype(nvml.Shutdown))dlsym(nvml.lib, "nvmlShutdown");
            nvml.DeviceGetHandleByIndex = (decltype(nvml.DeviceGetHandleByIndex))dlsym(nvml.lib, "nvmlDeviceGetHandleByIndex_v2");
            if (!nvml.DeviceGetHandleByIndex)
                nvml.DeviceGetHandleByIndex = (decltype(nvml.DeviceGetHandleByIndex))dlsym(nvml.lib, "nvmlDeviceGetHandleByIndex");
            nvml.DeviceGetUtilizationRates = (decltype(nvml.DeviceGetUtilizationRates))dlsym(nvml.lib, "nvmlDeviceGetUtilizationRates");
            nvml.DeviceGetMemoryInfo = (decltype(nvml.DeviceGetMemoryInfo))dlsym(nvml.lib, "nvmlDeviceGetMemoryInfo");
            nvml.DeviceGetCount = (decltype(nvml.DeviceGetCount))dlsym(nvml.lib, "nvmlDeviceGetCount_v2");
            if (!nvml.DeviceGetCount)
                nvml.DeviceGetCount = (decltype(nvml.DeviceGetCount))dlsym(nvml.lib, "nvmlDeviceGetCount");

            if (!nvml.Init || !nvml.DeviceGetHandleByIndex || !nvml.DeviceGetUtilizationRates || !nvml.DeviceGetMemoryInfo)
            {
                Logger::debug("NVML: missing required symbols");
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            if (nvml.Init() != NVML_SUCCESS)
            {
                Logger::debug("NVML: nvmlInit failed");
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            // Get first GPU handle (index 0)
            if (nvml.DeviceGetHandleByIndex(0, &nvml.device) != NVML_SUCCESS)
            {
                Logger::debug("NVML: could not get device handle");
                nvml.Shutdown();
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            Logger::info("NVML initialized for GPU diagnostics");
            return true;
        }

        // ── GPU discovery ───────────────────────────────────────────────────

        // Read PCI vendor ID from DRM card sysfs
        static uint16_t readVendorId(const std::string& cardPath)
        {
            std::ifstream f(cardPath + "/device/vendor");
            if (!f.is_open())
                return 0;
            uint16_t id = 0;
            f >> std::hex >> id;
            return id;
        }

        static GpuInfo findGpu(const DeviceInfo& device)
        {
            GpuInfo info;

            // the DRM node for the slot the game is actually on. on hybrid
            // laptops the first cardN is usually the iGPU, which is why this
            // used to report Intel while the game rendered on the dGPU.
            std::error_code ec;
            std::string wantedCard;
            if (!device.gpuPciSlot.empty())
            {
                for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec))
                {
                    std::string name = entry.path().filename().string();
                    if (name.find("card") != 0 || name.find("-") != std::string::npos)
                        continue;
                    std::error_code linkEc;
                    std::string target = std::filesystem::canonical(entry.path() / "device", linkEc).string();
                    const size_t slash = target.rfind('/');
                    if (slash != std::string::npos && target.substr(slash + 1) == device.gpuPciSlot)
                    {
                        wantedCard = entry.path().string();
                        break;
                    }
                }
            }

            try
            {
                for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm"))
                {
                    std::string name = entry.path().filename().string();
                    if (name.find("card") != 0 || name.find("-") != std::string::npos)
                        continue;

                    std::string cardPath = entry.path().string();
                    // prefer the slot the game is on; fall back to any supported
                    // card only when the slot can't be resolved
                    if (!wantedCard.empty() && cardPath != wantedCard)
                        continue;

                    uint16_t vendorId = readVendorId(cardPath);
                    if (!wantedCard.empty() && vendorId != device.gpuVendorId)
                        continue;

                    // 0x1002 = AMD, 0x8086 = Intel, 0x10de = NVIDIA
                    if (vendorId == 0x1002)
                    {
                        // AMD: check for gpu_busy_percent
                        if (std::filesystem::exists(cardPath + "/device/gpu_busy_percent"))
                        {
                            info.vendor = GpuVendor::AMD;
                            info.drmCardPath = cardPath;
                            info.vendorName = "AMD";
                            info.hasGpuUsage = true;
                            info.hasVram = std::filesystem::exists(cardPath + "/device/mem_info_vram_total");
                            info.hasGtt = std::filesystem::exists(cardPath + "/device/mem_info_gtt_total");
                            return info;
                        }
                    }
                    else if (vendorId == 0x8086)
                    {
                        // Intel: use frequency ratio as GPU utilization estimate
                        info.vendor = GpuVendor::Intel;
                        info.drmCardPath = cardPath;
                        info.vendorName = "Intel";
                        info.hasGpuUsage = std::filesystem::exists(cardPath + "/device/gt_act_freq_mhz") &&
                                           std::filesystem::exists(cardPath + "/device/gt_max_freq_mhz");
                        // Intel discrete (Arc) may have VRAM via drm_memory_stats
                        info.hasVram = std::filesystem::exists(cardPath + "/device/mem_info_vram_total");
                        info.hasGtt = false;
                        return info;
                    }
                    else if (vendorId == 0x10de)
                    {
                        // NVIDIA: use NVML (runtime dlopen)
                        info.vendor = GpuVendor::NVIDIA;
                        info.drmCardPath = cardPath;
                        info.vendorName = "NVIDIA";
                        if (initNvml())
                        {
                            info.hasGpuUsage = true;
                            info.hasVram = true;
                        }
                        return info;
                    }
                }
            }
            catch (...) {}

            return info;
        }

        // Read a single value from sysfs
        template<typename T>
        bool readSysfs(const std::string& path, T& value)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            file >> value;
            return !file.fail();
        }

        // ── Per-vendor stat readers ─────────────────────────────────────────

        float getGpuUsage(const GpuInfo& gpuInfo)
        {
            if (gpuInfo.vendor == GpuVendor::AMD)
            {
                int usage = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/gpu_busy_percent", usage))
                    return static_cast<float>(usage);
            }
            else if (gpuInfo.vendor == GpuVendor::Intel)
            {
                // Frequency ratio: (actual / max) * 100 as utilization estimate
                int actFreq = 0, maxFreq = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/gt_act_freq_mhz", actFreq) &&
                    readSysfs(gpuInfo.drmCardPath + "/device/gt_max_freq_mhz", maxFreq) &&
                    maxFreq > 0)
                {
                    return std::min(100.0f, (static_cast<float>(actFreq) / static_cast<float>(maxFreq)) * 100.0f);
                }
            }
            else if (gpuInfo.vendor == GpuVendor::NVIDIA && nvml.lib)
            {
                nvmlUtilization_t util = {};
                if (nvml.DeviceGetUtilizationRates(nvml.device, &util) == NVML_SUCCESS)
                    return static_cast<float>(util.gpu);
            }

            return -1.0f;
        }

        bool getVramUsage(const GpuInfo& gpuInfo, float& usedMB, float& totalMB)
        {
            if (gpuInfo.vendor == GpuVendor::AMD || gpuInfo.vendor == GpuVendor::Intel)
            {
                uint64_t used = 0, total = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/mem_info_vram_used", used) &&
                    readSysfs(gpuInfo.drmCardPath + "/device/mem_info_vram_total", total) &&
                    total > 0)
                {
                    usedMB = static_cast<float>(used) / (1024.0f * 1024.0f);
                    totalMB = static_cast<float>(total) / (1024.0f * 1024.0f);
                    return true;
                }
            }
            else if (gpuInfo.vendor == GpuVendor::NVIDIA && nvml.lib)
            {
                nvmlMemory_t mem = {};
                if (nvml.DeviceGetMemoryInfo(nvml.device, &mem) == NVML_SUCCESS && mem.total > 0)
                {
                    usedMB = static_cast<float>(mem.used) / (1024.0f * 1024.0f);
                    totalMB = static_cast<float>(mem.total) / (1024.0f * 1024.0f);
                    return true;
                }
            }

            return false;
        }

        bool getGttUsage(const GpuInfo& gpuInfo, float& usedMB, float& totalMB)
        {
            if (gpuInfo.vendor != GpuVendor::AMD || gpuInfo.drmCardPath.empty())
                return false;

            uint64_t used = 0, total = 0;
            if (readSysfs(gpuInfo.drmCardPath + "/device/mem_info_gtt_used", used) &&
                readSysfs(gpuInfo.drmCardPath + "/device/mem_info_gtt_total", total) &&
                total > 0)
            {
                usedMB = static_cast<float>(used) / (1024.0f * 1024.0f);
                totalMB = static_cast<float>(total) / (1024.0f * 1024.0f);
                return true;
            }

            return false;
        }

        // Helper to draw a graph with label
        void drawGraph(const char* label, const char* id, RingBuffer<float, 300>& history, float minVal, float maxVal,
                       const char* overlayFmt, ImVec4 color = UI::Primary())
        {
            ImGui::Text("%s", label);

            // Get data for plotting
            float data[300];
            history.copyTo(data);

            ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::ContainerLow());

            char overlay[64];
            snprintf(overlay, sizeof(overlay), overlayFmt, history.size() > 0 ? history.get(history.size() - 1) : 0.0f);

            ImGui::PlotLines(id, data, static_cast<int>(history.size()), 0, overlay,
                            minVal, maxVal, ImVec2(-1, ImGui::GetTextLineHeight() * 4.0f));

            ImGui::PopStyleColor(2);

            // Stats below graph
            if (history.size() > 0)
            {
                ImGui::TextDisabled("Min: %.1f  Avg: %.1f  Max: %.1f",
                    history.min(), history.avg(), history.max());
            }
        }
    }

    // Everything the diagnostics tab samples. One per overlay (owned through
    // ImGuiOverlay::diagnostics), so two overlays never share history.
    struct DiagnosticsState
    {
        RingBuffer<float, 300> frameTimeHistory;
        RingBuffer<float, 300> gpuUsageHistory;
        RingBuffer<float, 300> vramUsageHistory;
        RingBuffer<float, 300> gttUsageHistory;
        std::chrono::steady_clock::time_point lastFrameTime;
        GpuInfo gpuInfo;
        std::string detectedGameName;
        std::string autoDetectedConfig;

        // Sampling runs every overlay frame from launch, not just while the tab
        // is open, so the graph and FPS are already populated the first time the
        // user opens Diagnostics.
        bool initialized = false;
        std::string resolvedSlot;
        std::chrono::steady_clock::time_point lastGpuSampleTime = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point fpsWindowStart = std::chrono::steady_clock::now();
        int   fpsFrameCount = 0;
        float dispFps = 0.0f;
        float dispFps1Low = 0.0f;
        float dispFps01Low = 0.0f;
        float dispGpuUsage = -1.0f;
        float dispVramUsed = 0.0f, dispVramTotal = 0.0f;
        float dispGttUsed = 0.0f, dispGttTotal = 0.0f;
        bool  dispHasVram = false, dispHasGtt = false;
    };

    void ImGuiOverlay::sampleDiagnostics()
    {
        if (!diagnostics)
            diagnostics = std::make_shared<DiagnosticsState>();
        DiagnosticsState& d = *diagnostics;
        const auto now = std::chrono::steady_clock::now();

        if (!d.initialized || d.resolvedSlot != deviceInfo.gpuPciSlot)
        {
            if (d.initialized)
                Logger::info("Diagnostics: GPU changed to " + deviceInfo.gpuName + ", re-resolving");
            d.gpuInfo = findGpu(deviceInfo);
            d.resolvedSlot = deviceInfo.gpuPciSlot;
            d.detectedGameName = ConfigSerializer::detectGameName();
            d.autoDetectedConfig = ConfigSerializer::autoDetectConfig();
            d.lastFrameTime = now;
            d.fpsWindowStart = now;
            d.fpsFrameCount = 0;
            d.initialized = true;

            if (d.gpuInfo.vendor != GpuVendor::Unknown)
                Logger::info("Diagnostics: Found " + d.gpuInfo.vendorName + " GPU at " + d.gpuInfo.drmCardPath);
            else
                Logger::info("Diagnostics: No supported GPU found");
        }

        // measured every overlay frame from launch, not only while the tab is open
        const float frameTimeMs = std::chrono::duration<float, std::milli>(now - d.lastFrameTime).count();
        d.lastFrameTime = now;
        if (frameTimeMs > 0.1f && frameTimeMs < 500.0f)
            d.frameTimeHistory.push(frameTimeMs);

        d.fpsFrameCount++;
        const double windowSeconds = std::chrono::duration<double>(now - d.fpsWindowStart).count();
        if (windowSeconds >= 1.0)
        {
            d.dispFps = static_cast<float>(d.fpsFrameCount) / static_cast<float>(windowSeconds);
            // frametime percentiles: the worst 1% / 0.1% of frames expressed as FPS.
            const float p99  = d.frameTimeHistory.percentile(0.99f);
            const float p999 = d.frameTimeHistory.percentile(0.999f);
            d.dispFps1Low  = p99  > 0.0f ? 1000.0f / p99  : 0.0f;
            d.dispFps01Low = p999 > 0.0f ? 1000.0f / p999 : 0.0f;
            d.dispGpuUsage = d.gpuInfo.hasGpuUsage ? getGpuUsage(d.gpuInfo) : -1.0f;
            d.dispHasVram = getVramUsage(d.gpuInfo, d.dispVramUsed, d.dispVramTotal);
            d.dispHasGtt = getGttUsage(d.gpuInfo, d.dispGttUsed, d.dispGttTotal);
            // keep the leftover rather than snapping to exactly 1.0s, which under-counts.
            d.fpsWindowStart += std::chrono::milliseconds(static_cast<int64_t>(windowSeconds * 1000.0));
            d.fpsFrameCount = 0;
        }

        // Sample GPU stats at a fixed wall-clock interval (~200ms) so overhead
        // stays constant regardless of frame rate (not "every 10 frames" which
        // would over-poll at high FPS and under-poll at low FPS).
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - d.lastGpuSampleTime).count() >= 200)
        {
            d.lastGpuSampleTime = now;

            const float gpuUsage = getGpuUsage(d.gpuInfo);
            if (gpuUsage >= 0)
                d.gpuUsageHistory.push(gpuUsage);

            float vramUsed, vramTotal;
            if (getVramUsage(d.gpuInfo, vramUsed, vramTotal))
                d.vramUsageHistory.push((vramUsed / vramTotal) * 100.0f);

            float gttUsed, gttTotal;
            if (getGttUsage(d.gpuInfo, gttUsed, gttTotal))
                d.gttUsageHistory.push((gttUsed / gttTotal) * 100.0f);
        }
    }

    void ImGuiOverlay::renderDiagnosticsView()
    {
        if (!diagnostics)
            diagnostics = std::make_shared<DiagnosticsState>();
        DiagnosticsState& d = *diagnostics;
        if (ImGui::Button("Back to settings"))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();

        ImGui::BeginChild("DiagnosticsContent", ImVec2(0, 0), false);

        // FPS is sampled every frame in sampleDiagnostics(); this view only reads
        // the latest values, so opening the tab shows real history, not zeros.
        const float fps = d.dispFps;
        const float fps1Low = d.dispFps1Low;
        const float fps01Low = d.dispFps01Low;

        // --- Performance ---
        ImGui::SeparatorText("Performance");
        {
            ImGui::TextColored(UI::Success(), "%.0f FPS", fps);
            ImGui::SameLine();
            ImGui::TextDisabled("(1%% low: %.0f   ·   0.1%% low: %.0f)", fps1Low, fps01Low);
        }
        ImGui::Spacing();
        drawGraph("Frame Time", "##frametime", d.frameTimeHistory, 0.0f, 50.0f, "%.1f ms", UI::Primary());

        // --- GPU ---
        ImGui::Spacing();
        ImGui::SeparatorText("GPU");
        {
            // report what the game is really running on, not a vendor guessed
            // from DRM card order
            if (!deviceInfo.gpuName.empty())
            {
                ImGui::TextUnformatted(deviceInfo.gpuName.c_str());

                std::string info;
                if (deviceInfo.gpuApiVersion)
                    info = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(deviceInfo.gpuApiVersion)) + "." +
                           std::to_string(VK_API_VERSION_MINOR(deviceInfo.gpuApiVersion)) + "." +
                           std::to_string(VK_API_VERSION_PATCH(deviceInfo.gpuApiVersion));
                if (!deviceInfo.gpuDriverInfo.empty())
                    info += info.empty() ? deviceInfo.gpuDriverInfo : "  ·  " + deviceInfo.gpuDriverInfo;
                if (!info.empty())
                {
                    ImGui::TextDisabled("%s", info.c_str());
                }
            }
            else if (d.gpuInfo.vendor != GpuVendor::Unknown)
            {
                UI::TextSupport(d.gpuInfo.vendorName.c_str());
            }
            ImGui::Spacing();

            if (d.gpuInfo.vendor != GpuVendor::Unknown && d.gpuInfo.hasGpuUsage)
            {
                if (d.dispGpuUsage >= 0)
                {
                    const char* usageLabel = (d.gpuInfo.vendor == GpuVendor::Intel) ? "GPU Frequency" : "GPU Usage";
                    drawGraph(usageLabel, "##gpuusage", d.gpuUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::Secondary());
                    if (d.gpuInfo.vendor == GpuVendor::Intel)
                    {
                        ImGui::TextDisabled("(estimated from frequency ratio)");
                    }
                    ImGui::Spacing();
                }
            }

            if (d.dispHasVram)
            {
                ImGui::Text("VRAM: %.0f / %.0f MB", d.dispVramUsed, d.dispVramTotal);
                ImGui::ProgressBar(d.dispVramUsed / d.dispVramTotal, ImVec2(-1, 0));
                ImGui::Spacing();
            }

            if (d.dispHasGtt)
            {
                ImGui::Text("GTT (shared): %.0f / %.0f MB", d.dispGttUsed, d.dispGttTotal);
                ImGui::ProgressBar(d.dispGttUsed / d.dispGttTotal, ImVec2(-1, 0));
                ImGui::Spacing();
                drawGraph("Memory Usage", "##gttusage", d.gttUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::Tertiary());
            }
            else if (d.dispHasVram)
            {
                drawGraph("VRAM Usage", "##vramusage", d.vramUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::Tertiary());
            }
        }
        if (deviceInfo.gpuName.empty())
        {
            ImGui::TextDisabled("GPU stats not available.");
            ImGui::TextDisabled("No AMD/Intel/NVIDIA GPU detected via sysfs.");
        }

        // --- Game ---
        ImGui::Spacing();
        ImGui::SeparatorText("Game");
        if (!d.detectedGameName.empty())
        {
            ImGui::Text("Executable: %s", d.detectedGameName.c_str());
            if (!d.autoDetectedConfig.empty())
                ImGui::TextColored(UI::Success(), "Config: %s.conf (auto-detected)", d.autoDetectedConfig.c_str());
            else
                UI::TextSupport("No per-game config found.");
        }
        else
        {
            UI::TextSupport("Could not detect the game executable.");
        }

        // --- Input ---
        ImGui::Spacing();
        ImGui::SeparatorText("Input");
        {
            const SurfaceKind kind = getSurfaceKind();
            const bool wayland = kind == SurfaceKind::Wayland;
            const bool x11 = kind == SurfaceKind::X11;

            ImGui::Text("Backend:");
            ImGui::SameLine();
            if (wayland || x11)
                ImGui::TextColored(UI::Success(), "%s", wayland ? "Wayland" : "X11");
            else
                ImGui::TextColored(UI::Warning(), "Unknown (unsupported surface)");

            const bool enabled = inputBlockingEnabled();
            ImGui::Text("Blocking:");
            ImGui::SameLine();
            if (enabled)
                ImGui::TextColored(UI::Success(), "enabled");
            else
                ImGui::TextDisabled("disabled (see Settings)");

            if (wayland)
            {
                ImGui::Text("Keys withheld from the game:");
                ImGui::SameLine();
                if (inputKeyboardWithheld())
                    ImGui::TextColored(UI::Success(), "yes");
                else
                    ImGui::TextDisabled("no");
            }
            else if (x11)
            {
                ImGui::Text("Grab held:");
                ImGui::SameLine();
                if (inputGrabActive())
                    ImGui::TextColored(UI::Success(), "yes");
                else
                    ImGui::TextDisabled("no");
            }

            ImGui::Text("Overlay on screen:");
            ImGui::SameLine();
            if (isInputBlocked())
                ImGui::TextColored(UI::Success(), "yes");
            else
                ImGui::TextDisabled("no");

            if (enabled)
            {
                ImGui::Text("Pointer over overlay:");
                ImGui::SameLine();
                if (inputPointerOverOverlay())
                    ImGui::TextColored(UI::Success(), "yes");
                else
                    ImGui::TextDisabled("no");
            }
        }

        ImGui::EndChild();
    }

} // namespace VkHook
