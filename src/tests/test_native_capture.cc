#include "native_capture.hh"
#include "native_material_smoke_spirv.hh"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace VKIntox;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static bool recognized(const std::vector<uint32_t>& code) { return nativeProducerShader(code.data(), code.size() * 4); }
static std::vector<uint32_t> load(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "capture shader file unavailable");
    const auto size = file.tellg();
    require(size >= 0 && size % 4 == 0, "invalid capture shader size");
    std::vector<uint32_t> code(size_t(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    require(bool(file), "capture shader read failed");
    return code;
}
int main(int argc, char** argv)
{
    try {
        auto code = nativeMaterialSmokeSpirv();
        require(recognized(code), "original producer fixture not recognized");
        auto alphaProducer = code;
        for (size_t i = 5; i < alphaProducer.size(); i += alphaProducer[i] >> 16)
            if ((alphaProducer[i] & 0xffff) == 6) {
                char* name = reinterpret_cast<char*>(alphaProducer.data() + i + 3);
                if (std::strstr(name, "_fLightGroup")) name[0] = '\0';
            }
        require(recognized(alphaProducer), "RGB-only producer without light-group uniform rejected");
        auto missingPacking = code;
        for (size_t i = 5; i < missingPacking.size(); i += missingPacking[i] >> 16)
            if ((missingPacking[i] & 0xffff) == 12 && missingPacking[i + 4] == 32)
                missingPacking[i + 4] = 31; // sqrt is not DDDA sphere packing
        require(!recognized(missingPacking), "view transform without sphere packing accepted");
        require(!nativeProducerShader(nullptr, 0), "empty SPIR-V accepted");
        require(!nativeProducerShader(code.data(), code.size() * 4 - 1), "unaligned SPIR-V accepted");
        auto malformed = code;
        malformed[5] = 0;
        require(!recognized(malformed), "zero-length SPIR-V instruction accepted");
        malformed = code;
        malformed[5] = 0xffff0011;
        require(!recognized(malformed), "overrun SPIR-V instruction accepted");
        auto consumer = code;
        for (size_t i = 5; i < consumer.size(); i += consumer[i] >> 16) {
            if ((consumer[i] & 0xffff) == 6) {
                char* name = reinterpret_cast<char*>(consumer.data() + i + 3);
                if (auto suffix = std::strstr(name, "_fView")) suffix[6] = 'I';
            }
        }
        require(!recognized(consumer), "lighting consumer fViewI names accepted as producer");
        auto unnamed = code;
        for (size_t i = 5; i < unnamed.size(); i += unnamed[i] >> 16)
            if ((unnamed[i] & 0xffff) == 6) unnamed[i] = (unnamed[i] & 0xffff0000) | 5;
        require(!recognized(unnamed), "non-member debug strings accepted");
        if (argc != 1) {
            require(argc == 4, "usage: test_native_capture [producer34117.spv producer33495.spv scene33714.spv]");
            require(recognized(load(argv[1])), "captured producer34117 rejected");
            require(recognized(load(argv[2])), "captured producer33495 rejected");
            require(!recognized(load(argv[3])), "captured scene writer33714 falsely accepted");
        }
        const auto cb = reinterpret_cast<VkCommandBuffer>(uintptr_t(1));
        const auto secondary = reinterpret_cast<VkCommandBuffer>(uintptr_t(2));
        const auto queue = reinterpret_cast<VkQueue>(uintptr_t(3));
        const auto otherQueue = reinterpret_cast<VkQueue>(uintptr_t(4));
        NativeCapture state;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        state.begin(cb, begin);
        auto snapshot = std::make_shared<NativeSnapshot>();
        snapshot->extent = {640, 480, 1};
        state.commands[cb].copies.push_back(snapshot);
        state.commands[cb].last = snapshot;
        state.submitted(queue, cb);
        auto presented = state.takeFrame(queue);
        require(presented == snapshot, "submitted snapshot not published");
        require(!state.takeFrame(queue), "missing frame reused stale snapshot");
        state.submitted(queue, cb);
        state.submitted(otherQueue, cb);
        require(!state.takeFrame(queue), "cross-queue ambiguous source accepted");
        state.submitted(otherQueue, cb);
        require(!state.takeFrame(queue), "wrong queue accepted");
        std::weak_ptr<NativeSnapshot> lifetime = snapshot;
        snapshot.reset();
        state.begin(cb, begin);
        require(!state.commands[cb].last, "CB begin retained stale publication");
        require(!lifetime.expired(), "CB reset destroyed descriptor-owned snapshot");
        state.recycled.clear();
        require(!lifetime.expired(), "recycle retirement destroyed live presentation");
        presented.reset();
        require(lifetime.expired(), "snapshot lifetime leaked after owners retired");
        {
            NativeCapture recycling;
            recycling.begin(cb, begin);
            recycling.begin(secondary, begin);
            auto captured = std::make_shared<NativeSnapshot>();
            recycling.commands[secondary].copies.push_back(captured);
            recycling.commands[secondary].last = captured;
            recycling.execute(cb, 1, &secondary);
            captured.reset();
            recycling.begin(cb, begin);
            recycling.begin(secondary, begin);
            // A snapshot referenced by primary and secondary CBs must become
            // reusable, not remain pinned by duplicate entries in the pool.
            require(recycling.recycled.size() == 1 && recycling.recycled.front().use_count() == 1,
                    "secondary snapshot cannot be recycled after both command buffers reset");
        }
        begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        state.begin(secondary, begin);
        state.commands[secondary].producer = true;
        state.draw(secondary, true);
        state.commands[cb].inScope = true;
        state.execute(cb, 1, &secondary);
        require(state.commands[cb].drew && !state.commands[cb].rejected, "secondary producer identity lost");
        state.commands[secondary].producer = false;
        state.draw(secondary, true);
        state.execute(cb, 1, &secondary);
        require(state.commands[cb].rejected, "mixed secondary scope accepted");
        begin.flags = 0;
        state.begin(cb, begin);
        auto& c = state.commands[cb];
        c.inScope = c.producer = true;
        c.dynamicRequired = 3;
        state.draw(cb, true);
        require(c.rejected && !c.drew, "unknown dynamic blend state accepted");
        state.begin(cb, begin);
        c.inScope = c.producer = true;
        c.dynamicRequired = c.dynamicKnown = c.dynamicValid = 3;
        state.draw(cb, true);
        require(c.drew && !c.rejected, "verified dynamic output rejected");
        state.begin(cb, begin);
        c.inScope = c.producer = c.staticBlending = true;
        state.draw(cb, true);
        require(c.rejected && !c.drew, "blended group alpha accepted");
        state.begin(cb, begin);
        c.inScope = c.producer = c.staticBlending = c.alphaPreserved = true;
        state.draw(cb, true);
        require(c.drew && !c.rejected, "RGB-only blending that preserves group alpha rejected");
        state.begin(cb, begin);
        c.inScope = c.producer = c.alphaPreserved = true;
        c.dynamicRequired = c.dynamicKnown = 3;
        c.dynamicValid = 2; // RGB mask valid, blending enabled.
        state.draw(cb, true);
        require(c.drew && !c.rejected, "dynamic RGB blending rejected");
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.flags = VK_RENDERING_SUSPENDING_BIT;
        rendering.layerCount = 1;
        state.dynamicBegin(cb, rendering);
        require(c.rejected, "suspended rendering accepted for nested copy");
        state.end(nullptr, cb); // unsupported scope must not issue any Vulkan calls
        require(!c.inScope, "end did not retire scope");
        state.queueSafe = true;
        state.queueFamily = 4;
        c.queueFamily = 3;
        c.inScope = c.drew = true;
        c.rejected = false;
        state.end(nullptr, cb);
        require(!c.inScope && !c.last, "wrong queue-family scope copied");
        const auto pool = (VkCommandPool)uintptr_t(9);
        c.pool = pool;
        c.queueFamily = 4;
        c.last = std::make_shared<NativeSnapshot>();
        c.copies.push_back(c.last);
        state.erasePool(pool, false);
        require(!c.last && c.pool == pool && c.queueFamily == 4, "pool reset retained stale snapshot or lost family");
        state.erasePool(pool, true);
        require(!state.commands.count(cb), "pool destruction retained command-buffer state");
        std::cout << "PASS native signature, submission freshness, lifetime, reuse, secondary and dynamic-state regressions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
