"""HCTP SLES_520.36 (PS2 EE) disassembler with symbols.

    python eedis.py sym <regex>            list symbols (addr size name, demangled-ish)
    python eedis.py dis <name|0xaddr> [n]  disassemble a function (or n instructions)
    python eedis.py xref <name|0xaddr>     functions that call / reference an address
"""
import os
import re
import struct
import sys

import capstone

ELF = os.environ.get('HCTP_ELF', '')  # (the disc's SLES_520.36)


def _read_elf(path):
    """the 'main' section (address, bytes) and .symtab as (value, size, name, 'STT_FUNC'|...) - a 32-bit LE ELF,
    read with struct (no pyelftools needed)"""
    if not path:
        raise SystemExit('set HCTP_ELF to the HCTP disc\'s SLES_520.36')
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    names = secs[shstrndx]

    def cstr(off):
        return d[off:d.index(b'\0', off)].decode('latin-1')
    by = {cstr(names[4] + s[0]): s for s in secs}
    m = by['main']
    syms = []
    st, strtab = by['.symtab'], secs[by['.symtab'][6]]
    for o in range(st[4], st[4] + st[5], 16):
        nm, val, size, info = struct.unpack_from('<IIIB', d, o)
        if nm and val:
            syms.append((val, size, cstr(strtab[4] + nm), 'STT_FUNC' if info & 0xF == 2 else 'other'))
    return m[3], d[m[4]:m[4] + m[5]], syms


BASE, CODE, SYMS = _read_elf(ELF)
SYMS.sort()
BY_ADDR = {}
for a, sz, n, t in SYMS:
    BY_ADDR.setdefault(a, n)
BY_NAME = {n: (a, sz) for a, sz, n, t in SYMS}
FUNCS = [(a, sz, n) for a, sz, n, t in SYMS if t == 'STT_FUNC']
_md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)


def func_at(addr):
    lo, hi = 0, len(FUNCS)
    while lo < hi:
        mid = (lo + hi) // 2
        if FUNCS[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo and FUNCS[lo - 1][0] <= addr < FUNCS[lo - 1][0] + max(4, FUNCS[lo - 1][1]):
        return FUNCS[lo - 1]
    return None


def name_of(addr):
    if addr in BY_ADDR:
        return BY_ADDR[addr]
    f = func_at(addr)
    if f:
        return f'{f[2]}+{addr - f[0]:#x}'
    # nearest data symbol
    best = None
    for a, sz, n, t in SYMS:
        if a <= addr < a + max(sz, 1):
            best = f'{n}+{addr - a:#x}'
    return best


def u32(addr):
    return struct.unpack_from('<I', CODE, addr - BASE)[0]


def resolve(x):
    if x.startswith('0x'):
        a = int(x, 16)
        f = func_at(a)
        return (a, f[1] if f and f[0] == a else 0x100)
    if x in BY_NAME:
        return BY_NAME[x]
    cands = [n for n in BY_NAME if x in n]
    if len(cands) == 1:
        return BY_NAME[cands[0]]
    raise SystemExit(f'{x}: {len(cands)} matches: {cands[:20]}')


def disasm(addr, size):
    out = []
    regs = {}
    for off in range(0, size, 4):
        a = addr + off
        w = CODE[a - BASE:a - BASE + 4]
        ins = list(_md.disasm(w, a))
        if not ins:
            out.append(f'{a:08x}:  .word {struct.unpack("<I", w)[0]:08x}')
            continue
        i = ins[0]
        txt = f'{a:08x}:  {i.mnemonic:8s} {i.op_str}'
        ops = i.op_str.split(', ')
        note = ''
        if i.mnemonic in ('jal', 'j'):
            t = int(ops[-1], 16)
            note = name_of(t) or ''
        if i.mnemonic == 'lui':
            regs[ops[0]] = int(ops[1], 16) << 16
        elif i.mnemonic in ('addiu', 'ori', 'lw', 'sw', 'lh', 'lhu', 'lb', 'lbu', 'sh', 'sb', 'lwc1', 'swc1', 'ld', 'sd', 'lq', 'sq', 'daddiu'):
            m = re.match(r'(\$\w+), (-?0x[0-9a-f]+|-?\d+)\((\$\w+)\)', i.op_str)
            if m and m.group(3) in regs:
                v = (regs[m.group(3)] + int(m.group(2), 0)) & 0xFFFFFFFF
                note = f'[{v:#x}] {name_of(v) or ""}'
            elif i.mnemonic in ('addiu', 'daddiu', 'ori') and len(ops) == 3 and ops[1] in regs:
                imm = int(ops[2], 0)
                v = ((regs[ops[1]] | imm) if i.mnemonic == 'ori' else regs[ops[1]] + imm) & 0xFFFFFFFF
                note = f'= {v:#x} {name_of(v) or ""}'
                regs[ops[0]] = v
        out.append(txt + (f'    ; {note}' if note else ''))
    return out


def callers(target):
    hits = []
    for off in range(0, len(CODE) - 3, 4):
        w = struct.unpack_from('<I', CODE, off)[0]
        if (w >> 26) == 3 and ((w & 0x3FFFFFF) << 2) | ((BASE + off) & 0xF0000000) == target:
            hits.append(BASE + off)
    return hits


def demangle_hint(n):
    return n


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'sym':
        r = re.compile(sys.argv[2])
        for a, sz, n, t in SYMS:
            if r.search(n):
                print(f'{a:08x} {sz:6x} {t[4:8]} {n}')
    elif cmd == 'dis':
        a, sz = resolve(sys.argv[2])
        if len(sys.argv) > 3:
            sz = int(sys.argv[3]) * 4
        print(f'; {name_of(a)} size {sz:#x}')
        print('\n'.join(disasm(a, sz)))
    elif cmd == 'xref':
        a, _ = resolve(sys.argv[2])
        for c in callers(a):
            f = func_at(c)
            print(f'{c:08x} in {f[2] if f else "?"}')
