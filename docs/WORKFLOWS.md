# workflows

GitHub Actions builds VKIntox, runs the unit tests, compiles every ReShade shader from the package list, and smoke-tests the layer under `vkcube`. CI runs on Ubuntu with lavapipe, so the shader and smoke jobs exercise the layer without a GPU.

## vendor subtrees

`src/vendor/imgui` and `src/vendor/reshade` are ordinary tracked directories,
imported with `git subtree --squash`. A normal clone or Git source archive
contains their complete patched sources; no submodule initialization is needed.

| Prefix | Source repository | Initial imported revision |
| --- | --- | --- |
| `src/vendor/imgui` | https://github.com/buwryme/imgui-material-3.git | `80b80599b864489b78b6692a39ed7709c3404b2a` |
| `src/vendor/reshade` | https://github.com/buwryme/reshade-vkintox.git | `4e296c625414adbc3ff846a363492e977a86780f` |

These are the existing VKIntox-specific forks, not direct imports of
`ocornut/imgui` or `crosire/reshade`: extensions, file names and directory
layouts differ. The migration retained the pinned versions and committed all
existing local patches separately, including removal of the Material UI and
the ReShade code-generation fixes. No dependency upgrade was performed.

### committing local patches

Edit vendor sources in place and commit them in VKIntox:

```bash
git add src/vendor/imgui
git commit -m "Fix ImGui integration"
# Or stage src/vendor/reshade for a compiler patch.
```

Prefer separate vendor and application commits so exported patches stay focused.
There is no nested repository to commit or push first.

### updating a subtree

Start with a clean working tree. Review the source changes and replace `main`
below with a specific reviewed commit or tag when pinning an update:

```bash
git subtree pull --prefix=src/vendor/imgui \
    https://github.com/buwryme/imgui-material-3.git main --squash
git subtree pull --prefix=src/vendor/reshade \
    https://github.com/buwryme/reshade-vkintox.git main --squash
```

These commands merge the update with local patches; resolve conflicts normally.
Always retain `--squash`. Review theme removals and compiler fixes before
accepting an update, then build and smoke-test both architectures. Do not point
these commands at the original upstream repositories without a separate layout
migration. URLs are used deliberately: local Git remotes are not copied by clone.
This workstation also has `vendor-imgui` and `vendor-reshade` convenience remotes.

### exporting patches

Create vendor-only branches when preparing patches for a fork or upstream:

```bash
git subtree split --prefix=src/vendor/imgui --branch=patches/imgui
git subtree split --prefix=src/vendor/reshade --branch=patches/reshade
```

Use a new branch name if that name already exists. Splitting does not change
the checked-out application branch or push anything. The exported histories
start from the corresponding vendor baseline and contain VKIntox's vendor edits.

### local migration backup

Before conversion, both dirty submodule worktrees, binary diffs, complete Git
bundles, and file/hash inventories were saved outside the checkout under
`~/.local/state/vkintox/vendor-subtrees-20261018-204334/`.
Restored vendor files matched the original contents, modes, and symlinks before
the documentation updates. The old submodule object stores remain in `.git/modules`
for recovery, but their active local configuration and `.gitmodules` were removed.


## user-local native installation

Install existing 64-bit and 32-bit builds without root or changes to `/usr`:

```bash
scripts/install-local.sh install build-gcc build-zig32
```

Supply either build to update just that architecture. With no build arguments,
the default is `build`; relative paths are resolved from the repository root.
The script detects the architecture from the ELF header, not the directory name.
It needs Bash, `jq`, `readelf`, and GNU file utilities, plus `patchelf` when a
build supplies private runtime libraries. Run as your normal user, without `sudo`.

Installed files:

| Architecture | Library under `~/.local/lib/vkintox/` | Manifest under `${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d/` |
| --- | --- | --- |
| x86-64 | `x86_64/libvkintox.so` | `VKIntox.x86_64.json` |
| i386 | `i686/libvkintox.so` | `VKIntox.i686.json` |

Both manifests retain the same Vulkan layer name and declare `library_arch`
(`64` or `32`), allowing the loader to select the matching binary. The old
single-library installation is migrated automatically, including when adding
only a 32-bit build to an existing 64-bit installation.

