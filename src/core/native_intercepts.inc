// Included after the existing interceptors, inside namespace VKIntox. These
// wrappers are installed only for VKINTOX_DDDA_GBUFFER=1; forwarding keeps the
// established depth path intact and never holds globalLock across an old hook.
template<typename Handle, typename Function>
static void nativeTrack(Handle handle, Function&& function)
{
    scoped_lock lock(globalLock);
    auto found = deviceMap.find(GetKey(handle));
    if (found == deviceMap.end()) return;
    auto* device = found->second.get();
    if (!device->nativeCapture) device->nativeCapture = std::make_shared<NativeCapture>();
    function(*device->nativeCapture, device);
}
template<typename Function, typename Handle, size_t N>
static Function nativeDriver(Handle handle, const char (&name)[N])
{
    scoped_lock lock(globalLock);
    auto found = deviceMap.find(GetKey(handle));
    if (found == deviceMap.end()) return nullptr;
    auto& device = *found->second;
    if (!device.nativeCapture) device.nativeCapture = std::make_shared<NativeCapture>();
    // Device-scoped cache: no driver proc lookup or allocation on hot draw paths.
    auto [entry, inserted] = device.nativeCapture->driverFunctions.try_emplace(std::string_view(name, N - 1), nullptr);
    if (inserted) entry->second = device.vkd.GetDeviceProcAddr(device.device, name);
    return reinterpret_cast<Function>(entry->second);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image)
{
    auto modified = *info;
    const bool candidate = info->format == VK_FORMAT_B8G8R8A8_UNORM && info->imageType == VK_IMAGE_TYPE_2D
        && info->extent.depth == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT && info->arrayLayers == 1
        && info->mipLevels == 1 && info->tiling == VK_IMAGE_TILING_OPTIMAL
        && (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        && !(info->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        && !(info->flags & ~(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT));
    if (candidate) modified.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = VKIntox_CreateImage(device, &modified, allocator, image);
    if (result == VK_SUCCESS && candidate)
        nativeTrack(device, [&](auto& state, auto*) { state.images[*image] = {info->extent, ++state.generation}; });
    else if (candidate && modified.usage != info->usage) {
        // Optional capture must not turn a supported application allocation into
        // a failure on a device that cannot add transfer-source usage.
        result = VKIntox_CreateImage(device, info, allocator, image);
        Logger::warn("DDDA native G-buffer unavailable: image cannot add transfer-source usage");
    }
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.images.erase(image); });
    VKIntox_DestroyImage(device, image, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view)
{
    VkResult result = VKIntox_CreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS) nativeTrack(device, [&](auto& state, auto*) {
        const auto image = state.images.find(info->image);
        if (image != state.images.end() && info->format == VK_FORMAT_B8G8R8A8_UNORM && info->viewType == VK_IMAGE_VIEW_TYPE_2D
            && !info->subresourceRange.baseMipLevel && !info->subresourceRange.baseArrayLayer
            && (info->subresourceRange.levelCount == 1 || info->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS)
            && (info->subresourceRange.layerCount == 1 || info->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS)
            && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT)
            state.views[*view] = {info->image, image->second.generation};
    });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.views.erase(view); });
    VKIntox_DestroyImageView(device, view, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator, VkShaderModule* shader)
{
    const auto result = nativeDriver<PFN_vkCreateShaderModule>(device, "vkCreateShaderModule")(device, info, allocator, shader);
    if (result == VK_SUCCESS && nativeProducerShader(info->pCode, info->codeSize))
        nativeTrack(device, [&](auto& state, auto*) { state.shaders.insert(*shader); });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyShaderModule(VkDevice device, VkShaderModule shader, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.shaders.erase(shader); });
    nativeDriver<PFN_vkDestroyShaderModule>(device, "vkDestroyShaderModule")(device, shader, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    auto result = nativeDriver<PFN_vkCreateGraphicsPipelines>(device, "vkCreateGraphicsPipelines")(device, cache, count, infos, allocator, pipelines);
    nativeTrack(device, [&](auto& state, auto*) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& info = infos[i];
            if (!pipelines[i]) continue;
            NativeCapture::Pipeline tracked;
            for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
                if (p->sType != VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) continue;
                const auto& libraries = *reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(p);
                for (uint32_t j = 0; j < libraries.libraryCount; ++j) {
                    const auto library = state.pipelines.find(libraries.pLibraries[j]);
                    if (library == state.pipelines.end()) continue;
                    tracked.producer |= library->second.producer;
                    tracked.output |= library->second.output;
                    tracked.dynamic |= library->second.dynamic;
                    tracked.discard |= library->second.discard;
                    if (library->second.output) {
                        tracked.alphaPreserved = library->second.alphaPreserved;
                        tracked.blendEnabled = library->second.blendEnabled;
                    }
                }
            }
            uint32_t dynamic = 0;
            if (info.pDynamicState) for (uint32_t j = 0; j < info.pDynamicState->dynamicStateCount; ++j) {
                switch (info.pDynamicState->pDynamicStates[j]) {
                case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT: dynamic |= 1; break;
                case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT: dynamic |= 2; break;
                case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT: dynamic |= 4; break;
                case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT: dynamic |= 8; break;
                case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT: dynamic |= 16; break;
                case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: dynamic |= 32; break;
                default: break;
                }
            }
            tracked.dynamic |= dynamic;
            if (info.pRasterizationState && !(dynamic & 32))
                tracked.discard |= info.pRasterizationState->rasterizerDiscardEnable;
            if (info.pColorBlendState && info.pColorBlendState->attachmentCount == 1 && info.pMultisampleState) {
                const auto& color = *info.pColorBlendState;
                tracked.alphaPreserved = color.pAttachments && color.pAttachments[0].colorWriteMask == 7;
                tracked.blendEnabled = color.pAttachments && color.pAttachments[0].blendEnable;
                tracked.output = ((dynamic & 8) || !color.logicOpEnable)
                    && ((dynamic & 2) || (color.pAttachments && (color.pAttachments[0].colorWriteMask & 7) == 7))
                    && ((dynamic & 16) || info.pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT);
                for (auto p = static_cast<const VkBaseInStructure*>(color.pNext); p; p = p->pNext)
                    if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT && !(dynamic & 4)) {
                        const auto& writes = *reinterpret_cast<const VkPipelineColorWriteCreateInfoEXT*>(p);
                        tracked.output &= writes.attachmentCount && writes.pColorWriteEnables[0];
                    }
            }
            for (uint32_t j = 0; j < info.stageCount; ++j) {
                const auto& stage = info.pStages[j];
                if (stage.stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
                tracked.producer |= state.shaders.count(stage.module) != 0;
                // DXVK GPL creates modules inline rather than vkCreateShaderModule.
                for (auto p = static_cast<const VkBaseInStructure*>(stage.pNext); p; p = p->pNext) {
                    if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                        const auto& module = *reinterpret_cast<const VkShaderModuleCreateInfo*>(p);
                        tracked.producer |= nativeProducerShader(module.pCode, module.codeSize);
                    } else if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT) {
                        const auto& id = *reinterpret_cast<const VkPipelineShaderStageModuleIdentifierCreateInfoEXT*>(p);
                        tracked.producer |= state.shaderIdentifiers.count(std::string(reinterpret_cast<const char*>(id.pIdentifier), id.identifierSize)) != 0;
                    }
                }
            }
            state.pipelines[pipelines[i]] = tracked;
        }
    });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.pipelines.erase(pipeline); });
    nativeDriver<PFN_vkDestroyPipeline>(device, "vkDestroyPipeline")(device, pipeline, allocator);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline pipeline)
{
    nativeDriver<PFN_vkCmdBindPipeline>(cb, "vkCmdBindPipeline")(cb, bind, pipeline);
    if (bind == VK_PIPELINE_BIND_POINT_GRAPHICS) nativeTrack(cb, [&](auto& state, auto*) {
        const auto found = state.pipelines.find(pipeline);
        auto& c = state.commands[cb];
        c.producer = found != state.pipelines.end() && found->second.producer && found->second.output && !found->second.discard;
        c.dynamicRequired = found == state.pipelines.end() ? 0 : found->second.dynamic;
        c.dynamicKnown &= c.dynamicRequired;
        c.staticBlending = found != state.pipelines.end() && !(c.dynamicRequired & 1) && found->second.blendEnabled;
        if (!(c.dynamicRequired & 2))
            c.alphaPreserved = found != state.pipelines.end() && found->second.alphaPreserved;
    });
}
static VKAPI_ATTR void VKAPI_CALL Native_GetShaderModuleCreateInfoIdentifierEXT(VkDevice device, const VkShaderModuleCreateInfo* info, VkShaderModuleIdentifierEXT* identifier)
{
    nativeDriver<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(device, "vkGetShaderModuleCreateInfoIdentifierEXT")(device, info, identifier);
    if (nativeProducerShader(info->pCode, info->codeSize)) nativeTrack(device, [&](auto& state, auto*) {
        state.shaderIdentifiers.insert(std::string(reinterpret_cast<const char*>(identifier->identifier), identifier->identifierSize));
    });
}
static VKAPI_ATTR void VKAPI_CALL Native_GetShaderModuleIdentifierEXT(VkDevice device, VkShaderModule module, VkShaderModuleIdentifierEXT* identifier)
{
    nativeDriver<PFN_vkGetShaderModuleIdentifierEXT>(device, "vkGetShaderModuleIdentifierEXT")(device, module, identifier);
    nativeTrack(device, [&](auto& state, auto*) {
        if (state.shaders.count(module))
            state.shaderIdentifiers.insert(std::string(reinterpret_cast<const char*>(identifier->identifier), identifier->identifierSize));
    });
}
static void nativeDynamicColor(VkCommandBuffer cb, uint32_t bit, bool valid, bool alphaPreserved = false)
{
    nativeTrack(cb, [&](auto& state, auto*) {
        auto& c = state.commands[cb];
        c.dynamicKnown |= bit;
        c.dynamicValid = (c.dynamicValid & ~bit) | (valid ? bit : 0);
        if (bit == 2) c.alphaPreserved = alphaPreserved;
    });
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBool32* values)
{
    nativeDriver<PFN_vkCmdSetColorBlendEnableEXT>(cb, "vkCmdSetColorBlendEnableEXT")(cb, first, count, values);
    if (!first && count) nativeDynamicColor(cb, 1, !values[0]);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkColorComponentFlags* values)
{
    nativeDriver<PFN_vkCmdSetColorWriteMaskEXT>(cb, "vkCmdSetColorWriteMaskEXT")(cb, first, count, values);
    if (!first && count) nativeDynamicColor(cb, 2, (values[0] & 7) == 7, values[0] == 7);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdSetColorWriteEnableEXT(VkCommandBuffer cb, uint32_t count, const VkBool32* values)
{
    nativeDriver<PFN_vkCmdSetColorWriteEnableEXT>(cb, "vkCmdSetColorWriteEnableEXT")(cb, count, values);
    if (count) nativeDynamicColor(cb, 4, values[0]);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdSetLogicOpEnableEXT(VkCommandBuffer cb, VkBool32 value)
{
    nativeDriver<PFN_vkCmdSetLogicOpEnableEXT>(cb, "vkCmdSetLogicOpEnableEXT")(cb, value);
    nativeDynamicColor(cb, 8, !value);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdSetRasterizationSamplesEXT(VkCommandBuffer cb, VkSampleCountFlagBits value)
{
    nativeDriver<PFN_vkCmdSetRasterizationSamplesEXT>(cb, "vkCmdSetRasterizationSamplesEXT")(cb, value);
    nativeDynamicColor(cb, 16, value == VK_SAMPLE_COUNT_1_BIT);
}
#define NATIVE_DISCARD(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, VkBool32 value) { \
    nativeDriver<PFN_vk##Name>(cb, "vk" #Name)(cb, value); nativeDynamicColor(cb, 32, !value); }
NATIVE_DISCARD(CmdSetRasterizerDiscardEnable)
NATIVE_DISCARD(CmdSetRasterizerDiscardEnableEXT)
#undef NATIVE_DISCARD
template<typename Info>
static void nativePass(NativeCapture& state, VkRenderPass pass, const Info& info)
{
    auto& result = state.passes[pass];
    if (info.subpassCount != 1 || info.pSubpasses[0].colorAttachmentCount != 1) return;
    const uint32_t index = info.pSubpasses[0].pColorAttachments[0].attachment;
    if (index == VK_ATTACHMENT_UNUSED || index >= info.attachmentCount) return;
    const auto& a = info.pAttachments[index];
    result.attachment = index;
    result.finalLayout = a.finalLayout;
    result.usable = a.format == VK_FORMAT_B8G8R8A8_UNORM && a.samples == VK_SAMPLE_COUNT_1_BIT
        && a.storeOp == VK_ATTACHMENT_STORE_OP_STORE && !info.pSubpasses[0].viewMask;
}
// VkSubpassDescription (render-pass 1) has no viewMask.
static void nativePass(NativeCapture& state, VkRenderPass pass, const VkRenderPassCreateInfo& info)
{
    auto& result = state.passes[pass];
    if (info.subpassCount != 1 || info.pSubpasses[0].colorAttachmentCount != 1) return;
    for (auto p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) return;
    const auto index = info.pSubpasses[0].pColorAttachments[0].attachment;
    if (index == VK_ATTACHMENT_UNUSED || index >= info.attachmentCount) return;
    const auto& a = info.pAttachments[index];
    result = {index, a.finalLayout, a.format == VK_FORMAT_B8G8R8A8_UNORM && a.samples == VK_SAMPLE_COUNT_1_BIT && a.storeOp == VK_ATTACHMENT_STORE_OP_STORE};
}
#define NATIVE_CREATE_PASS(Name, Info) \
static VKAPI_ATTR VkResult VKAPI_CALL Native_##Name(VkDevice device, const Info* info, const VkAllocationCallbacks* allocator, VkRenderPass* pass) { \
    auto result = VKIntox_##Name(device, info, allocator, pass); \
    if (result == VK_SUCCESS) nativeTrack(device, [&](auto& state, auto*) { nativePass(state, *pass, *info); }); \
    return result; }
NATIVE_CREATE_PASS(CreateRenderPass, VkRenderPassCreateInfo)
NATIVE_CREATE_PASS(CreateRenderPass2, VkRenderPassCreateInfo2)
NATIVE_CREATE_PASS(CreateRenderPass2KHR, VkRenderPassCreateInfo2)
#undef NATIVE_CREATE_PASS
static VKAPI_ATTR void VKAPI_CALL Native_DestroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.passes.erase(pass); });
    VKIntox_DestroyRenderPass(device, pass, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer)
{
    auto result = VKIntox_CreateFramebuffer(device, info, allocator, framebuffer);
    if (result == VK_SUCCESS && !(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && info->layers == 1)
        nativeTrack(device, [&](auto& state, auto*) { state.framebuffers[*framebuffer].attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount); });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* allocator)
{
    nativeTrack(device, [&](auto& state, auto*) { state.framebuffers.erase(framebuffer); });
    VKIntox_DestroyFramebuffer(device, framebuffer, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkCommandPool* pool)
{
    auto result = nativeDriver<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool")(device, info, allocator, pool);
    if (result == VK_SUCCESS) nativeTrack(device, [&](auto& state, auto*) {
        state.pools[*pool] = (info->flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT) ? UINT32_MAX : info->queueFamilyIndex;
    });
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers)
{
    auto result = nativeDriver<PFN_vkAllocateCommandBuffers>(device, "vkAllocateCommandBuffers")(device, info, buffers);
    if (result == VK_SUCCESS) nativeTrack(device, [&](auto& state, auto*) {
        const auto pool = state.pools.find(info->commandPool);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            auto& c = state.commands[buffers[i]];
            c.pool = info->commandPool;
            c.level = info->level;
            c.queueFamily = pool == state.pools.end() ? UINT32_MAX : pool->second;
        }
    });
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info)
{
    auto result = VKIntox_BeginCommandBuffer(cb, info);
    if (result == VK_SUCCESS) nativeTrack(cb, [&](auto& state, auto*) { state.begin(cb, *info); });
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags)
{
    auto result = nativeDriver<PFN_vkResetCommandBuffer>(cb, "vkResetCommandBuffer")(cb, flags);
    if (result == VK_SUCCESS) nativeTrack(cb, [&](auto& state, auto*) { state.begin(cb, {}); });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers)
{
    VKIntox_FreeCommandBuffers(device, pool, count, buffers);
    nativeTrack(device, [&](auto& state, auto*) { for (uint32_t i = 0; i < count; ++i) state.commands.erase(buffers[i]); });
}
static VKAPI_ATTR VkResult VKAPI_CALL Native_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    auto result = nativeDriver<PFN_vkResetCommandPool>(device, "vkResetCommandPool")(device, pool, flags);
    if (result == VK_SUCCESS) nativeTrack(device, [&](auto& state, auto*) { state.erasePool(pool, false); });
    return result;
}
static VKAPI_ATTR void VKAPI_CALL Native_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
{
    nativeDriver<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool")(device, pool, allocator);
    nativeTrack(device, [&](auto& state, auto*) { state.erasePool(pool, true); state.pools.erase(pool); });
}
#define NATIVE_DYNAMIC_BEGIN(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, const VkRenderingInfo* info) { \
    nativeTrack(cb, [&](auto& state, auto*) { state.dynamicBegin(cb, *info); }); VKIntox_##Name(cb, info); }
