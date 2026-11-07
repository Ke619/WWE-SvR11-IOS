"""Checks an admin's new version of a Discovery mod (30 Oct, the bulk movie shrink): PUT /api/mods/{id}/file with
the storage node's key (X-Storage-Key), and the dashboard's PUT /api/admin/mods/{id}/file. The owner, author, id
and downloads stay; version / needs / size / file change; the log says "updated by admin (...)". Anyone else
still gets 401 / 403. Starts a private core + storage node on test ports with throwaway data folders.

    python test_mod_admin_file.py [--ports 8546]   (that port: the core; the next one: the node)
"""
import argparse
import hashlib
import io
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY = sys.executable
FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        FAILS.append(what)


def jpeg():
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (1024, 768), (40, 60, 90)).save(b, "JPEG", quality=70)
    return b.getvalue()


def svrmod(version, needs="", movie="movie.bik"):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.txt", "type=superstar\nname=Admin test\nversion=%s\nmovie=%s\n%s" % (
            version, movie, "needs=%s\n" % needs if needs else ""))
        z.writestr(movie, os.urandom(64 << 10))
    return out.getvalue()


def request(url, method="GET", data=None, headers=None):
    r = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(r, timeout=60) as f:
            return f.status, f.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def multipart(fields, files):
    boundary = "----ma" + secrets.token_hex(8)
    body = io.BytesIO()
    for n, v in fields.items():
        body.write(('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, n, v)).encode())
    for n, (filename, data, ctype) in files.items():
        body.write(('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\nContent-Type: %s\r\n\r\n' % (
            boundary, n, filename, ctype)).encode())
        body.write(data + b"\r\n")
    body.write(("--%s--\r\n" % boundary).encode())
    return body.getvalue(), "multipart/form-data; boundary=" + boundary


def put_file(url, mid, data, version, headers, path="/api/mods/%d/file", why=None):
    fields = {"version": version}
    if why:
        fields["why"] = why
    body, ctype = multipart(fields, {"svrmod": ("admin_test.svrmod", data, "application/zip")})
    st, out = request(url + path % mid, "PUT", body, dict(headers, **{"Content-Type": ctype}))
    try:
        return st, json.loads(out or b"{}")
    except ValueError:
        return st, {"raw": out[:200]}


def register(url, name):
    st, out = request(url + "/api/register", "POST", json.dumps({"name": name, "password": "pw-" + name}).encode(),
                      {"Content-Type": "application/json",
                       "X-Forwarded-For": "10.66.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))})
    return json.loads(out)


def wait_up(url, proc, path="/api/status", timeout=25):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            urllib.request.urlopen(url + path, timeout=1).read()
            return True
        except urllib.error.HTTPError:
            return True
        except Exception:
            time.sleep(0.3)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", type=int, default=8546)
    a = ap.parse_args()
    core_port, node_port = a.ports, a.ports + 1
    tmp = Path(tempfile.mkdtemp(prefix="svr_mod_admin_"))
    key = secrets.token_hex(32)
    (tmp / "key.txt").write_text(key)
    for d in ("core", "node"):
        (tmp / d).mkdir()
    procs = {}
    try:
        procs["node"] = subprocess.Popen(
            [PY, str(HERE / "server.py"), "--role", "storage", "--host", "127.0.0.1", "--port", str(node_port),
             "--data", str(tmp / "node"), "--storage-key", str(tmp / "key.txt"), "--log", str(tmp / "node.log")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs["core"] = subprocess.Popen(
            [PY, str(HERE / "server.py"), "--role", "core", "--test-open-accounts", "--host", "127.0.0.1",
             "--port", str(core_port), "--data", str(tmp / "core"), "--storage-url", "http://127.0.0.1:%d" % node_port,
             "--storage-key", str(tmp / "key.txt"), "--local-admin", "--log", str(tmp / "core.log")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        url = "http://127.0.0.1:%d" % core_port
        check(wait_up("http://127.0.0.1:%d" % node_port, procs["node"], "/__health") and wait_up(url, procs["core"]),
              "private core %d + node %d up" % (core_port, node_port))

        owner, other = register(url, "ma" + secrets.token_hex(3)), register(url, "mb" + secrets.token_hex(3))
        check(owner.get("token") and other.get("token"), "two accounts")
        img = jpeg()
        body, ctype = multipart({"name": "Admin test", "version": "1.0", "types": "superstar", "tags": "test"},
                                dict({"svrmod": ("admin_test.svrmod", svrmod("1.0"), "application/zip")},
                                     **{"img%d" % n: ("img%d.jpg" % n, img, "image/jpeg") for n in range(4)}))
        st, out = request(url + "/api/mods", "POST", body, {"Authorization": "Bearer " + owner["token"],
                                                            "Content-Type": ctype})
        up = json.loads(out)
        mid = up.get("id")
        check(st == 200 and mid, "the owner uploads mod %s (1.0)" % mid)
        before = json.loads(request(url + "/api/mods/%d" % mid)[1])
        request(url + "/api/mods/%d/file" % mid)  # (one download: the count must stay)
        before = json.loads(request(url + "/api/mods/%d" % mid)[1])

        new = svrmod("1.0.1", needs="2.1.6", movie="movie.mp4")
        st, _ = put_file(url, mid, new, "1.0.1", {})
        check(st == 401, "no sign-in: 401 (%d)" % st)
        st, _ = put_file(url, mid, new, "1.0.1", {"Authorization": "Bearer " + other["token"]})
        check(st == 403, "another account: 403 (%d)" % st)
        st, _ = put_file(url, mid, new, "1.0.1", {"X-Storage-Key": "0" * 64})
        check(st == 401, "a wrong storage key: 401 (%d)" % st)
        st, _ = put_file(url, mid, new, "1.0.1", {"X-Storage-Key": key, "X-Portal-Public": "1",
                                                  "X-Forwarded-For": "203.0.113.9"}, path="/api/admin/mods/%d/file")
        check(st == 403, "the admin route from outside (no key, no session): 403 (%d)" % st)
        st, _ = put_file(url, 999999, new, "1.0.1", {"X-Storage-Key": key})
        check(st == 404, "the key, no such mod: 404 (%d)" % st)
        st, out = put_file(url, mid, new, "1.0", {"X-Storage-Key": key})
        check(st == 400 and "new version" in out.get("error", ""), "the key, the same version: 400 (%d %s)" % (
            st, out.get("error")))

        st, out = put_file(url, mid, new, "1.0.1", {"X-Storage-Key": key, "X-Forwarded-For": "203.0.113.9"},
                           why="movie shrink")
        check(st == 200 and out.get("ok"), "the key (through the proxy): 200 (%d %s)" % (st, out.get("error")))
        after = json.loads(request(url + "/api/mods/%d" % mid)[1])
        same = {k: (before[k], after[k]) for k in ("id", "owner", "author", "name", "downloads", "created", "tags")
                if before[k] != after[k]}
        check(not same, "id / owner / author / name / downloads / created / tags kept %s" % (same or ""))
        check(after["version"] == "1.0.1" and after["needs"] == "2.1.6" and after["size"] == len(new) and
              after.get("updated"), "version 1.0.1, needs 2.1.6, the new size, updated set (%s %s %s)" % (
                  after["version"], after["needs"], after["size"]))
        st, got = request(url + "/api/mods/%d/file" % mid)
        check(st == 200 and hashlib.sha256(got).digest() == hashlib.sha256(new).digest(), "the download is the new file")
        log = (tmp / "core.log").read_text(encoding="utf-8", errors="replace")
        check("mod %d updated by admin (movie shrink) (file) 1.0 -> 1.0.1" % mid in log, "the log: updated by admin")
        mine = json.loads(request(url + "/api/mods/%d" % mid, headers={"Authorization": "Bearer " + owner["token"]})[1])
        check(mine.get("mine") is True, "still the owner's (mine: %s)" % mine.get("mine"))

        st, out = put_file(url, mid, svrmod("1.0.2", needs="2.1.6"), "1.0.2", {}, path="/api/admin/mods/%d/file")
        check(st == 200 and out.get("mod", {}).get("version") == "1.0.2",
              "the dashboard route (a local admin): 200 (%d %s)" % (st, out.get("error")))
        st, out = put_file(url, mid, svrmod("1.1"), "1.1", {"Authorization": "Bearer " + owner["token"]})
        after = json.loads(request(url + "/api/mods/%d" % mid)[1])
        check(st == 200 and after["version"] == "1.1" and after["owner"] == before["owner"],
              "the owner can still update it: 1.1 (%d)" % st)
        log = (tmp / "core.log").read_text(encoding="utf-8", errors="replace")
        check("updated by admin (admin) (file) 1.0.1 -> 1.0.2" in log and "updated by its owner (file) 1.0.2 -> 1.1"
              in log, "the log: admin (dashboard) and owner lines")
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.kill()
                p.wait(10)
    print("--- %s (%s)" % ("all ok" if not FAILS else "%d FAILED" % len(FAILS), tmp))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
