### Dear ImGui used by VkHook

The vendored core is Dear ImGui **1.92.6 WIP** (`IMGUI_VERSION_NUM` 19258),
from the upstream docking branch at
[`396b33d0d011290f11915b6a2a2bf7737fb42dd7`](https://github.com/ocornut/imgui/tree/396b33d0d011290f11915b6a2a2bf7737fb42dd7).
Widgets, scrollbars, navigation focus indicators and window titles use upstream
rendering and interaction rather than custom theme hooks.

VkHook retains its variable-font axis extensions in `imgui.h`,
`imgui_draw.cc` and `imstb_truetype.h`, right-button window resizing, and its
Vulkan backend changes. The runtime uses ImGui's embedded default font; setup
does not download or install interface fonts. Optional font fixtures and their
licenses remain separate from the runtime for the variable-font tests.

### repository integration

This directory is tracked as a squashed Git subtree of
[`buwryme/imgui-material-3`](https://github.com/buwryme/imgui-material-3), initially
imported at `80b80599b864489b78b6692a39ed7709c3404b2a`. That fork supplies the
file layout and extensions; local commits remove its Material theme and
restore the upstream widgets described above. This is not a direct subtree of
`ocornut/imgui`. See the repository's `docs/WORKFLOWS.md` for updates and patch
export. No submodule initialization is required.

### license
ImGui is licensed under the MIT License, see [LICENSE.txt](LICENSE.txt). the modifications that this repository has are under that license too.
