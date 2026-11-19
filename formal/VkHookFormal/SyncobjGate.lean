module

public import Veil

/-!
# SyncobjGate: Wayland surface commits vs. Vulkan presents on explicit-sync surfaces

Protects defect H24 (check-then-act race in `WaylandSyncobjGate`), C++ `input/wayland_syncobj_gate`.

Mesa's WSI sets `linux-drm-syncobj` points inside its present call. A `wl_surface.commit` sent
by *another* thread between the point and the attach is a protocol error; the compositor
(hyprland) kills the connection. So a commit send must never overlap an in-progress present on
the same surface. The presenting thread's *own* commits (made by Mesa inside the driver present
call) are part of the present and must pass.

## Action ↔ C++

| model action                | C++                                                                |
|-----------------------------|--------------------------------------------------------------------|
| `beginPresent t s`          | `WaylandSyncobjGate::beginPresent(surfaceId)` (target: takes the   |
|                             | per-surface `shared_mutex` **exclusively**, records the owner)     |
| `endPresent t s`            | `endPresent(surfaceId)`                                            |
| `commitStart t s` / `commitEnd t s` | `beginCommit(surfaceId)` in the `wl_proxy_marshal*` wrapper for opcode 6. Target: take the |
|                             | mutex **shared**, perform the real send, release after it          |
| `ownerCommit t s` / `ownerCommitEnd t s` | a commit from the presenting thread inside the driver call: re-entrant, must not take the lock (it would self-deadlock) |
| old `commitCheck` / `commitSend` | old `beginCommit`: test under the mutex, release, *then* marshal   |
| old `commitBudgetExpired`   | `waitForCloseLocked` timeout: "letting a commit through"           |

## Proved

`no_overlap` (`#check_invariants`, all sizes): if a commit is being sent on surface `S` while a
present is in progress on `S`, the committing thread is the presenting thread itself.

## Findings about the C++ target design

1. A plain `std::shared_mutex` would deadlock on the presenter's own commits; the target must
   keep the "same thread as the owner" bypass (modelled by `ownerCommit`).
2. The old wait budget ("letting a commit through" after 100 ms) is itself an overlap: the old model
   shows a commit is sent while the present is still open. A commit cannot be dropped or
   deferred safely, so the fixed design has no timeout escape. A wedged presenter has to be handled
   elsewhere (the exclusive section is only the driver present call).
3. The old `endPresent`/`beginPresent` also never waited for commits already mid-send
   (`commitSend` before `beginPresent`): `beginPresent` must drain shared holders.
-/

veil module SyncobjGate

type tid
type surf

function pres_owner : surf → tid
relation in_present : surf → Bool
relation shared : tid → surf → Bool
relation committing : tid → surf → Bool

#gen_state

after_init {
  pres_owner S := default
  in_present S := false
  shared T S := false
  committing T S := false
}

action beginPresent (t : tid) (s : surf) {
  require ¬ in_present s
  require ∀ u, ¬ shared u s
  in_present s := true
  pres_owner s := t
}

action endPresent (t : tid) (s : surf) {
  require in_present s ∧ pres_owner s = t
  require ¬ committing t s
  in_present s := false
}

-- Another thread's commit: shared lock held across the real send.
action commitStart (t : tid) (s : surf) {
  require ¬ in_present s
  require ¬ committing t s
  shared t s := true
  committing t s := true
}

action commitEnd (t : tid) (s : surf) {
  require shared t s
  shared t s := false
  committing t s := false
}

-- The presenter's own commit inside the driver present call (no lock taken).
action ownerCommit (t : tid) (s : surf) {
  require in_present s ∧ pres_owner s = t
  require ¬ committing t s
  committing t s := true
}

action ownerCommitEnd (t : tid) (s : surf) {
  require committing t s ∧ ¬ shared t s
  committing t s := false
}

safety [no_overlap] committing T S ∧ in_present S → pres_owner S = T

invariant [shared_is_committing] shared T S → committing T S
invariant [no_shared_during_present] in_present S → ¬ shared T S
invariant [commit_has_cover] committing T S → shared T S ∨ (in_present S ∧ pres_owner S = T)

set_option veil.smt.trust false

#gen_spec

#model_check { tid := Fin 3, surf := Fin 2 }

sat trace [other_thread_commits_between_presents] {
  beginPresent
  endPresent
  commitStart
  commitEnd
  assert (∃ s, ¬ in_present s)
}

sat trace [owner_commit_inside_present] {
  beginPresent
  ownerCommit
  assert (∃ t s, committing t s ∧ in_present s ∧ pres_owner s = t)
}

sat trace [present_waits_for_commit_then_runs] {
  commitStart
  commitEnd
  beginPresent
  assert (∃ s, in_present s)
}

#check_invariants

end SyncobjGate

/-! ## Buggy variant: check-then-act (defect H24) -/

veil module SyncobjGateBug

type tid
type surf

function pres_owner : surf → tid
relation in_present : surf → Bool
relation checked : tid → surf → Bool
relation committing : tid → surf → Bool

#gen_state

after_init {
  pres_owner S := default
  in_present S := false
  checked T S := false
  committing T S := false
}

-- Old beginPresent: records the owner under the map mutex and does not wait for commits.
action beginPresent (t : tid) (s : surf) {
  require ¬ in_present s
  in_present s := true
  pres_owner s := t
}

action endPresent (t : tid) (s : surf) {
  require in_present s ∧ pres_owner s = t
  require ¬ committing t s
  in_present s := false
}

-- Old beginCommit: the check happens under the mutex, which is released on return ...
action commitCheck (t : tid) (s : surf) {
  require ¬ in_present s ∨ pres_owner s = t
  checked t s := true
}

-- ... and only afterwards the wrapper marshals the commit (unlocked).
action commitSend (t : tid) (s : surf) {
  require checked t s
  checked t s := false
  committing t s := true
}

action commitEnd (t : tid) (s : surf) {
  require committing t s
  committing t s := false
}

-- Old waitForCloseLocked: after the wait budget the commit is let through.
action commitBudgetExpired (t : tid) (s : surf) {
  require in_present s ∧ pres_owner s ≠ t
  checked t s := true
}

#gen_spec

-- (a) the present starts between the check and the send.
sat trace [present_begins_between_check_and_send] {
  commitCheck
  beginPresent
  commitSend
  assert (∃ t s, committing t s ∧ in_present s ∧ pres_owner s ≠ t)
}

-- (b) the present starts while a commit is already being marshalled.
sat trace [present_begins_during_send] {
  commitCheck
  commitSend
  beginPresent
  assert (∃ t s, committing t s ∧ in_present s ∧ pres_owner s ≠ t)
}

-- (c) the wait budget expires and the commit goes through an open present.
sat trace [budget_expiry_lets_commit_through] {
  beginPresent
  commitBudgetExpired
  commitSend
  assert (∃ t s, committing t s ∧ in_present s ∧ pres_owner s ≠ t)
}

end SyncobjGateBug