Optional `BUILD_DIR/runtime-libs/` files (`libXi.so.6`, `libXtst.so.6`, and
`libxkbcommon.so.0`) are copied beside the corresponding layer in `runtime-libs/`.
The installer checks their architecture and sets absolute private RUNPATHs:
Steam Runtime's dependency importer did not resolve `$ORIGIN` for these bundled
dependencies. No global `LD_LIBRARY_PATH` or system package installation is needed.
Installed files remain usable independently of the checkout. Restart applications
after an update; running processes retain their existing library mappings.

The installer does not build, download fonts/shader packs, modify user configs,
or change shell/Steam settings. Activation remains explicit:

```bash
ENABLE_VKINTOX=1 your-application
```

For native Steam:

```text
ENABLE_VKINTOX=1 %command%
```

DDDA can keep its existing mod-loader override:

```text
ENABLE_VKINTOX=1 WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Press **Home** to open/close the overlay, **End** to toggle effects, and **F10**
to reload effects. Do not export `ENABLE_VKINTOX` globally if you want
per-application activation. `DISABLE_VKINTOX=1` takes precedence.
Flatpak Steam and Sober still need the separate Flatpak installation.

Remove both architectures, their private runtime libraries, and any legacy
installation while preserving configs, shaders, fonts, and unrelated files:

```bash
scripts/install-local.sh uninstall
```

Use the same `HOME` and `XDG_DATA_HOME` for install and uninstall.

### installed diagnostic shaders

The following files were installed into
`~/.config/VKIntox/reshade/Shaders/`, an already configured shader search path:

| Effect | What it checks |
| --- | --- |
| `DisplayDepth` | Depth visualization and normals reconstructed from depth |
| `DebugSepia` | Warm sepia tint; color processing only |
| `DebugGrayscale` | Grayscale image; color processing only |
| `DebugInvert` | Negative colors; color processing only |

`ReShade.fxh` and `DisplayDepth_L10N.fxh` were installed alongside the effects.
No DDDA preset was changed and no effects were enabled automatically.
The color-only shaders are maintained in `assets/builtInShaders/`; DisplayDepth
and its headers come from `src/tests/fixtures/` (see its provenance notes).

To repeat this shader installation from the repository root:

```bash
shader_dir="${XDG_CONFIG_HOME:-$HOME/.config}/VKIntox/reshade/Shaders"
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
scenario passes through rendering and clean teardown. Both installed layer
architectures were rebuilt and updated. Restart an already-running game before
using the new shaders so it loads that correction.

### installed dh shader pack

The local checkout at
`/home/sorche/Development/modding/reshade/dh-reshade-shaders` was checked before
installation. These eleven effects were copied into
`~/.config/VKIntox/reshade/Shaders/`:

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
attachment were checked. DDDA-specific depth selection, tuning, and optional
shader modes remain separate in-game checks.

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

After restarting DDDA to load the updated layer:

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
    --layer "$PWD/build-gcc/src/libvkintox.so"
