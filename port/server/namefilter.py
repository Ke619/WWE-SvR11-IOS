"""Community Creations name filter (server.py): the names and texts players put on the server - Community
Creations titles, tags and comments, Discovery mods' names / tags / descriptions, entrance songs' and movies'
names, account names - checked against a word list the dashboard edits (the Names tab).

The list lives in the server's data folder (name_filter.json: {"enabled", "words", "allow"}), not in the repo:
a new server starts from the seed below (kept compressed so the repo shows no plain list) and writes the file.

A list line is a term (letters; leetspeak and look-alikes in the list are read the same way as in names):
    hitler      a long term (5+ letters): found inside a word too ("xHitlerx"), or spelled across words
                ("hit ler", "h i t l e r")
    fag         a short one (under 5 letters): only as a whole word (+ s), or spelled out letter by letter
                ("f a g", "f.a.g") - not inside other words ("Scunthorpe" stays fine)
    =word       whole word only, whatever its length
    *word       inside words too, whatever its length
    two words   a phrase: the letters together, across the words ("sieg heil", "siegheil")
    # ...       a comment
The allow list: words that are never flagged though a term is inside them ("snigger").

How a name is read: NFKC + case folding; look-alike letters (Cyrillic / Greek) as Latin, accents dropped,
invisible characters removed; then three ways - leetspeak with 1 as i, with 1 as l (0 o, 3 e, 4 a, 5 s, 7 t,
8 b, 9 g, @ a, $ s, ! i, | i ...), and letters alone; anything else separates words. A term's letters may
repeat ("hiiitler"); a doubled letter in the term needs at least two ("coon" is not "con").
"""

import base64
import json
import os
import re
import threading
import unicodedata
import zlib

SHORT = 5           # (terms shorter than this: whole words only)
MAX_SPAN = 12       # (words a term may be spelled across)
REASON = "That name isn't allowed on Community Creations."
ACCOUNT_REASON = "Your account name isn't allowed on Community Creations."

# Look-alikes NFKC leaves as they are: Cyrillic and Greek letters that read as Latin ones.
CONFUSABLE = str.maketrans({
    "а": "a", "в": "b", "е": "e", "ё": "e", "к": "k", "м": "m", "н": "h", "о": "o", "р": "p", "с": "c",
    "т": "t", "у": "y", "х": "x", "ѕ": "s", "і": "i", "ї": "i", "ј": "j", "ԁ": "d", "ɡ": "g", "ԛ": "q",
    "ԝ": "w", "ı": "i", "ɩ": "i", "ӏ": "l", "ɑ": "a", "ʏ": "y", "ʀ": "r", "ɴ": "n", "ᴋ": "k",
    "α": "a", "β": "b", "ε": "e", "η": "n", "ι": "i", "κ": "k", "ν": "v", "ο": "o", "ρ": "p", "τ": "t",
    "υ": "u", "χ": "x", "ω": "w", "γ": "y", "μ": "u", "ς": "c", "σ": "o", "δ": "d", "λ": "l",
})
LEET = {"0": "o", "2": "z", "3": "e", "4": "a", "5": "s", "6": "g", "7": "t", "8": "b", "9": "g",
        "@": "a", "$": "s", "!": "i", "|": "i", "+": "t", "€": "e", "£": "l", "¥": "y", "¡": "i", "(": "c",
        "<": "c", "{": "c", "[": "c", "&": "a"}
ONE = ("i", "l")  # (1: either - two readings)

# The seed list (zlib + base64): a new server's name_filter.json starts from it.
_SEED = "eNotT0GOwzAIvPMKpL5gpT304scQh2Ji11i2o2z6+tJ0LzNIzDDDDRNNxnE+FysDqa74UNk7D0g6C3cIlV4KIbEWGMqC13T4lrHZ8VHknCH8/N7vIDRwJsaNjwE37BSVynWVZ6oacZS9D6gq4sYPkQeoXFB4QohmFcJoGr+Y4eC5kPPCVN0Uk1aPEzPHrJkdT8dOkphWmN6pXFNolBU2FVrMoFmPCZ9WM5/w0ta4f1UbNa96FUNX7hMH/+1UdJ5Xc+G6eu6DRMwLOkOYnWo93Wb+bf//KnSe1Nc3WfF1nQ=="
_SEED_ALLOW = ["snigger", "sniggers", "sniggered", "sniggering", "sniggerer", "scunthorpe"]


def seed():
    words = zlib.decompress(base64.b64decode(_SEED)).decode("utf-8").split("\n") if _SEED else []
    return {"enabled": True, "words": [w for w in words if w.strip()], "allow": list(_SEED_ALLOW)}


