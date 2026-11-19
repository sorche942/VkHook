#!/usr/bin/env python3
"""Compile every ReShade .fx shader of the packages in assets/EffectPackages.ini.

The manifest is the repository's own copy, and each package is downloaded at the
commit pinned in src/tests/fixtures/effect-package-pins.ini, never from a moving
branch archive.
"""

import argparse
import configparser
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "assets" / "EffectPackages.ini"
DEFAULT_PINS = Path(__file__).resolve().parent / "fixtures" / "effect-package-pins.ini"
GITHUB_ARCHIVE = re.compile(r"^https://github\.com/([^/]+)/([^/]+)/archive/(?:refs/heads/)?([^/]+?)\.zip$")


def read_ini(path: Path) -> configparser.ConfigParser:
    config = configparser.ConfigParser(strict=False, interpolation=None)
    config.optionxform = str
    config.read_string(path.read_text(encoding="utf-8-sig"))
    return config


def pinned_url(section: str, url: str, pins: configparser.ConfigParser) -> str:
    """The archive URL of the commit pinned for `section`."""
    match = GITHUB_ARCHIVE.match(url)
    if not match:
        raise RuntimeError(f"[{section}] DownloadUrl is not a github archive URL: {url}")
    if not pins.has_option(section, "commit"):
        raise RuntimeError(f"[{section}] has no pinned commit in the pins file; run with --update-pins")
    commit = pins[section]["commit"].strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise RuntimeError(f"[{section}] pinned commit is not a 40-digit sha: {commit}")
    return f"https://github.com/{match.group(1)}/{match.group(2)}/archive/{commit}.zip"


def update_pins(manifest: Path, pins_path: Path) -> None:
    """Resolve each package's branch to its current commit and rewrite the pins."""
    config = read_ini(manifest)
    out = []
    for section in config.sections():
        url = config[section].get("DownloadUrl")
        if not url:
            continue
        match = GITHUB_ARCHIVE.match(url)
        if not match:
            raise RuntimeError(f"[{section}] DownloadUrl is not a github archive URL: {url}")
        owner, repo, ref = match.groups()
        listing = subprocess.run(
            ["git", "ls-remote", f"https://github.com/{owner}/{repo}.git", f"refs/heads/{ref}"],
            check=True, capture_output=True, text=True).stdout.split()
        if not listing:
            raise RuntimeError(f"[{section}] branch {ref} not found in {owner}/{repo}")
        out.append(f"[{section}]\ncommit={listing[0]}\n")
    header = "".join(line for line in pins_path.read_text().splitlines(keepends=True) if line.startswith(";"))
    pins_path.write_text(header + "".join(out))
    print(f"wrote {pins_path}")


def download(url: str, destination: Path, cache_dir: Path | None) -> None:
    cached = cache_dir / (re.sub(r"[^A-Za-z0-9._-]", "_", url)) if cache_dir else None
    if cached and cached.is_file():
        shutil.copyfile(cached, destination)
        return
    request = urllib.request.Request(url, headers={"User-Agent": "VkHook-tests"})
    with urllib.request.urlopen(request, timeout=120) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)
    if cached:
        cache_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(destination, cached)