# For i686, supply --vkcube /absolute/path/to/a/32-bit/vkcube and its matching layer.
```

#### DDDA scene-transition attachment-clear crash

The Proton crash stack and native core identified `VKIntox_CmdClearAttachments`
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

Both user-local layers were reinstalled with `scripts/install-local.sh install
build-gcc build-zig32`. Restart DDDA to use them; remove `DISABLE_VKINTOX=1` if it
was added as a temporary bypass. No game presets or shader selections were changed.
DDDA's actual save-load transition still requires an in-game retry.

For the permanent GPU regression, `python3 src/tests/test_depth_clear_smoke.py
--help` includes native and Zig build/run commands. It requires an unobscured
X11/Xwayland window, uses temporary configuration, and checks real color and
captured-depth pixels before allowing the risky clear. Add `--legacy` to exercise
legacy render passes instead of dynamic rendering.

### Overlay input, readability, and scrolling

The DDDA log showed pointer grabs failing with `AlreadyGrabbed` while VKIntox
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
`~/.config/VKIntox/VKIntox.conf` means automatic; positive values select manual.
Each change scales fresh ImGui defaults, avoiding cumulative size growth.

Existing `theme.colors`, downloaded fonts, and old ImGui window records are
left untouched but no longer control the overlay. The new `VKIntox` window
starts with a readable framebuffer-bounded size, rather than inheriting the
old themed window's dimensions. Shader presets and launch activation are
unchanged. Installers no longer download or seed theme fonts.

Runtime checks use an isolated Xwayland display and real Vulkan applications.
The earlier input handoff checks established independent motion despite game
cursor warps, held-button release, one-shot wheel input, and restoration of game
capture. This verifies X11/Xwayland, DDDA's observed backend; it does not
establish native Wayland locked-pointer support or replace an in-game DDDA check.

The opt-in `x11 input handoff` regression now checks menu-session ownership,
raw motion versus game warps, held-button release, one-shot wheel delivery, and
held-key restoration. It fails against the old installed layer and passes with
the corrected layer. Run it on a disposable display: it synthesizes real input.

Both local layers were rebuilt (GCC x86_64, Zig i686) and installed with
`scripts/install-local.sh install build-gcc build-zig32`. Actual Vulkan UI checks
covered effect selection and parameters, tabs, a preset modal, wheel scrolling,
window movement/resizing, held-scrollbar stability, and manual-scale persistence.
Scaling stayed fixed during a drag and applied the selected value on release.
The installed native layer rendered in `vkcube`; the installed i686 layer
rendered at 1280×720 and completed both GPU fixture phases with clean shutdown.
Screenshots from this local run are under `build-gcc/stock-ui/`.

Restart the game to load the updated library. No `LD_PRELOAD`, global activation,
or system installation was added; shaders and presets were not changed.

### DDDA native materials and CompleteRT

CompleteRT 1.6.0.9 was installed from the user-supplied
`CompleteRT_1_6_0_9.zip` into `~/.config/VKIntox/reshade/Shaders/Complete/`.
Its blue-noise dependency is under
`~/.config/VKIntox/reshade/Textures/NiceGuy/BlueNoise-64frames128x128.png`.
The local adapter modifies `CompleteRT_Main.fxh` and `CompleteFX_Common.fxh`
and adds `VKIntox_DDDA_CompleteRT.fxh`. Original files were preserved with
the suffix `.pre-ddda-20261018-162554`; the ZIP and adjacent license remain intact.
These are private modifications: proprietary CompleteRT source is not shipped
in this repository and must not be redistributed without the author's permission.

The original, redistributable helper files are maintained in
`assets/builtInShaders/` and were installed in the root shader search directory:

```bash
shader_dir="${XDG_CONFIG_HOME:-$HOME/.config}/VKIntox/reshade/Shaders"
install -m644 assets/builtInShaders/VKIntox_DDDA.fxh \
    assets/builtInShaders/VKIntox_DDDA_Materials.fx \
    assets/builtInShaders/VKIntox_DDDA_Diagnostics.fx "$shader_dir/"