NATIVE_DYNAMIC_BEGIN(CmdBeginRendering)
NATIVE_DYNAMIC_BEGIN(CmdBeginRenderingKHR)
#undef NATIVE_DYNAMIC_BEGIN
static VKAPI_ATTR void VKAPI_CALL Native_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents contents)
{
    nativeTrack(cb, [&](auto& state, auto*) { state.legacyBegin(cb, *info); });
    VKIntox_CmdBeginRenderPass(cb, info, contents);
}
#define NATIVE_LEGACY_BEGIN(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass) { \
    nativeTrack(cb, [&](auto& state, auto*) { state.legacyBegin(cb, *info); }); VKIntox_##Name(cb, info, subpass); }
NATIVE_LEGACY_BEGIN(CmdBeginRenderPass2)
NATIVE_LEGACY_BEGIN(CmdBeginRenderPass2KHR)
#undef NATIVE_LEGACY_BEGIN
#define NATIVE_END(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb) { \
    VKIntox_##Name(cb); nativeTrack(cb, [&](auto& state, auto* device) { state.end(device, cb); }); }
NATIVE_END(CmdEndRendering)
NATIVE_END(CmdEndRenderingKHR)
NATIVE_END(CmdEndRenderPass)
#undef NATIVE_END
#define NATIVE_END2(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, const VkSubpassEndInfo* info) { \
    VKIntox_##Name(cb, info); nativeTrack(cb, [&](auto& state, auto* device) { state.end(device, cb); }); }
