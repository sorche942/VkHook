module

public import Veil

/-!
# LockDiscipline: no GPU wait under a layer lock; queue calls under the queue mutex

Protects AGENTS.md invariants 1 and 2 and defect C8 (GPU waits while holding `globalLock`,
which every app submit also needs: deadlock or freeze).

## The scenario (DXVK pattern)

The app has GPU batches linked by semaphores: batch `b` (submitted early by app thread `t0`)
waits on a semaphore that batch `c` signals, and `c` is submitted *later* by app thread `t1`
(`dep b c`; legal with timeline semaphores / wait-before-signal). The GPU can run `b` only
after `c` is done, and `c` only runs once `t1` has really called `vkQueueSubmit`, which goes
through the layer's hook. A present thread meanwhile drains the queue before it presents.

* Old design (`LockDisciplineBug`): the present thread takes `globalLock`, submits, waits for
  the queue to go idle **holding the lock**; `t1`'s submit hook needs `globalLock`: deadlock.
* Fixed design (`LockDiscipline`): the app hook holds only that queue's `QueueState` mutex for
  the duration of the `vkQueue*` call; the present path holds the short state lock only for
  the snapshot, each queue mutex only around a single `vkQueue*` call, and the GPU wait
  (fence) happens with *no* lock held.

## Action ↔ C++ (target design, AGENTS.md §3)

| model action          | C++                                                                       |
|-----------------------|---------------------------------------------------------------------------|
| `appEnter t b`        | hook `vkQueueSubmit*` etc.: `QueueState::lock()` (gpu/queue), call down   |
| `appExit t`           | return from the real `vkQueue*`, unlock; the batch is now on the GPU      |
| `presLockL/UnlockL`   | `present/present`: snapshot control state (short layer lock, no I/O)      |
| `presEnterSubmit/Exit`| layer's own `vkQueueSubmit` on the present queue under that queue's mutex |
| `presGpuWait t`       | `Timeline::wait(serial)` on the layer's fence, **no lock held**           |
| `presEnterPresent/Exit` | real `vkQueuePresentKHR` under the queue mutex                          |
| `gpuRun b`            | the GPU: a submitted batch completes once all semaphores it waits on are  |
|                       | signalled (permissive: ignores in-queue order, which only strengthens the |
|                       | claim for the layer since extra GPU blocking is not caused by the layer)  |
| old `pLockG`..        | `vkhook.cc` `QueuePresentKHR` under `globalLock` with `image.cc` waits    |

## What is proved

`#check_invariants` (all sizes, kernel-checked unless noted below):
* `in_call_holds_queue_mutex` (invariant 2): whoever is inside a `vkQueue*` call on `Q`
  holds `Q`'s mutex; hence `queue_call_exclusive` (Vulkan external synchronization).
* `no_wait_under_lock` (invariant 1): a thread blocked on the GPU holds no layer lock.
* `lock_holders_never_block`: every lock holder sits in a critical section whose only exit
  step has no precondition. Therefore any wait-for-lock chain has length one and ends at a
  runnable thread, and a GPU wait can never be what a lock holder is blocked on.

Absence of deadlock itself is *not* an inductive safety property over uninterpreted batch
types: it needs well-foundedness of `dep` (the GPU dependency graph), which first-order
logic over an arbitrary sort cannot express. It is established exhaustively by `#model_check`
(a state without successors that is not terminal is reported as a deadlock) on the
DXVK instance. Together: the lock structure is proved for all instance sizes, and the
"locks never block the progress of what the GPU waits for" argument is checked by
exhaustive search on the concrete instance.

The buggy model reaches the exact deadlock state (`stuck`).
-/

veil module LockDiscipline

type thr
type batch
type queue

instantiate tot : TotalOrder batch
open TotalOrder

immutable relation presenter : thr → Bool
immutable function submitter : batch → thr
immutable function bq : batch → queue
immutable function pres_q : thr → queue
immutable relation dep : batch → batch → Bool

enum pcState = { idle, aIn, pHoldL, pReady, pInSubmit, pAwait, pAwaited, pInPresent, done }

function pc : thr → pcState
function cur : thr → batch
relation qholds : thr → queue → Bool
relation lholds : thr → Bool
relation in_call : thr → queue → Bool
relation submitted : batch → Bool
relation gpu_done : batch → Bool

#gen_state

-- The app's own semaphore graph is acyclic (otherwise the app itself would hang).
assumption [dep_acyclic] ∀ b c, dep b c → le c b ∧ c ≠ b

after_init {
  pc T := idle
  cur T := default
  qholds T Q := false
  lholds T := false
  in_call T Q := false
  submitted B := false
  gpu_done B := false
}

-- App thread: the hooked vkQueueSubmit*.
action appEnter (t : thr) (b : batch) {
  require ¬ presenter t ∧ pc t = idle
  require submitter b = t ∧ ¬ submitted b
  let q := bq b
  require ∀ u, ¬ qholds u q
  qholds t q := true
  in_call t q := true
  cur t := b
  pc t := aIn
}

