#!/usr/bin/env python3
"""Build exactly the upstream scripts/build_shaders.ps1 PTX variants on any host."""
import argparse
from pathlib import Path
import subprocess
import sys


def variants():
    result = {}

    def add(name, generator, before, after=()):
        result[name + '.ptx'] = (generator + '.py', tuple(map(str, before)), tuple(map(str, after)))

    for k in (64, 128, 256):
        add(f'mlp_e4m3_K{k}', 'mlp_e4m3', (k,), (3, 1, 1))
    for c in (64, 128, 256, 512):
        add(f'qkv_e4m3_K{c}', 'qkv_e4m3', (c,), (80,))
    for k in (32, 64, 128, 256, 512, 1024):
        for flags in (5, 13, 4, 8, 6):
            add(f'gemm2_e4m3_K{k}_f{flags}', 'gemm2_e4m3', (k, flags))
    for c, rows, regs in ((64, 4, 0), (128, 4, 0), (256, 3, 80), (256, 4, 64)):
        add(f'ffn_e4m3_C{c}_R{rows}', 'ffn_e4m3', (c, rows), (regs,))
        add(f'ffn_e4m3_C{c}_R{rows}_proj', 'ffn_e4m3', (c, rows), (regs, 1))
    for k, flags, steps, splits in ((4096, 1, 32, 4), (1024, 0, 16, 2), (1024, 1, 8, 4), (1024, 0, 8, 4)):
        add(f'gemmt_e4m3_K{k}_f{flags}_p{steps}_s{splits}', 'gemmt_e4m3', (k, flags, steps, splits))
    for splits, flags in ((4, 4), (2, 8), (4, 8)):
        add(f'reduce_e4m3_s{splits}_f{flags}', 'reduce_e4m3', (splits, flags))
    for k, flags, splits in ((1024, 6, 1), (4096, 5, 4), (1024, 8, 2), (1024, 5, 4)):
        name = f'gemmv_e4m3_K{k}_f{flags}_s{splits}'
        add(name, 'gemmv_e4m3', (k, flags, splits))
        add(name + '_m96', 'gemmv_e4m3', (k, flags, splits), (4, 6, 0, 0, 0, 96))
    for padded in (64, 128, 192, 256):
        add(f'global_attention_e4m3_p{padded}', 'global_attention_e4m3', (padded,))
    add('global_normalize_e4m3', 'global_attention_stream_e4m3', ('normalize',))
    add('global_attention_stream_e4m3', 'global_attention_stream_e4m3', ('attention',), (4,))
    for k, flags in ((512, 4), (512, 5), (512, 13), (256, 5), (4096, 5), (1024, 8), (1024, 5)):
        add(f'gemmv_e4m3_K{k}_f{flags}_s1_m96', 'gemmv_e4m3', (k, flags, 1), (4, 6, 0, 0, 0, 96))
    for flags in (2, 66, 74, 130, 48):
        add(f'block32_e4m3_f{flags}', 'block32_e4m3', (flags,))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--generate', choices=variants())
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.list:
        print('\n'.join(variants()))
        return
    if args.generate is None or args.output is None:
        parser.error('--generate and --output are required unless --list is used')
    generator, before, after = variants()[args.generate]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, '-B', str(Path(__file__).parent / 'ptx' / generator),
                    *before, str(args.output), *after], check=True)


if __name__ == '__main__':
    main()
