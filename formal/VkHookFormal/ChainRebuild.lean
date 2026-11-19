module

public import Veil

/-!
# ChainRebuild: resource generations, layer vs. application command buffers

Protects AGENTS.md invariants 4 and 6 and defect C4 (snapshot pipelines, descriptor sets and
render passes were recorded into the *game's* command buffers, and destroyed while the game's
buffers still referenced them).

## Protocol (target design)

Effect chain / depth resources live in *generations* `gen`. `cur_gen` is the generation the
layer records new work in. A rebuild creates generation `n`, makes it current and retires the
old one, atomically at a present boundary (invariant 6). Layer command buffers (`lrecord`) are
bound to exactly one generation (the current one) and are owned and re-recorded by the layer,
so a rebuild simply unbinds the ones bound to the retiring generation. A generation is
destroyed only when no in-flight submission references it and no recorded command buffer
references it.

Application command buffers never reference a generation (invariant 4). The one permitted
injection is a transfer-only copy into layer-owned *storage* `stg` (`appRecord`); the layer
must know when that storage may go away:

| model action         | C++ (implemented)                                                         |
|----------------------|---------------------------------------------------------------------------|
| `lrecord c`          | `present/present` records into the layer's own cb for `cur_gen`; the depth|
|                      | capture cb (`DepthCapture::record`) is a layer cb of the same kind        |
| `appRecord c r`      | `DepthTracker::recordPreserveCopy`: the transfer-only copy, which binds   |
|                      | the storage into that cb's `CmdState::storages`                           |
| `appReset c`         | `DepthTracker::onResetCommandBuffer`, `onBeginCommandBuffer`,             |
|                      | `onFreeCommandBuffers`, `onResetCommandPool`, `onDestroyCommandPool` (H8) |
| `submit s c`         | `vkQueueSubmit*` (`DepthTracker::onSubmit` for app cbs; layer submits)    |
| `gpuComplete s`      | the GPU finishes `s` (`gpu/timeline` serial advances)                     |
| `rebuild n`          | `effects/graph` chain swap at present (a depth source change no longer    |
|                      | rebuilds: the capture target is stable, only its per-present cb changes)  |
| `destroyGen g`       | `gpu/retire` collect of the retired generation                            |
| `retireStg`/`destroyStg` | `DepthLayerStorage::~DepthLayerStorage` retires through `gpu/retire`; |
|                      | storage is a `shared_ptr` held by the tracker, by every recorded cb       |
|                      | bound to it and by the frame's `DepthSource`, so "no recorded cb bound"   |
|                      | is the reference count reaching zero                                      |

## Proved (`#check_invariants`)

* `no_inflight_destroyed`: no in-flight submission references a destroyed generation or storage.
* `no_recorded_destroyed`: no *recorded* (hence resubmittable) command buffer references a
  destroyed generation or storage, so no future submission can either.
* `recorded_gen_is_current` (invariant 6) and `app_cb_has_no_gen` (invariant 4).

## Finding about the C++ target design

AGENTS.md invariant 4 says the transfer storage's lifetime is "tied to the app submissions that
reference it (fence appended on the same queue)". The model shows that **this is not enough**:
an app command buffer is not one-time-submit by default and can be resubmitted at any later
time, so the storage must also outlive the *recorded command buffer*. The guard on
`destroyStg` therefore requires both "no in-flight submission" and "no recorded cb bound to
it", and `appReset` is driven by the command-buffer/pool lifetime hooks (H8). Without hooking
those, storage can only be tied to cbs recorded with `ONE_TIME_SUBMIT`.

Buggy variant `ChainRebuildBug` (old behaviour): `inject` binds a current-generation object
into an application cb; the layer's `destroyGen` can only see in-flight submissions, so a cb
recorded in generation g is resubmitted after g was destroyed.
-/

veil module ChainRebuild

type gen
type stg
type cb
type sub

enum resState = { unborn, live, retired, destroyed }

immutable relation is_app : cb → Bool

individual cur_gen : gen
function gstate : gen → resState
function sstate : stg → resState

relation bound_gen : cb → Bool
function cb_gen : cb → gen
relation bound_stg : cb → Bool
function cb_stg : cb → stg

relation sub_used : sub → Bool
relation inflight : sub → Bool
relation sub_has_gen : sub → Bool
function sub_gen : sub → gen
relation sub_has_stg : sub → Bool
function sub_stg : sub → stg

#gen_state

after_init {
  cur_gen := default
  gstate G := unborn
  let g := cur_gen
  gstate g := live
  sstate R := live
  bound_gen C := false
  cb_gen C := default
  bound_stg C := false
  cb_stg C := default
  sub_used S := false
  inflight S := false
  sub_has_gen S := false
  sub_gen S := default
  sub_has_stg S := false
  sub_stg S := default
}

ghost relation cb_busy (c : cb) :=
  ∃ s, inflight s ∧
    ((sub_has_gen s ∧ bound_gen c ∧ cb_gen c = sub_gen s) ∨
     (sub_has_stg s ∧ bound_stg c ∧ cb_stg c = sub_stg s))

-- The layer records its own cb in the current generation.
action lrecord (c : cb) {
  require ¬ is_app c ∧ ¬ bound_gen c
  require ∀ s, ¬ (inflight s ∧ sub_has_gen s ∧ cb_gen c = sub_gen s)
  bound_gen c := true
  cb_gen c := cur_gen
}

-- Application cb gets the permitted transfer-only copy into layer-owned live storage.
action appRecord (c : cb) (r : stg) {
  require is_app c ∧ ¬ bound_stg c
  require sstate r = live
  bound_stg c := true
  cb_stg c := r
}

