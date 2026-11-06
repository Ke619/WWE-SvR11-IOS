"""A game's DLC packages as extra source files, for the converters (superstar_port's Src).

The player gives one folder (--dlc, the Mod Maker's "DLC folder" in My game discs). Under it, up to 3 levels down:
Xbox 360 packages as the console keeps them (STFS: LIVE / PIRS / CON files, read straight from the package) and / or
packages already unpacked (a folder holding pac/ch). Only the packages of the source game's own title id count.

What a package adds: its superstars' model pacs. They are named pac/ch/ch<id>.pac or pac/ch/ch<id>_dlc<nn>.pac, and
`get('pac/ch/ch<id>.pac')` finds either. Its chEtc holds the character tables in the disc's format (GameW.merge_dlc).
Files are copied out once to <cache>/dlc/<package>/ when a step asks for them; the packages are only read.

    py dlc_src.py <folder> [title id hex]     lists the packages (JSON)
"""
import json
import os
import re
import sys

import stfs

MODEL = re.compile(r'^pac/ch/ch(\d+)(?:_dlc\d+)?\.pac$')
ETC = re.compile(r'^pac/ch/chetc(?:_dlc\d+)?\.pac$')
MAX_DEPTH = 5   # (Xenia's content folder: <profile>/<title id>/00000002/<package>/pac/ch; the console's Content folder too)


class Package:
    """One package: `files` {lower-case path: (its path, size, reader)}"""

    def __init__(self, path, name, title, display):
        self.path, self.name, self.title, self.display = path, name, title, display
        self.files = {}
        self._models = None

    def models(self):
        """{superstar id: lower-case path of the model pac}"""
        if self._models is None:
            self._models = {}
            for low in self.files:
                m = MODEL.match(low)
                if m:
                    self._models[int(m.group(1))] = low
        return self._models

    def etc(self):
        return next((low for low in sorted(self.files) if ETC.match(low)), None)

    def matching(self, pattern):
        """the paths matching a regular expression on the lower-case path, sorted"""
        return [low for low in sorted(self.files) if re.match(pattern, low)]

    def read(self, low):
        return self.files[low][2]()


def notice(text):
    """a line for the player: the Mod Maker shows it on screen when the job ends (tool_run.cpp)"""
    print('notice ' + text.replace('\n', ' '), file=sys.stderr, flush=True)


def _stfs_package(path):
    s = stfs.Stfs(path)
    p = Package(path, os.path.basename(path), s.title_id, s.display_name)
    for rel, size, entry in s.files:
        rel = rel.replace('\\', '/')
        p.files[rel.lower()] = (rel, size, lambda e=entry, r=s.read: r(e))
    return p


def _folder_package(path):
    p = Package(path, os.path.basename(os.path.normpath(path)), None, os.path.basename(os.path.normpath(path)))
    for d, _, names in os.walk(path):
        for n in names:
            full = os.path.join(d, n)
            rel = os.path.relpath(full, path).replace('\\', '/')

            def reader(f=full):
                with open(f, 'rb') as h:
                    return h.read()
            p.files[rel.lower()] = (rel, os.path.getsize(full), reader)
    return p


def _is_package(path):
    try:
        if os.path.getsize(path) < 0x1000:
            return False
        with open(path, 'rb') as h:
            return h.read(4) in (b'LIVE', b'PIRS', b'CON ')
    except OSError:
        return False


def find_packages(folder):
    """every package under the folder (a package file itself too), a folder holding pac/ch as one unpacked package"""
    out = []
    if os.path.isfile(folder):
        return [_stfs_package(folder)] if _is_package(folder) else []

    def walk(d, depth):
        if os.path.isdir(os.path.join(d, 'pac', 'ch')):
            out.append(_folder_package(d))
            return
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        for n in names:
            full = os.path.join(d, n)
            if os.path.isdir(full):
                if depth < MAX_DEPTH:
                    walk(full, depth + 1)
            elif _is_package(full):
                try:
                    out.append(_stfs_package(full))
                except (ValueError, OSError, IndexError, UnicodeError):
                    notice('DLC: %s can\'t be read (damaged?): left out.' % n)   # (the others still count)
    walk(folder, 0)
    return out


