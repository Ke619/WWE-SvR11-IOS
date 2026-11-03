"""Checks POST /api/reports and the dashboard's report routes against a running private server
(never the live one): py test_reports.py --url http://127.0.0.1:8495 --node-data <the node's --data>

The server needs --local-admin (the dashboard routes from this PC). Each request gives its own X-Forwarded-For
(from loopback the server takes it as the sender), so the rate limit is tested on one address only."""
import argparse
import io
import json
import os
import secrets
import struct
import sys
import urllib.error
import urllib.request
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reports  # noqa: E402

FAILS = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def png(w=4, h=3):
    raw = b"".join(b"\x00" + b"\x80\x20\x20" * w for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


JPG = b"\xff\xd8\xff\xe0" + b"\x00" * 64 + b"\xff\xd9"


import datetime  # noqa: E402

CRASH_NAME = "crashes/crash_%s.txt" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S")  # (dated: it counts)


def report_zip(crash=None, log=None):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("system.txt", "Windows 10.0.19045\n")
        if crash:
            z.writestr(CRASH_NAME, crash)
        if log:
            z.writestr("logs/svr2011.log", log)
    return out.getvalue()


def multipart(parts):
    boundary = "----svr" + secrets.token_hex(8)
    body = io.BytesIO()
    for part in parts:  # ((name, data, type), or (name, file name, type, data))
        name, filename, ctype, data = part if len(part) == 4 else (part[0], part[0], part[2], part[1])
        body.write(("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n" %
                    (boundary, name, filename, ctype)).encode())
        body.write(data)
        body.write(b"\r\n")
    body.write(("--%s--\r\n" % boundary).encode())
    return body.getvalue(), "multipart/form-data; boundary=" + boundary


def post(url, meta, zip_bytes=None, images=(), ip=None, raw_parts=None):
    parts = raw_parts if raw_parts is not None else (
        [("report", json.dumps(meta).encode(), "application/json")] +
        ([("zip", zip_bytes, "application/zip")] if zip_bytes is not None else []) +
        [("image%d" % n, img, "image/png") for n, img in enumerate(images)])
    body, ctype = multipart(parts)
    r = urllib.request.Request(url + "/api/reports", data=body, method="POST",
                               headers={"Content-Type": ctype, "X-Forwarded-For": ip or "10.9.%d.%d" % (
                                   secrets.randbelow(250), secrets.randbelow(250))})
    try:
        with urllib.request.urlopen(r, timeout=120) as f:
            return f.status, json.loads(f.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except ValueError:
            return e.code, {}


def get(url, path):
    try:
        with urllib.request.urlopen(url + path, timeout=60) as f:
            return f.status, f.read()
    except urllib.error.HTTPError as e:
        return e.code, b""


def meta(**kw):
    m = {"format": 1, "app": "launcher", "version": "2.1.5", "platform": "windows", "os": "10.0.19045",
         "problem": "Crash", "happens": "every time", "what": "It closed when I pressed Play.",
         "before": "Installed a mod.", "contact": ""}
    m.update(kw)
    return m


def post_json(url, path, body, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    r = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST", headers=headers)
    try:
        with urllib.request.urlopen(r, timeout=60) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def reply(url, rid, secret, text, images=(), ip=None):
    parts = [("report", json.dumps({"id": rid, "secret": secret, "text": text}).encode(), "application/json")]
    parts += [("image%d" % n, img, "image/png") for n, img in enumerate(images)]
    body, ctype = multipart(parts)
    headers = {"Content-Type": ctype, "X-Forwarded-For": ip or "10.9.%d.%d" % (secrets.randbelow(250),
                                                                               secrets.randbelow(250))}
    r = urllib.request.Request(url + "/api/reports/reply", data=body, method="POST", headers=headers)
    try:
        with urllib.request.urlopen(r, timeout=60) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def edit(url, rid, secret, what="", before="", contact="", images=(), ip=None):
    parts = [("report", json.dumps({"id": rid, "secret": secret, "what": what, "before": before,
                                    "contact": contact}).encode(), "application/json")]
    parts += [("image%d" % n, img, "image/png") for n, img in enumerate(images)]
    body, ctype = multipart(parts)
    headers = {"Content-Type": ctype, "X-Forwarded-For": ip or "10.8.%d.%d" % (secrets.randbelow(250),
                                                                               secrets.randbelow(250))}
    r = urllib.request.Request(url + "/api/reports/edit", data=body, method="POST", headers=headers)
    try:
        with urllib.request.urlopen(r, timeout=60) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def flagged_word(url):
    """A word the server's saved name list flags (found through the dashboard's tester), or None."""
    for line in json.loads(get(url, "/api/admin/names")[1] or b"{}").get("words", [])[:40]:
        w = "".join(ch for ch in str(line).lower() if "a" <= ch <= "z")
        if len(w) >= 4 and not post_json(url, "/api/admin/names/test", {"text": w})[1].get("passes", True):
            return w
    return None


def edits(url, node_data, r1, r2):
    """The sender's "Edit / add pictures": POST /api/reports/edit - more text, a new contact, pictures (5 in all),
    kept as a history beside the report as it was sent; closed reports refuse it."""
    def state(r):
        return (post_json(url, "/api/reports/status", {"reports": [{"id": r["id"], "secret": r["secret"]}]})[1]
                .get("reports") or [{}])[0]
    one = state(r1)
    check("status: no edits, its 2 pictures, editable (%s)" % one,
          one.get("edits") == 0 and one.get("pictures") == 2 and one.get("editable") is True)
    st, d = edit(url, r1["id"], "wrong", what="more")
    check("edit: a wrong secret -> 404", st == 404)
    st, d = edit(url, r1["id"], r1["secret"])
    check("edit: nothing to add -> 400 (%s)" % d.get("error"), st == 400)
    st, d = edit(url, r1["id"], r1["secret"], what="It also happens in the Cell.", before="I had changed the arena.",
                 contact="discord: tester", images=[png(), png(5, 4)])
    check("edit: text, contact and 2 pictures -> 200, 4 pictures now (%s)" % d,
          st == 200 and d.get("ok") and d.get("edits") == 1 and d.get("pictures") == 4)
    st, d = edit(url, r1["id"], r1["secret"], images=[png(), png()])
    check("edit: 2 more pictures would make 6 -> 409 (%s)" % d.get("error"), st == 409 and "5 pictures" in d.get("error", ""))
    st, d = edit(url, r1["id"], r1["secret"], images=[png(6, 2)])
    check("edit: the 5th picture -> 200 (%s)" % d, st == 200 and d.get("edits") == 2 and d.get("pictures") == 5)
    folder = os.path.join(node_data, "reports", reports._day(r1["id"]), r1["id"])
    log = [json.loads(l) for l in open(os.path.join(folder, "edits.jsonl"), encoding="utf-8")]
    check("edits.jsonl: both changes, their pictures on disk (%s)" % [sorted(e) for e in log],
          len(log) == 2 and log[0]["what"] == "It also happens in the Cell." and log[0]["before"] == "I had changed the arena."
          and log[0]["contact"] == "discord: tester" and log[0]["images"] == ["edit0_0.png", "edit0_1.png"]
          and log[1]["images"] == ["edit1_0.png"] and "what" not in log[1]
          and all(os.path.isfile(os.path.join(folder, n)) for n in ("edit0_0.png", "edit0_1.png", "edit1_0.png")))
    rec = json.load(open(os.path.join(folder, "report.json"), encoding="utf-8"))
    check("report.json: as it was sent", rec["what"] == "It closed when I pressed Play." and len(rec["images"]) == 2)
    one = state(r1)
    check("status: 2 edits, 5 pictures (%s)" % one, one.get("edits") == 2 and one.get("pictures") == 5)
    rows = json.loads(get(url, "/api/admin/reports")[1])["reports"]
    row = next((x for x in rows if x["id"] == r1["id"]), {})
    check("the dashboard's list: edits 2 and when (%s %s)" % (row.get("edits"), row.get("edited")),
          row.get("edits") == 2 and len(row.get("edited", "")) == 19)
    st, body = get(url, "/api/admin/report/%s/edits.jsonl" % r1["id"])
    check("the dashboard reads the changes", st == 200 and b"in the Cell" in body)
    st, body = get(url, "/api/admin/report/%s/edit0_1.png" % r1["id"])
    check("the dashboard shows an added picture", st == 200 and body == png(5, 4))
    import subprocess
    cli = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports.py"), "--data", node_data]
    out = subprocess.run(cli + ["show", r1["id"]], capture_output=True, text=True).stdout
    check("reports.py show: the edits", "edit 1 (" in out and "It also happens in the Cell." in out
          and "edit1_0.png" in out and "discord: tester" in out)
    out = subprocess.run(cli + ["list"], capture_output=True, text=True).stdout
    check("reports.py list: + 2 edit(s)", any(l.startswith(r1["id"]) and "+ 2 edit(s)" in l for l in out.splitlines()))
    bad = flagged_word(url)
    if bad:
        st, d = edit(url, r1["id"], r1["secret"], before="you are a %s" % bad)
        check("edit: the name filter refuses a flagged word (%s)" % d.get("error"), st == 400)
    # closed: done (and duplicate) refuse changes; open again, they take them
    mark(url, r1["id"], "done")
    st, d = edit(url, r1["id"], r1["secret"], what="once more")
    check("edit: done -> 409 (%s)" % d.get("error"), st == 409)
    check("status: done, not editable", state(r1).get("editable") is False)
    mark(url, r1["id"], "looking")
    # rate limit: 5 changes in 10 minutes from one address
    ip = "10.252.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))
    codes = [edit(url, r2["id"], r2["secret"], what="note %d" % n, ip=ip)[0] for n in range(6)]
    check("edit: 5 from one address, the 6th 429 (%s)" % codes, codes == [200] * 5 + [429])


def mark(url, rid, status, fix=None):
    return post_json(url, "/api/admin/report/%s/mark" % rid, {"status": status, "fix": fix})


def receipts(url, node_data, r1, r2, ver):
    """The senders' side: a secret per report, POST /api/reports/status, needs-info questions and replies."""
    check("a receipt: a secret in the answer, not stored as it is",
          len(r1.get("secret", "")) >= 20 and r1["secret"] != r2.get("secret"))
    day = "%s-%s-%s" % (r1["id"][0:4], r1["id"][4:6], r1["id"][6:8])
    text = open(os.path.join(node_data, "reports", day, r1["id"], "report.json"), encoding="utf-8").read()
    check("report.json: the secret's hash only", r1["secret"] not in text and "secret_hash" in text)
    st, d = post_json(url, "/api/reports/status", {"reports": [{"id": r1["id"], "secret": r1["secret"]},
                                                                {"id": r2["id"], "secret": "wrong"},
                                                                {"id": "20990101-000000-abcdef", "secret": "x"}]})
    rows = {x["id"]: x for x in d.get("reports", [])}
    check("status: the right secret -> received (%s)" % rows.get(r1["id"]),
          st == 200 and rows.get(r1["id"], {}).get("state") == "received")
    check("status: a wrong secret or no such report -> unknown, nothing else said",
          rows.get(r2["id"]) == {"id": r2["id"], "state": "unknown"} and
          rows.get("20990101-000000-abcdef") == {"id": "20990101-000000-abcdef", "state": "unknown"})
    one = rows.get(r1["id"], {})
    check("status: only the public fields (%s)" % sorted(one),
          set(one) <= {"id", "state", "received", "app", "problem", "what", "note", "version", "question", "date",
                       "replies", "from_account", "edits", "pictures", "editable"}
          and "contact" not in one and "signature" not in one)
    st, d = post_json(url, "/api/reports/status", {"reports": "nope"})
    check("status: not a list -> 400", st == 400)
    # a reply before any question: refused
    st, d = reply(url, r1["id"], r1["secret"], "here is more")
    check("reply: not asked yet -> 409 (%s)" % d.get("error"), st == 409)
    # the question (dashboard / reports.py mark --needs-info)
    st, d = mark(url, r1["id"], "needs_info", {"question": "Which arena was it?"})
    check("mark needs_info with a question: 200", st == 200)
    st, d = mark(url, r1["id"], "needs_info", {"question": ""})
    check("mark needs_info without a question: refused", st == 404)
    st, d = post_json(url, "/api/reports/status", {"reports": [{"id": r1["id"], "secret": r1["secret"]}]})
    one = (d.get("reports") or [{}])[0]
    check("status: needs_info with the question (%s)" % one,
          one.get("state") == "needs_info" and one.get("question") == "Which arena was it?" and one.get("replies") == 0)
    st, d = reply(url, r1["id"], "wrong", "an answer")
    check("reply: a wrong secret -> 404", st == 404)
    st, d = reply(url, r1["id"], r1["secret"], "", [])
    check("reply: empty -> 400", st == 400)
    st, d = reply(url, r1["id"], r1["secret"], "It was the RAW arena, after a ladder match.", [png()])
    check("reply: the answer with a picture -> 200 (%s)" % d, st == 200 and d.get("ok"))
    st, d = post_json(url, "/api/reports/status", {"reports": [{"id": r1["id"], "secret": r1["secret"]}]})
    one = (d.get("reports") or [{}])[0]
    check("status: replied, one answer (%s)" % one, one.get("state") == "replied" and one.get("replies") == 1)
    folder = os.path.join(node_data, "reports", day, r1["id"])
    got = [json.loads(l) for l in open(os.path.join(folder, "replies.jsonl"), encoding="utf-8")]
    check("replies.jsonl: the text and its picture", got and got[0]["text"].startswith("It was the RAW arena")
          and got[0]["images"] == ["reply0_0.png"] and os.path.isfile(os.path.join(folder, "reply0_0.png")))
    st, body = get(url, "/api/admin/report/%s/replies.jsonl" % r1["id"])
    check("the dashboard reads the answers", st == 200 and b"RAW arena" in body)
    st, d = mark(url, r1["id"], "replied")
    check("replied: only a player's answer sets it", st == 404)
    # fixed: the note and version
    st, d = mark(url, r1["id"], "fixed", {"note": "Ladder matches in RAW no longer crash.", "version": "2.1.9"})
    st, d = post_json(url, "/api/reports/status", {"reports": [{"id": r1["id"], "secret": r1["secret"]}]})
    one = (d.get("reports") or [{}])[0]
    check("status: fixed with the note and version (%s)" % one, one.get("state") == "fixed"
          and one.get("note") == "Ladder matches in RAW no longer crash." and one.get("version") == "2.1.9")
    st, d = reply(url, r1["id"], r1["secret"], "more")
    check("reply: once fixed -> 409", st == 409)
    st, d = mark(url, r1["id"], "looking")
    st, d = post_json(url, "/api/reports/status", {"reports": [{"id": r1["id"], "secret": r1["secret"]}]})
    check("status: looking", (d.get("reports") or [{}])[0].get("state") == "looking")
    # the dashboard's list: no secret hashes, the question / answers count
    st, body = get(url, "/api/admin/reports")
    check("the dashboard's list: no secret hashes", st == 200 and b"secret_hash" not in body)
    # the name filter on answers
    mark(url, r2["id"], "needs_info", {"question": "Again?"})
    bad = None  # (a word the saved list flags, found through the dashboard's tester)
    for line in json.loads(get(url, "/api/admin/names")[1] or b"{}").get("words", [])[:40]:
        w = "".join(ch for ch in str(line).lower() if "a" <= ch <= "z")
        if len(w) >= 4 and not post_json(url, "/api/admin/names/test", {"text": w})[1].get("passes", True):
            bad = w
            break
    if bad:
        st, d = reply(url, r2["id"], r2["secret"], "you are a %s" % bad)
        check("reply: the name filter refuses a flagged word (%s)" % d.get("error"), st == 400)
    # rate limit: 5 answers in 10 minutes from one address
    ip = "10.251.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))
    codes = [reply(url, r2["id"], r2["secret"], "answer %d" % n, ip=ip)[0] for n in range(6)]
    check("reply: 5 at most per report (%s)" % codes, codes[:5] == [200] * 5 and codes[5] in (409, 429))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--node-data", required=True, help="the --data folder of the server that keeps the reports")
    a = ap.parse_args()
    url = a.url.rstrip("/")
    tag = secrets.token_hex(3)  # (this run's own version: earlier runs don't count as duplicates)
    ver = "9.9." + tag

    # the signature's normalising (offline)
    n1 = reports.normalise("[2026-10-24 15:30:12.123] exception 0xC0000005 at 0x7FF6A1B2C3D4 thread 1234 in D:\\Games\\SvR\\svr2011.exe")
    n2 = reports.normalise("[2026-10-25 01:02:03.999] exception 0xC0000005 at 0x7FF6000000AA thread 77 in C:\\x\\svr2011.exe")
    check("normalise: times, addresses, thread ids and folders out (%s)" % n1, n1 == n2)

    # report zips carry old crash files too: the newest counts, one from before the session doesn't, report.txt
    # is no log (01e167a8: only the reported session - its logs and the crash reports written while it ran)
    stamp = lambda days: (datetime.datetime.now() - datetime.timedelta(days=days)).strftime("%Y%m%d_%H%M%S")
    log_time = lambda days: (datetime.datetime.now() - datetime.timedelta(days=days)).strftime("[%Y-%m-%d %H:%M:%S]")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("report.txt", "contents: the crashes folder, an error summary\n")
        z.writestr("crashes/crash_%s.txt" % stamp(5), "exception OLD\n")
        z.writestr("crashes/crash_%s.txt" % stamp(0.01), "exception NEW\n")
    check("the newest crash file, not an old one", reports.first_error_line(out.getvalue(), "launcher") == "exception NEW")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("report.txt", "contents: the crashes folder, an error summary\n")
        z.writestr("crashes/crash_%s.txt" % stamp(5), "exception OLD\n")
        z.writestr("logs/svr2011_7.log", "%s [info] started\n" % log_time(0.01))
    check("a crash file from before the session and report.txt: no line (the client's hint stands in)",
          reports.first_error_line(out.getvalue(), "launcher") == "")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("game/crashes/crash_%s.txt" % stamp(5), "exception GAME\n")
        z.writestr("logs/modmaker_%s.log" % stamp(0.01).replace("_", "-"), "%s info\nERROR: convert failed\n"
                   % log_time(0.01))
    check("the Mod Maker: its own log, not a game crash from before its session",
          reports.first_error_line(out.getvalue(), "modmaker") == "ERROR: convert failed")

    crash1 = "exception 0xC0000005 at 0x7FF6A1B2C3D4 (svr2011.exe+0x1234)\nstack...\n"
    crash2 = "exception 0xC0000005 at 0x7FF600001111 (svr2011.exe+0x1234)\nother stack\n"
    img = png()
    st, r1 = post(url, meta(version=ver), report_zip(crash1), [img, JPG])
    check("a report with a zip and two pictures: 200 (%s)" % r1, st == 200 and r1.get("ok") and r1.get("seen_before") == 0)
    st, r2 = post(url, meta(version=ver, what="again"), report_zip(crash2))
    check("the same crash at another address: the same signature, seen_before 1",
          st == 200 and r2.get("signature") == r1.get("signature") and r2.get("seen_before") == 1)
    st, r3 = post(url, meta(version=ver, problem="Freeze"), report_zip(crash1))
    check("another problem: another signature", st == 200 and r3.get("signature") != r1.get("signature"))
    log = "12:00:01 info: started\n12:00:05 ERROR: pac load failed: arena.pac (0x80070002)\n12:00:06 ERROR: second\n"
    st, r4 = post(url, meta(version=ver, app="modmaker", problem="Mods"), report_zip(log=log))
    st5, r5 = post(url, meta(version=ver, app="modmaker", problem="Mods"),
                   report_zip(log=log.replace("12:00:05", "23:59:59").replace("0x80070002", "0x80070003")))
    check("a log's first ERROR line, times out: the same signature",
          st == st5 == 200 and r4.get("signature") == r5.get("signature") and r5.get("seen_before") == 1)
    st, r6 = post(url, meta(version=ver, app="android", signature="Game froze on the title screen"))
    st7, r7 = post(url, meta(version=ver, app="android", signature="Game froze on the title screen"))
    check("no zip: the client's hint stands in", st == st7 == 200 and r6.get("signature") == r7.get("signature")
          and r7.get("seen_before") == 1)

    # refused
    st, r = post(url, meta(app="xbox"))
    check("an unknown app: 400 (%s)" % r.get("error"), st == 400 and not r.get("ok"))
    st, r = post(url, meta(problem=""))
    check("no problem: 400", st == 400)
    st, r = post(url, None, raw_parts=[("zip", report_zip(crash1), "application/zip")])
    check("no report part: 400", st == 400)
    st, r = post(url, None, raw_parts=[("report", b"{not json", "application/json")])
    check("bad JSON: 400", st == 400)
    st, r = post(url, meta(version=ver), report_zip(crash1), [b"GIF89a....."])
    check("a GIF picture: 400 (%s)" % r.get("error"), st == 400)
    st, r = post(url, meta(version=ver), None, [img] * 6)
    check("six pictures: 400", st == 400)
    st, r = post(url, meta(version=ver), b"not a zip")
    check("a zip that isn't one: 400", st == 400)
    st, r = post(url, meta(version=ver), b"PK" + os.urandom((25 << 20) + 10))
    check("a 25 MB+ zip: 413 (%s)" % r.get("error"), st == 413)
    st, r = post(url, meta(version=ver), None, [b"\x89PNG\r\n\x1a\n" + b"\x00" * (8 << 20)])
    check("an 8 MB+ picture: 413", st == 413)

    # the rate limit: 5 in 10 minutes from one address
    ip = "10.250.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))
    codes = [post(url, meta(version=ver, problem="Rate"), ip=ip)[0] for _ in range(6)]
    check("rate limit: 5 accepted, the 6th 429 (%s)" % codes, codes == [200] * 5 + [429])

    # the files on the PC
    day = "%s-%s-%s" % (r1["id"][0:4], r1["id"][4:6], r1["id"][6:8])
    folder = os.path.join(a.node_data, "reports", day, r1["id"])
    files = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
    check("on the node: report.json, report.zip, image0.png, image1.jpg (%s)" % files,
          files == ["image0.png", "image1.jpg", "report.json", "report.zip"])
    if files:
        rec = json.load(open(os.path.join(folder, "report.json"), encoding="utf-8"))
        text = open(os.path.join(folder, "report.json"), encoding="utf-8").read()
        check("report.json: the fields, the signature and its line", rec["what"] == "It closed when I pressed Play."
              and rec["signature"] == r1["signature"] and "0xC0000005" in rec["signature_line"])
        check("report.json: no sender address", "10.9." not in text and "127.0.0.1" not in text and "ip" not in rec)
        check("report.zip: the bytes sent", open(os.path.join(folder, "report.zip"), "rb").read() == report_zip(crash1)
              or zipfile.ZipFile(os.path.join(folder, "report.zip")).read(CRASH_NAME).decode() == crash1)
    index = [json.loads(l) for l in open(os.path.join(a.node_data, "reports", "index.jsonl"), encoding="utf-8")]
    mine = [x for x in index if x.get("version") == ver]
    receipts(url, a.node_data, r1, r2, ver)
    edits(url, a.node_data, r1, r2)
    check("index.jsonl: a line for each accepted report (%d)" % len(mine), len(mine) == 12)

    # the dashboard's routes
    st, body = get(url, "/api/admin/reports")
    rows = json.loads(body).get("reports", []) if st == 200 else []
    one = next((x for x in rows if x["id"] == r1["id"]), None)
    check("admin list: the report, its signature's count 2", one is not None and one["count"] == 2)
    st, body = get(url, "/api/admin/report/%s/image0.png" % r1["id"])
    check("admin picture: the PNG", st == 200 and body == img)
    st, body = get(url, "/api/admin/report/%s/report.zip" % r1["id"])
    check("admin zip: 200", st == 200 and body[:2] == b"PK")
    st, _ = get(url, "/api/admin/report/%s/..%%2Fsettings.json" % r1["id"])
    check("admin file outside a report: 404", st == 404)
    req = urllib.request.Request(url + "/api/admin/report/%s/mark" % r1["id"], method="POST",
                                 data=json.dumps({"status": "done"}).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as f:
        check("mark done: 200", f.status == 200)
    st, body = get(url, "/api/admin/reports")
    one = next((x for x in json.loads(body)["reports"] if x["id"] == r1["id"]), {})
    check("the mark shows in the list", one.get("status") == "done")

    # fixed: a whole signature group (the admin API), with how / commit / version / date
    def post_json(path, body):
        req = urllib.request.Request(url + path, method="POST", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as f:
                return f.status, json.loads(f.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, {}
    fix = {"note": "The pin count no longer repeats.", "commit": "abc1234", "version": "2.1.6", "date": "2026-10-25"}
    st, d = post_json("/api/admin/report/%s/mark" % r1["signature"], {"status": "fixed", "fix": fix})
    check("mark a signature fixed: 200, both its reports (%s)" % d.get("ids"),
          st == 200 and sorted(d.get("ids", [])) == sorted([r1["id"], r2["id"]]))
    st, body = get(url, "/api/admin/reports")
    rows = json.loads(body)["reports"]
    one = next((x for x in rows if x["id"] == r2["id"]), {})
    check("the fix shows in the list (status fixed, note, commit, version, date)",
          one.get("status") == "fixed" and one.get("fix") == fix)
    st, d = post_json("/api/admin/report/%s/mark" % ("0" * 16), {"status": "fixed", "fix": fix})
    check("mark an unknown signature: 404", st == 404)
    st, d = post_json("/api/admin/report/%s/mark" % r3["id"], {"status": "fixed", "fix": {"note": "x" * 5000}})
    rows = json.loads(get(url, "/api/admin/reports")[1])["reports"]
    one = next((x for x in rows if x["id"] == r3["id"]), {})
    check("a long note is cut, no date given -> today",
          st == 200 and len(one.get("fix", {}).get("note", "")) == 1000 and len(one["fix"]["date"]) == 10)

    # delete: the folder, the index line and the mark go
    folder = os.path.join(a.node_data, "reports", reports._day(r2["id"]), r2["id"])
    st, d = post_json("/api/admin/report/%s/delete" % r2["id"], {})
    rows = json.loads(get(url, "/api/admin/reports")[1])["reports"]
    status = json.load(open(os.path.join(a.node_data, "reports", "status.json"), encoding="utf-8"))
    check("delete: 200, the folder, its index line and its mark gone",
          st == 200 and not os.path.isdir(folder) and not any(x["id"] == r2["id"] for x in rows) and r2["id"] not in status)
    one = next((x for x in rows if x["id"] == r1["id"]), {})
    check("delete: its signature's count down to 1 (%s)" % one.get("count"), one.get("count") == 1)
    st, d = post_json("/api/admin/report/%s/delete" % r2["id"], {})
    check("delete it again: 404", st == 404)

    # the CLI on the node's folder (as Main runs it on the node's PC)
    import subprocess
    cli = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports.py"), "--data", a.node_data]
    out = subprocess.run(cli + ["mark", r6["id"], "--fixed", "Signed in again.", "--commit", "def5678", "--version",
                                "2.1.6"], capture_output=True, text=True)
    rows = json.loads(get(url, "/api/admin/reports")[1])["reports"]
    one = next((x for x in rows if x["id"] == r6["id"]), {})
    check("CLI mark --fixed: the server lists it (%s)" % out.stdout.strip(),
          out.returncode == 0 and one.get("status") == "fixed" and one["fix"]["commit"] == "def5678")
    out = subprocess.run(cli + ["list", "--fixed"], capture_output=True, text=True)
    check("CLI list --fixed: the fixed ones", out.returncode == 0 and r6["id"] in out.stdout and r1["id"] in out.stdout)
    out = subprocess.run(cli + ["mark", "nope", "--done"], capture_output=True, text=True)
    check("CLI mark an unknown id: an error", out.returncode != 0)

    # the match folder and its mods (the launcher's check, then only the missing mods with the report)
    import hashlib

    def client_hash(files):  # (HCTP's rule, written again here: files {relative path: bytes})
        h = hashlib.sha256()
        keep = {p.encode("utf-8").lower(): b for p, b in files.items()
                if not any(x.startswith(".") for x in p.split("/")) and p.split("/")[-1].lower() != "disabled"}
        for path in sorted(keep):
            h.update(path + b"\0" + len(keep[path]).to_bytes(8, "little") + keep[path])
        return h.hexdigest()

    def mod_zip(files):
        out = io.BytesIO()
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
            for p_, b in files.items():
                z.writestr(p_, b)
        return out.getvalue()
    mod1 = {"manifest.txt": b"type=superstar\nid=test_" + tag.encode() + b"\nname=TEST ONE\n",
            "Ch.pac": os.urandom(200000), "renders/Big.dds": os.urandom(50000), ".svr2011_marker": b"x", "disabled": b""}
    mod2 = {"manifest.txt": b"type=arena\nid=arena_" + tag.encode() + b"\n", "arena.pac": os.urandom(120000)}
    h1, h2 = client_hash(mod1), client_hash(mod2)
    check("the server's content hash = the launcher's (dot files and 'disabled' left out, paths lower-cased)",
          reports.content_hash(io.BytesIO(mod_zip(mod1))) == h1)
    req = urllib.request.Request(url + "/api/reports/mods/check", method="POST", headers={"Content-Type": "application/json"},
                                 data=json.dumps({"mods": [{"hash": h1, "size": 1}, {"hash": h2, "size": 1}]}).encode())
    with urllib.request.urlopen(req, timeout=30) as f:
        chk = json.loads(f.read())
    check("mods check: both missing, the caps given (%s)" % {k: chk.get(k) for k in ("max_upload", "max_mod", "mods_off")},
          sorted(chk.get("missing", [])) == sorted([h1, h2]) and chk.get("max_mod") == reports.MAX_MOD and not chk.get("mods_off"))
    match_json = {"format": 1, "complete": True, "game": {"version": "2.1.6", "commit": "abc"}, "start": "2026-10-26T10:00:00",
                  "length_s": 312, "result": "won: JEFF HARDY", "rule": {"id": 0x4D, "name": "SINGLE EXTREME RULES MATCH"},
                  "arena": {"id": 9, "name": "ROYAL RUMBLE", "mod": {"type": "Arenas", "id": "arena_" + tag}},
                  "people": [{"index": 0, "name": "JEFF HARDY", "cpu": False, "mod": {"type": "Superstars", "id": "test"}, "caw": None},
                             {"index": 1, "name": "RANDY ORTON", "cpu": True, "mod": None, "caw": None},
                             {"index": 2, "person": 6, "name": "REFEREE", "mod": None, "caw": None}],
                  "frame_rate": 60, "online": False}
    mz = io.BytesIO()
    with zipfile.ZipFile(mz, "w") as z:
        z.writestr("20261026-100000_JEFF HARDY vs RANDY ORTON/match.json", json.dumps(match_json))
        z.writestr("20261026-100000_JEFF HARDY vs RANDY ORTON/inputs.bin", os.urandom(3000))
    rep_meta = meta(version=ver, problem="Match", what="mods report")
    rep_meta["match"] = "20261026-100000_JEFF HARDY vs RANDY ORTON"
    rep_meta["mods"] = [{"hash": h1, "name": "TEST ONE", "type": "Superstars", "id": "test", "size": 250000, "sent": True},
                        {"hash": h2, "name": "ARENA", "type": "Arenas", "id": "arena", "size": 120000, "sent": True}]
    sent1 = mod_zip(mod1)
    parts = [("report", "report.json", "application/json", json.dumps(rep_meta).encode()),
             ("match", "match.zip", "application/zip", mz.getvalue()),
             ("mod0", h1 + ".zip", "application/zip", sent1),
             ("mod1", h2 + ".zip", "application/zip", mod_zip(dict(mod2, extra=b"changed")))]  # (not its hash)
    st, rm = post(url, None, raw_parts=parts, ip="10.9.77.%d" % (int(tag[:2], 16) % 250))
    check("report with a match and two mods: 200, one stored, one refused for its hash (%s)" % rm.get("mods_refused"),
          st == 200 and rm.get("match") and rm.get("mods_stored") == [h1] and rm.get("mods_refused", {}).get(h2) == "hash mismatch")
    with urllib.request.urlopen(urllib.request.Request(url + "/api/reports/mods/check", method="POST",
                                                       headers={"Content-Type": "application/json"},
                                                       data=json.dumps({"mods": [{"hash": h1}, {"hash": h2}]}).encode()),
                                timeout=30) as f:
        chk = json.loads(f.read())
    check("mods check again: only the refused one still missing", chk.get("missing") == [h2])
    st, body = get(url, "/api/admin/reports")
    d = json.loads(body)
    one = next((x for x in d["reports"] if x["id"] == rm.get("id")), {})
    m = one.get("match") or {}
    check("admin list: the match summary (people, rule, arena, version) (%s)" % {k: m.get(k) for k in ("rule", "arena_mod", "version")},
          [p["name"] for p in m.get("people", [])] == ["JEFF HARDY", "RANDY ORTON"] and m.get("rule") == "4D"
          and m.get("arena_mod") == "arena_" + tag and m.get("version") == "2.1.6" and m.get("people")[0]["mod"])
    check("admin list: the mods, stored or not",
          [(x["hash"], x["stored"]) for x in one.get("mods", [])] == [(h1, True), (h2, False)])
    check("admin list: the mod store's use (%s)" % d.get("mod_store"), (d.get("mod_store") or {}).get("bytes", 0) >= len(sent1))
    st, body = get(url, "/api/admin/report-mod/%s?name=test" % h1)
    check("admin mod download: the bytes sent", st == 200 and body == sent1)
    st, body = get(url, "/api/admin/report/%s/match.zip" % rm.get("id"))
    check("admin match zip: the bytes sent", st == 200 and body == mz.getvalue())
    st, body = get(url, "/api/admin/report/%s/replay.zip" % rm.get("id"))
    names = zipfile.ZipFile(io.BytesIO(body)).namelist() if st == 200 else []
    check("replay pack: match folder, the stored mod, mods.txt, report.json (%d files)" % len(names),
          any(n.endswith("/match.json") and n.startswith("match/") for n in names) and
          sum(1 for n in names if n.startswith("mods/") and n.endswith(".svrmod")) == 1 and "mods.txt" in names and "report.json" in names)
    mod_file = os.path.join(a.node_data, "reports", "mods", h1 + ".svrmod")
    req = urllib.request.Request(url + "/api/admin/report/%s/delete" % rm.get("id"), method="POST")
    with urllib.request.urlopen(req, timeout=30) as f:
        pass
    check("delete the report: its mod (no other report has it) goes too", not os.path.exists(mod_file))

    # arena names (match_names.py: the live sessions' arena)
    import match_names
    check("arena names: stock, custom, unnamed and past the stock ones",
          match_names.arena_name("arena 0") == "SMACKDOWN" and match_names.arena_name("arena 11") == "WRESTLEMANIA XXVI"
          and match_names.arena_name("arena 9", "KING OF THE RING 1998") == "KING OF THE RING 1998"
          and match_names.arena_name("arena 33") == "arena 33" and match_names.arena_name("arena 120") == "mod arena 120")

    print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
