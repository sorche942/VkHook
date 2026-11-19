# workflows

GitHub Actions builds VkHook, runs the unit tests, compiles every ReShade shader from the package list, and smoke-tests the layer under `vkcube`. CI runs on Ubuntu with lavapipe, so the shader and smoke jobs exercise the layer without a GPU.

## vendor subtrees

`src/vendor/imgui` and `src/vendor/reshade` are ordinary tracked directories,
imported with `git subtree --squash`. A normal clone or Git source archive
contains their complete patched sources; no submodule initialization is needed.

| Prefix | Initial imported revision |
| --- | --- |
| `src/vendor/imgui` | `80b80599b864489b78b6692a39ed7709c3404b2a` |
| `src/vendor/reshade` | `4e296c625414adbc3ff846a363492e977a86780f` |

These are VkHook-specific forks, not direct imports of
`ocornut/imgui` or `crosire/reshade`: extensions, file names and directory
layouts differ. The migration retained the pinned versions and committed all
existing local patches separately, including removal of the Material UI and
the ReShade code-generation fixes. No dependency upgrade was performed.

### committing local patches

Edit vendor sources in place and commit them in VkHook:

```bash
git add src/vendor/imgui
git commit -m "Fix ImGui integration"
# Or stage src/vendor/reshade for a compiler patch.
```

Prefer separate vendor and application commits so exported patches stay focused.
There is no nested repository to commit or push first.

### updating a subtree

Start with a clean working tree. Review the source changes and replace `main`
below with a specific reviewed commit or tag when pinning an update. `<fork-url>`
is the repository of the corresponding vendor fork:

```bash
git subtree pull --prefix=src/vendor/imgui <imgui-fork-url> main --squash
git subtree pull --prefix=src/vendor/reshade <reshade-fork-url> main --squash
```

These commands merge the update with local patches; resolve conflicts normally.
Always retain `--squash`. Review theme removals and compiler fixes before
accepting an update, then build and smoke-test both architectures. Do not point
these commands at the original upstream repositories without a separate layout
migration. URLs are used deliberately: local Git remotes are not copied by clone.

### exporting patches

Create vendor-only branches when preparing patches for a fork or upstream:

```bash
git subtree split --prefix=src/vendor/imgui --branch=patches/imgui HEAD \
    <imgui-fork-url>
git subtree split --prefix=src/vendor/reshade --branch=patches/reshade HEAD \
    <reshade-fork-url>
```

Use a new branch name if that name already exists. Splitting does not change
the checked-out application branch or push anything. The exported histories
start from the corresponding vendor baseline and contain VkHook's vendor edits.

Keep the repository URL on these commands: a fresh clone has the squashed
vendor sources but may not have the original vendor commit objects. Git can
fetch the missing baseline when exporting. Ordinary edits, commits and builds
do not need that fetch.

### migration verification

- A plain clone built the native layer from scratch without submodule setup.
  This workstation's existing local `wayland-protocols` package was supplied
  through `PKG_CONFIG_PATH`; vendor sources needed no external checkout.
- The Zig i686 build passed; all 15 Meson suites passed for each architecture,
  including the enabled X11 input handoff check on an isolated display.
- The clean-clone layer compiled DisplayDepth, resolved depth, and completed
  23 depth rebuilds during an 11-second `vkcube` smoke run without crashing.
- A transport-equivalent clone (`git clone --no-local`) exported both vendor
  histories using the commands above. Exported trees matched the corresponding
  VkHook directories and retained their original vendor baseline ancestry.


## DLSS5 neural rendering

`DLSS5` is a native effect, not a ReShade `.fx` shader. VkHook vendors the
OpenDLSS-NR Vulkan/PTX implementation in `src/vendor/opendlss_nr`, including its
license and provenance notice. It borrows the application's device through
per-device downstream dispatch; it does not create a second Vulkan device.

### Requirements and build

- NVIDIA hardware/driver exposing `VK_NV_cuda_kernel_launch`,
  `VK_KHR_cooperative_matrix`, `VK_NV_cooperative_matrix2`,
  `VK_EXT_shader_float8`, `VK_KHR_shader_clock`, `VK_NV_shader_sm_builtins`,
  and `VK_KHR_pipeline_executable_properties`, with the required feature bits.
  Vulkan 1.3 and a graphics queue supporting compute are required.
- Build with Vulkan headers containing `VkPhysicalDeviceShaderFloat8FeaturesEXT`,
  a matching `vk.xml`, and glslangValidator 16.6 or newer.
- Meson's `-Ddlss5=auto` enables the backend when the build prerequisites exist.
  Use `-Ddlss5=enabled` to require it, or `-Ddlss5=disabled` for other builds.
  Override the registry with `-Ddlss5_vulkan_registry=/path/to/vk.xml`.
- Both native and i686 builds compile the backend. The regular Meson install
  includes SPIR-V/PTX assets under `share/vkhook/dlss5`.
  The Flatpak manifest builds pinned glslang 16.6.0 (the 26.08 SDK ships 16.4.0)
  and requires `-Ddlss5=enabled`, so missing build prerequisites fail explicitly
  instead of silently omitting the backend. The compiler is build-only; the
  extension ships the compiled SPIR-V/PTX assets.
  NVIDIA driver 615.78.08 exposes CUDA kernel launch to x86-64 applications but
  not i686 applications on this workstation: i686 inference is unavailable.
  The i686 layer reports the missing extension and keeps rendering without
  DLSS5; that fallback was exercised through clean shutdown.
