# VkHook

VkHook is a Vulkan layer for Linux that injects post-processing into any
Vulkan application. It sits between the application and the driver, takes the
swapchain image (and the scene depth buffer when one can be identified), runs an
effect chain on it, and draws a configuration overlay on top.

Its main purpose is running the OpenDLSS-NR neural rendering network (`DLSS5`)
on NVIDIA hardware inside existing games. ReShade `.fx` shaders are also
supported and can be chained with it.

It works with native Vulkan applications and with Direct3D games running
through DXVK or vkd3d-proton under Wine/Proton. Nothing in it is specific to any
game.

## Features

- **DLSS5 effect:** a native backend for the OpenDLSS-NR Vulkan/PTX network,
  with adjustable intensity and local tone/structure, and temporal history
  from the hardware optical-flow engine.
- **ReShade effects:** `.fx` shaders compiled to SPIR-V at runtime, with
  per-shader caching, preset import, and per-application profiles.
- **Depth capture:** finds the application's scene depth buffer and gives it to
  effects without restarting the game. You can also pick the buffer by hand.
- **Overlay:** a Dear ImGui overlay for effect order, parameters, presets and
  diagnostics. It works on X11 and Wayland, and blocks input from reaching the
  application while it is open.
- **Architectures:** x86-64 and i686, so 32-bit Proton games are supported.

## Requirements

- A Vulkan 1.3 driver.
- DLSS5 needs an NVIDIA GPU and driver that expose `VK_NV_cuda_kernel_launch`,
  `VK_KHR_cooperative_matrix`, `VK_NV_cooperative_matrix2` and
  `VK_EXT_shader_float8`. Temporal history also needs `VK_NV_optical_flow`.
