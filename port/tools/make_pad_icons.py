"""PlayStation button pictures for the PC port (src/pad_icons.cpp).

The game's button icons are glyphs of its fonts: each font has an icon page
(a DXT5 texture, U+E000.. in the strings) - four different ones in all (HD /
SD, 512 x 512 and 256 x 512), copied into every font. The PS3 version has the
same pages with PlayStation art at the same places (some wider: START,
SELECT, L2 / R2, L3 / R3).

For each of the four this writes one taller picture (uncompressed RGBA DDS):
  - section 0: the Xbox 360 page as it is,
  - section 1: the PS3 page (same layout),
  - section 2: "Xbox / PlayStation" pairs, for prompts shared by players
    using different controllers;
and pad_icons.txt, the table src/pad_icons.cpp reads: the renderer shows the
taller picture in place of the page (recognized by its data), the fonts' icon
glyphs are moved to section 0 and get PlayStation and pair versions.

    python make_pad_icons.py --ps3 <PS3 USRDIR> --xbox <extracted 360 data> --out <Game Files>

(<extracted 360 data> has pac/menu/fontHD.pac; the PS3 one is the user's own
copy of the game.)
"""

import argparse
import io
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt  # noqa: E402

FONT_PACS = ['menu/fontHD.pac', 'menu/fontSD.pac']
BUTTONS = list(range(0xE000, 0xE017)) + list(range(0xE01B, 0xE038))  # (not the logos and stars)


def unbpe(b):
    while b[:4] == b'BPE ':
        b = svrfmt.bpe_decode(b)
    return b


def leaves(path):
    """Every leaf of a .pac: (key, data), PACH levels and texture bundles opened."""
    data = open(path, 'rb').read()
    _, groups, _ = svrfmt.epac_read(data)
    out = []

    def walk(blob, key):
        blob = unbpe(blob)
        if blob[:4] == b'PACH':
            try:
                sub = svrfmt.pach_read(blob)
            except Exception:
                out.append((key, blob))
                return
            for i, b in sub:
                walk(b, f'{key}/{i}')
        elif len(blob) >= 48 and blob[32:35].lower() == b'dds':
            n = struct.unpack_from('<I', blob, 0)[0]
            for i in range(n):
                r = 16 + i * 32
                name = blob[r:r + 16].split(b'\0')[0].decode('latin1')
                size, off = struct.unpack_from('<II', blob, r + 20)
                out.append((f'{key}/{name}', blob[off:off + size]))
        else:
            out.append((key, blob))

    for typ, ents in groups:
        for name, blob in ents:
            walk(blob, f'{typ.decode("latin1")}/{name.decode("latin1")}')
    return out


def icon_table(yfb):
    """The icon sub-font of a .yfb: {code: (x, y, w, h)} (pixels), or None."""
    p = 0
    while True:
        p = yfb.find(b'\x64\x00\x00\x00\x64\x00\x00\x00', p)
        if p < 0 or p + 0x80 > len(yfb):
            return None
        cnt = struct.unpack_from('<I', yfb, p + 0x3C)[0]
        lo, hi = struct.unpack_from('<II', yfb, p + 0x70)
        if 0 < cnt < 2000 and 0xE000 <= lo <= hi < 0x10000:
            codes = struct.unpack_from(f'<{cnt}H', yfb, p + 0x80)
            g = p + 0x80 + ((cnt * 2 + 15) & ~15)
            return {c: struct.unpack_from('<4H', yfb, g + 16 * i) for i, c in enumerate(codes)}
        p += 4


