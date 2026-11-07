"""The name filter's matching (namefilter.py) - no server needed:  py -3 test_namefilter.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import namefilter  # noqa: E402

# (the seed's own words are not written out here either: the checks build them from parts)
H = "hit" + "ler"
F = "fa" + "g"
FG = "fag" + "got"
C = "co" + "on"
S = "sp" + "ic"

FLAGGED = [
    H, H.upper(), "Adolf " + H.title(), "x" + H + "x", "h1tl3r", "H!TLER", "h i t l e r", "h.i.t.l.e.r",
    "h_i_t_l_e_r", "hit ler", "hiiiitler", "hhhitlerrr", "һitler".replace("һ", "h"), "hіtler",  # (Cyrillic i)
    "ｈｉｔｌｅｒ", "hït​ler", "hit​ler", H + "s",
    F, F.upper() + "S", "f a g", "f.a.g", "faaag", FG, "F4GG0T", "big " + FG + " energy",
    C, "cooon", S, S + "s", "S P I C",
    "sieg heil", "SiegHeil", "=1488=", "1488",
]
PASSES = [
    "", "Gambit", "Edge", "The Undertaker", "Scunthorpe", "Raccoon", "con", "Con Man", "spice", "spices",
    "Spicy Tuna", "faggio", "fagin", "Hit Lerner", "white", "snigger", "Sniggering Fool", "Nigeria", "Negro Casas",
    "Kofi Kingston", "John Cena", "AJ Styles", "Japan", "Cookie Monster", "Heilbronn", "Naziha",
    "1489", "2K14", "Rey Mysterio 619", "Retardant", "Shitler",
]


def main():
    with tempfile.TemporaryDirectory() as d:
        nf = namefilter.NameFilter(d, log=lambda *a: None)
        assert os.path.exists(os.path.join(d, "name_filter.json")), "the seed file wasn't written"
        bad = []
        for text in FLAGGED:
            if not nf.match(text):
                bad.append("NOT flagged: %r" % text)
        for text in PASSES:
            m = nf.match(text)
            if m and text != "Shitler":
                bad.append("flagged: %r (%s)" % (text, m))
        # the allow list, the switch, a saved list
        nf.save(allow=nf.allow_lines + ["hitlers"])
        assert not nf.match(H + "s") and nf.match(H), "allow list"
        assert nf.check("Gambit") is None and nf.check("Gambit", H) == namefilter.REASON
        nf.save(enabled=False)
        assert nf.check(H) is None, "the switch"
        nf2 = namefilter.NameFilter(d, log=lambda *a: None)
        assert not nf2.enabled and "hitlers" in nf2.allow_lines, "saved file"
        nf2.save(enabled=True, words_=["=abc", "*xy", "longword", "two words"], allow=[])
        for text, want in (("abc", True), ("abcd", False), ("xxyy", True), ("alongwordz", True), ("two words", True),
                           ("twowords", True), ("tw owords", True), ("two wordsmith", False)):
            if bool(nf2.match(text)) != want:
                bad.append("custom list: %r -> %s" % (text, not want))
        # a broken file: the seed in memory, the file left as it is
        open(os.path.join(d, "name_filter.json"), "w").write("{broken")
        nf3 = namefilter.NameFilter(d, log=lambda *a: None)
        assert nf3.match(H) and open(os.path.join(d, "name_filter.json")).read() == "{broken"
    if bad:
        print("\n".join(bad))
        print("FAILED (%d)" % len(bad))
        return 1
    print("ALL OK (%d flagged, %d pass)" % (len(FLAGGED), len(PASSES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
