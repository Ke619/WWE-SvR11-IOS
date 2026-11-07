// WWE SmackDown vs. Raw 2011 - the reports the phone sent and what became of
// them, as the PC's (launcher/report_receipts.c): a send's receipt (the
// report's id and secret) kept in <game>/UserData/reports.json - one report a
// line, the same file the PC launcher and Mod Maker keep - then their state
// asked from the server (POST /api/reports/status), an answer to our
// question sent (POST /api/reports/reply) and the player's changes to a report
// (POST /api/reports/edit: text to add, pictures - 5 a report in all).
// Suggestions (SuggestionsList: reports of kind "suggestion") keep theirs in
// UserData/suggestions.json (kSuggestions), as the PC's Suggestions tab. Plain
// Java (no android.*): it runs on the PC too, against a test server.
package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class Receipts {
    static final int kMax = 200, kMaxImages = 3, kMaxEditImages = 5;
    static final String kReports = "reports.json", kSuggestions = "suggestions.json";

    static final class Item {
        // kept on the phone
        String id = "", secret = "", date = "", app = "", problem = "", what = "";
        boolean picturesLater;  // the game's form sent it without pictures, "add them later"
        // the server's answer ("" until asked)
        String state = "", note = "", version = "", question = "";
        int replies;
        int edits, pictures;  // the player's changes so far; the pictures it has (sent + added)
        boolean fromAccount;  // sent from another device, listed by the signed-in account
        String kind = "report";  // "suggestion": its words are Sure / No, not planned / Done in
    }

    static File file(File gameFolder) {
        return file(gameFolder, kReports);
    }

    static File file(File gameFolder, String name) {
        File d = new File(gameFolder, "UserData");
        d.mkdirs();
        return new File(d, name);
    }

    // Keeps a sent report's receipt (the oldest go past kMax).
    static boolean add(File gameFolder, String app, String id, String secret, String problem, String what) {
        return add(gameFolder, app, id, secret, problem, what, false);
    }

    // picturesLater: the player adds pictures later from the launcher (the game's own form) - "pictures_later":1 on
    // its line (the PC's report_receipts.c receipt_add_ex; the Reports tab offers an edit with pictures).
    static boolean add(File gameFolder, String app, String id, String secret, String problem, String what,
                       boolean picturesLater) {
        return add(kReports, gameFolder, app, id, secret, problem, what, picturesLater);
    }

    // ... into one of the files (kReports / kSuggestions).
    static synchronized boolean add(String name, File gameFolder, String app, String id, String secret, String problem,
                                    String what, boolean picturesLater) {
        if (id == null || id.isEmpty() || secret == null || secret.isEmpty()) return false;
        List<String> kept = new ArrayList<>();
        File f = file(gameFolder, name);
        if (f.isFile()) {
            try {
                String text = new String(ProblemReport.read(f, 8 << 20), StandardCharsets.UTF_8);
                for (int[] o : objects(text, 0)) kept.add(text.substring(o[0], o[1]));
            } catch (IOException ignored) {
            }
        }
        while (kept.size() >= kMax) kept.remove(0);
        String w = what == null ? "" : what;
        if (w.length() > 160) w = w.substring(0, 160);
        StringBuilder o = new StringBuilder("{");
        ReportSend.str(o, "id", id);
        ReportSend.str(o, "secret", secret);
        ReportSend.str(o, "date", new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(new Date()));
        ReportSend.str(o, "app", app);
        ReportSend.str(o, "problem", problem);
        ReportSend.str(o, "what", w);
        if (picturesLater) o.append(",\"pictures_later\":1");
        kept.add(o.append('}').toString());
        StringBuilder out = new StringBuilder("[\n");
        for (int i = 0; i < kept.size(); i++) out.append(kept.get(i)).append(i + 1 < kept.size() ? ",\n" : "\n");
        out.append("]\n");
        File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream s = new FileOutputStream(tmp)) {
            s.write(out.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(f) || (f.delete() && tmp.renameTo(f));
    }

    // The kept receipts, newest first.
    static List<Item> load(File gameFolder) {
        return load(gameFolder, kReports);
    }

    static List<Item> load(File gameFolder, String name) {
        List<Item> out = new ArrayList<>();
        File f = file(gameFolder, name);
        if (!f.isFile()) return out;
        try {
            String text = new String(ProblemReport.read(f, 8 << 20), StandardCharsets.UTF_8);
            for (int[] o : objects(text, 0)) {
                String obj = text.substring(o[0], o[1]);
                Item it = new Item();
                it.id = field(obj, "id");
                it.secret = field(obj, "secret");
                it.date = field(obj, "date");
                it.app = field(obj, "app");
                it.problem = field(obj, "problem");
                it.what = field(obj, "what");
                if (name.equals(kSuggestions)) it.kind = "suggestion";
                String later = field(obj, "pictures_later");
                it.picturesLater = later.equals("1") || later.equals("true");
                if (!it.id.isEmpty() && !it.secret.isEmpty()) out.add(it);
            }
        } catch (IOException ignored) {
        }
        Collections.reverse(out);
        return out;
    }

    // Asks the server for their state (token: the signed-in account's, or "":
    // its reports from other devices are added after). Returns why it couldn't
    // ask, or null when it did.
    static String status(String server, String token, List<Item> items) {
        return status(server, token, items, "report");
    }

    // ... kind: the account's ones from other devices of this kind only ("report" / "suggestion").
    static String status(String server, String token, List<Item> items, String kind) {
        if (server == null || server.isEmpty()) return "No online server is set.";
        String refused = ReportSend.refuses(server);
        if (refused != null) return refused;
        StringBuilder b = new StringBuilder("{\"reports\":[");
        boolean any = false;
        for (Item it : items) {
            if (it.fromAccount || it.secret.isEmpty()) continue;
            StringBuilder o = new StringBuilder("{");
            ReportSend.str(o, "id", it.id);
            ReportSend.str(o, "secret", it.secret);
            b.append(any ? "," : "").append(o.append('}'));
            any = true;
        }
        b.append("]}");
        String[] answer = new String[1];
        int status = post(server, "/api/reports/status", "application/json; charset=utf-8",
            b.toString().getBytes(StandardCharsets.UTF_8), token, answer);
        if (status != 200) return why(status, answer[0]);
        String a = answer[0];
        int at = a.indexOf("\"reports\"");
        if (at < 0) return "The server's answer has no list.";
        for (Item it : items) it.state = "";
        for (int[] o : objects(a, at)) {
            String obj = a.substring(o[0], o[1]);
            String id = field(obj, "id"), k = field(obj, "kind");
            if (k.isEmpty()) k = "report";
            Item it = null;
            for (Item x : items) if (x.id.equals(id)) it = x;
            if (it == null && field(obj, "from_account").equals("true") && items.size() < kMax && k.equals(kind)) {
                it = new Item();
                it.id = id;
                it.fromAccount = true;
                it.date = field(obj, "received");
                if (it.date.length() > 16) it.date = it.date.substring(0, 16);
                it.app = field(obj, "app");
                it.problem = field(obj, "problem");
                it.what = field(obj, "what");
                items.add(it);
            }
            if (it == null) continue;
            it.kind = k;
            it.state = field(obj, "state");
            it.note = field(obj, "note");
            it.version = field(obj, "version");
            it.question = field(obj, "question");
            it.replies = number(field(obj, "replies"));
            it.edits = number(field(obj, "edits"));
            it.pictures = number(field(obj, "pictures"));
        }
        return null;
    }

    static int number(String s) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // True while the report can be changed: not done, a duplicate or unknown to the server.
    static boolean editable(Item it) {
        return !it.state.isEmpty() && !it.state.equals("done") && !it.state.equals("duplicate")
            && !it.state.equals("unknown");
    }

    // True while it waits for pictures: sent from the game with "add pictures later", none added yet.
    static boolean wantsPictures(Item it) {
        return it.picturesLater && it.pictures == 0 && editable(it);
    }

    // The player's answer to our question (text, up to 3 pictures). Returns why
    // it wasn't taken, or null when it was.
    static String reply(String server, String token, Item it, String text, List<File> images) {
        StringBuilder j = new StringBuilder("{");
        ReportSend.str(j, "id", it.id);
        ReportSend.str(j, "secret", it.secret);
        ReportSend.str(j, "text", text);
        String[] answer = new String[1];
        return form(server, token, "/api/reports/reply", j.append('}').toString(), images, kMaxImages, answer);
    }

    // The player's change to a report it sent (until it's done or a duplicate):
    // text to add to what happened ("" allowed) and pictures - 5 a report in all,
    // the server counts the sent ones. Returns why it wasn't taken, or null when
    // it was (it.edits / it.pictures updated).
    static String edit(String server, String token, Item it, String what, List<File> images) {
        StringBuilder j = new StringBuilder("{");
        ReportSend.str(j, "id", it.id);
        ReportSend.str(j, "secret", it.secret);
        ReportSend.str(j, "what", what);
        String[] answer = new String[1];
        String why = form(server, token, "/api/reports/edit", j.append('}').toString(), images, kMaxEditImages, answer);
        if (why == null) {
            it.edits++;
            String n = ReportSend.jsonValue(answer[0], "pictures");
            if (!n.isEmpty()) it.pictures = number(n);
        }
        return why;
    }

    // A form POST (an answer, a change): the JSON part "report" and the pictures
    // image0.. (max of them). Returns why it wasn't taken, or null (the answer's
    // text in answer[0]).
    static String form(String server, String token, String path, String json, List<File> images, int max,
                       String[] answer) {
        answer[0] = "";
        if (server == null || server.isEmpty()) return "No online server is set.";
        String refused = ReportSend.refuses(server);
        if (refused != null) return refused;
        String bound = "----svr2011reply7f3a90";
        java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
        try {
            body.write(("--" + bound + "\r\nContent-Disposition: form-data; name=\"report\"\r\nContent-Type: "
                + "application/json; charset=utf-8\r\n\r\n" + json + "\r\n").getBytes(StandardCharsets.UTF_8));
            for (int i = 0; images != null && i < images.size() && i < max; i++) {
                File f = images.get(i);
                String no = ReportSend.imageRefused(f);
                if (no != null) return no;
                byte[] d = ProblemReport.read(f, 5 << 20);
                String type = ReportSend.imageType(d);
                body.write(("--" + bound + "\r\nContent-Disposition: form-data; name=\"image" + i + "\"; filename=\"image"
                    + i + "." + type.substring(6) + "\"\r\nContent-Type: " + type + "\r\n\r\n").getBytes(StandardCharsets.UTF_8));
                body.write(d);
                body.write(new byte[] {'\r', '\n'});
            }
            body.write(("--" + bound + "--\r\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return "A picture can't be read.";
        }
        int status = post(server, path, "multipart/form-data; boundary=" + bound, body.toByteArray(), token, answer);
        return status == 200 ? null : why(status, answer[0]);
    }

    // The player's words for its state: a short badge and the line under it.
    static String badge(Item it) {
        if (it.kind.equals("suggestion"))
            switch (it.state) {
                case "received": return "Received";
                case "planned": return "Sure";
                case "not_planned": return "No, not planned";
                case "fixed": return it.version.isEmpty() ? "Done" : "Done in " + it.version;
                default: break;
            }
        switch (it.state) {
            case "": return "…";
            case "received": return "Received";
            case "looking": return "Looking at it";
            case "fixed": return "Fixed";
            case "needs_info": return "Needs your answer";
            case "replied": return "Answered";
            case "done": return "Done";
            case "duplicate": return "Already reported";
            default: return "Not found";
        }
    }

    static String line(Item it) {
        String note = it.note.isEmpty() ? "." : ": " + it.note;
        if (it.kind.equals("suggestion"))
            switch (it.state) {
                case "received": return "We have your idea and will answer it here.";
                case "planned": return "Sure - we plan to add this" + note;
                case "not_planned": return "No, not planned" + note;
                case "fixed": return (it.version.isEmpty() ? "Done" : "Done in " + it.version) + note;
                default: break;
            }
        switch (it.state) {
            case "": return "Not checked yet.";
            case "received": return "We have your report and will look at it.";
            case "looking": return "We're looking at this one now.";
            case "fixed": return "Fixed - in the next update" + (it.version.isEmpty() ? "" : " (" + it.version + ")")
                + (it.note.isEmpty() ? "." : ": " + it.note);
            case "needs_info": return "We need more information: " + it.question;
            case "replied": return "Thanks - we have your answer to: " + it.question;
            case "done": return "We've dealt with this report.";
            case "duplicate": return "Someone reported the same problem first - we're on it there.";
            default: return "The server doesn't know this report any more.";
        }
    }

    // ── JSON and HTTP ────────────────────────────────────────────────────────

    // The {...} objects in text from `from` (strings skipped whole): [start, end) each.
    static List<int[]> objects(String text, int from) {
        List<int[]> out = new ArrayList<>();
        int depth = 0, start = -1;
        boolean inStr = false;
        for (int i = from; i < text.length(); i++) {
            char c = text.charAt(i);
            if (inStr) {
                if (c == '\\') i++;
                else if (c == '"') inStr = false;
            } else if (c == '"') {
                inStr = true;
            } else if (c == '{') {
                if (depth++ == 0) start = i;
            } else if (c == '}' && depth > 0 && --depth == 0 && start >= 0) {
                out.add(new int[] {start, i + 1});
            }
        }
        return out;
    }

    // A flat object's value for "key", unescaped ("" when there's none). A key
    // inside a string value is escaped there, so the quote before it tells a key.
    static String field(String obj, String key) {
        String k = "\"" + key + "\"";
        for (int at = obj.indexOf(k); at >= 0; at = obj.indexOf(k, at + 1)) {
            if (at > 0 && obj.charAt(at - 1) == '\\') continue;
            int p = at + k.length();
            while (p < obj.length() && obj.charAt(p) == ' ') p++;
            if (p >= obj.length() || obj.charAt(p) != ':') continue;
            p++;
            while (p < obj.length() && obj.charAt(p) == ' ') p++;
            StringBuilder out = new StringBuilder();
            if (p < obj.length() && obj.charAt(p) == '"') {
                for (p++; p < obj.length() && obj.charAt(p) != '"'; p++) {
                    char c = obj.charAt(p);
                    if (c != '\\' || p + 1 >= obj.length()) { out.append(c); continue; }
                    char e = obj.charAt(++p);
                    if (e == 'n') out.append('\n');
                    else if (e == 'r') out.append('\r');
                    else if (e == 't') out.append('\t');
                    else if (e == 'u' && p + 4 < obj.length()) {
                        out.append((char) Integer.parseInt(obj.substring(p + 1, p + 5), 16));
                        p += 4;
                    } else out.append(e);
                }
            } else {
                while (p < obj.length() && ",} \r\n".indexOf(obj.charAt(p)) < 0) out.append(obj.charAt(p++));
            }
            return out.toString();
        }
        return "";
    }

    static String why(int status, String answer) {
        if (status == 0) return "The server can't be reached (offline, or it's down).";
        String e = ReportSend.jsonValue(answer, "error");
        return e.isEmpty() ? "The server said no (HTTP " + status + ")." : e;
    }

    // One POST: its HTTP status (0: no answer), the answer's text in answer[0].
    static int post(String server, String path, String type, byte[] body, String token, String[] answer) {
        answer[0] = "";
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(ReportSend.base(server) + path).openConnection();
            try {
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                c.setRequestProperty("Content-Type", type);
                if (token != null && !token.isEmpty()) c.setRequestProperty("Authorization", "Bearer " + token);
                try (OutputStream o = c.getOutputStream()) {
                    o.write(body);
                }
                int status = c.getResponseCode();
                java.io.InputStream in = status < 400 ? c.getInputStream() : c.getErrorStream();
                answer[0] = in == null ? "" : ReportSend.readAll(in);
                return status;
            } finally {
                c.disconnect();
            }
        } catch (Exception e) {
            return 0;
        }
    }
}
