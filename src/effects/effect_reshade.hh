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

#include "compiler.hh"
#include "effect.hh"
#include "effect_id.hh"
#include "config.hh"
#include "effect_config.hh"
#include "effect_registry.hh"
#include "reshade_uniforms.hh"
#include "reshade_texture_pool.hh"
#include "uniform_ring.hh"

#include "logical_device.hh"

#include "reshade/effect_parser.hh"
#include "reshade/effect_codegen.hh"
#include "reshade/effect_preprocessor.hh"

namespace VkHook
{
    class ReshadeEffect : public Effect
    {
    public:
        // `effectName` is an EffectId text (EffectId::format()): the effect runs
        // that id's technique of the file. An unresolved technique means the first.
        ReshadeEffect(LogicalDevice*       logicalDevice,
                      VkFormat             format,
                      VkExtent2D           imageExtent,
                      std::vector<VkImage> inputImages,
                      std::vector<VkImage> outputImages,
                      EffectRegistry*      effectRegistry,
                      ReshadeTexturePool& texturePool,
                      std::string          effectName,
                      std::string          effectPath = "",  // Optional: explicit path to .fx file
                      std::vector<PreprocessorDefinition> customDefs = {},  // Custom preprocessor definitions
                      // A finished compile of this entry (from EffectCompiler); when
                      // null the effect compiles synchronously through
                      // effectRegistry->shaderCache(). Its techniqueIndex selects the technique.
                      std::shared_ptr<const EffectCompileResult> precompiled = nullptr);
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        // Writes this image's slice of the uniform ring (see UniformRing).
        bool updateFrame(uint32_t imageIndex) override;
        void virtual useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout) override;
        int getOutputWrites() const { return outputWrites; }
        // True when the chain must follow this effect with a TransferEffect for
        // its output image to hold the result (see reshadeOutputNeedsForward).
        bool requiresOutputForward() const { return outputForwardRequired; }
        virtual ~ReshadeEffect();

        // Hands every GPU object this effect owns to the device's RetireQueue
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
                   std::vector<PreprocessorDefinition> customDefs,
                   std::shared_ptr<const EffectCompileResult> precompiled);

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
            VkViewport                  viewport = {};
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
        std::string                           effectName;  // EffectId::format()
        std::string                           effectPath;  // Path to .fx file (may differ from effectName)
        std::vector<PreprocessorDefinition>   customPreprocessorDefs;  // User-defined macros
        // Shared with the compile cache and every other effect built from the
        // same source; never modified after it is built.
        std::shared_ptr<const reshadefx::module> compiledModule;
        // The entry this effect runs: `effectName` parsed as an EffectId, with an
        // unresolved technique replaced by the file's first. The effect runs
        // exactly compiledModule->techniques[techniqueIndex].
        EffectId                              effectId;
        size_t                                techniqueIndex = 0;
        std::vector<VkDeviceMemory>           textureMemory;

        VkFormat    inputOutputFormatUNORM = VK_FORMAT_UNDEFINED;
        VkFormat    inputOutputFormatSRGB = VK_FORMAT_UNDEFINED;
        VkFormat    stencilFormat = VK_FORMAT_UNDEFINED;
        VkImage     stencilImage = VK_NULL_HANDLE;
        VkImageView stencilImageView = VK_NULL_HANDLE;
        // how often the shader writes to the reshade back buffer
        // we need to flip the "backbuffer" after each write if there is a next one
        int                      outputWrites = 0;
        bool                     outputForwardRequired = false;
        std::vector<VkImage>     backBufferImages;
        std::vector<VkImageView> backBufferImageViewsUNORM;
        std::vector<VkImageView> backBufferImageViewsSRGB;
        // One host-visible buffer holding one uniform slice per swapchain image
        // (uniformRing), persistently mapped and HOST_COHERENT. Slice i is written
        // by updateFrame(i) and bound by bufferDescriptorSets[i], so a frame in
        // flight on another image never sees its uniforms change underneath it.
        // Effects without uniforms never allocate any of this.
        VkBuffer                 uniformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory           uniformBufferMemory = VK_NULL_HANDLE;
        uint32_t                 bufferSize = 0;       // bytes of one slice
        void*                    uniformBufferMapped = nullptr;
        UniformRing              uniformRing;
        std::vector<VkDescriptorSet> bufferDescriptorSets;
        // Shader defaults of the uniforms without a live parameter, copied into a
        // slice whenever the bindings are rebuilt (UniformRing::needsDefaults).
        std::vector<uint8_t>     uniformDefaults;
        bool                     disableComputePipelineOptimization = false;

        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;
        // Non-owning views into `uniforms` for the bufready_depth sources.
        std::vector<DepthUniform*> depthUniforms;
        // User-editable uniforms resolved to parameter storage; valid only while
        // the registry generation is unchanged, since a reparse frees the params.
        std::vector<UniformBinding> uniformBindings;
        uint64_t bindingsGeneration = 0;

        void          buildBindings(const reshadefx::module& module);
        void          createReshadeModule(std::shared_ptr<const EffectCompileResult> precompiled);
        VkFormat      convertReshadeFormat(reshadefx::texture_format texFormat);
        VkCompareOp   convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp);
        VkStencilOp   convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp);
        VkBlendOp     convertReshadeBlendOp(reshadefx::pass_blend_op blendOp);
        VkBlendFactor convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor);
    };
} // namespace VkHook

#endif // EFFECT_RESHADE_HPP_INCLUDED
