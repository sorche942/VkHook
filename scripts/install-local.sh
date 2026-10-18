#!/usr/bin/env bash
# Install existing x86 builds for this user only; never build or use sudo.
set -euo pipefail

usage() {
    cat <<'EOF'
usage:
  scripts/install-local.sh install [BUILD_DIR ...]  install builds (default: build)
  scripts/install-local.sh uninstall                remove layers, keep config/shaders

Relative BUILD_DIR paths are resolved from the repository root.
Builds are identified by ELF architecture, not by directory name.
Enable per application with: ENABLE_VKINTOX=1 your-application
This installation is for native applications, not Flatpak sandboxes.
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

library_arch() {
    local header
    header="$(LC_ALL=C readelf -h -- "$1")" || die "cannot read ELF header: $1"
    if [[ "$header" =~ Class:[[:space:]]+ELF64([[:space:]]|$) &&
          "$header" =~ Machine:[[:space:]]+Advanced[[:space:]]+Micro[[:space:]]+Devices[[:space:]]+X86-64([[:space:]]|$) ]]; then
        printf 'x86_64\n'
    elif [[ "$header" =~ Class:[[:space:]]+ELF32([[:space:]]|$) &&
            "$header" =~ Machine:[[:space:]]+Intel[[:space:]]+80386([[:space:]]|$) ]]; then
        printf 'i686\n'
    else
        die "unsupported ELF architecture (expected x86-64 or i386): $1"
    fi
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="$HOME/.local/lib/vkintox"
LEGACY_LIBRARY="$LIB_DIR/libvkintox.so"
LAYER_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
LEGACY_MANIFEST="$LAYER_DIR/VKIntox.json"
ARCHITECTURES=(x86_64 i686)
RUNTIME_LIBRARIES=(libXi.so.6 libXtst.so.6 libxkbcommon.so.0)

case "${1:-}" in
    install)
        command -v jq >/dev/null || die 'jq is required to update the layer manifest'
        command -v readelf >/dev/null || die 'readelf is required to identify ELF architectures'
        shift
        [[ $# -gt 0 ]] || set -- build
        declare -A SOURCE_LIBRARIES=() SOURCE_MANIFESTS=() SOURCE_RUNTIME_DIRS=() SOURCE_BUILD_DIRS=()
        for BUILD_DIR in "$@"; do
            [[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
            SOURCE_LIBRARY="$BUILD_DIR/src/libvkintox.so"
            SOURCE_MANIFEST="$BUILD_DIR/packaging/meson-config/VKIntox.json"
            [[ -f "$SOURCE_LIBRARY" ]] || die "build the library first: $SOURCE_LIBRARY"
            [[ -f "$SOURCE_MANIFEST" ]] || die "missing build manifest: $SOURCE_MANIFEST"
            ARCH="$(library_arch "$SOURCE_LIBRARY")"
            [[ -z "${SOURCE_LIBRARIES[$ARCH]:-}" ]] || die "multiple builds supplied for $ARCH"
            SOURCE_LIBRARIES[$ARCH]="$SOURCE_LIBRARY"
            SOURCE_MANIFESTS[$ARCH]="$SOURCE_MANIFEST"
            SOURCE_RUNTIME_DIRS[$ARCH]="$BUILD_DIR/runtime-libs"
            SOURCE_BUILD_DIRS[$ARCH]="$BUILD_DIR"
        done

        # Preserve the old installation even when only the other architecture is
        # being added. Never replace an already migrated architecture with it.
        if [[ -e "$LEGACY_LIBRARY" || -L "$LEGACY_LIBRARY" ]]; then
            ARCH="$(library_arch "$LEGACY_LIBRARY")"
            if [[ -z "${SOURCE_LIBRARIES[$ARCH]:-}" ]] &&
               ! [[ -f "$LIB_DIR/$ARCH/libvkintox.so" && -f "$LAYER_DIR/VKIntox.$ARCH.json" ]]; then
                [[ -f "$LEGACY_MANIFEST" ]] || die "cannot migrate library without manifest: $LEGACY_MANIFEST"
                SOURCE_LIBRARIES[$ARCH]="$LEGACY_LIBRARY"
                SOURCE_MANIFESTS[$ARCH]="$LEGACY_MANIFEST"
            fi
        elif [[ -e "$LEGACY_MANIFEST" || -L "$LEGACY_MANIFEST" ]]; then
            if [[ -z "${SOURCE_LIBRARIES[x86_64]:-}" ]] &&
               ! [[ -f "$LIB_DIR/x86_64/libvkintox.so" && -f "$LAYER_DIR/VKIntox.x86_64.json" ]]; then
                die "legacy manifest has no library; supply an x86-64 build to replace it"
            fi
        fi

        # Validate every input before changing the installation, including the
        # optional, locally supplied runtime dependencies of a cross build.
        for ARCH in "${ARCHITECTURES[@]}"; do
            [[ -n "${SOURCE_LIBRARIES[$ARCH]:-}" ]] || continue
            RUNTIME_DIR="${SOURCE_RUNTIME_DIRS[$ARCH]:-}"
            [[ -n "$RUNTIME_DIR" ]] || continue
            for NAME in "${RUNTIME_LIBRARIES[@]}"; do
                [[ -e "$RUNTIME_DIR/$NAME" || -L "$RUNTIME_DIR/$NAME" ]] || continue
                [[ -f "$RUNTIME_DIR/$NAME" ]] || die "missing runtime library: $RUNTIME_DIR/$NAME"
                [[ "$(library_arch "$RUNTIME_DIR/$NAME")" = "$ARCH" ]] ||
                    die "runtime library architecture does not match $ARCH: $RUNTIME_DIR/$NAME"
                command -v patchelf >/dev/null ||
                    die 'patchelf is required when installing private runtime libraries'
            done
        done

        mkdir -p "$LIB_DIR" "$LAYER_DIR"
        STAGING="$(mktemp -d "$LIB_DIR/.install.XXXXXX")"
        trap 'rm -rf -- "$STAGING"' EXIT
        for ARCH in "${ARCHITECTURES[@]}"; do
            [[ -n "${SOURCE_LIBRARIES[$ARCH]:-}" ]] || continue
            BITS=64
            [[ "$ARCH" != i686 ]] || BITS=32
            mkdir -p "$STAGING/$ARCH/runtime-libs"
            install -m755 "${SOURCE_LIBRARIES[$ARCH]}" "$STAGING/$ARCH/libvkintox.so"
            jq --arg lib "$LIB_DIR/$ARCH/libvkintox.so" --arg bits "$BITS" \
                '.file_format_version = "1.2.1" |
                 .layer.library_path = $lib |
                 .layer.library_arch = $bits |
                 .layer.enable_environment.ENABLE_VKINTOX = "1"' \
                "${SOURCE_MANIFESTS[$ARCH]}" > "$STAGING/VKIntox.$ARCH.json"
            chmod 644 "$STAGING/VKIntox.$ARCH.json"
            RUNTIME_DIR="${SOURCE_RUNTIME_DIRS[$ARCH]:-}"
            HAS_RUNTIME=false
            if [[ -n "$RUNTIME_DIR" ]]; then
                for NAME in "${RUNTIME_LIBRARIES[@]}"; do
                    [[ -f "$RUNTIME_DIR/$NAME" ]] || continue
                    install -m755 "$RUNTIME_DIR/$NAME" "$STAGING/$ARCH/runtime-libs/$NAME"
                    # Steam's library importer does not resolve $ORIGIN here.
                    patchelf --set-rpath "$LIB_DIR/$ARCH/runtime-libs" \
                        "$STAGING/$ARCH/runtime-libs/$NAME"
                    HAS_RUNTIME=true
                done
            fi
            if [[ "$HAS_RUNTIME" = true ]]; then
                patchelf --set-rpath "$LIB_DIR/$ARCH/runtime-libs" "$STAGING/$ARCH/libvkintox.so"
            fi
            BUILD_DIR="${SOURCE_BUILD_DIRS[$ARCH]:-}"
            if [[ -n "$BUILD_DIR" && -f "$BUILD_DIR/meson-info/intro-installed.json" ]]; then
                jq -r 'to_entries[] | select(.value | contains("/vkintox/dlss5/")) |
                    [.key, (.value | split("/vkintox/dlss5/")[1])] | @tsv' \
                    "$BUILD_DIR/meson-info/intro-installed.json" > "$STAGING/$ARCH/assets.tsv"
                while IFS=$'\t' read -r SOURCE_ASSET RELATIVE_ASSET; do
                    [[ -n "$SOURCE_ASSET" ]] || continue
                    [[ -f "$SOURCE_ASSET" ]] || die "build the DLSS5 kernels first: $SOURCE_ASSET"
                    install -Dm644 "$SOURCE_ASSET" "$STAGING/$ARCH/dlss5/$RELATIVE_ASSET"
                done < "$STAGING/$ARCH/assets.tsv"
                if [[ -d "$STAGING/$ARCH/dlss5" ]]; then
                    install -m644 "$REPO_ROOT/src/vendor/opendlss_nr/LICENSE" "$REPO_ROOT/src/vendor/opendlss_nr/NOTICE" "$STAGING/$ARCH/dlss5/"
                fi
            fi
        done

        for ARCH in "${ARCHITECTURES[@]}"; do
            [[ -n "${SOURCE_LIBRARIES[$ARCH]:-}" ]] || continue
            mkdir -p "$LIB_DIR/$ARCH"
            for NAME in "${RUNTIME_LIBRARIES[@]}"; do
                if [[ -f "$STAGING/$ARCH/runtime-libs/$NAME" ]]; then
                    mkdir -p "$LIB_DIR/$ARCH/runtime-libs"
                    mv -f -- "$STAGING/$ARCH/runtime-libs/$NAME" "$LIB_DIR/$ARCH/runtime-libs/$NAME"
                fi
            done
            # Replace inodes rather than overwrite a running app's mappings.
            mv -f -- "$STAGING/$ARCH/libvkintox.so" "$LIB_DIR/$ARCH/libvkintox.so"
            if [[ -d "$STAGING/$ARCH/dlss5" ]]; then
                mkdir -p "$LIB_DIR/$ARCH/dlss5/shaders" "$LIB_DIR/$ARCH/dlss5/ptx"
                for KIND in shaders ptx; do
                    for ASSET in "$STAGING/$ARCH/dlss5/$KIND/"*; do
                        [[ -f "$ASSET" ]] || continue
                        mv -f -- "$ASSET" "$LIB_DIR/$ARCH/dlss5/$KIND/"
                    done
                done
                mv -f -- "$STAGING/$ARCH/dlss5/LICENSE" "$STAGING/$ARCH/dlss5/NOTICE" "$LIB_DIR/$ARCH/dlss5/"
            fi
            mv -f -- "$STAGING/VKIntox.$ARCH.json" "$LAYER_DIR/VKIntox.$ARCH.json"
            RUNTIME_DIR="${SOURCE_RUNTIME_DIRS[$ARCH]:-}"
            for NAME in "${RUNTIME_LIBRARIES[@]}"; do
                if [[ -z "$RUNTIME_DIR" || ! -f "$RUNTIME_DIR/$NAME" ]]; then
                    rm -f -- "$LIB_DIR/$ARCH/runtime-libs/$NAME"
                fi
            done
            if [[ -d "$LIB_DIR/$ARCH/runtime-libs" ]]; then
                rmdir --ignore-fail-on-non-empty -- "$LIB_DIR/$ARCH/runtime-libs"
            fi
            printf 'Installed library: %s\nInstalled manifest: %s\n' \
                "$LIB_DIR/$ARCH/libvkintox.so" "$LAYER_DIR/VKIntox.$ARCH.json"
        done
        # Retire the old paths only after their replacement is published.
        rm -f -- "$LEGACY_MANIFEST" "$LEGACY_LIBRARY"
        printf 'Enable per application: ENABLE_VKINTOX=1 your-application\n'
        ;;
    uninstall)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        for ARCH in "${ARCHITECTURES[@]}"; do
            rm -f -- "$LAYER_DIR/VKIntox.$ARCH.json" "$LIB_DIR/$ARCH/libvkintox.so"
            for NAME in "${RUNTIME_LIBRARIES[@]}"; do
                rm -f -- "$LIB_DIR/$ARCH/runtime-libs/$NAME"
            done
            if [[ -d "$LIB_DIR/$ARCH/dlss5" ]]; then
                rm -f -- "$LIB_DIR/$ARCH/dlss5/shaders/"*.spv "$LIB_DIR/$ARCH/dlss5/ptx/"*.ptx \
                    "$LIB_DIR/$ARCH/dlss5/LICENSE" "$LIB_DIR/$ARCH/dlss5/NOTICE"
                for KIND in shaders ptx; do
                    [[ ! -d "$LIB_DIR/$ARCH/dlss5/$KIND" ]] || rmdir --ignore-fail-on-non-empty -- "$LIB_DIR/$ARCH/dlss5/$KIND"
                done
                rmdir --ignore-fail-on-non-empty -- "$LIB_DIR/$ARCH/dlss5"
            fi
            if [[ -d "$LIB_DIR/$ARCH/runtime-libs" ]]; then
                rmdir --ignore-fail-on-non-empty -- "$LIB_DIR/$ARCH/runtime-libs"
            fi
            if [[ -d "$LIB_DIR/$ARCH" ]]; then
                rmdir --ignore-fail-on-non-empty -- "$LIB_DIR/$ARCH"
            fi
            printf 'Removed library: %s\nRemoved manifest: %s\n' \
                "$LIB_DIR/$ARCH/libvkintox.so" "$LAYER_DIR/VKIntox.$ARCH.json"
        done
        rm -f -- "$LEGACY_MANIFEST" "$LEGACY_LIBRARY"
        if [[ -d "$LIB_DIR" ]]; then
            rmdir --ignore-fail-on-non-empty -- "$LIB_DIR"
        fi
        printf 'Removed legacy library: %s\nRemoved legacy manifest: %s\n' \
            "$LEGACY_LIBRARY" "$LEGACY_MANIFEST"
        printf 'Config, shaders, fonts, and other Vulkan layers were left untouched.\n'
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
