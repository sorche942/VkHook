module

public import Veil

/-!
# PersistenceQueue: one ordered writer for every file mutation

Protects AGENTS.md invariant 12 and defects H18 (file writes came from two unordered threads,
so a deleted preset could be resurrected) and H16 (every autosave triggered a full reload:
the watcher mistook VkHook's own write for an external edit).

## Protocol (target `config/persist`)

Every mutation of a user file (save, delete, import, settings) is `issue`d to a single
`PersistenceQueue`. The queue holds at most one entry per path (per-path coalescing: a newer
operation replaces the queued one) and a single writer thread applies entries one at a time;
each application is an atomic replace (temp file + rename). Before touching the disk the writer
*registers the signature of the write* (`own_set`); the config watcher, on a file event, reads
the file's current signature and treats the change as external iff it is not registered.
Registration happens before the write, and registered signatures are never un-registered by
the event handler (a second event for the same write must still be recognised).

| model action             | C++ (target)                                                         |
|--------------------------|----------------------------------------------------------------------|
| `issue o p k v`          | `PersistenceQueue::save / remove / ...(path, ...)` (any thread)      |
| `writerTake p`           | writer thread pops a path's entry; `registerOwnWrite(signature)`     |
| `writerApply`            | writer thread performs the atomic replace / unlink                   |
| `externalWrite o p v`    | another program (the user's editor) rewrites the file                |
| `watcherHandle p`        | config watcher callback (inotify / polling) for path `p`             |
| `drainAtLastInstance`    | `VkHook_DestroyInstance` (`layer/instance.cc`), last instance gone:  |
|                          | `PersistenceQueue::waitForIdle()`; returns once the queue is empty   |
|                          | and no job is running                                                |
| `shutdown`               | `PersistenceQueue::shutdown()`: process teardown only; rejects every |
|                          | later `submit`, still runs what is already queued                     |

## Drain at the last instance, and restart

At the last `vkDestroyInstance` the layer *drains* the queue (`waitForIdle`): every queued job
runs and none is dropped, but the writer is **not** shut down. There is no separate restart
operation in C++ and none is needed: the queue never stopped, so the next `vkCreateInstance`
simply submits again and the same writer thread serves it. The model states exactly that:
`drainAtLastInstance` is enabled only once the queue is drained (the wait has returned) and does
not touch `stopped`; `issue` is enabled whenever `¬ stopped`, so after a drain it is enabled
again (`sat trace restart_after_drain`). Only `shutdown` sets `stopped`.

## Proved (`#check_invariants`, all sizes)

* `drained_matches_last_issued`: once the queue is drained, every path that has not been
  edited externally since holds exactly the effect of the last operation issued for it
  (content for a save, absence for a delete).
* `own_write_never_external`: the watcher never flags a write made by VkHook as external.
* `drain_leaves_queue_drained` / `drain_does_not_stop`: from the drain until the next `issue` or
  `shutdown`, the queue is drained (all queued work ran; `drained_matches_last_issued` then says
  none was dropped) and the drain itself did not stop it (`stopped` is what it was before).

## Buggy variant `PersistenceQueueBug` (old behaviour)

* H18: the UI thread deletes the file synchronously while an autosave for it sits in the queue;
  the writer then recreates it (`sat trace delete_resurrected`).
* H16: the old watcher suppressed events with a `self_writing` flag that the writer clears right
  after its write, but the filesystem event is delivered *later* (`sat trace own_write_reloaded`).

## Notes on the target design

* The model lets the writer pick any pending path, a superset of FIFO order. Per-path final
  state is order-independent across paths. If a multi-path operation (rename, import bundle)
  is added, its steps must be one queue entry, because cross-path ordering is *not* modelled.
* `externalWrite` followed by an own write overwrites the user's edit: that is the policy of
  "rewrite only keys VkHook owns" (invariant 12), implemented by read-modify-write inside the
  writer, which is outside this model.
-/

veil module PersistenceQueue

type path
type val
type op

enum kindT = { kSave, kDel }

function op_kind : op → kindT
function op_path : op → path
function op_val : op → val
relation op_used : op → Bool
relation op_ext : op → Bool

relation has_last : path → Bool
function last_op : path → op

relation pending : path → Bool
function pending_op : path → op
individual busy : Bool
individual w_op : op

relation own_set : op → Bool

-- `stopped`: shutdown() ran, submits are rejected. `just_drained`: the last-instance drain
-- returned and nothing has been submitted or shut down since.
individual stopped : Bool
individual just_drained : Bool
-- `stopped` as it was when the drain ran: the drain itself must not change it.
individual stopped_at_drain : Bool

relation exists_d : path → Bool
function disk_val : path → val
function disk_sig : path → op
relation ext_dirty : path → Bool
relation evt : path → Bool
relation ext_flagged : op → Bool

#gen_state

after_init {
  op_kind O := kSave
  op_path O := default
  op_val O := default
  op_used O := false
  op_ext O := false
  has_last P := false
  last_op P := default
  pending P := false
  pending_op P := default
  busy := false
  w_op := default
  own_set O := false
  stopped := false
  just_drained := false
  stopped_at_drain := false
  exists_d P := false
  disk_val P := default
  disk_sig P := default
  ext_dirty P := false
  evt P := false
  ext_flagged O := false
}

-- Any thread issues a mutation; it replaces a queued entry of the same path (coalescing).
action issue (o : op) (p : path) (k : kindT) (v : val) {
  require ¬ op_used o
  require ¬ stopped
  just_drained := false
  op_used o := true
  op_kind o := k
  op_path o := p
  op_val o := v
  has_last p := true
  last_op p := o
  pending p := true
  pending_op p := o
}

-- The single writer takes an entry and registers the write's signature before touching disk.
action writerTake (p : path) {
  require ¬ busy ∧ pending p
  busy := true
  w_op := pending_op p
  own_set (pending_op p) := true
  pending p := false
}

action writerApply {
  require busy
  let o := w_op
  let p := op_path o
  if op_kind o = kSave then
    exists_d p := true
    disk_val p := op_val o
  else
    exists_d p := false
  disk_sig p := o
  ext_dirty p := false
  evt p := true
  busy := false
}

-- Last vkDestroyInstance: waitForIdle() returns once the queue is empty and idle. The writer
-- keeps running; `stopped` is untouched.
action drainAtLastInstance {
  require ¬ busy ∧ (∀ p, ¬ pending p)
  just_drained := true
  stopped_at_drain := stopped
}

-- Process teardown: no further submits are accepted; queued work still runs.
action shutdown {
  stopped := true
  just_drained := false
}

-- Someone else edits the file.
action externalWrite (o : op) (p : path) (v : val) {
  require ¬ op_used o
  op_used o := true
  op_ext o := true
  op_path o := p
  exists_d p := true
  disk_val p := v
  disk_sig p := o
  ext_dirty p := true
  evt p := true
}

-- Watcher: external iff the current signature was not registered by the writer.
action watcherHandle (p : path) {
  require evt p
  evt p := false
  let s := disk_sig p
  if ¬ own_set s then
    ext_flagged s := true
}

ghost relation drained := ¬ busy ∧ (∀ p, ¬ pending p)

ghost relation effect_holds (p : path) (o : op) :=
  (op_kind o = kSave ∧ exists_d p ∧ disk_val p = op_val o) ∨
  (op_kind o = kDel ∧ ¬ exists_d p)

safety [drained_matches_last_issued]
  drained → has_last P → ¬ ext_dirty P → effect_holds P (last_op P)
safety [own_write_never_external] ext_flagged X → op_ext X
safety [drain_leaves_queue_drained] just_drained → drained
safety [drain_does_not_stop] just_drained → (stopped ↔ stopped_at_drain)

invariant [last_op_wellformed]
  has_last P → op_used (last_op P) ∧ ¬ op_ext (last_op P) ∧ op_path (last_op P) = P
invariant [queued_is_latest]
  pending P → has_last P ∧ pending_op P = last_op P
invariant [inflight_wellformed]
  busy → op_used w_op ∧ ¬ op_ext w_op ∧ own_set w_op ∧ has_last (op_path w_op) ∧
    (¬ pending (op_path w_op) → last_op (op_path w_op) = w_op)
invariant [settled_matches_last]
  has_last P ∧ ¬ pending P ∧ ¬ (busy ∧ op_path w_op = P) ∧ ¬ ext_dirty P →
    effect_holds P (last_op P)
invariant [event_sig_known]
  evt P → op_used (disk_sig P) ∧ op_path (disk_sig P) = P ∧
    (¬ op_ext (disk_sig P) → own_set (disk_sig P))
invariant [own_set_not_external] own_set O → ¬ op_ext O
invariant [flags_only_used_ops] (op_ext O → op_used O) ∧ (own_set O → op_used O)

set_option veil.smt.trust false

#gen_spec

#model_check { path := Fin 2, val := Fin 2, op := Fin 3 }

sat trace [save_applied_and_ignored_by_watcher] {
  issue
  writerTake
  writerApply
  watcherHandle
  assert (∃ p, exists_d p ∧ ¬ evt p ∧ (∀ o, ¬ ext_flagged o))
}

sat trace [coalesced_save_then_delete] {
  issue
  issue
  writerTake
  writerApply
  assert (∃ p o, has_last p ∧ last_op p = o ∧ ¬ pending p ∧ op_path o = p)
}

-- Drain-at-last-instance: the queued save lands, nothing is left, the queue is not stopped.
sat trace [drain_runs_queued_jobs] {
  issue
  writerTake
  writerApply
  drainAtLastInstance
  assert (just_drained ∧ ¬ stopped ∧ (∃ p, exists_d p))
}

-- Restart: a job submitted after the drain (the next instance) is accepted and applied.
sat trace [restart_after_drain] {
  issue
  writerTake
  writerApply
  drainAtLastInstance
  issue
  writerTake
  writerApply
  drainAtLastInstance
  assert (just_drained ∧ ¬ stopped ∧ (∃ o1 o2, o1 ≠ o2 ∧ op_used o1 ∧ op_used o2))
}

-- shutdown (not drain) is what rejects submits.
sat trace [shutdown_rejects_after_drain] {
  drainAtLastInstance
  shutdown
  assert (stopped ∧ ¬ just_drained)
}

sat trace [external_edit_detected] {
  externalWrite
  watcherHandle
  assert (∃ o, ext_flagged o)
}

#check_invariants

end PersistenceQueue

/-! ## Buggy variant: synchronous delete racing a queued autosave (H18), flag-based watcher (H16) -/

veil module PersistenceQueueBug

type path
type val
type op

enum kindT = { kSave, kDel }

function op_kind : op → kindT
function op_path : op → path
function op_val : op → val
relation op_used : op → Bool
relation op_ext : op → Bool

relation has_last : path → Bool
function last_op : path → op

relation pending : path → Bool
function pending_op : path → op
individual busy : Bool
individual w_op : op

relation self_writing : path → Bool

relation exists_d : path → Bool
function disk_val : path → val
function disk_sig : path → op
relation evt : path → Bool
relation ext_flagged : op → Bool

#gen_state

after_init {
  op_kind O := kSave
  op_path O := default
  op_val O := default
  op_used O := false
  op_ext O := false
  has_last P := false
  last_op P := default
  pending P := false
  pending_op P := default
  busy := false
  w_op := default
  self_writing P := false
  exists_d P := false
  disk_val P := default
  disk_sig P := default
  evt P := false
  ext_flagged O := false
}

-- Autosave: queued for the writer thread.
action issueSave (o : op) (p : path) (v : val) {
  require ¬ op_used o
  op_used o := true
  op_kind o := kSave
  op_path o := p
  op_val o := v
  has_last p := true
  last_op p := o
  pending p := true
  pending_op p := o
}

-- BUG: the delete runs synchronously on the calling thread, outside the queue, and does not
-- cancel the queued autosave of the same path.
action syncDelete (o : op) (p : path) {
  require ¬ op_used o
  op_used o := true
  op_kind o := kDel
  op_path o := p
  has_last p := true
  last_op p := o
  exists_d p := false
}

action writerTake (p : path) {
  require ¬ busy ∧ pending p
  busy := true
  w_op := pending_op p
  self_writing p := true
  pending p := false
}

action writerApply {
  require busy
  let o := w_op
  let p := op_path o
  exists_d p := true
  disk_val p := op_val o
  disk_sig p := o
  evt p := true
}

-- BUG: the suppression flag is dropped right after the write, but the file event arrives later.
action writerFinish {
  require busy
  let p := op_path w_op
  self_writing p := false
  busy := false
}

action watcherHandle (p : path) {
  require evt p
  evt p := false
  if ¬ self_writing p then
    ext_flagged (disk_sig p) := true
}

invariant [busy_has_op] busy → op_used w_op

#gen_spec

-- H18: delete issued after an autosave was queued; the file ends up existing although the
-- last issued operation was the delete, and the queue is drained.
sat trace [delete_resurrected] {
  issueSave
  syncDelete
  writerTake
  writerApply
  writerFinish
  assert (∃ p, has_last p ∧ op_kind (last_op p) = kDel ∧ exists_d p ∧ ¬ busy ∧ (∀ q, ¬ pending q))
}

-- H16: VkHook's own write is reported as an external change.
sat trace [own_write_reloaded] {
  issueSave
  writerTake
  writerApply
  writerFinish
  watcherHandle
  assert (∃ o, ext_flagged o ∧ ¬ op_ext o)
}

end PersistenceQueueBug
