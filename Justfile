set shell := ["sh", "-eu", "-c"]

repo_root := justfile_directory()
build_dir := repo_root / "build"
lib_dir := build_dir / "src"
layer_dir := build_dir / "packaging/meson-config"
dev_layer_dir := "/tmp/vkhook-dev-layer"
dev_layer_manifest := dev_layer_dir / "VkHook.json"
layer_name := "VK_LAYER_VKHOOK_post_processing"
# keep in sync with `branch` in packaging/flatpak/org.freedesktop.Platform.VulkanLayer.VkHook.json
layer_branch := "26.08"
dev_layer_name := "VK_LAYER_VKHOOK_post_processing_dev"
default_log := "/tmp/vkhook.log"

default:
    @just --list

flatpak_manifest := repo_root / "packaging/flatpak/org.freedesktop.Platform.VulkanLayer.VkHook.json"
flatpak_build_dir := repo_root / "build/flatpak"
flatpak_repo_dir := repo_root / "build/flatpak-repo"

# Check the Veil models and proofs in formal/ (needs ~/Development/veil or VEIL_HOME).
formal:
    cd {{ repo_root }}/formal && lake build

# Unit tests (no GPU).
test:
    meson test -C {{ build_dir }} --print-errorlogs

# The layer under vkcube on Xvfb (set VK_ICD_FILENAMES for lavapipe; VKHOOK_SMOKE_VALIDATION=1 adds Khronos validation).
smoke seconds='11':
    env -u WAYLAND_DISPLAY xvfb-run -a src/tests/smoke_vkcube.sh {{ build_dir }}/src/libvkhook.so {{ seconds }}

# Pixel readback: what the X server shows must be the frames the app rendered.
pixels:
    meson test -C {{ build_dir }} 'present pixels' --print-errorlogs

# Sanitizer builds in their own build dirs: asan (+UBSan) runs everything, tsan the threaded suite.
sanitize kind='asan':
    #!/usr/bin/env bash
    set -euo pipefail
    dir={{ repo_root }}/build-{{ kind }}
    case {{ kind }} in
      asan) opts="-Db_sanitize=address,undefined"; suite="" ;;
      tsan) opts="-Db_sanitize=thread"; suite="--suite threaded" ;;
      *) echo "kind is asan or tsan" >&2; exit 2 ;;
    esac
    [ -f "$dir/build.ninja" ] || CC=clang CXX=clang++ meson setup "$dir" {{ repo_root }} \
      --buildtype=debugoptimized -Ddlss5=disabled -Db_lundef=false $opts
    meson test -C "$dir" $suite --print-errorlogs

# Cross-compile the i686 layer with Zig (docs/WORKFLOWS.md, "Building the i686 layer").
i686:
    scripts/build-i686.sh {{ repo_root }}/build-i686

flatpak-build:
    flatpak-builder \
      --force-clean \
      --install-deps-from=flathub \
      --repo={{ flatpak_repo_dir }} \
      {{ flatpak_build_dir }} \
      {{ flatpak_manifest }}
    flatpak remote-add --user --if-not-exists vkhook-local {{ flatpak_repo_dir }} --no-gpg-verify
    flatpak remote-modify --user --url=file://{{ flatpak_repo_dir }} --no-gpg-verify vkhook-local
    flatpak install --user --or-update -y vkhook-local org.freedesktop.Platform.VulkanLayer.VkHook//{{ layer_branch }}

nsight_dir := "/opt/nsight-graphics/NVIDIA-Nsight-Graphics-2026.1/host/linux-desktop-nomad-x64"
nsight_capture_bin := nsight_dir / "ngfx-capture.bin"
nsight_capture_dir := "/tmp/nsight-captures"

build:
    meson compile -C {{ build_dir }}

# Configure a native build with Clang and ccache. Meson records the compiler
# in the build directory, so use a separate build dir if changing toolchains.
configure:
    CC='ccache clang' CXX='ccache clang++' meson setup --buildtype=release --prefix=/usr {{ build_dir }}

prepare-layer:
    mkdir -p {{ dev_layer_dir }}
    sed -E \
      -e 's#"name": "{{ layer_name }}"#"name": "{{ dev_layer_name }}"#' \
      -e 's#"library_path": "[^"]*"#"library_path": "{{ lib_dir }}/libvkhook.so"#' \
      {{ layer_dir }}/VkHook.json > {{ dev_layer_manifest }}