def dds_blocks(dds):
    """A DXT5 DDS's base level: (width, height, block data)."""
    h, w = struct.unpack_from('<II', dds, 12)
    size = max(1, w // 4) * max(1, h // 4) * 16
    return w, h, dds[128:128 + size]


def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def slash(h):
    """A "/" in the icons' style (white, dark edge), h pixels high."""
    w = max(6, h * 2 // 5)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    t = max(2, h // 10)
    d.line([(w - t, t), (t, h - t)], fill=(30, 30, 30, 255), width=t + 2)
    d.line([(w - t, t), (t, h - t)], fill=(255, 255, 255, 255), width=t)
    return img


# ---------------------------------------------------------------------------
# Keyboard: a keycap for every key the controls can be bound to (the SDK's
# key names, src/ui/keybinds.cpp), the game picks the bound one.

KEY_NAMES = ([f'F{i}' for i in range(1, 25)] + [chr(c) for c in range(ord('A'), ord('Z') + 1)] +
             [str(i) for i in range(10)] +
             ['Backtick', 'Minus', 'Plus', 'Comma', 'Period', 'Semicolon', 'Slash', 'Backslash', 'LBracket',
              'RBracket', 'Quote', 'Escape', 'Return', 'Space', 'Tab', 'Backspace', 'Delete', 'Insert', 'Home',
              'End', 'PageUp', 'PageDown', 'Left', 'Right', 'Up', 'Down', 'Shift', 'Control', 'Alt'] +
             [f'Numpad{i}' for i in range(10)] +
             ['NumpadEnter', 'NumpadPlus', 'NumpadMinus', 'NumpadStar', 'NumpadSlash', 'PrintScreen', 'Pause',
              'CapsLock', 'NumLock', 'ScrollLock', 'LMB', 'RMB', 'MMB'])
KEY_LABELS = {'Backtick': '`', 'Minus': '-', 'Plus': '=', 'Comma': ',', 'Period': '.', 'Semicolon': ';',
              'Slash': '/', 'Backslash': '\\', 'LBracket': '[', 'RBracket': ']', 'Quote': "'", 'Escape': 'ESC',
              'Return': 'ENTER', 'Space': 'SPACE', 'Tab': 'TAB', 'Backspace': 'BKSP', 'Delete': 'DEL',
              'Insert': 'INS', 'Home': 'HOME', 'End': 'END', 'PageUp': 'PGUP', 'PageDown': 'PGDN',
              'Shift': 'SHIFT', 'Control': 'CTRL', 'Alt': 'ALT', 'NumpadEnter': 'N ENT', 'NumpadPlus': 'N +',
              'NumpadMinus': 'N -', 'NumpadStar': 'N *', 'NumpadSlash': 'N /', 'PrintScreen': 'PRTSC',
              'Pause': 'PAUSE', 'CapsLock': 'CAPS', 'NumLock': 'NUMLK', 'ScrollLock': 'SCRLK',
              **{f'Numpad{i}': f'N {i}' for i in range(10)}}
ARROWS = {'Up': 0, 'Right': 1, 'Down': 2, 'Left': 3}
# Two-key binds as the defaults use them ("Shift+Up"), and two special caps.
COMBOS = [f'{m}+{a}' for m in ('Shift', 'Control', 'Alt') for a in ARROWS]
SPECIAL = ['WASD', 'ARROWS']


def font(size):
    from PIL import ImageFont
    for name in ('arialbd.ttf', 'DejaVuSans-Bold.ttf', 'LiberationSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def arrow(d, cx, cy, r, direction, fill):
    pts = [(0, -1), (1, 0.7), (-1, 0.7)]  # (up)
    rot = {0: lambda x, y: (x, y), 1: lambda x, y: (-y, x), 2: lambda x, y: (-x, -y), 3: lambda x, y: (y, -x)}[direction]
    d.polygon([(cx + rot(x, y)[0] * r, cy + rot(x, y)[1] * r) for x, y in pts], fill=fill)


def keycap(name, h):
    """A key in the game's icon style (dark rounded cap, light rim, white label), h pixels high."""
    if '+' in name:  # (modifier + key: two caps)
        a, b = name.split('+')
        ca, cb = keycap(a, h), keycap(b, h)
        plus = max(8, h // 4)
        img = Image.new('RGBA', (ca.width + plus + cb.width, h), (0, 0, 0, 0))
        img.paste(ca, (0, 0))
        img.paste(cb, (ca.width + plus, 0))
        d = ImageDraw.Draw(img)
        c, t = ca.width + plus // 2, max(2, h // 16)
        d.rectangle([c - plus // 2 + 1, h // 2 - t, c + plus // 2 - 1, h // 2 + t], fill=(255, 255, 255, 255))
        d.rectangle([c - t, h // 2 - plus // 2 + 1, c + t, h // 2 + plus // 2 - 1], fill=(255, 255, 255, 255))
        return img
    label = KEY_LABELS.get(name, name)
    size = int(h * (0.52 if len(label) <= 2 else 0.38))
    f = font(size)
    tw = 0 if name in ARROWS or name == 'ARROWS' else int(f.getlength(label))
    w = max(h, tw + h // 2)
    if name in ('LMB', 'RMB', 'MMB'):
        w = h
    pad = max(2, h // 14)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rad = h // 5
    d.rounded_rectangle([0, 0, w - 1, h - 1], rad, fill=(20, 20, 20, 255))  # (rim)
    d.rounded_rectangle([pad, pad, w - 1 - pad, h - 1 - pad], rad - pad, fill=(200, 200, 200, 255))
    d.rounded_rectangle([pad * 2, pad * 2, w - 1 - pad * 2, h - 1 - pad * 3], rad - pad * 2,
                        fill=(58, 58, 62, 255))  # (the cap's face, a light bottom edge below)
    white = (255, 255, 255, 255)
    face_cy = (pad * 2 + h - 1 - pad * 3) / 2
    if name in ARROWS:
        arrow(d, w / 2, face_cy, h * 0.24, ARROWS[name], white)
    elif name == 'ARROWS':
        for k, (dx, dy) in enumerate([(0, -1), (1, 0), (0, 1), (-1, 0)]):
            arrow(d, w / 2 + dx * h * 0.2, face_cy + dy * h * 0.2, h * 0.11, k, white)
    elif name in ('LMB', 'RMB', 'MMB'):  # (a mouse, the button lit)
        mx0, my0, mx1, my1 = w * 0.3, h * 0.2, w * 0.7, h * 0.78
        d.rounded_rectangle([mx0, my0, mx1, my1], int(w * 0.18), outline=white, width=max(2, h // 20))
        mid = (mx0 + mx1) / 2
        d.line([(mid, my0), (mid, my0 + (my1 - my0) * 0.4)], fill=white, width=max(1, h // 24))
        lit = {'LMB': (mx0 + 3, my0 + 3, mid - 2, my0 + (my1 - my0) * 0.4 - 1),
               'RMB': (mid + 2, my0 + 3, mx1 - 3, my0 + (my1 - my0) * 0.4 - 1),
               'MMB': (mid - 2, my0 + 4, mid + 2, my0 + (my1 - my0) * 0.35)}[name]
        d.rectangle(lit, fill=(255, 200, 40, 255))
    else:
        d.text((w / 2, face_cy), label, font=f, fill=white, anchor='mm', stroke_width=1, stroke_fill=(0, 0, 0, 255))
    return img


# ---------------------------------------------------------------------------
# Keycaps from WWE 2K15 PC (--k15 <game folder>): its menu font
# pac/menu/Assets/font/LTAG_font.pac holds an icon page (4096 x 2048) with a
# keycap for every key, glyph U+E200 + the DirectInput scan code. Mouse
# buttons 2K15 doesn't have: drawn in its style.

DIK = {'Escape': 0x01, **{str(i): 0x01 + i for i in range(1, 10)}, '0': 0x0B, 'Minus': 0x0C, 'Plus': 0x0D,
       'Backspace': 0x0E, 'Tab': 0x0F, 'Q': 0x10, 'W': 0x11, 'E': 0x12, 'R': 0x13, 'T': 0x14, 'Y': 0x15,
       'U': 0x16, 'I': 0x17, 'O': 0x18, 'P': 0x19, 'LBracket': 0x1A, 'RBracket': 0x1B, 'Return': 0x1C,
       'Control': 0x1D, 'A': 0x1E, 'S': 0x1F, 'D': 0x20, 'F': 0x21, 'G': 0x22, 'H': 0x23, 'J': 0x24,
       'K': 0x25, 'L': 0x26, 'Semicolon': 0x27, 'Quote': 0x28, 'Backtick': 0x29, 'Shift': 0x2A,
       'Backslash': 0x2B, 'Z': 0x2C, 'X': 0x2D, 'C': 0x2E, 'V': 0x2F, 'B': 0x30, 'N': 0x31, 'M': 0x32,
       'Comma': 0x33, 'Period': 0x34, 'Slash': 0x35, 'NumpadStar': 0x37, 'Alt': 0x38, 'Space': 0x39,
       'CapsLock': 0x3A, **{f'F{i}': 0x3A + i for i in range(1, 11)}, 'NumLock': 0x45, 'ScrollLock': 0x46,
       'Numpad7': 0x47, 'Numpad8': 0x48, 'Numpad9': 0x49, 'NumpadMinus': 0x4A, 'Numpad4': 0x4B,
       'Numpad5': 0x4C, 'Numpad6': 0x4D, 'NumpadPlus': 0x4E, 'Numpad1': 0x4F, 'Numpad2': 0x50,
       'Numpad3': 0x51, 'Numpad0': 0x52, 'F11': 0x57, 'F12': 0x58, 'NumpadEnter': 0x9C, 'NumpadSlash': 0xB5,
       'PrintScreen': 0xB7, 'Pause': 0xC5, 'Home': 0xC7, 'Up': 0xC8, 'PageUp': 0xC9, 'Left': 0xCB,
       'Right': 0xCD, 'End': 0xCF, 'Down': 0xD0, 'PageDown': 0xD1, 'Insert': 0xD2, 'Delete': 0xD3}

K15 = {}  # code -> RGBA glyph (full size)


def load_k15(game):
    """WWE 2K15's keycap glyphs into K15 (code -> picture)."""
    import ch_tool
    path = os.path.join(game, 'pac', 'menu', 'Assets', 'font', 'LTAG_font.pac')
    _, groups = ch_tool.epk8_read(open(path, 'rb').read())
    ents = [(i, unbpe(e)) for i, e in svrfmt.pach_read(groups[0][1][0][1])]
    # (each font: descriptor 'd\0\0\0', its pages; the keycap font's descriptor
    # lists the U+E2xx codes, its page is the 4096 x 2048 DDS after it)
    for k, (i, d) in enumerate(ents):
        if d[:4] != b'd\0\0\0' or len(d) < 0x70:
            continue
        n = struct.unpack_from('<I', d, 0x3C)[0]
        if n <= 0 or 0x70 + 2 * n > len(d):
            continue
        codes = struct.unpack_from(f'<{n}H', d, 0x70)
        if 0xE21E not in codes:
            continue
        page = next((p for _, p in ents[k + 1:] if p[:4] == b'DDS ' and struct.unpack_from('<I', p, 16)[0] >= 2048),
                    None)
        if page is None:
            continue
        img = Image.open(io.BytesIO(page)).convert('RGBA')
        off = struct.unpack_from('<I', d, 0x60)[0]
        for j, c in enumerate(codes):
            x, y, w, h = struct.unpack_from('<4h', d, off + 16 * j)
            if w > 0 and h > 0:
                K15[c] = img.crop((x, y, x + w, y + h))
        return True
    return False


def k15_cap(name, h):
    """2K15's keycap for a key, h pixels high (its own width), or None."""
    code = DIK.get(name)
    g = K15.get(0xE200 + code) if code is not None else None
    if g is None:
        return None
    g = g.crop(g.getbbox() or (0, 0, g.width, g.height))
    return g.resize((max(1, round(g.width * h / g.height)), h), Image.LANCZOS)


def k15_cluster(keys, h):
    """Four 2K15 keycaps as a cluster (up key on top, the other three below), h high."""
    s = h // 2
    caps = [k15_cap(k, s) for k in keys]
    if any(c is None for c in caps):
        return None
    cw = max(c.width for c in caps)
    img = Image.new('RGBA', (cw * 3, s * 2), (0, 0, 0, 0))
    img.alpha_composite(caps[0], (cw + (cw - caps[0].width) // 2, 0))
    for k, c in enumerate(caps[1:]):
        img.alpha_composite(c, (k * cw + (cw - c.width) // 2, s))
    return img


def k15_mouse(name, h):
    """A mouse in 2K15's keycap style (dark gloss, red rim), the button lit."""
    s = 4  # (drawn large, scaled down)
    W, H = int(h * 0.72) * s, h * s
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rim, body = (150, 20, 20, 255), (34, 34, 38, 255)
    d.rounded_rectangle([s, s, W - s, H - s], int(W * 0.48), fill=(10, 10, 10, 255))
    d.rounded_rectangle([3 * s, 3 * s, W - 3 * s, H - 3 * s], int(W * 0.45), fill=rim)
    d.rounded_rectangle([6 * s, 6 * s, W - 6 * s, H - 6 * s], int(W * 0.42), fill=body)
    split = int(H * 0.42)
    mid = W // 2
    d.line([(6 * s, split), (W - 6 * s, split)], fill=(10, 10, 10, 255), width=2 * s)
    d.line([(mid, 6 * s), (mid, split)], fill=(10, 10, 10, 255), width=2 * s)
    lit = (230, 30, 30, 255)
    if name == 'LMB':
        d.pieslice([6 * s, 6 * s, W - 6 * s, int(H * 0.8)], 180, 270, fill=lit)
        d.rectangle([6 * s + W // 4, int(H * 0.2), mid - s, split - s], fill=lit)
    elif name == 'RMB':
        d.pieslice([6 * s, 6 * s, W - 6 * s, int(H * 0.8)], 270, 360, fill=lit)
        d.rectangle([mid + s, int(H * 0.2), W - 6 * s - W // 4, split - s], fill=lit)
    else:  # (MMB: the wheel)
        d.rounded_rectangle([mid - 3 * s, int(H * 0.12), mid + 3 * s, int(H * 0.34)], 3 * s, fill=lit)
    # (gloss)
    d.ellipse([W * 0.25, H * 0.5, W * 0.55, H * 0.85], fill=(255, 255, 255, 28))
    return img.resize((W // s, H // s), Image.LANCZOS)


def keycap_any(name, h):
    """2K15's keycap when --k15 gave its pictures, the drawn one otherwise."""
    if K15:
        if name in ('LMB', 'RMB', 'MMB'):
            return k15_mouse(name, h)
        if name == 'WASD':
            c = k15_cluster(['W', 'A', 'S', 'D'], h)
            if c:
                return c
        if name == 'ARROWS':
            c = k15_cluster(['Up', 'Left', 'Down', 'Right'], h)
            if c:
                return c
        if '+' in name:
            a, b = name.split('+')
            ca, cb = k15_cap(a, h), k15_cap(b, h)
            if ca and cb:
                plus = max(8, h // 4)
                img = Image.new('RGBA', (ca.width + plus + cb.width, h), (0, 0, 0, 0))
                img.paste(ca, (0, 0))
                img.paste(cb, (ca.width + plus, 0))
                d = ImageDraw.Draw(img)
                c, t = ca.width + plus // 2, max(2, h // 16)
                d.rectangle([c - plus // 2 + 1, h // 2 - t, c + plus // 2 - 1, h // 2 + t], fill=(255, 255, 255, 255))
                d.rectangle([c - t, h // 2 - plus // 2 + 1, c + t, h // 2 + plus // 2 - 1], fill=(255, 255, 255, 255))
                return img
        c = k15_cap(name, h)
        if c:
            return c
    return keycap(name, h)


def pack(items, width, y0=0):
    """Rows of images: [(key, x, y, img)] and the height used."""
    placed, x, y, row_h = [], 0, y0, 0
    for k, img in items:
        if x + img.width > width:
            x, y, row_h = 0, y + row_h + 4, 0
        placed.append((k, x, y, img))
        x += img.width + 4
        row_h = max(row_h, img.height)
    return placed, (y + row_h + 3) & ~3


# ---------------------------------------------------------------------------
# Character select's controller pictures for a keyboard player: the menus'
# contro02 (silhouette: COM, the bottom bar) and contro03 (the player's pad),
# 128 x 64 DXT5 in menuHD / menuSD.

def keyboard_picture(w, h, filled):
    """A keyboard in the style of the 360 controller pictures."""
    s = 4
    img = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = w * s * 0.06, h * s * 0.22, w * s * 0.94, h * s * 0.86
    rad = int(h * s * 0.12)
    rim = int(s * 2.5)
    if not filled:  # (black with a white rim, as contro02; a cable on top)
        cx = (x0 + x1) / 2
        d.line([(cx, y0), (cx, y0 - h * s * 0.08), (cx + w * s * 0.08, y0 - h * s * 0.16)],
               fill=(255, 255, 255, 255), width=int(s * 4.5), joint='curve')
        d.line([(cx, y0), (cx, y0 - h * s * 0.08), (cx + w * s * 0.08, y0 - h * s * 0.16)],
               fill=(0, 0, 0, 255), width=int(s * 2), joint='curve')
        d.rounded_rectangle([x0, y0, x1, y1], rad, fill=(255, 255, 255, 255))
        d.rounded_rectangle([x0 + rim, y0 + rim, x1 - rim, y1 - rim], rad - rim, fill=(0, 0, 0, 255))
    else:  # (white body, dark keys, as the white 360 pad in contro03)
        d.rounded_rectangle([x0, y0 + s * 2, x1, y1 + s * 2], rad, fill=(0, 0, 0, 120))  # (shadow)
        d.rounded_rectangle([x0, y0, x1, y1], rad, fill=(70, 70, 74, 255))
        d.rounded_rectangle([x0 + rim, y0 + rim, x1 - rim, y1 - rim], rad - rim, fill=(238, 238, 240, 255))
        kx0, ky0, kx1, ky1 = x0 + rim * 3, y0 + rim * 3, x1 - rim * 3, y1 - rim * 3
        rows, cols = 4, 12
        kw, kh = (kx1 - kx0) / cols, (ky1 - ky0) / rows
        g = s * 1.2
        for r in range(rows):
            if r == rows - 1:  # (bottom row: space bar between small keys)
                keys = [(0, 2), (2, 9), (9, 12)]
            else:
                off = [0, 0.4, 0.7][r]
                keys = [(c + off, c + off + 1) for c in range(cols - (1 if r else 0))]
            for a, b in keys:
                col = (60, 60, 64, 255)
                d.rounded_rectangle([kx0 + a * kw + g, ky0 + r * kh + g, kx0 + min(b, cols) * kw - g,
                                     ky0 + (r + 1) * kh - g], int(s * 1.5), fill=col)
        # (plain keys: coloured movement keys read as glitches at the picture's size - 2.1.8 report 7edf25)
    return img.resize((w, h), Image.LANCZOS)


SPRITE_PACS = ['game2dHD.pac', 'game2dSD.pac', 'menu/menuHD.pac', 'menu/menuSD.pac', 'menu/ScrLayHD.pac',
               'menu/ScrLaySD.pac']


def dds_info(dds):
    h, w = struct.unpack_from('<II', dds, 12)
    return w, h, dds[84:88]


def block_bytes(w, h, fourcc):
    bpb = 8 if fourcc == b'DXT1' else 16
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb


def write_dds_rgba(path, img):
    w, h = img.size
    hdr = bytearray(128)
    struct.pack_into('<4sIIIIIII', hdr, 0, b'DDS ', 124, 0x100F, h, w, w * 4, 0, 1)
    struct.pack_into('<IIIIIIII', hdr, 76, 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into('<I', hdr, 108, 0x1000)
    with open(path, 'wb') as f:
        f.write(hdr)
        # (B8G8R8A8 in memory, as the masks say)
        r, g, b, a = img.split()
        f.write(Image.merge('RGBA', (b, g, r, a)).tobytes())


# Keyboard prompts: the match HUD's button pictures (game2d pacs: GA2D English, GI2D / GG2D / GS2D / GF2D the
# Italian / German / Spanish / French copies - their BASE/<...> entries hold the same buttons) and the buttons in
# each, left to right (checked by eye: E007 Y, E008 B, E009 A, E00A X, E00B LB, E00E RB, E00F RT, E010 R3,
# E011 the right stick, E012 the left stick). The sheet BASE/1/0/1 and the stick pictures are left as they are.
PROMPT_BUTTONS = {
    'BASE/0/0/0': [0xE00E], 'BASE/0/4/0': [0xE007], 'BASE/0/6/0': [0xE007], 'BASE/0/7/1': [0xE008],
    'BASE/0/9/0': [0xE011], 'BASE/0/10/1': [0xE00B], 'BASE/0/12/1': [0xE007], 'BASE/0/18/1': [0xE008],
    'BASE/0/21/0': [0xE010], 'BASE/0/27/1': [0xE008], 'BASE/0/27/2': [0xE009], 'BASE/0/29/1': [0xE008],
    'BASE/0/29/2': [0xE00A], 'BASE/0/33/2': [0xE007], 'BASE/0/33/3': [0xE009], 'BASE/0/90/60': [0xE00F],
    'BASE/6/0/1': [0xE009], 'BASE/6/1/2': [0xE010], 'BASE/7/0/0': [0xE011], 'BASE/8/0/1': [0xE00E, 0xE007],
    'BASE/8/6/1': [0xE012, 0xE009], 'BASE/12/90/13': [0xE012], 'BASE/13/11/0': [0xE012],
}


def button_boxes(xi, pi, codes, glyphs):
    """Where the buttons `codes` (left to right) are in a 360 picture: the parts its PS3 version changes - for
    one button the part most like its Xbox glyph -> [(code, x, y, w, h)]. (PIL only.)"""
    from PIL import ImageChops, ImageFilter, ImageStat

    def flat(im):  # (colour times alpha, and the alpha: what shows)
        bg = Image.new('RGBA', im.size, (0, 0, 0, 0))
        rgb = Image.alpha_composite(bg, im).convert('RGB')
        return Image.merge('RGBA', (*rgb.split(), im.split()[3]))
    xf, pf = flat(xi), flat(pi)
    d = ImageChops.difference(xf, pf)
    m = d.split()[0]
    for band in d.split()[1:]:
        m = ImageChops.lighter(m, band)
    mask = m.point(lambda v: 255 if v > 40 else 0).filter(ImageFilter.MaxFilter(5))
    w, h = mask.size
    px = mask.load()
    seen = set()
    parts = []
    for y0 in range(h):
        for x0 in range(w):
            if not px[x0, y0] or (x0, y0) in seen:
                continue
            stack, xs, ys = [(x0, y0)], [], []
            seen.add((x0, y0))
            while stack:
                x, y = stack.pop()
                xs.append(x), ys.append(y)
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and px[nx, ny] and (nx, ny) not in seen:
                        seen.add((nx, ny))
                        stack.append((nx, ny))
            bx0, by0, bx1, by1 = min(xs), min(ys), max(xs) + 1, max(ys) + 1
            if bx1 - bx0 >= 8 and by1 - by0 >= 8:
                parts.append((bx0, by0, bx1 - bx0, by1 - by0))
    if not parts:
        return []
    if len(codes) == 1:  # (the part most like the button's glyph)
        g = dict(glyphs).get({0xE011: 0xE010, 0xE012: 0xE00D}.get(codes[0], codes[0]))  # (a stick: its click's cap)
        def score(r):
            if g is None:
                return -r[2] * r[3]
            crop = xf.crop((r[0], r[1], r[0] + r[2], r[1] + r[3]))
            return sum(ImageStat.Stat(ImageChops.difference(crop, flat(g.resize((r[2], r[3]), Image.BILINEAR)))).mean)
        # (a part wider than the button - its text changed too: the button-shaped window in it most like it)
        windows = []
        for r in parts:
            gw = max(8, round(r[3] * (g.width / g.height))) if g is not None else r[2]
            if r[2] <= gw * 1.3:
                windows.append(r)
            else:
                windows += [(x, r[1], gw, r[3]) for x in range(r[0], r[0] + r[2] - gw + 1)]
        return [(codes[0],) + min(windows, key=score)]
    parts.sort()
    if len(parts) != len(codes):
        return []
    return [(c,) + r for c, r in zip(codes, parts)]


def light_caps(x_dds, p_dds, fcc, n):
    """A match prompt's PS3 picture with light button caps, as the 360's (the PS3's dark caps with coloured symbols
    showed as dim, washed-out buttons over the ring's mat - 2.1.8 report 7edf25): in each button - a dark opaque
    piece up to 96 px across that differs from the 360 picture (text, logos and lines stay as they are) - grey
    turns over (the cap light, white letters dark), the edge gets a dark outline, coloured symbols get stronger.
    -> the DXT blocks (n bytes); the PS3's own if anything fails. (PIL only.)"""
    import colorsys
    from PIL import ImageChops, ImageFilter
    try:
        xi = Image.open(io.BytesIO(x_dds)).convert('RGBA')
        pi = Image.open(io.BytesIO(p_dds)).convert('RGBA')

        def flat(im):
            bg = Image.new('RGBA', im.size, (0, 0, 0, 0))
            rgb = Image.alpha_composite(bg, im).convert('RGB')
            return Image.merge('RGBA', (*rgb.split(), im.split()[3]))
        d = ImageChops.difference(flat(xi), flat(pi))
        m = d.split()[0]
        for band in d.split()[1:]:
            m = ImageChops.lighter(m, band)
        changed = m.point(lambda v: 255 if v > 40 else 0).filter(ImageFilter.MaxFilter(5)).load()
        alpha = pi.split()[3]
        solid = alpha.point(lambda v: 255 if v >= 128 else 0)
        edge = ImageChops.subtract(solid, solid.filter(ImageFilter.MinFilter(3))).load()
        w, h = pi.size
        ap, pp = alpha.load(), pi.load()
        take = Image.new('L', (w, h), 0)
        tk = take.load()
        seen = bytearray(w * h)
        for y0 in range(h):
            for x0 in range(w):
                if seen[y0 * w + x0] or ap[x0, y0] < 128:
                    continue
                stack, pts, hit = [(x0, y0)], [], False
                seen[y0 * w + x0] = 1
                while stack:
                    x, y = stack.pop()
                    pts.append((x, y))
                    hit = hit or changed[x, y] > 0
                    for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                        if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and ap[nx, ny] >= 128:
                            seen[ny * w + nx] = 1
                            stack.append((nx, ny))
                xs, ys = [q[0] for q in pts], [q[1] for q in pts]
                bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
                # (a button: a dark cap - white text that only moved on the PS3 is light on the whole)
                dark = sum(sum(pp[q][:3]) for q in pts) < 120 * 3 * len(pts)
                if hit and bw <= 96 and bh <= 96 and dark:
                    for q in pts:
                        tk[q] = 255
        mask = take.filter(ImageFilter.MaxFilter(3)).load()  # (and the soft edge around each)
        out = pi.copy()
        px = out.load()
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if not a or not mask[x, y]:
                    continue
                if max(r, g, b) - min(r, g, b) > 30:  # (a coloured symbol / arrow)
                    hh, ss, vv = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
                    r2, g2, b2 = colorsys.hsv_to_rgb(hh, min(1.0, ss * 1.7), vv * 0.9)
                    px[x, y] = (int(r2 * 255), int(g2 * 255), int(b2 * 255), a)
                    continue
                v = 70 if edge[x, y] else int(232 - (r + g + b) // 3 * 0.75)
                px[x, y] = (v, v, min(255, v + 4), a)
        dds = io.BytesIO()
        out.save(dds, 'DDS', pixel_format=fcc.decode())
        data = dds.getvalue()[128:128 + n]
        return data if len(data) == n else p_dds[128:128 + n]
    except Exception as e:  # (never a broken picture: the PS3's own)
        print(f'light caps: kept the PS3 picture ({e})')
        return p_dds[128:128 + n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps3', required=True, help='the PS3 game\'s USRDIR')
    ap.add_argument('--xbox', required=True, help='the extracted 360 data (with pac/)')
    ap.add_argument('--out', required=True, help='the game folder (pad_icons/ is written there)')
    ap.add_argument('--previews', action='store_true', help='also PNG previews of the pictures')
    ap.add_argument('--k15', help='WWE 2K15 PC game folder: its keycap pictures for the keyboard')
    args = ap.parse_args()
    if args.k15:
        print('2K15 keycaps:', len(K15) if load_k15(args.k15) else 'not found')

    atlases = {}  # 360 fnv -> dict
    for rel in FONT_PACS:
        x = dict(leaves(os.path.join(args.xbox, 'pac', rel)))
        p = dict(leaves(os.path.join(args.ps3, 'pac', rel)))
        for key, yfb in x.items():
            if not yfb.startswith(b'd\0\0\0d\0\0\0'):
                continue
            tx = icon_table(yfb)
            ty = icon_table(p.get(key, b''))
            if not tx or not ty:
                continue
            # (the font's pages: <entry>/<n-1>/<page>; the icon page is the last DDS)
            parts = key.split('/')
            tex_key = '/'.join(parts[:-1] + [str(int(parts[-1]) - 1)])
            pages = sorted(k for k in x if k.startswith(tex_key + '/') and x[k][:4] == b'DDS ')
            if not pages:
                continue
            page = pages[-1]
            if page not in p or p[page][:4] != b'DDS ':
                continue
            w, h, blocks = dds_blocks(x[page])
            pw, ph, _ = dds_blocks(p[page])
            if (pw, ph) != (w, h):
                continue  # (fontSD F001: a different page on the PS3)
            key_hash = fnv64(blocks)
            if key_hash in atlases:
                continue
            atlases[key_hash] = dict(source=f'{rel}:{page}', w=w, h=h, x360=x[page], ps3=p[page], tx=tx, ty=ty)

    os.makedirs(os.path.join(args.out, 'pad_icons'), exist_ok=True)
    lines = ['# pad_icons.txt - written by tools/make_pad_icons.py (src/pad_icons.cpp reads it)',
             '# atlas <fnv64 of the 360 page> <page w> <page h> <picture h> <file>',
             '# xbox <code>:<x>,<y>,<w>,<h> ...  (Xbox glyphs that identify the page)',
             '# ps <code> <x> <y> <w> <h>      (section 1)',
             '# pair <code> <x> <y> <w> <h>    (section 2: Xbox / PlayStation)']
    ref_glyphs = []  # (code, its Xbox glyph picture): the largest font's - what prompt buttons are matched to
    for n, (hsh, a) in enumerate(sorted(atlases.items(), key=lambda kv: (kv[1]['w'], kv[1]['source']))):
        w, h = a['w'], a['h']
        x360 = Image.open(io.BytesIO(a['x360'])).convert('RGBA')
        ps3 = Image.open(io.BytesIO(a['ps3'])).convert('RGBA')
        tx, ty = a['tx'], a['ty']
        # Section 2: the pairs, packed in rows.
        pairs = []
        for c in BUTTONS:
            if c not in tx or c not in ty:
                continue
            xr, pr = tx[c], ty[c]
            gx = x360.crop((xr[0], xr[1], xr[0] + xr[2], xr[1] + xr[3]))
            gp = ps3.crop((pr[0], pr[1], pr[0] + pr[2], pr[1] + pr[3]))
            gh = max(xr[3], pr[3])
            s = slash(gh)
            img = Image.new('RGBA', (xr[2] + s.width + pr[2], gh), (0, 0, 0, 0))
            img.paste(gx, (0, (gh - xr[3]) // 2))
            img.paste(s, (xr[2], 0), s)
            img.paste(gp, (xr[2] + s.width, (gh - pr[3]) // 2))
            pairs.append((c, img))
        rows, x0, y0, row_h = [], 0, 0, 0
        placed = []
        for c, img in pairs:
            if x0 + img.width > w:
                x0, y0, row_h = 0, y0 + row_h + 4, 0
            placed.append((c, x0, y0, img))
            x0 += img.width + 4
            row_h = max(row_h, img.height)
        if not placed:
            continue  # (the ticker font's PPV logos: no buttons)
        pair_h = (y0 + row_h + 3) & ~3
        # Section 3: the keycaps, as high as the face buttons.
        cap_h = tx[0xE009][3]
        caps, keys_h = pack([(k, keycap_any(k, cap_h)) for k in KEY_NAMES + COMBOS + SPECIAL], w)
        total = h * 2 + pair_h + keys_h
        tall = Image.new('RGBA', (w, total), (0, 0, 0, 0))
        tall.paste(x360, (0, 0))
        tall.paste(ps3, (0, h))
        for c, px, py, img in placed:
            tall.paste(img, (px, h * 2 + py))
        for k, px, py, img in caps:
            tall.paste(img, (px, h * 2 + pair_h + py))
        name = f'icons{n}_{w}x{h}.dds'
        write_dds_rgba(os.path.join(args.out, 'pad_icons', name), tall)
        if args.previews:
            tall.save(os.path.join(args.out, 'pad_icons', name[:-4] + '.png'))
        lines.append(f'atlas {hsh:016x} {w} {h} {total} {name}   # {a["source"]}')
        if 0xE009 in tx and (not ref_glyphs or tx[0xE009][3] > ref_glyphs[0][1].height):
            ref_glyphs = [(c, x360.crop((tx[c][0], tx[c][1], tx[c][0] + tx[c][2], tx[c][1] + tx[c][3])))
                          for c in BUTTONS if c in tx and c not in (0xE011, 0xE012)]
        # (how a font's icon page is recognized: its Xbox glyphs' places)
        lines.append('xbox ' + ' '.join(f'{c:04x}:{tx[c][0]},{tx[c][1]},{tx[c][2]},{tx[c][3]}'
                                        for c in (0xE000, 0xE009, 0xE00B) if c in tx))
        for c in BUTTONS:
            if c in ty and c in tx:
                r = ty[c]
                lines.append(f'ps {c:04x} {r[0]} {r[1] + h} {r[2]} {r[3]}')
        for c, px, py, img in placed:
            lines.append(f'pair {c:04x} {px} {py + h * 2} {img.width} {img.height}')
        for k, px, py, img in caps:
            lines.append(f'key {k} {px} {py + h * 2 + pair_h} {img.width} {img.height}')
        print(f'{name}: {a["source"]} ({len(placed)} pairs, {len(caps)} keys, {w}x{total})')

    # Pictures other than the fonts': the PS3 ones where they differ (button
    # prompts), and keyboard controller pictures; sprites.bin holds the data.
    sprites, blob, seen = [], bytearray(), set()
    for rel in SPRITE_PACS:
        xp, pp = os.path.join(args.xbox, 'pac', rel), os.path.join(args.ps3, 'pac', rel)
        if not os.path.exists(xp) or not os.path.exists(pp):
            continue
        x = dict(leaves(xp))
        p = dict(leaves(pp))
        for key, xd in x.items():
            if xd[:4] != b'DDS ' or key not in p or p[key][:4] != b'DDS ' or 'DLC' in key:
                continue
            w, h, fcc = dds_info(xd)
            pw, ph, pfcc = dds_info(p[key])
            if (w, h, fcc) != (pw, ph, pfcc) or fcc not in (b'DXT1', b'DXT5'):
                continue
            n = block_bytes(w, h, fcc)
            xb, pb = xd[128:128 + n], p[key][128:128 + n]
            if xb == pb or len(xb) != n or len(pb) != n:
                continue
            hsh = fnv64(xb)
            name = key.rsplit('/', 1)[-1]
            if (hsh, 'ps') not in seen:
                seen.add((hsh, 'ps'))
                sprites.append(f'sprite ps {hsh:016x} {w} {h} {fcc.decode()} {len(blob)} {n}   # {rel}:{key}')
                # (the match HUD's: light caps, as the 360's buttons - the PS3's dark ones read badly in matches)
                blob += light_caps(xd, p[key], fcc, n) if rel.startswith('game2d') else pb
                # (keyboard prompts: where its buttons are; the 360 picture the game paints keycaps into)
                codes = PROMPT_BUTTONS.get(key[key.find('BASE/'):]) if rel.startswith('game2d') and '2D/BASE/' in key else None
                if fcc == b'DXT5' and codes:
                    boxes = button_boxes(Image.open(io.BytesIO(xd)).convert('RGBA'),
                                         Image.open(io.BytesIO(p[key])).convert('RGBA'), codes, ref_glyphs)
                    if boxes:
                        sprites.append(f'sprite xb {hsh:016x} {w} {h} DXT5 {len(blob)} {n}   # {rel}:{key}')
                        blob += xb
                        for c, bx, by, bw, bh in boxes:
                            sprites.append(f'kbbox {hsh:016x} {c:04x} {bx} {by} {bw} {bh}')
            # (character select: keyboard versions of the controller pictures, RGBA)
            if name in ('contro02', 'contro03') and (hsh, 'kb') not in seen:
                seen.add((hsh, 'kb'))
                pic = keyboard_picture(w, h, name == 'contro03')
                os.makedirs(os.path.join(args.out, 'pad_icons'), exist_ok=True)
                if args.previews:
                    pic.save(os.path.join(args.out, 'pad_icons', f'keyboard_{name}_{w}x{h}.png'))
                dds = io.BytesIO()
                pic.save(dds, 'DDS', pixel_format='DXT5')
                data = dds.getvalue()[128:128 + n]
                sprites.append(f'sprite kb {hsh:016x} {w} {h} DXT5 {len(blob)} {len(data)}   # {rel}:{key}')
                blob += data
    with open(os.path.join(args.out, 'pad_icons', 'sprites.bin'), 'wb') as f:
        f.write(blob)
    lines.append('# sprite <ps|kb|xb> <fnv64 of the 360 picture> <w> <h> <DXT1|DXT5> <offset> <size> (sprites.bin, as the DDS)')
    lines.append('# kbbox <fnv64> <code> <x> <y> <w> <h>  (a button in that picture: the keyboard version gets its keycap there)')
    lines += sprites
    print(f'sprites: {len(sprites)} ({len(blob) // 1024} KB)')
    with open(os.path.join(args.out, 'pad_icons', 'pad_icons.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
