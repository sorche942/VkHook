#include "commands.hh"

namespace VkHook
{
    namespace
    {
        template<typename P>
        void copyVector(P& target, const P& source)
        {
            for (uint32_t i = 0; i < 4; ++i)
                target.value[i] = source.value[i];
        }
    } // namespace

    bool assignParamValue(EffectParam& target, const EffectParam& source)
    {
        if (target.getType() != source.getType())
            return false;
        switch (target.getType())
        {
            case ParamType::Float:
                static_cast<FloatParam&>(target).value = static_cast<const FloatParam&>(source).value;
                return true;
            case ParamType::Int:
                static_cast<IntParam&>(target).value = static_cast<const IntParam&>(source).value;
                return true;
            case ParamType::Uint:
                static_cast<UintParam&>(target).value = static_cast<const UintParam&>(source).value;
                return true;
            case ParamType::Bool:
                static_cast<BoolParam&>(target).value = static_cast<const BoolParam&>(source).value;
                return true;
            case ParamType::FloatVec:
            {
                auto& to = static_cast<FloatVecParam&>(target);
                const auto& from = static_cast<const FloatVecParam&>(source);
                if (to.componentCount != from.componentCount)
                    return false;
                copyVector(to, from);
                return true;
            }
            case ParamType::IntVec:
            {
                auto& to = static_cast<IntVecParam&>(target);
                const auto& from = static_cast<const IntVecParam&>(source);
                if (to.componentCount != from.componentCount)
                    return false;
                copyVector(to, from);
                return true;
            }
            case ParamType::UintVec:
            {
                auto& to = static_cast<UintVecParam&>(target);
                const auto& from = static_cast<const UintVecParam&>(source);
                if (to.componentCount != from.componentCount)
                    return false;
                copyVector(to, from);
                return true;
            }
        }
        return false;
    }
} // namespace VkHook
