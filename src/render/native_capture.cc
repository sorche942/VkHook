#include "native_capture.hh"
#include "logical_device.hh"
#include "logger.hh"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace VKIntox {
bool nativeCaptureEnabled()
{
    static const bool enabled = [] { const char* value = std::getenv("VKINTOX_DDDA_GBUFFER"); return value && std::strcmp(value, "1") == 0; }();
    return enabled;
}

bool nativeProducerShader(const uint32_t* words, size_t bytes)
{
    if (!words || bytes % 4 || bytes < 20 || words[0] != 0x07230203) return false;
    unsigned names = 0;
    bool fragment = false, outputZero = false, half = false, eight = false;
    uint32_t glsl = 0;
    bool spherePacking = false, exponentPacking = false;
    const size_t count = bytes / 4;
    for (size_t i = 5; i < count;) {
        const unsigned length = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!length || length > count - i) return false;
        if (opcode == 11 && length >= 3) {
            const char* text = reinterpret_cast<const char*>(words + i + 2);
            const size_t capacity = (length - 2) * 4;
            const void* end = std::memchr(text, 0, capacity);
            if (end && std::string_view(text, static_cast<const char*>(end) - text) == "GLSL.std.450")
                glsl = words[i + 1];
        }
        if (opcode == 12 && length >= 6 && glsl && words[i + 3] == glsl) {
            spherePacking |= words[i + 4] == 32; // InverseSqrt
            exponentPacking |= words[i + 4] == 30; // Log2
        }
        if (opcode == 15 && length >= 4 && words[i + 1] == 4) fragment = true;
        if (opcode == 71 && length == 4 && words[i + 2] == 30 && words[i + 3] == 0) outputZero = true;
        if (opcode == 43 && length == 4) {
            half |= words[i + 3] == 0x3f000000;
            eight |= words[i + 3] == 0x41000000;
        }
        if (opcode == 6 && length >= 4) { // OpMemberName, NOT arbitrary debug string bytes
            const char* text = reinterpret_cast<const char*>(words + i + 3);
            const size_t capacity = (length - 3) * 4;
            const void* end = std::memchr(text, 0, capacity);
            if (!end) return false;
            const std::string_view name(text, static_cast<const char*>(end) - text);
            const char* suffixes[] = {"_fView0", "_fView1", "_fView2"};
            for (unsigned n = 0; n < 3; ++n) {
                const std::string_view suffix(suffixes[n]);
                if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) names |= 1u << n;
            }
        }
        i += length;
    }
    // Lighting uses fViewI. Alpha-tested/blended producers omit fLightGroup:
    // they write packed RGB while the color-write mask preserves group alpha.
    return names == 7 && fragment && outputZero && half && eight && spherePacking && exponentPacking;
}

NativeSnapshot::~NativeSnapshot()
{
    if (!device) return;
    if (view) device->vkd.DestroyImageView(device->device, view, nullptr);
    if (image) device->vkd.DestroyImage(device->device, image, nullptr);
    if (memory) device->vkd.FreeMemory(device->device, memory, nullptr);
}

