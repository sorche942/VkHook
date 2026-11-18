#!/usr/bin/env python3
"""Opt-in GPU regression; needs Python 3, libX11, DISPLAY and an Xlib vkcube.

Usage: python3 src/tests/test_shared_texture_smoke.py --layer /abs/libvkhook.so
Supply --vkcube /path/to/32-bit/vkcube with a matching 32-bit layer if desired.
No desktop screenshots, global key injection, or user configuration writes.
"""

import argparse
import ctypes as C
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


class XImage(C.Structure):
    _fields_ = [(name, kind) for name, kind in (
        ("width", C.c_int), ("height", C.c_int), ("xoffset", C.c_int),
        ("format", C.c_int), ("data", C.c_void_p), ("byte_order", C.c_int),
        ("bitmap_unit", C.c_int), ("bitmap_bit_order", C.c_int),
        ("bitmap_pad", C.c_int), ("depth", C.c_int),
        ("bytes_per_line", C.c_int), ("bits_per_pixel", C.c_int),
        ("red_mask", C.c_ulong), ("green_mask", C.c_ulong),
        ("blue_mask", C.c_ulong))]


class ClientMessage(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong),
                ("send_event", C.c_int), ("display", C.c_void_p),
                ("window", C.c_ulong), ("message_type", C.c_ulong),
                ("format", C.c_int), ("data", C.c_long * 5)]


class XEvent(C.Union):
    _fields_ = [("client", ClientMessage), ("padding", C.c_long * 24)]


