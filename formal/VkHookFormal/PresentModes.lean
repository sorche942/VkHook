module

public import Veil

/-!
# PresentModes: the screen only ever shows what the game rendered

Protects AGENTS.md invariant 5 and defect C2 (bypass paths presented the *real* swapchain
images, which the game never rendered, so the screen froze or showed garbage; a depth hold
armed on every swapchain create used to trigger it; it no longer exists), plus the zero-wait-semaphore present.

## Protocol

A swapchain is *faked* if the layer replaced its images by game-facing fake images at
creation (`faked`, fixed for the swapchain's lifetime). The game acquires slot `i`, renders
frame `f` into the fake image of that slot (real image if the swapchain is not faked) and
signals an app present semaphore `s`. Semaphores are binary: a signal requires the semaphore
unsignalled, a wait consumes it. The game then calls `vkQueuePresentKHR` for `f` waiting on `s`.
The layer picks a mode from the swapchain and a "bypass requested" input (effects disabled,
overlay hidden, ...):

* faked: `Effects`, or `CopyOnly` when bypass is requested. Both go through a layer
  submission that waits on `s` (consuming it exactly once), copies/processes fake → real
  and then the real present shows the real image.
* never faked: `Passthrough`: the present itself waits on `s` and shows the real image the
  game rendered into.

## Action ↔ C++ (target design, AGENTS.md §3)

| model action        | C++                                                                      |
|---------------------|--------------------------------------------------------------------------|
| `render f sc i s`   | the game (outside the layer): draws into the image returned by           |
|                     | `vkAcquireNextImageKHR` (fake image from `present/swapchain` ring)       |
| `present f bypass`  | hooked `vkQueuePresentKHR` → `present/present` `chooseMode(...)`         |
| `layerSubmit f`     | `present/present` layer submission: waits all app present semaphores,    |
|                     | records fake→real copy (+effects) on the present queue                   |
| `realPresent f`     | real `vkQueuePresentKHR` of the real image via `QueueState` present      |
| `passthrough f`     | `present/present` Passthrough: forward the app's present unchanged       |

`chooseMode` in the model is `if faked then (if bypass then CopyOnly else Effects) else
Passthrough`: **bypass never selects Passthrough on a faked swapchain**.

## Proved (`#check_invariants`)

* `shows_rendered_frame`: when a present completes, the real image shown holds frame `f`
  itself, the frame the game rendered for that present.
* `sem_consumed_once`: every present consumes its app semaphore exactly once (never 0, never 2).
* `passthrough_only_unfaked`: Passthrough happens only for swapchains that were never faked.

Buggy variant `PresentModesBug` (old bypass): bypass presents the real image directly,
(a) with the wait, showing stale content on a faked swapchain, and (b) with zero waits,
leaving the binary semaphore signalled. Both `sat trace`s must stay satisfiable.
-/

veil module PresentModes

type sc
type slot
type frame
type sem

immutable relation faked : sc → Bool

enum phaseT = { fresh, rendered, called, submitted, shown }
enum modeT = { effects, copyOnly, passthrough }

function phase : frame → phaseT
function mode : frame → modeT
function fsc : frame → sc
function fslot : frame → slot
function fsem : frame → sem
function pwaits : frame → Nat
function shown_content : frame → frame

function fake_content : sc → slot → frame
function real_content : sc → slot → frame
relation slot_busy : sc → slot → Bool
relation sem_sig : sem → Bool

#gen_state

after_init {
  phase F := fresh
  mode F := effects
  fsc F := default
  fslot F := default
  fsem F := default
  pwaits F := 0
  shown_content F := default
  fake_content S I := default
  real_content S I := default
  slot_busy S I := false
  sem_sig M := false
}

-- The game renders frame `f` into slot `i` and signals app semaphore `s`.
action render (f : frame) (c : sc) (i : slot) (s : sem) {
  require phase f = fresh
  require ¬ slot_busy c i
  require ¬ sem_sig s
  fsc f := c
  fslot f := i
  fsem f := s
  if faked c then
    fake_content c i := f
  else
    real_content c i := f
  slot_busy c i := true
  sem_sig s := true
  phase f := rendered
}

-- Hooked vkQueuePresentKHR: choose the mode.
action present (f : frame) (bypass : Bool) {
  require phase f = rendered
  if faked (fsc f) then
    if bypass then
      mode f := copyOnly
    else
      mode f := effects
  else
    mode f := passthrough
  phase f := called
}

-- Layer submission: waits `s` (consumed), copies fake → real.
action layerSubmit (f : frame) {
  require phase f = called ∧ mode f ≠ passthrough
  require sem_sig (fsem f)
  sem_sig (fsem f) := false
  pwaits f := pwaits f + 1
  real_content (fsc f) (fslot f) := fake_content (fsc f) (fslot f)
  phase f := submitted
}

action realPresent (f : frame) {
  require phase f = submitted
  shown_content f := real_content (fsc f) (fslot f)
  slot_busy (fsc f) (fslot f) := false
  phase f := shown
}

-- Never-faked swapchain: the app's present is forwarded and waits `s` itself.
action passthroughPresent (f : frame) {
  require phase f = called ∧ mode f = passthrough
  require sem_sig (fsem f)
  sem_sig (fsem f) := false
  pwaits f := pwaits f + 1
  shown_content f := real_content (fsc f) (fslot f)
  slot_busy (fsc f) (fslot f) := false
  phase f := shown
}

ghost relation active (f : frame) := phase f = rendered ∨ phase f = called ∨ phase f = submitted
ghost relation unsent (f : frame) := phase f = rendered ∨ phase f = called

safety [shows_rendered_frame] phase F = shown → shown_content F = F
safety [sem_consumed_once] (phase F = shown → pwaits F = 1) ∧ (pwaits F = 0 ∨ pwaits F = 1)
safety [passthrough_only_unfaked]
  (phase F = called ∨ phase F = submitted ∨ phase F = shown) → mode F = passthrough → ¬ faked (fsc F)

invariant [active_slot_busy] active F → slot_busy (fsc F) (fslot F)
invariant [slot_owner_unique]
  active F ∧ active G ∧ fsc F = fsc G ∧ fslot F = fslot G → F = G
invariant [content_where_rendered]
  unsent F →
    (faked (fsc F) → fake_content (fsc F) (fslot F) = F) ∧
    (¬ faked (fsc F) → real_content (fsc F) (fslot F) = F)
invariant [content_copied] phase F = submitted → faked (fsc F) ∧ real_content (fsc F) (fslot F) = F
invariant [mode_matches_swapchain]
  (phase F = called ∨ phase F = submitted ∨ phase F = shown) →
    (faked (fsc F) → mode F ≠ passthrough) ∧ (¬ faked (fsc F) → mode F = passthrough)
invariant [waits_by_phase]
  (phase F = fresh ∨ phase F = rendered ∨ phase F = called → pwaits F = 0) ∧
  (phase F = submitted ∨ phase F = shown → pwaits F = 1)
invariant [pending_sem_signalled] unsent F → sem_sig (fsem F)
invariant [sem_owner_unique]
  unsent F ∧ unsent G ∧ fsem F = fsem G → F = G

set_option veil.smt.trust false

#gen_spec

#model_check { sc := Fin 2, slot := Fin 1, frame := Fin 3, sem := Fin 2 }
  { faked := fun c => c.val = 0 }

sat trace [faked_effects_present] {
  render
  present
  layerSubmit
  realPresent
  assert (∃ f, phase f = shown ∧ faked (fsc f) ∧ mode f = effects)
}

sat trace [faked_copy_only_present_under_bypass] {
  render
  present
  layerSubmit
  realPresent
  assert (∃ f, phase f = shown ∧ faked (fsc f) ∧ mode f = copyOnly)
}

sat trace [unfaked_passthrough_present] {
  render
  present
  passthroughPresent
  assert (∃ f, phase f = shown ∧ ¬ faked (fsc f) ∧ mode f = passthrough)
}

#check_invariants

end PresentModes

/-! ## Buggy variant: old bypass presents the real image directly (defect C2) -/

veil module PresentModesBug

type sc
type slot
type frame
type sem

immutable relation faked : sc → Bool

enum phaseT = { fresh, rendered, called, submitted, shown }
enum modeT = { effects, copyOnly, passthrough }

function phase : frame → phaseT
function mode : frame → modeT
function fsc : frame → sc
function fslot : frame → slot
function fsem : frame → sem
function pwaits : frame → Nat
function shown_content : frame → frame

function fake_content : sc → slot → frame
function real_content : sc → slot → frame
relation slot_busy : sc → slot → Bool
relation sem_sig : sem → Bool

#gen_state

after_init {
  phase F := fresh
  mode F := effects
  fsc F := default
  fslot F := default
  fsem F := default
  pwaits F := 0
  shown_content F := default
  fake_content S I := default
  real_content S I := default
  slot_busy S I := false
  sem_sig M := false
}

action render (f : frame) (c : sc) (i : slot) (s : sem) {
  require phase f = fresh
  require ¬ slot_busy c i
  require ¬ sem_sig s
  fsc f := c
  fslot f := i
  fsem f := s
  if faked c then
    fake_content c i := f
  else
    real_content c i := f
  slot_busy c i := true
  sem_sig s := true
  phase f := rendered
}

-- BUG: a bypass request (effects off, soft-disabled, ...) selects
-- Passthrough even though the swapchain's images were replaced by fakes.
action present (f : frame) (bypass : Bool) {
  require phase f = rendered
  if bypass then
    mode f := passthrough
  else
    if faked (fsc f) then
      mode f := effects
    else
      mode f := passthrough
  phase f := called
}

action layerSubmit (f : frame) {
  require phase f = called ∧ mode f ≠ passthrough
  require sem_sig (fsem f)
  sem_sig (fsem f) := false
  pwaits f := pwaits f + 1
  real_content (fsc f) (fslot f) := fake_content (fsc f) (fslot f)
  phase f := submitted
}

action realPresent (f : frame) {
  require phase f = submitted
  shown_content f := real_content (fsc f) (fslot f)
  slot_busy (fsc f) (fslot f) := false
  phase f := shown
}

-- (a) the old bypass forwards the present: the real image is shown, the game never drew it.
action passthroughPresent (f : frame) {
  require phase f = called ∧ mode f = passthrough
  require sem_sig (fsem f)
  sem_sig (fsem f) := false
  pwaits f := pwaits f + 1
  shown_content f := real_content (fsc f) (fslot f)
  slot_busy (fsc f) (fslot f) := false
  phase f := shown
}

-- (b) the old bypass presents the real image with zero wait semaphores: the app's binary
-- semaphore is never consumed.
action passthroughPresentNoWait (f : frame) {
  require phase f = called ∧ mode f = passthrough
  shown_content f := real_content (fsc f) (fslot f)
  slot_busy (fsc f) (fslot f) := false
  phase f := shown
}

invariant [pwaits_small] pwaits F = 0 ∨ pwaits F = 1

#gen_spec

sat trace [bypass_shows_stale_real_image] {
  render
  present
  passthroughPresent
  assert (∃ f, phase f = shown ∧ faked (fsc f) ∧ shown_content f ≠ f)
}

sat trace [bypass_present_with_zero_waits] {
  render
  present
  passthroughPresentNoWait
  assert (∃ f, phase f = shown ∧ pwaits f = 0 ∧ sem_sig (fsem f))
}

end PresentModesBug