class DlcSet:
    """The source game's packages, with a cache for the files a step asks for"""

    def __init__(self, folder, cache, title=None):
        self.folder, self.cache = folder, cache
        every = find_packages(folder)
        # (an unpacked package has no title id: it counts; another game's package in the same folder doesn't)
        self.packages = [p for p in every if title is None or p.title is None or p.title == title]
        self.others = len(every) - len(self.packages)
        if self.others and not self.packages:
            notice('DLC folder: its %d package(s) belong to another game: none used.' % self.others)
        self._by_model = {}
        for p in self.packages:
            for i in p.models():
                self._by_model.setdefault(i, p)

    def __bool__(self):
        return bool(self.packages)

    def find(self, rel):
        """(package, lower-case path) of a file: exact, or pac/ch/ch<id>.pac in its ch<id>_dlc<nn>.pac form"""
        low = rel.replace('\\', '/').lower()
        for p in self.packages:
            if low in p.files:
                return p, low
        m = MODEL.match(low)
        if m and int(m.group(1)) in self._by_model:
            p = self._by_model[int(m.group(1))]
            return p, p.models()[int(m.group(1))]
        return None, None

    def exists(self, rel):
        return self.find(rel)[0] is not None

    def package_of(self, cid):
        return self._by_model.get(cid)

    def get(self, rel):
        """a local path of the file (copied out once), or None"""
        p, low = self.find(rel)
        if p is None:
            return None
        dst = os.path.join(self.cache, 'dlc', re.sub(r'[^0-9A-Za-z._-]', '_', p.name)[:48], *low.split('/'))
        if not os.path.exists(dst) or os.path.getsize(dst) != p.files[low][1]:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + '.part'
            with open(tmp, 'wb') as f:
                f.write(p.read(low))
            os.replace(tmp, dst)
        return dst


class SourceView:
    """A disc reader (discfs: exists / isdir / listdir / read / size / walk / info) with the game's packages beside it:
    what the disc lacks comes from them. `reads` gives every copy of a file: two packages can hold the same name
    (WWE '12's Pack 3 and Pack 4 both have pac/evt/NyujyoC.pac, each with its own entrances)."""

    def __init__(self, disc, dlc):
        self.disc, self.dlc = disc, dlc

    def __getattr__(self, name):
        return getattr(self.disc, name)

    @staticmethod
    def _low(path):
        return path.replace('\\', '/').strip('/').lower()

    def _copies(self, path):
        low = self._low(path)
        return [p for p in self.dlc.packages if low in p.files]

    def exists(self, path):
        return self.disc.exists(path) or bool(self._copies(path)) or self.isdir(path)

    def isdir(self, path):
        pre = self._low(path) + '/'
        return (hasattr(self.disc, 'isdir') and self.disc.isdir(path)) or any(
            low.startswith(pre) for p in self.dlc.packages for low in p.files)

    def listdir(self, path=''):
        try:
            names = list(self.disc.listdir(path))
        except (OSError, KeyError, ValueError):
            names = []
        seen = {n.lower() for n in names}
        pre = self._low(path) + '/' if self._low(path) else ''
        for p in self.dlc.packages:
            for low, (rel, _, _) in p.files.items():
                if low.startswith(pre) and '/' not in low[len(pre):] and low[len(pre):] not in seen:
                    seen.add(low[len(pre):])
                    names.append(rel.rsplit('/', 1)[-1])
        return names

    def read(self, path, off=None, size=None):
        if self.disc.exists(path):
            return self.disc.read(path) if off is None else self.disc.read(path, off, size)
        copies = self._copies(path)
        if not copies:
            return self.disc.read(path)
        d = copies[0].read(self._low(path))
        return d if off is None else d[off:off + (len(d) - off if size is None else size)]

    def reads(self, path):
        """every copy of the file: the disc's, then each package's"""
        out = [self.disc.read(path)] if self.disc.exists(path) else []
        return out + [p.read(self._low(path)) for p in self._copies(path)]

    def size(self, path):
        if self.disc.exists(path):
            return self.disc.size(path)
        copies = self._copies(path)
        return copies[0].files[self._low(path)][1] if copies else self.disc.size(path)


def view(disc):
    """a disc reader with SVR2011_PORT_DLC's packages beside it (superstar_port --dlc), or the reader itself"""
    folder = os.environ.get('SVR2011_PORT_DLC')
    if not folder:
        return disc
    try:
        title = disc.info().get('title_id')
    except (AttributeError, OSError, ValueError):
        title = None
    dlc = DlcSet(folder, '', int(title, 16) if title else None)
    return SourceView(disc, dlc) if dlc else disc


def main(argv):
    title = int(argv[2], 16) if len(argv) > 2 else None
    s = DlcSet(argv[1], '', title)
    print(json.dumps([dict(name=p.name, title='%08X' % p.title if p.title is not None else None, display=p.display,
                           superstars=sorted(p.models())) for p in s.packages]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
