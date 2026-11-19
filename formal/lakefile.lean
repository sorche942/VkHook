import Lake
open Lake DSL System

/-- Veil checkout. Override with `VEIL_HOME`; defaults to `~/Development/veil`. -/
def veilHome : FilePath :=
  run_io do
    if let some p ← IO.getEnv "VEIL_HOME" then return p
    let home := (← IO.getEnv "HOME").getD "/"
    return (home : FilePath) / "Development" / "veil"

require veil from veilHome

package vkhookFormal where
  -- Reuse Veil's already-built dependencies (lean-smt, Loom, cvc5, …) instead of
  -- cloning and rebuilding them per checkout.
  packagesDir := veilHome / ".lake" / "packages"

@[default_target]
lean_lib VkHookFormal where