```

#### Activation and effect order

Restart DDDA with:

```text
ENABLE_VKINTOX=1 VKINTOX_DDDA_GBUFFER=1 WINEDLLOVERRIDES="dinput8=n,b" %command%
```

The new capture path is separately opt-in. No global environment, Steam launch
options, or existing game presets were changed. RenderDoc and
`ENABLE_VULKAN_RENDERDOC_CAPTURE` are not needed for normal use.
Neither `DISABLE_VKINTOX` nor `DXVK_DEBUG=markers` is required.
Current [DXVK emits SPIR-V debug names unconditionally](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_shader_ir.cpp);
builds that strip the identifying metadata fall back rather than guessing.

Use **Shaders → Rescan all**, add these effects, and arrange them in this order:

1. `dh_uber_motion.fx`
2. `VKIntox_DDDA_Materials.fx`
3. `Complete/COMPLETE_RT.fx`
4. `VKIntox_DDDA_Diagnostics.fx` — optional, last; disable for gameplay.

Disable `dh_uber_rt.fx` while comparing CompleteRT, rather than stacking two
lighting effects unintentionally. Keep CompleteRT's
`C_RT_USE_LAUNCHPAD_MOTIONS=0`: it consumes DH's shared `texMotionVectors`
directly, with `previousUV-currentUV` coordinates; no Launchpad addon is needed.
These are estimated optical-flow vectors, not native game velocity.

CompleteRT's **DDDA source status strip** is an optional small status indicator:
red means no native source; orange means the producer is absent/out of order;
green means the current producer and source are available. The separate
Diagnostics effect exposes validity, normals, specular exponent, light groups,
roughness, metallic/skin/hair confidence, and normal-agreement views.
Magenta pixels in validity/material diagnostics are uncovered/invalid native
pixels, not a reason to force the native path on. CompleteRT uses its original
depth/color-derived behavior there.

Native normals default on; estimated roughness blend defaults to 1. For a
conservative comparison, set **DDDA HEURISTIC specular F0 blend** to 0 first,
then increase it only after inspecting the confidence maps. Its installed
default is 0.5. The DDDA camera FOV override and depth-normal fallback default
to the captured vertical FOV of 55 degrees; adjust both if a camera mod changes it.

#### What is real, and what is estimated

| Output | Meaning |
| --- | --- |
| Normal XYZ | Native view-space normals, including the game's material normal maps |
| Specular exponent | Native legacy lobe exponent, decoded from G-buffer B |
| Light-group ID | Native alpha byte; a lighting group, **not** a material class |
| Perceptual roughness | Approximation `(2 / (exponent + 2))^(1/4)`, with calibration controls |
| Metallic confidence | Adjustable sharp-lobe/lit-color heuristic |
| Skin confidence | Adjustable chroma/lobe/local-contrast heuristic |
| Hair confidence | Adjustable dark/directional-texture/lobe heuristic |

Lit scene color is not albedo. Wood, leather, water, foliage, fabric and lighting
can fool the estimates. Confidence is not a probability or semantic segmentation.
Optional light-group gates are user-selected filters, not built-in skin/hair IDs.
The adapter uses these estimates for roughness and scalar Fresnel F0; it does not
add subsurface scattering, colored-metal albedo, or anisotropic hair scattering.
Screen-space RT still lacks off-screen/occluded scene data and can ghost during
disocclusion. Native normals do not remove those limitations.

The shared interface is `VKIntox_DDDA.fxh`: signed RH view normals in
`VKIntox_DDDA_NormalTex`; roughness/metallic/skin/hair in
`VKIntox_DDDA_MaterialTex`; exponent/group/depth/validity in
`VKIntox_DDDA_NativeTex`. A separate RGBA32F status texel carries exact low/high
16-bit frame words and validity, preventing disabled or out-of-order producers
from silently reusing old maps. CompleteRT converts normals to its own convention
as `(-x,+y,+z)` and does its own GGX roughness squaring.

#### Capture and compatibility checks

The engine identifies the actual DDDA producer shader, its sphere-normal and
log-exponent packing, and its BGRA8 attachment. No RenderDoc resource IDs are
hard-coded. Opaque RGBA writes and blended RGB-only writes that preserve the
light-group alpha are supported. Copies occur outside render scopes, into
owned snapshots; application layouts are restored. Missing submissions and
extent mismatches report unavailable rather than retaining old scene data.

The path requires one queue in the effect graphics family, single-sample,
single-layer attachments, and supported render scopes. Other-family submissions,
ambiguous same-family queues, suspended/resumed rendering, multiview, imageless
or multi-subpass legacy rendering and unsupported producer metadata fail closed.
Per-image fences protect descriptor replacement; affected effect command buffers
are re-recorded before submission without requiring descriptor-indexing features.

The saved DDDA frame 3038 contains 380 main G-buffer draws using 40 fragment
shader variants. All 40 pass the production classifier; the scene-color writer
is rejected. The captured device requests one queue each in families 0 and 1,
matching the queue safety requirement. Zero light-group pixels in this capture
are outside the normal encoding domain; all nonzero groups are inside it.

CompleteRT also exposed an engine uniform bug: a float `source="framecount"`
received integer bit patterns (frame 1 became `1.4013e-45`). The source now writes
numeric floats while preserving integer/unsigned counters. Regression coverage
also checks missing-frame invalidation, queue ambiguity, dynamic output state,
secondary-command-buffer ownership, and snapshot recycling.

Two additional compiler fixes were necessary for CompleteRT: scalar texel-fetch
coordinates must use signed zero components, and `out`/`inout` values must be cast
back to the caller's original l-value type (including swizzles and array elements).
Both previously generated invalid SPIR-V despite successful frontend compilation.
Small original fixtures now run through `spirv-val` when that tool is available.
The private CompleteRT copy also fixes the disabled-filter paths in spatial
passes 1 and 3 to read their input ping-pong texture, not the attachment being
cleared and written. File-backed sRGB sampling views no longer inherit unsupported
storage-image usage from the underlying UNORM image.

The GPU regression uses isolated configuration, not the installed game preset:

```bash
python3 src/tests/test_depth_clear_smoke.py --native-material \
    --binary "$PWD/build-gcc/depth-clear-smoke" \
    --layer "$PWD/build-gcc/src/libvkintox.so" --artifacts "$PWD/build-gcc"