- Temporal history additionally requires `VK_NV_optical_flow`, its feature bit,
  supported image formats/grid size, and a usable optical-flow queue.
  VkHook prefers an unused family. If Proton/vkd3d already reserves the separate
  optical engine, VkHook shares its ordinary queue zero without changing the
  application's queue-create infos. Layer submissions and application queue
  submits, waits, and debug labels use a shared host mutex; GPU work still uses
  semaphore dependencies. Protected or mixed graphics/compute queues are not
  borrowed. Without a usable queue/feature, DLSS5 logs the reason and runs without
  history; an enabled Temporal checkbox expresses a preference, not availability.

### Extract the model

Weights are proprietary, are not bundled, and are never downloaded by VkHook.
Supply your own `nvngx_dlssnr_2026_10_16.zip` or its DLL:

```bash
python3 scripts/extract-dlss5-weights.py \
  /path/to/nvngx_dlssnr_2026_10_16.zip \
  "${XDG_CONFIG_HOME:-$HOME/.config}/VkHook/models/dlss5"
```

The output directory must not exist. DLL 310.9.16.0 carries two networks of the
same family, and `--network` selects one:

| `--network` | Resource | Blocks | Tensors | Packed bytes |
| --- | --- | ---: | ---: | ---: |
| `small` (default) | `WEIGHTS_CC_SMALL_FP8`, the DLL's default preset | 31 | 77 | 113,887,490 |
| `original` | `WEIGHTS_HT`, the 310.8.0 model (preset 1) | 71 | 153 | 147,683,778 |

Both use the same layer types, weight layouts and kernels; the small network
has two blocks in every encoder/decoder stage instead of four to eight (the eight
ViT blocks are unchanged) and is about 1.8× faster. The extractor validates the
DLL and resource hashes, checks every tensor's shape and storage, then publishes
`manifest.json` and 11 stage files atomically. The runtime identifies the network
by its block count, logs it with the activation memory placed for the current
resolution (`DLSS5: loaded the small network (31 blocks, 478 MiB of activations)`)
and verifies each stage's hash before uploading. To switch, extract the other network
into its own directory and point `dlss5ModelPath` or `VKHOOK_DLSS5_MODEL` at it.
Other DLL versions are rejected rather than guessed.

### Enable

Select **DLSS5** in Add Effects, or add it to the active config:

```ini
enableOnLaunch = true
depthCapture = on
effects = DLSS5
DLSS5 = DLSS5
DLSS5.intensity = 1
DLSS5.localTone = 1
DLSS5.localStructure = 1
DLSS5.skinStructure = -1
DLSS5.autoMask = true
DLSS5.style = 0
DLSS5.seed = 0
DLSS5.temporal = true
DLSS5.historyStrength = 1
```

The parameter UI exposes these controls. Intensity ranges from 0 (byte-exact
passthrough) to 1; local tone and structure range from 0 to 1, and skin structure
from -1 to 1.

| Conditioning control | Default | Behavior |
| --- | --- | --- |
| Auto-mask (`autoMask`) | On | Uses the bundled OpenDLSS-NR auto-mask conditioning mode. Off uses global Local structure and ignores Skin structure. This is not an explicit segmentation mask or a guarantee of face/HUD detection. |
| Style (experimental) (`style`) | 0 | Passed to the model as `style / 128`. VkHook bounds it to 0–128, an exploratory range rather than a validated visual scale. Zero preserves the previous conditioning input. |
| Noise seed (`seed`) | 0 | Fixed unsigned 32-bit seed for Gaussian model input. The slider covers 0–65535; right-click **Type in manually...** accepts the full 0–4294967295 range. It does not randomize each frame or select a quality level. |

