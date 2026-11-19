#pragma once

// ReShade preset / shader-profile import.

#include <string>

namespace VkHook
{
    class Config;
    class EffectRegistry;

    // Applies the shader profile at `path` on top of `config` (effect list,
    // technique order, per-effect parameters and macros) and records it as the
    // active shader profile. `registry` resolves the preset's effect files to
    // installed shaders. An empty path is a no-op.
    void applyShaderProfile(Config* config, EffectRegistry& registry, const std::string& path);
} // namespace VkHook