class X11:
    def __init__(self):
        self.lib = C.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        ptr, ul, integer = C.c_void_p, C.c_ulong, C.c_int
        signatures = {
            "XOpenDisplay": (ptr, [C.c_char_p]),
            "XCloseDisplay": (integer, [ptr]),
            "XDefaultRootWindow": (ul, [ptr]),
            "XQueryTree": (integer, [ptr, ul, C.POINTER(ul), C.POINTER(ul),
                                      C.POINTER(C.POINTER(ul)), C.POINTER(C.c_uint)]),
            "XFetchName": (integer, [ptr, ul, C.POINTER(ptr)]),
            "XFree": (integer, [ptr]),
            "XInternAtom": (ul, [ptr, C.c_char_p, integer]),
            "XGetWindowProperty": (integer, [ptr, ul, ul, C.c_long, C.c_long,
                integer, ul, C.POINTER(ul), C.POINTER(integer), C.POINTER(ul),
                C.POINTER(ul), C.POINTER(ptr)]),
            "XGetGeometry": (integer, [ptr, ul, C.POINTER(ul),
                C.POINTER(integer), C.POINTER(integer), C.POINTER(C.c_uint),
                C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]),
            "XGetImage": (C.POINTER(XImage), [ptr, ul, integer, integer,
                                               C.c_uint, C.c_uint, ul, integer]),
            "XGetPixel": (ul, [C.POINTER(XImage), integer, integer]),
            "XDestroyImage": (integer, [C.POINTER(XImage)]),
            "XSendEvent": (integer, [ptr, ul, integer, C.c_long, C.POINTER(XEvent)]),
            "XFlush": (integer, [ptr]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, arguments
        # A window can disappear between discovery and capture. Do not let the
        # default Xlib error handler terminate Python and bypass child cleanup.
        callback = C.CFUNCTYPE(integer, ptr, ptr)
        self.error_handler = callback(lambda display, event: 0)
        self.lib.XSetErrorHandler.argtypes = [callback]
        self.lib.XSetErrorHandler.restype = ptr
        self.lib.XSetErrorHandler(self.error_handler)
        self.display = self.lib.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("cannot open DISPLAY; provide DISPLAY/XAUTHORITY (or xvfb-run)")
        self.root = self.lib.XDefaultRootWindow(self.display)

    def windows(self):
        pending, found = [self.root], set()
        while pending:
            window = pending.pop()
            if window in found:
                continue
            found.add(window)
            root, parent = C.c_ulong(), C.c_ulong()
            children, count = C.POINTER(C.c_ulong)(), C.c_uint()
            if self.lib.XQueryTree(self.display, window, C.byref(root), C.byref(parent),
                                   C.byref(children), C.byref(count)):
                pending.extend(children[i] for i in range(count.value))
            if children:
                self.lib.XFree(children)
        return found

    def name(self, window):
        value = C.c_void_p()
        self.lib.XFetchName(self.display, window, C.byref(value))
        try:
            return C.string_at(value).decode(errors="replace") if value.value else ""
        finally:
            if value.value:
                self.lib.XFree(value)

    def pid(self, window):
        atom = self.lib.XInternAtom(self.display, b"_NET_WM_PID", 1)
        if not atom:
            return None
        kind, count, remaining = C.c_ulong(), C.c_ulong(), C.c_ulong()
        fmt, value = C.c_int(), C.c_void_p()
        self.lib.XGetWindowProperty(self.display, window, atom, 0, 1, 0, 6,
            C.byref(kind), C.byref(fmt), C.byref(count), C.byref(remaining), C.byref(value))
        try:
            if value.value and fmt.value == 32 and count.value == 1:
                return C.cast(value, C.POINTER(C.c_ulong))[0]
            return None
        finally:
            if value.value:
                self.lib.XFree(value)

    def size(self, window):
        root, x, y = C.c_ulong(), C.c_int(), C.c_int()
        width, height, border, depth = (C.c_uint() for _ in range(4))
        if not self.lib.XGetGeometry(self.display, window, C.byref(root), C.byref(x),
                C.byref(y), C.byref(width), C.byref(height), C.byref(border), C.byref(depth)):
            return None
        return width.value, height.value

    def sample(self, window):
        size = self.size(window)
        if size is None or min(size) < 16:
            return None
        width, height = size
        # Capture only the identified client window; never the root/desktop.
        image = self.lib.XGetImage(self.display, window, 0, 0,
                                   width, height, C.c_ulong(-1), 2)
        if not image:
            return None
        try:
            masks = (image.contents.red_mask, image.contents.green_mask, image.contents.blue_mask)
            if not all(masks):
                raise RuntimeError("XGetImage did not provide RGB masks (TrueColor display required)")
            shifts = [(mask & -mask).bit_length() - 1 for mask in masks]
            pixels = []
            for fy in (1, 2, 3):
                for fx in (1, 2, 3):
                    px, py = width * fx // 4, height * fy // 4
                    pixel = self.lib.XGetPixel(image, px, py)
                    rgb = [round(((pixel & mask) >> shift) * 255 / (mask >> shift))
                           for mask, shift in zip(masks, shifts)]
                    pixels.append({"xy": [px, py], "rgb": rgb})
            return pixels
        finally:
            self.lib.XDestroyImage(image)

    def close_window(self, window):
        event = XEvent()
        event.client.type = 33  # ClientMessage, directed only to our own window.
        event.client.display = self.display
        event.client.window = window
        event.client.message_type = self.lib.XInternAtom(self.display, b"WM_PROTOCOLS", 0)
        event.client.format = 32
        event.client.data[0] = self.lib.XInternAtom(self.display, b"WM_DELETE_WINDOW", 0)
        event.client.data[1] = 0  # CurrentTime
        self.lib.XSendEvent(self.display, window, 0, 0, C.byref(event))
        self.lib.XFlush(self.display)

    def close(self):
        self.lib.XCloseDisplay(self.display)


def seed(work, layer, executable):
    layer_dir, config = work / "layers", work / "config" / "VkHook"
    profiles = config / "configs" / "shaders"
    shaders = config / "reshade" / "Shaders"
    for directory in (layer_dir, profiles, shaders, work / "home", work / "data", work / "cache"):
        directory.mkdir(parents=True, exist_ok=True)
    layer_name = "VK_LAYER_VKHOOK_post_processing"
    manifest = {"file_format_version": "1.0.0", "layer": {
        "name": layer_name, "type": "GLOBAL", "library_path": str(layer),
        "api_version": "1.3.223", "implementation_version": "1",
        "description": "VkHook shared texture GPU regression",
        "functions": {"vkGetInstanceProcAddr": "VkHook_GetInstanceProcAddr",
                      "vkGetDeviceProcAddr": "VkHook_GetDeviceProcAddr"}}}
    # Deliberately no disable_environment: DISABLE_VKHOOK disables installed
    # implicit manifests, not this explicitly requested test manifest.
    (layer_dir / "VkHook.json").write_text(json.dumps(manifest), encoding="utf-8")
    names = ("SharedTextureProducer", "SharedTextureConsumer")
    for name in names:
        shutil.copyfile(Path(__file__).resolve().parent / "fixtures" / (name + ".fx"),
                        shaders / (name + ".fx"))
    (config / "VkHook.conf").write_text(
        "enableOnLaunch = true\noverlayKey = Home\nreloadKey = F10\n"
        "maxEffects = 10\nautoApply = false\ndepthCapture = on\nshowDebugWindow = false\n",
        encoding="utf-8")
    # The layer keys profiles by the resolved /proc/self/exe basename, including
    # custom 32-bit binary names, not argv[0] or a hard-coded 'vkcube'.
    game = executable.name
    (config / "configs" / (game + ".conf")).write_text(
        "".join(name + " = " + name + ".fx\n" for name in names) +
        "effects = " + ":".join(names) + "\n", encoding="utf-8")
    techniques = ",".join(name + "@" + name + ".fx" for name in names)
    (profiles / (game + "@shared-texture.ini")).write_text(
        "Techniques=" + techniques + "\nTechniqueSorting=" + techniques + "\n" +
        "".join("\n[" + name + ".fx]\n" for name in names), encoding="utf-8")
    (profiles / (game + ".last-profile")).write_text("shared-texture\n", encoding="utf-8")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("VKHOOK_", "VK_LAYER_", "VK_ADD_LAYER_", "VK_LOADER_LAYERS_"))
           and key not in ("VK_INSTANCE_LAYERS", "ENABLE_VKHOOK", "DISABLE_VKHOOK")}
    # Preserve default X authentication before replacing HOME for the child.
    authority = Path(os.environ.get("HOME", "")) / ".Xauthority"
    if "XAUTHORITY" not in env and authority.is_file():
        env["XAUTHORITY"] = str(authority.resolve())
    env.update(HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "config"),
               XDG_DATA_HOME=str(work / "data"), XDG_CACHE_HOME=str(work / "cache"),
               VK_LAYER_PATH=str(layer_dir), VK_INSTANCE_LAYERS=layer_name,
               VK_LOADER_LAYERS_DISABLE="~implicit", DISABLE_VKHOOK="1",
               VK_LOADER_DEBUG="layer", VKHOOK_LOG_LEVEL="debug")
    return env