These defaults preserve the previous model inputs. Controls persist in shader
profiles under `[DLSS5]` (or the individual instance's section); changing
Auto-mask, Style, or Noise seed invalidates temporal history on the next
submission. They do not reduce neural-network work or replace native DLSS SR.

Temporal enables hardware optical-flow history; history strength ranges from 0
(no reuse) to 1 (full confidence-weighted reuse). Both default to enabled/full
strength. Parameters update without rebuilding the network; toggling temporal
re-records the affected command buffers.
Keep the normal `depthCapture = on` setting: the layer's existing depth-transition
submission gate is also used for native effects.

The default model directory is shown in the extraction command. Override it
with `dlss5ModelPath` in the active config or `VKHOOK_DLSS5_MODEL`.
Uninstalled builds also need the compiled asset directories:

```bash
VKHOOK_DLSS5_MODEL="$PWD/build-gcc/dlss5-model" \
VKHOOK_DLSS5_SHADER_DIR="$PWD/build-gcc/src/vendor/opendlss_nr" \
VKHOOK_DLSS5_PTX_DIR="$PWD/build-gcc/src/vendor/opendlss_nr" \
ENABLE_VKHOOK=1 your-application
```

Equivalent config keys are `dlss5ShaderPath` and `dlss5PtxPath`; explicit config
values override environment defaults. Installed builds resolve the layer library's
symlinks, check adjacent assets, then locate assets using Meson's libdir/datadir
relationship relative to that library. The configured absolute installation paths
are the final fallback. Building this relocation support requires Meson 1.3 or newer.
Restart the application after changing model or kernel paths. Missing assets or
unsupported hardware/formats produce an effect error and preserve the ordinary
transfer path.

### Heroic Flatpak

Heroic uses the 26.08 Vulkan layer extension
(`org.freedesktop.Platform.VulkanLayer.VkHook`) with `ENABLE_VKHOOK=1`. A native
host installation is separate and does not update this extension.

Place your already-extracted model directory, containing `manifest.json` and
the stage files, at:

```text
~/.var/app/com.heroicgameslauncher.hgl/config/VkHook/models/dlss5/
```

Alternatively, use `VKHOOK_DLSS5_MODEL` with a path the sandbox can read;
setting an environment variable alone does not grant filesystem access.
No shader/PTX path overrides are needed for the installed extension.
Fully quit Heroic (including its tray process) and the game, then relaunch.
The log is
`~/.var/app/com.heroicgameslauncher.hgl/config/VkHook/vkhook.log`.

“Built without DLSS5 support” indicates an old/backend-disabled layer binary,
not missing weights. A supported build with missing weights instead reports
the missing model.

Proton/UMU adds a pressure-vessel container inside Heroic's Flatpak. Its imported
layer symlink can target
`/run/parent/usr/lib/extensions/vulkan/VkHook/lib/libvkhook.so`, while the
compiled `/usr/lib/extensions/...` asset paths are absent. Library-relative
lookup handles this remapping without environment overrides or copied kernels.
A missing `dlss5_luma.spv` at the old absolute path can indicate an older layer
without this relocation support. The corrected extension rendered 1,800 vkcube
frames with the full 71-block model and hardware optical flow inside Heroic's
pressure-vessel runtime, then exited cleanly.

### Inspect optical flow and temporal reuse

In DLSS5's parameters, select **Debug view**, or set `DLSS5.debugView` in
the active config (default `0`). No separate ReShade effect is needed: the
debug shader reads the live optical-flow and reprojection buffers.

| Value | View | Interpretation |
| --- | --- | --- |
| 0 | Off | Normal neural rendering. |
| 1 | Optical flow | Raw current-to-previous displacement. Hue encodes direction: right red, down yellow-green, left cyan, up violet. Brightness encodes magnitude: zero is black, eight pixels is half brightness. |
| 2 | History confidence | Green intensity is accepted-history confidence; black means no accepted history. Includes the frame-wide rejection gate. |
| 3 | Reprojected history | The previous neural image warped into the current frame; rejected pixels are black. |
| 4 | Temporal blend weight | Grayscale actual blend weight: neural sigmoid × history strength × confidence. Black means no explicit history blend; white means full history weight. |

The eight-pixel top strip reports **magenta** for temporal off/unavailable,
**amber** for first/reset frames, **red** for frame rejection/no reuse, and
**green** for an accepted frame. Off/unavailable and reset frames also show
a matching checkerboard, not stale vectors. Raw flow may be meaningless on
textureless surfaces; compare it with confidence rather than assuming every
colored vector is used.

Debug views override display intensity, preserve alpha, and never enter neural
feedback. Switching views does not reset history or rebuild the network. The
blend-weight view measures the explicit temporal composite, not the influence
of history conditioning inside the model. Keep DLSS5 last in the effect chain
while inspecting these views so later effects do not alter the diagnostic colors.

### Scope and verification

This is **same-resolution neural rendering**, not super-resolution or frame
generation. Only RGBA8/BGRA8 UNORM/SRGB swapchains in the SRGB_NONLINEAR color
space are accepted; HDR is rejected. Alpha is preserved.

VkHook has no engine motion-vector/reprojection inputs. Instead, the temporal
path estimates current-to-previous and previous-to-current motion from the raw
game image using NVIDIA hardware optical flow. It reprojects the previous neural
result with bounded Catmull-Rom filtering, rejecting out-of-bounds pixels,
unreliable flow, inconsistent bidirectional motion, and photometric mismatches.
A frame-wide consistency check rejects scene cuts without discarding a valid
moving object merely because its background is textureless. The model's blend
head, per-pixel confidence, and history strength control reuse.

History resets on effect recreation/resize, parameter changes, skipped effect
frames, and submission gaps of 250 ms or more. First/reset frames and rejected
pixels use OpenDLSS-NR's current-color no-history proxy. Disabling temporal
bypasses optical flow, reprojection, and history reads/writes entirely.
The normal path uses GPU semaphore/barrier synchronization, not CPU waits or
readback, but adds two optical-flow passes plus conversion/reprojection work.

Estimated flow is not engine velocity: disocclusions, transparency, lighting
changes, HUD, and text remain imperfect inputs. HUD and text are processed as
part of the final image; visual quality and temporal stability are not guaranteed.

Both networks were compared with NVIDIA's own DLL output from DLSSNR-AMD's
published NGX captures (`docs/ngx-verification`, MIT): one Tomb Raider frame
at three sizes, evaluated by `nvngx_dlssnr` on an RTX 5090 with the default
controls and Reset. `vkhook-dlss5-bench --image` ran the same frame here;
`original` is compared with the 310.8.0 captures (310.9.16's preset 1 gives
byte-identical output), `small` with the 310.9.16 default preset. "Edit" is the
output minus the input:

| Network | Size | PSNR vs NVIDIA | Pixels within one 8-bit step | Edit correlation |
| --- | --- | ---: | ---: | ---: |
| small | 1920×1080 | 49.43 dB | 87.2% | 0.9960 |
| small | 2560×1440 | 49.39 dB | 87.0% | 0.9962 |
| small | 3840×2160 | 50.82 dB | 92.8% | 0.9958 |
| original | 1920×1080 | 47.36 dB | 77.2% | 0.9959 |
| original | 2560×1440 | 48.34 dB | 82.1% | 0.9957 |
| original | 3840×2160 | 49.17 dB | 86.6% | 0.9955 |

NVIDIA's two networks differ from each other by 34–36 dB on this frame, so the
small port tracks its own reference, not the other model. The output is not
bit-identical to NVIDIA's. The residual is the same size for both networks and
for DLSSNR-AMD's independent port (45.6–50.6 dB), so it is not specific to the
small network; its source on the input or output side of the DLL is unknown.
Inside the graph, the small network gives the same head bit for bit under the
barrier and counter-chain schedules, every `DLSS5VK_*` route switch, and the
unfused GLSL reference kernels.

On an RTX 5070 Ti, the real 71-block PTX graph produced finite output in an
X11 `vkcube` run. GPU compositing matched an independent CPU reconstruction
exactly; alpha was unchanged, and intensity zero preserved all 307,200 pixels
byte-for-byte at 640×480.

The conditional Meson test `DLSS5 device feature preservation` covers application
feature preservation, promoted feature merging, duplicate loader nodes, input
chain immutability, optional optical-flow negotiation, private/shared optical-queue
selection (including protected/mixed-queue rejection), and unsupported-device
rejection. The opt-in hardware test runs real optical flow and bridge shaders
without proprietary weights:

```bash
VKHOOK_TEST_DLSS5_GPU=1 meson test -C build-gcc \
  'DLSS5 optical-flow temporal GPU' --print-errorlogs
```

On the RTX 5070 Ti, it recovered an 8×4-pixel translation, rejected disocclusion
and scene-cut history, retained moving-object history on a textureless background,
and checked temporal bypass, reset, alpha, zero intensity, and odd dimensions.
A full-model `vkcube` smoke also confirmed that the PTX graph consumes reprojected
history and that live temporal off/on toggles bypass and restore reuse.
Control's DX12 startup through Steam/CachyOS Proton was also exercised at
3840×2160: the layer shared the application's optical queue (family 5, queue 0
on this driver), displayed live motion vectors, and showed accepted history
with a green debug-status strip. A magenta checkerboard with Temporal enabled
means the runtime path is unavailable, not that the image has no motion;
check the `DLSS5 optical flow`/`DLSS5 temporal unavailable` startup messages.
GPU inference requires the hardware above and locally extracted weights;
lavapipe CI cannot prove it. These checks do not establish NVIDIA DLL parity.