NATIVE_END2(CmdEndRenderPass2)
NATIVE_END2(CmdEndRenderPass2KHR)
#undef NATIVE_END2
static VKAPI_ATTR void VKAPI_CALL Native_CmdExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* children)
{
    nativeTrack(cb, [&](auto& state, auto*) { state.execute(cb, count, children); });
    VKIntox_CmdExecuteCommands(cb, count, children);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    nativeTrack(cb, [&](auto& state, auto*) { state.draw(cb, vertices && instances); });
    VKIntox_CmdDraw(cb, vertices, instances, first, firstInstance);
}
static VKAPI_ATTR void VKAPI_CALL Native_CmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t first, int32_t vertexOffset, uint32_t firstInstance)
{
    nativeTrack(cb, [&](auto& state, auto*) { state.draw(cb, indices && instances); });
    VKIntox_CmdDrawIndexed(cb, indices, instances, first, vertexOffset, firstInstance);
}
#define NATIVE_INDIRECT(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) { \
    nativeTrack(cb, [&](auto& state, auto*) { state.draw(cb, count != 0); }); VKIntox_##Name(cb, buffer, offset, count, stride); }
NATIVE_INDIRECT(CmdDrawIndirect)
NATIVE_INDIRECT(CmdDrawIndexedIndirect)
#undef NATIVE_INDIRECT
#define NATIVE_INDIRECT_COUNT(Name) \
static VKAPI_ATTR void VKAPI_CALL Native_##Name(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t count, uint32_t stride) { \
    nativeTrack(cb, [&](auto& state, auto*) { state.draw(cb, count != 0); }); VKIntox_##Name(cb, buffer, offset, countBuffer, countOffset, count, stride); }
