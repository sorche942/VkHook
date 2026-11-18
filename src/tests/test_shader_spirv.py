#!/usr/bin/env python3
"""Validate generated shader modules, not just successful frontend parsing."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

checker, validator, fixture_name = sys.argv[1:]
fixture = Path(__file__).resolve().parent / "fixtures" / fixture_name
with tempfile.TemporaryDirectory(prefix="vkhook-shader-spirv-") as directory:
    module = Path(directory) / "shader.spv"
    subprocess.run(
        [checker, str(fixture), "--include", str(fixture.parent)],
        env={**os.environ, "VKHOOK_DUMP_SPIRV": str(module)},
        check=True,
    )
    subprocess.run([validator, "--target-env", "vulkan1.3", str(module)], check=True)
