#!/usr/bin/env bash
# smoke test: run vkcube under the freshly built layer and fail if it dies
#
# the point is not that vkcube renders correctly, it is that driving a real
# swapchain through the layer for a few seconds does not take the process down.
# that catches the class of bug unit tests cannot: a bad interceptor, a null
# handle dereference, a use-after-free on a handle whose lifetime we guessed at.
#
# usage: src/tests/smoke_vkcube.sh [LIBVKHOOK_SO] [SECONDS]
#
# VKHOOK_SMOKE_DEPTH_REFRESH sets how often, in ms, the run forces a depth
# re-detect (default 500), which is what drives the rebuild assertions below.
#
# VKHOOK_SMOKE_VALIDATION=1 enables ONLY VK_LAYER_KHRONOS_validation plus the
# test layer (never the "*VKHOOK*" glob, which would also pull in an installed
# VkHook), and asserts that the loader inserted the validation layer and that
# the run produced zero "Validation Error" / VUID- lines. Set VK_LAYER_ENABLES
# (e.g. VALIDATION_CHECK_ENABLE_SYNCHRONIZATION_VALIDATION) to add checks; it is
# passed through untouched.
#
# headless: `env -u WAYLAND_DISPLAY xvfb-run -a src/tests/smoke_vkcube.sh LIB 11`.
# vkcube prefers Wayland whenever WAYLAND_DISPLAY is set, even under xvfb-run, so
# the script unsets it for the vkcube child when it knows the display is virtual:
# VKHOOK_SMOKE_HEADLESS=1 (explicit), or xvfb-run's own XAUTHORITY
# (/tmp/xvfb-run.*/Xauthority*), or this script having re-exec'd itself under
# xvfb-run. On a real display none of these hold and the environment is untouched.
#
# needs a display. on CI wrap it: xvfb-run -a src/tests/smoke_vkcube.sh
# there is no GPU on a CI runner either, so point VK_ICD_FILENAMES at lavapipe
# or the loader will find nothing to run on.

set -euo pipefail

# resolved from the script's own location rather than the cwd, so the shader
# fixtures are found when this is invoked from anywhere -- CI in particular runs
# it through xvfb-run from the repository root, and a cwd-relative path would
# quietly seed no effect at all, turning the run into a pass-through test that
# looks like it passed.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LIBRARY="${1:-build/src/libvkhook.so}"
DURATION="${2:-11}"
LAYER_NAME="VK_LAYER_VKHOOK_smoke_test"
# the installed production layer, which on a developer machine would otherwise
# be discovered alongside ours and could shadow the build we actually want to test
INSTALLED_LAYER="VK_LAYER_VKHOOK_post_processing"

if [[ -t 1 ]]; then
    B='\033[1m'; BLUE='\033[34m'; GREEN='\033[32m'; YELLOW='\033[33m'; RED='\033[31m'; DIM='\033[2m'; NC='\033[0m'
else
    B=''; BLUE=''; GREEN=''; YELLOW=''; RED=''; DIM=''; NC=''
fi

