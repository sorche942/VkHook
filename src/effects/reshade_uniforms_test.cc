#include "reshade_uniforms.hh"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
    }

    reshadefx::uniform_info makeUniform(const char* name, reshadefx::type::datatype base, uint32_t offset, uint32_t size,
                                        const char* source = nullptr)
    {
        reshadefx::uniform_info info{};
        info.name = name;
        info.type.base = base;
        info.type.rows = info.type.cols = 1;
        info.offset = offset;
        info.size = size;
        if (source)
        {
            reshadefx::annotation annotation{};
            annotation.name = "source";
            annotation.value.string_data = source;
            info.annotations.push_back(annotation);
        }
        return info;
    }
}

int main()
{
    // CompleteRT uses a float framecount for temporal sampling; integer bits
    // reinterpreted as float leave it effectively stuck at frame zero.
    for (auto type : {reshadefx::type::t_float, reshadefx::type::t_int, reshadefx::type::t_uint}) {
        reshadefx::uniform_info info{};
        info.type.base = type;
        info.type.rows = info.type.cols = 1;
        info.offset = sizeof(uint32_t);
        info.size = sizeof(uint32_t);
        reshadefx::annotation source{};
        source.name = "source";
        source.value.string_data = "framecount";
        info.annotations.push_back(source);
        VkHook::FrameCountUniform uniform(info);
        std::array<uint32_t, 3> buffer{0x12345678u, 0, 0x87654321u};
        for (uint32_t frame = 0; frame < 130; ++frame) {
            uniform.update(buffer.data());
            bool correct;
            if (type == reshadefx::type::t_float) {
                float value;
                std::memcpy(&value, &buffer[1], sizeof(value));
                correct = value == static_cast<float>(frame);
                if (!correct)
                    std::cerr << "float framecount at " << frame << " was " << value << '\n';
            } else {
                correct = buffer[1] == frame;
            }
            if (!correct || buffer[0] != 0x12345678u || buffer[2] != 0x87654321u)
                return 1;
        }
    }

    // Sources with no data (and unknown ones) write zeros on every update
    // instead of leaving whatever the buffer held, and are reported by name.
    {
        reshadefx::module module;
        module.total_uniform_size = 32;
        module.uniforms.push_back(makeUniform("count", reshadefx::type::t_uint, 0, 4, "framecount"));
        module.uniforms.push_back(makeUniform("mouse", reshadefx::type::t_float, 8, 8, "mousepoint"));
        module.uniforms.push_back(makeUniform("odd", reshadefx::type::t_float, 16, 4, "no_such_source"));
        module.uniforms.push_back(makeUniform("tooSmall", reshadefx::type::t_float, 20, 4, "date"));
        module.uniforms.push_back(makeUniform("outside", reshadefx::type::t_float, 32, 4, "timer"));
        std::vector<std::string> zeroed;
        auto uniforms = VkHook::createReshadeUniforms(module, &zeroed);
        expect(uniforms.size() == 4, "uniforms outside the buffer are dropped, the rest get a writer");
        expect(zeroed.size() == 3, "three sources are reported as zeroed");
        std::array<uint8_t, 40> buffer;
        for (int frame = 0; frame < 3; ++frame) {
            buffer.fill(0xAB);
            for (auto& uniform : uniforms)
                uniform->update(buffer.data());
            bool zeros = true;
            for (size_t i = 8; i < 16; ++i)
                zeros &= buffer[i] == 0;
            for (size_t i = 16; i < 24; ++i)
                zeros &= buffer[i] == 0;
            expect(zeros, "zeroed sources overwrite stale bytes every update");
            expect(buffer[24] == 0xAB && buffer[4] == 0xAB, "neighbouring bytes are left alone");
        }
    }

    // Editable uniforms are bound once; the per-frame copy follows the live
    // parameter values, and a uniform without a parameter keeps its default.
    {
        reshadefx::module module;
        module.total_uniform_size = 16;
        module.uniforms.push_back(makeUniform("strength", reshadefx::type::t_float, 0, 4));
        module.uniforms.push_back(makeUniform("enabled", reshadefx::type::t_bool, 4, 4));
        auto orphan = makeUniform("orphan", reshadefx::type::t_float, 8, 4);
        orphan.has_initializer_value = true;
        orphan.initializer_value.as_float[0] = 3.0f;
        module.uniforms.push_back(orphan);
        module.uniforms.push_back(makeUniform("frames", reshadefx::type::t_uint, 12, 4, "framecount"));

        VkHook::FloatParam strength;
        strength.name = "strength";
        strength.value = 0.5f;
        VkHook::BoolParam enabled;
        enabled.name = "enabled";
        enabled.value = true;

        std::array<uint32_t, 4> buffer;
        buffer.fill(0xCDCDCDCDu);
        auto bindings = VkHook::buildUniformBindings(
            module, sizeof(buffer), buffer.data(), [&](const std::string& name) -> const VkHook::EffectParam* {
                if (name == "strength")
                    return &strength;
                if (name == "enabled")
                    return &enabled;
                return nullptr;
            });
        expect(bindings.size() == 2, "only uniforms with parameters are bound");
        float orphanValue;
        std::memcpy(&orphanValue, &buffer[2], sizeof(orphanValue));
        expect(orphanValue == 3.0f, "an unbound uniform keeps its shader default");
        expect(buffer[3] == 0xCDCDCDCDu, "source uniforms are not bound");

        VkHook::applyUniformBindings(bindings, buffer.data());
        float value;
        std::memcpy(&value, &buffer[0], sizeof(value));
        expect(value == 0.5f && buffer[1] == VK_TRUE, "bound values are copied");

        strength.value = 0.75f;
        enabled.value = false;
        VkHook::applyUniformBindings(bindings, buffer.data());
        std::memcpy(&value, &buffer[0], sizeof(value));
        expect(value == 0.75f && buffer[1] == VK_FALSE, "edits show up on the next copy without rebuilding");
    }

    if (failures == 0)
        std::cout << "uniform sources, zeroing and bindings behave\n";
    return failures == 0 ? 0 : 1;
}