action appExit (t : thr) {
  require pc t = aIn
  let b := cur t
  let q := bq b
  submitted b := true
  in_call t q := false
  qholds t q := false
  pc t := idle
}

-- Present thread.
action presLockL (t : thr) {
  require presenter t ∧ pc t = idle
  require ∀ u, ¬ lholds u
  lholds t := true
  pc t := pHoldL
}

action presUnlockL (t : thr) {
  require pc t = pHoldL
  lholds t := false
  pc t := pReady
}

action presEnterSubmit (t : thr) {
  require pc t = pReady
  let q := pres_q t
  require ∀ u, ¬ qholds u q
  qholds t q := true
  in_call t q := true
  pc t := pInSubmit
}

action presExitSubmit (t : thr) {
  require pc t = pInSubmit
  let q := pres_q t
  in_call t q := false
  qholds t q := false
  pc t := pAwait
}

-- GPU wait: enabled only when the queue is idle; the thread holds no lock here.
action presGpuWait (t : thr) {
  require pc t = pAwait
  require ∀ b, bq b = pres_q t → submitted b → gpu_done b
  pc t := pAwaited
}

action presEnterPresent (t : thr) {
  require pc t = pAwaited
  let q := pres_q t
  require ∀ u, ¬ qholds u q
  qholds t q := true
  in_call t q := true
  pc t := pInPresent
}

action presExitPresent (t : thr) {
  require pc t = pInPresent
  let q := pres_q t
  in_call t q := false
  qholds t q := false
  pc t := done
}

-- The GPU.
action gpuRun (b : batch) {
  require submitted b ∧ ¬ gpu_done b
  require ∀ c, dep b c → gpu_done c
  gpu_done b := true
}

ghost relation holds_lock (t : thr) := lholds t ∨ ∃ q, qholds t q

safety [in_call_holds_queue_mutex] in_call T Q → qholds T Q
safety [queue_call_exclusive] in_call T Q ∧ in_call U Q → T = U
safety [no_wait_under_lock] pc T = pAwait → ¬ holds_lock T
safety [lock_holders_never_block]
  holds_lock T →
    (¬ presenter T ∧ pc T = aIn) ∨
    (presenter T ∧ (pc T = pHoldL ∨ pc T = pInSubmit ∨ pc T = pInPresent))

invariant [mutex_queue] qholds T Q ∧ qholds U Q → T = U
invariant [mutex_state] lholds T ∧ lholds U → T = U
invariant [call_iff_held] in_call T Q = qholds T Q
invariant [qholds_shape]
  qholds T Q →
    (¬ presenter T ∧ pc T = aIn ∧ Q = bq (cur T)) ∨
    (presenter T ∧ (pc T = pInSubmit ∨ pc T = pInPresent) ∧ Q = pres_q T)
invariant [lholds_shape] lholds T → presenter T ∧ pc T = pHoldL
invariant [role_pc]
  (pc T = aIn → ¬ presenter T) ∧
  (pc T ≠ idle ∧ pc T ≠ aIn → presenter T)
invariant [shape_lock_backing]
  (pc T = aIn → qholds T (bq (cur T))) ∧
  ((pc T = pInSubmit ∨ pc T = pInPresent) → qholds T (pres_q T)) ∧
  (pc T = pHoldL → lholds T)

termination [all_done]
  (∀ T, presenter T → pc T = done) ∧
  (∀ B, submitted B ∧ gpu_done B)

set_option veil.smt.trust false

#gen_spec

-- DXVK instance: t0 submits b1 which waits on b0; b0 is submitted later, from t1; t2 is the present thread; a single shared queue.
#model_check { thr := Fin 3, batch := Fin 2, queue := Fin 1 }
  { presenter := fun t => t.val = 2,
    submitter := fun b => if b.val = 1 then 0 else 1,
    bq := fun _ => 0,
    pres_q := fun _ => 0,
    dep := fun b c => b.val = 1 ∧ c.val = 0 }

sat trace [present_completes] {
  presLockL
  presUnlockL
  presEnterSubmit
  presExitSubmit
  presGpuWait
  presEnterPresent
  presExitPresent
  assert (∃ t, pc t = done)
}

-- The DXVK pattern: the app submits the *waiting* batch before its signalling sibling.
sat trace [semaphore_chain_completes] {
  appEnter
  appExit
  appEnter
  appExit
  gpuRun
  gpuRun
  assert (∃ b c, dep b c ∧ gpu_done b ∧ gpu_done c)
}

-- An app submit can run while the present thread waits on the GPU (no lock is held).
sat trace [app_submit_during_gpu_wait] {
  presLockL
  presUnlockL
  presEnterSubmit
  presExitSubmit
  appEnter
  appExit
  assert (∃ t, pc t = pAwait ∧ ∃ b, submitted b)
}

#check_invariants

end LockDiscipline

/-! ## Buggy variant: `globalLock` held across the GPU wait (defect C8) -/

veil module LockDisciplineBug

type thr
type batch
type queue

