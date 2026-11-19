# AGENTS.md — VkHook

VkHook is an implicit Vulkan layer for Linux. It runs ReShade `.fx` effects and the DLSS5
(OpenDLSS-NR) network on the swapchain, draws an ImGui overlay, and interposes X11/Wayland
input. It must survive arbitrary applications: native Vulkan, DXVK and vkd3d-proton under
Proton, both x86-64 and i686, and multithreaded command recording. Everything the layer does runs
inside someone else's process, on their threads, against their queues.

This file is the contract for anyone (human or agent) changing the code. Read it fully before
editing.

---

## 1. Build, test, verify

```bash
# configure (one build dir per checkout/worktree; build*/ is gitignored)
CC=clang CXX=clang++ meson setup build --buildtype=debugoptimized
meson compile -C build
meson test -C build --print-errorlogs
```

- Configure needs the `wayland-protocols` package (installed system-wide).
- Agents should run GUI tests headless, e.g. `xvfb-run -a src/tests/smoke_vkcube.sh …`, so that
  no windows appear on the user's desktop.
- **Validation build:** run the layer with `VK_LOADER_LAYERS_ENABLE=*validation*,*VKHOOK*`
  (the Khronos validation layer is installed). New code must not add validation errors. Turn on
  synchronization validation with `VK_LAYER_ENABLES=VALIDATION_CHECK_ENABLE_SYNCHRONIZATION_VALIDATION`.
- **Smoke test (needs a display; GPU or lavapipe):**
  `src/tests/smoke_vkcube.sh build/src/libvkhook.so 11`. To run on lavapipe, add
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`.
- **Shader corpus:** `python3 src/tests/test_reshade_shaders.py --shader-check build/src/vkhook-shader-check`.
- **Depth regression (GPU, X11):** see the header of `src/tests/test_depth_clear_smoke.py`.
- **Formal models:** `just formal`, which is `cd formal && lake build`. See §6.
- **32-bit:** cross file `packaging/meson-config/zig-x86.ini`; see `docs/WORKFLOWS.md`.

What "done" means for a change:
1. The build is clean, with no new warnings.
2. `meson test` passes.
3. If the change is on the Vulkan path, the vkcube smoke test passes on the GPU.
4. If it touches a modeled protocol, `just formal` is green.

Report only checks you actually ran.

---

## 2. Repository map

| Path | Contents |
|---|---|
| `src/layer/` | Entry points (GIPA/GDPA, intercept table), instance/device create, shared layer state |
| `src/present/` | Swapchain create/destroy, effect chain build, `QueuePresentKHR` steps, queue submit hooks |
| `src/depth/` | Depth-tracking `vkCmd*`/create/destroy hooks and the depth tracker |
| `src/control/` | Hotkeys, hot-reload, profile switching, ReShade preset import |
| `src/diag/` | GPU crash diagnostics, `panicLayer` |
| `src/core/` | Config (`config/`: INI tokenizer, paths, `PersistenceQueue`, settings), logger, utils |
| `src/render/` | Thin Vulkan helpers: images, buffers, render passes, pipelines, descriptor sets, swapchain state, dispatch tables, depth state |
| `src/effects/` | Effect interface, ReShade runtime (`effect_reshade.cc`), parser, uniforms, texture pool, compile cache, DLSS5 effect, GLSL shaders |
| `src/overlay/` | ImGui overlay, views, parameter field editors |
| `src/input/` | X11 and Wayland input capture/blocking, Wayland symbol interposition, syncobj commit gate |
| `src/vendor/` | **Third-party subtrees** (imgui, reshade FX compiler, opendlss_nr, stb). Change these only through the subtree patch flow in `docs/WORKFLOWS.md` |
| `src/tests/` | Unit tests (one `main()` each, wired in `src/meson.build`), GPU smoke scripts, fixtures |
| `formal/` | Lean 4 + Veil models and proofs of the layer's concurrency/lifetime protocols |
| `packaging/` | Layer manifest template, zig i686 cross file, Flatpak, Nix (root `flake.*` and `todos.txt` are symlinks) |
| `docs/` | `WORKFLOWS.md` (operations), `todos.txt` (roadmap) |

---

## 3. Target architecture

The overhaul (§5) moves the code toward this shape. New code goes into the target modules. Do
not add global state to `layer/state.hh` or grow `LogicalDevice`.

```
layer/entry        GIPA/GDPA, interface negotiation, one table {name, fn, required ext/version}
layer/registry     InstanceState / DeviceState keyed by dispatch key; read-mostly, no global mutex
layer/device_plan  pure planInstanceCreate / planDeviceCreate (unit-tested; no Vulkan calls)
gpu/queue          QueueState per VkQueue: mutex, family, real present fn. Every vkQueue* goes through it
gpu/timeline       per-device GPU serials (fence-backed); completedSerial()
gpu/retire         per-device RetireQueue {serial, VkObjectType, handle}; collect(completed) at present
present/swapchain  SwapchainState: owned create-info copy, frame ring, game-facing fake images,
                   two ping-pong images, per-swapchain overlay target
