// WWE SmackDown vs. Raw 2011 - Report a problem's Send on the phone, as the PC
// launcher's (launcher/report_send.c, report_match.c): POST
// <online_server>/api/reports, multipart - report (report.json), zip (ProblemReport's),
// image0-4, match (a recorded match's folder, zipped) and mod0.. (the mods that
// match used that the server doesn't have yet: asked first by content hash,
// POST /api/reports/mods/check) - docs/REPORTS_API.md. Plain Java (no android.*):
// it runs on the PC too, against a test server.
package io.github.kaikoclanworth1.svr2011;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

final class ReportSend {
    static final int kMaxImages = 5, kMaxMods = 64, kMaxMatches = 20;

    // The test-run guard (as the PC launcher's test_run_refuses): in a test (the
    // launcher started with the svr2011_test extra, or the report_send test aid) a
    // report goes only to a server on this phone or its own network - 127.0.0.1
    // through adb reverse, or a PC on the LAN - never the live one, unless
    // allowLive (the svr2011_allow_live extra, as SVR2011_ALLOW_LIVE=1).
    static volatile boolean testRun, allowLive;
    // A test's report server (the svr2011_report_server extra, test runs only), or null.
    static volatile String testServer;

    // A server address -> its base URL (a bare host:port is plain http; as OnlinePage.base).
    static String base(String server) {
        String b = server.contains("://") ? server : "http://" + server;
        while (b.endsWith("/")) b = b.substring(0, b.length() - 1);
        return b;
    }

