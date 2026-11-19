# formal/ — Veil models of VkHook's protocols

Lean 4 + [Veil](https://github.com/verse-lab/veil) models of the layer's concurrency and
lifetime protocols. The models are the specification that stage-2 C++ follows; see
`AGENTS.md` §4 (invariants) and §6 (rules).

## Build

```bash
just formal            # = cd formal && lake build
```

Needs a built Veil checkout at `~/Development/veil` (override with `VEIL_HOME`); the package
reuses Veil's `.lake/packages`. `lake-manifest.json` and `.lake/` are gitignored. A full build
takes about 30–60 s: each file runs an exhaustive `#model_check` of a small instance, a
`#check_invariants` proof for all instance sizes, and `sat trace` queries. Every file starts with
`module` / `public import Veil` (compiled model checks need the module system). Plain
`/-- … -/` doc comments cannot precede Veil commands; use `/- … -/`. In an action, later
statements see earlier assignments, so read a pre-state value into a `let` first.

## Models ↔ C++

| Model (`VkHookFormal/…`) | Protects | C++ (target, AGENTS.md §3) | Old defects shown reachable |
|---|---|---|---|
| `RetireQueue` | inv. 3 | `gpu/retire`, `gpu/timeline` | H6: `panicLayer` flush without wait; process-global queue |
| `LockDiscipline` | inv. 1–2 | `gpu/queue`, every hook, `present/present` | C8: GPU wait under `globalLock` deadlocks with app semaphore chains; H3: layer submit without the queue mutex |
| `PresentModes` | inv. 5 | `present/present` | C2: bypass presents the never-rendered real image; present with zero wait semaphores |
| `ChainRebuild` | inv. 4, 6 | `present/swapchain`, `effects/graph`, `depth/tracker` (storage lifetime) | C4: layer objects recorded into app command buffers outlive their generation |
| `SyncobjGate` | Wayland commit/present exclusion | `input/wayland_syncobj_gate` | H24: check-then-act; wait-budget escape |
| `PersistenceQueue` | inv. 12; drain at the last `vkDestroyInstance` (queued jobs all run, writer not shut down) and restart (a new instance's jobs are accepted again) | `config/persist`, `layer/instance` | H18: sync delete resurrected by queued autosave; H16: own write seen as external |

Each file's header comment maps every model action to the C++ function that performs it.

## Rules (AGENTS.md §6)

Each protocol has a fixed model (the C++ as implemented or targeted) with `safety`/`invariant`
clauses, a `#model_check` on a small instance and a `#check_invariants` that closes the
inductive invariant, `sat trace` non-vacuity checks, and, where the old code was buggy, a
separate `veil module …Bug` encoding the old behaviour whose `sat trace` reaches the violating
state, so the build fails if the bug becomes unreachable. A change to a modeled protocol updates
the model first and `just formal` must stay green; where a C++ component implements a model, a unit
test drives it with random action sequences and checks the model's safety clauses after every
step; prefer kernel-checked proofs (`set_option veil.smt.trust false`, used in every fixed model
here) and, where reconstruction fails, leave the SMT-trusted result with a comment saying why.
