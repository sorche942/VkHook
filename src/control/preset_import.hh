#pragma once

// ReShade preset / shader-profile import.

#include <string>

namespace VkHook
{
    class Config;

    // Applies the shader profile at `path` on top of `config` (effect list,
    // technique order, per-effect parameters and macros) and records it as the
    // active shader profile. An empty path is a no-op.
    void applyShaderProfile(Config* config, const std::string& path);
} // namespace VkHook
