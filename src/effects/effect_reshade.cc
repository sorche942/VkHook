#include "effect_reshade.hh"

#include <cstring>
#include <climits>
#include <cstdlib>
#include <cassert>

#include <set>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "image_view.hh"
#include "c_resource.hh"
#include "descriptor_set.hh"
#include "buffer.hh"
#include "renderpass.hh"
#include "graphics_pipeline.hh"
#include "framebuffer.hh"
#include "shader.hh"
#include "sampler.hh"
#include "image.hh"
#include "format.hh"
#include "config_serializer.hh"
#include "store.hh"
#include "reshade_compile.hh"
#include "texture_convert.hh"
#include "descriptor_budget.hh"
#include "effect_renderpass.hh"
#include "output_forward.hh"
#include "shader_cache.hh"
#include "texture_upload.hh"

#include "util.hh"

#include "stb_image.h"
#include "stb_image_dds.h"
#include "stb_image_resize.h"

namespace VkHook
{
    namespace
    {
        // stb returns a malloc'd buffer that only stbi_image_free may release.
        // The decode below sits between the load and the upload with several
        // exits, so owning the buffer is what stops a new early return from
        // leaking a decoded image. A deleter struct rather than a bare function
        // pointer because unique_ptr cannot default-construct with the latter,
        // and the pixels handle has to exist before the load is attempted.
        struct StbiDeleter
        {
            void operator()(void* pixels) const noexcept
            {
                stbi_image_free(pixels);
            }
        };

        using StbiImage = std::unique_ptr<stbi_uc, StbiDeleter>;

        struct RuntimeCodegenPolicy
        {
            bool useLocalSizeId = true;
            bool emitDebugInfo = true;
            bool disableComputePipelineOptimization = false;
        };

        bool getLocalSizeIdEnabledOverride(bool& hasOverride, bool defaultValue)
        {
            hasOverride = false;
            const char* value = std::getenv("VKHOOK_ENABLE_LOCAL_SIZE_ID");
            if (value == nullptr || *value == '\0')
                return defaultValue;

            hasOverride = true;
            if (value[0] == '0')
                return false;
            if (value[0] == '1')
                return true;

            Logger::warn(std::string("Invalid VKHOOK_ENABLE_LOCAL_SIZE_ID value '") + value +
                         "'. Expected 0 or 1.");
            return defaultValue;
        }

        RuntimeCodegenPolicy selectRuntimeCodegenPolicy(const LogicalDevice* logicalDevice)
        {
            RuntimeCodegenPolicy policy;
            const bool isNvidiaGpu = (logicalDevice != nullptr) && logicalDevice->isNvidiaGpu;
            policy.emitDebugInfo = !isNvidiaGpu;
            policy.disableComputePipelineOptimization = isNvidiaGpu;

            bool hasLocalSizeIdOverride = false;
            policy.useLocalSizeId = getLocalSizeIdEnabledOverride(hasLocalSizeIdOverride, !isNvidiaGpu);
            return policy;
        }

    }

    // Texture uploads run as one submission per effect, through the queue's
    // QueueState mutex, with the fence wait taken outside any lock.
    static bool submitUploadAndWait(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer)
    {
        VkSubmitInfo submitInfo       = {};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &commandBuffer;
        const VkResult result = logicalDevice->queues.submitAndWait(logicalDevice->queue, submitInfo, 4000);
        if (result != VK_SUCCESS)
        {
            Logger::err("texture upload: submit/wait failed: " + std::to_string(result));
            return false;
        }
        return true;
    }

    void ReshadeEffect::requireDescriptorSets(const std::vector<VkDescriptorSet>& sets, const char* what) const
    {
        if (sets.size() != inputImages.size() ||
            std::any_of(sets.begin(), sets.end(), [](VkDescriptorSet set) { return set == VK_NULL_HANDLE; }))
        {
            throw EffectBuildError(std::string("descriptor set allocation failed (") + what + ") for effect '" + effectName + "'");
        }
    }

    ReshadeEffect::ReshadeEffect(LogicalDevice*       logicalDevice,
                                 VkFormat             format,
                                 VkExtent2D           imageExtent,
                                 std::vector<VkImage> inputImages,
                                 std::vector<VkImage> outputImages,
                                 EffectRegistry*      effectRegistry,
                                 ReshadeTexturePool& texturePool,
                                 std::string          effectName,
                                 std::string          effectPath,
                                 std::vector<PreprocessorDefinition> customDefs)
    {
        try
        {
            build(logicalDevice, format, imageExtent, std::move(inputImages), std::move(outputImages), effectRegistry,
                  texturePool, std::move(effectName), std::move(effectPath), std::move(customDefs));
        }
        catch (...)
        {
            // A throwing constructor never runs the destructor, so everything
            // build() created before failing is released here instead.
            releaseResources();
            throw;
        }
    }

