#ifndef EFFECT_RESHADE_HPP_INCLUDED
#define EFFECT_RESHADE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <set>
#include <array>

#include "vulkan_include.hh"

#include "effect.hh"
#include "config.hh"
#include "effect_config.hh"
#include "effect_registry.hh"
#include "reshade_uniforms.hh"
#include "reshade_texture_pool.hh"

#include "logical_device.hh"

#include "reshade/effect_parser.hh"
#include "reshade/effect_codegen.hh"
#include "reshade/effect_preprocessor.hh"

namespace VkHook
{
    class ReshadeEffect : public Effect
    {
    public:
        ReshadeEffect(LogicalDevice*       logicalDevice,
                      VkFormat             format,
                      VkExtent2D           imageExtent,
                      std::vector<VkImage> inputImages,
                      std::vector<VkImage> outputImages,
                      EffectRegistry*      effectRegistry,
                      ReshadeTexturePool& texturePool,
                      std::string          effectName,
                      std::string          effectPath = "",  // Optional: explicit path to .fx file
                      std::vector<PreprocessorDefinition> customDefs = {});  // Custom preprocessor definitions
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void virtual updateEffect() override;
        void virtual useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout) override;
        int getOutputWrites() const { return outputWrites; }
        virtual ~ReshadeEffect();

        // Hands every GPU object this effect owns to the deferred destroy queue
        // instead of releasing them inline. The handles stay raw members so the
        // several hundred use sites are untouched; what changes is that nothing is
        // destroyed at scope exit any more, and the queue enforces the release
        // order at a point where the GPU is known to be finished with it.
        void releaseResources();

        // Deduplicated views and images, so a handle reachable through several
        // members is only ever released once. See the definitions for why the
        // per-member loops this replaced were crashing the NVIDIA driver.
        std::set<VkImageView> collectOwnedImageViews() const;
        std::set<VkImage>     collectOwnedImages() const;

    private:
        // The constructor body. Split out so the constructor can release what a
        // failed build already created (a throwing constructor runs no destructor).
        void build(LogicalDevice*       logicalDevice,
                   VkFormat             format,
                   VkExtent2D           imageExtent,
                   std::vector<VkImage> inputImages,
                   std::vector<VkImage> outputImages,
                   EffectRegistry*      effectRegistry,
                   ReshadeTexturePool& texturePool,
                   std::string          effectName,
                   std::string          effectPath,
                   std::vector<PreprocessorDefinition> customDefs);

        // Throws EffectBuildError unless `sets` holds one descriptor set per swapchain image.
        void requireDescriptorSets(const std::vector<VkDescriptorSet>& sets, const char* what) const;

        LogicalDevice*           logicalDevice = nullptr;
        std::vector<VkImage>     inputImages;
        std::vector<VkImage>     outputImages;
        std::vector<VkImageView> inputImageViewsSRGB;
        std::vector<VkImageView> inputImageViewsUNORM;
        std::vector<VkImageView> outputImageViewsSRGB;
        std::vector<VkImageView> outputImageViewsUNORM;

        std::unordered_map<std::string, std::vector<VkImage>>     textureImages;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsUNORM;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsUNORM;

        std::unordered_map<std::string, VkFormat>   textureFormatsUNORM;
        std::unordered_map<std::string, VkFormat>   textureFormatsSRGB;
        std::unordered_map<std::string, uint32_t>   textureMipLevels;
        std::unordered_map<std::string, VkExtent3D> textureExtents;
        // Own shared resources independently of the build-local lookup pool.
        std::vector<std::shared_ptr<ReshadeTexture>> sharedTextures;

        std::vector<VkDescriptorSet> inputDescriptorSets;
        std::vector<VkDescriptorSet> outputDescriptorSets;
        std::vector<VkDescriptorSet> backBufferDescriptorSets;

        struct PassRuntime
        {
            bool                        isCompute = false;
            VkPipeline                  pipeline = VK_NULL_HANDLE;
            VkRenderPass                renderPass = VK_NULL_HANDLE;
            std::vector<VkFramebuffer>  framebuffers;
            std::vector<std::string>    renderTargets;
            bool                        switchSamplers = false;
            uint32_t                    vertexCount = 3;
            uint32_t                    dispatchSizeX = 1;
            uint32_t                    dispatchSizeY = 1;
            uint32_t                    dispatchSizeZ = 1;
            VkRect2D                    renderArea = {};
            uint32_t                    clearValueCount = 0;
            std::array<VkClearValue, 9> clearValues = {};
        };
        std::vector<PassRuntime> passRuntimes;

        VkDescriptorSetLayout                 uniformDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout                 imageSamplerDescriptorSetLayout = VK_NULL_HANDLE;
        VkShaderModule                        shaderModule = VK_NULL_HANDLE;
        VkDescriptorPool                      descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout                      pipelineLayout = VK_NULL_HANDLE;
        VkExtent2D                            imageExtent;
        std::vector<VkSampler>                samplers;
        EffectRegistry*                       effectRegistry = nullptr;
        std::string                           effectName;
        std::string                           effectPath;  // Path to .fx file (may differ from effectName)
        std::vector<PreprocessorDefinition>   customPreprocessorDefs;  // User-defined macros
        reshadefx::module                     module;
        std::vector<VkDeviceMemory>           textureMemory;

        VkFormat    inputOutputFormatUNORM = VK_FORMAT_UNDEFINED;
        VkFormat    inputOutputFormatSRGB = VK_FORMAT_UNDEFINED;
        VkFormat    stencilFormat = VK_FORMAT_UNDEFINED;
        VkImage     stencilImage = VK_NULL_HANDLE;
        VkImageView stencilImageView = VK_NULL_HANDLE;
        // how often the shader writes to the reshade back buffer
        // we need to flip the "backbuffer" after each write if there is a next one
        int                      outputWrites = 0;
        std::vector<VkImage>     backBufferImages;
        std::vector<bool>        outputImageInitialized;
        std::vector<VkImageView> backBufferImageViewsUNORM;
        std::vector<VkImageView> backBufferImageViewsSRGB;
        // Effects without uniforms never allocate this buffer.
        VkBuffer                 stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory           stagingBufferMemory = VK_NULL_HANDLE;
        uint32_t                 bufferSize = 0;
        void*                    stagingBufferMapped = nullptr;  // Persistent map (HOST_COHERENT)
        VkDescriptorSet          bufferDescriptorSet = VK_NULL_HANDLE;
        bool                     disableComputePipelineOptimization = false;

        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;
        // Non-owning views into `uniforms` for the bufready_depth sources.
        std::vector<DepthUniform*> depthUniforms;
        // User-editable uniforms resolved to parameter storage; valid only while
        // the registry generation is unchanged, since a reparse frees the params.
        std::vector<UniformBinding> uniformBindings;
        uint64_t bindingsGeneration = 0;

        void          buildBindings();
        void          createReshadeModule();
        VkFormat      convertReshadeFormat(reshadefx::texture_format texFormat);
        VkCompareOp   convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp);
        VkStencilOp   convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp);
        VkBlendOp     convertReshadeBlendOp(reshadefx::pass_blend_op blendOp);
        VkBlendFactor convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor);
    };
} // namespace VkHook

#endif // EFFECT_RESHADE_HPP_INCLUDED
