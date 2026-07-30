#! /usr/bin/env python3
"""Package kernel + controller into a UF2 for RP2350 RISC-V (Pico 2).

Flash layout:
  0x00000000  boot2 (256 bytes, assembled from source)
  0x00000100  kernel binary (loaded by boot ROM to SRAM @ 0x20000000)
   0x00010000  [4-byte size][controller sef]  (read via XIP by init)
"""

import struct
import subprocess
import sys
import os
import tempfile

UF2_MAGIC_START = 0x0A324655
UF2_MAGIC_END   = 0x0AB16F30
BLOCK_SIZE      = 512
DATA_SIZE       = 476
FLAG_DATA       = 0x00002000

BOOT2_FLASH_OFFSET   = 0x00000000
KERNEL_FLASH_OFFSET  = 0x00000100
CTRL_FLASH_OFFSET    = 0x00010000

BOOT2_ASM = """
.text
.globl _start
_start:
    mv   t0, ra
    lui  a3, 0x40080
    lui  a0, 0x1
    addi a0, a0, 0x021
    sw   a0, 0x00(a3)
    li   a0, 3
    sw   a0, 0x04(a3)
    lui  a0, 0xa40
    sw   a0, 0x08(a3)
    jr   t0
"""

def assemble_boot2():
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'boot2.S')
        obj = os.path.join(tmp, 'boot2.o')
        elf = os.path.join(tmp, 'boot2.elf')
        raw = os.path.join(tmp, 'boot2.bin')
        with open(src, 'w') as f:
            f.write(BOOT2_ASM)
        subprocess.run(['riscv64-elf-as', '-march=rv32imac', '-mabi=ilp32',
                        '-o', obj, src], check=True, capture_output=True)
        subprocess.run(['riscv64-elf-ld', '-m', 'elf32lriscv', '-e', '_start',
                        '--no-relax', '-Ttext=0', '-o', elf, obj],
                       check=True, capture_output=True)
        subprocess.run(['riscv64-elf-objcopy', '-O', 'binary', elf, raw],
                       check=True, capture_output=True)
        with open(raw, 'rb') as f:
            return f.read()

def uf2_block(data, addr, block_no, total):
    chunk = data[:DATA_SIZE]
    # UF2 header is 32 bytes: 8 x uint32 (the 8th word is reserved/zero)
    hdr = struct.pack('<IIIIIIII',
        UF2_MAGIC_START, FLAG_DATA, addr, len(chunk),
        block_no, total, 0, 0)
    return hdr + chunk.ljust(DATA_SIZE, b'\x00') + struct.pack('<I', UF2_MAGIC_END)


def uf2_blocks(data, flash_offset, block_start, total):
    blocks = bytearray()
    off = 0
    for i in range((len(data) + DATA_SIZE - 1) // DATA_SIZE):
        addr = flash_offset + off
        blocks.extend(uf2_block(data[off:], addr, block_start + i, total))
        off += DATA_SIZE
    return blocks

def main(kernel_bin, ctrl_sef, output_uf2):
    boot2 = assemble_boot2().ljust(256, b'\x00')
    with open(kernel_bin, 'rb') as f:
        kernel = f.read()
    with open(ctrl_sef, 'rb') as f:
        ctrl = f.read()

    ctrl_packed = struct.pack('<I', len(ctrl)) + ctrl

    n_b = 1
    n_k = (len(kernel) + DATA_SIZE - 1) // DATA_SIZE
    n_c = (len(ctrl_packed) + DATA_SIZE - 1) // DATA_SIZE
    total = n_b + n_k + n_c

    uf2 = bytearray()
    uf2.extend(uf2_blocks(boot2, BOOT2_FLASH_OFFSET, 0, total))
    uf2.extend(uf2_blocks(kernel, KERNEL_FLASH_OFFSET, n_b, total))
    uf2.extend(uf2_blocks(ctrl_packed, CTRL_FLASH_OFFSET, n_b + n_k, total))

    with open(output_uf2, 'wb') as f:
        f.write(uf2)

    print(f"packrom: {output_uf2}: {total} blocks "
          f"(boot2=256, kernel={len(kernel)}, ctrl={len(ctrl_packed)})")

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <kernel.bin> <controller.sef> <output.uf2>")
        sys.exit(1)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