- DLSS5 model weights are proprietary and are not distributed or downloaded.
  You have to extract them from your own copy of the NVIDIA DLL; see
  [docs/WORKFLOWS.md](docs/WORKFLOWS.md#dlss5-neural-rendering).

To build:

- meson ≥ 1.3, ninja, a C++20 compiler (clang or gcc)
- Vulkan headers and `vk.xml`, glslangValidator ≥ 16.6 (for DLSS5)
- X11 (`libX11`, `libXi`, `libXtst`), Wayland and `xkbcommon` development
  headers; GIO (`glib-2.0`) is optional and enables the file-chooser portal
- `spirv-val` (optional; enables the SPIR-V validity tests)
- [just](https://github.com/casey/just) (optional; used for the recipes below)

To run the GUI smoke tests headless: `vkcube` (`vulkan-tools`), `xvfb`, `xauth`
and a software Vulkan driver (lavapipe, in `mesa-vulkan-drivers`).

Dear ImGui, the ReShade FX compiler and OpenDLSS-NR are vendored under
`src/vendor/`.

## Building

```bash
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
meson test -C build
sudo meson install -C build
```

To build only the layer, without the tests:

```bash
meson compile -C build vkhook
```

Meson's `-Ddlss5=` option controls the DLSS5 backend:

- `auto` (default): built when its prerequisites are found.
- `enabled`: required; the build fails without it.
- `disabled`: left out.

Other options (`meson_options.txt`): `with_so` and `with_json` choose what to
install, `append_libdir_vkhook` installs into `<libdir>/vkhook`, and
`layer_library_path` chooses what `VkHook.json` names as the library (below).

The version number lives only in the root `VERSION` file. Meson reads it for the
project version, the library's own version string and the manifest's
`implementation_version`, and the Nix flake reads it too.

### 32-bit build

32-bit Proton games need an i686 layer installed alongside the x86-64 one:

```bash
scripts/build-i686.sh build-i686
```

It cross-compiles with Zig; see [docs/WORKFLOWS.md](docs/WORKFLOWS.md#building-the-i686-layer)
for the dependencies and what is machine-specific.

Both architectures install the same `VkHook.json`. An absolute `library_path` can
name only one of them, so for a system install that carries both configure each
build with `-Dlayer_library_path=soname`. The manifest then names the bare
`libvkhook.so`, which each process's dynamic loader resolves to its own
architecture's library (both libdirs must be on its search path, as `/usr/lib`
and `/usr/lib32` are on multilib distributions).

### Flatpak

Applications running inside Flatpak, such as Flatpak Steam or Heroic, need the
layer packaged as a Flatpak Vulkan layer extension:

```bash
just flatpak-build
```

This builds `packaging/flatpak/org.freedesktop.Platform.VulkanLayer.VkHook.json`
with `flatpak-builder` and installs it for the current user from a local
repository. The source is the repository's committed `main` branch (a `git`
source), so commit what you want packaged; build directories and logs in the
working tree are not copied. Then expose the layer to the application:

```bash
flatpak override --user --env=ENABLE_VKHOOK=1 com.valvesoftware.Steam
```

## Usage

The layer is implicit and stays off until you enable it:

```bash
ENABLE_VKHOOK=1 your-application
```

For Steam, set the launch option to:

```text
ENABLE_VKHOOK=1 %command%
```

`DISABLE_VKHOOK=1` overrides `ENABLE_VKHOOK`.

| Key   | Action                         |
| :---- | :----------------------------- |
| Home  | Open or close the overlay      |
| End   | Turn all effects on or off     |
| F10   | Reload config, recompile shaders |

Configuration, profiles, shaders and logs are stored in
`${XDG_CONFIG_HOME:-~/.config}/VkHook/`. To get verbose logs, set
`VKHOOK_LOG_LEVEL=debug`.

## Development

[AGENTS.md](AGENTS.md) is the contract for changing the code: invariants, the
module map and the rules for the formal models. Read it before editing.

Layout, briefly: `src/layer` (entry points), `src/present` (swapchain and
present), `src/depth`, `src/control` (hotkeys, commands, presets), `src/effects`,
`src/overlay`, `src/input`, `src/core` (config, logger, util). The config,
logger, util and preset-model sources build once as the `vkhook_core` static
library, which the layer and the tests link.

```bash
meson setup build --buildtype=debugoptimized
meson compile -C build
meson test -C build --print-errorlogs          # unit tests, no GPU
meson test -C build --suite threaded           # the lock-hammering tests only
```

Further checks, all described in [docs/WORKFLOWS.md](docs/WORKFLOWS.md):

- **Smoke and pixel tests** run the layer under `vkcube` and a purpose-built
  test app, headless on Xvfb or on a GPU display, with optional Khronos
  validation. The pixel test proves that what the screen shows is the frame the
  game rendered.
- **Sanitizers:** `-Db_sanitize=address,undefined` for the whole suite;
  `-Db_sanitize=thread` with `--suite threaded`.
- **i686:** `scripts/build-i686.sh`.
- **Shader corpus:** `src/tests/test_reshade_shaders.py` compiles every shader of
  the packages in `assets/EffectPackages.ini` at pinned commits.
- **Formal models:** `just formal` (`cd formal && lake build`) checks the Lean 4 +
  Veil models of the concurrency and lifetime protocols in `formal/`. It needs a
  built Veil checkout; run it before merging a change to a modeled protocol.

The `Justfile` has recipes for running a local build against `vkcube` without
installing it. Vendor subtree maintenance and DLSS5 model extraction are in
WORKFLOWS.md too.

## License and attribution

VkHook is a modified version of [VKIntox](https://github.com/buwryme/VKIntox),
which comes from [vkShade](https://github.com/slobodaapl) and
[vkBasalt](https://github.com/DadSchoorse/vkBasalt). It is distributed under the
zlib license in [LICENSE](LICENSE). This is an altered version and is not the
original software.

The vendored components keep their own licenses: Dear ImGui (MIT), the ReShade
FX compiler (BSD-3-Clause), and OpenDLSS-NR (MIT, see `src/vendor/opendlss_nr`).
