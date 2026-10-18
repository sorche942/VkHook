### Dear ImGui used by VKIntox

The vendored core is Dear ImGui **1.92.6 WIP** (`IMGUI_VERSION_NUM` 19258),
from the upstream docking branch at
[`396b33d0d011290f11915b6a2a2bf7737fb42dd7`](https://github.com/ocornut/imgui/tree/396b33d0d011290f11915b6a2a2bf7737fb42dd7).
Widgets, scrollbars, navigation focus indicators and window titles use upstream
rendering and interaction rather than custom theme hooks.

VKIntox retains its variable-font axis extensions in `imgui.h`,
`imgui_draw.cc` and `imstb_truetype.h`, right-button window resizing, and its
Vulkan backend changes. The runtime uses ImGui's embedded default font; setup
does not download or install interface fonts. Optional font fixtures and their
licenses remain separate from the runtime for the variable-font tests.

### license
ImGui is licensed under the MIT License, see [LICENSE.txt](LICENSE.txt). the modifications that this repository has are under that license too.
