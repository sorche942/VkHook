#include "reshade_uniforms.hh"

#include <cstring>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <random>

#include <algorithm>

#include "logger.hh"

namespace VkHook
{
    // Thread-safe RNG (avoids std::rand() which is not thread-safe)
    static thread_local std::mt19937 tlRng{std::random_device{}()};

    namespace
    {
        // Bytes each source writes at its offset. A uniform declared smaller than
        // this would be written past its slot (and possibly past the buffer).
        uint32_t sourceWriteSize(const std::string& source)
        {
            if (source == "date")
                return sizeof(float) * 4;
            if (source == "pingpong")
                return sizeof(float) * 2;
            return sizeof(float);
        }

        // Sources the layer has no data for: they read back as zeros.
        bool isZeroSource(const std::string& source)
        {
            return source == "key" || source == "mousebutton" || source == "mousepoint" || source == "mousedelta";
        }
    } // namespace

    std::vector<std::shared_ptr<ReshadeUniform>> createReshadeUniforms(const reshadefx::module& module,
                                                                       std::vector<std::string>* zeroedSources)
    {
        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;
        for (const auto& uniform : module.uniforms)
        {
            auto it = std::find_if(uniform.annotations.begin(), uniform.annotations.end(), [](const auto& a) {
                           return a.name == "source";
                       });
            if (it == uniform.annotations.end())
                continue;
            const std::string& source = it->value.string_data;

            const bool inBuffer = uniform.offset <= module.total_uniform_size &&
                                  uniform.size <= module.total_uniform_size - uniform.offset;
            if (!inBuffer)
            {
                Logger::warn("uniform '" + uniform.name + "' (source '" + source + "') lies outside the uniform buffer; ignored");
                continue;
            }

            if (isZeroSource(source) || uniform.size < sourceWriteSize(source) ||
                !(source == "frametime" || source == "framecount" || source == "date" || source == "timer" ||
                  source == "pingpong" || source == "random" || source == "bufready_depth"))
            {
                // Unknown source, or a slot too small for what the source writes.
                uniforms.push_back(std::make_shared<ZeroUniform>(uniform));
                if (zeroedSources)
                    zeroedSources->push_back(source);
                continue;
            }

            if (source == "frametime")
                uniforms.push_back(std::make_shared<FrameTimeUniform>(uniform));
            else if (source == "framecount")
                uniforms.push_back(std::make_shared<FrameCountUniform>(uniform));
            else if (source == "date")
                uniforms.push_back(std::make_shared<DateUniform>(uniform));
            else if (source == "timer")
                uniforms.push_back(std::make_shared<TimerUniform>(uniform));
            else if (source == "pingpong")
                uniforms.push_back(std::make_shared<PingPongUniform>(uniform));
            else if (source == "random")
                uniforms.push_back(std::make_shared<RandomUniform>(uniform));
            else
                uniforms.push_back(std::make_shared<DepthUniform>(uniform));
        }
        return uniforms;
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    ZeroUniform::ZeroUniform(const reshadefx::uniform_info& uniformInfo)
    {
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void ZeroUniform::update(void* mapedBuffer)
    {
        std::memset(static_cast<uint8_t*>(mapedBuffer) + offset, 0, size);
    }
    ZeroUniform::~ZeroUniform()
    {
    }

    namespace
    {
        bool hasSourceAnnotation(const reshadefx::uniform_info& uniform)
        {
            return std::any_of(uniform.annotations.begin(), uniform.annotations.end(), [](const auto& a) {
                return a.name == "source";
            });
        }

        // The shader's declared initial value, for a uniform with no parameter.
        void writeDefaultUniformValue(void* buffer, uint32_t bufferSize, const reshadefx::uniform_info& uniform)
        {
            if (!uniform.has_initializer_value)
                return;

            const uint32_t componentCount = std::min<uint32_t>(uniform.type.components(), 16u);
            if (componentCount == 0 || uniform.offset > bufferSize ||
                componentCount * sizeof(uint32_t) > bufferSize - uniform.offset)
                return;

            uint8_t* dst = static_cast<uint8_t*>(buffer) + uniform.offset;
            switch (uniform.type.base)
            {
                case reshadefx::type::t_float:
                    std::memcpy(dst, uniform.initializer_value.as_float, componentCount * sizeof(float));
                    break;
                case reshadefx::type::t_bool:
                {
                    VkBool32 values[16] = {};
                    for (uint32_t i = 0; i < componentCount; ++i)
                        values[i] = uniform.initializer_value.as_uint[i] != 0 ? VK_TRUE : VK_FALSE;
                    std::memcpy(dst, values, componentCount * sizeof(VkBool32));
                    break;
                }
                case reshadefx::type::t_int:
                    std::memcpy(dst, uniform.initializer_value.as_int, componentCount * sizeof(int32_t));
                    break;
                case reshadefx::type::t_uint:
                    std::memcpy(dst, uniform.initializer_value.as_uint, componentCount * sizeof(uint32_t));
                    break;
                default:
                    break;
            }
        }

        bool bindParam(const reshadefx::uniform_info& uniform, const EffectParam& param, UniformBinding& binding)
        {
            const uint32_t uniformComponents = std::min<uint32_t>(uniform.type.components(), 4u);
            binding.offset = uniform.offset;
            binding.isBool = false;
            switch (param.getType())
            {
                case ParamType::Float:
                    binding.src = &static_cast<const FloatParam&>(param).value;
                    binding.bytes = sizeof(float);
                    return true;
                case ParamType::FloatVec:
                {
                    const auto& p = static_cast<const FloatVecParam&>(param);
                    binding.src = p.value;
                    binding.bytes = std::min(p.componentCount, uniformComponents) * sizeof(float);
                    return true;
                }
                case ParamType::Int:
                    binding.src = &static_cast<const IntParam&>(param).value;
                    binding.bytes = sizeof(int32_t);
                    return true;
                case ParamType::IntVec:
                {
                    const auto& p = static_cast<const IntVecParam&>(param);
                    binding.src = p.value;
                    binding.bytes = std::min(p.componentCount, uniformComponents) * sizeof(int32_t);
                    return true;
                }
                case ParamType::Uint:
                    binding.src = &static_cast<const UintParam&>(param).value;
                    binding.bytes = sizeof(uint32_t);
                    return true;
                case ParamType::UintVec:
                {
                    const auto& p = static_cast<const UintVecParam&>(param);
                    binding.src = p.value;
                    binding.bytes = std::min(p.componentCount, uniformComponents) * sizeof(uint32_t);
                    return true;
                }
                case ParamType::Bool:
                    binding.src = &static_cast<const BoolParam&>(param).value;
                    binding.bytes = sizeof(VkBool32);
                    binding.isBool = true;
                    return true;
            }
            return false;
        }
    } // namespace

    std::vector<UniformBinding> buildUniformBindings(const reshadefx::module& module,
                                                     uint32_t bufferSize,
                                                     void* buffer,
                                                     const UniformParamLookup& lookup)
    {
        std::vector<UniformBinding> bindings;
        for (const auto& uniform : module.uniforms)
        {
            if (uniform.name.empty() || hasSourceAnnotation(uniform))
                continue;

            const EffectParam* param = lookup(uniform.name);
            UniformBinding binding;
            if (param == nullptr || !bindParam(uniform, *param, binding) || binding.bytes == 0 ||
                binding.offset > bufferSize || binding.bytes > bufferSize - binding.offset)
            {
                writeDefaultUniformValue(buffer, bufferSize, uniform);
                continue;
            }
            bindings.push_back(binding);
        }
        return bindings;
    }

    void applyUniformBindings(const std::vector<UniformBinding>& bindings, void* buffer)
    {
        uint8_t* base = static_cast<uint8_t*>(buffer);
        for (const UniformBinding& binding : bindings)
        {
            if (binding.isBool)
            {
                const VkBool32 value = *static_cast<const bool*>(binding.src) ? VK_TRUE : VK_FALSE;
                std::memcpy(base + binding.offset, &value, sizeof(value));
            }
            else
            {
                std::memcpy(base + binding.offset, binding.src, binding.bytes);
            }
        }
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    FrameTimeUniform::FrameTimeUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "frametime")
        {
            Logger::err("Tried to create a FrameTimeUniform from a non frametime uniform_info");
        }
        lastFrame = std::chrono::high_resolution_clock::now();
        offset    = uniformInfo.offset;
        size      = uniformInfo.size;
    }
    void FrameTimeUniform::update(void* mapedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - lastFrame;
        lastFrame                                             = currentFrame;
        float frametime                                       = duration.count();
        std::memcpy((uint8_t*) mapedBuffer + offset, &(frametime), sizeof(float));
    }
    FrameTimeUniform::~FrameTimeUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    FrameCountUniform::FrameCountUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "framecount")
        {
            Logger::err("Tried to create a FrameCountUniform from a non framecount uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
        floatingPoint = uniformInfo.type.is_floating_point();
    }
    void FrameCountUniform::update(void* mapedBuffer)
    {
        if (floatingPoint) {
            const float value = static_cast<float>(count);
            std::memcpy(static_cast<uint8_t*>(mapedBuffer) + offset, &value, sizeof(value));
        } else {
            std::memcpy(static_cast<uint8_t*>(mapedBuffer) + offset, &count, sizeof(count));
        }
        count++;
    }
    FrameCountUniform::~FrameCountUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    DateUniform::DateUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "date")
        {
            Logger::err("Tried to create a DateUniform from a non date uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void DateUniform::update(void* mapedBuffer)
    {
        // Date only changes once per second — cache the result and only
        // recompute when the second changes. Saves ~234 localtime_r calls/sec
        // at 235 FPS.
        static thread_local std::time_t lastSecond = 0;
        static thread_local float cachedDate[4] = {};

        auto        now  = std::chrono::system_clock::now();
        std::time_t nowC = std::chrono::system_clock::to_time_t(now);

        if (nowC != lastSecond)
        {
            lastSecond = nowC;
            struct tm currentTimeBuf;
            localtime_r(&nowC, &currentTimeBuf);
            cachedDate[0] = 1900.0f + static_cast<float>(currentTimeBuf.tm_year);
            cachedDate[1] = 1.0f + static_cast<float>(currentTimeBuf.tm_mon);
            cachedDate[2] = static_cast<float>(currentTimeBuf.tm_mday);
            cachedDate[3] = static_cast<float>((currentTimeBuf.tm_hour * 60 + currentTimeBuf.tm_min) * 60 + currentTimeBuf.tm_sec);
        }

        std::memcpy((uint8_t*) mapedBuffer + offset, cachedDate, sizeof(float) * 4);
    }
    DateUniform::~DateUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    TimerUniform::TimerUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "timer")
        {
            Logger::err("Tried to create a TimerUniform from a non timer uniform_info");
        }
        start  = std::chrono::high_resolution_clock::now();
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void TimerUniform::update(void* mapedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - start;
        float                                    timer        = duration.count();
        std::memcpy((uint8_t*) mapedBuffer + offset, &(timer), sizeof(float));
    }
    TimerUniform::~TimerUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    PingPongUniform::PingPongUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "pingpong")
        {
            Logger::err("Tried to create a PingPongUniform from a non pingpong uniform_info");
        }
        if (auto minAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "min"; });
            minAnnotation != uniformInfo.annotations.end())
        {
            min = minAnnotation->type.is_floating_point() ? minAnnotation->value.as_float[0] : static_cast<float>(minAnnotation->value.as_int[0]);
        }
        if (auto maxAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "max"; });
            maxAnnotation != uniformInfo.annotations.end())
        {
            max = maxAnnotation->type.is_floating_point() ? maxAnnotation->value.as_float[0] : static_cast<float>(maxAnnotation->value.as_int[0]);
        }
        if (auto smoothingAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "smoothing"; });
            smoothingAnnotation != uniformInfo.annotations.end())
        {
            smoothing = smoothingAnnotation->type.is_floating_point() ? smoothingAnnotation->value.as_float[0]
                                                                      : static_cast<float>(smoothingAnnotation->value.as_int[0]);
        }
        if (auto stepAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "step"; });
            stepAnnotation != uniformInfo.annotations.end())
        {
            stepMin =
                stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[0] : static_cast<float>(stepAnnotation->value.as_int[0]);
            stepMax =
                stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[1] : static_cast<float>(stepAnnotation->value.as_int[1]);
        }

        lastFrame = std::chrono::high_resolution_clock::now();

        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void PingPongUniform::update(void* mapedBuffer)
    {
        auto currentFrame = std::chrono::high_resolution_clock::now();

        std::chrono::duration<float, std::ratio<1>> frameTime = currentFrame - lastFrame;

        float increment = stepMax == 0 ? stepMin : (stepMin + std::fmod(static_cast<float>(tlRng()), stepMax - stepMin + 1.0f));
        if (currentValue[1] >= 0)
        {
            increment = std::max(increment - std::max(0.0f, smoothing - (max - currentValue[0])), 0.05f);
            increment *= frameTime.count();

            if ((currentValue[0] += increment) >= max)
            {
                currentValue[0] = max, currentValue[1] = -1.0f;
            }
        }
        else
        {
            increment = std::max(increment - std::max(0.0f, smoothing - (currentValue[0] - min)), 0.05f);
            increment *= frameTime.count();

            if ((currentValue[0] -= increment) <= min)
            {
                currentValue[0] = min, currentValue[1] = 1.0f;
            }
        }
        std::memcpy((uint8_t*) mapedBuffer + offset, currentValue, sizeof(float) * 2);
    }
    PingPongUniform::~PingPongUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    RandomUniform::RandomUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "random")
        {
            Logger::err("Tried to create a RandomUniform from a non random uniform_info");
        }
        if (auto minAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "min"; });
            minAnnotation != uniformInfo.annotations.end())
        {
            min = minAnnotation->type.is_integral() ? minAnnotation->value.as_int[0] : static_cast<int>(minAnnotation->value.as_float[0]);
        }
        if (auto maxAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "max"; });
            maxAnnotation != uniformInfo.annotations.end())
        {
            max = maxAnnotation->type.is_integral() ? maxAnnotation->value.as_int[0] : static_cast<int>(maxAnnotation->value.as_float[0]);
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void RandomUniform::update(void* mapedBuffer)
    {
        if (min > max) std::swap(min, max);
        std::uniform_int_distribution<int32_t> dist(min, max);
        int32_t value = dist(tlRng);
        std::memcpy((uint8_t*) mapedBuffer + offset, &(value), sizeof(int32_t));
    }
    RandomUniform::~RandomUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    DepthUniform::DepthUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "bufready_depth")
        {
            Logger::err("Tried to create a DepthUniform from a non bufready_depth uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void DepthUniform::update(void* mapedBuffer)
    {
        VkBool32 hasDepth = depthAvailable ? VK_TRUE : VK_FALSE;
        std::memcpy((uint8_t*) mapedBuffer + offset, &(hasDepth), sizeof(VkBool32));
    }
    DepthUniform::~DepthUniform()
    {
    }
} // namespace VkHook
