#!/usr/bin/env python3
"""Opt-in Xlib/GPU regression for an in-scope D24S8 depth clear.

From the repository root, after building the corresponding layer/shader headers:

  g++ -std=c++17 -O2 -Ibuild-gcc/src/libvkhook.so.p \
    src/tests/depth_clear_smoke.cc -o build-gcc/depth-clear-smoke -lvulkan -lX11
  zig c++ \
    -target x86-linux-gnu.2.34 -std=c++17 -O2 -idirafter /usr/include \
    -Ibuild-zig32/src/libvkhook.so.p src/tests/depth_clear_smoke.cc \
    -L/usr/lib32 -lvulkan -lX11 -Wl,--allow-shlib-undefined -o build-zig32/depth-clear-smoke

  python3 \
    src/tests/test_depth_clear_smoke.py --binary "$PWD/build-gcc/depth-clear-smoke" \
    --layer "$PWD/build-gcc/src/libvkhook.so" --artifacts "$PWD/build-gcc"
  python3 \
    src/tests/test_depth_clear_smoke.py --binary "$PWD/build-zig32/depth-clear-smoke" \
    --layer "$PWD/build-zig32/src/libvkhook.so" --artifacts "$PWD/build-zig32"

Pass --layer /absolute/baseline.so for the unfixed-layer negative control.
Keep the test's own window visible/unobscured. --width 3840 --height 2160
reproduces the dump's dimensions when the display can show the entire window.
By default, exercise dynamic rendering with GENERAL depth; --legacy tests render passes.
No global input injection, desktop capture, or user configuration writes.
"""

import argparse
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import time

from test_shared_texture_smoke import X11


EFFECT = """
texture SceneColor : COLOR;
sampler SceneColorSampler { Texture = SceneColor; };
texture SceneDepth : DEPTH;
sampler SceneDepthSampler { Texture = SceneDepth; };
void DepthClearVS(uint id : SV_VertexID, out float4 position : SV_Position,
                  out float2 texcoord : TEXCOORD)
{
    texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 DepthClearPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (uv.y < 0.5) return tex2D(SceneColorSampler, uv);
    float depth = tex2D(SceneDepthSampler, uv).r;
    return float4(depth, depth, depth, 1.0);
}
technique DepthClearProbe
{
    pass { VertexShader = DepthClearVS; PixelShader = DepthClearPS; }
}
"""


def seed(work, args):
    config = work / "config" / "VkHook"
    profiles = config / "configs" / "shaders"
    shaders = config / "reshade" / "Shaders"
    layers = work / "layers"
    for directory in (profiles, shaders, layers, work / "home", work / "data", work / "cache"):
        directory.mkdir(parents=True, exist_ok=True)
    name = "VK_LAYER_VKHOOK_post_processing"
    manifest = {"file_format_version": "1.0.0", "layer": {
        "name": name, "type": "GLOBAL", "library_path": str(args.layer),
        "api_version": "1.3.223", "implementation_version": "1",
        "description": "VkHook depth clear GPU regression",
        "functions": {"vkGetInstanceProcAddr": "VkHook_GetInstanceProcAddr",
                      "vkGetDeviceProcAddr": "VkHook_GetDeviceProcAddr"}}}
    (layers / "VkHook.json").write_text(json.dumps(manifest), encoding="utf-8")
    effect_name = "DepthClearProbe"
    effect = EFFECT
    (shaders / (effect_name + ".fx")).write_text(effect, encoding="utf-8")
    (config / "VkHook.conf").write_text(
        "enableOnLaunch = true\noverlayKey = Home\nreloadKey = F10\n"
        "maxEffects = 10\nautoApply = false\ndepthCapture = on\nshowDebugWindow = false\n",
        encoding="utf-8")
    game = args.binary.name
    (config / "configs" / (game + ".conf")).write_text(
        f"{effect_name} = {effect_name}.fx\neffects = {effect_name}\n", encoding="utf-8")
    (profiles / (game + "@depth-clear.ini")).write_text(
        f"Techniques={effect_name}@{effect_name}.fx\n"
        f"TechniqueSorting={effect_name}@{effect_name}.fx\n\n[{effect_name}.fx]\n", encoding="utf-8")
    (profiles / (game + ".last-profile")).write_text("depth-clear\n", encoding="utf-8")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("VKHOOK_", "VK_LAYER_", "VK_ADD_LAYER_", "VK_LOADER_LAYERS_"))
           and key not in ("VK_INSTANCE_LAYERS", "ENABLE_VKHOOK", "DISABLE_VKHOOK")}
    authority = Path(os.environ.get("HOME", "")) / ".Xauthority"
    if "XAUTHORITY" not in env and authority.is_file():
        env["XAUTHORITY"] = str(authority.resolve())
    env.update(HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "config"),
               XDG_DATA_HOME=str(work / "data"), XDG_CACHE_HOME=str(work / "cache"),
               VK_LAYER_PATH=str(layers), VK_INSTANCE_LAYERS=name,
               VK_LOADER_LAYERS_DISABLE="~implicit", DISABLE_VKHOOK="1",
               VK_LOADER_DEBUG="layer", VKHOOK_LOG_LEVEL="debug")
    if args.validation:
        # Validation is BELOW VkHook so it observes injected commands too.
        env["VK_INSTANCE_LAYERS"] += ":VK_LAYER_KHRONOS_validation"
        env["VK_LAYER_PATH"] += ":/usr/share/vulkan/explicit_layer.d:/etc/vulkan/explicit_layer.d"
    return env


