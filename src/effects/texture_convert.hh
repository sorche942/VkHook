#ifndef TEXTURE_CONVERT_HPP_INCLUDED
#define TEXTURE_CONVERT_HPP_INCLUDED

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vulkan_include.hh"

namespace VkHook
{
    // Size in bytes of one texel of `format`, or 0 for a format this table does
    // not know. Staging sizes are always derived from this, never from a
    // channel count guessed at the call site.
    uint32_t formatTexelSize(VkFormat format);

    // How a ReShade `source` image file reaches a texture of a given format.
    struct TextureFileLayout
    {
        int      decodeChannels;  // channels requested from the decoder (1 or 4)
        uint32_t texelChannels;   // channels stored in the texture (1, 2 or 4)
        uint32_t texelSize;       // bytes per texel in the texture
    };

    // The decode path for `format`, or nullopt when files cannot be loaded into
    // it. Only 8-bit formats have one: the decoder yields 8-bit channels, and
    // pretending otherwise under-sizes the upload.
    std::optional<TextureFileLayout> textureFileLayout(VkFormat format);

    // Converts a decoded image (`decodeChannels` interleaved bytes per pixel,
    // `srcWidth` x `srcHeight`) into exactly `targetWidth` x `targetHeight`
    // texels of `layout`: channels are dropped to the texture's channel count
    // and the image is resized when its extent differs. `out` ends up exactly
    // targetWidth * targetHeight * layout.texelSize bytes. Returns false with
    // `error` set when the input or target is unusable.
    bool convertDecodedTexture(const uint8_t* src,
                               uint32_t srcWidth,
                               uint32_t srcHeight,
                               const TextureFileLayout& layout,
                               uint32_t targetWidth,
                               uint32_t targetHeight,
                               std::vector<uint8_t>& out,
                               std::string& error);
} // namespace VkHook

#endif // TEXTURE_CONVERT_HPP_INCLUDED
