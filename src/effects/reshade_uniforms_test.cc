#include "reshade_uniforms.hh"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

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
        VKIntox::FrameCountUniform uniform(info);
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
    std::cout << "float, int and uint framecount advance numerically without overwriting adjacent uniforms\n";
}