# Add --legacy for ordinary render passes; use matching i686 paths for Proton.
```

Installed both verified libraries with
`scripts/install-local.sh install build-gcc build-zig32`. Each architecture's
Meson run passed 15 tests and skipped the opt-in X11 handoff test. GPU checks
passed on both architectures with dynamic rendering and legacy render passes:
native binding, missing-frame invalidation, source resize, RGB-only blended
writers, recovery, and shutdown. The installed i686 library also rendered the
DH Motion → DDDA Materials → CompleteRT chain; CompleteRT's own normal-debug
output matched the synthetic native normal. Separate pixel checks verified the
roughness map and missing-producer warning. These are isolated GPU fixtures and
saved-capture checks, **not a live DDDA gameplay verification**.

Vulkan validation is not globally clean: swapchain initial-layout and binary
semaphore diagnostics (`01430`, `09600`, `00067`) remain. The previous installed
library reproduced these in the ordinary depth fixture without native capture.
The final CompleteRT runs no longer reported the shader-module, sRGB usage, or
render-target sampling-layout errors fixed above. Validation logs and screenshots
are retained under `build-gcc/` and `build-zig32/`; the baseline comparison is
`build-gcc/depth-clear-qnncti22/`.

### Zig 32-bit build on this workstation

`packaging/meson-config/zig-x86.ini` targets `x86-linux-gnu.2.34` with Zig's
C++ standard library. It uses `/usr/lib32/pkgconfig` plus relocatable metadata
from locally extracted packages. This is an Arch/CachyOS multilib build, not
a self-contained binary for arbitrary distributions: X11, Wayland, GLib and
their remaining 32-bit runtime dependencies must already be available.

The Zig installation used here is:

```bash
export PATH="$HOME/Apps/zig/zig-x86_64-linux-0.18.0-dev.253+16d31ceed:$HOME/.local/bin:$PATH"
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
scripts/install-local.sh install build-zig32
```

The ReShade subtree has small local C++20/libc++ compatibility fixes (qualified
`reshadefx::module`, explicit `<cstdlib>` and `<alloca.h>` includes). Retain them
when updating that subtree. The main source also preserves full-width Vulkan
handles on i386 and handles both ELF32 REL and ELF64 RELA relocations, including
Wayland's non-PLT `GLOB_DAT` GOT entries.

### local installation record

The original GCC x86-64 installation was migrated to the architecture-specific
layout above, and `build-zig32/` was added alongside `build-gcc/`.
The i386 private dependencies are libXi 1.8.3, libXtst 1.2.5, and xkbcommon 1.13.2.
`patchelf` was installed as a user-local `uv` tool. No system packages, global
environment settings, or user shader/config files were changed.

DDDA's diagnostic log confirmed an x86 DXVK build and only 32-bit Steam layers
in its rendering chain; the old VKIntox installation loaded in 64-bit helper
processes only. Verification after adding i386:

- Steam Runtime 4's `i386-linux-gnu-check-vulkan` loaded the installed i386
  VKIntox into its Vulkan device, initialized the overlay, and reported
  `can-draw: true` on the NVIDIA GPU. Without opt-in it rendered without VKIntox.
- Native Wayland `vkcube` selected only the x86-64 copy. Its six-second
  DisplayDepth smoke run passed with depth resolve and 14 depth rebuilds.
- Nine regression suites passed on each architecture; the opt-in X11 input
  handoff suite was skipped. The previously failing Wayland hook regression
  now passes: the library used `GLOB_DAT` rather than the assumed jump slots.
- Isolated installer checks covered legacy migration, dual installation,
  individual updates, Steam dependency capture, malformed/mismatched input
  rejection, repeated uninstall, and preservation of unrelated files.

The user confirmed that DDDA loads the layer and opens its overlay. Depth-buffer
selection and the diagnostic shaders still need checking in the actual game.

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

**async writer** (`src/core/async_writer_test.cc`). Exercises the background `AsyncWriter`: jobs run FIFO, a throwing job does not poison the queue, a null job is dropped, `waitForIdle` returns on an unused writer, and concurrent submitters lose nothing. Tells you that profile and settings writes leave the present thread without being dropped or reordered.

**compile cache** (`src/effects/compile_cache_test.cc`). Exercises `CompileCache`: store/lookup round-trip, source and flag partitioning, re-store is a no-op, FIFO eviction at capacity, `clear`, hash determinism, and a threaded store/lookup. Tells you the cache that keeps a chain reload from re-parsing every ReShade effect is keyed correctly and bounded.

**depth copy state** (`src/render/depth_copy_state_test.cc`). Exercises `DepthCopyState`: publish/consume returns each copy once, the ring hands out slots in order, `detach` clears, and a threaded publish/consume never observes a torn `DepthState`. Tells you the mutex guarding the `CmdEndRenderPass` → `QueueSubmit` handoff and the copy ring actually holds — the race it covers only shows as an intermittent device loss under load.

**m3 theme** (`src/tests/m3_theme_test.cc`). Links the M3 theme and ImGui core with no renderer and exercises colour maths, token parsing, the exporter round-trip, the motion springs, and the connected button-group geometry. Tells you a colour, theme-file or motion regression is caught without a GPU, since none of it can be eyeballed in CI.

## reshade shader check

Compile every shader from the package list fetched by `setup`:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check
```