-- vkResetCommandBuffer / vkFreeCommandBuffers / pool reset or destroy.
action appReset (c : cb) {
  require is_app c ∧ bound_stg c
  require ∀ s, ¬ (inflight s ∧ sub_has_stg s ∧ cb_stg c = sub_stg s)
  bound_stg c := false
}

action submit (s : sub) (c : cb) {
  require ¬ sub_used s
  require bound_gen c ∨ bound_stg c
  sub_used s := true
  inflight s := true
  sub_has_gen s := bound_gen c
  sub_gen s := cb_gen c
  sub_has_stg s := bound_stg c
  sub_stg s := cb_stg c
}

action gpuComplete (s : sub) {
  require inflight s
  inflight s := false
}

-- Atomic generation swap: n becomes current, the old generation retires, and the layer's
-- own cbs bound to it are dropped (they are re-recorded in the new generation).
action rebuild (n : gen) {
  require gstate n = unborn
  let g := cur_gen
  gstate n := live
  gstate g := retired
  cur_gen := n
  bound_gen C := bound_gen C ∧ cb_gen C ≠ g
}

action destroyGen (g : gen) {
  require gstate g = retired
  require ∀ s, ¬ (inflight s ∧ sub_has_gen s ∧ sub_gen s = g)
  require ∀ c, ¬ (bound_gen c ∧ cb_gen c = g)
  gstate g := destroyed
}

action retireStg (r : stg) {
  require sstate r = live
  sstate r := retired
}

action destroyStg (r : stg) {
  require sstate r = retired
  require ∀ s, ¬ (inflight s ∧ sub_has_stg s ∧ sub_stg s = r)
  require ∀ c, ¬ (bound_stg c ∧ cb_stg c = r)
  sstate r := destroyed
}

safety [no_inflight_destroyed]
  inflight S →
    (sub_has_gen S → gstate (sub_gen S) ≠ destroyed) ∧
    (sub_has_stg S → sstate (sub_stg S) ≠ destroyed)
safety [no_recorded_destroyed]
  (bound_gen C → gstate (cb_gen C) ≠ destroyed) ∧
  (bound_stg C → sstate (cb_stg C) ≠ destroyed)
safety [recorded_gen_is_current] bound_gen C → cb_gen C = cur_gen
safety [app_cb_has_no_gen] is_app C → ¬ bound_gen C

invariant [layer_cb_has_no_stg] ¬ is_app C → ¬ bound_stg C
invariant [cur_gen_live] gstate cur_gen = live
invariant [only_cur_is_live] gstate G = live → G = cur_gen
invariant [inflight_used] inflight S → sub_used S

set_option veil.smt.trust false

#gen_spec

#model_check { gen := Fin 3, stg := Fin 2, cb := Fin 3, sub := Fin 3 }
  { is_app := fun c => c.val ≠ 0 }

-- Layer cb survives two rebuilds, with in-flight work delaying destruction.
sat trace [generation_retired_and_destroyed] {
  lrecord
  submit
  rebuild
  gpuComplete
  destroyGen
  assert (∃ g, gstate g = destroyed)
}

sat trace [app_storage_lifecycle] {
  appRecord
  submit
  retireStg
  gpuComplete
  appReset
  destroyStg
  assert (∃ r, sstate r = destroyed)
}

-- An app cb is resubmitted after the first submission completed.
sat trace [app_cb_resubmitted] {
  appRecord
  submit
  gpuComplete
  submit
  assert (∃ s c, inflight s ∧ is_app c ∧ bound_stg c)
}

#check_invariants

end ChainRebuild

/-! ## Buggy variant: layer objects injected into application command buffers (defect C4) -/

veil module ChainRebuildBug

type gen
type cb
type sub

enum resState = { unborn, live, retired, destroyed }

immutable relation is_app : cb → Bool

individual cur_gen : gen
function gstate : gen → resState

relation bound_gen : cb → Bool
function cb_gen : cb → gen

relation sub_used : sub → Bool
relation inflight : sub → Bool
function sub_gen : sub → gen

#gen_state

after_init {
  cur_gen := default
  gstate G := unborn
  let g := cur_gen
  gstate g := live
  bound_gen C := false
  cb_gen C := default
  sub_used S := false
  inflight S := false
  sub_gen S := default
}

-- BUG: pipelines / descriptor sets / render passes of the current generation are recorded
-- into the application's command buffer.
action inject (c : cb) {
  require is_app c ∧ ¬ bound_gen c
  bound_gen c := true
  cb_gen c := cur_gen
}

-- The app submits (and later resubmits) its cb whenever it likes.
action submit (s : sub) (c : cb) {
  require ¬ sub_used s
  require bound_gen c
  sub_used s := true
  inflight s := true
  sub_gen s := cb_gen c
}

action gpuComplete (s : sub) {
  require inflight s
  inflight s := false
}

action rebuild (n : gen) {
  require gstate n = unborn
  let g := cur_gen
  gstate n := live
  gstate g := retired
  cur_gen := n
}

-- BUG: the layer can only see in-flight submissions, not the lifetime of recorded app cbs.
action destroyGen (g : gen) {
  require gstate g = retired
  require ∀ s, ¬ (inflight s ∧ sub_gen s = g)
  gstate g := destroyed
}

invariant [inflight_used] inflight S → sub_used S

#gen_spec

sat trace [resubmit_after_generation_destroyed] {
  inject
  rebuild
  destroyGen
  submit
  assert (∃ s, inflight s ∧ gstate (sub_gen s) = destroyed)
}

end ChainRebuildBug
