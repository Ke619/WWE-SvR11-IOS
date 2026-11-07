"""The match types the port adds, one .svrmod each (Mods/MatchTypes/<id>: a
manifest only - the code is in the game, src/match_types.cpp). A player who
doesn't want one switches its mod off on the launcher's Mods tab (or removes
it); bundled, they come switched on (launcher/mods_tab.c).

    python make_matchtype_mods.py [<out dir>]     (default: "Bundled Mods" at the top of the project)
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(TOP, "Bundled Mods")
VERSION = "1.0"

# (id, name, where it is in the game)
MATCH_TYPES = [
    ("falls_count_anywhere", "Falls Count Anywhere",
     "ONE ON ONE, TWO ON TWO, TRIPLE THREAT and FATAL-4-WAY: pins and submissions count anywhere."),
    ("championship_scramble", "Championship Scramble",
     "FATAL-4-WAY: five superstars, one more each minute, interim champions, the title holder at the bell wins."),
    ("royal_rumble_15_25", "15- and 25-Man Royal Rumble", "ROYAL RUMBLE: 15-MAN and 25-MAN beside 10, 20 and 30."),
    ("lumberjack", "Lumberjack", "6-MAN: LUMBERJACK, with lumberjacks who attack whoever lands on the floor."),
    # (free_roaming_backstage: removed from the game for 2.1.6 - src/match_types.cpp MatchTypeSwitches)
    ("backstage_more_people", "Backstage for 3, 4 and 6", "BACKSTAGE brawls in TRIPLE THREAT, FATAL-4-WAY and 6-MAN."),
    ("weapons_everywhere", "Weapons Everywhere", "EXTREME RULES: weapons already lying in and around the ring."),
    ("slobber_knocker", "Slobber Knocker", "One wrestler against an endless line of opponents."),
    ("three_stages_of_hell", "Three Stages of Hell",
     "ONE ON ONE -> EXTREME RULES: a normal fall, then Falls Count Anywhere, then Last Man Standing."),
    ("elimination", "Elimination", "TRIPLE THREAT and FATAL-4-WAY: a pin or give up eliminates; the last one left wins."),
    ("mystery_opponent", "Mystery Opponent", "ONE ON ONE -> NORMAL MATCH: the CPU picks your opponent in secret."),
    ("hiac_tables_ladders", "Hell in a Cell: Tables & Ladders", "HELL IN A CELL: tables, ladders and chairs lying inside the cell."),
    ("hiac_roof_start", "Hell in a Cell: Start on the Roof", "HELL IN A CELL: the match starts on top of the cell."),
    ("three_stages_tables_ladders", "Three Stages of Hell: Tables & Ladders",
     "THREE STAGES OF HELL (one on one, two on two, triple threat, fatal-4-way): TABLE / LADDER / TABLE and TABLE / TLC / LADDER, in the TLC match."),
    ("gauntlet_eliminator", "Gauntlet Eliminator", "HANDICAP: six superstars, two start, one more each minute; a pin or submission eliminates."),
    ("cage_weapons", "Steel Cage: Weapons Inside", "STEEL CAGE: chairs and other weapons lying in the ring."),
    ("inferno_weapons", "Inferno: Weapons Inside", "INFERNO MATCH: chairs and other weapons lying in the ring."),
    ("online_first_blood", "Online First Blood", "ONLINE Player Match: FIRST BLOOD under EXTREME RULES MATCHES (one on one, 3-man)."),
]


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    for mid, name, about in MATCH_TYPES:
        manifest = "\n".join([
            "type=matchtype", f"id={mid}", f"name={name}", "author=SvR 2011 PC port", f"version={VERSION}",
            "made_with=Port tools (SvR 2011)", f"about={about}", "",
        ])
        path = os.path.join(OUT, f"matchtype_{mid}.svrmod")
        data = manifest.encode("utf-8")
        if os.path.exists(path):
            with zipfile.ZipFile(path) as z:
                if z.namelist() == ["manifest.txt"] and z.read("manifest.txt") == data:
                    continue  # (unchanged: the launcher installs a bundle again only when it changes)
        info = zipfile.ZipInfo("manifest.txt", date_time=(2026, 10, 7, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        with zipfile.ZipFile(path + ".tmp", "w") as z:
            z.writestr(info, data)
        os.replace(path + ".tmp", path)
        print("wrote", os.path.basename(path))
    print(len(MATCH_TYPES), "match type mods in", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
