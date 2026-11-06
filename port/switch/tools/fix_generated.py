#!/usr/bin/env python3
"""Switch build: makes the recompiled code faster and correct on a weakly ordered CPU.

fix_generated.py <generated/default folder>   (safe to run twice)

 1. lwsync / sync: the recompiler emits nothing for them, which x86 hides. On ARM they
    become a real barrier.
 2. Guest memory reads and writes stop being volatile, so the compiler may keep values
    in registers (volatile also made it reload the context registers around each one).
    A compiler barrier on each backward jump keeps spin loops re-reading memory, and
    the sync barriers hold ordering. MMIO accesses stay volatile.
 3. The GCC form of DEFINE_REX_FUNC, which the header doesn't have.
"""
import glob
import os
import re
import sys

folder = sys.argv[1]
DMB = '__asm__ volatile("dmb ish" ::: "memory");'
FENCE = '__asm__ volatile("" ::: "memory");'
MARK = '// [switch] fixed'
label_re = re.compile(r'^(loc_[0-9A-Fa-f]+):')
goto_re = re.compile(r'^(\s*)(if \(.*\) )?goto (loc_[0-9A-Fa-f]+);\s*$')
sync_re = re.compile(r'^\s*// (lwsync|sync|hwsync)\b')
func_re = re.compile(r'^DEFINE_REX_FUNC\(')

files = sync = loops = 0
for path in sorted(glob.glob(os.path.join(folder, 'svr2011_recomp.*.cpp'))):
    with open(path, encoding='utf-8', newline='') as f:
        text = f.read()
    if text.startswith(MARK):
        continue
    nl = '\r\n' if '\r\n' in text[:2000] else '\n'
    out = [MARK]
    seen = set()
    for line in text.split(nl):
        if func_re.match(line):
            seen = set()
        m = label_re.match(line)
        if m:
            seen.add(m.group(1))
        m = goto_re.match(line)
        if m and m.group(3) in seen:
            indent, cond, label = m.group(1), m.group(2) or '', m.group(3)
            if cond:
                out.append('%s%s{ %s goto %s; }' % (indent, cond, FENCE, label))
            else:
                out.append('%s%s goto %s;' % (indent, FENCE, label))
            loops += 1
            continue
        out.append(line)
        if sync_re.match(line):
            out.append('\t' + DMB)
            sync += 1
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(nl.join(out))
    files += 1

# The header
p = os.path.join(folder, 'svr2011_pch.h')
with open(p, encoding='utf-8', newline='') as f:
    s = f.read()
nl = '\r\n' if '\r\n' in s else '\n'
if '[switch] plain memory' not in s:
    n = 0
    def plain(m):
        global n
        n += 1
        return m.group(1) + m.group(2)
    # REX_LOAD_* / REX_STORE_* only: the REX_MM_* macros keep volatile.
    lines = s.split(nl)
    for i, line in enumerate(lines):
        if re.match(r'#define REX_(LOAD|STORE)_U(8|16|32|64)\(', line):
            lines[i] = line.replace('*(volatile u', '*(u')
            n += 1
    s = ('// [switch] plain memory: see port/switch/tools/fix_generated.py' + nl) + nl.join(lines)
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(s)
    print('header macros changed:', n)
print('files', files, 'sync barriers', sync, 'loop barriers', loops)
