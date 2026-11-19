#ifndef OUTPUT_FORWARD_HPP_INCLUDED
#define OUTPUT_FORWARD_HPP_INCLUDED

#include <algorithm>

#include "reshade/effect_module.hh"

namespace VkHook
{
    // Whether the chain must append a TransferEffect after a ReShade effect so
    // that the effect's output image holds its result.
    //
    // A ReShade effect always seeds its output image with a copy of its input
    // before running. When none of its passes writes the back buffer
    // (outputWrites == 0) that copy is already the correct output, unless a pass
    // modifies the *input* image in place: a compute pass (its storage images
    // are the input views) or a graphics pass rendering into a texture that
    // aliases the back buffer (COLOR or DEPTH semantic). Only then does the
    // result live in the input image and need forwarding.
    //
    // Effects that write the back buffer (outputWrites > 0) never need it.
    inline bool reshadeOutputNeedsForward(const reshadefx::module& module, int outputWrites)
    {
        if (outputWrites > 0 || module.techniques.empty())
            return false;

        const auto aliasesBackBuffer = [&module](const std::string& name) {
            return std::any_of(module.textures.begin(), module.textures.end(), [&name](const reshadefx::texture_info& texture) {
                return texture.unique_name == name && (texture.semantic == "COLOR" || texture.semantic == "DEPTH");
            });
        };

        for (const reshadefx::pass_info& pass : module.techniques[0].passes)
        {
            if (!pass.cs_entry_point.empty())
                return true;
            for (const std::string& target : pass.render_target_names)
            {
                if (!target.empty() && aliasesBackBuffer(target))
                    return true;
            }
        }
        return false;
    }
} // namespace VkHook

#endif // OUTPUT_FORWARD_HPP_INCLUDED