    // Why a test may not send to `server`, or null when it may.
    static String refuses(String server) {
        if (!testRun || allowLive) return null;
        String host;
        try {
            host = new URL(base(server)).getHost().toLowerCase(Locale.US);
        } catch (Exception e) {
            return "Test run: " + server + " isn't a web address.";
        }
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length() - 1);
        if (host.equals("127.0.0.1") || host.equals("localhost") || host.equals("::1") || host.startsWith("10.")
            || host.startsWith("192.168.")) return null;
        if (host.startsWith("172.")) {
            String[] p = host.split("\\.");
            try {
                int b = Integer.parseInt(p[1]);
                if (p.length == 4 && b >= 16 && b <= 31) return null;
            } catch (Exception ignored) {
            }
        }
        return "Test run: a report to " + host + " was refused - not a server on this phone or its network "
            + "(svr2011_allow_live lets it through).";
    }

    // ── report.json ──────────────────────────────────────────────────────────

    static final class Answers {
        String app = "android", version = "", os = "", problem = "", happens = "", what = "", before = "",
            contact = "", signature = "", updater = "";
    }

    static void str(StringBuilder b, String key, String v) {
        if (b.length() > 1) b.append(',');
        b.append('"').append(key).append("\":\"");
        String s = v == null ? "" : v;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c == '\t') b.append("\\t");
            else if (c < 0x20) b.append(String.format(Locale.US, "\\u%04x", (int) c));
            else b.append(c);
        }
        b.append('"');
    }

    // report.json: the answers, with the match folder's name and the mods
    // (sent: their zip goes with this attempt).
    static String json(Answers a, String matchName, List<Mod> mods, boolean withMods) {
        StringBuilder b = new StringBuilder("{");
        b.append("\"format\":1");
        str(b, "app", a.app);
        str(b, "version", a.version);
        str(b, "platform", "android");
        str(b, "os", a.os);
        str(b, "problem", a.problem);
        str(b, "happens", a.happens);
        str(b, "what", a.what);
        str(b, "before", a.before);
        str(b, "contact", a.contact);
        str(b, "signature", a.signature);
        if (a.updater != null && !a.updater.isEmpty()) str(b, "updater", a.updater);
        if (matchName != null) str(b, "match", matchName);
        if (mods != null && !mods.isEmpty()) {
            b.append(",\"mods\":[");
            for (int i = 0; i < mods.size(); i++) {
                Mod m = mods.get(i);
                StringBuilder o = new StringBuilder("{");
                str(o, "hash", m.hash);
                str(o, "name", m.name);
                str(o, "type", m.type);
                str(o, "id", m.id);
                str(o, "version", m.version);
                o.append(",\"size\":").append(m.size).append(",\"sent\":").append(withMods && m.zip != null).append('}');
                b.append(i > 0 ? "," : "").append(o);
            }
            b.append(']');
        }
        return b.append('}').toString();
    }

    // The hint for the server's duplicate check (as report_signature): this
    // session's crash report's first line, else the newest game log's last
    // "[error]" line that isn't one every session logs.
    static final String[] kHarmless = {"BaseHeap::Alloc", "PhysicalHeap::Alloc", "ResolvePath(", "NtCreateFile",
        "XMA: Write to unknown register", "VFS: '", "IoDismountVolume", "Stub", "STUB", "SeasonVO", "catalog_hd.arc",
        "gamecontrollerdb"};

    static String signature(File gameFolder) {
        File log = newest(new File(gameFolder, "logs"), "svr2011_", ".log");
        File crash = newest(new File(gameFolder, "UserData/crashes"), "crash_", ".txt");
        try {
            if (crash != null && crash.length() > 0 && (log == null || crash.lastModified() >= created(log))) {
                String t = new String(ProblemReport.read(crash, 1 << 20), StandardCharsets.UTF_8);
                for (String line : t.split("\n")) if (!line.trim().isEmpty()) return cap(line.trim());
            }
            if (log == null) return "";
            String hit = "";
            for (String line : new String(ProblemReport.read(log, 16 << 20), StandardCharsets.ISO_8859_1).split("\n")) {
                int at = line.indexOf("[error]");
                if (at < 0) continue;
                boolean skip = false;
                for (String h : kHarmless) skip |= line.contains(h);
                if (skip) continue;
                int end = line.indexOf(']', at);
                hit = line.substring(end + 1).trim();
            }
            return cap(new String(hit.getBytes(StandardCharsets.ISO_8859_1), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return "";
        }
    }

    static String cap(String s) { return s.length() > 300 ? s.substring(0, 300) : s; }

    static long created(File f) {
        try {
            return java.nio.file.Files.readAttributes(f.toPath(), java.nio.file.attribute.BasicFileAttributes.class)
                .creationTime().toMillis();
        } catch (Exception e) {
            return 0;
        }
    }

    static File newest(File dir, String prefix, String suffix) {
        File[] files = dir.listFiles((d, n) -> n.startsWith(prefix) && n.endsWith(suffix));
        File best = null;
        if (files != null)
            for (File f : files) if (f.isFile() && (best == null || f.lastModified() > best.lastModified())) best = f;
        return best;
    }

    // ── the recorded matches and their mods ──────────────────────────────────

    // <game>/logs/matches/<start>_<people>/ folders, newest first (their names start with the match's time).
    static List<File> matches(File gameFolder) {
        File[] dirs = new File(gameFolder, "logs/matches").listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        List<File> out = new ArrayList<>();
        if (dirs == null) return out;
        Arrays.sort(dirs, (x, y) -> {
            int c = y.getName().compareTo(x.getName());
            return c != 0 ? c : Long.compare(y.lastModified(), x.lastModified());
        });
        for (File d : dirs) {
            if (out.size() >= kMaxMatches) break;
            out.add(d);
        }
        return out;
    }

    static final class Mod {
        String type = "", id = "", name = "", version = "", hash = "";
        File folder;        // <game>/Mods/<type>/<id>; null when it isn't installed any more
        long size;          // its files' bytes
        boolean onServer;   // the check said the server has it
        File zip;           // made for this send (the ones to upload)
    }

    // The mods a match used (its mods.txt: "<Type>\t<id>\t<version>\t<on|off>\t<used|->\t<fnv64>\t<name>" a
    // line; a line of the first kind, "<Type>\t<id>\t<version>\t<name>", counts as used), each hashed.
    static List<Mod> matchMods(File gameFolder, File match) {
        List<Mod> out = new ArrayList<>();
        File txt = new File(match, "mods.txt");
        if (!txt.isFile()) return out;
        String text;
        try {
            text = new String(ProblemReport.read(txt, 1 << 20), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return out;
        }
        for (String raw : text.split("\n")) {
            String line = raw.replace("\r", "");
            if (line.isEmpty() || line.startsWith("#") || out.size() >= kMaxMods) continue;
            String[] c = line.split("\t", -1);
            if (c.length < 3 || (c.length >= 7 && !c[4].equals("used"))) continue;
            Mod m = new Mod();
            m.type = c[0];
            m.id = c[1];
            m.version = c[2];
            m.name = c.length >= 7 ? c[6] : c.length >= 4 ? c[3] : c[1];
            if (m.type.isEmpty() || m.id.isEmpty() || m.type.contains("/") || m.type.contains("\\") || m.id.contains("/")
                || m.id.contains("\\") || m.id.contains("..")) continue;  // (a name, not a path)
            File folder = new File(gameFolder, "Mods/" + m.type + "/" + m.id);
            if (folder.isDirectory()) {
                long[] size = new long[1];
                String h = modHash(folder, size);
                if (h != null) {
                    m.folder = folder;
                    m.hash = h;
                    m.size = size[0];
                }
            }
            out.add(m);
        }
        return out;
    }

    // A folder's files (recursively), no file or folder starting with '.' and no
    // "disabled" file, sorted by their lower-cased relative path's UTF-8 bytes.
    static final class Entry {
        String rel;
        byte[] key;
        File file;
    }

    static List<Entry> files(File dir) {
        List<Entry> out = new ArrayList<>();
        walk(dir, "", out);
        out.sort((a, b) -> {
            for (int i = 0; i < Math.min(a.key.length, b.key.length); i++) {
                int c = (a.key[i] & 0xFF) - (b.key[i] & 0xFF);
                if (c != 0) return c;
            }
            return a.key.length - b.key.length;
        });
        return out;
    }

    static void walk(File dir, String rel, List<Entry> out) {
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            String n = f.getName();
            if (n.startsWith(".") || n.equalsIgnoreCase("disabled")) continue;
            String r = rel.isEmpty() ? n : rel + "/" + n;
            if (f.isDirectory()) {
                if (!java.nio.file.Files.isSymbolicLink(f.toPath())) walk(f, r, out);
                continue;
            }
            Entry e = new Entry();
            e.rel = r;
            e.file = f;
            byte[] k = r.getBytes(StandardCharsets.UTF_8);
            for (int i = 0; i < k.length; i++) if (k[i] >= 'A' && k[i] <= 'Z') k[i] += 32;  // (ASCII letters only)
            e.key = k;
            out.add(e);
        }
    }

    // The mod's content hash (as report_mod_hash and the server's): SHA-256 over
    // each file as its key, a 0 byte, its size (8 bytes little-endian) and its
    // bytes. null when it can't be read.
    static String modHash(File folder, long[] size) {
        try {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 20];
            size[0] = 0;
            List<Entry> files = files(folder);
            if (files.isEmpty()) return null;
            for (Entry e : files) {
                long n = e.file.length();
                sha.update(e.key);
                sha.update((byte) 0);
                for (int k = 0; k < 8; k++) sha.update((byte) (n >>> (8 * k)));
                try (InputStream in = new FileInputStream(e.file)) {
                    int r;
                    while ((r = in.read(buf)) > 0) sha.update(buf, 0, r);
                }
                size[0] += n;
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest()) hex.append(String.format(Locale.US, "%02x", b & 0xFF));
            return hex.toString();
        } catch (Exception e) {
            return null;
        }
    }

    // Every file under dir (the hash's files) into a zip at `zip`, paths relative to dir.
    static boolean zipDir(File dir, File zip) {
        byte[] buf = new byte[1 << 16];
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(zip))) {
            List<Entry> files = files(dir);
            if (files.isEmpty()) return false;
            for (Entry e : files) {
                z.putNextEntry(new ZipEntry(e.rel));
                try (InputStream in = new FileInputStream(e.file)) {
                    int r;
                    while ((r = in.read(buf)) > 0) z.write(buf, 0, r);
                }
                z.closeEntry();
            }
            return true;
        } catch (IOException e) {
            zip.delete();
            return false;
        }
    }

    // Asks the server which of the mods it lacks: sets onServer. Returns why it
    // couldn't ask (the mods then all count as missing), "" when it could, or
    // "off" when the server takes no mods right now.
    static String modsCheck(String server, List<Mod> mods) {
        StringBuilder b = new StringBuilder("{\"mods\":[");
        boolean any = false;
        for (Mod m : mods) {
            m.onServer = false;
            if (m.hash.isEmpty()) continue;
            StringBuilder o = new StringBuilder("{");
            str(o, "hash", m.hash);
            o.append(",\"size\":").append(m.size);
            str(o, "name", m.name);
            str(o, "type", m.type);
            str(o, "id", m.id);
            str(o, "version", m.version);
            b.append(any ? "," : "").append(o.append('}'));
            any = true;
        }
        if (!any) return "";
        b.append("]}");
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(base(server) + "/api/reports/mods/check")
                .openConnection();
            try {
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json; charset=utf-8");
                try (OutputStream o = c.getOutputStream()) {
                    o.write(b.toString().getBytes(StandardCharsets.UTF_8));
                }
                int status = c.getResponseCode();
                if (status != 200) return "The server didn't answer the mod check (HTTP " + status + ").";
                String answer = readAll(c.getInputStream());
                if (answer.replace(" ", "").contains("\"mods_off\":true")) return "off";
                int at = answer.indexOf("\"missing\"");
                int end = at < 0 ? -1 : answer.indexOf(']', at);
                if (at < 0) return "The server's mod check answer has no list.";
                String missing = answer.substring(at, end < 0 ? answer.length() : end);
                for (Mod m : mods) m.onServer = !m.hash.isEmpty() && !missing.contains(m.hash);
                return "";
            } finally {
                c.disconnect();
            }
        } catch (Exception e) {
            return "The server can't be reached.";
        }
    }

    // ── sending ──────────────────────────────────────────────────────────────

    interface Progress { void at(long done, long total); }

    static final class Upload {
        Answers answers;
        File zip;                              // ProblemReport's (null: none)
        List<File> images = new ArrayList<>(); // PNG / JPEG / WebP, up to kMaxImages
        File match;                            // the match folder (null: none)
        List<Mod> mods = new ArrayList<>();    // the match's mods
        boolean withMods;                      // the tick: upload the ones the server lacks
        Progress progress;
    }

    static final class Result {
        boolean ok;
        int status;                  // HTTP status, 0: no answer
        String id = "", why = "", secret = "";
        int seenBefore, modsSent, modsStored;
        boolean matchStored, modsDropped;
        String modsWhy = "";
    }

    static String imageType(byte[] d) {
        if (d.length >= 8 && d[0] == (byte) 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
        if (d.length >= 3 && d[0] == (byte) 0xFF && d[1] == (byte) 0xD8 && d[2] == (byte) 0xFF) return "image/jpeg";
        if (d.length >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' && d[9] == 'E'
            && d[10] == 'B' && d[11] == 'P') return "image/webp";
        return null;
    }

    // Why a picture can't go (null: it can).
    static String imageRefused(File f) {
        if (!f.isFile() || f.length() > (5 << 20)) return f.getName() + " can't be added: it can't be read, or it's over 5 MB.";
        try (InputStream in = new FileInputStream(f)) {
            byte[] head = new byte[16];
            int off = 0, k;
            while (off < 16 && (k = in.read(head, off, 16 - off)) > 0) off += k;
            return imageType(Arrays.copyOf(head, off)) == null ? f.getName() + " isn't a PNG, JPEG or WebP picture." : null;
        } catch (IOException e) {
            return f.getName() + " can't be read.";
        }
    }

    // A piece of the body: bytes, or a whole file.
    static final class Piece {
        byte[] mem;
        File file;
        long size() { return mem != null ? mem.length : file.length(); }
    }

    static final String kBound = "----svr2011report5d2e81";

    static void head(List<Piece> body, String name, String file, String type) {
        Piece p = new Piece();
        p.mem = ("--" + kBound + "\r\nContent-Disposition: form-data; name=\"" + name + "\""
            + (file != null ? "; filename=\"" + file + "\"" : "") + "\r\nContent-Type: " + type + "\r\n\r\n")
            .getBytes(StandardCharsets.UTF_8);
        body.add(p);
    }

    static void mem(List<Piece> body, byte[] b) {
        Piece p = new Piece();
        p.mem = b;
        body.add(p);
    }

    static void file(List<Piece> body, File f) {
        Piece p = new Piece();
        p.file = f;
        body.add(p);
    }

    static final byte[] kCrLf = {'\r', '\n'};

    // One POST: the answer's status and text in r (status 0: no answer, r.why says why).
    static String postOnce(String server, Upload u, boolean withMods, Result r) {
        List<Piece> body = new ArrayList<>();
        head(body, "report", null, "application/json; charset=utf-8");
        mem(body, json(u.answers, u.match != null ? u.match.getName() : null, u.mods, withMods)
            .getBytes(StandardCharsets.UTF_8));
        mem(body, kCrLf);
        if (u.zip != null) {
            head(body, "zip", "report.zip", "application/zip");
            file(body, u.zip);
            mem(body, kCrLf);
        }
        for (int i = 0; i < u.images.size() && i < kMaxImages; i++) {
            File img = u.images.get(i);
            String why = imageRefused(img);
            if (why != null) {
                r.why = why;
                return null;
            }
            String type;
            try {
                type = imageType(ProblemReport.read(img, 5 << 20));
            } catch (IOException e) {
                type = null;
            }
            if (type == null) {
                r.why = img.getName() + " can't be read.";
                return null;
            }
            head(body, "image" + i, "image" + i + "." + type.substring(6), type);
            file(body, img);
            mem(body, kCrLf);
        }
        File matchZip = u.match != null ? new File(u.zip != null ? u.zip.getParentFile() : u.match.getParentFile(),
            "report-match.zip") : null;
        if (matchZip != null && zipDir(u.match, matchZip)) {
            head(body, "match", "match.zip", "application/zip");
            file(body, matchZip);
            mem(body, kCrLf);
        }
        r.modsSent = 0;
        for (Mod m : u.mods) {
            if (!withMods || m.zip == null) continue;
            head(body, "mod" + r.modsSent, m.hash + ".zip", "application/zip");
            file(body, m.zip);
            mem(body, kCrLf);
            r.modsSent++;
        }
        mem(body, ("--" + kBound + "--\r\n").getBytes(StandardCharsets.UTF_8));
        long total = 0, done = 0;
        for (Piece p : body) total += p.size();
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(base(server) + "/api/reports").openConnection();
            try {
                c.setConnectTimeout(10000);
                c.setReadTimeout(120000);
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(total);
                c.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + kBound);
                byte[] buf = new byte[1 << 16];
                if (u.progress != null) u.progress.at(0, total);
                try (OutputStream o = c.getOutputStream()) {
                    for (Piece p : body) {
                        if (p.mem != null) {
                            o.write(p.mem);
                            done += p.mem.length;
                            continue;
                        }
                        try (InputStream in = new FileInputStream(p.file)) {
                            long left = p.file.length();
                            while (left > 0) {
                                int k = in.read(buf, 0, (int) Math.min(buf.length, left));
                                if (k <= 0) throw new IOException(p.file.getName() + " changed while it was sent.");
                                o.write(buf, 0, k);
                                left -= k;
                                done += k;
                                if (u.progress != null) u.progress.at(done, total);
                            }
                        }
                    }
                } catch (IOException e) {
                    // (an early answer - 413 - can still be read below)
                    if (r.why.isEmpty()) r.why = "The upload stopped: " + e.getMessage();
                }
                if (u.progress != null) u.progress.at(total, total);
                r.status = c.getResponseCode();
                InputStream in = r.status < 400 ? c.getInputStream() : c.getErrorStream();
                return in == null ? "" : readAll(in);
            } finally {
                c.disconnect();
            }
        } catch (Exception e) {
            r.status = 0;
            r.why = "The server can't be reached (offline, or it's down).";
            return null;
        } finally {
            if (matchZip != null) matchZip.delete();
        }
    }

    // Sends the report: the mods first with it, and when the server refuses that
    // (too big, its mod store off) or the upload with them breaks, the report alone.
    static Result send(String server, Upload u) {
        Result r = new Result();
        if (server == null || server.isEmpty()) {
            r.why = "No server is set (online_server).";
            return r;
        }
        String refused = refuses(server);
        if (refused != null) {
            r.why = refused;
            return r;
        }
        boolean anyMods = false;
        for (Mod m : u.mods) anyMods |= u.withMods && m.zip != null;
        String answer = postOnce(server, u, u.withMods, r);
        if (anyMods && r.status != 200) {
            String err = jsonValue(answer, "error");
            r.modsWhy = r.status != 0 ? (err.isEmpty() ? "the server refused the report with the mods (HTTP " + r.status + ")"
                : "the server refused the mods: " + err) : "the upload with the mods didn't get through";
            r.why = "";
            answer = postOnce(server, u, false, r);
            r.modsDropped = true;
        }
        if (r.status == 200 && !(r.id = jsonValue(answer, "id")).isEmpty()) {
            r.ok = true;
            r.secret = jsonValue(answer, "secret");
            try {
                r.seenBefore = Integer.parseInt(jsonValue(answer, "seen_before"));
            } catch (NumberFormatException e) {
                r.seenBefore = 0;
            }
            r.matchStored = jsonValue(answer, "match").equals("true");
            r.modsStored = count(answer, "mods_stored");
            if (r.modsWhy.isEmpty()) {
                int at = answer.indexOf("\"mods_refused\"");
                int close = at < 0 ? -1 : answer.indexOf('}', at);
                int colon = at < 0 ? -1 : answer.indexOf(':', answer.indexOf(':', at) + 1);
                if (colon > 0 && (close < 0 || colon < close)) {
                    int q = answer.indexOf('"', colon), e = q < 0 ? -1 : answer.indexOf('"', q + 1);
                    if (q > 0 && e > q) r.modsWhy = answer.substring(q + 1, e);
                }
            }
        } else if (r.status == 0) {
            if (r.why.isEmpty()) r.why = "The server can't be reached (offline, or it's down).";
        } else {
            String err = jsonValue(answer, "error");
            r.why = err.isEmpty() ? "The server refused it (HTTP " + r.status + ")." : "The server refused it (" + r.status
                + "): " + err;
            r.id = "";
        }
        return r;
    }

    // A flat JSON answer's string or number value for "key" ("" when there's none).
    static String jsonValue(String j, String key) {
        if (j == null) return "";
        int at = j.indexOf("\"" + key + "\"");
        if (at < 0) return "";
        int p = at + key.length() + 2;
        while (p < j.length() && (j.charAt(p) == ' ' || j.charAt(p) == ':')) p++;
        StringBuilder out = new StringBuilder();
        if (p < j.length() && j.charAt(p) == '"') {
            for (p++; p < j.length() && j.charAt(p) != '"'; p++) {
                if (j.charAt(p) == '\\' && p + 1 < j.length()) p++;
                out.append(j.charAt(p));
            }
        } else {
            while (p < j.length() && ",} \n".indexOf(j.charAt(p)) < 0) out.append(j.charAt(p++));
        }
        return out.toString();
    }

    // How many 64-hex hashes the array "key" holds.
    static int count(String j, String key) {
        int at = j.indexOf("\"" + key + "\"");
        int open = at < 0 ? -1 : j.indexOf('[', at), end = open < 0 ? -1 : j.indexOf(']', open);
        if (end < 0) return 0;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"[0-9a-f]{64}\"").matcher(j.substring(open, end));
        int n = 0;
        while (m.find()) n++;
        return n;
    }

    static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        int k;
        while ((k = in.read(b)) > 0 && out.size() < (1 << 20)) out.write(b, 0, k);
        in.close();
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }
}
