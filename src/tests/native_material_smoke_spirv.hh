#pragma once
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

// Original sphere-normal/log-exponent fragment shader. Unused struct member
// names reproduce producer metadata without including game bytecode.
inline std::vector<uint32_t> nativeMaterialSmokeSpirv()
{
    std::vector<uint32_t> code = {
        0x07230203, 0x00010000, 0, 27, 0,
        0x00020011, 1,
        0x0006000b, 17, 0x4c534c47, 0x6474732e, 0x3035342e, 0,
        0x0003000e, 0, 1,
        0x0006000f, 4, 10, 0x6e69616d, 0, 6,
        0x00030010, 10, 7
    };
    const char* names[] = {"c1_fView0", "c2_fView1", "c3_fView2", "c4_fLightGroup"};
    for (uint32_t i = 0; i < 4; ++i) {
        const size_t words = (std::strlen(names[i]) + 1 + 3) / 4;
        code.push_back(uint32_t((words + 3) << 16) | 6);
        code.push_back(16);
        code.push_back(i);
        const size_t start = code.size();
        code.resize(start + words, 0);
        std::memcpy(code.data() + start, names[i], std::strlen(names[i]));
    }
    const uint32_t body[] = {
        0x00040047, 6, 30, 0,
        0x00020013, 1,
        0x00030021, 2, 1,
        0x00030016, 3, 32,
        0x00040017, 4, 3, 4,
        0x0006001e, 16, 4, 4, 4, 4,
        0x00040020, 5, 3, 4,
        0x0004003b, 5, 6, 3,
        0x0004002b, 3, 7, 0x3f000000, // normal R/G = .5
        0x0004002b, 3, 13, 0x3b808081, // light group = 1/255
        0x0004002b, 3, 14, 0x41000000, // producer packing constant 8
        0x0004002b, 3, 18, 0,          // normal x/y = 0
        0x0004002b, 3, 19, 0x42000000, // exponent = 32
        0x0004002b, 3, 20, 0x3db17218, // ln(2)/8
        0x00050036, 1, 10, 0, 2,
        0x000200f8, 11,
        0x0006000c, 3, 21, 17, 32, 14, // inverse sqrt(8)
        0x00050085, 3, 22, 18, 21,     // x/y * inverse sqrt
        0x00050081, 3, 23, 22, 7,      // packed normal .5
        0x0006000c, 3, 24, 17, 30, 19, // log2(32)
        0x00050085, 3, 25, 24, 20,     // ln(32)/8
        0x00070050, 4, 26, 23, 23, 25, 13,
        0x0003003e, 6, 26,
        0x000100fd,
        0x00010038
    };
    code.insert(code.end(), std::begin(body), std::end(body));
    return code;
}