### Network performance

`vkhook-dlss5-bench` (built with the backend) times the neural network alone on
a private Vulkan device with synthetic input features; no weights leave the model
directory and no window is needed:

```bash
B=build-gcc/src/vendor/opendlss_nr
build-gcc/src/vkhook-dlss5-bench --model build-gcc/dlss5-model-small \
  --shaders "$B" --ptx "$B" --width 3840 --height 2160 --frames 40
```

It records the graph once, resubmits it, and prints the network, its pooled
activation memory (see "Video memory" below), and the minimum and median GPU
time. `--profile` re-records every frame with per-launch timestamps
and lists the minimum span per kernel and per block. `--chain 0|1` selects Vulkan
barriers (the effect's schedule) or upstream's spinning counter chains.
`--dump FILE` writes the network head as f32, and `--compare FILE` reports how
many values differ from such a dump, so a PTX or scheduling change can be checked
for bit-exact output against a previous build's `--ptx` directory.

`--image FILE` replaces the synthetic features with a raw RGBA8 frame of
`--width`×`--height` (`magick in.png -depth 8 rgba:in.rgba`), preprocessed with
the default controls and noise `--seed` (0, as after a Reset). The output is
composed as the effect composes a frame without history at intensity 1;
`--output FILE` writes it as raw RGBA8 and `--expect FILE` reports PSNR,
identical pixels and pixels within one 8-bit step against another raw RGBA8
frame, such as NVIDIA's output in the comparison above.

The vendored PTX generators are tuned for the RTX 5070 Ti (Blackwell, 70 SMs);
every change only moves data or synchronization, so the head is bit-identical
to the untuned graph:

- Release fences before chain signals run only when chaining is on.
- The window-attention (`qkv_e4m3`) grid is a multiple of the head count, so a
  workgroup keeps one head: its prior rows load once, and for K ≤ 128 its weights
  stay in shared memory; the kernel runs at `.maxnreg 88`.
- The fused FFN reads its residual seeds from shared tiles instead of narrow
  global loads, and C = 128 runs at `.maxnreg 64` (two workgroups per SM).

Best median of 40 frames over three interleaved rounds, barrier schedule, the
untuned and tuned PTX on the same build:

| Resolution | Original, untuned | Original, tuned | Small, untuned | Small, tuned | Small tuned vs original untuned |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920×1080 | 7.32 ms | 6.51 ms | 3.82 ms | 3.58 ms | 2.05× |
| 2560×1440 | 11.21 ms | 10.29 ms | 5.85 ms | 5.45 ms | 2.06× |
| 3840×2160 | 24.69 ms | 22.40 ms | 13.53 ms | 12.70 ms | 1.94× |

The tuning gains less on the small network (−6 to −7% against −8 to −11%)
because the kernels it does not touch weigh more there: at 1080p the two
full-resolution 32-channel blocks take 20% of the small network's time and the
unchanged ViT 29%.

The PTX still targets `sm_89`, so Ada GPUs run it, but the register caps were
chosen on Blackwell; re-measure them with this tool before tuning for another GPU.
Absolute times on this GPU vary by up to ~10% between processes, so compare
builds with interleaved runs.

### Video memory

At 3840×2160 the small network's 82 tensors, input features included, took
2.9 GiB as one allocation each; 980 MiB of that was allocated but never used by
the default PTX routes. The graph constructor now records the network once into
a command buffer that is never submitted, learns which barrier-separated launches
use each tensor, and binds tensors whose launch intervals never overlap to the
same range of one allocation (64 KiB aligned, largest first). Unused tensors get
no memory. Every later recording is checked against that plan and throws before
recording a tensor outside its placed interval. The head stays valid after the
recording; the caller's 16-channel F32 input features (510 MiB at 4K) keep their
own memory. Only the debug capture options keep one allocation per tensor.

Peak process memory of `vkhook-dlss5-bench` (weights, network and input
features; no swapchain, history or optical-flow images), RTX 5070 Ti, 30 frames:

| Network | Resolution | Pooled activations | Before | After |
| --- | --- | ---: | ---: | ---: |
| small | 1920×1080 | 126 MiB | 1192 MiB | 669 MiB |
| small | 2560×1440 | 215 MiB | 1754 MiB | 862 MiB |
| small | 3840×2160 | 478 MiB | 3418 MiB | 1443 MiB |
| original | 1920×1080 | 126 MiB | 1270 MiB | 745 MiB |
| original | 2560×1440 | 215 MiB | 1832 MiB | 938 MiB |
| original | 3840×2160 | 478 MiB | 3496 MiB | 1519 MiB |

With the effect and temporal history in an X11 `vkcube` at 3840×2028, peak
process memory fell from 4872 to 2980 MiB. The head stayed bit-identical
(`--compare`) at six resolutions from 1280×720 to 3840×2160, after 1 to 20
frames, with `--chain 1`, under each `DLSS5VK_*` route switch, and for both
networks. Interleaved 4K runs showed no GPU time change beyond noise.

This addresses a CONTROL (DX12, vkd3d-proton) crash at 3840×2160 on the 16 GiB
card. That game alone already exceeded VRAM: vkd3d logged system-memory
fallbacks even without VkHook. With DLSS5 adding about 3.9 GB, the session ended
after 52 minutes. The NVIDIA DRM driver could no longer allocate the compositor's
Xwayland buffers (`Failed to allocate NVKMS memory for GEM object`), and the game
exited without an exception. A 2 h 17 min session without DLSS5 had no such errors.

If an effect's auxiliary submission (DLSS5 optical flow) or the frame submission
waiting on it fails, VkHook logs `VkHook panic: …` with the `VkResult` and turns
effects off until restart. It no longer fails every later present silently.

## shader compile cache

Compiling a ReShade effect (parse and SPIR-V codegen) dominates the time it takes
to enable an effect or rebuild a chain. The layer keeps compiled effects in memory
and on disk in `${XDG_CACHE_HOME:-~/.cache}/VkHook/shaders/`, one `.vksc` file per
compiled variant. An entry is keyed by the preprocessed source (every include and
macro folded in), the macro set, the codegen flags and a compiler tag, so editing
a shader, an include or a macro, or upgrading VkHook, simply compiles again.
Entries are written by the persistence queue, never on the present thread; a
truncated or corrupt file is ignored and overwritten. The directory keeps at most
1024 entries (oldest written first). It is safe to delete at any time.

## diagnostic shaders

These effects are used as diagnostics. Install them into
`~/.config/VkHook/reshade/Shaders/`, an already configured shader search path:

| Effect | What it checks |
| --- | --- |
| `DisplayDepth` | Depth visualization and normals reconstructed from depth |
| `DebugSepia` | Warm sepia tint; color processing only |
| `DebugGrayscale` | Grayscale image; color processing only |
| `DebugInvert` | Negative colors; color processing only |

`ReShade.fxh` and `DisplayDepth_L10N.fxh` were installed alongside the effects.
No preset was changed and no effects were enabled automatically.
The color-only shaders are maintained in `assets/builtInShaders/`; DisplayDepth
and its headers come from `src/tests/fixtures/` (see its provenance notes).

To repeat this shader installation from the repository root:

```bash
shader_dir="${XDG_CONFIG_HOME:-$HOME/.config}/VkHook/reshade/Shaders"
mkdir -p "$shader_dir"
install -m644 src/tests/fixtures/DisplayDepth.fx \
    src/tests/fixtures/DisplayDepth_L10N.fxh src/tests/fixtures/ReShade.fxh \
    assets/builtInShaders/DebugSepia.fx assets/builtInShaders/DebugGrayscale.fx \
    assets/builtInShaders/DebugInvert.fx "$shader_dir/"
```

In-game, use **Shaders → Rescan all**, then **Effects → Add effects…** and search
for the names above. Enable one at a time. For DisplayDepth, choose
**Present type → Show both (Vertical 50/50)**: reconstructed normals on the left,
depth on the right. These are not the game's original normal-map/G-buffer data.
Use **End** to compare with effects disabled.

All four passed the 32-bit shader compiler and built/rendered real Vulkan effect
passes inside Steam Runtime 4. The three color effects also rendered and exited
cleanly on x86-64, and their visible `vkcube` output was checked.

This check exposed a cleanup bug for effects without uniforms: the optional
uniform-buffer handles were uninitialized and teardown could destroy garbage
handles. They now start at `VK_NULL_HANDLE`; the previously crashing Sepia
scenario passes through rendering and clean teardown. Restart an already-running game before
using the new shaders so it loads that correction.

### installed dh shader pack

The local `dh-reshade-shaders` checkout was checked before
installation. These eleven effects were copied into
`~/.config/VkHook/reshade/Shaders/`:

- `dh_Lain.fx`
- `dh_ahoh.fx`
- `dh_ambient_remove.fx`
- `dh_anime.fx`
- `dh_canvas.fx`
- `dh_pastel_bug.fx`
- `dh_sync.fx`
- `dh_undither.fx`
- `dh_uniformity_correction.fx`
- `dh_uber_motion.fx`
- `dh_uber_rt.fx`

All six supplied PNG assets were copied into the sibling `Textures/` directory.
The installed shader copies use `#include "ReShade.fxh"` instead of the upstream
`#include "Reshade.fxh"`: Linux filename matching is case-sensitive. The existing
header, source checkout, unrelated shaders, and game presets were left unchanged.
No effects were enabled automatically.

Two files remain deliberately uninstalled:

| File | Missing runtime support |
| --- | --- |
| `DH_UI_TextDetect.fx` | Independently ordered BEFORE/AFTER techniques |
| `dh_uber_mask.fx` | Independently ordered BEFORE/AFTER techniques |

The renderer still executes only `module.techniques[0]`. The two BEFORE/AFTER
workflows require independently ordered techniques and are not enabled by the
shared-texture change described below.

Verification: after the include-case correction, all thirteen files compiled
with the 32-bit checker and completed pipeline-initialization/cleanup probes in
Steam Runtime 4. The original nine standalone effects were additionally run from
their final installation paths in native `vkcube`; their visible output and depth
attachment were checked. Per-game depth selection, tuning, and optional shader
modes remain separate in-game checks.

Use **Shaders → Rescan all**, then **Effects → Add effects…** and search `dh_`.
Start with one effect at a time. **End** toggles the chain for comparison.

#### DH Uber Motion / Uber RT

Both installed layer architectures now share generated ReShade textures by
qualified name within an effect-chain build. Dimensions, format, mip count and
view type must agree; incompatible declarations report an error instead of
aliasing incompatible images. Effects retain shared ownership of image, views
and memory. A reload or another swapchain gets a fresh pool, and the last owner
hands destruction to the existing GPU-safe deferred queue.

The motion producer's `texMotionVectors` and Uber RT's sampler now refer to the
same image. Mipmap barriers include rendering and transfer writes, attachment
views cover only the base mip, and sRGB views exclude unsupported storage usage.

After restarting the game to load the updated layer:

1. **Shaders → Rescan all**, then add both effects.
2. Enable and order **`dh_uber_motion` before `dh_uber_rt`**.
3. Keep depth capture enabled and confirm scene geometry with DisplayDepth.
4. In Uber RT, use **Debug → Display → Motion** while moving the camera.
   Motion should show spatial variation, not an entirely flat gray image.
5. Return **Display** to **Output** for normal rendering.

Leave the optional `USE_MARTY_LAUNCHPAD_MOTION`, `USE_VORT_MOTION` and
`USE_LUMINITE_KERNEL` macros at their defaults for this DH-only chain. Those
alternative producers were not installed or tested. Game presets were not
modified or enabled automatically.

Verification:

- The GPU contents regression read black on the old layer and `(0,255,0)` from
  the producer on the patched layer, on both x86-64 and i686; clean exits passed.
- Real DH Motion + RT showed motion vectors in both native architectures.
  Config reload, swapchain resize and teardown were exercised; normal RT output
  was also visually checked in the Zig-built 32-bit cube.
- The installed i686 layer and installed shader pair ran in Steam Runtime 4
  with real scene depth, a shared motion image, and clean teardown.
- Ten suites passed and one opt-in input suite was skipped per architecture;
  the pool regression covers aliasing, incompatible declarations, independent
  chain scopes, consumer lifetime and partial-construction cleanup.
- Vulkan validation is **not clean**: presentation, depth-layout and stencil
  synchronization diagnostics also occur on the unpatched baseline. No new
  diagnostic categories appeared in the comparison; the mip/view violations
  addressed above disappeared. This is not a general renderer-validation fix.

The standalone GPU regression requires X11/Xwayland and an unobscured vkcube
window. It uses temporary configs and does not change installed presets:

```bash
python3 src/tests/test_shared_texture_smoke.py \
    --layer "$PWD/build-gcc/src/libvkhook.so"
# For i686, supply --vkcube /absolute/path/to/a/32-bit/vkcube and its matching layer.
```

#### Scene-transition attachment-clear crash

The Proton crash stack and native core identified `VkHook_CmdClearAttachments`
injecting a shader depth snapshot into an active application render pass.
The snapshot began and ended another render pass before forwarding the game's
clear. This invalid Vulkan sequence crashed NVIDIA's driver on DXVK's command
submission thread; changing DH shader settings would not repair it.

The attachment-clear interception has been removed. Application clears now go
directly to the next layer, and the common snapshot recorder rejects active
tracked render scopes before emitting commands. Existing end-of-scope depth
selection and layer-owned snapshot command buffers remain enabled. Out-of-pass
depth clears retain capture under the tracking lock and use the layout supplied
by the application, including transfer-stage synchronization where required.

**Limitation:** pre-clear depth inside a render pass is not preserved. If a game
clears scene depth before ending that pass, the snapshot can contain the cleared
depth. Supporting that case requires legal pass splitting and graphics-state
restoration; inserting a nested pass or transfer is not a valid substitute.

Both native and Zig i686 builds passed eleven suites with the opt-in X11 input
suite skipped. The new `depth snapshot scope` regression checks that an active
scope emits no snapshot commands, leaving the scope enables capture, independent
command buffers are unaffected, and transfer layouts are restored correctly.
The existing native GPU texture-sharing regression also passed.



The focused GPU regression reproduced SIGSEGV with both old architectures.
With the corrected layers, both dynamic rendering (GENERAL depth, shader
snapshot) and legacy render passes passed: a partial clear left the expected
red/green geometry and 0.25/0.75 captured depth, subsequent depth-tested drawing
remained correct without rebinding the pipeline, and shutdown completed cleanly.
The installed i686 layer also passed this scenario inside Steam Runtime 4.

Both layers were rebuilt and reinstalled. Restart the game to use them; remove `DISABLE_VKHOOK=1` if it
was added as a temporary bypass. No game presets or shader selections were changed.

For the permanent GPU regression, `python3 src/tests/test_depth_clear_smoke.py
--help` includes native and Zig build/run commands. It requires an unobscured
X11/Xwayland window, uses temporary configuration, and checks real color and
captured-depth pixels before allowing the risky clear. Add `--legacy` to exercise
legacy render passes instead of dynamic rendering.

### Overlay input, readability, and scrolling

A Proton game's log showed pointer grabs failing with `AlreadyGrabbed` while VkHook
incorrectly reported capture as active. The interactive menu now acquires the
keyboard first so Wine can relinquish capture, retries the pointer handoff, and
owns input until close rather than only while the pointer crosses a panel.
Successful capture uses an ImGui cursor driven by XI2 raw motion; game cursor
warps do not move it. XI2.1 is required because XI2.0 suppresses root-selected raw
motion during an active grab. Buttons and wheel use the grabbed core event stream
once, instead of combining it with duplicate raw events.

Open/close with the configured overlay key (`Home` in this installation).
While the menu captures input, clicks outside its panel do not reach the game.
Closing releases capture immediately.

The overlay now uses standard Dear ImGui dark styling and widgets. Material
theme patches, animated folds/navigation, custom menus/dialogs, icon fonts, and
the backdrop-blur render pass have been removed. The normal title bar supports
moving the window; its edges and corner grip resize it. Effects, Shaders,
Settings, and About use ordinary tabs. Picker/config/subview scroll positions
have separate identities rather than inheriting another page's scroll.

**Settings → Appearance → Automatic UI scale** defaults on. Automatic scale is
framebuffer height / 900, bounded to 1–2.5×; 3840×2160 selects 2.4×. The default
font is 18px before scaling. Uncheck Automatic UI scale for the 0.75–3× manual
slider. Scaling applies after the active interaction ends, so dragging this
slider does not move its own geometry. `uiScale = 0` in
`~/.config/VkHook/VkHook.conf` means automatic; positive values select manual.
Each change scales fresh ImGui defaults, avoiding cumulative size growth.

Existing `theme.colors`, downloaded fonts, and old ImGui window records are
left untouched but no longer control the overlay. The new `VkHook` window
starts with a readable framebuffer-bounded size, rather than inheriting the
old themed window's dimensions. Shader presets and launch activation are
unchanged. Installers no longer download or seed theme fonts.

Runtime checks use an isolated Xwayland display and real Vulkan applications.
The earlier input handoff checks established independent motion despite game
cursor warps, held-button release, one-shot wheel input, and restoration of game
capture. This verifies X11/Xwayland; it does not establish native Wayland
locked-pointer support or replace an in-game check.

The opt-in `x11 input handoff` regression now checks menu-session ownership,
raw motion versus game warps, held-button release, one-shot wheel delivery, and
held-key restoration. It fails against the old installed layer and passes with
the corrected layer. Run it on a disposable display: it synthesizes real input.

Both local layers were rebuilt (GCC x86_64, Zig i686). Actual Vulkan UI checks
covered effect selection and parameters, tabs, a preset modal, wheel scrolling,
window movement/resizing, held-scrollbar stability, and manual-scale persistence.
Scaling stayed fixed during a drag and applied the selected value on release.
The installed native layer rendered in `vkcube`; the installed i686 layer
rendered at 1280×720 and completed both GPU fixture phases with clean shutdown.
Screenshots from this local run are under `build-gcc/stock-ui/`.

Restart the game to load the updated library. No `LD_PRELOAD` or global activation
was added; shaders and presets were not changed.

### CompleteRT compatibility fixes

CompleteRT exposed an engine uniform bug: a float `source="framecount"`
received integer bit patterns (frame 1 became `1.4013e-45`). The source now writes
numeric floats while preserving integer/unsigned counters.

Two compiler fixes were also necessary: scalar texel-fetch coordinates must use
signed zero components, and `out`/`inout` values must be cast back to the caller's
original l-value type (including swizzles and array elements). Both previously
generated invalid SPIR-V despite successful frontend compilation. Small original
fixtures now run through `spirv-val` when that tool is available. File-backed sRGB
sampling views no longer inherit unsupported storage-image usage from the
underlying UNORM image.

### Zig 32-bit build on this workstation

`packaging/meson-config/zig-x86.ini` targets `x86-linux-gnu.2.34` with Zig's
C++ standard library. It uses `/usr/lib32/pkgconfig` plus relocatable metadata
from locally extracted packages. This is an Arch/CachyOS multilib build, not
a self-contained binary for arbitrary distributions: X11, Wayland, GLib and
their remaining 32-bit runtime dependencies must already be available.

Put a Zig toolchain and `patchelf` on `PATH`:

```bash
export PATH="/path/to/zig:$HOME/.local/bin:$PATH"
uv tool install patchelf
```

The missing three multilib packages were downloaded and extracted locally, not
installed system-wide. From the repository root, recreate that dependency tree:

```bash
mkdir -p build-deps32
for url in $(pacman -Sp --print-format '%l' lib32-libxi lib32-libxtst lib32-libxkbcommon); do
    package="build-deps32/${url##*/}"
    curl --fail --location --output "$package" "$url"
    bsdtar -xf "$package" -C build-deps32 usr
done
```

`wayland-protocols` was also missing. The cross file accepts either the system
package or the previously extracted copy at `build/deps/wayland-protocols`.
To recreate the local copy:

```bash
mkdir -p build/deps/wayland-protocols
url=$(pacman -Sp --print-format '%l' wayland-protocols)
curl --fail --location --output build/deps/wayland-protocols.pkg.tar.zst "$url"
bsdtar -xf build/deps/wayland-protocols.pkg.tar.zst -C build/deps/wayland-protocols usr
```

Configure once and compile (subsequent rebuilds need only the compile command):

```bash
uv tool run --with ninja meson setup build-zig32 \
    --cross-file packaging/meson-config/zig-x86.ini --buildtype=release --prefix=/usr
uv tool run --with ninja meson compile -C build-zig32 -j 4
```

Stage the private dependencies for installation and for running build-tree tests:

```bash
mkdir -p build-zig32/runtime-libs
for name in libXi.so.6 libXtst.so.6 libxkbcommon.so.0; do
    install -m755 "build-deps32/usr/lib32/$name" "build-zig32/runtime-libs/$name"
    patchelf --set-rpath '$ORIGIN' "build-zig32/runtime-libs/$name"
done
ln -sfn ../runtime-libs build-zig32/src/runtime-libs
uv tool run --with ninja meson test -C build-zig32 --no-rebuild --print-errorlogs
```

The ReShade subtree has small local C++20/libc++ compatibility fixes (qualified
`reshadefx::module`, explicit `<cstdlib>` and `<alloca.h>` includes). Retain them
when updating that subtree. The main source also preserves full-width Vulkan
handles on i386 and handles both ELF32 REL and ELF64 RELA relocations, including
Wayland's non-PLT `GLOB_DAT` GOT entries.


## local checks

Build the layer and every test executable:

```bash
meson setup build
meson compile -C build
```

Run the test suites:

```bash
meson test -C build --print-errorlogs
```

Each test is a small `main()` that prints `N checks, M failures` and exits non-zero on failure, so `meson test` reports pass/fail per suite.

## what the tests cover

**config serializer** (`src/tests/config_serializer_test.cc`). Exercises `ConfigSerializer`: profile save/load round-trips, the legacy `.conf` to per-game migration, settings persistence, imported ReShade presets, disabled-effect sidecars, and the rejection of path separators in config/profile names. Tells you that writing a profile and reading it back gives the same effects, params and macros — the file format the whole UI depends on.

**vk handle ordering** (`src/render/vk_handle_test.cc`). Exercises `DeferredDestroyQueue`: releases run in ascending phase order, within a phase in reverse registration order, `flush` drains exactly once, and `discard` drops without running. Tells you that a view is destroyed before the image it was made from and device memory is freed last — the order that keeps a driver from faulting during teardown, which a headless test cannot observe through a real GPU.

**persistence queue** (`src/tests/persist_test.cc`). Exercises `PersistenceQueue`, the one writer for every user-file mutation: jobs run FIFO, repeated writes to one key coalesce to the newest, a throwing job does not poison the queue, shutdown drains and later submits resolve `false`, and a queued autosave can never resurrect a deleted preset. Also checks the `SettingsManager` snapshot and queued save. Tells you that profile and settings writes leave the present thread without being dropped, reordered or undone.

**field parse** (`src/tests/field_parse_test.cc`). Pins the `std::from_chars` grammar of the overlay numeric field editors: exact component count, no partial application, no negatives for unsigned, no trailing junk.

**compile cache** (`src/effects/compile_cache_test.cc`). Exercises `CompileCache`: store/lookup round-trip, source and flag partitioning, re-store is a no-op, FIFO eviction at capacity, `clear`, hash determinism, and a threaded store/lookup. Tells you the cache that keeps a chain reload from re-parsing every ReShade effect is keyed correctly and bounded.

**depth copy state** (`src/render/depth_copy_state_test.cc`). Exercises `DepthCopyState`: publish/consume returns each copy once, the ring hands out slots in order, `detach` clears, and a threaded publish/consume never observes a torn `DepthState`. Tells you the mutex guarding the `CmdEndRenderPass` → `QueueSubmit` handoff and the copy ring actually holds — the race it covers only shows as an intermittent device loss under load.

**m3 theme** (`src/tests/m3_theme_test.cc`). Links the M3 theme and ImGui core with no renderer and exercises colour maths, token parsing, the exporter round-trip, the motion springs, and the connected button-group geometry. Tells you a colour, theme-file or motion regression is caught without a GPU, since none of it can be eyeballed in CI.

## reshade shader check

Compile every shader from the package list:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkhook-shader-check
```

This does not need a GPU: `vkhook-shader-check` runs the embedded reshadefx compiler over each `.fx` and exits non-zero if any shader fails. It tells you the compiler integration still builds a real community shader pack, which the unit tests never touch.

To use an installed shader tree instead of downloading the listed packages, pass its `Shaders` directory:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkhook-shader-check \
  --shader-dir /path/to/Shaders
```

Set `VKHOOK_EFFECT_PACKAGES_URL` to use a different `EffectPackages.ini` URL.

## smoke test

Run `vkcube` under the freshly built layer for a few seconds and fail if the process dies:

```bash
src/tests/smoke_vkcube.sh                                  # 11s against the default build
src/tests/smoke_vkcube.sh build/src/libvkhook.so 6
```

It needs `vkcube` from `vulkan-tools` and a display. It writes its own layer manifest under a name nothing else uses and sets `VK_LOADER_LAYERS_ENABLE` so that copy is the one in the chain: on a machine with VkHook already installed, the installed layer would otherwise be discovered too and could shadow the build under test, making a green run validate stale code. The script fails rather than passes if the loader never loaded the library it was pointed at, so a green result always means the layer really ran.

It seeds a config with one depth-consuming effect and asserts the effect built a render pass and that a depth attachment was seen, so a run that silently falls back to pass-through fails instead of passing vacuously.

Without a GPU, point it at a software implementation:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  src/tests/smoke_vkcube.sh
```

On a headless machine wrap it in `xvfb-run -a`, which is what CI does. Under X11 plus lavapipe the layer is deliberately pass-through, so on CI the smoke test proves the layer loads, builds a real effect and does not crash; the depth path needs a GPU and is not covered there.