def _fold(text):
    """NFKC + case folding, look-alikes as Latin, accents and invisible characters gone."""
    t = unicodedata.normalize("NFKC", str(text or "")).casefold().translate(CONFUSABLE)
    out = []
    for ch in unicodedata.normalize("NFD", t):
        cat = unicodedata.category(ch)
        if cat == "Mn" or cat == "Cf":  # (accents; zero-width and other invisible characters)
            continue
        out.append(ch)
    return unicodedata.normalize("NFC", "".join(out))


def words(text):
    """The name's words in each reading: [[word, ...] per reading] (letters a-z only)."""
    t = _fold(text)
    readings = []
    for one in ONE + (None,):
        cur, ws = [], []
        for ch in t:
            if "a" <= ch <= "z":
                cur.append(ch)
                continue
            if one is not None:
                m = one if ch == "1" else LEET.get(ch)
                if m:
                    cur.append(m)
                    continue
            if cur:
                ws.append("".join(cur))
                cur = []
        if cur:
            ws.append("".join(cur))
        if ws not in readings:
            readings.append(ws)
    return readings


def _letters(term):
    """A list term's letters (its first reading: leetspeak read as letters)."""
    return "".join(words(term)[0]) if words(term) else ""


def _pattern(letters):
    """Each run of a letter: that letter at least as many times ("coon" -> c+o{2,}n+)."""
    out = []
    for m in re.finditer(r"(.)\1*", letters):
        n = len(m.group(0))
        out.append(re.escape(m.group(1)) + ("+" if n == 1 else "{%d,}" % n))
    return "".join(out)


class Term:
    def __init__(self, line):
        raw = line.strip()
        self.line = raw
        mode = ""
        if raw[:1] in "=*":
            mode, raw = raw[0], raw[1:].strip()
        self.letters = _letters(raw)
        self.whole = mode == "=" or (mode != "*" and len(self.letters) < SHORT)
        body = _pattern(self.letters)
        plural = "(?:e?s)?" if re.search(r"(?:s|x|z|ch|sh)$", self.letters) else "s?"
        self.full = re.compile(body + plural) if self.letters else None   # (a whole word, or words spelled out)
        self.inner = re.compile(body) if self.letters else None           # (inside a word)


class NameFilter:
    def __init__(self, folder, log=print):
        self.path = os.path.join(folder, "name_filter.json")
        self.log = log
        self.lock = threading.Lock()
        try:
            with open(self.path, encoding="utf-8") as f:
                data = json.load(f)
        except FileNotFoundError:
            data = seed()
            self._write(data)
            log("names: %s made from the seed list (%d words)" % (self.path, len(data["words"])))
        except (OSError, ValueError) as e:  # (a broken file: the seed in memory, the file left for the owner)
            log("names: %s unreadable (%s) - the seed list until it is saved again" % (self.path, e))
            data = seed()
        self._set(data)

    def _set(self, data):
        self.enabled = bool(data.get("enabled", True))
        self.word_lines = [str(w).strip() for w in data.get("words", []) if str(w).strip()]
        self.allow_lines = [str(w).strip() for w in data.get("allow", []) if str(w).strip()]
        self.terms = [t for t in (Term(w) for w in self.word_lines if not w.startswith("#")) if t.letters]
        self.allow = set()
        for a in self.allow_lines:
            for reading in words(a):
                self.allow.update(reading)

    def _write(self, data):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.path)

    def state(self):
        return {"enabled": self.enabled, "words": self.word_lines, "allow": self.allow_lines,
                "terms": len(self.terms), "short": SHORT}

    def save(self, enabled=None, words_=None, allow=None):
        with self.lock:
            data = {"enabled": self.enabled if enabled is None else bool(enabled),
                    "words": self.word_lines if words_ is None else [str(w) for w in words_],
                    "allow": self.allow_lines if allow is None else [str(w) for w in allow]}
            self._write(data)
            self._set(data)

    # -- the check --------------------------------------------------------------------------------------------

    def match(self, text):
        """The list line a text is flagged for, or None (whatever the switch)."""
        if not text or not self.terms:
            return None
        for ws in words(text):
            if not ws:
                continue
            for t in self.terms:
                if self._hit(t, ws):
                    return t.line
        return None

    def _hit(self, t, ws):
        n = len(ws)
        for s in range(n):
            joined = ""
            for e in range(s, min(n, s + MAX_SPAN)):
                joined += ws[e]
                if e > s and t.whole and any(len(w) > 2 for w in ws[s:e + 1]):
                    break  # (a short term across words: only when spelled out letter by letter)
                if t.full.fullmatch(joined):
                    if e == s and ws[s] in self.allow:
                        continue
                    return True
            if not t.whole and ws[s] not in self.allow and t.inner.search(ws[s]):
                return True
        return False

    def check(self, *texts):
        """REASON when the filter is on and any text is flagged, else None."""
        if not self.enabled:
            return None
        for text in texts:
            if self.match(text):
                return REASON
        return None