run +args: prepare-layer
    env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKHOOK=${ENABLE_VKHOOK:-1} \
      DISABLE_VKHOOK=1 \
      {{ args }}

run-debug log +args: prepare-layer
    env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKHOOK=${ENABLE_VKHOOK:-1} \
      DISABLE_VKHOOK=1 \
      VKHOOK_LOG_LEVEL=debug \
      VKHOOK_LOG_FILE={{ log }} \
      {{ args }}

vkcube +args:
    just run vkdcube {{ args }}

vkcube-debug +args:
    just run-debug {{ default_log }} vkdcube {{ args }}

vkcube-debug-log log +args:
    just run-debug {{ log }} vkdcube {{ args }}

prime-run +args: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    prime-run env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKHOOK=${ENABLE_VKHOOK:-1} \
      DISABLE_VKHOOK=1 \
      {{ args }}

prime-run-debug log +args: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    prime-run env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKHOOK=${ENABLE_VKHOOK:-1} \
      DISABLE_VKHOOK=1 \
      VKHOOK_LOG_LEVEL=debug \
      VKHOOK_LOG_FILE={{ log }} \
      {{ args }}

vkdcube-prime +args:
    just prime-run vkdcube {{ args }}

vkdcube-prime-debug +args:
    just prime-run-debug {{ default_log }} vkdcube {{ args }}

vkdcube-prime-debug-log log +args:
    just prime-run-debug {{ log }} vkdcube {{ args }}

vkcube-capture shot log delay='3':
    rm -f {{ shot }} {{ log }} /tmp/vkhook-vkcube-capture.out
    just vkcube-debug-log {{ log }} >/tmp/vkhook-vkcube-capture.out 2>&1 & pid=$!;       sleep {{ delay }};       spectacle -b -n -o {{ shot }};       kill -INT $pid;       wait $pid || true;       ls -l {{ shot }};       echo ---LOG---;       tail -n 60 {{ log }} || true

vkcube-capture-config config shot log delay='3':
    rm -f {{ shot }} {{ log }} /tmp/vkhook-vkcube-capture.out
    VKHOOK_CONFIG_FILE={{ config }} just vkcube-debug-log {{ log }} >/tmp/vkhook-vkcube-capture.out 2>&1 & pid=$!;       sleep {{ delay }};       spectacle -b -n -o {{ shot }};       kill -INT $pid;       wait $pid || true;       ls -l {{ shot }};       echo ---LOG---;       tail -n 60 {{ log }} || true

vkcube-capture-default:
    just vkcube-capture /tmp/vkhook-shot.png /tmp/vkhook.log 3

nsight-vkdcube-capture frame='10' count='1' capture_dir=nsight_capture_dir: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{ capture_dir }}
    prime-run env -u LD_LIBRARY_PATH \
      {{ nsight_capture_bin }} \
      -e "$(command -v vkdcube)" \
      --env=VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      --env=VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      --env=ENABLE_VKHOOK=1 \
      --env=GDK_PIXBUF_MODULEDIR=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders \
      --env=GDK_PIXBUF_MODULE_FILE=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache \
      --env=XDG_DATA_DIRS=/usr/local/share:/usr/share \
      --working-dir {{ repo_root }} \
      --output-dir {{ capture_dir }} \
      --capture-frame {{ frame }} \
      -n {{ count }} \
      --terminate-after-capture \
      --diagnostic-mode

nsight-vkdcube-capture-hotkey count='1' capture_dir=nsight_capture_dir: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{ capture_dir }}
    prime-run env -u LD_LIBRARY_PATH \
      {{ nsight_capture_bin }} \
      -e "$(command -v vkdcube)" \
      --env=VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      --env=VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      --env=ENABLE_VKHOOK=1 \
      --env=GDK_PIXBUF_MODULEDIR=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders \
      --env=GDK_PIXBUF_MODULE_FILE=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache \
      --env=XDG_DATA_DIRS=/usr/local/share:/usr/share \
      --working-dir {{ repo_root }} \
      --output-dir {{ capture_dir }} \
      --capture-hotkey \
      -n {{ count }} \
      --terminate-after-capture \
      --diagnostic-mode
