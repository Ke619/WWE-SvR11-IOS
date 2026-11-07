"""HCTP (SLES_520.36) backstage gate tables, read from the ELF (CGate4 / CMoveStage, docs/HCTP_BACKSTAGE.md):
per move marker id (the stage 50000 'm' points) its destination stage and entry marker (g_stg_audi_list, 2 bytes
per id), its exit action (bord_group_list+0x10, s16 per id), and the entry placement offsets (offx / offz, 8 floats,
one per wrestler; CMoveStage's Rear puts wrestler i at marker + rot(offx[i], 0, offz[i] - 33)).
    py gate_tables.py [elf]   -> JSON"""
import json
import struct
import sys

ELF = sys.argv[1] if len(sys.argv) > 1 else r"D:\Xbox Games Ports\HCTP Extract\SLES_520.36"
d = open(ELF, 'rb').read()
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]


def at(addr, n):
    for s in secs:
        if s[3] <= addr < s[3] + s[5] and s[1] != 8:  # (not NOBITS)
            o = s[4] + addr - s[3]
            return d[o:o + n]
    raise KeyError(hex(addr))


AUDI, BORD, OFFX, OFFZ = 0x419FB0, 0x419F90, 0x41A180, 0x41A1A0
gates = {}
for mid in range(200, 213):
    dest, entry = at(AUDI + mid * 2, 2)
    act, = struct.unpack('<h', at(BORD + mid * 2, 2))
    gates[mid] = {'dest_stage': dest, 'entry_marker': entry, 'exit_act': act}
out = {'gates': gates, 'offx': list(struct.unpack('<8f', at(OFFX, 32))), 'offz': list(struct.unpack('<8f', at(OFFZ, 32))),
       'entry_back': 33.0, 'trigger_half': 10.0}
print(json.dumps(out, indent=1))
