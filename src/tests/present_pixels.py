#!/usr/bin/env python3
"""Driver for present-pixels: what reaches the screen when the layer swaps the swapchain images.

usage: present_pixels.py PRESENT_PIXELS_BINARY LIBVKHOOK_SO [--no-recreate] [VARIANT...]

--no-recreate makes the app keep its swapchain after a present reports SUBOPTIMAL
(an app may); the layer must keep processing frames either way.

Variants (default: all):
  copy-off     no effect, depthCapture off        -> plain copy path, colour unchanged
  copy-on      no effect, depthCapture on         -> same with the depth hold armed
  invert-off   DebugInvert, depthCapture off      -> C2 primary case, colour inverted
  invert-on    DebugInvert, depthCapture on       -> colour inverted

Exit codes: 0 pass, 1 fail, 77 skip (no display / no usable Vulkan device).

The layer is loaded the way smoke_vkcube.sh loads it: a manifest of its own under a
layer name nothing else uses, VK_LOADER_LAYERS_ENABLE, ENABLE_VKHOOK=1 and a seeded
XDG_CONFIG_HOME. The run fails unless the loader says it loaded exactly that library.
Verification of the pixels is done by the app through the X server, not by the layer.

Under Wayland-only sessions the test unsets WAYLAND_DISPLAY and uses X11 (Xwayland, or
Xvfb when no display exists at all); there is no layer-independent way to read back a
native Wayland surface.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ASSETS = SCRIPT_DIR.parent.parent / "assets" / "builtInShaders"
FIXTURES = SCRIPT_DIR / "fixtures"
LAYER_NAME = "VK_LAYER_VKHOOK_smoke_test"
INSTALLED_LAYER = "VK_LAYER_VKHOOK_post_processing"
SKIP = 77

VARIANTS = {
    # name: (depthCapture, effect, expected colour handling)
    "copy-off": ("off", None, "identity"),
    "copy-on": ("on", None, "identity"),
    "invert-off": ("off", "DebugInvert", "invert"),
    "invert-on": ("on", "DebugInvert", "invert"),
}


def seed(work, library, variant, game):
    depth, effect, _ = VARIANTS[variant]
    layers = work / "layer"
    config = work / "config" / "VkHook"
    (config / "configs" / "shaders").mkdir(parents=True)
    (config / "reshade" / "packages" / "Shaders").mkdir(parents=True)
    layers.mkdir()
    for d in ("home", "data", "cache"):
        (work / d).mkdir()
    manifest = {"file_format_version": "1.0.0", "layer": {
        "name": LAYER_NAME, "type": "GLOBAL", "library_path": str(library),
        "api_version": "1.3.223", "implementation_version": "1",
        "description": "vkhook present pixels",
        "functions": {"vkGetInstanceProcAddr": "VkHook_GetInstanceProcAddr",
                      "vkGetDeviceProcAddr": "VkHook_GetDeviceProcAddr"},
        "enable_environment": {"ENABLE_VKHOOK": "1"},
        "disable_environment": {"DISABLE_VKHOOK": "1"}}}
    (layers / (LAYER_NAME + ".json")).write_text(json.dumps(manifest))
    # the default autoApplyDelay is kept short so the effect is live well inside the run
    (config / "VkHook.conf").write_text(
        "enableOnLaunch = true\noverlayKey = Home\nreloadKey = F10\nmaxEffects = 10\n"
        f"autoApply = true\nautoApplyDelay = 200\ndepthCapture = {depth}\n"
        "showDebugWindow = false\ndepthResolveMode = 0\ndepthManualPin =\n"
        "depthTransientWorkaround = true\n"
        "depthInvert = false\n")
    if effect:
        shaders = config / "reshade" / "packages" / "Shaders"
        shutil.copy(FIXTURES / "ReShade.fxh", shaders / "ReShade.fxh")
        shutil.copy(ASSETS / (effect + ".fx"), shaders / (effect + ".fx"))
        (config / "configs" / (game + ".conf")).write_text(f"{effect} = {effect}.fx\neffects = {effect}\n")
        (config / "configs" / "shaders" / (game + "@pixels.ini")).write_text(
            f"Techniques={effect}@{effect}.fx\nTechniqueSorting={effect}@{effect}.fx\n\n[{effect}.fx]\n")
        (config / "configs" / "shaders" / (game + ".last-profile")).write_text("pixels\n")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("VKHOOK_", "VK_LAYER_", "VK_ADD_LAYER_", "VK_LOADER_LAYERS_"))
           and k not in ("VK_INSTANCE_LAYERS", "ENABLE_VKHOOK", "DISABLE_VKHOOK", "WAYLAND_DISPLAY")}
    env.update(HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "config"),
               XDG_DATA_HOME=str(work / "data"), XDG_CACHE_HOME=str(work / "cache"),
               VK_LAYER_PATH=str(layers), VK_LOADER_LAYERS_ENABLE=LAYER_NAME,
               VK_LOADER_LAYERS_DISABLE=INSTALLED_LAYER, ENABLE_VKHOOK="1",
               VK_LOADER_DEBUG="layer", VKHOOK_LOG_LEVEL="debug")
    authority = Path(os.environ.get("HOME", "")) / ".Xauthority"
    if "XAUTHORITY" not in env and authority.is_file():
        env["XAUTHORITY"] = str(authority.resolve())
    return env, config


def run_variant(binary, library, variant, work_root, app_args=()):
    depth, effect, expect = VARIANTS[variant]
    work = Path(tempfile.mkdtemp(prefix=f"vkhook-pixels-{variant}-", dir=work_root))
    env, config = seed(work, library, variant, binary.name)
    try:
        proc = subprocess.run([str(binary), "--expect", expect, *app_args], env=env, capture_output=True, text=True,
                              timeout=90)
    except subprocess.TimeoutExpired as e:
        print(f"  x {variant}: timed out (artifacts {work})")
        return 1
    out = proc.stdout + proc.stderr
    (work / "app.log").write_text(out)

    def bad(msg):
        print(f"  x {variant}: {msg}\n    artifacts kept: {work}")
        shown = 0
        for line in out.splitlines():
            # loader chatter and the frames that were fine are not evidence
            if line.startswith("[Vulkan Loader]") or (line.startswith("FRAME") and line.endswith(" ok")):
                continue
            if shown < 40:
                print("    | " + line[:200])
            shown += 1
        return 1

    if proc.returncode == SKIP:
        print(f"  - {variant}: skipped ({next((l for l in out.splitlines() if l.startswith('SKIP')), 'skip')})")
        shutil.rmtree(work, ignore_errors=True)
        return SKIP
    if "RESULT" not in out and proc.returncode != 0:
        return bad(f"app died with status {proc.returncode} before reporting")
    if str(library) not in out:
        return bad(f"the loader never loaded {library}; an installed copy may be shadowing it")
    layer_log = config / "vkhook.log"
    if not layer_log.is_file() or layer_log.stat().st_size == 0:
        return bad("layer loaded but wrote no log; it never initialised")
    text = layer_log.read_text(errors="replace")
    if "VkHook panic" in text:
        return bad("the layer panicked during the run")
    if effect and not re.search(rf"\[effect-built\] {re.escape(effect)}\.fx\|{re.escape(effect)} passes=[1-9]", text):
        return bad(f"{effect} was never built, the run exercised the pass-through path")
    if proc.returncode != 0 or "RESULT PASS" not in out:
        return bad("pixel check failed: what the X server shows is not what the app rendered")
    summary = next(l for l in out.splitlines() if l.startswith("SUMMARY"))
    print(f"  ok {variant}: {summary}")
    shutil.rmtree(work, ignore_errors=True)
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    binary = Path(argv[1]).resolve()
    library = Path(argv[2]).resolve()
    app_args = [a for a in argv[3:] if a == "--no-recreate"]
    names = [a for a in argv[3:] if a not in app_args] or list(VARIANTS)
    for n in names:
        if n not in VARIANTS:
            print(f"unknown variant {n}; known: {', '.join(VARIANTS)}")
            return 2
    for p in (binary, library):
        if not p.is_file():
            print(f"missing: {p}")
            return 1

    if not os.environ.get("DISPLAY"):
        # No X display (headless CI, or a Wayland-only session without Xwayland): provide one.
        if not os.environ.get("VKHOOK_PIXELS_XVFB") and shutil.which("xvfb-run"):
            env = dict(os.environ, VKHOOK_PIXELS_XVFB="1")
            os.execvpe("xvfb-run", ["xvfb-run", "-a", sys.executable, __file__, *argv[1:]], env)
        print("SKIP: no X11 display and no xvfb-run")
        return SKIP

    root = tempfile.mkdtemp(prefix="vkhook-pixels-")
    results = [run_variant(binary, library, v, root, app_args) for v in names]
    if not any(r == 1 for r in results):
        shutil.rmtree(root, ignore_errors=True)
    if any(r == 1 for r in results):
        return 1
    if all(r == SKIP for r in results):
        return SKIP
    print("present pixels PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