def install_manifest_packages(manifest: Path, pins: configparser.ConfigParser, work_dir: Path, shader_dir: Path,
                              cache_dir: Path | None) -> None:
    config = read_ini(manifest)
    shader_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    installed = 0
    for section in config.sections():
        package = config[section]
        name = package.get("PackageName", section)
        url = package.get("DownloadUrl")
        if not url:
            continue

        install_path = package.get("InstallPath", r".\reshade-shaders\Shaders")
        install_path = install_path.replace("\\", "/").lstrip("./")
        subdir = install_path.replace("reshade-shaders/Shaders", "", 1).strip("/")
        target_dir = shader_dir / subdir if subdir else shader_dir
        target_dir.mkdir(parents=True, exist_ok=True)

        allowed = {value.strip() for value in package.get("EffectFiles", "").split(",") if value.strip()}
        denied = {value.strip() for value in package.get("DenyEffectFiles", "").split(",") if value.strip()}
        archive_path = work_dir / f"{section}.zip"
        extract_dir = work_dir / section
        try:
            download(pinned_url(section, url, pins), archive_path, cache_dir)
            extract_dir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(archive_path) as archive:
                for member in archive.infolist():
                    member_path = Path(member.filename)
                    if member_path.is_absolute() or ".." in member_path.parts:
                        continue
                    if member.is_dir():
                        continue
                    filename = member_path.name
                    if filename in denied or not filename.endswith((".fx", ".fxh")):
                        continue
                    if filename.endswith(".fx") and allowed and filename not in allowed:
                        continue
                    output_path = extract_dir / filename
                    with archive.open(member) as source, output_path.open("wb") as output:
                        shutil.copyfileobj(source, output)

            copied = 0
            for source in extract_dir.iterdir():
                if source.is_file() and source.name.endswith((".fx", ".fxh")):
                    shutil.copy2(source, target_dir / source.name)
                    copied += 1
            if copied == 0:
                failures.append(f"{name}: package yielded no matching shader files")
            else:
                installed += 1
                print(f"fetched {name}: {copied} shader/include file(s)", flush=True)
        except Exception as error:  # Network and archive failures need package context.
            failures.append(f"{name}: {error}")

    if failures:
        raise RuntimeError("Package fetch failures:\n  " + "\n  ".join(failures))
    if installed == 0:
        raise RuntimeError("EffectPackages.ini did not provide any shader packages")


def compile_shaders(shader_check: Path, shader_dir: Path) -> int:
    shaders = sorted(shader_dir.rglob("*.fx"))
    if not shaders:
        print(f"No .fx shaders found under {shader_dir}", file=sys.stderr)
        return 1

    include_dirs = sorted({shader_dir, *(path.parent for path in shader_dir.rglob("*.fxh"))})
    command_prefix = [str(shader_check)]
    for include_dir in include_dirs:
        command_prefix.extend(["--include", str(include_dir)])

    failures = []
    print(f"compiling {len(shaders)} ReShade shader(s) from {shader_dir}", flush=True)
    for index, shader in enumerate(shaders, start=1):
        result = subprocess.run(
            [*command_prefix, str(shader)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            details = (result.stdout + result.stderr).strip()
            failures.append((shader, details))
            print(f"failed [{index}/{len(shaders)}]: {shader}", flush=True)
        elif index % 25 == 0 or index == len(shaders):
            print(f"compiled [{index}/{len(shaders)}]", flush=True)

    if failures:
        print(f"\n{len(failures)} of {len(shaders)} shader(s) failed:", file=sys.stderr)
        for shader, details in failures:
            print(f"\n--- {shader} ---\n{details}", file=sys.stderr)
        return 1

    print(f"all {len(shaders)} ReShade shader(s) compiled successfully", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-check", type=Path, help="vkhook-shader-check executable")
    parser.add_argument("--shader-dir", type=Path, help="use an existing installed shader tree")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="EffectPackages.ini (default: the repository's)")
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS, help="package commit pins")
    parser.add_argument("--cache-dir", type=Path, help="keep downloaded archives here between runs")
    parser.add_argument("--update-pins", action="store_true", help="re-resolve every package's branch head into the pins file and exit")
    args = parser.parse_args()

    if args.update_pins:
        try:
            update_pins(args.manifest, args.pins)
        except Exception as error:
            print(error, file=sys.stderr)
            return 1
        return 0
    if not args.shader_check:
        parser.error("--shader-check is required")
    if not args.shader_check.is_file():
        parser.error(f"shader checker does not exist: {args.shader_check}")
    if args.shader_dir and not args.shader_dir.is_dir():
        parser.error(f"shader directory does not exist: {args.shader_dir}")
    if not args.manifest.is_file():
        parser.error(f"manifest does not exist: {args.manifest}")

    with tempfile.TemporaryDirectory(prefix="vkhook-shader-tests-") as temporary:
        work_dir = Path(temporary)
        shader_dir = args.shader_dir
        if shader_dir is None:
            shader_dir = work_dir / "Shaders"
            try:
                install_manifest_packages(args.manifest, read_ini(args.pins), work_dir, shader_dir, args.cache_dir)
            except Exception as error:
                print(error, file=sys.stderr)
                return 1

        return compile_shaders(args.shader_check.resolve(), shader_dir.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
