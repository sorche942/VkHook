module

public import Veil

/-!
# RetireQueue: deferred destruction keyed on per-device GPU serials

Protects AGENTS.md invariant 3 and defect H6 (`DeferredDestroyQueue` was process-global
and untagged, and `panicLayer` flushed it without waiting).

Target C++ (`gpu/timeline`, `gpu/retire`): every device has a timeline with a *submitted*
serial and a fence-backed *completed* serial. A layer object that may still be read by the
GPU is not destroyed; it is *retired* with the device's submitted serial at that moment and
destroyed by `collect` once `completedSerial() >= tag`.

| model action    | C++ (target)                                                                  |
|-----------------|-------------------------------------------------------------------------------|
| `submit d s o`  | any `vkQueueSubmit*` on device `d` (app hook or layer): `Timeline::nextSerial()` |
|                 | and the submission references object `o` (layer-owned image, view, ...)       |
| `gpuComplete d`    | `Timeline::completedSerial()` advances: fence/timeline-semaphore signal       |
|                 | (monotone, in order, one serial at a time)                                    |
| `retire d o`    | `RetireQueue::retire(type, handle)`: tag with the *current* submitted serial  |
| `collect d o`   | `RetireQueue::collect(completed)` run at present: destroy entries with        |
|                 | `tag <= completed`                                                            |
| `waitIdle d`    | `vkDeviceWaitIdle` in `vkDestroyDevice`: completed := submitted               |
| `drainAll d`    | `RetireQueue::drainAll()` + destroying remaining layer objects, only after    |
|                 | `waitIdle`; the device is dead afterwards                                     |

Safety (`no_destroyed_in_flight`): no object referenced by a submission that has not
completed is destroyed. The inductive invariant is `retire_tag_covers_users`: once an object
is retired, every submission that references it has serial `<=` its tag; so `tag <= completed`
implies all its users completed.

