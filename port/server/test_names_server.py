"""Checks the name filter (namefilter.py) on a server of its own: a temporary data folder, the dashboard on loopback.

  sign-up names; Community Creations records (title, tags, comments; create and update) refused with the reason in
  X-Svr-Notice; an uploader whose account name is flagged; Discovery mods (upload, edit); entrance names; the
  dashboard's Names endpoints (list, switch, test, flagged names) - and nothing deleted on its own.

    python test_names_server.py [--port 8562] [--keep]
"""
import argparse
import io
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import namefilter  # noqa: E402

H = "hit" + "ler"  # (the seed's words are not written out here either)


def call(server, method, path, body=None, token=None, headers=None, raw=False):
    """(status, answer, response headers); a JSON body unless raw (bytes)."""
    h = dict(headers or {})
    if token:
        h["Authorization"] = "Bearer " + token
    if "/api/admin/" not in path:
        h.setdefault("X-Forwarded-For", "10.8.%d.%d" % tuple(os.urandom(2)))
    if body is not None and not raw:
        body = json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(server + path, data=body if body is not None else (b"" if method == "POST" else None),
                                 method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data, status, rh = r.read(), r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        data, status, rh = e.read(), e.code, dict(e.headers)
    try:
        return status, json.loads(data or b"{}"), rh
    except ValueError:
        return status, {"text": data.decode(errors="replace")}, rh


def multipart(parts):
    boundary = "----nf" + secrets.token_hex(8)
    out = io.BytesIO()
    for name, value, filename, ctype in parts:
        out.write(("--%s\r\nContent-Disposition: form-data; name=\"%s\"%s\r\nContent-Type: %s\r\n\r\n" % (
            boundary, name, '; filename="%s"' % filename if filename else "", ctype)).encode())
        out.write(value if isinstance(value, bytes) else value.encode())
        out.write(b"\r\n")
    out.write(("--%s--\r\n" % boundary).encode())
    return out.getvalue(), "multipart/form-data; boundary=" + boundary


def jpeg():
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (1024, 768), (40, 60, 90)).save(b, "JPEG", quality=60)
    return b.getvalue()


def svrmod():
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("manifest.txt", "type=superstar\nname=Name test\n")
        z.writestr("data.bin", os.urandom(2000))
    return out.getvalue()


def sake(server, token, func, values, recordid=None):
    """A Community Creations record (the game's SOAP): (result text, X-Svr-Notice)."""
    fields = "".join('<ns1:RecordField><ns1:name>%s</ns1:name><ns1:value><ns1:%s><ns1:value>%s</ns1:value></ns1:%s>'
                     '</ns1:value></ns1:RecordField>' % (k, t, v, t) for k, (t, v) in values.items())
    soap = ('<?xml version="1.0" encoding="UTF-8"?><SOAP-ENV:Envelope xmlns:SOAP-ENV='
            '"http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="http://gamespy.net/sake"><SOAP-ENV:Body>'
            '<ns1:%s><ns1:gameid>2886</ns1:gameid><ns1:tableid>UserContent</ns1:tableid>%s'
            '<ns1:values>%s</ns1:values></ns1:%s></SOAP-ENV:Body></SOAP-ENV:Envelope>' % (
                func, "<ns1:recordid>%d</ns1:recordid>" % recordid if recordid else "", fields, func))
    st, r, h = call(server, "POST", "/game/SakeStorageServer/StorageServer.asmx", soap.encode("utf-8"), token,
                    {"Content-Type": "text/xml", "SOAPAction": '"http://gamespy.net/sake/%s"' % func,
                     "X-Svr-ModFree": "1"}, raw=True)
    text = r.get("text", "")
    result = text.split("Result>")[1].split("<")[0] if "Result>" in text else "?"
    rid = text.split("<recordid>")[1].split("<")[0] if "<recordid>" in text else None
    return result, h.get("X-Svr-Notice"), rid


def upload_file(server, token):
    body, ctype = multipart([("file", os.urandom(3000), "f.bin", "application/octet-stream")])
    _, _, h = call(server, "POST", "/game/SakeFileServer/upload.aspx?gameid=2886&pid=1", body, token,
                   {"Content-Type": ctype}, raw=True)
    return int(h.get("Sake-File-Id") or 0)


