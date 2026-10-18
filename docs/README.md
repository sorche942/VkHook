<div align="center">

<img src="assets/icon.svg" width="128" height="128" alt="VKIntox Icon">

# vkintox

vulkan post-processing layer with advanced depth buffer resolve for linux.

[![version](https://img.shields.io/badge/version-0.2.0--rc1-blueviolet?style=flat-square)](#)
[![license](https://img.shields.io/badge/license-zlib-2e8b57?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-linux-555555?style=flat-square)](#)
[![CI](https://github.com/buwryme/VKIntox/actions/workflows/ci.yml/badge.svg?branch=main&style=flat-square)](https://github.com/buwryme/VKIntox/actions/workflows/ci.yml)

</div>

---

> [!WARNING]
> this is `v0.2.0-rc1`: a release candidate. the 0.2.0 line is feature-complete but still soaking, so expect rough edges before the final tag.

## showcase

<div align="center">
  <img src="assets/showcase_screenshot1.png" width="45%" alt="VKIntox in action">&nbsp;&nbsp;
  <img src="assets/showcase_screenshot2.png" width="45%" alt="VKIntox UI">
</div>

<div align="center">
  <img src="assets/showcase_screenshot3.png" width="90%" alt="Depth resolve modes">
</div>

## requirements

-   **gpu:** vulkan-capable hardware with recent drivers ([lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html) works for testing)
-   **remote install:** flatpak, curl, jq, unzip, python3, readelf
-   **local build:** just, clang/clang++, ccache, meson, ninja, glslangvalidator, wayland and/or x11 dev headers

## usage

### install

fetches the latest binary, config, fonts and shaders automatically.

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash
```

works in bash, zsh and fish alike. for another flatpak app, append its id:

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash -s -- com.target.app
```

the script picks the version interactively, and `... | bash -s -- uninstall` removes the layer while keeping your config.

> [!IMPORTANT]
> there are no system prebuilts. use local compilation for native installs.

from a checkout:

```bash
git clone https://github.com/buwryme/VKIntox.git
cd VKIntox
./setup sober          # or: ./setup flatpak com.app.id | ./setup system
```

Dear ImGui and the ReShade compiler are included as Git subtrees; a normal clone
contains their patched sources. Vendor maintenance is documented in
[workflows](docs/WORKFLOWS.md#vendor-subtrees).

enable per-session with `ENABLE_VKINTOX=1 your_game_command`.

### keybinds

| key | action |
| :--- | :--- |
| `home` | toggle the overlay gui |
| `end` | enable/disable every effect |
| `f10` | reload config and recompile shaders |

## features

**overlay**

-   **dear imgui:** standard dark style, default font, native tabs, buttons, sliders, menus, dialogs, and scrollbars. no Material theme patches, animated cards, or backdrop blur.
-   **readability:** automatic framebuffer-based scaling, with a persistent manual scale under Settings → Appearance.
-   **window:** move it by its title bar and resize with its edges or corner grip. the about tab reports the version compiled into the library.

**effects**

-   **reshade fx compat:** runtime compilation of `.fx` shaders to spir-v, cached per shader and extent.
-   **DLSS5 neural rendering:** optional native OpenDLSS-NR PTX backend with adjustable intensity, structure/tone, and hardware-optical-flow temporal history. Requires a compatible NVIDIA GPU, recent driver, and locally extracted model weights. Same-resolution SDR processing; [setup and limitations](WORKFLOWS.md#dlss5-neural-rendering).
-   **depth resolve:** auto-detects or manually selects the correct depth buffer to fix z-fighting in ao and dof. depth capture is on by default, so depth effects work on a game's first launch without a restart, and per-game defaults seed themselves; sober starts with inverted depth, everything else does not.
-   **effect list:** native checkboxes, drag/drop ordering, and either inline parameters or a separate tabbed parameter section.
-   **add effects:** a searchable table with a package selector, queued additions/removals, and per-effect duplicate controls that respect the max-effects limit.
-   **shader presets:** create (inherits the active preset), rename, delete behind a confirm, and import a reshade `.ini` through the desktop portal, with an in-overlay browser as the fallback.
-   **shader manager:** compile-test every discovered shader, grey the test out once the installed set is unchanged, and flag which shaders need a depth buffer.
-   **built-in shaders:** a from-scratch morphological msaa shader (search steps, edge threshold, blend strength, three refinement passes) and an exponential height fog, each shipping in its own `vkintox` package.
-   **per-app profiles:** one config per detected game.

**diagnostics**

-   frame time and gpu stats sampled from launch, plus a debug window for the effect registry and log output.

**input**

-   x11 and wayland backends, with overlay popups (dialogs, dropdowns) withholding pointer input so clicks never leak into the game.

**setup**

-   **one installer** for sober, flatpak and native targets, with an interactive version picker and an uninstall that leaves your config alone.
-   **assets:** installs effect packages under `reshade/` grouped by package and drops the built-in vkintox shaders beside them. the overlay needs no external fonts.

**under the hood**

-   raii for every c resource, `std::optional` registry lookups, a central deferred-destroy queue, atomic config writes, and six unit suites (documented in `docs/WORKFLOWS.md`).

## how it works

vkintox intercepts vulkan swapchain calls as a layer between the game and the driver.

1.  captures the render pipeline without a game restart.
2.  resolves depth buffers to prevent the artifacts common in vkbasalt.
3.  compiles reshade shaders locally and runs them in the effect chain.
4.  draws the overlay onto the swapchain image after the effects, capturing and blurring that frame first when the backdrop blur is on.

## caveats

### known issues

-   **catalog stability:** extremely poor in sober. avoid opening it.
-   **settings:** changing in-game graphics quality often causes crashes.
-   **wine:** games using dxvk/vkd3d might break, and anti-cheat could get you moderated.
-   **flatpak layer updates:** a reboot can be needed after re-installing or updating the layer for flatpak targets (remote and local builds alike). it may also apply to native system installs.

### troubleshooting

**logs and diagnostics.** logs save to `/path/to/config/VKIntox/vkintox.log`, and the `about` tab reports the embedded version. for sober debug output:

```bash
flatpak run --env=VKINTOX_LOG_LEVEL=debug org.vinegarhq.Sober
```

the sober log path is `~/.var/app/org.vinegarhq.Sober/config/VKIntox/vkintox.log`.

**gpu and rendering fixes.**

1.  update the runtimes: `flatpak update`
2.  clear stale overrides:
    ```bash
    flatpak override --user --unset-env=VK_ICD_FILENAMES \
      --unset-env=VK_LOADER_DRIVERS_DISABLE \
      --unset-env=__NV_PRIME_RENDER_OFFLOAD \
      --unset-env=__GLX_VENDOR_LIBRARY_NAME org.vinegarhq.Sober
    ```
3.  force nvidia on hybrid laptops:
    ```bash
    flatpak override --user --env=__NV_PRIME_RENDER_OFFLOAD=1 \
      --env=__GLX_VENDOR_LIBRARY_NAME=nvidia org.vinegarhq.Sober
    ```

verify active drivers with `flatpak --gl-drivers`. host and flatpak nvidia runtime versions must match.

**common fixes.**

-   **effects off at launch:** press `f10` (or your reload key) to re-enable them. effects are on by default, so a shader that failed to compile at launch is the usual cause.
-   **3d effects frozen:** press the reload keybind (`f10`) to refresh the depth buffer.
-   **reshade presets:** drop the `.ini` into `/path/to/config/VKIntox/configs/shaders/`, then reload. it appears in the shader ini dropdown.
-   **freezes:** restart sober once or twice. catalog freezes are expected.

> [!NOTE]
> wayland and x11 are both supported. on x11 (including xwayland) the overlay takes a pointer grab while the cursor is over it, and a keyboard grab only while it is consuming keys (a text field is active or a popup is open); if the compositor does not honour xwayland's keyboard-grab protocol, the game can still receive keys while you type in the overlay. games that don't route through an abstractor like SDL or GLFW may handle input in ways the overlay can't intercept, so input blocking can stop working.

## credits

thanks to **slobodaapl** (vkshade), **dadschoorse** (vkbasalt), **daaboulex** (wayland overlay), **crosire** (reshade), and **ocornut** (dear imgui).

---

> beta-stage software; not fully stable, use carefully.

<div align="center">

maintained with ♡ by [buwryme](https://github.com/buwryme)

</div>