present/present    snapshot control state → choose mode {Effects, CopyOnly, Passthrough} → submit → present
effects/graph      EffectGraph → RenderPlan (tracked layouts, minimal barriers); built off-thread, swapped at present
effects/compiler   one preprocess setup + prelude; worker thread; memory + disk cache
depth/tracker      DepthTracker (own mutex), per-command-buffer CmdState, barrier-tracked layouts, chosen at submit
control/frontend   hotkeys, overlay commands, profile switching, resize debounce → ControlCommands per frame
config/store       typed schema; one INI tokenizer; immutable snapshots for hot threads
config/persist     PersistenceQueue: single ordered writer for every file mutation; ignores own writes in watcher
overlay/ui         OverlayUi owns its ImGuiContext; reads CoreSnapshot; emits commands; never touches core state
diag/              GPU crash diagnostics, checkpoints, submit journal, debug dumps
```

---

## 4. Invariants (non-negotiable)

Each rule names the failure it prevents. Rules marked **[F]** are modeled and proved in `formal/` (§6).

1. **No GPU wait, file I/O, or shader compile while holding any layer mutex.** [F: `LockDiscipline`]
   The app's submits pass through the layer's hooks. If a hook blocks on a lock held by a thread
   that is itself waiting on the GPU, the system can deadlock, for example when DXVK waits on a
   semaphore that a later submit signals.
2. **Every `vkQueue*` call holds that queue's `QueueState` mutex.** This applies to the app's
   hooked calls and to layer submits alike. The layer submits only on the queue it was handed:
   the present queue, or the optical-flow queue under its own lock. [F: `LockDiscipline`]
3. **Layer objects are destroyed only after the GPU serial of their last use completes.**
   Deferred destruction is per device and keyed on a GPU serial. `vkDestroyDevice` drains it after
   `vkDeviceWaitIdle`. There is no process-global destroy queue, and nothing is flushed "when it
   looks safe". [F: `RetireQueue`]
4. **The layer does not record its pipelines, descriptor sets or render passes into application
   command buffers.** The only allowed injection is a transfer-only copy into layer-owned storage.
   That storage may be destroyed only when no in-flight submission references it and no recorded
   app command buffer is still bound to it. A recorded buffer can be resubmitted later, so this
   requires tracking reset/re-begin/free/pool-reset/pool-destroy (H8). Without that tracking,
   inject only into `ONE_TIME_SUBMIT` buffers. [F: `ChainRebuild`]
5. **Present never shows an image the game did not render.** For a swapchain whose images were
   replaced by fakes, every present goes through a layer submission. That submission waits on the
   app's present semaphores, consuming each exactly once, and writes the real image. Valid modes
   are `Effects` and `CopyOnly`. `Passthrough` is legal only for never-faked swapchains:
   `mode = faked ? (bypass ? CopyOnly : Effects) : Passthrough`. [F: `PresentModes`]
6. **Chain or depth rebuilds swap generations atomically.** Command buffers reference exactly one
   resource generation. A generation is retired only after its last submission completes.
   [F: `ChainRebuild`]
7. **Hot paths stay cheap.** `vkCmd*` hooks, `vkQueueSubmit*` and `vkBeginCommandBuffer` must:
   - take no global mutex;
   - allocate nothing;
   - build no strings before a log-level check (use `VKHOOK_LOG_DEBUG([&]{ … })`);
   - not exist at all when unused. GDPA must not return draw/render-pass hooks while depth
     capture is off, and must return null for commands the device did not enable.
8. **Game threads read immutable snapshots.** Settings, effect parameters and control state are
   published as `shared_ptr<const T>` via atomic swap. UI code never mutates core state directly; it
   emits commands that core drains at a present boundary.
9. **Device creation is planned, not patched.**
   - Never enable a feature or extension without querying support.
   - Never set `pEnabledFeatures` when the app chains `VkPhysicalDeviceFeatures2`.
   - Deep-copy anything you keep from a create-info.
10. **No process-wide side effects.**
    - No signal handlers and no `setjmp`/`longjmp`.
    - No `dlsym` export or interposition beyond the documented Wayland GOT patch.
    - No exception crosses the C ABI: every entry point catches all and soft-disables the device.
11. **Images: layouts are tracked, not guessed.**
    - Barrier aspects match the format (depth+stencil for combined formats).
    - Use `DEPTH_READ_ONLY_OPTIMAL` only with `separateDepthStencilLayouts` enabled.
    - Never call `vkCmdResolveImage` on depth.
12. **User files are the user's.**
    - VkHook rewrites only keys it owns and preserves unknown keys and comments.
    - Every file mutation (save, delete, rename, import, settings) goes through the single
      `PersistenceQueue`.
    - The config watcher ignores VkHook's own writes. [F: `PersistenceQueue`]
13. **Per-device and per-swapchain state, never per-process.**
    - Two devices and several swapchains must work.
    - No function-local `static` state in hooks.
    - No global `realQueuePresent`.
    - No single shared ImGui context.

---

## 5. The overhaul

The overhaul is driven by a full code review. Its defect catalog is §7. The work runs in
three git worktrees in parallel, plus the formal model on `main`.

| Worktree | Branch | Path | Owns |
|---|---|---|---|
| core | `overhaul/core` | `../VKHook-wt/core` | `src/core/vkhook*.{cc,hh}` and the new `src/{layer,gpu,present,depth,control,diag}/`; `src/render/{logical_device,logical_swapchain,fake_swapchain,vkdispatch,vkfuncs,vk_handle,command_buffer,renderpass,depth_*}*` |
| effects | `overhaul/effects` | `../VKHook-wt/effects` | `src/effects/**`; `src/render/{image,image_view,buffer,memory,sampler,format,shader,graphics_pipeline,descriptor_set,framebuffer,dlss5_*}*`; effect tests |
| frontend | `overhaul/frontend` | `../VKHook-wt/frontend` | `src/core/config/**`; `src/core/{logger,async_writer,util}*`; `src/overlay/**`; `src/input/**`; packaging, CI, `scripts/`; their tests |
| formal | `main` | `formal/` | `formal/**` |

Rules for parallel work:
- **Edit only files your worktree owns.** If you need a change in someone else's file, write it
  down as a follow-up in your final report with exact file, symbol and change. The integrator
  routes it.
- **`src/meson.build` is shared.** Add or remove only the lines for your own files, and keep each
  hunk small and local.
- **Do not edit `AGENTS.md`** from a worktree. The integrator updates §7 status at merge.
- **Commits:** imperative subject, optionally prefixed by area (`core: …`, `effects: …`). Make
  small coherent commits that each build. Never rewrite `main`'s history.
- **Integration:** the integrator merges worktree branches into `main` at the end of each stage,
  runs the full suite, then rebases every worktree onto the new `main`.

Stages:
1. **Fixes and split.**
   - core: split `vkhook.cc` into the §3 modules without changing behaviour; delete the signal
     handlers; fix the entry-point bugs.
   - effects: memory-safety and correctness fixes.
   - frontend: config data loss, persistence ordering, overlay and input fixes, logger.
   - formal: models and proofs.
2. **Architecture.**
   - core: registry, queue locks, timeline + RetireQueue, present modes, device plan.
   - effects: EffectGraph, uniform ring, worker compile, EffectId, pipeline cache.
   - frontend: per-swapchain overlay, UI command queue, ConfigStore, PresetModel.
3. **Depth and infra.**
   - core: DepthTracker rewrite.
   - frontend: meson core library, CI matrix with validation, pixel readback in smoke tests,
     packaging fixes.

---

## 6. Formal models (`formal/`)

`formal/` is a Lake package that depends on Veil (`~/Development/veil`, override with
`VEIL_HOME`). It reuses Veil's prebuilt `.lake/packages`. Build with `just formal` or
`cd formal && lake build`. `lake-manifest.json` and `.lake/` are gitignored, because the manifest
embeds machine paths.

Each protocol model has these parts:
- A fixed model: the protocol as the C++ implements it, or will implement it in the target design.
  It carries `safety`/`invariant` clauses, a `#model_check` on a small instance, and a
  `#check_invariants` that closes the inductive invariant.
- A buggy model where the old code had a defect. It encodes the old behaviour, and a
  `sat trace` or a `#model_check` violation shows the defect is reachable. This is the regression
  evidence that the fix matters.
- A doc comment mapping each action to the C++ function that performs it.

| Model | Protects | C++ |
|---|---|---|
| `RetireQueue` | Invariant 3 | `gpu/retire`, `gpu/timeline` |
| `LockDiscipline` | Invariants 1–2 (no deadlock with app semaphore chains) | `gpu/queue`, every hook |
| `PresentModes` | Invariant 5 | `present/present` |
| `ChainRebuild` | Invariants 4, 6 | `present/swapchain`, `effects/graph`, `depth/tracker` |
| `SyncobjGate` | Commit/present exclusion on Wayland explicit sync. Same-thread owner bypass is required (Mesa commits from inside the driver present); no wait-budget escape; `beginPresent` drains in-flight commits | `input/wayland_syncobj_gate` |
| `PersistenceQueue` | Invariant 12 | `config/persist` |

Rules:
- **Change the model first.** A change to a modeled protocol updates the model in the same branch,
  and `just formal` must stay green.
- **Test the C++ against the model.** Where a C++ component implements a model, a unit test drives
  it with random action sequences and checks the model's safety clauses after every step.
- **Prefer proofs checked in the kernel.** Use `set_option veil.smt.trust false` where
  reconstruction works. Otherwise leave the SMT-trusted result and document why.

---

## 7. Defect catalog (status tracked at integration)

These were found by review. Paths and lines are from before the overhaul.

The status column uses three values:
- **open**: not fixed yet.
- **S*n***: fixed in stage *n*.
- **dropped**: the item was rejected or made obsolete; the reason is noted inline.

### Critical

| # | Defect | Where | Status |
|---|---|---|---|
| C1 | Process-wide SIGSEGV/SIGABRT/SIGFPE handlers with no chaining. They kill Wine/Proton fault handling, and their `siglongjmp` is undefined behaviour | `vkhook.cc:641-687,1355-1381`, `reshade_parser.cc:424-460` | S1 |
| C2 | Bypass paths present the real images, which the game never rendered (the game draws into fakes): the screen freezes or shows garbage. The depth hold armed on every swapchain create triggers this | `vkhook.cc:2341,3755-3764,3844` | open |
| C3 | `saveSettings` rewrites `VkHook.conf` with only the 22 keys it owns, deleting the user's other keys | `config_serializer.cc:349-427` | S1 |
| C4 | Snapshot pipelines, descriptor sets and render passes are recorded into the game's command buffers. App state is not restored, the game's depth `storeOp` becomes DONT_CARE, and the objects are destroyed while the game's buffers still reference them | `vkhook.cc:5128,5190,5228`, `vkhook_depth.cc:806`, `renderpass.cc:95,213` | open |
| C5 | The overlay is built from the first swapchain only. More swapchain images cause an out-of-bounds read; a format change causes a framebuffer mismatch | `vkhook.cc:2622`, `imgui_overlay.cc:879` | S1 partial (bounds check, `onSwapchainChanged`); wiring + per-swapchain renderer S2 |
| C6 | The replacement `wl_pointer` listener is missing v9/v11 events, so libwayland aborts. It also passes NULL user data | `wayland_interpose.cc:244-256,522,539` | S1 |
| C7 | RG8 and wide-format texture loading overflows the heap | `effect_reshade.cc:488,551,581` | S1 |
| C8 | GPU waits happen while holding `globalLock`, which every app submit also needs: deadlock or freeze | `image.cc:215,318`, `vkhook.cc:3612,3867,4418-4440` | open |

### High

| # | Defect | Status |
|---|---|---|
| H1 | `globalLock` is taken in every draw, render-pass, submit and begin hook, even when depth capture is off. The draw hook calls down while holding it | S1 partial (no call-down under lock; depth hooks not installed when capture off); lock removal S2 |
| H2 | Reload, disk I/O, compiles and overlay updates run on the present thread under `globalLock` | open |
| H3 | The layer submits to graphics queue 0 without synchronizing with app threads | open |
| H4 | `realQueuePresent` is a single process global, wrong for a second device or driver | open |
| H5 | `CreateDevice` forces unsupported features and sets `pEnabledFeatures` alongside Features2 | open |
| H6 | `DeferredDestroyQueue` is process-global and untagged. `panicLayer` flushes it without waiting | open |
| H7 | `BeginCommandBuffer` on an unknown device returns success without calling down | S1 |
| H8 | Command pool reset and destroy are not hooked, so the per-command-buffer maps go stale | open |
| H9 | `vkCmdResolveImage` is used on depth; `DEPTH_READ_ONLY_OPTIMAL` is used without `separateDepthStencilLayouts`; barriers on D/S images cover only one aspect | open |
| H10 | Depth capture method 2 disables itself (storage image missing from `depthImageExtents`). There is no `vkQueueSubmit2` hook | open |
| H11 | Image layouts are guessed rather than tracked; there are no barrier hooks | open |
| H12 | One uniform buffer per effect is rewritten while earlier frames are still in flight | open |
| H13 | Uniforms become specialization constants on non-NVIDIA GPUs, so slider edits do nothing | S1 |
| H14 | Only `techniques[0]` of a file ever runs | open |
| H15 | The registry parse and the runtime compile use different preludes, extents and macros | S1 (core must call `setCompileExtent`: S2) |
| H16 | Every autosave triggers a full reload | S1 |
| H17 | Settings saves `fsync` on the present thread with a mutex held | S1 (hot-thread snapshot call sites: S2 core) |
| H18 | File writes come from two unordered threads, so a deleted preset can be resurrected | S1 |
| H19 | Two `VkHook.conf` parsers that disagree | S1 |
| H20 | The swapchain command pool lacks the RESET flag but buffers are re-recorded. Real images lack `TRANSFER_DST` | open |
| H21 | VRAM: `imageCount × (maxEffects+1)` full-resolution images, and effects beyond the cap are silently dropped | open |
| H22 | Overlay fence is reset without a submit, so it stays unsignalled forever. ImGui context ownership is ambiguous | S1 |
| H23 | X11 makes blocking round trips every frame. Wayland game key handlers run on the wrong thread under a mutex | S1 |
| H24 | Syncobj gate check-then-act race | S1 |
| H25 | Instances of the same effect share private textures, including history | S1 |
| H26 | Unknown or unhandled uniform sources leave uninitialized bytes | S1 |

### Medium and low (abridged)

The full lists are in the review reports.
- The monolith files (`vkhook.cc` with an ~890-line `QueuePresentKHR`; `vkhook_internal.hh` with duplicate declarations).
- The `LogicalDevice` god object.
- About 750 lines of copy-pasted hooks.
- Twelve depth-resolve shader variants, of which one is used.
- Six copy-pasted numeric field editors.
- Three render-pass builders.
- A pipeline cache that is never persisted.
- Synchronous per-texture uploads.
- Debug strings built eagerly.
- Dead code:
  - `performDeferredDepthRealloc`
  - `rebuildEffectsOnNextPresent`
  - `LogicalSwapchain::reloadEffects`
  - the logger history API
  - the pointer-constraints module
  - `tools/test-shaders.cc`
  - `effects/shaders/shaders-meson.build`
  - `normals_to_depth.comp.glsl`
- Build and packaging:
  - 32-bit and 64-bit installs overwrite each other's `VkHook.json`;
  - Flatpak copies the whole tree, including `build-*/`;
  - CI downloads moving shader archives;
  - CI has no sanitizer, gcc or i686 job.

---

## 8. Conventions

- **Language and naming.** C++20, namespace `VkHook`, headers `.hh`, sources `.cc`.
  Types are `PascalCase`, functions and variables `camelCase`. Format with `.clang-format`
  (`scripts/format.sh`), but never reformat code you did not touch.
- **Comments** say why, not what. Delete stale comments and dead code when you touch an area;
  don't leave `// REMOVED:` markers.
- **Errors.** Vulkan results are checked. Builders return `std::expected`-style results or
  `bool`, plus RAII owners. Do not "log and return" from a constructor, and do not use the
  unbraced `ASSERT_VULKAN` hidden-return pattern in new code.
- **Tests** are deterministic, need no GPU unless they are smoke tests, and are wired in
  `src/meson.build`. The fake-dispatch fixture in `src/render/depth_snapshot_scope_test.cc`
  lets you test Vulkan-facing logic without a GPU.
- **Logging.** Use `Logger::{debug,info,warn,err}` only outside hot paths. On hot paths use the
  `VKHOOK_LOG_*` macros with a lambda.
- **Docs.** User-visible behaviour changes update `README.md` or `docs/WORKFLOWS.md`.
