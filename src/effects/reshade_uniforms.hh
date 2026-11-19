#ifndef RESHADE_UNIFORMS_HPP_INCLUDED
#define RESHADE_UNIFORMS_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <functional>
#include <string>
#include <iostream>
#include <vector>
#include <chrono>
#include <memory>

#include "vulkan_include.hh"

#include "reshade/effect_module.hh"
#include "effects/params/effect_param.hh"

namespace VkHook
{
    class ReshadeUniform
    {
    public:
        void virtual update(void* mapedBuffer) = 0;
        virtual ~ReshadeUniform(){};

    protected:
        uint32_t offset;
        uint32_t size;
    };

    // One user-editable uniform, resolved to the parameter storage it copies from.
    // Built once per effect (and again when the registry replaces its parameters),
    // so the per-frame update is a loop over these with no lookups or casts.
    struct UniformBinding
    {
        uint32_t    offset = 0;
        uint32_t    bytes = 0;      // bytes copied from `src` (ignored for bools)
        const void* src = nullptr;  // the parameter's value storage
        bool        isBool = false; // `src` is a bool that is stored as a VkBool32
    };

    using UniformParamLookup = std::function<const EffectParam*(const std::string& uniformName)>;

    // Resolves every non-source, named uniform of `module`. A uniform whose
    // parameter `lookup` cannot find keeps its shader default, which is written
    // into `buffer` (`bufferSize` bytes) here. Nothing outside the buffer is touched.
    std::vector<UniformBinding> buildUniformBindings(const reshadefx::module& module,
                                                     uint32_t bufferSize,
                                                     void* buffer,
                                                     const UniformParamLookup& lookup);

    // Copies the current parameter values into the uniform buffer.
    void applyUniformBindings(const std::vector<UniformBinding>& bindings, void* buffer);

    // `zeroedSources`, when given, receives the source name of every uniform that
    // is written as zeros instead of live data.
    std::vector<std::shared_ptr<ReshadeUniform>> createReshadeUniforms(const reshadefx::module& module,
                                                                       std::vector<std::string>* zeroedSources = nullptr);

    class FrameTimeUniform : public ReshadeUniform
    {
    public:
        FrameTimeUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~FrameTimeUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;
    };

    class FrameCountUniform : public ReshadeUniform
    {
    public:
        FrameCountUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~FrameCountUniform();

    private:
        uint32_t count = 0;
        bool floatingPoint = false;
    };

    class DateUniform : public ReshadeUniform
    {
    public:
        DateUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~DateUniform();
    };

    class TimerUniform : public ReshadeUniform
    {
    public:
        TimerUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~TimerUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> start;
    };

    class PingPongUniform : public ReshadeUniform
    {
    public:
        PingPongUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~PingPongUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;

        float min             = 0.0f;
        float max             = 0.0f;
        float stepMin         = 0.0f;
        float stepMax         = 0.0f;
        float smoothing       = 0.0f;
        float currentValue[2] = {0.0f, 1.0f};
    };

    class RandomUniform : public ReshadeUniform
    {
    public:
        RandomUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~RandomUniform();

    private:
        int max = 0;
        int min = 0;
    };

    // Writes zeros every update. Stands in for sources the layer has no data for
    // (key, mousebutton, mousepoint, mousedelta), unknown sources and slots too
    // small for their source, so the buffer never holds stale bytes there.
    class ZeroUniform : public ReshadeUniform
    {
    public:
        ZeroUniform(const reshadefx::uniform_info& uniformInfo);
        void virtual update(void* mapedBuffer) override;
        virtual ~ZeroUniform();
    };

    class DepthUniform : public ReshadeUniform
    {
    public:
        DepthUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mapedBuffer) override;
        void setDepthAvailable(bool available) { depthAvailable = available; }
        virtual ~DepthUniform();

    private:
        bool depthAvailable = false;
    };
} // namespace VkHook

#endif // RESHADE_UNIFORMS_HPP_INCLUDED
