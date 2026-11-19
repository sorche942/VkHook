#!/usr/bin/env bash
# Install the packages a CI job needs on an Ubuntu runner.
# usage: scripts/ci-install-deps.sh build|smoke|i686|dlss5 [more...]
# Groups combine: `build smoke` installs both.
set -euo pipefail

pkgs=()
for group in "$@"; do
    case "$group" in
        build)
            pkgs+=(clang meson ninja-build pkg-config glslang-tools spirv-headers spirv-tools
                   libvulkan-dev libx11-dev libxi-dev libxtst-dev libwayland-dev libxkbcommon-dev
                   libglib2.0-dev wayland-protocols python3)
            ;;
        smoke)
            # xauth is only a Recommends of xvfb and xvfb-run shells out to it; without it the
            # wrapper dies before vkcube runs and the step fails with nothing useful in the log.
            pkgs+=(mesa-vulkan-drivers vulkan-tools vulkan-validationlayers xauth xvfb jq)
            ;;
        dlss5)
            pkgs+=(cmake git python3)
            ;;
        i686)
            sudo dpkg --add-architecture i386
            pkgs+=(libc6:i386 libx11-dev:i386 libxi-dev:i386 libxtst-dev:i386
                   libwayland-dev:i386 libxkbcommon-dev:i386 libvulkan1:i386 curl xz-utils)
            ;;
        *)
            echo "unknown dependency group: $group" >&2
            exit 2
            ;;
    esac
done

sudo apt-get update
sudo apt-get install --no-install-recommends -y "${pkgs[@]}"