Buggy variants (same file, separate modules): `RetireQueueBugPanic` (old `panicLayer`:
flush without a GPU wait) and `RetireQueueBugGlobal` (old process-global queue: a flush
after idling device A destroys device B's retired objects). Their `sat trace`s must stay
satisfiable: the build fails if the bug becomes unreachable.
-/

veil module RetireQueue

type dev
type obj
type sub

/- Owning device of every layer object (theory). -/
immutable function owner : obj → dev

relation created : obj → Bool
relation retired : obj → Bool
relation destroyed : obj → Bool
function retire_tag : obj → Nat


function submitted : dev → Nat
function completed : dev → Nat
relation drained : dev → Bool

relation sub_used : sub → Bool
function sub_dev : sub → dev
function sub_serial : sub → Nat
relation refs : sub → obj → Bool

#gen_state

after_init {
  created O := false
  retired O := false
  destroyed O := false
  retire_tag O := 0
  submitted D := 0
  completed D := 0
  drained D := false
  sub_used S := false
  sub_dev S := default
  sub_serial S := 0
  refs S O := false
}

action create (o : obj) {
  require ¬ created o
  created o := true
}

/- App or layer submit on device `d` referencing live (created, not retired) object `o`. -/
action submit (d : dev) (s : sub) (o : obj) {
  require ¬ sub_used s
  require ¬ drained d
  require created o ∧ ¬ retired o ∧ ¬ destroyed o
  require owner o = d
  require submitted d < 3
  let n := submitted d + 1
  submitted d := n
  sub_used s := true
  sub_dev s := d
  sub_serial s := n
  refs s o := true
}

/- The GPU finishes the next submission on `d` (monotone, in order). -/
action gpuComplete (d : dev) {
  require completed d < submitted d
  completed d := completed d + 1
}

action retire (o : obj) {
  require created o ∧ ¬ retired o
  retired o := true
  retire_tag o := submitted (owner o)
}

action collect (o : obj) {
  require retired o ∧ ¬ destroyed o
  require retire_tag o ≤ completed (owner o)
  destroyed o := true
}

action waitIdle (d : dev) {
  completed d := submitted d
}

action drainAll (d : dev) {
  require completed d = submitted d
  drained d := true
  destroyed O := destroyed O ∨ owner O = d
}

ghost relation incomplete (s : sub) :=
  sub_used s ∧ completed (sub_dev s) < sub_serial s

/- Invariant 3: nothing the GPU may still read is destroyed. -/
safety [no_destroyed_in_flight] incomplete S ∧ refs S O → ¬ destroyed O

invariant [counters_ordered] completed D ≤ submitted D
invariant [serial_assigned] sub_used S → 0 < sub_serial S ∧ sub_serial S ≤ submitted (sub_dev S)
invariant [refs_owner] refs S O → sub_used S ∧ owner O = sub_dev S ∧ created O
invariant [retire_tag_covers_users]
  refs S O ∧ retired O → sub_serial S ≤ retire_tag O
invariant [retire_tag_bound] retired O → retire_tag O ≤ submitted (owner O)
invariant [destroyed_means_safe]
  destroyed O → drained (owner O) ∨ (retired O ∧ retire_tag O ≤ completed (owner O))
invariant [drained_idle] drained D → completed D = submitted D

set_option veil.smt.trust false

#gen_spec

#model_check { dev := Fin 2, obj := Fin 3, sub := Fin 3 }
  { owner := fun o => if o.val = 0 then 0 else 1 }

sat trace [object_destroyed_after_use] {
  create
  submit
  retire
  gpuComplete
  collect
  assert (∃ o, destroyed o)
}

sat trace [drain_after_idle] {
  create
  submit
  waitIdle
  drainAll
  assert (∃ d, drained d)
}

#check_invariants

end RetireQueue

/-! ## Buggy variant (a): old `panicLayer`, flush without waiting -/

veil module RetireQueueBugPanic

type dev
type obj
type sub

immutable function owner : obj → dev

relation created : obj → Bool
relation retired : obj → Bool
relation destroyed : obj → Bool
function retire_tag : obj → Nat
function submitted : dev → Nat
function completed : dev → Nat
relation sub_used : sub → Bool
function sub_dev : sub → dev
function sub_serial : sub → Nat
relation refs : sub → obj → Bool

#gen_state

after_init {
  created O := false
  retired O := false
  destroyed O := false
  retire_tag O := 0
  submitted D := 0
  completed D := 0
  sub_used S := false
  sub_dev S := default
  sub_serial S := 0
  refs S O := false
}

action create (o : obj) {
  require ¬ created o
  created o := true
}

action submit (d : dev) (s : sub) (o : obj) {
  require ¬ sub_used s
  require created o ∧ ¬ retired o ∧ ¬ destroyed o
  require owner o = d
  require submitted d < 3
  let n := submitted d + 1
  submitted d := n
  sub_used s := true
  sub_dev s := d
  sub_serial s := n
  refs s o := true
}

action gpuComplete (d : dev) {
  require completed d < submitted d
  completed d := completed d + 1
}

action retire (o : obj) {
  require created o ∧ ¬ retired o
  retired o := true
  retire_tag o := submitted (owner o)
}

/- BUG: `panicLayer` flushed the deferred-destroy queue without waiting for the GPU. -/
action panicFlush {
  destroyed O := destroyed O ∨ retired O
}

invariant [counters_ordered] completed D ≤ submitted D

#gen_spec

/- Reaches the violation: an in-flight submission references a destroyed object. -/
sat trace [panic_flush_destroys_in_flight_object] {
  create
  submit
  retire
  panicFlush
  assert (∃ s o, sub_used s ∧ completed (sub_dev s) < sub_serial s ∧ refs s o ∧ destroyed o)
}

end RetireQueueBugPanic

/-! ## Buggy variant (b): process-global queue, flush after idling one device -/

veil module RetireQueueBugGlobal

type dev
type obj
type sub

immutable function owner : obj → dev

relation created : obj → Bool
relation retired : obj → Bool
relation destroyed : obj → Bool
function retire_tag : obj → Nat
function submitted : dev → Nat
function completed : dev → Nat
relation sub_used : sub → Bool
function sub_dev : sub → dev
function sub_serial : sub → Nat
relation refs : sub → obj → Bool

#gen_state

after_init {
  created O := false
  retired O := false
  destroyed O := false
  retire_tag O := 0
  submitted D := 0
  completed D := 0
  sub_used S := false
  sub_dev S := default
  sub_serial S := 0
  refs S O := false
}

action create (o : obj) {
  require ¬ created o
  created o := true
}

action submit (d : dev) (s : sub) (o : obj) {
  require ¬ sub_used s
  require created o ∧ ¬ retired o ∧ ¬ destroyed o
  require owner o = d
  require submitted d < 3
  let n := submitted d + 1
  submitted d := n
  sub_used s := true
  sub_dev s := d
  sub_serial s := n
  refs s o := true
}

action gpuComplete (d : dev) {
  require completed d < submitted d
  completed d := completed d + 1
}

action retire (o : obj) {
  require created o ∧ ¬ retired o
  retired o := true
  retire_tag o := submitted (owner o)
}

action waitIdle (d : dev) {
  completed d := submitted d
}

/- BUG: the destroy queue was process-global and untagged. A flush that is safe for the
    device that just idled destroys every device's retired objects. -/
action globalFlush (d : dev) {
  require completed d = submitted d
  destroyed O := destroyed O ∨ retired O
}

invariant [counters_ordered] completed D ≤ submitted D

#gen_spec

/- Reaches the violation: device 0 idles and flushes, destroying device 1's object that an
    incomplete submission on device 1 still references. -/
sat trace [global_flush_destroys_other_devices_object] {
  create
  create
  submit
  retire
  waitIdle
  globalFlush
  assert (∃ s o, sub_used s ∧ completed (sub_dev s) < sub_serial s ∧ refs s o ∧ destroyed o)
}

end RetireQueueBugGlobal