static std::shared_ptr<NativeSnapshot> allocateSnapshot(LogicalDevice* device, VkExtent3D extent)
{
    auto result = std::make_shared<NativeSnapshot>();
    result->device = device;
    result->extent = extent;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ci.extent = extent;
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (device->vkd.CreateImage(device->device, &ci, nullptr, &result->image) != VK_SUCCESS) return {};
    VkMemoryRequirements requirements{};
    device->vkd.GetImageMemoryRequirements(device->device, result->image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    device->vki.GetPhysicalDeviceMemoryProperties(device->physicalDevice, &properties);
    uint32_t type = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == properties.memoryTypeCount) return {};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = requirements.size;
    ai.memoryTypeIndex = type;
    if (device->vkd.AllocateMemory(device->device, &ai, nullptr, &result->memory) != VK_SUCCESS
        || device->vkd.BindImageMemory(device->device, result->image, result->memory, 0) != VK_SUCCESS) return {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = result->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (device->vkd.CreateImageView(device->device, &vi, nullptr, &result->view) != VK_SUCCESS) return {};
    return result;
}

void NativeCapture::begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo& info)
{
    auto& command = commands[cb];
    const auto pool = command.pool;
    const auto level = command.level;
    const auto queueFamily = command.queueFamily;
    for (auto& copy : command.copies)
        if (std::find(recycled.begin(), recycled.end(), copy) == recycled.end())
            recycled.push_back(std::move(copy));
    command.copies.clear();
    auto reusableCopies = std::move(command.copies);
    command = {};
    command.copies = std::move(reusableCopies);
    command.pool = pool;
    command.queueFamily = queueFamily;
    command.level = level;
    command.inherited = (info.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0;
}
void NativeCapture::dynamicBegin(VkCommandBuffer cb, const VkRenderingInfo& info)
{
    auto& c = commands[cb];
    c.inScope = true;
    c.drew = false;
    c.attachment = VK_NULL_HANDLE;
    c.rejected = info.flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
    c.rejected |= info.viewMask != 0 || info.layerCount != 1 || info.renderArea.offset.x != 0 || info.renderArea.offset.y != 0;
    c.area = info.renderArea.extent;
    if (info.colorAttachmentCount == 1 && info.pColorAttachments) {
        const auto& a = info.pColorAttachments[0];
        c.attachment = a.imageView;
        c.finalLayout = a.imageLayout;
        c.rejected |= a.storeOp != VK_ATTACHMENT_STORE_OP_STORE || a.resolveMode != VK_RESOLVE_MODE_NONE;
    } else c.rejected = true;
}
void NativeCapture::legacyBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo& info)
{
    auto& c = commands[cb];
    c.inScope = true;
    c.drew = false;
    c.rejected = true;
    c.attachment = VK_NULL_HANDLE;
    c.area = info.renderArea.extent;
    const auto p = passes.find(info.renderPass);
    const auto f = framebuffers.find(info.framebuffer);
    if (p == passes.end() || f == framebuffers.end() || !p->second.usable
        || p->second.attachment >= f->second.attachments.size() || info.renderArea.offset.x || info.renderArea.offset.y) return;
    c.attachment = f->second.attachments[p->second.attachment];
    c.finalLayout = p->second.finalLayout;
    c.rejected = false;
}
void NativeCapture::draw(VkCommandBuffer cb, bool nonempty)
{
    auto& c = commands[cb];
    if (!nonempty) return;
    const uint32_t required = c.dynamicRequired & ~(c.alphaPreserved ? 1u : 0u);
    if (c.producer && (!c.staticBlending || c.alphaPreserved)
        && (c.dynamicKnown & required) == required
        && (c.dynamicValid & required) == required) c.drew = true;
    else if (c.inScope || c.inherited) c.rejected = true;
}
void NativeCapture::execute(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* children)
{
    auto& c = commands[cb];
    for (uint32_t i = 0; i < count; ++i) {
        auto child = commands.find(children[i]);
        if (child == commands.end()) { c.rejected = true; continue; }
        const auto& other = child->second;
        if (c.inScope) {
            c.drew |= other.drew;
            c.rejected |= other.rejected || !other.inherited;
        } else if (!other.inherited) {
            c.copies.insert(c.copies.end(), other.copies.begin(), other.copies.end());
            if (other.last) c.last = other.last;
        }
    }
}
void NativeCapture::end(LogicalDevice* device, VkCommandBuffer cb)
{
    auto found = commands.find(cb);
    if (found == commands.end()) return;
    auto& c = found->second;
    if (!queueSafe || c.queueFamily != queueFamily) {
        c.inScope = false;
        return;
    }
    const bool capture = c.inScope && c.drew && !c.rejected && !c.inherited;
    c.inScope = false;
    if (!capture) return;
    const auto v = views.find(c.attachment);
    if (v == views.end()) return;
    const auto image = images.find(v->second.image);
    if (image == images.end() || image->second.generation != v->second.generation
        || image->second.extent.width != c.area.width || image->second.extent.height != c.area.height
        || c.finalLayout == VK_IMAGE_LAYOUT_UNDEFINED || c.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) return;
    const auto extent = image->second.extent;
    auto matches = [&](const auto& s) { return s && s->extent.width == extent.width && s->extent.height == extent.height; };
    std::shared_ptr<NativeSnapshot> snapshot;
    if (matches(c.last)) snapshot = c.last;
    for (auto it = recycled.begin(); !snapshot && it != recycled.end();) {
        if (it->use_count() != 1) { ++it; continue; }
        if (matches(*it)) snapshot = std::move(*it);
        it = recycled.erase(it); // release obsolete resize generations when safe
    }
    if (!snapshot) snapshot = allocateSnapshot(device, extent);
    if (!snapshot) { Logger::warn("DDDA native G-buffer unavailable: snapshot allocation failed"); return; }
    VkImageMemoryBarrier barriers[2]{};
    for (auto& b : barriers) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].image = image->first;
    barriers[0].oldLayout = c.finalLayout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].image = snapshot->image;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    // ALL_COMMANDS also orders a repeated execution against earlier effect reads
    // of this same owned image on the accepted queue.
    device->vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 2, barriers);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = snapshot->extent;
    device->vkd.CmdCopyImage(cb, image->first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           snapshot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = c.finalLayout;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    device->vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0, 0, nullptr, 0, nullptr, 2, barriers);
    if (snapshot != c.last) c.copies.push_back(snapshot);
    c.last = std::move(snapshot);
}
void NativeCapture::submitted(VkQueue queue, VkCommandBuffer cb)
{
    const auto c = commands.find(cb);
    if (c == commands.end() || !c->second.last) return;
    if (pendingQueue && pendingQueue != queue) queueConflict = true;
    pendingQueue = queue;
    pending = c->second.last;
}
std::shared_ptr<NativeSnapshot> NativeCapture::takeFrame(VkQueue queue)
{
    auto result = !queueConflict && pendingQueue == queue ? std::move(pending) : nullptr;
    pending.reset();
    pendingQueue = VK_NULL_HANDLE;
    queueConflict = false;
    const int valid = result ? 1 : 0;
    if (valid != availability) {
        availability = valid;
        Logger::info(valid ? "DDDA native G-buffer source recognized: submitted producer + BGRA8 attachment snapshot"
                          : "DDDA native G-buffer unavailable: no supported producer submitted this frame (depth-normal fallback)");
    }
    return result;
}
void NativeCapture::erasePool(VkCommandPool pool, bool destroy)
{
    for (auto it = commands.begin(); it != commands.end();) {
        if (it->second.pool != pool) { ++it; continue; }
        if (destroy) it = commands.erase(it);
        else { begin(it->first, {}); ++it; }
    }
}
}
