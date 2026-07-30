import struct, sys, subprocess, os

def build(elf_path, sexec_output, flags=0):
    sections = {}
    result = subprocess.run(
        ['riscv64-elf-objdump', '-h', elf_path],
        capture_output=True, text=True
    )
    for line in result.stdout.split('\n'):
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit():
            name = parts[1]
            if name in ('.text', '.rodata', '.data', '.bss'):
                sections[name] = int(parts[2], 16)

    result = subprocess.run(
        ['riscv64-elf-readelf', '-h', elf_path],
        capture_output=True, text=True
    )
    entry = 0
    for line in result.stdout.split('\n'):
        if 'Entry point address' in line:
            entry = int(line.split(':')[1].strip(), 16)

    result = subprocess.run(
        ['riscv64-elf-objcopy', '-O', 'binary', elf_path, elf_path + '.bin'],
        capture_output=True, text=True
    )
    with open(elf_path + '.bin', 'rb') as f:
        flat = f.read()
    os.unlink(elf_path + '.bin')

    segments = []
    off = 0
    if '.text' in sections:
        segments.append((0, off, sections['.text']))
        off += sections['.text']
    if '.rodata' in sections:
        segments.append((1, off, sections['.rodata']))
        off += sections['.rodata']
    if '.data' in sections:
        segments.append((1, off, sections['.data']))
        off += sections['.data']
    if '.bss' in sections:
        segments.append((2, off, sections['.bss']))

    num = len(segments)
    hdr = 12 + num * 16

    out = bytearray()
    out += struct.pack('<IIHH', 0x45584553, entry, num, flags)
    dc = hdr
    for st, sv, ss in segments:
        out += struct.pack('<IIII', st, sv, ss, dc)
        dc += ss
    out += flat

    with open(sexec_output, 'wb') as f:
        f.write(out)

    print(f"Created {sexec_output}: {len(out)} bytes, {num} segments, entry=0x{entry:x}, flags=0x{flags:x}")

def build_h(sexec_path, h_output):
    with open(sexec_path, 'rb') as f:
        data = f.read()
    base = os.path.splitext(os.path.basename(sexec_path))[0]
    guard = f'SCORPION_{base.upper()}_SEXEC_H'
    define = f'{base.upper()}_SEXEC_SIZE'
    var = f'{base}_sexec'
    with open(h_output, 'w') as f:
        f.write(f'#ifndef {guard}\n#define {guard}\n\n')
        f.write(f'#define {define} {len(data)}\n\n')
        f.write(f'static const unsigned char {var}[] = {{\n')
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            f.write('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',\n')
        f.write('};\n\n#endif\n')
    print(f"Generated {h_output} from {sexec_path}")

if __name__ == '__main__':
    flags = 0
    args = sys.argv[1:]
    if '--flags' in args:
        idx = args.index('--flags')
        flags = int(args[idx + 1], 0)
        args = args[:idx] + args[idx+2:]
    elf_path, sexec_output = args[0], args[1]
    build(elf_path, sexec_output, flags)