def stop(child, x11, window):
    if child.poll() is not None:
        return
    if window is not None:
        x11.close_window(window)
        try:
            child.wait(timeout=5)
            return
        except subprocess.TimeoutExpired:
            pass
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def diagnostics(work, artifacts, samples):
    print("Last sampled pixels: " + json.dumps(samples), flush=True)
    for path in sorted(work.rglob("*.log")):
        print("--- " + str(path.relative_to(work)) + " (last 64 KiB) ---", flush=True)
        print(path.read_text(errors="replace")[-65536:], flush=True)
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix="shared-texture-", dir=artifacts))
        for path in work.rglob("*.log"):
            target = destination / path.relative_to(work)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        (destination / "pixels.json").write_text(json.dumps(samples, indent=2) + "\n", encoding="utf-8")
        print("Failure artifacts: " + str(destination), flush=True)


def run(args, executable):
    with tempfile.TemporaryDirectory(prefix="vkhook-shared-texture-") as temporary:
        work = Path(temporary)
        child, x11, window, samples = None, None, None, None
        try:
            env = seed(work, args.layer, executable)
            x11 = X11()
            previous = x11.windows()
            with (work / "vkcube.log").open("wb") as output:
                child = subprocess.Popen([str(executable), "--wsi", "xlib", "--width", "640", "--height", "480"], env=env,
                                         stdout=output, stderr=subprocess.STDOUT)
                deadline, consecutive = time.monotonic() + args.timeout, 0
                while time.monotonic() < deadline:
                    if child.poll() is not None:
                        raise RuntimeError("vkcube exited before contents passed: " + str(child.returncode))
                    if window is None:
                        candidates = []
                        for candidate in x11.windows() - previous:
                            title = x11.name(candidate).lower()
                            owner = x11.pid(candidate)
                            if owner is not None and owner != child.pid:
                                continue
                            if ("vulkan cube" in title or "vkcube" in title or owner == child.pid
                                    or x11.size(candidate) == (640, 480)):
                                candidates.append(candidate)
                        if len(candidates) > 1:
                            raise RuntimeError("multiple new cube windows; refusing an ambiguous capture")
                        if candidates:
                            window = candidates[0]
                            print("Capturing new vkcube window: " + hex(window), flush=True)
                    if window is not None:
                        samples = x11.sample(window)
                        green = samples and all(r <= 12 and g >= 243 and b <= 12
                                                for r, g, b in (p["rgb"] for p in samples))
                        consecutive = consecutive + 1 if green else 0
                        if consecutive == 3:
                            break
                    time.sleep(0.2)
                else:
                    raise RuntimeError("timed out waiting for producer green (0,255,0) in consumer; "
                                       "keep the test window visible and unobscured")
                # Pixel contents are the assertion, but also establish that the
                # requested build, not an installed layer, actually participated.
                if str(args.layer) not in (work / "vkcube.log").read_text(errors="replace"):
                    raise RuntimeError("loader trace does not identify the requested layer library")
                if not (work / "config" / "VkHook" / "vkhook.log").is_file():
                    raise RuntimeError("requested layer produced no initialization log")
                stop(child, x11, window)
                if child.returncode != 0:
                    raise RuntimeError("vkcube did not shut down cleanly: " + str(child.returncode))
            print("PASS: producer-to-consumer shared texture contents; sampled pixels: " +
                  json.dumps(samples), flush=True)
            return 0
        except (OSError, RuntimeError, KeyboardInterrupt) as error:
            if child is not None:
                stop(child, x11, window)
            print("FAIL: " + (str(error) or type(error).__name__), flush=True)
            diagnostics(work, args.artifacts, samples)
            return 130 if isinstance(error, KeyboardInterrupt) else 1
        finally:
            if child is not None and child.poll() is None:
                stop(child, x11, window)
            if x11 is not None:
                x11.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--layer", type=Path, required=True, help="absolute path to libvkhook.so under test")
    parser.add_argument("--vkcube", default="vkcube", help="Xlib-capable vkcube executable (default: vkcube)")
    parser.add_argument("--timeout", type=float, default=30, help="window/contents readiness deadline in seconds (default: 30)")
    parser.add_argument("--artifacts", type=Path, help="optional directory for failure logs/pixels; otherwise stdout only")
    args = parser.parse_args()
    if not args.layer.is_absolute() or not args.layer.is_file():
        parser.error("--layer must name an existing absolute library path")
    args.layer = args.layer.resolve()
    if not 0 < args.timeout <= 300:
        parser.error("--timeout must be greater than zero and at most 300 seconds")
    executable = shutil.which(args.vkcube)
    if not executable:
        parser.error("vkcube executable not found: " + args.vkcube)
    if not os.environ.get("DISPLAY"):
        parser.error("DISPLAY is required; run on X11/Xwayland or under xvfb-run")
    return run(args, Path(executable).resolve())


if __name__ == "__main__":
    sys.exit(main())