This does not need a GPU: `vkintox-shader-check` runs the embedded reshadefx compiler over each `.fx` and exits non-zero if any shader fails. It tells you the compiler integration still builds a real community shader pack, which the unit tests never touch.

To use an installed shader tree instead of downloading the listed packages, pass its `Shaders` directory:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check \
  --shader-dir "$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox/reshade/packages/Shaders"
```

Set `VKINTOX_EFFECT_PACKAGES_URL` to use a different `EffectPackages.ini` URL. The default matches `scripts/setup`.

## smoke test

Run `vkcube` under the freshly built layer for a few seconds and fail if the process dies:

```bash
src/tests/smoke_vkcube.sh                                  # 11s against the default build
src/tests/smoke_vkcube.sh build/src/libvkintox.so 6
```

It needs `vkcube` from `vulkan-tools` and a display. It writes its own layer manifest under a name nothing else uses and sets `VK_LOADER_LAYERS_ENABLE` so that copy is the one in the chain: on a machine with VKIntox already installed, the installed layer would otherwise be discovered too and could shadow the build under test, making a green run validate stale code. The script fails rather than passes if the loader never loaded the library it was pointed at, so a green result always means the layer really ran.

It seeds a config with one depth-consuming effect and asserts the effect built a render pass and that a depth attachment was seen, so a run that silently falls back to pass-through fails instead of passing vacuously.

Without a GPU, point it at a software implementation:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  src/tests/smoke_vkcube.sh
```

On a headless machine wrap it in `xvfb-run -a`, which is what CI does. Under X11 plus lavapipe the layer is deliberately pass-through, so on CI the smoke test proves the layer loads, builds a real effect and does not crash; the depth path needs a GPU and is not covered there.
