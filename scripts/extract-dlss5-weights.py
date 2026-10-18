#!/usr/bin/env python3
"""Extract a user-owned DLSS NR DLL/ZIP into an OpenDLSS-NR model directory.

Usage: python3 scripts/extract-dlss5-weights.py INPUT.zip OUTPUT_DIRECTORY

Only NVIDIA DLL 310.9.16.0 (CL 39025366), distributed in the 2026-10-16
archive, is supported. No DLL code is executed, no files are downloaded, and
no proprietary weights are included in this script. Keep extracted weights
local; build-gcc/dlss5-model is covered by this repository's build*/ ignore.

WEIGHTS_HT is a PE RCDATA resource, not compressed/encrypted model data. It
starts with its u64 byte length, followed by lexically ordered tensor records:
  u64 name length; ASCII name; u64 serialized length; u64 repeated length;
  u64 payload length; u32 type (1); packed payload; u64 zero; u64 rank (1);
  u32 element count (payload length / 2).
The serialized length is payload length + 40, including its repeated length.
Payloads already contain the MMA-fragment E4M3 matrices and f16/f32 auxiliary
values expected by OpenDLSS-NR src/nr_model.cpp and src/nr_graph.cpp. They must
NOT be decoded, transposed, or converted to FP8 again. The eleven files group
consecutive graph stages; tensor names, including the blend scalar and the
eight two-byte ViT layer3 records, are preserved exactly.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import zipfile


DLL_SIZE = 266424944
DLL_SHA256 = "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f"
RESOURCE_SIZE = 147695410
RESOURCE_SHA256 = "836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4"
VERSION = (310, 9, 16, 0)
STAGES = (
    ("encoder32", 0, 4),
    ("encoder64", 5, 8),
    ("encoder128", 9, 14),
    ("encoder256", 15, 22),
    ("encoder512", 23, 30),
    ("vit1024", 31, 38),
    ("decoder512", 39, 47),
    ("decoder256", 48, 55),
    ("decoder128", 56, 61),
    ("decoder64", 62, 65),
    ("decoder32", 66, 70),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unpack(data, offset, fmt):
    require(0 <= offset <= len(data) - struct.calcsize(fmt),
            "truncated binary structure")
    return struct.unpack_from(fmt, data, offset)


def load_dll(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            candidates = [entry for entry in archive.infolist()
                          if entry.filename.replace("\\", "/").split("/")[-1].lower()
                          == "nvngx_dlssnr.dll" and not entry.is_dir()]
            require(len(candidates) == 1,
                    "ZIP must contain exactly one nvngx_dlssnr.dll")
            entry = candidates[0]
            require(entry.file_size == DLL_SIZE, "unsupported DLL size in ZIP")
            require(not entry.flag_bits & 1, "encrypted ZIP is unsupported")
            data = archive.read(entry)
    else:
        require(path.stat().st_size == DLL_SIZE, "unsupported DLL size")
        data = path.read_bytes()
    require(len(data) == DLL_SIZE, "unsupported DLL size")
    digest = hashlib.sha256(data).hexdigest()
    require(digest == DLL_SHA256,
            "unsupported DLL SHA-256: " + digest + "; expected " + DLL_SHA256)
    return data


def pe_resources(data):
    require(data[:2] == b"MZ", "not a PE DLL")
    pe, = unpack(data, 0x3C, "<I")
    require(data[pe:pe + 4] == b"PE\0\0", "invalid PE signature")
    machine, section_count = unpack(data, pe + 4, "<HH")
    optional_size, = unpack(data, pe + 20, "<H")
    optional = pe + 24
    magic, = unpack(data, optional, "<H")
    require(machine == 0x8664 and magic == 0x20B, "expected x86-64 PE32+ DLL")
    directory_count, = unpack(data, optional + 108, "<I")
    require(directory_count >= 3 and optional_size >= 136,
            "missing PE resource directory")
    resource_rva, resource_size = unpack(data, optional + 128, "<II")
    sections = []
    for index in range(section_count):
        section = optional + optional_size + index * 40
        _, rva, size, offset = unpack(data, section + 8, "<IIII")
        require(offset + size <= len(data), "PE section exceeds DLL")
        sections.append((rva, size, offset))

    def file_offset(rva, size):
        for start, length, offset in sections:
            if start <= rva and rva + size <= start + length:
                return offset + rva - start
        raise ValueError("resource RVA exceeds file-backed PE sections")

    base = file_offset(resource_rva, resource_size)
    directory = memoryview(data)[base:base + resource_size]
    entries = {}

    def walk(relative, path):
        require(len(path) < 3, "unsupported PE resource nesting")
        named, numbered = unpack(directory, relative + 12, "<HH")
        for index in range(named + numbered):
            name, child = unpack(directory, relative + 16 + index * 8, "<II")
            if name & 0x80000000:
                name_offset = name & 0x7FFFFFFF
                chars, = unpack(directory, name_offset, "<H")
                end = name_offset + 2 + chars * 2
                require(end <= len(directory), "truncated resource name")
                name = bytes(directory[name_offset + 2:end]).decode("utf-16le")
            key = path + (name,)
            if child & 0x80000000:
                walk(child & 0x7FFFFFFF, key)
            else:
                rva, size, _, reserved = unpack(directory, child, "<IIII")
                require(len(key) == 3 and key not in entries and reserved == 0,
                        "invalid or duplicate resource entry")
                entries[key] = (file_offset(rva, size), size)

    walk(0, ())
    return entries


def fused_size(channels, upsample=False, transition=False):
    # FusedLayout / upsampleFusedLayout, including trailing alignment bytes.
    if channels == 32:
        ffn = 2 * channels * 128
    else:
        experts = channels // 32
        ffn = experts * channels * 128 + experts * 128 * 32 + channels * channels
    heads = channels // 32
    if upsample:
        prefix = ffn + 2 * channels * channels + (32 if channels == 32 else 0)
        prefix += 4 * channels
    else:
        prefix = ffn + 32 + 2 * channels
    end = prefix + 4 * channels * channels + heads * 8192
    end += ((heads * 4 + 15) // 16) * 16 + 2 * channels
    if transition:
        # The serialized 32 -> 64 transition also retains 16 trailing zero bytes.
        return end + 2 * channels * channels + (16 if channels == 32 else 0)
    return end + 16


def expected_tensors():
    expected = {}

    def add(block, layer, length, parameter="layer"):
        expected[f"block{block}.layer{layer}.{parameter}"] = length

    add(0, 0, 21696)
    for first, last, channels in ((1, 4, 32), (5, 8, 64),
                                  (9, 14, 128), (15, 22, 256)):
        for block in range(first, last + 1):
            add(block, 0, fused_size(channels, transition=block == last))
    for block in list(range(23, 31)) + list(range(40, 48)):
        for layer, length in enumerate((524288, 263168, 917568, 263168)):
            add(block, layer, length)
    add(30, 4, 512 * 1024 + 16)
    for block in range(31, 39):
        for layer, length in enumerate((4194320, 4196352, 3145856, 2, 1050624)):
            add(block, layer, length)
    add(39, 0, 1024 * 512 + 512 * 2)
    for first, last, channels in ((48, 55, 256), (56, 61, 128),
                                  (62, 65, 64), (66, 69, 32)):
        for block in range(first, last + 1):
            add(block, 0, fused_size(channels, upsample=block == first))
    add(70, 0, 2, "blend_scale")
    add(70, 0, 21808)
    return expected


def parse_tensors(resource):
    size, = unpack(resource, 0, "<Q")
    require(size == len(resource), "resource length header mismatch")
    expected = expected_tensors()
    tensors = []
    position = 8
    previous_name = ""
    while position < len(resource):
        name_length, = unpack(resource, position, "<Q")
        position += 8
        require(1 <= name_length <= 64 and position + name_length <= len(resource),
                "invalid tensor name length")
        name = bytes(resource[position:position + name_length]).decode("ascii")
        position += name_length
        match = re.fullmatch(r"block(\d+)\.layer(\d+)\.(layer|blend_scale)", name)
        require(match is not None and name in expected and name > previous_name,
                "unexpected, duplicate, or unordered tensor: " + name)
        previous_name = name
        outer, inner, length, kind = unpack(resource, position, "<QQQI")
        position += 28
        require(outer == inner == length + 40 and kind == 1,
                "unsupported tensor serialization: " + name)
        require(length == expected[name], "graph tensor layout mismatch: " + name)
        require(position + length + 20 <= len(resource), "truncated tensor: " + name)
        zero, rank, elements = unpack(resource, position + length, "<QQI")
        require(zero == 0 and rank == 1 and elements * 2 == length,
                "unsupported tensor shape: " + name)
        tensors.append({"name": name, "block": int(match[1]), "layer": int(match[2]),
                        "parameter": match[3], "resourceOffset": position,
                        "byteLength": length})
        position += length + 20
    require(position == len(resource) and len(tensors) == len(expected) == 153,
            "incomplete 153-tensor model")
    require({tensor["block"] for tensor in tensors} == set(range(71)),
            "incomplete 71-block model")
    return sorted(tensors, key=lambda tensor: (tensor["block"], tensor["layer"],
                                              tensor["parameter"]))


def extract(source, output):
    require(not output.exists() and not output.is_symlink(),
            "output already exists; choose a new directory (existing models are never overwritten)")
    dll = load_dll(source)
    resources = pe_resources(dll)
    require((16, 1, 1033) in resources, "missing DLL version resource")
    version_offset, version_size = resources[(16, 1, 1033)]
    version_data = dll[version_offset:version_offset + version_size]
    signature = version_data.find(struct.pack("<I", 0xFEEF04BD))
    require(signature >= 0, "missing VS_FIXEDFILEINFO")
    ms, ls = unpack(version_data, signature + 8, "<II")
    require((ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF) == VERSION,
            "unsupported DLL version")
    key = (10, "WEIGHTS_HT", 1033)
    require(key in resources, "missing WEIGHTS_HT RCDATA resource")
    resource_offset, resource_size = resources[key]
    require(resource_size == RESOURCE_SIZE, "unsupported WEIGHTS_HT size")
    resource = memoryview(dll)[resource_offset:resource_offset + resource_size]
    require(hashlib.sha256(resource).hexdigest() == RESOURCE_SHA256,
            "unsupported WEIGHTS_HT SHA-256")
    tensors = parse_tensors(resource)
    manifest = {
        "format": "OpenDLSS-NR packed E4M3 stages",
        "source": {"dllVersion": ".".join(map(str, VERSION)), "dllSha256": DLL_SHA256,
                   "resource": "WEIGHTS_HT", "resourceLanguage": 1033,
                   "resourceFileOffset": resource_offset, "resourceByteLength": resource_size,
                   "resourceSha256": RESOURCE_SHA256},
        "totals": {"blockCount": 71, "tensorCount": 153, "stageCount": 11,
                   "packedByteLength": sum(tensor["byteLength"] for tensor in tensors)},
        "stages": [], "tensors": tensors,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="." + output.name + "-", dir=output.parent))
    try:
        (temporary / "model").mkdir()
        for stage_id, first, last in STAGES:
            filename = stage_id + ".bin"
            digest = hashlib.sha256()
            stage_offset = 0
            with (temporary / "model" / filename).open("wb") as stage_file:
                for tensor in tensors:
                    if not first <= tensor["block"] <= last:
                        continue
                    offset, length = tensor["resourceOffset"], tensor["byteLength"]
                    payload = resource[offset:offset + length]
                    stage_file.write(payload)
                    digest.update(payload)
                    tensor.update(stage=stage_id, stageOffset=stage_offset,
                                  dllOffset=resource_offset + offset,
                                  sha256=hashlib.sha256(payload).hexdigest())
                    stage_offset += length
            manifest["stages"].append({"id": stage_id, "file": filename,
                                       "packedByteLength": stage_offset,
                                       "sha256": digest.hexdigest()})
        encoded = (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
        (temporary / "manifest.json").write_bytes(encoded)
        # Publish only a completely parsed/written model; never replace an existing one.
        require(not output.exists() and not output.is_symlink(), "output appeared during extraction")
        os.rename(temporary, output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f"Extracted {len(tensors)} tensors / 71 blocks / 11 stages to {output}")
    print(f"Packed bytes: {manifest['totals']['packedByteLength']}")
    print(f"DLL SHA-256: {DLL_SHA256}")
    print(f"WEIGHTS_HT SHA-256: {RESOURCE_SHA256}")
    print(f"Manifest SHA-256: {hashlib.sha256(encoded).hexdigest()}")
    for stage in manifest["stages"]:
        print(f"  {stage['id']}: {stage['packedByteLength']} bytes  {stage['sha256']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="user-supplied nvngx_dlssnr DLL or ZIP")
    parser.add_argument("output", type=Path, help="new model directory (must not already exist)")
    args = parser.parse_args()
    try:
        extract(args.input, args.output)
    except (OSError, ValueError, struct.error, zipfile.BadZipFile,
            NotImplementedError, RuntimeError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