def evidence(text, width, height, legacy):
    # 129 is VK_FORMAT_D24_UNORM_S8_UINT. The capture may sample the image live
    # or from the layer's preserved copy; either is a real source.
    required = {
        "tracked D24S8 depth": rf"tracked depth image \S+ 129 {width}x{height} samples: 1(?:\s|$)",
        "capture source selected": r"depth capture source changed: image \S+ \((?:live|preserved copy)\)",
    }
    found = {}
    for label, pattern in required.items():
        match = re.search(pattern, text)
        if not match:
            raise RuntimeError("missing layer evidence: " + label)
        found[label] = match.group(0).strip()
    return found


def pixels_match(samples, phase):
    if not samples:
        return False
    # X11 helper returns a 3x3 grid. Avoid the half-width/half-height edges.
    expected = {0: (0, 255, 0) if phase == "clear" else (255, 0, 0),
                2: (255, 0, 0),
                6: (191, 191, 191) if phase == "clear" else (64, 64, 64),
                8: (64, 64, 64)}
    return all(all(abs(actual - target) <= 12 for actual, target in zip(samples[index]["rgb"], color))
               for index, color in expected.items())


def run(args):
    with tempfile.TemporaryDirectory(prefix="vkhook-depth-clear-") as temporary:
        work = Path(temporary)
        child, x11 = None, None
        samples, selected = {}, {}
        result = 1
        try:
            env = seed(work, args)
            x11 = X11()
            log = work / "application.log"
            layer_log = work / "config" / "VkHook" / "vkhook.log"
            with log.open("wb") as output:
                command = [str(args.binary), "--width", str(args.width), "--height", str(args.height)]
                if args.legacy:
                    command.append("--legacy")
                child = subprocess.Popen(command,
                                         env=env, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                                         preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
                phases = ("warmup", "clear")
                for phase in phases:
                    deadline = time.monotonic() + args.timeout
                    consecutive = 0
                    while time.monotonic() < deadline:
                        if child.poll() is not None:
                            raise RuntimeError("application exited during " + phase + ": " + str(child.returncode))
                        text = log.read_text(errors="replace")
                        match = re.search(r"^WINDOW=(\d+)$", text, re.MULTILINE)
                        if match and "READY phase=" + phase in text:
                            window = int(match.group(1))
                            if x11.name(window) != "VkHook depth clear regression":
                                raise RuntimeError("test window identity changed")
                            samples[phase] = x11.sample(window)
                            consecutive = consecutive + 1 if pixels_match(samples[phase], phase) else 0
                            if consecutive >= 3:
                                break
                        time.sleep(0.2)
                    else:
                        raise RuntimeError("timed out waiting for " + phase + " color AND captured depth pixels; "
                                           "keep the test window visible and unobscured")
                    if str(args.layer) not in text:
                        raise RuntimeError("loader trace does not identify requested layer")
                    if not layer_log.is_file():
                        raise RuntimeError("layer initialization log missing")
                    layer_text = layer_log.read_text(errors="replace")
                    selected[phase] = evidence(layer_text, args.width, args.height, args.legacy)
                    print(phase + " selected-source evidence: " + json.dumps(selected[phase]), flush=True)
                    print(phase + " window color/depth pixels: " + json.dumps(samples[phase]), flush=True)
                    # The risky clear cannot be recorded until shader resources
                    # are proven active by both log and actual depth texture output.
                    child.stdin.write(b"continue\n")
                    child.stdin.flush()
                child.stdin.close()
                child.wait(timeout=args.timeout)
                if child.returncode != 0:
                    raise RuntimeError("unclean shutdown: " + str(child.returncode))
            text = log.read_text(errors="replace")
            if "PASS clean_shutdown=true" not in text:
                raise RuntimeError("missing clean-shutdown marker")
            all_logs = text + "\n" + layer_log.read_text(errors="replace")
            if re.search(r"Validation Error|VUID-|UNASSIGNED-.*(?:Error|error)", all_logs):
                raise RuntimeError("Vulkan validation reported an invalid command; see artifacts")
            print("PASS: in-scope depth clear, continued draws, GPU color readback, captured depth before/after clear, clean shutdown",
                  flush=True)
            result = 0
        except (OSError, RuntimeError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            print("FAIL: " + (str(error) or type(error).__name__), flush=True)
            result = 130 if isinstance(error, KeyboardInterrupt) else 1
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            if child is not None and child.stdin is not None and not child.stdin.closed:
                try:
                    child.stdin.close()
                except OSError:
                    pass
            if x11 is not None:
                x11.close()
            if args.artifacts:
                args.artifacts.mkdir(parents=True, exist_ok=True)
                destination = Path(tempfile.mkdtemp(prefix="depth-clear-", dir=args.artifacts))
                for path in work.rglob("*.log"):
                    target = destination / path.relative_to(work)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target)
                (destination / "evidence.json").write_text(json.dumps(
                    {"pixels": samples, "selected_depth": selected, "exit_code": result}, indent=2) + "\n", encoding="utf-8")
                print("Artifacts: " + str(destination), flush=True)
            if result:
                print("Last sampled pixels: " + json.dumps(samples), flush=True)
                for path in sorted(work.rglob("*.log")):
                    print("--- " + str(path.relative_to(work)) + " (last 64 KiB) ---", flush=True)
                    print(path.read_text(errors="replace")[-65536:], flush=True)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="explicit path to architecture-matched smoke binary")
    parser.add_argument("--layer", type=Path, required=True, help="explicit path to layer under test")
    parser.add_argument("--artifacts", type=Path, help="retain success/failure logs and pixel evidence here")
    parser.add_argument("--timeout", type=float, default=90, help="deadline per phase, in seconds")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--validation", action="store_true", help="also enable installed architecture-matched Khronos validation")
    parser.add_argument("--legacy", action="store_true", help="use legacy render passes instead of dynamic rendering")
    args = parser.parse_args()
    for name in ("binary", "layer"):
        path = getattr(args, name).resolve()
        if not path.is_file():
            parser.error("--" + name + " must name an existing file")
        setattr(args, name, path)
    if not os.environ.get("DISPLAY"):
        parser.error("DISPLAY and appropriate XAUTHORITY are required")
    if not 0 < args.timeout <= 300 or not all(64 <= value <= 8192 for value in (args.width, args.height)):
        parser.error("timeout must be in (0, 300]; dimensions must be in [64, 8192]")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
