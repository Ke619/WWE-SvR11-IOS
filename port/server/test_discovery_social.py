"""Checks Discovery comments and star ratings (discovery_social.py, docs/DISCOVERY_API.md "Comments and ratings") on
a private server of its own: loopback, a throwaway data folder, --test-open-accounts, the dashboard on loopback.

  the mod page's rating / comment count / rights; posting (signed in only), every automatic refusal (links,
  invites, the name filter, the comment word list and its look-alikes, split archives, length, lines, repeats,
  the rate limit), who may delete, reports, ratings (own mod, change, take back, bad values), the rating sort and
  discovery_rating_min, mutes, the dashboard's hide / show / delete / clear reports / clear ratings, the switches,
  the Names tab's comment word list, a banned account, a hidden mod, account and mod deletion, the 30-day prune.

    python test_discovery_social.py [--port 8532] [--keep]
"""
import argparse
import io
import json
import os
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
FAILS = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        FAILS.append(what)


def request(url, method="GET", data=None, headers=None):
    r = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(r, timeout=30) as f:
            return f.status, f.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


class Player:
    def __init__(self, url, name):
        self.url, self.name = url, name
        self.ip = "10.77.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))
        st, r = self.call("POST", "/api/register", {"name": name, "password": "pw-" + name}, token=False)
        self.token = r.get("token", "")
        self.id = r.get("id")

    def call(self, method, path, value=None, token=True):
        headers = {"Content-Type": "application/json", "X-Forwarded-For": self.ip}
        if token and self.token:
            headers["Authorization"] = "Bearer " + self.token
        data = json.dumps(value).encode() if value is not None else (b"{}" if method == "POST" else None)
        st, body = request(self.url + path, method, data, headers)
        try:
            return st, json.loads(body or b"{}")
        except ValueError:
            return st, {"raw": body[:200]}

    def comment(self, mid, text):
        return self.call("POST", "/api/mods/%d/comments" % mid, {"text": text})

    def rate(self, mid, stars):
        return self.call("POST", "/api/mods/%d/rating" % mid, {"stars": stars})


def admin(url, method, path, value=None):
    data = json.dumps(value).encode() if value is not None else (b"{}" if method == "POST" else None)
    st, body = request(url + path, method, data, {"Content-Type": "application/json"})
    try:
        return st, json.loads(body or b"{}")
    except ValueError:
        return st, {"raw": body[:200]}


def anon(url, path):
    st, body = request(url + path)
    try:
        return st, json.loads(body or b"{}")
    except ValueError:
        return st, {}


def jpeg():
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (1024, 768), (90, 40, 60)).save(b, "JPEG", quality=70)
    return b.getvalue()


def svrmod(name):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.txt", "type=superstar\nname=%s\nversion=1.0\n" % name)
        z.writestr("data.bin", os.urandom(4 << 10))
    return out.getvalue()


def upload(p, name, img):
    boundary = "----ds" + secrets.token_hex(8)
    body = io.BytesIO()
    for n, v in {"name": name, "version": "1.0", "types": "superstar", "tags": "test"}.items():
        body.write(('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, n, v)).encode())
    files = dict({"svrmod": (name + ".svrmod", svrmod(name), "application/zip")},
                 **{"img%d" % n: ("img%d.jpg" % n, img, "image/jpeg") for n in range(4)})
    for n, (fn, data, ctype) in files.items():
        body.write(('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\nContent-Type: %s\r\n\r\n' % (
            boundary, n, fn, ctype)).encode())
        body.write(data + b"\r\n")
    body.write(("--%s--\r\n" % boundary).encode())
    st, out = request(p.url + "/api/mods", "POST", body.getvalue(),
                      {"Authorization": "Bearer " + p.token, "X-Forwarded-For": p.ip,
                       "Content-Type": "multipart/form-data; boundary=" + boundary})
    return json.loads(out).get("id")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8532)  # (my test servers: 8530 and up)
    ap.add_argument("--keep", action="store_true", help="leave the data folder")
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix="svr_social_")
    url = "http://127.0.0.1:%d" % args.port
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py"), "--host", "127.0.0.1", "--port",
                             str(args.port), "--data", data, "--local-admin", "--test-open-accounts",
                             "--log", os.path.join(data, "server.log")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        for _ in range(150):
            if proc.poll() is not None:
                break
            try:
                if request(url + "/api/status")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        check(proc.poll() is None, "private server up on 127.0.0.1:%d" % args.port)
        run(url, data)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    prune_check(data)
    log_check(data)
    if not args.keep:
        shutil.rmtree(data, ignore_errors=True)
    else:
        print("data folder kept:", data)
    print("all passed" if not FAILS else "%d FAILED" % len(FAILS))
    return 0 if not FAILS else 1


