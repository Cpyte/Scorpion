"""
elf2sef.py — ELF (RV32, PIC, --emit-relocs) to Scorpion SEF v2 converter.

Produces a relocatable SEF image (docs/dynamic-linking.md):

  * SEG_TEXT  / SEG_DATA / SEG_BSS  — flattened load image (linked at 0)
  * SEG_RELOC — load-time relocations for absolute references that were
                resolved at link time (R_RISCV_32 data words, lui-based
                HI20/LO12 absolute pairs, .got/.got.plt slots)
  * SEG_IMPORT — unresolved external symbols the loader must bind
  * SEG_EXPORT — symbols other images may import (--export NAME)

Link with:  -fPIC ... -Wl,-q --unresolved-symbols=ignore-all --no-relax

Usage:
  elf2sef.py [--flags N] [--export NAME]... [--export-file FILE] \
             <input.elf> <output.sef>
"""

import struct
import sys

SEF_MAGIC = 0x00464553

SEG_TEXT = 0
SEG_DATA = 1
SEG_BSS = 2
SEG_RELOC = 3
SEG_IMPORT = 4
SEG_EXPORT = 5

SEF_FLAG_PRIV_CONTROLLER = 0x0001
SEF_FLAG_DYNAMIC = 0x0002

SEF_R_RELATIVE = 0
SEF_R_HI20 = 1
SEF_R_LO12I = 2
SEF_R_LO12S = 3
SEF_R_CALL = 4

# ELF constants
SHT_NOBITS = 8
SHT_RELA = 4
SHT_SYMTAB = 2
SHF_ALLOC = 0x2
SHF_WRITE = 0x1
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2
SHN_XINDEX = 0xFFFF

STT_SECTION = 3

# RISC-V relocation types (psABI)
R_RISCV_32 = 1
R_RISCV_HI20 = 26
R_RISCV_LO12_I = 27
R_RISCV_LO12_S = 28
R_RISCV_CALL = 18
R_RISCV_CALL_PLT = 19

# relocations whose value is an absolute address; everything else is
# PC-relative / branch / relax / debug and is left alone at load time
ABSOLUTE_RELOCS = {
    R_RISCV_32: SEF_R_RELATIVE,
    R_RISCV_HI20: SEF_R_HI20,
    R_RISCV_LO12_I: SEF_R_LO12I,
    R_RISCV_LO12_S: SEF_R_LO12S,
    R_RISCV_CALL: SEF_R_CALL,
    R_RISCV_CALL_PLT: SEF_R_CALL,
}

MAPPED_MAX = 0x00100000  # 1 MiB sanity bound for the flat image


class Section:
    def __init__(self, name, type_, flags, addr, offset, size, link, info,
                 entsize):
        self.name = name
        self.type = type_
        self.flags = flags
        self.addr = addr
        self.offset = offset
        self.size = size
        self.link = link
        self.info = info
        self.entsize = entsize


class Symbol:
    def __init__(self, name, value, size, info, shndx):
        self.name = name
        self.value = value
        self.size = size
        self.info = info
        self.shndx = shndx

    @property
    def is_defined(self):
        return self.shndx != SHN_UNDEF and self.shndx != SHN_ABS

    @property
    def is_section(self):
        return (self.info & 0xF) == STT_SECTION


