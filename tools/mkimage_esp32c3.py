#!/usr/bin/env python3
"""mkimage_esp32c3.py — pack the Scorpion kernel ELF into a flashable
Espressif firmware image (image format v1), for ESP32-C3 (RV32) and
ESP32-S3 (Xtensa) alike.

The ESP32 family ROM cannot execute a bare binary from flash: images
must carry an 8-byte header, a 16-byte extended header (wp_pin,
spi_pin_drv, CHIP_ID, ...) plus one descriptor per loadable segment.
This script reads the PT_LOAD program headers straight out of the ELF
(no external dependencies) and emits the exact layout esptool produces:

    [header][extended header]
    [segment desc + data]*
    [pad][XOR checksum byte]

Flash it at offset 0x0 — the boot ROM validates the image, copies each
segment to its VMA and enters the entry point directly (no second-stage
bootloader):

    esptool.py --chip esp32c3 --port /dev/ttyUSB0 \
        --baud 921600 write_flash 0x0 Scorpion_esp32c3.bin

Usage: mkimage_esp32c3.py [--chip c3|s3] <kernel.elf> <output.bin>
       (--chip defaults to c3)
"""

import struct
import sys

IMAGE_MAGIC = 0xE9

# Espressif efuse chip_id values (soc/chip_revision.h / esptool).
CHIP_IDS = {
    "c3": 5,    # ESP32-C3
    "s3": 9,    # ESP32-S3
}

EM_RISCV = 243
EM_XTENSA = 94


def fail(msg):
    sys.exit(f"mkimage_esp32c3: {msg}")


def load_segments(elf, expect_machine):
    """Return (entry, [(vaddr, bytes)]) for PT_LOAD segments of an
    ELF32 little-endian file."""
    if elf[:4] != b"\x7fELF":
        fail("not an ELF file")
    if elf[4] != 1 or elf[5] != 1:
        fail("expected 32-bit little-endian ELF")

    e_entry = struct.unpack_from("<I", elf, 24)[0]
    e_phoff = struct.unpack_from("<I", elf, 28)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 42)

    machine = struct.unpack_from("<H", elf, 18)[0]
    if machine != expect_machine:
        print(f"warning: e_machine {machine} does not match the selected "
              f"chip (expected {expect_machine})")

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _fl, _al = \
            struct.unpack_from("<8I", elf, off)
        if p_type != 1 or p_filesz == 0:      # PT_LOAD with content
            continue
        if p_memsz > p_filesz:
            print(f"note: segment @0x{p_vaddr:08x} has .bss tail "
                  f"({p_memsz - p_filesz} B, zeroed by crt0)")
        segments.append((p_vaddr, elf[p_offset:p_offset + p_filesz]))

    return e_entry, segments


def main(argv):
    args = argv[1:]
    chip = "c3"
    if args and args[0] == "--chip":
        if len(args) < 2 or args[1] not in CHIP_IDS:
            fail("--chip must be one of: " + ", ".join(CHIP_IDS))
        chip = args[1]
        args = args[2:]

    if len(args) != 2:
        fail("usage: mkimage_esp32c3.py [--chip c3|s3] "
             "<kernel.elf> <output.bin>")

    elf_path, out_path = args
    expect_machine = EM_RISCV if chip == "c3" else EM_XTENSA

    with open(elf_path, "rb") as f:
        entry, segments = load_segments(f.read(), expect_machine)

    if not segments:
        fail("ELF has no loadable segments")

    image = bytearray()
    # ---- 8-byte main header -----------------------------------------
    image.append(IMAGE_MAGIC)
    image.append(len(segments))            # segment count
    image.append(0x00)                     # flash mode (leave as-is)
    image.append(0x00)                     # flash speed/size (as-is)
    image += struct.pack("<I", entry)      # entry point
    # ---- 16-byte extended header ------------------------------------
    image.append(0xEE)                     # wp_pin: disabled
    image += b"\x00\x00\x00"               # spi_pin_drv
    image += struct.pack("<H", CHIP_IDS[chip])
    image += struct.pack("<H", 0)          # min_chip_rev
    image += struct.pack("<HH", 0, 0)      # min/max_chip_rev_full
    image += b"\x00" * 4                   # reserved

    checksum = 0
    for vaddr, blob in segments:
        image += struct.pack("<II", len(blob), vaddr)
        image += blob
        for b in blob:
            checksum ^= b

    # Pad so the trailing checksum byte sits on a 16-byte boundary.
    while (len(image) % 16) != 15:
        image.append(0xFF)
    image.append(checksum)

    with open(out_path, "wb") as f:
        f.write(image)

    print(f"mkimage_esp32c3[{chip}]: {out_path} ({len(image)} bytes, "
          f"{len(segments)} segments, chip_id={CHIP_IDS[chip]}, "
          f"entry=0x{entry:08x})")


if __name__ == "__main__":
    main(sys.argv)
