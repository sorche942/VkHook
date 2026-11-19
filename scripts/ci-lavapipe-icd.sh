#!/usr/bin/env bash
# Print the path of the native-architecture lavapipe ICD, or fail.
#
# Distros differ: Ubuntu ships an arch-independent lvp_icd.json where Fedora ships
# lvp_icd.x86_64.json, and several ship an i686 ICD next to the native one, so a
# bare `head -1` can pick the 32-bit driver and vkcube then reports no compatible
# ICD. Prefer the native arch, fall back to the unsuffixed name, never take i686.
set -euo pipefail
dir=/usr/share/vulkan/icd.d
for candidate in "$dir/lvp_icd.$(uname -m).json" "$dir/lvp_icd.json"; do
    if [[ -f "$candidate" ]]; then
        echo "$candidate"
        exit 0
    fi
done
found=$(ls "$dir"/lvp_icd*.json 2>/dev/null | grep -v i686 | head -1 || true)
if [[ -n "$found" ]]; then
    echo "$found"
    exit 0
fi
echo "lavapipe ICD not found; mesa-vulkan-drivers did not install" >&2
ls "$dir" >&2 || true
exit 1
