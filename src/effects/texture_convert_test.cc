#include "texture_convert.hh"

#include <cstdio>
#include <string>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }
}

int main()
{
    // Texel sizes come from the format, including the ones wider than 8 bits.
    expect(formatTexelSize(VK_FORMAT_R8_UNORM) == 1, "R8 texel");
    expect(formatTexelSize(VK_FORMAT_R8G8_UNORM) == 2, "RG8 texel");
    expect(formatTexelSize(VK_FORMAT_R8G8B8A8_SRGB) == 4, "RGBA8 texel");
    expect(formatTexelSize(VK_FORMAT_R16_UNORM) == 2, "R16 texel");
    expect(formatTexelSize(VK_FORMAT_R16G16B16A16_SFLOAT) == 8, "RGBA16F texel");
    expect(formatTexelSize(VK_FORMAT_R32G32B32A32_SFLOAT) == 16, "RGBA32F texel");
    expect(formatTexelSize(VK_FORMAT_UNDEFINED) == 0, "unknown format has no texel size");

    // Only 8-bit formats have a decode path; wider ones are rejected instead of
    // being uploaded from a 4-byte-per-pixel buffer.
    expect(textureFileLayout(VK_FORMAT_R8_UNORM).has_value(), "R8 loads");
    expect(textureFileLayout(VK_FORMAT_R8G8_UNORM).has_value(), "RG8 loads");
    expect(textureFileLayout(VK_FORMAT_R8G8B8A8_UNORM).has_value(), "RGBA8 loads");
    expect(!textureFileLayout(VK_FORMAT_R16_UNORM).has_value(), "R16 rejected");
    expect(!textureFileLayout(VK_FORMAT_R16G16B16A16_SFLOAT).has_value(), "RGBA16F rejected");
    expect(!textureFileLayout(VK_FORMAT_R32_SFLOAT).has_value(), "R32F rejected");
    expect(!textureFileLayout(VK_FORMAT_A2R10G10B10_UNORM_PACK32).has_value(), "RGB10A2 rejected");

    // RG8: a 2x2 RGBA file keeps R,G of every pixel and nothing else.
    {
        const std::vector<uint8_t> rgba = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        auto layout = textureFileLayout(VK_FORMAT_R8G8_UNORM);
        std::vector<uint8_t> out;
        std::string error;
        expect(layout && convertDecodedTexture(rgba.data(), 2, 2, *layout, 2, 2, out, error), "RG8 conversion succeeds");
        const std::vector<uint8_t> expected = {1, 2, 5, 6, 9, 10, 13, 14};
        expect(out == expected, "RG8 keeps R and G of each pixel");
    }

    // A file smaller than the declared extent is resized: the staging size is
    // the declared extent times the texel size, and never reads past the file.
    {
        const std::vector<uint8_t> rgba(4 * 4 * 4, 200);  // 4x4 file
        auto layout = textureFileLayout(VK_FORMAT_R8G8B8A8_UNORM);
        std::vector<uint8_t> out;
        std::string error;
        expect(layout && convertDecodedTexture(rgba.data(), 4, 4, *layout, 16, 8, out, error), "upscale succeeds");
        expect(out.size() == 16u * 8u * 4u, "RGBA8 staging size follows the declared extent");
        bool flat = !out.empty();
        for (uint8_t v : out)
            flat &= v == 200;
        expect(flat, "resize of a flat image stays flat");
    }

    // RG8 from a smaller file: the squeeze must not walk the declared extent.
    {
        const std::vector<uint8_t> rgba(2 * 2 * 4, 90);  // 2x2 file, declared 8x8
        auto layout = textureFileLayout(VK_FORMAT_R8G8_UNORM);
        std::vector<uint8_t> out;
        std::string error;
        expect(layout && convertDecodedTexture(rgba.data(), 2, 2, *layout, 8, 8, out, error), "RG8 upscale succeeds");
        expect(out.size() == 8u * 8u * 2u, "RG8 staging size is two bytes per declared texel");
    }

    // A file larger than declared is shrunk to the declared extent.
    {
        const std::vector<uint8_t> grey(64 * 64, 7);
        auto layout = textureFileLayout(VK_FORMAT_R8_UNORM);
        std::vector<uint8_t> out;
        std::string error;
        expect(layout && convertDecodedTexture(grey.data(), 64, 64, *layout, 8, 4, out, error), "downscale succeeds");
        expect(out.size() == 8u * 4u, "R8 staging size follows the declared extent");
    }

    // Unusable inputs are errors.
    {
        auto layout = textureFileLayout(VK_FORMAT_R8_UNORM);
        std::vector<uint8_t> out;
        std::string error;
        const uint8_t pixel = 0;
        expect(!convertDecodedTexture(&pixel, 1, 1, *layout, 0, 4, out, error) && !error.empty(), "zero target extent rejected");
        expect(!convertDecodedTexture(nullptr, 1, 1, *layout, 4, 4, out, error), "null source rejected");
        expect(!convertDecodedTexture(&pixel, 1, 1, *layout, 60000, 60000, out, error), "oversized target rejected");
    }

    if (failures == 0)
        std::printf("texture conversion checks passed\n");
    return failures == 0 ? 0 : 1;
}