NATIVE_INDIRECT_COUNT(CmdDrawIndirectCount)
NATIVE_INDIRECT_COUNT(CmdDrawIndirectCountKHR)
NATIVE_INDIRECT_COUNT(CmdDrawIndexedIndirectCount)
NATIVE_INDIRECT_COUNT(CmdDrawIndexedIndirectCountKHR)
#undef NATIVE_INDIRECT_COUNT
static VKAPI_ATTR VkResult VKAPI_CALL Native_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
{
    auto result = VKIntox_QueueSubmit(queue, count, infos, fence);
    if (result == VK_SUCCESS) nativeTrack(queue, [&](auto& state, auto*) {
        for (uint32_t i = 0; i < count; ++i) for (uint32_t j = 0; j < infos[i].commandBufferCount; ++j) state.submitted(queue, infos[i].pCommandBuffers[j]);
    });
    return result;
}
#define NATIVE_SUBMIT2(Name) \
static VKAPI_ATTR VkResult VKAPI_CALL Native_##Name(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) { \
    auto result = VKIntox_##Name(queue, count, infos, fence); \
    if (result == VK_SUCCESS) nativeTrack(queue, [&](auto& state, auto*) { \
        for (uint32_t i = 0; i < count; ++i) for (uint32_t j = 0; j < infos[i].commandBufferInfoCount; ++j) state.submitted(queue, infos[i].pCommandBufferInfos[j].commandBuffer); }); \
    return result; }