say() { printf "${BLUE}  →${NC} %s\n" "$*"; }
ok() { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}  !${NC} %s\n" "$*"; }
die() {
    printf "${RED}  ✗${NC} %s\n" "$*" >&2
    if [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        printf "  ${DIM}artifacts kept: %s${NC}\n" "$WORK_DIR" >&2
    fi
    exit 1
}
finish() { printf "\n${BLUE}${B}%s${NC}\n" "$*"; }

command -v vkcube >/dev/null 2>&1 || die "vkcube not found; install vulkan-tools"
[[ -f "$LIBRARY" ]] || die "layer library not found: $LIBRARY (build it first)"
LIBRARY="$(cd "$(dirname "$LIBRARY")" && pwd)/$(basename "$LIBRARY")"

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    # A headless runner has no display at all and vkcube cannot open a window
    # without one. Re-exec under xvfb-run so the same command works on a
    # developer desktop and on CI. The env marker keeps a broken xvfb-run from
    # turning this into an exec loop.
    if [[ -z "${VKHOOK_SMOKE_XVFB:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
        VKHOOK_SMOKE_XVFB=1 exec xvfb-run -a "$0" "$@"
    fi
    die "no display, and xvfb-run is not available to provide one"
fi

WORK_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT
    if (( status == 0 )) && [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        rm -rf -- "$WORK_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT

# A manifest of our own, under a layer name nothing else uses, so the loader
# cannot mistake it for an installed copy and drop it as a duplicate.
LAYER_DIR="$WORK_DIR/layer"
CONFIG_DIR="$WORK_DIR/config"
mkdir -p "$LAYER_DIR" "$CONFIG_DIR"
cat >"$LAYER_DIR/$LAYER_NAME.json" <<JSON
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "$LAYER_NAME",
    "type": "GLOBAL",
    "library_path": "$LIBRARY",
    "api_version": "1.3.223",
    "implementation_version": "1",
    "description": "vkhook smoke test",
    "functions": {
      "vkGetInstanceProcAddr": "VkHook_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "VkHook_GetDeviceProcAddr"
    },
    "enable_environment": { "ENABLE_VKHOOK": "1" },
    "disable_environment": { "DISABLE_VKHOOK": "1" }
  }
}
JSON
jq -e . "$LAYER_DIR/$LAYER_NAME.json" >/dev/null || die "generated manifest is not valid json"

# Seed a config that actually exercises the layer. With no config the layer
# starts with no effects enabled and takes the pass-through path, which is close
# to doing nothing at all, so a crash-only smoke test would be asserting almost
# nothing. Enabling one depth-consuming effect forces the capture and resolve
# machinery to run, which is where the layer actually earns its crash reports.
#
# The depth settings are pinned rather than left to defaults so the test is not
# quietly at the mercy of whatever a previous run happened to write:
#   depthInvert = false -> near/far not flipped
seed_config() {
    local base="$CONFIG_DIR/VkHook"
    mkdir -p "$base/configs/shaders" "$base/reshade/packages/Shaders"

    cat >"$base/VkHook.conf" <<'CONF'
enableOnLaunch = true
overlayKey = Home
reloadKey = F10
maxEffects = 10
autoApply = true
autoApplyDelay = 200
depthCapture = on
showDebugWindow = false
depthResolveMode = 0
depthManualPin =
depthTransientWorkaround = true
depthInvert = false
CONF

    # The effect is a real, community-maintained ReShade shader rather than
    # something written for this test. That matters: a hand-written effect proved
    # the depth path ran, but exercised almost none of the compile pipeline. This
    # one carries uniform preprocessor definitions, a localisation include and a
    # large UI parameter set, so it goes through the same machinery a user's shader
    # does. The files live in src/tests/fixtures -- see the README there for
    # provenance and the open licence question on two of the three.
    local fixtures="$SCRIPT_DIR/fixtures"
    for fixture in DisplayDepth.fx ReShade.fxh DisplayDepth_L10N.fxh; do
        [[ -f "$fixtures/$fixture" ]] || die "missing test fixture: $fixtures/$fixture"
        cp "$fixtures/$fixture" "$base/reshade/packages/Shaders/$fixture"
    done

    # vkcube is the game name the layer derives from the process, so the config
    # and profile files are keyed on it. Parameters are deliberately omitted so the
    # effect runs on the defaults declared in the shader itself.
    # The chain comes from the ReShade preset below: its enabled technique
    # becomes the entry "DisplayDepth.fx|DisplayDepth". (The shared-texture smoke
    # covers a config of old instance names, which the registry migrates.)
    cat >"$base/configs/vkcube.conf" <<'CONF'
effects =
CONF

    cat >"$base/configs/shaders/vkcube@smoke.ini" <<'INI'
Techniques=DisplayDepth@DisplayDepth.fx
TechniqueSorting=DisplayDepth@DisplayDepth.fx

[DisplayDepth.fx]
INI

    printf 'smoke\n' >"$base/configs/shaders/vkcube.last-profile"

    # Exercise automatic or manual scale without changing the user's config.
    if [[ -n "${VKHOOK_SMOKE_UI_SCALE:-}" ]]; then
        printf 'uiScale = %s\n' "$VKHOOK_SMOKE_UI_SCALE" >>"$base/VkHook.conf"
    fi
}

seed_config

say "layer      $LIBRARY"
say "duration   ${DURATION}s"

# VK_LOADER_LAYERS_ENABLE is what guarantees our layer is the one that ends up in
# the chain. Without it, a VkHook already installed on the machine wins and the
# run silently tests the installed build instead of this one.
#
# The XDG_CONFIG_HOME redirect sends the layer's own log somewhere disposable, so
# "did the layer actually engage" is answerable without reading a stale log from
# an earlier run, and a local run cannot scribble on a real config.
# Headless: a virtual X display only works if vkcube is steered away from Wayland.
VKCUBE_ENV=()
if [[ "${VKHOOK_SMOKE_HEADLESS:-}" == 1 || -n "${VKHOOK_SMOKE_XVFB:-}" || "${XAUTHORITY:-}" == */xvfb-run.*/Xauthority* ]]; then
    VKCUBE_ENV+=(-u WAYLAND_DISPLAY)
    say "headless   virtual X display, WAYLAND_DISPLAY unset for vkcube"
fi

# Validation mode: only the Khronos validation layer and the test layer, and the
# system explicit-layer dirs so the loader can find validation.
ENABLE_LAYERS="$LAYER_NAME"
SEARCH_PATH="$LAYER_DIR"
if [[ "${VKHOOK_SMOKE_VALIDATION:-}" == 1 ]]; then
    ENABLE_LAYERS="VK_LAYER_KHRONOS_validation,$LAYER_NAME"
    data_dirs="${XDG_DATA_DIRS:-}"
    for d in /usr/share /etc ${data_dirs//:/ }; do
        layer_dir="$d/vulkan/explicit_layer.d"
        if [[ -d "$layer_dir" && ":$SEARCH_PATH:" != *":$layer_dir:"* ]]; then
            SEARCH_PATH="$SEARCH_PATH:$layer_dir"
        fi
    done
    say "validation on ($ENABLE_LAYERS)"
fi

set +e
# Debug level is set here rather than left to the ambient default, because the
# assertions below need lines that only exist at debug. Notably "detected depth
# image", which is the only way to tell "the app gave us no depth at all" (a
# real failure) apart from "depth was found but resolve never engaged" (an
# environment limitation). At the default info level those two look identical,
# so the test cannot fail correctly without asking for debug.
env "${VKCUBE_ENV[@]}" \
    VK_LOADER_DEBUG=layer \
    VKHOOK_LOG_LEVEL=debug \
    VKHOOK_SMOKE_DEPTH_REFRESH="${VKHOOK_SMOKE_DEPTH_REFRESH:-500}" \
    VK_LOADER_LAYERS_ENABLE="$ENABLE_LAYERS" \
    VK_LOADER_LAYERS_DISABLE="$INSTALLED_LAYER" \
    VK_LAYER_PATH="$SEARCH_PATH" \
    ENABLE_VKHOOK=1 \
    XDG_CONFIG_HOME="$CONFIG_DIR" \
    timeout "$DURATION" vkcube >"$WORK_DIR/vkcube.log" 2>&1
STATUS=$?
set -e

LOADED=no
if grep -qF "$LIBRARY" "$WORK_DIR/vkcube.log"; then
    LOADED=yes
fi

if [[ "$LOADED" != yes ]]; then
    printf "%s\n" "--- vkcube + loader output ---" >&2
    cat "$WORK_DIR/vkcube.log" >&2
    die "the loader never loaded $LIBRARY, so this run proves nothing. an installed copy of the layer may be shadowing it; check the loader lines above."
fi
ok "layer loaded from the build directory"

if [[ "${VKHOOK_SMOKE_VALIDATION:-}" == 1 ]]; then
    # The loader's own log line is the proof validation was actually in the chain;
    # without it "0 errors" would also be what a run with no validation reports.
    if ! grep -qE 'Insert instance layer "?VK_LAYER_KHRONOS_validation' "$WORK_DIR/vkcube.log"; then
        grep -E 'VK_LAYER_KHRONOS_validation|layer' "$WORK_DIR/vkcube.log" | head -20 >&2 || true
        die "validation requested but the loader never inserted VK_LAYER_KHRONOS_validation"
    fi
    # one "Validation Error:" line per message; the VUID- spec URL on the next
    # line would double count, so VUID- is only the fallback for other formats
    VALIDATION_ERRORS=$(grep -c 'Validation Error' "$WORK_DIR/vkcube.log" || true)
    (( VALIDATION_ERRORS > 0 )) || VALIDATION_ERRORS=$(grep -c 'VUID-' "$WORK_DIR/vkcube.log" || true)
    if (( VALIDATION_ERRORS > 0 )); then
        grep -E -B1 -A3 'Validation Error|VUID-' "$WORK_DIR/vkcube.log" | head -60 >&2
        die "validation: $VALIDATION_ERRORS error line(s) (full log: $WORK_DIR/vkcube.log)"
    fi
    ok "validation: 0 errors, layer loaded"
fi

LAYER_LOG="$CONFIG_DIR/VkHook/vkhook.log"
if [[ -s "$LAYER_LOG" ]]; then
    ok "layer engaged ($(wc -l <"$LAYER_LOG") log lines)"
else
    die "layer loaded but wrote no log; it may not have reached initialisation"
fi

# A panic means the layer hit a state it does not trust and soft-disabled itself
# or tore the device down; for a smoke run that is a failure even when the
# process limps on to the end, because nothing after it proves anything.
if grep -qF 'VkHook panic' "$LAYER_LOG"; then
    grep -F 'VkHook panic' "$LAYER_LOG" >&2
    die "the layer panicked during the run (see the line above)"
fi

# Assert the effect actually engaged, not merely that the layer loaded.
#
# Without this the test degrades silently. Every reason the seed can fail -- a
# fixture not copied, a shader that stops compiling, an effect name that no
# longer resolves -- results in the layer running with nothing enabled, taking
# the pass-through path and surviving. The run still reports PASSED, having
# asserted almost nothing.
#
# The signal is [effect-built], logged at the end of the effect's constructor.
# That makes it a creation-time fact: reaching the line means the shader was
# found, parsed, compiled and turned into render passes. An earlier version of
# this assertion matched the destructor's trace instead, which was wrong -- a
# trace from a destructor only appears if something tears the effect down, and
# on a CI runner the normal ending is vkcube being killed by the timeout, so it
# never got the chance. That assertion passed locally only because the resize
# reload happened to destroy effects mid-run, and failed on CI for the same
# reason inverted.
#
# passes= must be non-zero: zero would mean the effect object exists but never
# got a pipeline, so it never drew.
if grep -qE '\[effect-built\] DisplayDepth\.fx\|DisplayDepth passes=[1-9]' "$LAYER_LOG"; then
    ok "DisplayDepth compiled and built a render pass"
elif grep -q 'pass through only' "$LAYER_LOG"; then
    # The layer supports Wayland surfaces only. On X11 it deliberately does
    # nothing at all, so there is no effect to build and nothing to assert about
    # the effect pipeline here. This is the CI configuration, which is why the
    # run is a pass-through check on CI and a real one locally.
    warn "non-Wayland surface: layer is pass-through, effect pipeline not exercised (CI runs this way)"
else
    printf '%s\n' "--- layer log ---" >&2
    cat "$LAYER_LOG" >&2
    die "DisplayDepth was never built, so this run exercised the pass-through path and proves nothing about the effect pipeline. The log above shows how far it got."
fi

# The depth capture: the tracker must see vkcube's depth image, and the capture
# must pick it as the effect's source. The capture runs in the layer's own
# command buffer ahead of the effects, so it works the same on lavapipe as on a
# GPU.
if grep -q 'tracked depth image' "$LAYER_LOG"; then
    ok "depth image tracked"
else
    die "no depth image tracked. DisplayDepth uses depth, so capture is off or the app never provided one; the run exercised nothing depth-related."
fi
if grep -q 'depth capture source changed: image' "$LAYER_LOG"; then
    ok "depth capture engaged"

    # VKHOOK_SMOKE_DEPTH_REFRESH drops the depth selection every 500ms, the same
    # as clicking "Refresh depth buffers": each refresh must re-select a source
    # without rebuilding anything or stalling present.
    reselects=$(grep -c 'depth capture source changed: image' "$LAYER_LOG" || true)
    refreshes=$(grep -c 'smoke hook: forced depth re-detect' "$LAYER_LOG" || true)
    if (( refreshes > 1 )); then
        (( reselects > 1 )) || die "the refresh hook ran $refreshes times but the depth source was never re-selected"
        ok "$reselects depth selection(s) across $refreshes refresh(es)"
    fi
else
    die "depth image tracked but never captured: the selection found no window-sized image that drew"
fi

# 124 is what timeout reports when it had to stop the process, which is the only
# outcome that means "still alive when we stopped watching".
case "$STATUS" in
    124) ok "vkcube survived ${DURATION}s without crashing" ;;
    0)   ok "vkcube exited cleanly on its own" ;;
    137) die "vkcube was SIGKILLed (exit 137), usually the OOM killer" ;;
    139) die "vkcube segfaulted (exit 139) under the layer" ;;
    134) die "vkcube aborted (exit 134) under the layer" ;;
    *)
        printf "%s\n" "--- vkcube output ---" >&2
        cat "$WORK_DIR/vkcube.log" >&2
        die "vkcube exited with $STATUS"
        ;;
esac

finish "smoke test PASSED"