    void ReshadeEffect::build(LogicalDevice*       logicalDevice,
                                 VkFormat             format,
                                 VkExtent2D           imageExtent,
                                 std::vector<VkImage> inputImages,
                                 std::vector<VkImage> outputImages,
                                 EffectRegistry*      effectRegistry,
                                 ReshadeTexturePool& texturePool,
                                 std::string          effectName,
                                 std::string          effectPath,
                                 std::vector<PreprocessorDefinition> customDefs)
    {
        Logger::debug("in creating ReshadeEffect");

        this->logicalDevice        = logicalDevice;
        this->imageExtent           = imageExtent;
        this->inputImages           = inputImages;
        this->outputImages          = outputImages;
        this->effectRegistry       = effectRegistry;
        this->effectName            = effectName;
        this->effectPath            = effectPath;
        this->customPreprocessorDefs = customDefs;
        inputOutputFormatUNORM = convertToUNORM(format);
        inputOutputFormatSRGB  = convertToSRGB(format);

        inputImageViewsSRGB  = createImageViews(logicalDevice, inputOutputFormatSRGB, inputImages);
        inputImageViewsUNORM = createImageViews(logicalDevice, inputOutputFormatUNORM, inputImages);
        Logger::debug("created input ImageViews");
        outputImageViewsSRGB  = createImageViews(logicalDevice, inputOutputFormatSRGB, outputImages);
        outputImageViewsUNORM = createImageViews(logicalDevice, inputOutputFormatUNORM, outputImages);
        if (inputImageViewsSRGB.empty() || inputImageViewsUNORM.empty() || outputImageViewsSRGB.empty() || outputImageViewsUNORM.empty())
            throw EffectBuildError("swapchain image view creation failed for effect '" + effectName + "'");
        Logger::debug("created ImageViews");

        createReshadeModule();
        const reshadefx::module& module = *compiledModule;

        std::vector<std::string> zeroedSources;
        uniforms = createReshadeUniforms(module, &zeroedSources);
        for (auto& uniform : uniforms)
        {
            if (auto* depthUniform = dynamic_cast<DepthUniform*>(uniform.get()))
                depthUniforms.push_back(depthUniform);
        }
        if (!zeroedSources.empty())
        {
            std::string list;
            for (const auto& source : zeroedSources)
                list += (list.empty() ? "" : ", ") + source;
            Logger::warn("effect '" + effectName + "': uniform sources without live data are written as zeros: " + list);
        }

        bufferSize = module.total_uniform_size;
        if (bufferSize)
        {
            uniformRing = UniformRing(static_cast<uint32_t>(inputImages.size()), bufferSize);
            createBuffer(logicalDevice,
                         uniformRing.totalBytes(),
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         uniformBuffer,
                         uniformBufferMemory);
            if (uniformBuffer == VK_NULL_HANDLE || uniformBufferMemory == VK_NULL_HANDLE)
                throw EffectBuildError("uniform buffer creation failed for effect '" + effectName + "'");
            // Persistent map: HOST_COHERENT means no flush is needed, writes are just made.
            VkResult mapResult = logicalDevice->vkd.MapMemory(logicalDevice->device, uniformBufferMemory, 0, uniformRing.totalBytes(), 0, &uniformBufferMapped);
            if (mapResult != VK_SUCCESS)
            {
                uniformBufferMapped = nullptr;
                throw EffectBuildError("MapMemory failed for effect '" + effectName + "': " + std::to_string(mapResult));
            }
            // Fresh device memory holds whatever was there before; no uniform may
            // read it, including ones nothing below writes.
            std::memset(uniformBufferMapped, 0, uniformRing.totalBytes());
            buildBindings(module);
        }

        stencilFormat = getStencilFormat(logicalDevice);
        Logger::debug("Stencil Format: " + std::to_string(stencilFormat));
        textureMemory.push_back(VK_NULL_HANDLE);
        stencilImage = createSingleImage(logicalDevice,
                                    {imageExtent.width, imageExtent.height, 1},
                                    stencilFormat,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    textureMemory.back());
        if (stencilImage == VK_NULL_HANDLE)
            throw EffectBuildError("stencil image creation failed for effect '" + effectName + "'");

        stencilImageView = createSingleImageView(
            logicalDevice, stencilFormat, stencilImage, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (stencilImageView == VK_NULL_HANDLE)
            throw EffectBuildError("stencil view creation failed for effect '" + effectName + "'");

        std::vector<std::vector<VkImageView>> imageViewVector;

        // Cache shader manager config once for all texture lookups (avoids re-reading per texture)
        ShaderManagerConfig cachedShaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();

        // Every file texture's initial contents (or its clear, when the file is
        // unusable) go to the GPU in one submission after the loop.
        TextureUploadBatch textureUploads;

        // Private textures belong to this instance of the effect file. Another
        // file that declares the same name still shares it (ReShade semantics).
        const uint32_t instanceOrdinal = texturePool.beginEffect(this->effectPath);

        for (size_t i = 0; i < module.textures.size(); i++)
        {
            textureMipLevels[module.textures[i].unique_name] = module.textures[i].levels;
            const bool textureIs3D = module.textures[i].dimensions == 3;
            const VkImageViewType textureViewType = textureIs3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
            const uint32_t textureDepth = textureIs3D ? std::max(module.textures[i].depth, 1u) : 1u;
            textureExtents[module.textures[i].unique_name] = {module.textures[i].width, module.textures[i].height, textureDepth};
            if (module.textures[i].semantic == "COLOR")
            {
                textureImageViewsUNORM[module.textures[i].unique_name] = inputImageViewsUNORM;
                renderImageViewsUNORM[module.textures[i].unique_name]  = inputImageViewsUNORM;

                textureImageViewsSRGB[module.textures[i].unique_name] = inputImageViewsSRGB;
                renderImageViewsSRGB[module.textures[i].unique_name]  = inputImageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = inputOutputFormatUNORM;
                textureFormatsSRGB[module.textures[i].unique_name]  = inputOutputFormatSRGB;
                continue;
            }
            if (module.textures[i].semantic == "DEPTH")
            {
                textureImageViewsUNORM[module.textures[i].unique_name] = inputImageViewsUNORM;
                renderImageViewsUNORM[module.textures[i].unique_name]  = inputImageViewsUNORM;

                textureImageViewsSRGB[module.textures[i].unique_name] = inputImageViewsSRGB;
                renderImageViewsSRGB[module.textures[i].unique_name]  = inputImageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = inputOutputFormatUNORM;
                textureFormatsSRGB[module.textures[i].unique_name]  = inputOutputFormatSRGB;
                continue;
            }
            VkExtent3D textureExtent = {module.textures[i].width, module.textures[i].height, textureDepth};
            if (const auto source = std::find_if(
                    module.textures[i].annotations.begin(), module.textures[i].annotations.end(), [](const auto& a) { return a.name == "source"; });
                source == module.textures[i].annotations.end())
            {
                const auto& info = module.textures[i];
                auto texture = texturePool.acquire(info.unique_name, {
                    textureExtent, convertReshadeFormat(info.format), info.levels, textureViewType}, instanceOrdinal);
                const std::string& name = info.unique_name;
                textureImages[name] = {texture->image};
                textureImageViewsUNORM[name].assign(inputImages.size(), texture->unormView);
                textureImageViewsSRGB[name].assign(inputImages.size(), texture->srgbView);
                renderImageViewsUNORM[name].assign(inputImages.size(), texture->unormRenderView);
                renderImageViewsSRGB[name].assign(inputImages.size(), texture->srgbRenderView);
                textureFormatsUNORM[name] = convertToUNORM(convertReshadeFormat(info.format));
                textureFormatsSRGB[name] = convertToSRGB(convertReshadeFormat(info.format));
                sharedTextures.push_back(std::move(texture));
                continue;
            }
            else
            {
                textureMemory.push_back(VK_NULL_HANDLE);
                std::vector<VkImage> images =
                    createImages(logicalDevice,
                                 1,
                                 textureExtent,
                                 convertReshadeFormat(module.textures[i].format), // TODO search for format and save it
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 textureMemory.back(),
                                 module.textures[i].levels);

                if (images.empty())
                    throw EffectBuildError("image creation failed for texture '" + module.textures[i].unique_name + "' in effect '" + effectName + "'");
                textureImages[module.textures[i].unique_name] = images;

                std::vector<VkImageView> imageViews = createImageViews(logicalDevice,
                                                                       convertToUNORM(convertReshadeFormat(module.textures[i].format)),
                                                                       images,
                                                                       textureViewType,
                                                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                                                       module.textures[i].levels);

                if (imageViews.empty())
                    throw EffectBuildError("view creation failed for texture '" + module.textures[i].unique_name + "' in effect '" + effectName + "'");
                std::vector<VkImageView> imageViewsUNORM = std::vector<VkImageView>(inputImages.size(), imageViews[0]);

                // File textures may have storage-capable UNORM views, but sRGB
                // sampling views must not inherit unsupported STORAGE usage.
                imageViews = createImageViews(logicalDevice,
                                              convertToSRGB(convertReshadeFormat(module.textures[i].format)),
                                              images,
                                              textureViewType,
                                              VK_IMAGE_ASPECT_COLOR_BIT,
                                              module.textures[i].levels,
                                              VK_IMAGE_USAGE_SAMPLED_BIT);

                if (imageViews.empty())
                    throw EffectBuildError("sRGB view creation failed for texture '" + module.textures[i].unique_name + "' in effect '" + effectName + "'");
                std::vector<VkImageView> imageViewsSRGB = std::vector<VkImageView>(inputImages.size(), imageViews[0]);

                textureImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                textureImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;

                renderImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                renderImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = convertToUNORM(convertReshadeFormat(module.textures[i].format));
                textureFormatsSRGB[module.textures[i].unique_name]  = convertToSRGB(convertReshadeFormat(module.textures[i].format));

                // Image files load only into formats with a decode path; anything
                // else is rejected rather than uploaded from an under-sized buffer.
                // A texture that cannot be loaded is cleared, never left undefined.
                const std::string textureName = source->value.string_data;
                auto leaveBlank = [&]() { textureUploads.addClear(images[0], module.textures[i].levels); };
                const auto fileLayout = textureFileLayout(textureFormatsUNORM[module.textures[i].unique_name]);
                if (!fileLayout)
                {
                    Logger::err("texture " + textureName + " cannot be loaded: no decode path for texture format " +
                                std::to_string(textureFormatsUNORM[module.textures[i].unique_name]));
                    leaveBlank();
                    continue;
                }
                if (textureExtent.depth != 1)
                {
                    Logger::err("texture " + textureName + " cannot be loaded: image files fill 2D textures only");
                    leaveBlank();
                    continue;
                }


                // Search for texture in discovered paths from shader manager
                std::string filePath;
                UniqueCFile file;

                for (const auto& texPath : cachedShaderMgrConfig.discoveredTexturePaths)
                {
                    filePath = texPath + "/" + textureName;
                    file.reset(fopen(filePath.c_str(), "rb"));
                    if (file)
                        break;
                }

                // Fallback: recursive search by basename under discovered texture paths.
                // iMMERSE packages textures in subdirectories (e.g. Textures/iMMERSE),
                // while shader annotations often reference only the filename.
                if (!file)
                {
                    std::filesystem::path texturePath(textureName);
                    const std::string textureBaseName = texturePath.filename().string();

                    for (const auto& texPath : cachedShaderMgrConfig.discoveredTexturePaths)
                    {
                        std::error_code ec;
                        std::filesystem::recursive_directory_iterator it(
                            texPath, std::filesystem::directory_options::skip_permission_denied, ec);
                        std::filesystem::recursive_directory_iterator end;
                        if (ec)
                            continue;

                        for (; it != end; it.increment(ec))
                        {
                            if (ec)
                                break;
                            if (!it->is_regular_file(ec))
                                continue;
                            if (ec)
                                continue;

                            if (it->path().filename() == textureBaseName)
                            {
                                filePath = it->path().string();
                                file.reset(fopen(filePath.c_str(), "rb"));
                                if (file)
                                    break;
                            }
                        }

                        if (file)
                            break;
                    }
                }

                if (!file)
                {
                    Logger::err("couldn't open texture: " + textureName + " (searched " +
                        std::to_string(cachedShaderMgrConfig.discoveredTexturePaths.size()) + " directories, including recursive fallback)");
                    leaveBlank();
                    continue;
                }

                StbiImage pixels;
                int       width  = 0;
                int       height = 0;
                {
                    int channels = 0;
                    if (stbi_dds_test_file(file.get()))
                        pixels.reset(stbi_dds_load_from_file(file.get(), &width, &height, &channels, fileLayout->decodeChannels));
                    else
                        pixels.reset(stbi_load_from_file(file.get(), &width, &height, &channels, fileLayout->decodeChannels));
                }

                if (!pixels || width <= 0 || height <= 0)
                {
                    Logger::err("failed to decode texture: " + textureName + " from " + filePath);
                    leaveBlank();
                    continue;
                }

                // Every size below comes from the format's real texel size and the
                // extent actually uploaded, not from the file's size.
                std::vector<uint8_t> upload;
                std::string          convertError;
                if (!convertDecodedTexture(pixels.get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                           *fileLayout, textureExtent.width, textureExtent.height, upload, convertError))
                {
                    Logger::err("cannot use texture " + textureName + " (" + std::to_string(width) + "x" +
                                std::to_string(height) + ") as " + std::to_string(textureExtent.width) + "x" +
                                std::to_string(textureExtent.height) + ": " + convertError);
                    leaveBlank();
                    continue;
                }

                textureUploads.addUpload(images[0], textureExtent, module.textures[i].levels, std::move(upload));
            }
        }

        if (!textureUploads.submit(logicalDevice, [this](VkCommandBuffer commands) { return submitUploadAndWait(this->logicalDevice, commands); }))
            throw EffectBuildError("texture upload failed for effect '" + effectName + "'");

        std::set<std::string> depthTextureNames;
        for (const auto& texture : module.textures)
        {
            if (texture.semantic == "DEPTH")
                depthTextureNames.insert(texture.unique_name);
        }

        std::vector<reshadefx::texture_filter> samplerFilters;
        samplerFilters.reserve(module.samplers.size());
        for (size_t i = 0; i < module.samplers.size(); i++)
        {
            reshadefx::sampler_info info = module.samplers[i];
            if (depthTextureNames.find(info.texture_name) != depthTextureNames.end())
                info.filter = reshadefx::texture_filter::min_mag_mip_point;
            samplerFilters.push_back(info.filter);

            VkSampler sampler = createReshadeSampler(logicalDevice, info);
            if (sampler == VK_NULL_HANDLE)
                throw EffectBuildError("sampler creation failed for '" + info.unique_name + "' in effect '" + effectName + "'");

            samplers.push_back(sampler);

            imageViewVector.push_back(info.srgb ? textureImageViewsSRGB[info.texture_name] : textureImageViewsUNORM[info.texture_name]);
        }

        std::vector<VkDescriptorType> samplerBindingTypes(module.samplers.size(), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        size_t sampledBindingCount = 0;
        size_t storageBindingCount = 0;
        for (size_t i = 0; i < module.samplers.size(); ++i)
        {
            if (module.samplers[i].storage)
            {
                samplerBindingTypes[i] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                ++storageBindingCount;
            }
            else
            {
                ++sampledBindingCount;
            }
            Logger::debug(
                "sampler binding " + std::to_string(i) + " name=" + module.samplers[i].unique_name +
                " texture=" + module.samplers[i].texture_name +
                " storage=" + (module.samplers[i].storage ? "1" : "0") +
                " filter=" + std::to_string(static_cast<uint32_t>(samplerFilters[i])));
        }

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(logicalDevice, samplerBindingTypes);
        uniformDescriptorSetLayout      = createUniformBufferDescriptorSetLayout(logicalDevice);
        if (imageSamplerDescriptorSetLayout == VK_NULL_HANDLE || uniformDescriptorSetLayout == VK_NULL_HANDLE)
            throw EffectBuildError("descriptor set layout creation failed for effect '" + effectName + "'");
        Logger::debug("created descriptorSetLayouts");

        // Output writes decide how many image/sampler set groups exist, so count
        // them before sizing the pool.
        for (const auto& pass : module.techniques[0].passes)
        {
            if (pass.cs_entry_point.empty() && pass.render_target_names[0].empty())
            {
                outputWrites++;
            }
        }
        outputForwardRequired = reshadeOutputNeedsForward(module, outputWrites);
        Logger::debug("output writes: " + std::to_string(outputWrites));

        const DescriptorBudget budget = computeDescriptorBudget(static_cast<uint32_t>(inputImages.size()),
                                                                static_cast<uint32_t>(sampledBindingCount),
                                                                static_cast<uint32_t>(storageBindingCount),
                                                                samplerSetGroups(outputWrites),
                                                                bufferSize != 0);
        descriptorPool = createDescriptorPool(logicalDevice, budget.poolSizes(), budget.maxSets);
        if (descriptorPool == VK_NULL_HANDLE)
            throw EffectBuildError("descriptor pool creation failed for effect '" + effectName + "'");
        Logger::debug("created descriptorPool");

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {uniformDescriptorSetLayout, imageSamplerDescriptorSetLayout};

        pipelineLayout = createGraphicsPipelineLayout(logicalDevice, descriptorSetLayouts);
        if (pipelineLayout == VK_NULL_HANDLE)
            throw EffectBuildError("pipeline layout creation failed for effect '" + effectName + "'");

        Logger::debug("created Pipeline layout");

        if (bufferSize)
        {
            for (uint32_t slot = 0; slot < uniformRing.slotCount(); ++slot)
            {
                bufferDescriptorSets.push_back(writeBufferDescriptorSet(
                    logicalDevice, descriptorPool, uniformDescriptorSetLayout, uniformBuffer, uniformRing.offset(slot), bufferSize));
                if (bufferDescriptorSets.back() == VK_NULL_HANDLE)
                    throw EffectBuildError("uniform descriptor set allocation failed for effect '" + effectName + "'");
            }
        }

        inputDescriptorSets =
            allocateAndWriteImageSamplerDescriptorSets(
                logicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);
        requireDescriptorSets(inputDescriptorSets, "input");

        // if there is only one outputWrite, we can directly write to outputImages
        if (outputWrites > 1)
        {
            textureMemory.push_back(VK_NULL_HANDLE);
            backBufferImages = createImages(logicalDevice,
                                            inputImages.size(),
                                            {imageExtent.width, imageExtent.height, 1},
                                            format, // TODO search for format and save it
                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            textureMemory.back());
            if (backBufferImages.empty())
                throw EffectBuildError("back buffer image creation failed for effect '" + effectName + "'");
            changeImageLayout(logicalDevice, backBufferImages, 1);

            backBufferImageViewsSRGB  = createImageViews(logicalDevice, inputOutputFormatSRGB, backBufferImages);
            backBufferImageViewsUNORM = createImageViews(logicalDevice, inputOutputFormatUNORM, backBufferImages);
            if (backBufferImageViewsSRGB.empty() || backBufferImageViewsUNORM.empty())
                throw EffectBuildError("back buffer view creation failed for effect '" + effectName + "'");

            std::replace(imageViewVector.begin(), imageViewVector.end(), inputImageViewsSRGB, backBufferImageViewsSRGB);
            std::replace(imageViewVector.begin(), imageViewVector.end(), inputImageViewsUNORM, backBufferImageViewsUNORM);

            backBufferDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
                logicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);
            requireDescriptorSets(backBufferDescriptorSets, "back buffer");
        }
        if (outputWrites > 2)
        {
            std::replace(imageViewVector.begin(), imageViewVector.end(), backBufferImageViewsSRGB, outputImageViewsSRGB);
            std::replace(imageViewVector.begin(), imageViewVector.end(), backBufferImageViewsUNORM, outputImageViewsUNORM);
            outputDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
                logicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);
            requireDescriptorSets(outputDescriptorSets, "output");
        }

        Logger::debug("after writing ImageSamplerDescriptorSets");

        bool firstTimeStencilAccess = true; // Used to clear the sttencil attachment on the first time

        for (bool outputToBackBuffer = outputWrites % 2 == 0; auto& pass : module.techniques[0].passes)
        {
            const bool isComputePass = !pass.cs_entry_point.empty();
            if (!isComputePass && (pass.vs_entry_point.empty() || pass.ps_entry_point.empty()))
            {
                std::string error = "unsupported pass type in effect '" + effectName +
                                    "': graphics pass requires vertex and fragment entry points";
                Logger::err(error);
                throw EffectBuildError(error);
            }

            if (isComputePass)
            {
                // Registered before its pipeline exists, so a failure below
                // still releases whatever this pass already created.
                passRuntimes.emplace_back();
                PassRuntime& runtime = passRuntimes.back();
                runtime.isCompute = true;
                runtime.dispatchSizeX = std::max(pass.dispatch_size_x, 1u);
                runtime.dispatchSizeY = std::max(pass.dispatch_size_y, 1u);
                runtime.dispatchSizeZ = std::max(pass.dispatch_size_z, 1u);

                VkPipelineShaderStageCreateInfo shaderStageCreateInfoCompute = {};
                shaderStageCreateInfoCompute.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                shaderStageCreateInfoCompute.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
                shaderStageCreateInfoCompute.module = shaderModule;
                shaderStageCreateInfoCompute.pName  = pass.cs_entry_point.c_str();

                VkComputePipelineCreateInfo computePipelineCreateInfo = {};
                computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                computePipelineCreateInfo.stage = shaderStageCreateInfoCompute;
                computePipelineCreateInfo.layout = pipelineLayout;
                if (disableComputePipelineOptimization)
                    computePipelineCreateInfo.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;

                Logger::debug("creating compute pipeline entry: " + pass.cs_entry_point);
                const VkResult computeResult = logicalDevice->vkd.CreateComputePipelines(
                    logicalDevice->device, devicePipelineCache(logicalDevice), 1, &computePipelineCreateInfo, nullptr, &runtime.pipeline);
                if (computeResult != VK_SUCCESS)
                {
                    std::string error = "CreateComputePipelines failed for effect '" + effectName +
                                        "', pass CS='" + pass.cs_entry_point +
                                        "', VkResult=" + std::to_string(computeResult);
                    Logger::err(error);
                    throw EffectBuildError(error);
                }

                Logger::debug("compute  entry: " + pass.cs_entry_point +
                              " dispatch(" + std::to_string(runtime.dispatchSizeX) + ", " +
                              std::to_string(runtime.dispatchSizeY) + ", " +
                              std::to_string(runtime.dispatchSizeZ) + ")");
                continue;
            }

            EffectRenderPassDesc                             renderPassDesc;
            std::vector<VkPipelineColorBlendAttachmentState> attachmentBlendStates;
            std::vector<std::vector<VkImageView>>            attachmentImageViews;
            std::vector<std::string>                         currentRenderTargets;

            for (int i = 0; i < 8; i++)
            {
                std::string target = pass.render_target_names[i];
                Logger::debug("render target:" + target);

                if (target.empty() && i > 0)
                {
                    break;
                }

                // Target 0 empty means the pass writes the effect's back buffer,
                // which is always loaded (the seed copy is already in it).
                const VkFormat format = target.empty() ? (pass.srgb_write_enable ? inputOutputFormatSRGB : inputOutputFormatUNORM)
                                                       : (pass.srgb_write_enable ? textureFormatsSRGB[target] : textureFormatsUNORM[target]);
                const VkAttachmentLoadOp loadOp = (!target.empty() && pass.clear_render_targets) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;
                renderPassDesc.attachments.push_back(effectColorAttachment(format, loadOp));

                VkPipelineColorBlendAttachmentState colorBlendAttachment;
                colorBlendAttachment.blendEnable         = pass.blend_enable;
                colorBlendAttachment.srcColorBlendFactor = convertReshadeBlendFactor(pass.src_blend);
                colorBlendAttachment.dstColorBlendFactor = convertReshadeBlendFactor(pass.dest_blend);
                colorBlendAttachment.colorBlendOp        = convertReshadeBlendOp(pass.blend_op);
                colorBlendAttachment.srcAlphaBlendFactor = convertReshadeBlendFactor(pass.src_blend_alpha);
                colorBlendAttachment.dstAlphaBlendFactor = convertReshadeBlendFactor(pass.dest_blend_alpha);
                colorBlendAttachment.alphaBlendOp        = convertReshadeBlendOp(pass.blend_op_alpha);
                colorBlendAttachment.colorWriteMask      = pass.color_write_mask;

                attachmentBlendStates.push_back(colorBlendAttachment);

                attachmentImageViews.push_back(pass.srgb_write_enable ? renderImageViewsSRGB[target] : renderImageViewsUNORM[target]);
                if (!target.empty())
                {
                    currentRenderTargets.push_back(target);
                }
            }

            VkRect2D scissor;
            scissor.offset        = {0, 0};
            scissor.extent.width  = pass.viewport_width ? pass.viewport_width : imageExtent.width;
            scissor.extent.height = pass.viewport_height ? pass.viewport_height : imageExtent.height;

            Logger::debug(std::to_string(scissor.extent.width) + " x " + std::to_string(scissor.extent.height));

            VkViewport viewport;
            viewport.x        = 0.0f;
            viewport.y        = 0.0f;
            viewport.width    = static_cast<float>(scissor.extent.width);
            viewport.height   = static_cast<float>(scissor.extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            if (scissor.extent.width == imageExtent.width && scissor.extent.height == imageExtent.height)
            {
                renderPassDesc.hasDepthStencil = true;
                renderPassDesc.attachments.push_back(effectStencilAttachment(stencilFormat, firstTimeStencilAccess));
                attachmentImageViews.push_back(std::vector<VkImageView>(inputImages.size(), stencilImageView));

                firstTimeStencilAccess = false;
            }
            const uint32_t attachmentCount = static_cast<uint32_t>(renderPassDesc.attachments.size());

            // Registered before any of its objects exist, so a failure below
            // still releases whatever this pass already created.
            passRuntimes.emplace_back();
            PassRuntime& runtime = passRuntimes.back();
            runtime.renderTargets = std::move(currentRenderTargets);
            runtime.vertexCount = pass.num_vertices == 0 ? 3 : pass.num_vertices;
            runtime.viewport = viewport;

            VkResult result = createEffectRenderPass(logicalDevice, renderPassDesc, runtime.renderPass);
            if (result != VK_SUCCESS)
            {
                runtime.renderPass = VK_NULL_HANDLE;
                const std::string error = "CreateRenderPass failed for effect '" + effectName +
                                          "', VkResult=" + std::to_string(result);
                Logger::err(error);
                throw EffectBuildError(error);
            }

            runtime.renderArea = scissor;
            runtime.clearValueCount = std::min<uint32_t>(attachmentCount, runtime.clearValues.size());

            if (pass.render_target_names[0].empty())
            {
                std::vector<VkImageView> backBufferImageViews = pass.srgb_write_enable ? backBufferImageViewsSRGB : backBufferImageViewsUNORM;
                std::vector<VkImageView> finalOutputImageViews = pass.srgb_write_enable ? outputImageViewsSRGB : outputImageViewsUNORM;
                runtime.framebuffers = createFramebuffers(
                    logicalDevice,
                    runtime.renderPass,
                    imageExtent,
                    {outputToBackBuffer ? backBufferImageViews : finalOutputImageViews, std::vector<VkImageView>(inputImages.size(), stencilImageView)});
                outputToBackBuffer = !outputToBackBuffer;
                runtime.switchSamplers = true;
            }
            else
            {
                runtime.framebuffers = createFramebuffers(logicalDevice, runtime.renderPass, scissor.extent, attachmentImageViews);
                runtime.switchSamplers = false;
            }
            if (runtime.framebuffers.empty())
                throw EffectBuildError("framebuffer creation failed for effect '" + effectName + "'");

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoVert = {};
            shaderStageCreateInfoVert.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoVert.stage               = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStageCreateInfoVert.module              = shaderModule;
            shaderStageCreateInfoVert.pName               = pass.vs_entry_point.c_str();

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoFrag = {};
            shaderStageCreateInfoFrag.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoFrag.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStageCreateInfoFrag.module              = shaderModule;
            shaderStageCreateInfoFrag.pName               = pass.ps_entry_point.c_str();

            VkPipelineShaderStageCreateInfo shaderStages[] = {shaderStageCreateInfoVert, shaderStageCreateInfoFrag};

            VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
            vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            switch (pass.topology)
            {
                case reshadefx::primitive_topology::point_list: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
                case reshadefx::primitive_topology::line_list: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
                case reshadefx::primitive_topology::line_strip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
                case reshadefx::primitive_topology::triangle_list: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
                case reshadefx::primitive_topology::triangle_strip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
                default: Logger::err("unsupported primitiv type" + convertToString((uint8_t) pass.topology)); break;
            }

            VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
            inputAssemblyCreateInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssemblyCreateInfo.topology               = topology;
            inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

            // Viewport and scissor are dynamic: the pipeline does not depend on the
            // swapchain extent, and applyEffect sets both before each draw.
            VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
            viewportStateCreateInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportStateCreateInfo.viewportCount = 1;
            viewportStateCreateInfo.scissorCount  = 1;

            VkPipelineRasterizationStateCreateInfo rasterizationCreateInfo = {};
            rasterizationCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizationCreateInfo.depthClampEnable        = VK_FALSE;
            rasterizationCreateInfo.rasterizerDiscardEnable = VK_FALSE;
            rasterizationCreateInfo.polygonMode             = VK_POLYGON_MODE_FILL;
            rasterizationCreateInfo.cullMode                = VK_CULL_MODE_NONE;
            rasterizationCreateInfo.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizationCreateInfo.lineWidth               = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisampleCreateInfo = {};
            multisampleCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampleCreateInfo.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
            multisampleCreateInfo.sampleShadingEnable   = VK_FALSE;
            multisampleCreateInfo.minSampleShading      = 1.0f;
            multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
            multisampleCreateInfo.alphaToOneEnable      = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlendCreateInfo = {};
            colorBlendCreateInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlendCreateInfo.logicOpEnable   = VK_FALSE;
            colorBlendCreateInfo.logicOp         = VK_LOGIC_OP_NO_OP;
            colorBlendCreateInfo.attachmentCount = attachmentBlendStates.size();
            colorBlendCreateInfo.pAttachments    = attachmentBlendStates.data();

            const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = {};
            dynamicStateCreateInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicStateCreateInfo.dynamicStateCount = 2;
            dynamicStateCreateInfo.pDynamicStates    = dynamicStates;

            VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {};
            depthStencilStateCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencilStateCreateInfo.depthTestEnable       = VK_FALSE;
            depthStencilStateCreateInfo.depthWriteEnable      = VK_FALSE;
            depthStencilStateCreateInfo.depthCompareOp        = VK_COMPARE_OP_ALWAYS;
            depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
            depthStencilStateCreateInfo.stencilTestEnable     = pass.stencil_enable;
            depthStencilStateCreateInfo.front.failOp          = convertReshadeStencilOp(pass.stencil_op_fail);
            depthStencilStateCreateInfo.front.passOp          = convertReshadeStencilOp(pass.stencil_op_pass);
            depthStencilStateCreateInfo.front.depthFailOp     = convertReshadeStencilOp(pass.stencil_op_depth_fail);
            depthStencilStateCreateInfo.front.compareOp       = convertReshadeCompareOp(pass.stencil_comparison_func);
            depthStencilStateCreateInfo.front.compareMask     = pass.stencil_read_mask;
            depthStencilStateCreateInfo.front.writeMask       = pass.stencil_write_mask;
            depthStencilStateCreateInfo.front.reference       = pass.stencil_reference_value;
            depthStencilStateCreateInfo.back                  = depthStencilStateCreateInfo.front;
            depthStencilStateCreateInfo.minDepthBounds        = 0.0f;
            depthStencilStateCreateInfo.maxDepthBounds        = 1.0f;

            VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineCreateInfo.stageCount          = 2;
            pipelineCreateInfo.pStages             = shaderStages;
            pipelineCreateInfo.pVertexInputState   = &vertexInputCreateInfo;
            pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
            pipelineCreateInfo.pViewportState      = &viewportStateCreateInfo;
            pipelineCreateInfo.pRasterizationState = &rasterizationCreateInfo;
            pipelineCreateInfo.pMultisampleState   = &multisampleCreateInfo;
            pipelineCreateInfo.pDepthStencilState  = &depthStencilStateCreateInfo;
            pipelineCreateInfo.pColorBlendState    = &colorBlendCreateInfo;
            pipelineCreateInfo.pDynamicState       = &dynamicStateCreateInfo;
            pipelineCreateInfo.layout              = pipelineLayout;
            pipelineCreateInfo.renderPass          = runtime.renderPass;
            pipelineCreateInfo.subpass             = 0;
            pipelineCreateInfo.basePipelineHandle  = VK_NULL_HANDLE;
            pipelineCreateInfo.basePipelineIndex   = -1;

            Logger::debug("creating graphics pipeline VS=" + pass.vs_entry_point + " PS=" + pass.ps_entry_point);
            result = logicalDevice->vkd.CreateGraphicsPipelines(
                logicalDevice->device, devicePipelineCache(logicalDevice), 1, &pipelineCreateInfo, nullptr, &runtime.pipeline);
            if (result != VK_SUCCESS)
            {
                std::string error = "CreateGraphicsPipelines failed for effect '" + effectName +
                                    "', pass VS='" + pass.vs_entry_point + "', PS='" + pass.ps_entry_point +
                                    "', VkResult=" + std::to_string(result);
                Logger::err(error);
                throw EffectBuildError(error);
            }

            Logger::debug("vertex   entry: " + pass.vs_entry_point);
            Logger::debug("fragment entry: " + pass.ps_entry_point);
        }
        // Info, not debug, and deliberately here rather than in the destructor:
        // this line is how the smoke test proves an effect was really built
        // instead of the layer quietly falling back to the pass-through path.
        // The version of that assertion this replaces keyed off the destructor's
        // trace, which only appears if something tears the effect down -- and a
        // crash-only test cannot rely on a teardown happening, since the usual
        // outcome is vkcube being killed by the timeout. Reaching this point at
        // all means the shader was found, parsed, compiled, and turned into
        // render passes, which is the thing actually worth asserting.
        Logger::info("[effect-built] " + effectName + " passes=" + std::to_string(passRuntimes.size()));
    }

    // Resolving the registry param per frame meant taking the registry mutex and
    // scanning effect then parameter name lists for every uniform of every
    // enabled effect. The registry bumps its generation when a reparse frees
    // params, so resolve once here and rebuild when that generation moves.
    //
    // The shader defaults of the uniforms left without a parameter land in
    // `uniformDefaults`, not in a slice: a slice may still be read by a frame in
    // flight, so each one picks the new defaults up when its image next updates.
    void ReshadeEffect::buildBindings(const reshadefx::module& module)
    {
        // Read the generation first: a bump that races this rebuild is then seen
        // by the next update and rebuilds again.
        bindingsGeneration = effectRegistry->parameterGeneration();
        uniformDefaults.assign(bufferSize, 0);
        uniformBindings = buildUniformBindings(
            module, bufferSize, uniformDefaults.data(), [this](const std::string& uniformName) -> const EffectParam* {
                auto paramOpt = effectRegistry->getParameter(effectName, uniformName);
                return paramOpt ? *paramOpt : nullptr;
            });
    }

    bool ReshadeEffect::updateFrame(uint32_t imageIndex)
    {
        if (!uniformBufferMapped || !uniformRing.contains(imageIndex))
            return false;

        if (effectRegistry->parameterGeneration() != bindingsGeneration)
            buildBindings(*compiledModule);

        void* slice = uniformRing.slice(uniformBufferMapped, imageIndex);
        if (uniformRing.needsDefaults(imageIndex, bindingsGeneration))
        {
            std::memcpy(slice, uniformDefaults.data(), bufferSize);
            uniformRing.markDefaults(imageIndex, bindingsGeneration);
        }

        applyUniformBindings(uniformBindings, slice);
        for (auto& uniform : uniforms)
            uniform->update(slice);
        // HOST_COHERENT: no flush needed, GPU sees writes automatically
        return false;
    }

    void ReshadeEffect::useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout)
    {
        if (imageIndex >= inputImages.size() || imageIndex >= inputDescriptorSets.size() || imageIndex >= inputImageViewsUNORM.size())
            return;
        const reshadefx::module& module = *compiledModule;

        // Update DepthUniforms so bufready_depth reports correctly
        const bool hasDepth = (depthImageView != VK_NULL_HANDLE);
        for (DepthUniform* depthUniform : depthUniforms)
            depthUniform->setDepthAvailable(hasDepth);

        std::vector<std::string> depthTextureNames;

        for (auto& texture : module.textures)
        {
            if (texture.semantic == "DEPTH")
            {
                depthTextureNames.push_back(texture.unique_name);
            }
        }

        for (size_t i = 0; i < module.samplers.size(); i++)
        {
            reshadefx::sampler_info info = module.samplers[i];
            for (auto& name : depthTextureNames)
            {
                if (info.texture_name == name)
                {
                    VkDescriptorImageInfo imageInfo;
                    imageInfo.sampler = module.samplers[i].storage ? VK_NULL_HANDLE : samplers[i];
                    // Use a input image if there is no depth image to prevent a crash
                    imageInfo.imageView   = depthImageView ? depthImageView : inputImageViewsUNORM[imageIndex];
                    imageInfo.imageLayout = module.samplers[i].storage
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : (depthImageView ? depthImageLayout : VK_IMAGE_LAYOUT_GENERAL);

                    VKHOOK_LOG_DEBUG([&](std::string& s) {
                        s = "useDepthImage: effect=" + effectName
                            + " imageIndex=" + std::to_string(imageIndex)
                            + " binding=" + std::to_string(i)
                            + " texture=" + name
                            + " hasDepth=" + std::string(depthImageView ? "true" : "false")
                            + " imageView=" + convertToString(imageInfo.imageView)
                            + " imageLayout=" + convertToString(imageInfo.imageLayout);
                    });

                    VkWriteDescriptorSet writeDescriptorSet = {};

                    writeDescriptorSet.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSet.pNext            = nullptr;
                    writeDescriptorSet.dstSet           = inputDescriptorSets[imageIndex];
                    writeDescriptorSet.dstBinding       = i;
                    writeDescriptorSet.dstArrayElement  = 0;
                    writeDescriptorSet.descriptorCount  = 1;
                    writeDescriptorSet.descriptorType =
                        module.samplers[i].storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writeDescriptorSet.pImageInfo       = &imageInfo;
                    writeDescriptorSet.pBufferInfo      = nullptr;
                    writeDescriptorSet.pTexelBufferView = nullptr;

                    logicalDevice->vkd.UpdateDescriptorSets(logicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    if (outputWrites > 1 && imageIndex < backBufferDescriptorSets.size())
                    {
                        writeDescriptorSet.dstSet = backBufferDescriptorSets[imageIndex];
                        logicalDevice->vkd.UpdateDescriptorSets(logicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    }
                    if (outputWrites > 2 && imageIndex < outputDescriptorSets.size())
                    {
                        writeDescriptorSet.dstSet = outputDescriptorSets[imageIndex];
                        logicalDevice->vkd.UpdateDescriptorSets(logicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    }
                    break;
                }
            }
        }
    }
    void ReshadeEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        const VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        const VkPipelineStageFlags transferStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkImageCopy fullCopyRegion = {};
        fullCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fullCopyRegion.srcSubresource.mipLevel = 0;
        fullCopyRegion.srcSubresource.baseArrayLayer = 0;
        fullCopyRegion.srcSubresource.layerCount = 1;
        fullCopyRegion.dstSubresource = fullCopyRegion.srcSubresource;
        fullCopyRegion.extent = {imageExtent.width, imageExtent.height, 1};

        // Transition input image for seed copy.
        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.srcAccessMask       = 0;
        memoryBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        memoryBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image               = inputImages[imageIndex];

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, transferStage, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        // Seed output image from current input so backbuffer/load-based passes never start from undefined data.
        memoryBarrier.image = outputImages[imageIndex];
        // The copy below overwrites the whole image, so what it held is never read.
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.srcAccessMask = 0;
        memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, transferStage, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        logicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         inputImages[imageIndex],
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         outputImages[imageIndex],
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1,
                                         &fullCopyRegion);

        if (outputWrites > 1)
        {
            memoryBarrier.image = backBufferImages[imageIndex];
            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // fully overwritten by the copy
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages,
                transferStage,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &memoryBarrier);

            logicalDevice->vkd.CmdCopyImage(commandBuffer,
                                             inputImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             backBufferImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             1,
                                             &fullCopyRegion);
        }

        // Used to make the image accessible by shaders after seed copy.
        memoryBarrier.image = inputImages[imageIndex];
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        // Reverses the first Barrier
        VkImageMemoryBarrier secondBarrier;
        secondBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        secondBarrier.pNext               = nullptr;
        secondBarrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        secondBarrier.dstAccessMask       = 0;
        secondBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        secondBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        secondBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.image               = inputImages[imageIndex];

        secondBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        secondBarrier.subresourceRange.baseMipLevel   = 0;
        secondBarrier.subresourceRange.levelCount     = 1;
        secondBarrier.subresourceRange.baseArrayLayer = 0;
        secondBarrier.subresourceRange.layerCount     = 1;

        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, transferStage, shaderStages, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        memoryBarrier.image     = outputImages[imageIndex];
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, transferStage, shaderStages, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        if (outputWrites > 1)
        {
            memoryBarrier.image = backBufferImages[imageIndex];
            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   transferStage,
                                                   shaderStages,
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }

        const bool hasGraphicsPass = std::any_of(passRuntimes.begin(), passRuntimes.end(), [](const PassRuntime& passRuntime) {
            return !passRuntime.isCompute;
        });
        if (hasGraphicsPass)
        {
            // stencil image
            memoryBarrier.image                       = stencilImage;
            memoryBarrier.srcAccessMask               = 0;
            memoryBarrier.dstAccessMask               = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            memoryBarrier.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
            memoryBarrier.newLayout                   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            memoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;

            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }

        VkDescriptorSet currentSamplerSet = inputDescriptorSets[imageIndex];
        bool backBufferNext = outputWrites % 2 == 0;
        for (size_t i = 0; i < passRuntimes.size(); i++)
        {
            auto& runtime = passRuntimes[i];
            if (runtime.isCompute)
            {
                logicalDevice->vkd.CmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &currentSamplerSet, 0, nullptr);
                if (bufferSize)
                {
                    logicalDevice->vkd.CmdBindDescriptorSets(
                        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &bufferDescriptorSets[imageIndex], 0, nullptr);
                }

                logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, runtime.pipeline);
                logicalDevice->vkd.CmdDispatch(commandBuffer, runtime.dispatchSizeX, runtime.dispatchSizeY, runtime.dispatchSizeZ);
            }
            else
            {
                logicalDevice->vkd.CmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &currentSamplerSet, 0, nullptr);
                if (bufferSize)
                {
                    logicalDevice->vkd.CmdBindDescriptorSets(
                        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bufferDescriptorSets[imageIndex], 0, nullptr);
                }

