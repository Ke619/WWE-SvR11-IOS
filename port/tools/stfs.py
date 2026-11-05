"""Minimal reader for Xbox 360 STFS packages (CON / LIVE / PIRS): title
updates and DLC. Lists and extracts files.

    python stfs.py <package>                 list
    python stfs.py <package> <out dir>       extract everything
Library: Stfs(path).files -> [(path, size, entry)], .read(entry) -> bytes,
.display_name, .title_id, .content_type.
"""
import mmap
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (0xAA, 0x70E4, 0x4AF768)  # data blocks covered by each hash level


def i24le(b, o):
    return b[o] | b[o + 1] << 8 | b[o + 2] << 16


class Stfs:
    def __init__(self, path):
        with open(path, "rb") as f:  # (a DLC package can be 1 GB: mapped, read as asked)
            self.data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.data
        if d[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError(f"{path}: not an STFS package")
        self.header_size = struct.unpack_from(">I", d, 0x340)[0]
        self.content_type = struct.unpack_from(">I", d, 0x344)[0]
        self.title_id = struct.unpack_from(">I", d, 0x360)[0]
        self.display_name = d[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        vd = d[0x379:0x379 + 0x24]
        separation = vd[2]
        table_blocks = struct.unpack_from("<H", vd, 3)[0]
        table_start = i24le(vd, 5)
        self.base = (self.header_size + 0xFFF) & ~0xFFF
        # Read-only packages (LIVE/PIRS, and CON headers of 0xB000) keep one
        # hash block per table; otherwise two (active/backup).
        if self.base == 0xB000 or separation & 1:
            self.shift = 0
        else:
            self.shift = 1
        self.files = []
        names = {}
        entries = []
        for t in range(table_blocks):
            off = self.offset(table_start + t)
            for e in range(BLOCK // 0x40):
                raw = d[off + e * 0x40: off + e * 0x40 + 0x40]
                flags = raw[0x28]
                n = flags & 0x3F
                if n == 0:
                    continue
                name = raw[:n].decode("latin-1")
                entry = {
                    "index": len(entries),
                    "name": name,
                    "dir": bool(flags & 0x80),
                    "contiguous": bool(flags & 0x40),
                    "blocks": i24le(raw, 0x29),
                    "start": i24le(raw, 0x2F),
                    "parent": struct.unpack_from(">h", raw, 0x32)[0],
                    "size": struct.unpack_from(">I", raw, 0x34)[0],
                }
                entries.append(entry)
        for e in entries:
            parts, p = [e["name"]], e["parent"]
            while p >= 0 and p < len(entries):
                parts.append(entries[p]["name"])
                p = entries[p]["parent"]
            e["path"] = "/".join(reversed(parts))
            if not e["dir"]:
                self.files.append((e["path"], e["size"], e))

    def offset(self, block):
        """Data block number -> file offset (skipping the hash tables)."""
        b = block
        for level_base in PER_LEVEL:
            b += ((block + level_base) // level_base) << self.shift
            if block < level_base:
                break
        return self.base + b * BLOCK

    def hash_entry(self, block):
        """The level-0 hash entry of a data block (for its next-block link)."""
        table = (block // PER_LEVEL[0]) * PER_LEVEL[0]
        # Offset of the hash table covering `block` = the block right before
        # its first data block, minus the higher-level tables in between.
        first = self.offset(table)
        hash_off = first - (BLOCK << self.shift)
        # Higher levels sit before level 0 when the table starts a level-1 run.
        if table % PER_LEVEL[1] == 0 and table:
            hash_off -= BLOCK << self.shift
        return hash_off + (block % PER_LEVEL[0]) * 0x18

    def read(self, entry):
        out = bytearray()
        remaining = entry["size"]
        block = entry["start"]
        for i in range(entry["blocks"]):
            off = self.offset(block)
            n = min(BLOCK, remaining)
            out += self.data[off:off + n]
            remaining -= n
            if remaining <= 0:
                break
            if entry["contiguous"]:
                block += 1
            else:
                h = self.hash_entry(block)
                block = i24le(bytes(reversed(self.data[h + 0x15:h + 0x18])), 0)
        return bytes(out)


def main():
    pkg = Stfs(sys.argv[1])
    print(f"{pkg.display_name}  title {pkg.title_id:08X}  type {pkg.content_type:08X}")
    for path, size, _ in pkg.files:
        print(f"  {size:>12}  {path}")
    if len(sys.argv) > 2:
        for path, size, entry in pkg.files:
            dst = os.path.join(sys.argv[2], *path.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            data = pkg.read(entry)
            if len(data) != size:
                raise SystemExit(f"{path}: read {len(data)} of {size} bytes")
            open(dst, "wb").write(data)
        print(f"extracted {len(pkg.files)} files to {sys.argv[2]}")


if __name__ == "__main__":
    main()