def run(url, data):
    tag = secrets.token_hex(2)
    a, b, c, d, e = (Player(url, "%s_%s" % (n, tag)) for n in ("ann", "bob", "cat", "dan", "eve"))
    check(all(p.token for p in (a, b, c, d, e)), "five test accounts")
    img = jpeg()
    m1, m3 = upload(a, "Social one", img), upload(a, "Social three", img)
    m2 = upload(b, "Social two", img)
    check(m1 and m2 and m3, "three mods (ann: %s, %s; bob: %s)" % (m1, m3, m2))

    # -- a mod's page
    st, r = anon(url, "/api/mods/%d" % m1)
    check(st == 200 and r.get("rating") == {"avg": 0, "count": 0} and r.get("comments") == 0 and
          r.get("comments_on") is True and r.get("ratings_on") is True and "can_rate" not in r,
          "page, no token: rating 0/0, 0 comments, both on, no rights")
    st, r = b.call("GET", "/api/mods/%d" % m1)
    check(r.get("can_rate") is True and r.get("can_comment") is True and r.get("my_rating") == 0,
          "page, another account: can rate and comment, my_rating 0")
    st, r = a.call("GET", "/api/mods/%d" % m1)
    check(r.get("can_rate") is False and r.get("can_comment") is True and r.get("mine") is True,
          "page, the uploader: can't rate its own, can comment")

    # -- posting
    st, r = b.call("POST", "/api/mods/%d/comments" % m1, {"text": "no token"}, token=False)
    check(st == 401 and r.get("code") == "sign_in_needed", "no token: 401 sign_in_needed")
    st, r = b.comment(m1, "  Great mod, the entrance is perfect!  ")
    check(st == 200 and r.get("comment", {}).get("text") == "Great mod, the entrance is perfect!" and
          r["comment"].get("author") == b.name and r.get("total") == 1, "bob comments (trimmed, author = his name)")
    first = r.get("comment", {}).get("id")
    st, r = b.comment(m1, "Great mod, the entrance is perfect!")
    check(st == 400 and r.get("code") == "repeat", "the same text again: repeat")
    st, r = b.comment(m1, "GREAT MOD, the entrance is perfect!")
    check(st == 400 and r.get("code") == "repeat", "the same text in capitals: repeat")
    refusals = [
        ("", "empty"), ("   \n\n  ", "empty"), ("x" * 501, "too_long"), ("\n".join(["line %d" % i for i in range(9)]), "too_many_lines"),
        ("see www.example.com for more", "link"), ("http://x.y", "link"), ("my site example.com", "link"),
        ("join us at discord.gg/abc123", "link"), ("join discord . gg/abc", "link"), ("t.me/somebody", "link"),
        ("our server .gg/xyz", "link"), ("youtu.be/abc", "link"), ("discord.com/invite/abc", "link"),
        ("where is the iso", "piracy"), ("i s o please", "piracy"), ("anyone got the I.S.O", "piracy"),
        ("drop the default.xex", "piracy"), ("is there a torrent", "piracy"), ("ROMS here", "piracy"),
        ("download the game first", "piracy"), ("get SvR.7z.001 and .002", "piracy"), ("part2: game.part1.rar", "piracy"),
        ("the nsp works", "piracy"), ("Free Download now", "piracy"),
    ]
    for text, code in refusals:
        st, r = c.comment(m1, text)
        check(st == 400 and r.get("code") == code and r.get("error"),
              "refused %-15s %r -> %s (%s)" % (code, text[:30], r.get("code"), r.get("error", "")[:50]))
    # (refusals don't count toward the rate limit: cat posts 4 here, ann 3 - 5 a minute each at most)
    for n, text in enumerate(("The isolated ring looks great", "Roman Reigns attire is spot on",
                              "Version 1.2.3 works fine", "Use the Mod Maker to install it",
                              "My email is in my profile", "Loved the pyro, 10/10", "Works on Steam Deck too")):
        st, r = (c if n < 4 else a).comment(m2, text)
        check(st == 200, "passes: %r (%s)" % (text, r.get("code", "")))

    # -- the name filter's list (a test word through the Names tab), then put back
    st, names = admin(url, "GET", "/api/admin/names")
    old_words = names.get("words", [])
    admin(url, "POST", "/api/admin/names", {"words": old_words + ["zzslurtest"]})
    st, r = d.comment(m1, "you zzslurtest")
    check(st == 400 and r.get("code") == "word", "the name filter: word (%s)" % r.get("code"))
    admin(url, "POST", "/api/admin/names", {"words": old_words})

    # -- the comment word list (the Names tab, ?list=comments)
    st, r = admin(url, "GET", "/api/admin/names?list=comments")
    check(st == 200 and "iso" in r.get("words", []) and "torrent" in r.get("words", []), "comment word list: seeded")
    words = r.get("words", [])
    st, r = admin(url, "POST", "/api/admin/names", {"list": "comments", "words": words + ["zzpiratesite"]})
    check(st == 200 and "zzpiratesite" in r.get("words", []), "comment word list: a word added from the dashboard")
    st, r = admin(url, "GET", "/api/admin/names")
    check("zzpiratesite" not in r.get("words", []), "the name filter's list unchanged by it")
    st, r = d.comment(m1, "try zzpiratesite for it")
    check(st == 400 and r.get("code") == "piracy", "the added word refused at once (%s)" % r.get("code"))
    st, r = admin(url, "POST", "/api/admin/names/test", {"list": "comments", "text": "x.e.x"})
    check(st == 200 and r.get("passes") is False, "Names tab test on the comment list: x.e.x refused")
    st, r = admin(url, "POST", "/api/admin/names", {"list": "comments", "enabled": False})
    st, r = d.comment(m1, "the iso talk while the list is off")
    check(st == 200, "comment word list off: let through (%s)" % r.get("code"))
    admin(url, "POST", "/api/admin/names", {"list": "comments", "enabled": True, "words": words})

    # -- the rate limit (eve: 5 a minute)
    codes = [e.comment(m3, "eve comment number %d" % i)[1].get("code", "") for i in range(6)]
    check(codes[:5] == [""] * 5 and codes[5] == "too_fast", "rate limit: 5 a minute, the 6th too_fast (%s)" % codes)

    # -- reading
    st, r = anon(url, "/api/mods/%d/comments" % m2)
    check(st == 200 and r.get("enabled") and r.get("total") == 7 and r["comments"][0]["text"] ==
          "Works on Steam Deck too" and "can_delete" not in r["comments"][0] and "account" not in r["comments"][0],
          "list: newest first, no rights or account ids without a token (total %s)" % r.get("total"))
    st, r = b.call("GET", "/api/mods/%d/comments" % m2)
    check(all(x.get("can_delete") and not x.get("mine") for x in r.get("comments", [])),
          "the mod's uploader may delete every comment on it")
    st, r = d.call("GET", "/api/mods/%d/comments" % m2)
    check(not any(x.get("can_delete") for x in r.get("comments", [])) and r.get("can_comment") is True,
          "another account may delete none")
    st, r = anon(url, "/api/mods/%d" % m2)
    check(r.get("comments") == 7, "the page's comment count (%s)" % r.get("comments"))
    st, r = anon(url, "/api/mods/%d/comments?page=1" % m2)
    check(st == 200 and r.get("page") == 1 and r.get("comments") == [], "page 1 of 7: empty")

    # -- deleting
    st, r = c.call("GET", "/api/mods/%d/comments" % m2)
    cat_ids = [x["id"] for x in r.get("comments", []) if x.get("mine")]
    check(len(cat_ids) == 4, "cat's own: 4, marked mine")
    st, r = d.call("DELETE", "/api/mods/%d/comments/%d" % (m2, cat_ids[0]))
    check(st == 403 and r.get("code") == "not_yours", "dan can't delete cat's comment")
    st, r = c.call("DELETE", "/api/mods/%d/comments/%d" % (m1, cat_ids[0]))
    check(st == 404 and r.get("code") == "no_comment", "the wrong mod in the path: 404")
    st, r = c.call("DELETE", "/api/mods/%d/comments/%d" % (m2, cat_ids[0]))
    check(st == 200, "cat deletes her own")
    st, r = b.call("DELETE", "/api/mods/%d/comments/%d" % (m2, cat_ids[1]))
    check(st == 200, "bob (the uploader) deletes one of cat's on his mod")
    st, r = c.call("DELETE", "/api/mods/%d/comments/%d" % (m2, cat_ids[0]))
    check(st == 404, "deleted already: 404")
    st, r = anon(url, "/api/mods/%d/comments" % m2)
    check(r.get("total") == 5 and cat_ids[0] not in [x["id"] for x in r["comments"]], "both gone for players")
    st, r = admin(url, "GET", "/api/admin/discovery/comments?state=deleted")
    dl = {x["id"]: x for x in r.get("comments", [])}
    check(cat_ids[0] in dl and dl[cat_ids[0]]["changed_by"] == "writer" and dl.get(cat_ids[1], {}).get(
        "changed_by") == "owner", "the dashboard still sees them: deleted by writer / owner")

    # -- reports
    st, r = d.call("POST", "/api/mods/%d/comments/%d/report" % (m1, first), {"why": "  rude  "})
    check(st == 200 and r.get("new") is True, "dan reports bob's comment")
    st, r = d.call("POST", "/api/mods/%d/comments/%d/report" % (m1, first), {"why": "again"})
    check(st == 200 and r.get("new") is False, "again: not counted twice")
    st, r = c.call("POST", "/api/mods/%d/comments/%d/report" % (m1, first), {})
    st, r = admin(url, "GET", "/api/admin/discovery/comments")
    top = (r.get("comments") or [{}])[0]
    check(top.get("id") == first and top.get("reports") == 2 and [x["by"] for x in top.get("reported_by", [])] ==
          [d.name, c.name] and top["reported_by"][0]["why"] == "rude" and top.get("mod_name") == "Social one",
          "the dashboard: reported first, 2 reports, who and why")
    st, r = b.call("POST", "/api/mods/%d/comments/%d/report" % (m1, first), {}, token=False)
    check(st == 401, "report without a token: 401")
    st, r = admin(url, "POST", "/api/admin/discovery/comment/%d/clear-reports" % first)
    st, r = admin(url, "GET", "/api/admin/discovery/comments?state=reported")
    check(r.get("total") == 0, "clear reports: none reported")

    # -- the dashboard's hide / show
    admin(url, "POST", "/api/admin/discovery/comment/%d/hide" % first)
    st, r = anon(url, "/api/mods/%d/comments" % m1)
    check(first not in [x["id"] for x in r.get("comments", [])], "hidden: gone for players")
    st, r = b.call("DELETE", "/api/mods/%d/comments/%d" % (m1, first))
    check(st == 404, "a hidden comment can't be deleted by its writer")
    admin(url, "POST", "/api/admin/discovery/comment/%d/show" % first)
    st, r = anon(url, "/api/mods/%d/comments" % m1)
    check(first in [x["id"] for x in r.get("comments", [])], "shown again")

    # -- ratings
    st, r = a.rate(m1, 5)
    check(st == 403 and r.get("code") == "own_mod", "the uploader rates its own: own_mod")
    st, r = b.rate(m1, 5)
    check(st == 200 and r.get("rating") == {"avg": 5, "count": 1} and r.get("my_rating") == 5, "bob 5: 5.0 / 1")
    st, r = c.rate(m1, 2)
    check(r.get("rating") == {"avg": 3.5, "count": 2}, "cat 2: 3.5 / 2 (%s)" % r.get("rating"))
    st, r = b.rate(m1, 1)
    check(r.get("rating") == {"avg": 1.5, "count": 2}, "bob changes to 1: 1.5 / 2 (%s)" % r.get("rating"))
    for bad in (6, -1, "5", 2.5, True, None):
        st, r = b.rate(m1, bad)
        check(st == 400 and r.get("code") == "bad_stars", "stars %r: bad_stars" % (bad,))
    st, r = b.rate(m1, 0)
    check(r.get("rating") == {"avg": 2, "count": 1} and r.get("my_rating") == 0, "bob takes his back: 2 / 1")
    st, r = c.call("GET", "/api/mods/%d" % m1)
    check(r.get("my_rating") == 2 and r.get("rating") == {"avg": 2, "count": 1}, "the page: cat's own rating 2")
    st, r = b.call("POST", "/api/mods/%d/rating" % m1, {"stars": 4}, token=False)
    check(st == 401, "rating without a token: 401")

    # -- the rating sort: m2 4.0 x 3, m1 5.0 x 1 (fewer than discovery_rating_min 3), m3 none
    c.rate(m1, 5)
    for p in (a, c, d):
        p.rate(m2, 4)
    st, r = anon(url, "/api/mods?sort=rating")
    order = [x["id"] for x in r.get("mods", [])]
    check(order[:3] == [m2, m1, m3] and r.get("ratings_on") is True,
          "sort=rating, min 3: the 3-rating mod before the single 5 (%s)" % order)
    admin(url, "POST", "/api/admin/settings", {"discovery_rating_min": 1})
    st, r = anon(url, "/api/mods?sort=rating")
    order = [x["id"] for x in r.get("mods", [])]
    check(order[:3] == [m1, m2, m3], "min 1: the single 5 first (%s)" % order)
    admin(url, "POST", "/api/admin/settings", {"discovery_rating_min": 3})
    row = [x for x in r.get("mods", []) if x["id"] == m2][0]
    check(row.get("rating") == {"avg": 4, "count": 3}, "the list's row has the rating")

    # -- mutes
    st, r = admin(url, "GET", "/api/admin/discovery/comments?account=%d" % c.id)
    cat_comment = r["comments"][0]["id"]
    admin(url, "POST", "/api/admin/discovery/comment/%d/mute-comments" % cat_comment)
    time.sleep(61)  # (cat's comments a minute: free again, so only the mute refuses her)
    st, r = c.comment(m3, "muted cat speaks")
    check(st == 403 and r.get("code") == "muted", "muted (comments): comment refused")
    st, r = c.call("GET", "/api/mods/%d" % m3)
    check(r.get("can_comment") is False and r.get("can_rate") is True, "the page: can't comment, can rate")
    st, r = c.rate(m3, 3)
    check(st == 200, "muted (comments): still rates")
    admin(url, "POST", "/api/admin/discovery/mute", {"account": c.id, "scope": "all", "note": "test"})
    st, r = c.rate(m3, 4)
    check(st == 403 and r.get("code") == "muted_ratings", "muted (all): rating refused")
    st, r = admin(url, "GET", "/api/admin/discovery/comments")
    check([m["name"] for m in r.get("mutes", [])] == [c.name] and r["mutes"][0]["scope"] == "all",
          "the dashboard lists the mute")
    admin(url, "POST", "/api/admin/discovery/mute", {"account": c.id, "scope": ""})
    st, r = c.comment(m3, "cat is back")
    check(st == 200, "unmuted: comments again (%s)" % r.get("code"))
    st, r = admin(url, "POST", "/api/admin/discovery/mute", {"account": c.id, "scope": "forever"})
    check(st == 400, "a bad scope: 400")

    # -- the switches
    admin(url, "POST", "/api/admin/settings", {"discovery_comments": "off"})
    st, r = anon(url, "/api/mods/%d/comments" % m1)
    check(st == 200 and r.get("enabled") is False and r.get("comments") == [], "comments off: the list empty, enabled false")
    st, r = d.comment(m1, "while off")
    check(st == 403 and r.get("code") == "comments_off", "comments off: posting refused")
    st, r = anon(url, "/api/mods/%d" % m1)
    check(r.get("comments_on") is False and r.get("comments") == 0, "comments off: the page says so")
    admin(url, "POST", "/api/admin/settings", {"discovery_comments": "on"})
    admin(url, "POST", "/api/admin/settings", {"discovery_ratings": "off"})
    st, r = anon(url, "/api/mods?sort=rating")
    check(r.get("ratings_on") is False and all("rating" not in x for x in r.get("mods", [])) and
          [x["id"] for x in r["mods"]][:1] == [m2], "ratings off: no rating in rows, sort falls back to new")
    st, r = d.rate(m1, 3)
    check(st == 403 and r.get("code") == "ratings_off", "ratings off: rating refused")
    st, r = d.call("GET", "/api/mods/%d" % m1)
    check("rating" not in r and r.get("can_rate") is False and r.get("ratings_on") is False, "ratings off: the page")
    admin(url, "POST", "/api/admin/settings", {"discovery_ratings": "on"})
    st, r = anon(url, "/api/mods/%d" % m2)
    check(r.get("rating") == {"avg": 4, "count": 3}, "ratings on again: kept (%s)" % r.get("rating"))

    # -- clear a mod's ratings
    st, r = admin(url, "POST", "/api/admin/mod/%d/clear-ratings" % m1)
    check(st == 200 and r.get("cleared") == 1, "clear ratings: 1 cleared (%s)" % r.get("cleared"))
    st, r = anon(url, "/api/mods/%d" % m1)
    check(r.get("rating") == {"avg": 0, "count": 0}, "the mod's rating 0 / 0")

    # -- a hidden mod
    admin(url, "POST", "/api/admin/mod/%d/hide" % m3)
    st, r = anon(url, "/api/mods/%d/comments" % m3)
    check(st == 404 and r.get("code") == "no_mod", "a hidden mod: comments 404")
    st, r = d.rate(m3, 3)
    check(st == 404, "a hidden mod: rating 404")
    admin(url, "POST", "/api/admin/mod/%d/show" % m3)

    # -- a banned account
    users = {u["name"]: u for u in admin(url, "GET", "/api/admin/users")[1]}
    admin(url, "POST", "/api/admin/user/%d/ban" % users[e.name]["id"])
    st, r = e.comment(m1, "eve banned")
    check(st == 401 and r.get("code") == "sign_in_needed", "banned: its session is gone (401)")

    # -- account deleted: dan's ratings, reports and comments go; the mods' counts follow
    d.call("POST", "/api/mods/%d/comments/%d/report" % (m1, first), {"why": "x"})
    st, r = admin(url, "POST", "/api/admin/user/%d/delete" % users[d.name]["id"])
    st, r = anon(url, "/api/mods/%d" % m2)
    check(r.get("rating") == {"avg": 4, "count": 2}, "dan deleted: m2 4.0 / 2 (%s)" % r.get("rating"))
    st, r = admin(url, "GET", "/api/admin/discovery/comments?state=reported")
    check(r.get("total") == 0, "dan's report gone")
    db = sqlite3.connect(os.path.join(data, "community.db"))
    n = db.execute("SELECT (SELECT COUNT(*) FROM mod_ratings WHERE account = ?) + (SELECT COUNT(*) FROM mod_comments"
                   " WHERE account = ?)", (d.id, d.id)).fetchone()[0]
    check(n == 0, "dan's rows gone from the database")

    # -- mod deleted by its owner: its comments and ratings go
    st, r = a.call("DELETE", "/api/mods/%d" % m1)
    check(st == 200, "ann deletes mod %d" % m1)
    n = db.execute("SELECT (SELECT COUNT(*) FROM mod_comments WHERE mod = ?) + (SELECT COUNT(*) FROM mod_ratings"
                   " WHERE mod = ?) + (SELECT COUNT(*) FROM mod_comment_reports WHERE comment = ?)",
                   (m1, m1, first)).fetchone()[0]
    check(n == 0, "its comments, ratings and reports gone")
    st, r = anon(url, "/api/mods/%d/comments" % m1)
    check(st == 404, "its comments: 404")
    db.close()

    # -- the dashboard's delete for good
    st, r = admin(url, "GET", "/api/admin/discovery/comments?mod=%d" % m3)
    victim = r["comments"][0]["id"]
    admin(url, "POST", "/api/admin/discovery/comment/%d/delete" % victim)
    st, r = admin(url, "GET", "/api/admin/discovery/comments?mod=%d" % m3)
    check(victim not in [x["id"] for x in r.get("comments", [])], "the dashboard's Delete: gone for good")
    st, r = admin(url, "POST", "/api/admin/discovery/comment/%d/explode" % cat_comment)
    check(st == 400, "an unknown action: 400")

    # -- the dashboard's routes from outside (a public request through the portal: not an admin)
    outside = {"X-Portal-Public": "1", "X-Forwarded-For": "203.0.113.9", "Content-Type": "application/json"}
    for method, path in (("GET", "/api/admin/discovery/comments"), ("POST", "/api/admin/discovery/mute"),
                         ("POST", "/api/admin/discovery/comment/%d/hide" % cat_comment),
                         ("GET", "/api/admin/names?list=comments"), ("POST", "/api/admin/names"),
                         ("POST", "/api/admin/mod/%d/clear-ratings" % m2)):
        st, _ = request(url + path, method, b"{}" if method == "POST" else None, outside)
        check(st in (401, 403), "from outside: %s %s refused (%d)" % (method, path, st))


