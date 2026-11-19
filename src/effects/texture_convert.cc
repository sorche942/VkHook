#include "texture_convert.hh"

#include <climits>
#include <cstring>

#include "stb_image_resize.h"

namespace VkHook
{
    namespace
    {
        // Larger than any texture a ReShade effect declares; bounds the
        // allocation a corrupt extent could otherwise request.
        constexpr uint64_t kMaxTextureBytes = 1ull << 30;
    }

    uint32_t formatTexelSize(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_SRGB:
                return 1;
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8_SRGB:
            case VK_FORMAT_R16_UNORM:
            case VK_FORMAT_R16_SFLOAT:
                return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            case VK_FORMAT_R16G16_UNORM:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32_SINT:
            case VK_FORMAT_R32_UINT:
                return 4;
            case VK_FORMAT_R16G16B16A16_UNORM:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
                return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return 16;
            default:
                return 0;
        }
    }

    std::optional<TextureFileLayout> textureFileLayout(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_SRGB:
                return TextureFileLayout{1, 1, formatTexelSize(format)};
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8_SRGB:
                // Decoded as RGBA and reduced to R,G: the decoder's
                // grey+alpha request is not reliable for every file type.
                return TextureFileLayout{4, 2, formatTexelSize(format)};
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                return TextureFileLayout{4, 4, formatTexelSize(format)};
            default:
                return std::nullopt;
        }
    }

    bool convertDecodedTexture(const uint8_t* src,
                               uint32_t srcWidth,
                               uint32_t srcHeight,
                               const TextureFileLayout& layout,
                               uint32_t targetWidth,
                               uint32_t targetHeight,
                               std::vector<uint8_t>& out,
                               std::string& error)
    {
        out.clear();
        error.clear();

        const bool layoutValid = layout.decodeChannels >= 1 && layout.decodeChannels <= 4 && layout.texelChannels >= 1 &&
                                 layout.texelChannels <= static_cast<uint32_t>(layout.decodeChannels) &&
                                 layout.texelSize == layout.texelChannels;
        if (!layoutValid)
        {
            error = "inconsistent texture layout";
            return false;
        }
        if (src == nullptr || srcWidth == 0 || srcHeight == 0 || targetWidth == 0 || targetHeight == 0)
        {
            error = "empty source or target extent";
            return false;
        }
        const uint64_t srcBytes = static_cast<uint64_t>(srcWidth) * srcHeight * layout.decodeChannels;
        const uint64_t targetBytes = static_cast<uint64_t>(targetWidth) * targetHeight * layout.texelSize;
        if (srcWidth > INT_MAX || srcHeight > INT_MAX || targetWidth > INT_MAX || targetHeight > INT_MAX ||
            srcBytes > kMaxTextureBytes || targetBytes > kMaxTextureBytes)
        {
            error = "texture extent too large";
            return false;
        }

        // Drop to the texture's channel count at the file's own size, so every
        // read stays inside the decoded buffer whatever extent the shader declared.
        const uint8_t* reduced = src;
        std::vector<uint8_t> reducedStorage;
        if (layout.texelChannels != static_cast<uint32_t>(layout.decodeChannels))
        {
            const uint64_t pixelCount = static_cast<uint64_t>(srcWidth) * srcHeight;
            reducedStorage.resize(pixelCount * layout.texelChannels);
            for (uint64_t p = 0; p < pixelCount; ++p)
                std::memcpy(&reducedStorage[p * layout.texelChannels], &src[p * layout.decodeChannels], layout.texelChannels);
            reduced = reducedStorage.data();
        }

        if (srcWidth == targetWidth && srcHeight == targetHeight)
        {
            out.assign(reduced, reduced + targetBytes);
            return true;
        }

        out.resize(targetBytes);
        if (!stbir_resize_uint8(reduced, static_cast<int>(srcWidth), static_cast<int>(srcHeight), 0,
                                out.data(), static_cast<int>(targetWidth), static_cast<int>(targetHeight), 0,
                                static_cast<int>(layout.texelChannels)))
        {
            out.clear();
            error = "resize failed";
            return false;
        }
        return true;
    }
} // namespace VkHook