                VkRenderPassBeginInfo renderPassBeginInfo = {};
                renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                renderPassBeginInfo.renderPass = runtime.renderPass;
                renderPassBeginInfo.framebuffer = runtime.framebuffers[imageIndex];
                renderPassBeginInfo.renderArea = runtime.renderArea;
                renderPassBeginInfo.clearValueCount = runtime.clearValueCount;
                renderPassBeginInfo.pClearValues = runtime.clearValues.data();

                logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
                logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, runtime.pipeline);
                logicalDevice->vkd.CmdSetViewport(commandBuffer, 0, 1, &runtime.viewport);
                logicalDevice->vkd.CmdSetScissor(commandBuffer, 0, 1, &runtime.renderArea);
                logicalDevice->vkd.CmdDraw(commandBuffer, runtime.vertexCount, 1, 0, 0);
                logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

                if (runtime.switchSamplers && outputWrites > 1)
                {
                    if (backBufferNext)
                    {
                        currentSamplerSet = backBufferDescriptorSets[imageIndex];
                    }
                    else if (outputWrites > 2)
                    {
                        currentSamplerSet = outputDescriptorSets[imageIndex];
                    }
                    backBufferNext = !backBufferNext;
                }

                for (const auto& renderTarget : runtime.renderTargets)
                {
                    generateMipMaps(
                        logicalDevice, commandBuffer, textureImages[renderTarget][0], textureExtents[renderTarget], textureMipLevels[renderTarget]);
                }
            }

            // Global rather than image-local: the next pass/effect may sample
            // a named texture written here, including generated mip levels.
            VkMemoryBarrier passMemoryBarrier = {};
            passMemoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            passMemoryBarrier.srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            passMemoryBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages | transferStage,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages,
                                                   0,
                                                   1,
                                                   &passMemoryBarrier,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr);
        }

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer, shaderStages, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &secondBarrier);
        secondBarrier.image = outputImages[imageIndex];
        secondBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer, shaderStages, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &secondBarrier);
    }

    // Everything below may still be read by an in-flight submission, so it is
    // retired (tagged with the submitted serial), never destroyed inline. The
    // retire queue destroys dependents first (framebuffers before render passes,
    // views before images, memory last) once the GPU serial passes.
    void ReshadeEffect::releaseResources()
    {
        if (!logicalDevice || logicalDevice->device == VK_NULL_HANDLE)
            return;

        Logger::info("[DESTROY-TRACE] ~ReshadeEffect start: " + effectName + " passRuntimes=" + std::to_string(passRuntimes.size()));

        auto  device = logicalDevice->device;
        auto& vkd    = logicalDevice->vkd;
        size_t retired = 0;
        const auto retire = [&](VkObjectType type, auto handle) {
            logicalDevice->retire(type, handleToU64(handle));
            ++retired;
        };

        for (auto memory : textureMemory)
        {
            if (memory != VK_NULL_HANDLE)
                retire(VK_OBJECT_TYPE_DEVICE_MEMORY, memory);
        }
        const VkDeviceMemory uniformMemory = uniformBufferMemory;

        // the unmap has to happen before the memory goes, and it is a live call on
        // the device, so it stays inline rather than being retired
        if (uniformBufferMapped)
            vkd.UnmapMemory(device, uniformBufferMemory);
        if (uniformMemory != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_DEVICE_MEMORY, uniformMemory);

        for (auto image : collectOwnedImages())
            retire(VK_OBJECT_TYPE_IMAGE, image);

        for (auto view : collectOwnedImageViews())
            retire(VK_OBJECT_TYPE_IMAGE_VIEW, view);

        for (auto sampler : samplers)
        {
            if (sampler != VK_NULL_HANDLE)
                retire(VK_OBJECT_TYPE_SAMPLER, sampler);
        }

        const VkBuffer     buffer     = uniformBuffer;
        const VkShaderModule module     = shaderModule;
        if (buffer != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_BUFFER, buffer);
        if (module != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_SHADER_MODULE, module);

        const VkDescriptorSetLayout uniformLayout  = uniformDescriptorSetLayout;
        const VkDescriptorSetLayout samplerLayout  = imageSamplerDescriptorSetLayout;
        const VkPipelineLayout      pipelineLay    = pipelineLayout;
        if (uniformLayout != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, uniformLayout);
        if (samplerLayout != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, samplerLayout);
        if (pipelineLay != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipelineLay);

        // the pool implicitly frees its sets
        const VkDescriptorPool pool = descriptorPool;
        if (pool != VK_NULL_HANDLE)
            retire(VK_OBJECT_TYPE_DESCRIPTOR_POOL, pool);

        for (auto& passRuntime : passRuntimes)
        {
            for (auto framebuffer : passRuntime.framebuffers)
            {
                if (framebuffer != VK_NULL_HANDLE)
                    retire(VK_OBJECT_TYPE_FRAMEBUFFER, framebuffer);
            }
            if (passRuntime.renderPass != VK_NULL_HANDLE)
                retire(VK_OBJECT_TYPE_RENDER_PASS, passRuntime.renderPass);
            if (passRuntime.pipeline != VK_NULL_HANDLE)
                retire(VK_OBJECT_TYPE_PIPELINE, passRuntime.pipeline);
        }

        Logger::info("[DESTROY-TRACE] ~ReshadeEffect complete: retired " + std::to_string(retired) + " objects");
    }

    // The same handle is stored in several members -- a COLOR and a DEPTH texture
    // can share an image view, and back buffer views are also reachable through the
    // texture maps. Releasing them in per-member loops double-frees, which the
    // NVIDIA driver does not survive, so every release goes through these dedup
    // sets instead.
    std::set<VkImageView> ReshadeEffect::collectOwnedImageViews() const
    {
        std::set<VkImageView> all;

        auto collectViews = [&all](const std::vector<VkImageView>& views)
        {
            for (auto v : views)
                if (v != VK_NULL_HANDLE)
                    all.insert(v);
        };
        auto collectViewMap = [&all](const std::unordered_map<std::string, std::vector<VkImageView>>& map)
        {
            for (auto& kv : map)
                for (auto v : kv.second)
                    if (v != VK_NULL_HANDLE)
                        all.insert(v);
        };

        collectViews(inputImageViewsSRGB);
        collectViews(inputImageViewsUNORM);
        collectViews(outputImageViewsSRGB);
        collectViews(outputImageViewsUNORM);
        collectViews(backBufferImageViewsSRGB);
        collectViews(backBufferImageViewsUNORM);
        collectViewMap(textureImageViewsSRGB);
        collectViewMap(textureImageViewsUNORM);
        collectViewMap(renderImageViewsSRGB);
        collectViewMap(renderImageViewsUNORM);
        // Shared texture owners release their views only after the last effect.
        for (const auto& texture : sharedTextures)
        {
            all.erase(texture->unormView);
            all.erase(texture->srgbView);
            all.erase(texture->unormRenderView);
            all.erase(texture->srgbRenderView);
        }
        if (stencilImageView != VK_NULL_HANDLE)
            all.insert(stencilImageView);

        return all;
    }

    std::set<VkImage> ReshadeEffect::collectOwnedImages() const
    {
        std::set<VkImage> all;
        for (auto& kv : textureImages)
            for (auto img : kv.second)
                if (img != VK_NULL_HANDLE)
                    all.insert(img);
        for (const auto& texture : sharedTextures)
            all.erase(texture->image);
        for (auto img : backBufferImages)
            if (img != VK_NULL_HANDLE)
                all.insert(img);
        if (stencilImage != VK_NULL_HANDLE)
            all.insert(stencilImage);
        return all;
    }

    ReshadeEffect::~ReshadeEffect()
    {
        // Everything is handed to the deferred queue rather than released here.
        // The device guard moved into releaseResources() with the rest of the
        // logic; keeping it in one place is the point, since a null device is a
        // real case here: the VkDevice can be destroyed before every
        // shared_ptr<Effect> reference drops, on teardown paths where the
        // swapchain is destroyed and recreated before a frame ever renders.
        releaseResources();
    }

    void ReshadeEffect::createReshadeModule()
    {
        // One preprocess setup shared with the registry parse: same prelude,
        // depth macros, user macros and include paths, with the real extent.
        const uint32_t colorBitDepth = (inputOutputFormatUNORM == VK_FORMAT_A2R10G10B10_UNORM_PACK32) ? 10 : 8;
        const ReshadePreprocessSetup setup = effectRegistry->preprocessSetup(
            imageExtent.width, imageExtent.height, colorBitDepth, customPreprocessorDefs);
        for (const auto& def : customPreprocessorDefs)
            Logger::debug("  custom macro: " + def.name + " = " + def.value);

        // Use provided effectPath, or try to find it in discovered shader paths
        std::string shaderPath = this->effectPath;
        if (shaderPath.empty())
        {
            shaderPath = effectRegistry->getEffectFilePath(effectName);
            if (shaderPath.empty())
            {
                // Search discovered shader paths for the effect
                for (const auto& searchPath : setup.includePaths)
                {
                    std::string candidate = searchPath + "/" + effectName + ".fx";
                    if (std::filesystem::exists(candidate))
                    {
                        shaderPath = candidate;
                        break;
                    }
                    candidate = searchPath + "/" + effectName;
                    if (std::filesystem::exists(candidate))
                    {
                        shaderPath = candidate;
                        break;
                    }
                }
            }
        }
        // The resolved path identifies the effect file to the texture pool.
        this->effectPath = shaderPath;

        std::string preprocessedSource;
        std::string error;
        if (!preprocessReshadeFile(shaderPath, setup, preprocessedSource, error))
        {
            Logger::err("failed to load shader '" + effectName + "': " + error);
            throw EffectBuildError("failed to load shader: " + effectName + ": " + error);
        }

        const RuntimeCodegenPolicy runtimePolicy = selectRuntimeCodegenPolicy(logicalDevice);
        disableComputePipelineOptimization = runtimePolicy.disableComputePipelineOptimization;
        Logger::debug("runtime codegen policy: local_size_id=" + std::to_string(runtimePolicy.useLocalSizeId ? 1 : 0) +
                      " debug_info=" + std::to_string(runtimePolicy.emitDebugInfo ? 1 : 0) +
                      " disable_compute_opt=" + std::to_string(runtimePolicy.disableComputePipelineOptimization ? 1 : 0));

        // The key is the preprocessed source (every include and macro already
        // folded in) plus the macro digest, the codegen flags and the compiler
        // tag; see ShaderCacheKey. Memory in front of disk: an unchanged effect is
        // reused across chain rebuilds within a run and across runs.
        const uint32_t codegenFlags =
            (runtimePolicy.useLocalSizeId ? 1u : 0u) |
            (runtimePolicy.emitDebugInfo ? 4u : 0u);
        const ShaderCacheKey cacheKey = makeShaderCacheKey(preprocessedSource, hashPreprocessSetup(setup), codegenFlags);

        ShaderCache& shaderCache = sharedShaderCache();
        compiledModule = shaderCache.lookup(cacheKey);
        if (compiledModule)
        {
            Logger::debug("reshade compile cache hit for " + effectName);
        }
        else
        {
            const ReshadeCodegenOptions options{runtimePolicy.emitDebugInfo, runtimePolicy.useLocalSizeId};
            auto fresh = std::make_shared<reshadefx::module>();
            if (!compileReshadeSource(std::move(preprocessedSource), options, *fresh, error))
            {
                Logger::err("failed to compile shader '" + effectName + "': " + error);
                throw EffectBuildError("failed to compile shader: " + effectName + ": " + error);
            }
            compiledModule = fresh;
            shaderCache.store(cacheKey, compiledModule);
        }
        const reshadefx::module& module = *compiledModule;

        if (module.techniques.empty())
        {
            Logger::err("shader has no techniques: " + effectName);
            throw EffectBuildError("shader has no techniques: " + effectName);
        }

        if (module.spirv.empty())
        {
            Logger::err("shader produced empty SPIR-V: " + effectName);
            throw EffectBuildError("shader produced empty SPIR-V: " + effectName);
        }

        VkShaderModuleCreateInfo shaderCreateInfo;
        shaderCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.pNext    = nullptr;
        shaderCreateInfo.flags    = 0;
        shaderCreateInfo.codeSize = module.spirv.size() * sizeof(uint32_t);
        shaderCreateInfo.pCode    = module.spirv.data();

        VkResult result = logicalDevice->vkd.CreateShaderModule(logicalDevice->device, &shaderCreateInfo, nullptr, &shaderModule);
        if (result != VK_SUCCESS)
        {
            shaderModule = VK_NULL_HANDLE;
            Logger::err("failed to create shader module for: " + effectName);
            throw EffectBuildError("VkCreateShaderModule failed for: " + effectName);
        }

        Logger::debug("created reshade shaderModule");
    }

    VkFormat ReshadeEffect::convertReshadeFormat(reshadefx::texture_format texFormat)
    {
        switch (texFormat)
        {
            case reshadefx::texture_format::r8: return VK_FORMAT_R8_UNORM;
            case reshadefx::texture_format::r16: return VK_FORMAT_R16_UNORM;
            case reshadefx::texture_format::r16f: return VK_FORMAT_R16_SFLOAT;
            case reshadefx::texture_format::r32f: return VK_FORMAT_R32_SFLOAT;
            case reshadefx::texture_format::r32i: return VK_FORMAT_R32_SINT;
            case reshadefx::texture_format::rg8: return VK_FORMAT_R8G8_UNORM;
            case reshadefx::texture_format::rg16: return VK_FORMAT_R16G16_UNORM;
            case reshadefx::texture_format::rg16f: return VK_FORMAT_R16G16_SFLOAT;
            case reshadefx::texture_format::rg32f: return VK_FORMAT_R32G32_SFLOAT;
            case reshadefx::texture_format::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
            case reshadefx::texture_format::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
            case reshadefx::texture_format::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case reshadefx::texture_format::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case reshadefx::texture_format::rgb10a2: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            case reshadefx::texture_format::r32u: return VK_FORMAT_R32_UINT;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    VkCompareOp ReshadeEffect::convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp)
    {
        switch (compareOp)
        {
            case reshadefx::pass_stencil_func::never: return VK_COMPARE_OP_NEVER;
            case reshadefx::pass_stencil_func::less: return VK_COMPARE_OP_LESS;
            case reshadefx::pass_stencil_func::equal: return VK_COMPARE_OP_EQUAL;
            case reshadefx::pass_stencil_func::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case reshadefx::pass_stencil_func::greater: return VK_COMPARE_OP_GREATER;
            case reshadefx::pass_stencil_func::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
            case reshadefx::pass_stencil_func::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case reshadefx::pass_stencil_func::always: return VK_COMPARE_OP_ALWAYS;
            default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkStencilOp ReshadeEffect::convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp)
    {
        switch (stencilOp)
        {
            case reshadefx::pass_stencil_op::zero: return VK_STENCIL_OP_ZERO;
            case reshadefx::pass_stencil_op::keep: return VK_STENCIL_OP_KEEP;
            case reshadefx::pass_stencil_op::replace: return VK_STENCIL_OP_REPLACE;
            case reshadefx::pass_stencil_op::incr_sat: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case reshadefx::pass_stencil_op::decr_sat: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case reshadefx::pass_stencil_op::invert: return VK_STENCIL_OP_INVERT;
            case reshadefx::pass_stencil_op::incr: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case reshadefx::pass_stencil_op::decr: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default: return VK_STENCIL_OP_KEEP;
        }
    }

    VkBlendOp ReshadeEffect::convertReshadeBlendOp(reshadefx::pass_blend_op blendOp)
    {
        switch (blendOp)
        {
            case reshadefx::pass_blend_op::add: return VK_BLEND_OP_ADD;
            case reshadefx::pass_blend_op::subtract: return VK_BLEND_OP_SUBTRACT;
            case reshadefx::pass_blend_op::rev_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case reshadefx::pass_blend_op::min: return VK_BLEND_OP_MIN;
            case reshadefx::pass_blend_op::max: return VK_BLEND_OP_MAX;
            default: return VK_BLEND_OP_ADD;
        }
    }

    VkBlendFactor ReshadeEffect::convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor)
    {
        switch (blendFactor)
        {
            case reshadefx::pass_blend_func::zero: return VK_BLEND_FACTOR_ZERO;
            case reshadefx::pass_blend_func::one: return VK_BLEND_FACTOR_ONE;
            case reshadefx::pass_blend_func::src_color: return VK_BLEND_FACTOR_SRC_COLOR;
            case reshadefx::pass_blend_func::src_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
            case reshadefx::pass_blend_func::inv_src_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case reshadefx::pass_blend_func::inv_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case reshadefx::pass_blend_func::dst_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
            case reshadefx::pass_blend_func::inv_dst_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case reshadefx::pass_blend_func::dst_color: return VK_BLEND_FACTOR_DST_COLOR;
            case reshadefx::pass_blend_func::inv_dst_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            default: return VK_BLEND_FACTOR_ZERO;
        }
    }
} // namespace VkHook