def record(title, fid, **extra):
    v = {"Title": ("unicodeStringValue", title), "TitleKeys": ("unicodeStringValue", title),
         "UserKey0": ("unicodeStringValue", ""), "UserComment0": ("unicodeStringValue", ""),
         "ContentType": ("intValue", 0), "F00": ("intValue", fid), "FileSize": ("intValue", 3000),
         "AuthorName": ("unicodeStringValue", "0")}
    v.update({k: ("unicodeStringValue", s) for k, s in extra.items()})
    return v


def mod_upload(server, token, name, description=""):
    img = jpeg()
    body, ctype = multipart([("name", name, None, "text/plain"), ("version", "1.0", None, "text/plain"),
                             ("types", "superstar", None, "text/plain"), ("description", description, None, "text/plain"),
                             ("svrmod", svrmod(), "m.svrmod", "application/zip")] +
                            [("img%d" % n, img, "i%d.jpg" % n, "image/jpeg") for n in range(4)])
    st, r, _ = call(server, "POST", "/api/mods", body, token, {"Content-Type": ctype}, raw=True)
    return st, r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8562)  # (outside 8460-8499: the test tools' free-port range)
    ap.add_argument("--keep", action="store_true", help="leave the data folder")
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix="svr_names_")
    server = "http://127.0.0.1:%d" % args.port
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py"), "--port", str(args.port), "--data", data,
                             "--local-admin", "--test-open-accounts"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    print("test server pid %d on port %d, data %s" % (proc.pid, args.port, data))
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def names(body=None):
        return call(server, "POST" if body is not None else "GET", "/api/admin/names", body)[1]

    try:
        for _ in range(100):
            try:
                if call(server, "GET", "/api/status")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        check(os.path.exists(os.path.join(data, "name_filter.json")), "the list file made from the seed")
        st = names()
        check(st.get("enabled") and st.get("terms", 0) > 20, "the dashboard's list: on, %d words" % st.get("terms", 0))

        # -- sign-up --
        st, r, _ = call(server, "POST", "/api/register", {"name": "Adolf_" + H.title(), "password": "goodpass1"})
        check(st != 200 and "isn't allowed" in r.get("error", ""), "sign-up as a flagged name refused: %s" % r.get("error"))
        st, r, _ = call(server, "POST", "/api/register", {"name": "h1tl3r_fan", "password": "goodpass1"})
        check(st != 200, "leetspeak sign-up refused")
        st, r, _ = call(server, "POST", "/api/register", {"name": "Scunthorpe", "password": "goodpass1"})
        check(st == 200 and r.get("token"), "Scunthorpe signs up")
        good = r.get("token", "")

        # -- Community Creations records --
        fid = upload_file(server, good)
        check(fid > 0, "a file up (%d)" % fid)
        res, note, rid = sake(server, good, "CreateRecord", record("Gambit", fid))
        check(res == "Success" and not note and rid, "a record titled Gambit: %s" % res)
        fid2 = upload_file(server, good)
        res, note, _ = sake(server, good, "CreateRecord", record("x" + H.upper() + "x", fid2))
        check(res == "NoPermission" and note == namefilter.REASON, "a flagged title refused, the reason: %r" % note)
        res, note, _ = sake(server, good, "CreateRecord", record("Nice CAW", fid2, UserKey0="h i t l e r"))
        check(res == "NoPermission" and note, "a flagged tag refused")
        res, note, _ = sake(server, good, "CreateRecord", record("Nice CAW", fid2, UserComment1="xx" + H + "s"))
        check(res == "NoPermission" and note, "a flagged comment refused")
        res, note, _ = sake(server, good, "UpdateRecord", {"Title": ("unicodeStringValue", "hiiitler")}, int(rid))
        check(res == "NoPermission" and note, "an update to a flagged title refused")
        res, note, _ = sake(server, good, "UpdateRecord", {"Title": ("unicodeStringValue", "Gambit 2")}, int(rid))
        check(res == "Success" and not note, "a good update")

        # -- Discovery --
        st, r = mod_upload(server, good, "The " + H + " mod")
        check(st == 400 and r.get("error") == namefilter.REASON, "a Discovery mod with a flagged name refused")
        st, r = mod_upload(server, good, "Clean mod", "desc " + H)
        check(st == 400, "a Discovery mod with a flagged description refused")
        st, r = mod_upload(server, good, "Clean mod")
        mid = r.get("id")
        check(st == 200 and mid, "a clean Discovery mod (%s)" % mid)
        st, r, _ = call(server, "PATCH", "/api/mods/%s" % mid, {"tags": "cool, " + H}, good)
        check(st == 400 and r.get("error") == namefilter.REASON, "a Discovery edit with a flagged tag refused")
        st, r, _ = call(server, "PATCH", "/api/mods/%s" % mid, {"tags": "cool, wrestling"}, good)
        check(st == 200, "a good Discovery edit")

        # -- entrances --
        ent = {"music": {"playlist": H + " songs", "file": "song.mp3", "sha": "0" * 64}}
        st, r, h = call(server, "PUT", "/api/entrance/%d" % fid, ent, good)
        check(st == 400 and h.get("X-Svr-Notice") == namefilter.REASON, "a flagged entrance playlist refused")
        ent = {"movie": {"name": "H.I.T.L.E.R.bik", "sha": "0" * 64}}
        st, r, h = call(server, "PUT", "/api/entrance/%d" % fid, ent, good)
        check(st == 400 and h.get("X-Svr-Notice"), "a flagged entrance movie name refused")

        # -- the switch; an account name flagged after it was made --
        names({"enabled": False})
        check(not names().get("enabled"), "switched off")
        st, r, _ = call(server, "POST", "/api/register", {"name": "MrHitlerX", "password": "goodpass1"})
        check(st == 200, "off: a flagged sign-up lets through")
        bad = r.get("token", "")
        fid3 = upload_file(server, bad)
        res, note, rid_bad = sake(server, bad, "CreateRecord", record("Some " + H + " CAW", fid3))
        check(res == "Success", "off: a flagged title lets through")
        st, r = mod_upload(server, good, H + " arena")
        bad_mid = r.get("id")
        check(st == 200, "off: a flagged Discovery mod lets through")
        names({"enabled": True})
        fid4 = upload_file(server, bad)
        res, note, _ = sake(server, bad, "CreateRecord", record("Gambit", fid4))
        check(res == "NoPermission" and note == namefilter.ACCOUNT_REASON,
              "on: a flagged account's clean upload refused - %r" % note)

        # -- the test box --
        st, r, _ = call(server, "POST", "/api/admin/names/test", {"text": "H!TL3R"})
        check(not r.get("passes") and r.get("term"), "test box: H!TL3R refused (%s; read as %s)" % (r.get("term"), r.get("readings")))
        st, r, _ = call(server, "POST", "/api/admin/names/test", {"text": "Raccoon City"})
        check(r.get("passes"), "test box: Raccoon City passes")

        # -- flagged names: what's on the server; nothing deleted --
        uploads_before = len(call(server, "GET", "/api/admin/uploads")[1])
        fl = call(server, "GET", "/api/admin/names/flagged")[1].get("flagged", [])
        kinds = sorted({(f["kind"], f["id"]) for f in fl}, key=str)
        check(("upload", int(rid_bad)) in kinds, "flagged: the upload (%s)" % kinds)
        check(("mod", bad_mid) in kinds, "flagged: the Discovery mod")
        check(any(f["kind"] == "account" and f["text"] == "MrHitlerX" for f in fl), "flagged: the account")
        check(not any(f["kind"] == "upload" and f["id"] == int(rid) for f in fl), "not flagged: Gambit 2")
        check(len(call(server, "GET", "/api/admin/uploads")[1]) == uploads_before, "the scan deleted nothing")
        st, r, _ = call(server, "POST", "/api/admin/upload/%s/delete" % rid_bad)
        fl = call(server, "GET", "/api/admin/names/flagged")[1].get("flagged", [])
        check(st == 200 and not any(f["kind"] == "upload" for f in fl), "the dashboard's Delete: gone from the list")

        # -- the list edited from the dashboard --
        st = names({"words": names()["words"] + ["*zzbad", "=qux"], "allow": names()["allow"] + ["zzbadge"]})
        st, r, _ = call(server, "POST", "/api/register", {"name": "my_zzbad", "password": "goodpass1"})
        check(st != 200, "a word added on the dashboard is used at once")
        st, r, _ = call(server, "POST", "/api/register", {"name": "zzbadge", "password": "goodpass1"})
        check(st == 200, "an allow-list word passes")
        saved = json.load(open(os.path.join(data, "name_filter.json"), encoding="utf-8"))
        check("*zzbad" in saved["words"] and "zzbadge" in saved["allow"] and saved["enabled"], "the list file saved")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("test server pid %d stopped" % proc.pid)
        if not args.keep:
            shutil.rmtree(data, ignore_errors=True)
    print("ALL OK" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