instantiate tot : TotalOrder batch
open TotalOrder

immutable relation presenter : thr → Bool
immutable function submitter : batch → thr
immutable function bq : batch → queue
immutable function pres_q : thr → queue
immutable relation dep : batch → batch → Bool

enum pcState = { idle, aIn, pHoldG, pAwait, pAwaited, done }

function pc : thr → pcState
function cur : thr → batch
relation gholds : thr → Bool
relation submitted : batch → Bool
relation gpu_done : batch → Bool

#gen_state

assumption [dep_acyclic] ∀ b c, dep b c → le c b ∧ c ≠ b

after_init {
  pc T := idle
  cur T := default
  gholds T := false
  submitted B := false
  gpu_done B := false
}

-- Old app hook: `globalLock` around the call down.
action appEnter (t : thr) (b : batch) {
  require ¬ presenter t ∧ pc t = idle
  require submitter b = t ∧ ¬ submitted b
  require ∀ u, ¬ gholds u
  gholds t := true
  cur t := b
  pc t := aIn
}

action appExit (t : thr) {
  require pc t = aIn
  submitted (cur t) := true
  gholds t := false
  pc t := idle
}

-- Old `QueuePresentKHR`: take globalLock, submit layer work, queue-idle wait, present.
action presLockG (t : thr) {
  require presenter t ∧ pc t = idle
  require ∀ u, ¬ gholds u
  gholds t := true
  pc t := pHoldG
}

action presSubmit (t : thr) {
  require pc t = pHoldG
  pc t := pAwait
}

-- BUG: waits for the GPU while holding globalLock.
action presGpuWait (t : thr) {
  require pc t = pAwait
  require ∀ b, bq b = pres_q t → submitted b → gpu_done b
  pc t := pAwaited
}

action presPresent (t : thr) {
  require pc t = pAwaited
  gholds t := false
  pc t := done
}

action gpuRun (b : batch) {
  require submitted b ∧ ¬ gpu_done b
  require ∀ c, dep b c → gpu_done c
  gpu_done b := true
}

-- Every action is disabled: the present thread waits on the GPU holding globalLock, the
-- batch it waits for depends on a batch whose submitter is blocked on that lock.
ghost relation stuck :=
  (∃ t, presenter t ∧ pc t = pAwait ∧ gholds t) ∧
  (∀ t, ¬ presenter t → pc t = idle) ∧
  (∀ t, presenter t → pc t = idle ∨ pc t = pAwait ∨ pc t = done) ∧
  (∀ t b, ¬ presenter t → submitter b = t → ¬ submitted b → ∃ u, gholds u) ∧
  (∀ b, submitted b → ¬ gpu_done b → ∃ c, dep b c ∧ ¬ gpu_done c) ∧
  (∀ t, presenter t → pc t = pAwait → ∃ b, bq b = pres_q t ∧ submitted b ∧ ¬ gpu_done b) ∧
  (∃ t b, ¬ presenter t ∧ submitter b = t ∧ ¬ submitted b)

invariant [mutex_global] gholds T ∧ gholds U → T = U

termination [all_done]
  (∀ T, presenter T → pc T = done) ∧ (∀ B, submitted B ∧ gpu_done B)

#gen_spec

-- The same DXVK instance, exhaustively: the old design has a reachable deadlock (a state
-- without successors that is not terminal). `violationIsError false` keeps the build green
-- because the finding is the point; the `sat trace` below is what must stay satisfiable.
set_option veil.violationIsError false in
#model_check { thr := Fin 3, batch := Fin 2, queue := Fin 1 }
  { presenter := fun t => t.val = 2,
    submitter := fun b => if b.val = 1 then 0 else 1,
    bq := fun _ => 0,
    pres_q := fun _ => 0,
    dep := fun b c => b.val = 1 ∧ c.val = 0 }

sat trace [old_present_deadlocks_with_app_submit] {
  appEnter
  appExit
  presLockG
  presSubmit
  assert stuck
}

end LockDisciplineBug

/-! ## Buggy variant: layer submits on a queue without its mutex (invariant 2, defect H3) -/

veil module LockDisciplineBugUnlockedQueue

type thr
type queue

relation qholds : thr → queue → Bool
relation in_call : thr → queue → Bool

#gen_state

after_init {
  qholds T Q := false
  in_call T Q := false
}

action appEnter (t : thr) (q : queue) {
  require ∀ u, ¬ qholds u q
  qholds t q := true
  in_call t q := true
}

-- BUG: the layer's own vkQueueSubmit (overlay / optical flow / present) does not take the
-- queue mutex of the queue it was handed.
action layerSubmitUnlocked (t : thr) (q : queue) {
  in_call t q := true
}

invariant [call_holds_mutex] in_call T Q → qholds T Q

#gen_spec

sat trace [concurrent_queue_calls] {
  appEnter
  layerSubmitUnlocked
  assert (∃ t u q, t ≠ u ∧ in_call t q ∧ in_call u q)
}

end LockDisciplineBugUnlockedQueue