def prune_check(data):
    """discovery_social.prune on the stopped server's database: a comment deleted 31 days ago goes, one deleted
    today stays."""
    import discovery_social

    class S:
        pass
    s = S()
    s.db = sqlite3.connect(os.path.join(data, "community.db"))
    s.lock = threading.RLock()
    rows = [r[0] for r in s.db.execute("SELECT id FROM mod_comments WHERE state = 2 ORDER BY id")]
    if len(rows) < 2:
        check(False, "prune: two deleted comments to work with (%d)" % len(rows))
        return
    s.db.execute("UPDATE mod_comments SET changed = ? WHERE id = ?", (time.time() - 31 * 86400, rows[0]))
    s.db.commit()
    gone = discovery_social.prune(s)
    left = [r[0] for r in s.db.execute("SELECT id FROM mod_comments WHERE state = 2 ORDER BY id")]
    check(gone == 1 and left == rows[1:], "prune: the 31-day-old deleted comment removed, the newer one kept")
    s.db.close()


def log_check(data):
    try:
        text = open(os.path.join(data, "server.log"), encoding="utf-8", errors="replace").read()
    except OSError:
        text = ""
    check("discovery: comment " in text and "refused (piracy)" in text, "the log has comments and refusals")
    check("Great mod, the entrance" not in text and "Bearer" not in text, "the log has no comment text, no token")
    check("Traceback" not in text and "error:" not in text, "no errors in the server log")


if __name__ == "__main__":
    sys.exit(main())
