#ifndef SETTINGS_MANAGER_HPP_INCLUDED
#define SETTINGS_MANAGER_HPP_INCLUDED

#include "store.hh"

namespace VkHook
{
    // The layer's call sites still spell the store `settingsManager`; the
    // alias and this header go once they include store.hh and name
    // ConfigStore directly.
    using SettingsManager = ConfigStore;
    extern SettingsManager settingsManager;
} // namespace VkHook

#endif // SETTINGS_MANAGER_HPP_INCLUDED
