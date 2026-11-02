// WWE SmackDown vs. Raw 2011 - "Report a problem" on the phone (as the PC
// launcher's, launcher/report.c): the zip ReportSend sends (made in the app's
// cache, gone once sent - reports are sent, not saved) with the newest game logs, crash reports and svr2011.toml (the account's token and
// password removed), plus report.txt (app version, phone, Android version),
// saves_check.txt (the Created Superstar saves' checksums, not the saves) and
// mods_text.txt (the superstar mods' text files) - port/docs/PROBLEM_REPORTS.md.

package io.github.kaikoclanworth1.svr2011;

import android.os.Build;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

final class ProblemReport {
    static final class Result {
        File zip;
        int logs, crashes;
    }

    // Makes the zip in `dir`, the player's description (ReportForm) at the top of
    // report.txt and (withSave: the form's "Include my main save" tick) the main
    // save as save/ (addMainSave); throws if it can't be written.
    static Result make(File dir, File gameFolder, String appVersion, String description, boolean withSave)
            throws IOException {
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        dir.mkdirs();
        Result r = new Result();
        r.zip = new File(dir, "SvR2011-report-" + stamp + ".zip");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(r.zip))) {
            String info = "SvR 2011 port problem report (Android)\n"
                + "app version: " + appVersion + "\n"
                + "phone: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")\n"
                + "Android: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "chip: " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : "?") + "\n"
                + "GPU: " + (GpuInfo.known().isEmpty() ? "?" : GpuInfo.known()) + "\n"
                + "made: " + stamp + "\n"
                + "contents: the newest game logs (logs/), crash reports (crashes/) and svr2011.toml "
                + "(account token and password removed)" + (withSave ? ", the main save (save/: SaveData.dat - "
                + "no Created Superstars or Paint Tool)" : "") + "; IP addresses blanked (x.x.x.x)\n";
            // (the player's words first: what a reader looks for)
            if (description != null && !description.isEmpty()) info = description + "\n" + info;
            put(zip, "report.txt", info.getBytes(StandardCharsets.UTF_8));
            r.logs = addLogs(zip, new File(gameFolder, "logs"), 16 << 20);
            addNewest(zip, new File(gameFolder, "logs"), "matchbox_", ".txt", 3, "logs/", 1 << 20);  // (the last matches' black boxes)
            r.crashes = addNewest(zip, new File(gameFolder, "UserData/crashes"), "crash_", ".txt", 3, "crashes/", 1 << 20);
            File toml = new File(gameFolder, "svr2011.toml");
            if (toml.isFile()) {
                StringBuilder out = new StringBuilder();
                for (String line : new String(read(toml, 1 << 20), StandardCharsets.UTF_8).split("\n", -1)) {
                    String t = line.trim();
                    if (t.startsWith("online_token")) out.append("online_token = \"(removed)\"\n");
                    else if (t.startsWith("online_password")) out.append("online_password = \"(removed)\"\n");
                    else out.append(line).append('\n');
                }
                put(zip, "svr2011.toml", out.toString().getBytes(StandardCharsets.UTF_8));
            }
            addSavesCheck(zip, gameFolder);
            addModsText(zip, gameFolder);
            if (withSave) addMainSave(zip, gameFolder);
        }
        return r;
    }

    // The player's main save, as the PC launcher's (launcher/report.c main_save): save/SaveData.dat (story, Universe
    // and Road to WrestleMania progress, options), save/.info/SaveData.dat.header (the name the game finds it by)
    // and save/.universe/* (the mods' Universe data) - not the Created Superstars (*.cas), Paint Tool, logos,
    // highlight reels, replays, the online cache or backups. tools/report_save.py puts it in a test user data.
    // Returns its bytes (zip null: only counts them - the form's tick text).
    static long addMainSave(ZipOutputStream zip, File gameFolder) throws IOException {
        File dir = savesDir(gameFolder);
        long total = 0;
        String[] names = {"SaveData.dat", ".info/SaveData.dat.header"};
        for (String n : names) {
            File f = new File(dir, n);
            if (!f.isFile()) continue;
            total += f.length();
            if (zip != null) put(zip, "save/" + n, read(f, 32 << 20));
        }
        File[] uni = new File(dir, ".universe").listFiles();
        if (uni != null)
            for (File f : uni) {
                if (!f.isFile()) continue;
                total += f.length();
                if (zip != null) put(zip, "save/.universe/" + f.getName(), read(f, 8 << 20));
            }
        return total;
    }

    // The saves folder: svr2011.toml's saves_folder (relative to the game folder), else <game>/Saves.
    static File savesDir(File gameFolder) {
        File toml = new File(gameFolder, "svr2011.toml");
        if (toml.isFile()) {
            try {
                for (String line : new String(read(toml, 1 << 20), StandardCharsets.UTF_8).split("\n")) {
                    String t = line.trim();
                    int q = t.indexOf('"'), e = t.lastIndexOf('"');
                    if (t.startsWith("saves_folder") && q >= 0 && e > q + 1) {
                        File f = new File(t.substring(q + 1, e));
                        return f.isAbsolute() ? f : new File(gameFolder, f.getPath());
                    }
                }
            } catch (IOException ignored) {
            }
        }
        return new File(gameFolder, "Saves");
    }

    static long be32(byte[] d, int at) {
        return ((d[at] & 0xFFL) << 24) | ((d[at + 1] & 0xFFL) << 16) | ((d[at + 2] & 0xFFL) << 8) | (d[at + 3] & 0xFFL);
    }

    // saves_check.txt: each Created Superstar save's size, slot, attires with a picture, and whether its three
    // checksums agree with each other and with SaveData.dat's record for its slot (as launcher/report.c).
    static void addSavesCheck(ZipOutputStream zip, File gameFolder) throws IOException {
        final int cas = 1347532, slotAt = 746340, slotSize = 149528, pic = 131072, rec = 0x1483C4;
        File dir = savesDir(gameFolder);
        StringBuilder t = new StringBuilder("Created Superstar saves in " + dir + "\n");
        File sdFile = new File(dir, "SaveData.dat");
        byte[] sd = sdFile.isFile() ? read(sdFile, 8 << 20) : null;
        if (sd != null && sd.length > 0x812DC) {
            long sum = 0;
            for (int i = 0x1C; i < 0x812DC; i++) sum += sd[i] & 0xFF;
            t.append("SaveData.dat: ").append(sd.length).append(" bytes, its checksum ")
                .append((sum & 0xFFFFFFFFL) == be32(sd, 0x18) ? "OK" : "DOESN'T MATCH").append('\n');
        } else {
            t.append("SaveData.dat: ").append(sd == null ? "none" : "too small").append('\n');
        }
        File[] files = dir.listFiles((d, n) -> n.endsWith("CreateSuperStar.cas"));
        if (files == null || files.length == 0) {
            t.append("(none)\n");
        } else {
            Arrays.sort(files);
            for (File f : files) {
                byte[] d = f.length() == cas ? read(f, cas) : null;
                if (d == null || d.length != cas) {
                    t.append(f.getName()).append(": ").append(f.length()).append(" bytes - NOT ").append(cas)
                        .append(", the game can't load it\n");
                    continue;
                }
                int slot = ((d[rec + 0x20] & 0xFF) << 8) | (d[rec + 0x21] & 0xFF);
                StringBuilder pics = new StringBuilder();
                for (int a = 0; a < 4; a++) {
                    boolean any = false;
                    for (int i = 0; i < pic && !any; i += 97) any = d[slotAt + a * slotSize + i] != 0;
                    if (any) pics.append(pics.length() > 0 ? "+" : "").append(a + 1);
                }
                long s1 = be32(d, 12), s2 = be32(d, 0x148FC8), s3 = be32(d, 0x148DBC), sv = 0;
                int r = 0x13FB8 + slot * 0x6BC + 0x65C;
                boolean inSd = sd != null && slot < 50 && r + 4 <= sd.length;
                if (inSd) sv = be32(sd, r);
                boolean agree = s1 == s2 && s1 == s3 && (!inSd || sv == s1);
                t.append(String.format(Locale.US,
                    "%s: slot %d, attires with a picture %s, checksums %08X %08X %08X, SaveData.dat %s%08X - %s\n",
                    f.getName(), slot, pics.length() > 0 ? pics : "none", s1, s2, s3, inSd ? "" : "(none) ", sv,
                    agree ? "agree" : "DON'T AGREE (the game will call it damaged)"));
            }
        }
        put(zip, "saves_check.txt", t.toString().getBytes(StandardCharsets.UTF_8));
    }

    // mods_text.txt: each superstar mod's manifest.txt, moves.txt and threads.txt (64 KB each, 512 KB in all).
    static void addModsText(ZipOutputStream zip, File gameFolder) throws IOException {
        File[] mods = new File(gameFolder, "Mods/Superstars").listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (mods == null || mods.length == 0) return;
        Arrays.sort(mods);
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        for (File m : mods) {
            for (String n : new String[] {"manifest.txt", "moves.txt", "threads.txt"}) {
                File f = new File(m, n);
                if (!f.isFile() || out.size() >= (512 << 10)) continue;
                byte[] b = read(f, 64 << 10);
                out.write(("==== " + m.getName() + "/" + n + " (" + b.length + " bytes)\n").getBytes(StandardCharsets.UTF_8));
                out.write(b);
                out.write('\n');
            }
        }
        if (out.size() > 0) put(zip, "mods_text.txt", out.toByteArray());
    }

    static void put(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    // A file's bytes, or its last `max` bytes (the end of a long log: what happened last).
    static byte[] read(File f, int max) throws IOException {
        long length = f.length();
        try (InputStream in = new FileInputStream(f)) {
            if (length > max) {
                long skip = length - max;
                while (skip > 0) skip -= in.skip(skip);
            }
            int n = (int) Math.min(length, max);
            byte[] b = new byte[n];
            int off = 0, k;
            while (off < n && (k = in.read(b, off, n - off)) > 0) off += k;
            return off == n ? b : Arrays.copyOf(b, off);
        }
    }

    static int addNewest(ZipOutputStream zip, File dir, String prefix, String suffix, int want, String folder,
                         int max) throws IOException {
        File[] files = dir.listFiles((d, n) -> n.startsWith(prefix) && n.endsWith(suffix));
        if (files == null) return 0;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        int added = 0;
        for (File f : files) {
            if (added >= want) break;
            put(zip, folder + f.getName(), redactIps(read(f, max)));
            added++;
        }
        return added;
    }

    // The game's logs: svr2011_<n>.log, rotated at 5 MB into svr2011_<n>.<k>.log
    // (the higher k, the older; up to 20). A freeze can fill several parts with
    // the same fault line, pushing out what came before it, so the newest
    // game's parts all go in, oldest first, with the repeated fault lines left
    // out after the first ones - its first part (the device, the first faults'
    // host pc) always, then the newest parts back to `budget` bytes - and the
    // two games before it (their last part).
    static final String kFault = "Unhandled guest access violation";
    static final int kFaultsKept = 30;

    static int addLogs(ZipOutputStream zip, File dir, int budget) throws IOException {
        File[] files = dir.listFiles((d, n) -> n.startsWith("svr2011_") && n.endsWith(".log"));
        if (files == null || files.length == 0) return 0;
        java.util.Map<String, java.util.List<File>> games = new java.util.HashMap<>();
        for (File f : files) {
            String n = f.getName();
            String game = n.substring(0, n.indexOf('.'));
            games.computeIfAbsent(game, k -> new java.util.ArrayList<>()).add(f);
        }
        // (games newest first, by their newest file)
        java.util.List<String> order = new java.util.ArrayList<>(games.keySet());
        java.util.Map<String, Long> newest = new java.util.HashMap<>();
        for (String g : order) {
            long t = 0;
            for (File f : games.get(g)) t = Math.max(t, f.lastModified());
            newest.put(g, t);
        }
        order.sort((a, b) -> Long.compare(newest.get(b), newest.get(a)));
        int added = 0;
        java.util.List<File> parts = games.get(order.get(0));
        parts.sort((a, b) -> Integer.compare(part(b), part(a)));  // (oldest first: highest k, the plain .log last)
        // (the fault lines before each part: the game's first ones are kept)
        int[] before = new int[parts.size()];
        for (int i = 1; i < parts.size(); i++) before[i] = before[i - 1] + countFaults(parts.get(i - 1));
        byte[][] kept = new byte[parts.size()][];
        long left = budget;
        // The first part, then the newest ones back, while they fit.
        java.util.List<Integer> pick = new java.util.ArrayList<>();
        pick.add(0);
        for (int i = parts.size() - 1; i > 0; i--) pick.add(i);
        for (int i : pick) {
            if (left <= 0) break;
            byte[] b = condense(read(parts.get(i), 64 << 20), new int[] {before[i]});
            if (b.length > left) b = Arrays.copyOfRange(b, (int) (b.length - left), b.length);
            kept[i] = b;
            left -= b.length;
        }
        for (int i = 0; i < parts.size(); i++) {
            if (kept[i] == null) continue;
            put(zip, "logs/" + parts.get(i).getName(), redactIps(kept[i]));
            added++;
        }
        // The games before (their last part: how they ended).
        for (int g = 1; g < order.size() && g <= 2 && left > 0; g++) {
            File last = null;
            for (File f : games.get(order.get(g))) if (part(f) == 0) last = f;
            if (last == null) continue;
            byte[] b = condense(read(last, (int) Math.min(left, 4 << 20)), new int[1]);
            put(zip, "logs/" + last.getName(), redactIps(b));
            left -= b.length;
            added++;
        }
        return added;
    }

    static int countFaults(File f) throws IOException {
        int n = 0;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(new FileInputStream(f), StandardCharsets.ISO_8859_1), 1 << 16)) {
            String line;
            while ((line = r.readLine()) != null) if (line.contains(kFault)) n++;
        }
        return n;
    }

    // svr2011_12.3.log -> 3, svr2011_12.log -> 0.
    static int part(File f) {
        String[] p = f.getName().split("\\.");
        if (p.length < 3) return 0;
        try {
            return Integer.parseInt(p[1]);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // The log without the repeated fault lines after the first kFaultsKept
    // (counted over the game's parts in `faults`), saying how many went.
    static byte[] condense(byte[] data, int[] faults) {
        String text = new String(data, StandardCharsets.ISO_8859_1);
        StringBuilder out = new StringBuilder(Math.min(text.length(), 1 << 20));
        int dropped = 0;
        for (String line : text.split("\n", -1)) {
            if (line.contains(kFault) && ++faults[0] > kFaultsKept) {
                dropped++;
                continue;
            }
            if (dropped > 0) {
                out.append("(report: ").append(dropped).append(" more '").append(kFault).append("' lines left out)\n");
                dropped = 0;
            }
            out.append(line).append('\n');
        }
        if (dropped > 0) out.append("(report: ").append(dropped).append(" more '").append(kFault).append("' lines left out)\n");
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }

    private static final java.util.regex.Pattern IPV4 = java.util.regex.Pattern.compile(
        "(?<![\\d.])(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})(?!\\d|\\.\\d)");

    // Every IPv4 address (the P2P lines log the player's public one) becomes
    // x.x.x.x, as the PC launcher's report does. Loopback stays, and so do lines
    // with "ersion" in them (driver / API versions such as 1.3.1.1).
    static byte[] redactIps(byte[] data) {
        String text = new String(data, StandardCharsets.ISO_8859_1);  // (bytes kept as they are)
        StringBuilder out = new StringBuilder(text.length());
        boolean first = true;
        for (String line : text.split("\n", -1)) {
            if (!first) out.append('\n');
            first = false;
            if (line.contains("ersion")) {
                out.append(line);
                continue;
            }
            java.util.regex.Matcher m = IPV4.matcher(line);
            StringBuffer sb = new StringBuffer();
            while (m.find()) {
                boolean ip = true;
                for (int g = 1; g <= 4; g++) ip &= Integer.parseInt(m.group(g)) <= 255;
                m.appendReplacement(sb, ip && !m.group(1).equals("127") ? "x.x.x.x" : m.group());
            }
            m.appendTail(sb);
            out.append(sb);
        }
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }
}