def parse_elf(path):
    with open(path, 'rb') as f:
        data = f.read()

    if data[:4] != b'\x7fELF':
        sys.exit(f'error: {path}: not an ELF file')
    if data[4] != 1:      # ELFCLASS32
        sys.exit(f'error: {path}: not a 32-bit ELF')

    ei_data = data[5]
    endian = '<' if ei_data == 1 else '>'
    if ei_data == 2:
        sys.exit(f'error: {path}: big-endian ELF unsupported')

    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from(endian + 'HHIIIIIHHHHHH', data, 16)

    if e_machine != 0xF3:
        sys.exit(f'error: {path}: not RISC-V (machine={e_machine:#x})')

    if e_shnum == 0 or e_shentsize != 40:
        sys.exit(f'error: {path}: bad section headers')

    shdr = []
    for i in range(e_shnum):
        shdr.append(struct.unpack_from(endian + 'IIIIIIIIII', data,
                                       e_shoff + i * 40))

    # section name string table
    shstr = shdr[e_shstrndx]
    shstr_data = data[shstr[4]:shstr[4] + shstr[5]]

    def cstr(buf, off):
        if off >= len(buf):
            return ''
        end = buf.find(b'\x00', off)
        if end < 0:
            return ''
        return buf[off:end].decode('latin1')

    sections = {}
    by_index = []
    for i, s in enumerate(shdr):
        name = cstr(shstr_data, s[0])
        sec = Section(name, s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[9])
        sections[name] = sec
        by_index.append(sec)

    def sym_by_index(idx):
        return symbols[idx] if idx < len(symbols) else None

    def section_by_index(idx):
        return by_index[idx] if idx < len(by_index) else None

    # symbol table (symbols[i] is ELF symbol index i; [0] is the null symbol)
    symbols = []
    if '.symtab' in sections:
        st = sections['.symtab']
        strtab = section_by_index(st.link)
        str_data = b''
        if strtab is not None:
            str_data = data[strtab.offset:strtab.offset + strtab.size]
        for i in range(st.size // st.entsize):
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from(endian + 'IIIBBH', data,
                                   st.offset + i * st.entsize)
            name = cstr(str_data, st_name)
            symbols.append(Symbol(name, st_value, st_size, st_info, st_shndx))

    # alloc sections in address order
    alloc = [s for s in sections.values()
             if (s.flags & SHF_ALLOC) and s.size > 0]
    alloc.sort(key=lambda s: (s.addr, s.size))

    if not alloc:
        sys.exit(f'error: {path}: no allocated sections')

    base = alloc[0].addr
    mapped_end = max(s.addr + s.size for s in alloc)

    if mapped_end - base > MAPPED_MAX:
        sys.exit(f'error: {path}: image too large '
                 f'({mapped_end - base:#x} bytes)')

    flat = bytearray(mapped_end - base)
    for s in alloc:
        if s.type != SHT_NOBITS:
            flat[s.addr - base:s.addr - base + s.size] = \
                data[s.offset:s.offset + s.size]

    # relocations: site -> (type, sym_index, addend)
    relocs = []
    for sec in sections.values():
        if sec.type != SHT_RELA or sec.link == 0:
            continue
        target = by_index[sec.info] if sec.info < len(by_index) else None
        if target is None or (target.flags & SHF_ALLOC) == 0:
            continue  # .rela.debug* etc.: no memory image
        for i in range(sec.size // sec.entsize):
            r_offset, r_info, r_addend = struct.unpack_from(
                endian + 'IIi', data, sec.offset + i * sec.entsize)
            r_sym = r_info >> 8
            r_type = r_info & 0xFF
            relocs.append((r_offset, r_type, r_sym, r_addend))

    return {
        'entry': e_entry,
        'base': base,
        'mapped_end': mapped_end,
        'flat': flat,
        'sections': sections,
        'by_index': by_index,
        'symbols': symbols,
        'sym_by_index': sym_by_index,
        'section_by_index': section_by_index,
        'relocs': relocs,
    }


def is_absolute_pair(elf, sym_idx):
    """True if the LO12 reloc's symbol points at a `lui` (absolute pair)
    rather than an `auipc` (PC-relative pair)."""
    sym = elf['sym_by_index'](sym_idx)
    if sym is None or not sym.is_defined:
        return False
    off = sym.value - elf['base']
    flat = elf['flat']
    if off + 4 > len(flat):
        return False
    insn = struct.unpack_from('<I', flat, off)[0]
    return (insn & 0x7f) == 0x37  # lui


def main(argv):
    flags = 0
    exports = []
    export_file = None
    positionals = []
    args = list(argv)

    i = 0
    while i < len(args):
        opt = args[i]
        if opt == '--flags':
            flags = int(args[i + 1], 0)
            i += 2
        elif opt == '--export':
            exports.append(args[i + 1])
            i += 2
        elif opt == '--export-file':
            export_file = args[i + 1]
            i += 2
        elif opt.startswith('--'):
            sys.exit(f'error: unknown option {opt}')
        else:
            positionals.append(opt)
            i += 1

    if len(positionals) != 2:
        sys.exit(__doc__)

    elf_path, sef_path = positionals
    elf = parse_elf(elf_path)
    flat = elf['flat']
    base = elf['base']
    sections = elf['sections']

    if export_file:
        with open(export_file) as f:
            exports.extend(line.strip() for line in f if line.strip())

    # --- map / segment layout (linked at 0: vaddr == image offset) ---
    text = sections.get('.text')
    bss = sections.get('.bss')

    if text is None or (text.flags & SHF_ALLOC) == 0:
        sys.exit(f'error: {elf_path}: no allocated .text section')

    text_vaddr = text.addr - base
    text_size = text.size

    if bss is not None and (bss.flags & SHF_ALLOC):
        data_vaddr = text_vaddr + text_size
        data_size = (bss.addr - base) - data_vaddr
        bss_vaddr = bss.addr - base
        bss_size = bss.size
    else:
        data_vaddr = text_vaddr + text_size
        data_size = (elf['mapped_end'] - base) - data_vaddr
        bss_vaddr = 0
        bss_size = 0

    if data_size < 0 or bss_size < 0:
        sys.exit(f'error: {elf_path}: unexpected section order')

    # --- process relocations ---
    reloc_records = []   # (type, offset, value)
    import_records = {}  # name -> (type, slot)  (one slot per name)
    sym_by_index = elf['sym_by_index']
    section_by_index = elf['section_by_index']
    mapped = len(flat)

    for r_offset, r_type, r_sym, r_addend in elf['relocs']:
        if r_type not in ABSOLUTE_RELOCS:
            continue
        if r_offset < base or r_offset - base + 4 > mapped:
            continue  # site outside the load image (debug etc.)

        site = r_offset - base

        if r_type == R_RISCV_LO12_I or r_type == R_RISCV_LO12_S:
            if not is_absolute_pair(elf, r_sym):
                continue  # PC-relative pair; no load-time fixup needed

        sym = sym_by_index(r_sym)
        if sym is None:
            continue
        if not sym.is_defined:
            # unresolved external -> import
            value = sym.value + r_addend
            if sym.name:
                import_records.setdefault(sym.name,
                                          (ABSOLUTE_RELOCS[r_type], site))
            continue

        sec = section_by_index(sym.shndx)
        if sec is not None and (sec.flags & SHF_ALLOC) == 0:
            continue  # e.g. debug symbol

        value = sym.value + r_addend
        reloc_records.append((ABSOLUTE_RELOCS[r_type], site, value))

    # --- scan .got/.got.plt for slots the linker resolved without a reloc ---
    defined_values = set()
    for sym in elf['symbols']:
        if sym is not None and sym.is_defined:
            sec = section_by_index(sym.shndx)
            if sec is not None and (sec.flags & SHF_ALLOC):
                defined_values.add(sym.value)
                if sym.is_section:
                    for delta in range(0, min(sec.size, 0x100), 4):
                        defined_values.add(sym.value + delta)

    plt = sections.get('.plt')
    plt_sec = sections.get('.plt.sec')
    got_records = {}
    for got_name in ('.got', '.got.plt'):
        got = sections.get(got_name)
        if got is None or (got.flags & SHF_ALLOC) == 0:
            continue
        for off in range(0, got.size, 4):
            slot_addr = got.addr + off
            word = struct.unpack_from('<I', flat, slot_addr - base)[0]
            if word == 0 or word == 0xFFFFFFFF:
                continue
            if plt and plt.addr <= word < plt.addr + plt.size:
                continue  # PLT trampoline pointer (external call)
            if plt_sec and plt_sec.addr <= word < plt_sec.addr + plt_sec.size:
                continue
            if word in defined_values:
                got_records.setdefault(slot_addr - base, word)

    for slot, word in got_records.items():
        if not any(r[0] == SEF_R_RELATIVE and r[1] == slot
                   for r in reloc_records):
            reloc_records.append((SEF_R_RELATIVE, slot, word))

    # --- exports ---
    export_records = []
    for name in exports:
        for sym in elf['symbols']:
            if sym is not None and sym.is_defined and sym.name == name:
                export_records.append((sym.value, name))
                break
        else:
            sys.exit(f'error: {elf_path}: export {name!r} not found')

    # --- build SEF ---
    dynamic = bool(reloc_records or import_records or export_records)
    if dynamic:
        flags |= SEF_FLAG_DYNAMIC

    segments = []  # (type, vaddr, size, data)
    segments.append((SEG_TEXT, text_vaddr, text_size, None))
    segments.append((SEG_DATA, data_vaddr, data_size, None))
    if bss_size:
        segments.append((SEG_BSS, bss_vaddr, bss_size, None))

    reloc_data = b''.join(struct.pack('<III', t, o, v)
                          for (t, o, v) in reloc_records)
    if reloc_data:
        segments.append((SEG_RELOC, 0, len(reloc_data), reloc_data))

    import_data = b''
    for name, (rtype, slot) in import_records.items():
        nb = name.encode('latin1')
        import_data += struct.pack('<III', rtype, slot, len(nb)) + nb
        import_data += b'\x00' * ((4 - len(nb) % 4) % 4)
    if import_data:
        segments.append((SEG_IMPORT, 0, len(import_data), import_data))

    export_data = b''
    for value, name in export_records:
        nb = name.encode('latin1')
        export_data += struct.pack('<II', value, len(nb)) + nb
        export_data += b'\x00' * ((4 - len(nb) % 4) % 4)
    if export_data:
        segments.append((SEG_EXPORT, 0, len(export_data), export_data))

    out = bytearray()
    out += struct.pack('<IIHH', SEF_MAGIC, elf['entry'], len(segments), flags)

    dc = 12 + len(segments) * 16
    body = bytearray()
    for st, vaddr, size, payload in segments:
        if payload is None:
            start = dc
            dc += size
            out += struct.pack('<IIII', st, vaddr, size, start)
            body += flat[vaddr:vaddr + size]
        else:
            out += struct.pack('<IIII', st, vaddr, size, dc)
            dc += len(payload)
            body += payload

    out += body

    with open(sef_path, 'wb') as f:
        f.write(out)

    print(f'Created {sef_path}: {len(out)} bytes, {len(segments)} segments, '
          f'entry=0x{elf["entry"]:x}, flags=0x{flags:x}')
    print(f'  relocs={len(reloc_records)} imports={len(import_records)} '
          f'exports={len(export_records)}')


if __name__ == '__main__':
    main(sys.argv[1:])
