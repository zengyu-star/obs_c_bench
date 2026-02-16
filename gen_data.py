#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys
import os

OUTPUT_FILE = "test_data.bin"
FILE_SIZE_MB = 10 
PATTERN_SIZE = 1024 * 1024 

# LCG 参数 (与 C 代码完全一致)
LCG_A = 1664525
LCG_C = 1013904223
LCG_SEED = 0
LCG_MOD = 255 

def generate_pattern_buffer(size, seed):
    print(f"Generating {size/1024/1024:.0f}MB pattern buffer in memory...")
    buf = bytearray(size)
    for i in range(size):
        val = (i * LCG_A + LCG_C + seed) % LCG_MOD
        buf[i] = val
    return buf

def write_file(filename, size_mb, pattern_buf):
    total_bytes = int(size_mb * 1024 * 1024)
    written = 0
    print(f"Writing {size_mb}MB to '{filename}' ...")
    with open(filename, 'wb') as f:
        while written < total_bytes:
            remaining = total_bytes - written
            to_write = min(remaining, len(pattern_buf))
            if to_write == len(pattern_buf):
                f.write(pattern_buf)
            else:
                f.write(pattern_buf[:to_write])
            written += to_write
    print(f"\nDone! File '{filename}' generated ({written} bytes).")

if __name__ == "__main__":
    size_mb = FILE_SIZE_MB
    filename = OUTPUT_FILE
    if len(sys.argv) > 1:
        size_mb = float(sys.argv[1])
    if len(sys.argv) > 2:
        filename = sys.argv[2]
    pattern = generate_pattern_buffer(PATTERN_SIZE, LCG_SEED)
    write_file(filename, size_mb, pattern)