NATIVE_SUBMIT2(QueueSubmit2)
NATIVE_SUBMIT2(QueueSubmit2KHR)
#undef NATIVE_SUBMIT2
PFN_vkVoidFunction nativeIntercept(const char* name)
{
    if (!nativeCaptureEnabled()) return nullptr;
#define NATIVE_PROC(Name) if (std::strcmp(name, "vk" #Name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Native_##Name)
    NATIVE_PROC(CreateImage); NATIVE_PROC(DestroyImage); NATIVE_PROC(CreateImageView); NATIVE_PROC(DestroyImageView);
    NATIVE_PROC(CreateShaderModule); NATIVE_PROC(DestroyShaderModule); NATIVE_PROC(CreateGraphicsPipelines); NATIVE_PROC(DestroyPipeline);
    NATIVE_PROC(GetShaderModuleCreateInfoIdentifierEXT);
    NATIVE_PROC(CmdSetColorBlendEnableEXT); NATIVE_PROC(CmdSetColorWriteMaskEXT); NATIVE_PROC(CmdSetColorWriteEnableEXT);
    NATIVE_PROC(CmdSetLogicOpEnableEXT); NATIVE_PROC(CmdSetRasterizationSamplesEXT);
    NATIVE_PROC(CmdBindPipeline); NATIVE_PROC(CreateRenderPass); NATIVE_PROC(CreateRenderPass2); NATIVE_PROC(CreateRenderPass2KHR);
    NATIVE_PROC(CmdSetRasterizerDiscardEnable); NATIVE_PROC(CmdSetRasterizerDiscardEnableEXT);
    NATIVE_PROC(GetShaderModuleIdentifierEXT);
    NATIVE_PROC(DestroyRenderPass); NATIVE_PROC(CreateFramebuffer); NATIVE_PROC(DestroyFramebuffer);
    NATIVE_PROC(AllocateCommandBuffers); NATIVE_PROC(BeginCommandBuffer); NATIVE_PROC(ResetCommandBuffer); NATIVE_PROC(FreeCommandBuffers);
    NATIVE_PROC(CreateCommandPool);
    NATIVE_PROC(ResetCommandPool); NATIVE_PROC(DestroyCommandPool); NATIVE_PROC(CmdExecuteCommands);
    NATIVE_PROC(CmdBeginRendering); NATIVE_PROC(CmdBeginRenderingKHR); NATIVE_PROC(CmdEndRendering); NATIVE_PROC(CmdEndRenderingKHR);
    NATIVE_PROC(CmdBeginRenderPass); NATIVE_PROC(CmdBeginRenderPass2); NATIVE_PROC(CmdBeginRenderPass2KHR);
    NATIVE_PROC(CmdEndRenderPass); NATIVE_PROC(CmdEndRenderPass2); NATIVE_PROC(CmdEndRenderPass2KHR);
    NATIVE_PROC(CmdDraw); NATIVE_PROC(CmdDrawIndexed); NATIVE_PROC(CmdDrawIndirect); NATIVE_PROC(CmdDrawIndexedIndirect);
    NATIVE_PROC(CmdDrawIndirectCount); NATIVE_PROC(CmdDrawIndirectCountKHR); NATIVE_PROC(CmdDrawIndexedIndirectCount); NATIVE_PROC(CmdDrawIndexedIndirectCountKHR);
    NATIVE_PROC(QueueSubmit); NATIVE_PROC(QueueSubmit2); NATIVE_PROC(QueueSubmit2KHR);
#undef NATIVE_PROC
    return nullptr;
}
