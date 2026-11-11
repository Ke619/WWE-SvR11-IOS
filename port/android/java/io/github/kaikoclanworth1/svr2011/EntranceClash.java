// WWE SmackDown vs. Raw 2011 - entrance number clashes between installed mods, as the PC launcher's
// launcher/entrance_clash.c (the same rules and the same .entrance_moves record: a mod moved on one and copied to the
// other agrees). A mod's pack.txt (pack.txt / moves/pack.txt) adds entrances by number - `pacentry evt\NyujyoN.pac
// EVPx|EVTx <n>`, Create-An-Entrance parts 10000 + 1000k + n, pyro `prtcls.pac PCE6|LPE6 <0nnn>` - and victory
// scenes (`evt\Winning0.pac EVPA|EVTA <n>`); its manifest picks them (entrance=, attire1..4_entrance=, victory=).
// Before a mod is put in place its numbers another installed mod uses for different files move to free ones (the
// pack's lines with the new range's pac / group letters, the scripts' own number, the manifest), recorded in the
// mod folder's .entrance_moves; an update keeps that number while it is free. Plain Java (no android.*): the PC JVM
// tests it.
package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class EntranceClash {
    static final int kEntrance = 0, kVictory = 1;

    // 2011's own numbers: main entrance numbers (pac/evt/Nyujyo*.pac EVP*, below 10000) and victory scenes
    // (pac/evt/Winning*.pac EVP*) - tools/entrance_port.py stock_2011 / stock_victories (entrance_clash.c's tables).
    static final int[] kStockEntrances = {
        50, 100, 101, 102, 103, 104, 107, 110, 112, 115, 117, 122, 123, 125, 131, 132,
        135, 137, 139, 143, 145, 158, 160, 161, 164, 165, 169, 174, 175, 176, 177, 178,
        179, 182, 201, 205, 208, 218, 219, 224, 261, 262, 263, 265, 267, 268, 271, 272,
        275, 276, 277, 278, 279, 281, 282, 283, 284, 290, 291, 292, 293, 294, 295, 297,
        305, 306, 321, 400, 401, 402, 410, 480, 481, 482, 483, 484, 485, 486, 487, 500,
        501, 502, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 518,
        519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534,
        535, 536, 537, 538, 539, 540, 541, 543, 544, 545, 546, 547, 548, 549, 552, 554,
        555, 556, 557, 558, 560, 562, 563, 600, 601, 602, 603, 604, 605, 606, 607, 608,
        609, 610, 611, 612, 613, 614, 615, 616, 617, 618, 620, 621, 622, 628, 629, 630,
        631, 632, 633, 634, 637, 638, 642, 643, 644, 645, 646, 647, 648, 649, 650, 651,
        652, 700, 701, 702, 703, 704, 705, 706, 707, 708, 709, 710, 711, 713, 714, 715,
        716, 720, 721, 722, 723, 724, 725, 726, 727, 728, 729, 730, 732, 733, 735, 736,
        750, 751, 752, 753, 754, 755, 756, 770, 771, 772, 773, 774, 775, 800, 803, 807,
        808, 809, 812, 813, 816, 817, 820, 821, 822, 823, 824, 830, 833, 840, 850};
    static final int[] kStockVictories = {
        6050, 6100, 6101, 6102, 6103, 6104, 6107, 6110, 6112, 6123, 6125, 6131, 6135, 6139, 6145, 6160,
        6161, 6175, 6176, 6179, 6219, 6262, 6272, 6277, 6279, 6700, 6702, 7000, 7001, 7002, 7003, 7004,
        7005, 7006, 7007, 7008, 7009, 7010, 7011, 7012, 7013, 7014, 7015, 7100, 7101, 7102, 7103, 7104,
        7105, 7106, 7107, 7150, 7151, 7152, 7153, 7154, 7155, 7156, 7158, 7200, 7201, 7202, 7203, 7204,
        7205, 7206, 7207, 7208, 7300, 7301, 7400, 7401, 7402, 7403, 7404, 7500, 7501, 7502, 7503, 7600,
        7601, 7602, 7603, 7604, 7605, 8000, 8020, 8030, 8040, 8041, 8050, 8051, 8052, 8060, 8061, 8062,
        8100, 8101, 8102, 8103, 8200, 8201, 8202, 8203, 8300, 8301, 8400, 8401, 8410, 8411, 8412, 8500,
        9000, 9001, 9002, 9003, 9004, 9005, 9100, 9101, 9102, 9103, 9200, 9201, 9202, 9203, 9400};
    static final class Line {
        String text, pac = "", group = "", file = "";
        int kind = -1, base, num;
        boolean pyro;
    }

    static final class Pack {
        File dir;
        List<Line> lines = new ArrayList<>();
    }

    static final class Use {
        int kind, base;
        long hash;
        int rank;   // (the check of installed mods: its mod's place in the install order)
        File owner;
    }

    static final class Move {
        int kind, from, to, clash;   // clash: 1 a new number, 2 kept for a clash, 3 kept, 4 its old number taken since
        String other = "";
    }

    private static final Pattern kPacEntry = Pattern.compile("^\\s*(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(.+?)\\s*$");

    private static long fnv(byte[] b, long h) {
        for (byte x : b) h = (h ^ (x & 0xFF)) * 1099511628211L;
        return h;
    }

    private static byte[] read(File f) {
        try {
            return Files.readAllBytes(f.toPath());
        } catch (IOException e) {
            return null;
        }
    }

    private static boolean digits(String s) {
        if (s.isEmpty()) return false;
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
        return true;
    }

    private static boolean pacIs(String pac, String stem) {
        return pac.replace('/', '\\').toLowerCase().startsWith(stem);
    }

    static void classify(Line l) {
        Matcher m = kPacEntry.matcher(l.text);
        l.kind = -1;
        l.pyro = false;
        if (!m.matches() || !m.group(1).equalsIgnoreCase("pacentry") || !digits(m.group(4))) return;
        l.pac = m.group(2);
        l.group = m.group(3);
        l.file = m.group(5);
        String name = m.group(4);
        l.num = Integer.parseInt(name);
        String g = l.group.toUpperCase();
        if (pacIs(l.pac, "evt\\nyujyo") && (g.startsWith("EVP") || g.startsWith("EVT")) && g.length() == 4) {
            l.kind = kEntrance;
            l.base = l.num >= 10000 ? l.num % 1000 : l.num;
        } else if (pacIs(l.pac, "evt\\winning") && (g.equals("EVPA") || g.equals("EVTA"))) {
            l.kind = kVictory;
            l.base = l.num;
        } else if (pacIs(l.pac, "prtcls.pac") && (g.equals("PCE6") || g.equals("LPE6")) && name.length() == 4) {
            l.kind = kEntrance;
            l.base = l.num;
            l.pyro = true;
        }
    }

    static Pack loadPack(File dir) {
        byte[] d = read(new File(dir, "pack.txt"));
        if (d == null) return null;
        Pack p = new Pack();
        p.dir = dir;
        String text = new String(d, StandardCharsets.ISO_8859_1);   // (byte for byte, as the C reads it)
        int a = 0;
        while (a < text.length()) {
            int b = text.indexOf('\n', a);
            if (b < 0) b = text.length();
            String s = text.substring(a, b);
            if (s.endsWith("\r")) s = s.substring(0, s.length() - 1);
            Line l = new Line();
            l.text = s;
            classify(l);
            p.lines.add(l);
            a = b + 1;
        }
        return p;
    }

    private static File lineFile(Pack p, Line l) {
        return new File(p.dir, l.file.replace('\\', '/'));
    }

    // A line's share of its number's hash: its group (not the range letter), its part and its file's bytes - an
    // EVP's own number left out (a moved copy is the same entrance).
    static long lineHash(Pack p, Line l) {
        long h = 0xcbf29ce484222325L;
        String g = l.group.length() >= 3 ? l.group.substring(0, 3).toUpperCase() : l.group.toUpperCase();
        h = fnv(g.getBytes(StandardCharsets.ISO_8859_1), h);
        int part = l.num >= 10000 ? (l.num - 10000) / 1000 : -1;
        h = fnv(new byte[] {(byte) part, (byte) (part >> 8), (byte) (part >> 16), (byte) (part >> 24)}, h);
        byte[] d = read(lineFile(p, l));
        if (d != null) {
            if (d.length > 0x22 && d[0] == '0' && d[1] == 'F' && d[2] == 'O' && d[3] == 'P') {
                d[0x20] = d[0x21] = 0;
                long tp = u32(d, 0x1C);
                if (tp != 0 && tp + 10 <= d.length) d[(int) tp + 8] = d[(int) tp + 9] = 0;
            }
            h = fnv(d, h);
        }
        return h;
    }

    private static long u32(byte[] d, int at) {
        return ((long) (d[at] & 0xFF) << 24) | ((d[at + 1] & 0xFF) << 16) | ((d[at + 2] & 0xFF) << 8) | (d[at + 3] & 0xFF);
    }

    static List<Pack> modPacks(File folder) {
        List<Pack> out = new ArrayList<>();
        Pack p = loadPack(folder);
        if (p != null) out.add(p);
        p = loadPack(new File(folder, "moves"));
        if (p != null) out.add(p);
        return out;
    }

    static void addUses(File folder, List<Pack> packs, List<Use> uses) {
        for (Pack p : packs)
            for (Line l : p.lines) {
                if (l.kind < 0) continue;
                Use u = null;
                for (Use x : uses)
                    if (x.kind == l.kind && x.base == l.base && x.owner.equals(folder)) { u = x; break; }
                if (u == null) {
                    u = new Use();
                    u.kind = l.kind;
                    u.base = l.base;
                    u.owner = folder;
                    uses.add(u);
                }
                u.hash += lineHash(p, l);   // (a sum: the lines' order doesn't matter)
            }
    }

    static String manifestName(File folder) {
        byte[] d = read(new File(folder, "manifest.txt"));
        if (d != null)
            for (String s : new String(d, StandardCharsets.UTF_8).split("\n")) {
                s = s.replace("\r", "");
                if (s.startsWith("name=") && s.length() > 5) return s.substring(5);
            }
        return folder.getName();
    }

    static boolean stock(int kind, int n) {
        for (int x : kind == kEntrance ? kStockEntrances : kStockVictories) if (x == n) return true;
        return false;
    }

    // Entrances by kind: tag 700-769, trio 770-799, singles 100-999 outside them; victory scenes 6300-6999.
    static int[] rangeOf(int kind, int base) {
        if (kind == kVictory) return new int[] {6300, 6999};
        if (base >= 700 && base <= 769) return new int[] {700, 769};
        if (base >= 770 && base <= 799) return new int[] {770, 799};
        return new int[] {100, 999};
    }

    static boolean taken(int kind, int n, List<Use> uses, List<Use> mine, List<Move> mv) {
        if (stock(kind, n)) return true;
        for (Use u : uses) if (u.kind == kind && u.base == n) return true;
        for (Use u : mine) if (u.kind == kind && u.base == n) return true;
        for (Move m : mv) if (m.kind == kind && m.to == n) return true;
        return false;
    }

    static int freeNumber(int kind, int base, List<Use> uses, List<Use> mine, List<Move> mv) {
        int[] r = rangeOf(kind, base);
        for (int n = r[0]; n <= r[1]; n++) {
            if (kind == kEntrance && r[0] == 100 && n >= 700 && n <= 799) continue;
            if (!taken(kind, n, uses, mine, mv)) return n;
        }
        return 0;
    }

    // The pac digit and range letter of an entrance number (Yuke's Nyujyo ranges).
    static char[] nyujyoFor(int n) {
        int[][] r = {{100, 134}, {135, 178}, {179, 271}, {272, 290}, {291, 479}, {480, 511}, {512, 531}, {532, 551},
            {552, 599}, {600, 999}};
        String f = "0123456789", g = "12345EFGHI";
        for (int i = 0; i < r.length; i++)
            if (n >= r[i][0] && n <= r[i][1]) return new char[] {f.charAt(i), g.charAt(i)};
        return new char[] {'9', 'I'};
    }

    static void patchEvp(File f, int oldNum, int newNum) throws IOException {
        byte[] d = read(f);
        if (d == null || d.length <= 0x22 || d[0] != '0' || d[1] != 'F' || d[2] != 'O' || d[3] != 'P') return;
        boolean changed = false;
        if (((d[0x20] & 0xFF) << 8 | (d[0x21] & 0xFF)) == oldNum) {
            d[0x20] = (byte) (newNum >> 8);
            d[0x21] = (byte) newNum;
            changed = true;
        }
        long tp = u32(d, 0x1C);
        if (tp != 0 && tp + 10 <= d.length && ((d[(int) tp + 8] & 0xFF) << 8 | (d[(int) tp + 9] & 0xFF)) == oldNum) {
            d[(int) tp + 8] = (byte) (newNum >> 8);
            d[(int) tp + 9] = (byte) newNum;
            changed = true;
        }
        if (changed) Files.write(f.toPath(), d);
    }

    static void moveInPack(Pack p, Move m) throws IOException {
        boolean any = false;
        for (Line l : p.lines) {
            if (l.kind != m.kind || l.base != m.from) continue;
            int nn = l.pyro ? m.to : l.num >= 10000 ? 10000 + 1000 * ((l.num - 10000) / 1000) + m.to : m.to;
            if (!l.pyro && l.group.toUpperCase().startsWith("EVP")) patchEvp(lineFile(p, l), l.num, nn);
            if (l.kind == kEntrance && !l.pyro) {
                char[] fg = nyujyoFor(m.to);
                l.pac = "evt\\Nyujyo" + fg[0] + ".pac";
                l.group = l.group.substring(0, 3) + fg[1];
            }
            l.text = "pacentry " + l.pac + " " + l.group + " " + (l.pyro ? String.format("%04d", nn) : Integer.toString(nn))
                + " " + l.file;
            l.num = nn;
            any = true;
        }
        if (any) {
            StringBuilder sb = new StringBuilder();
            for (Line l : p.lines) sb.append(l.text).append("\r\n");
            Files.write(new File(p.dir, "pack.txt").toPath(), sb.toString().getBytes(StandardCharsets.ISO_8859_1));
        }
    }

    // manifest.txt: entrance= / attire1..4_entrance= (entrances), victory= (victory scenes) of a moved number.
    static void moveInManifest(File folder, List<Move> mv) throws IOException {
        File f = new File(folder, "manifest.txt");
        byte[] d = read(f);
        if (d == null) return;
        String[] lines = new String(d, StandardCharsets.UTF_8).split("\n", -1);
        boolean changed = false;
        for (int i = 0; i < lines.length; i++) {
            String s = lines[i];
            int eq = s.indexOf('=');
            if (eq < 0) continue;
            String k = s.substring(0, eq);
            int kind = k.equals("entrance") || k.matches("attire[1-4]_entrance") ? kEntrance : k.equals("victory") ? kVictory : -1;
            if (kind < 0) continue;
            boolean cr = s.endsWith("\r");
            int v;
            try {
                v = Integer.parseInt(s.substring(eq + 1).trim());
            } catch (NumberFormatException e) {
                continue;
            }
            for (Move m : mv)
                if (m.kind == kind && m.from == v) {
                    lines[i] = k + "=" + m.to + (cr ? "\r" : "");
                    changed = true;
                    break;
                }
        }
        if (changed) Files.write(f.toPath(), String.join("\n", lines).getBytes(StandardCharsets.UTF_8));
    }

    // The old copy's moves (dest/.entrance_moves: "entrance <from> <to>" / "victory <from> <to>").
    static List<Move> readRecord(File dest) {
        List<Move> out = new ArrayList<>();
        byte[] d = dest == null ? null : read(new File(dest, ".entrance_moves"));
        if (d == null) return out;
        for (String s : new String(d, StandardCharsets.ISO_8859_1).split("[\r\n]+")) {
            String[] w = s.trim().split("\\s+");
            if (w.length != 3) continue;
            try {
                Move m = new Move();
                m.kind = w[0].equals("victory") ? kVictory : kEntrance;
                m.from = Integer.parseInt(w[1]);
                m.to = Integer.parseInt(w[2]);
                out.add(m);
            } catch (NumberFormatException e) {
                // (a line it doesn't know)
            }
        }
        return out;
    }

    // Every installed mod folder with a pack (Mods/<type>/<mod>), `dest` and switched-off ones left out.
    static List<Use> installedUses(File mods, File dest) {
        List<Use> uses = new ArrayList<>();
        File[] types = mods.listFiles();
        if (types == null) return uses;
        for (File t : types) {
            if (!t.isDirectory() || t.getName().startsWith(".") || t.getName().equalsIgnoreCase("PacOverlay")) continue;
            File[] ms = t.listFiles();
            if (ms == null) continue;
            for (File m : ms) {
                if (!m.isDirectory() || m.getName().startsWith(".") || new File(m, "disabled").exists()) continue;
                if (dest != null && m.getAbsoluteFile().equals(dest.getAbsoluteFile())) continue;
                List<Pack> packs = modPacks(m);
                if (!packs.isEmpty()) addUses(m, packs, uses);
            }
        }
        return uses;
    }

    // The record of a mod's moves (folder/.entrance_moves). merge: the record already there kept, a number moved again
    // followed on ("entrance 504 105" + 105 -> 106: "entrance 504 106"), new ones added (entrance_clash.c write_record).
    static void writeRecord(File folder, List<Move> mv, boolean merge) throws IOException {
        if (mv.isEmpty()) return;
        List<Move> rec = merge ? readRecord(folder) : new ArrayList<>();
        for (Move m : mv) {
            Move hit = null;
            for (Move r : rec) if (r.kind == m.kind && r.to == m.from) { hit = r; break; }
            if (hit != null) hit.to = m.to;
            else rec.add(m);
        }
        StringBuilder rs = new StringBuilder();
        for (Move m : rec)
            if (m.from != m.to)
                rs.append(m.kind == kVictory ? "victory" : "entrance").append(' ').append(m.from).append(' ').append(m.to).append("\r\n");
        try (OutputStream o = new FileOutputStream(new File(folder, ".entrance_moves"))) {
            o.write(rs.toString().getBytes(StandardCharsets.ISO_8859_1));
        }
    }

    // incoming: the mod unpacked (rewritten in place); dest: the folder it will replace or take (its old copy's
    // record read; left out of the installed mods compared); mods: the Mods folder. -> one plain line per move
    // ("" none).
    static String fix(File mods, File incoming, File dest, String name) throws IOException {
        return fixCore(incoming, dest, name, installedUses(mods, dest), -1, Integer.MAX_VALUE, false, new ArrayList<>());
    }

    // The fix itself: incoming's numbers against uses - skip: a mod's own (its rank), below: only those of mods before
    // it clash. recheck: a mod already in place (incoming is its folder) - only its clashes move, its record is added to.
    static String fixCore(File incoming, File dest, String name, List<Use> uses, int skip, int below, boolean recheck,
                          List<Move> mv) throws IOException {
        List<Pack> packs = modPacks(incoming);
        if (packs.isEmpty()) return "";
        List<Use> mine = new ArrayList<>();
        addUses(incoming, packs, mine);
        if (mine.isEmpty()) return "";
        List<Use> others = new ArrayList<>();
        for (Use x : uses) if (x.rank != skip) others.add(x);
        List<Move> rec = recheck ? new ArrayList<>() : readRecord(dest);   // (a recheck moves clashes only)
        for (Use u : mine) {
            Use clash = null;
            for (Use x : others) if (x.rank < below && x.kind == u.kind && x.base == u.base && x.hash != u.hash) { clash = x; break; }
            int prev = 0, to = 0;
            for (Move r : rec) if (r.kind == u.kind && r.from == u.base) prev = r.to;
            if (prev != 0 && prev != u.base && !taken(u.kind, prev, others, mine, mv)) to = prev;
            else if (clash != null) to = freeNumber(u.kind, u.base, others, mine, mv);
            if (to == 0) continue;
            Move m = new Move();
            m.kind = u.kind;
            m.from = u.base;
            m.to = to;
            m.clash = clash != null ? (to == prev ? 2 : 1) : 3;
            if (prev != 0 && to != prev) m.clash = 4;
            if (clash != null) m.other = manifestName(clash.owner);
            mv.add(m);
        }
        for (Move m : mv) for (Pack p : packs) moveInPack(p, m);
        if (mv.isEmpty()) return "";
        moveInManifest(incoming, mv);
        writeRecord(incoming, mv, recheck);
        StringBuilder report = new StringBuilder();
        for (Move m : mv) {
            String what = m.kind == kVictory ? "victory scene" : "entrance";
            if (report.length() > 0) report.append('\n');
            if (m.clash == 1 || m.clash == 4)
                report.append('"').append(name).append("\" used ").append(what).append(' ').append(m.from).append(", which \"")
                    .append(m.other).append("\" already uses for a different ").append(what).append(" - it now uses ")
                    .append(m.to).append(", so both play their own.").append(m.clash == 4 ? " (Its earlier number was taken since.)" : "");
            else
                report.append('"').append(name).append("\" keeps ").append(what).append(' ').append(m.to)
                    .append(", the number it was moved to before (its own ").append(m.from).append(' ')
                    .append(m.clash == 2 ? "is another mod's" : "was moved").append(").");
        }
        return report.toString();
    }

    // When a mod was installed: its manifest.txt's time (written by the install - a fresh file, not the .svrmod's
    // time); the folder's when it has none. (The PC reads the folder's creation time; Android's file systems keep none.)
    static long installedAt(File folder) {
        File man = new File(folder, "manifest.txt");
        long t = man.isFile() ? man.lastModified() : 0;
        return t != 0 ? t : folder.lastModified();
    }

    // The mods already installed checked with the same rules (mods installed before the fix, report 85741e;
    // entrance_clash.c entrance_clash_recheck): in the order they were installed, each against the ones before it - a
    // number used with different files moves in the later mod (pack, scripts, manifest; added to its .entrance_moves).
    // Shared entrances (the same files) and numbers moved before are left alone; a second run moves nothing.
    // -> one plain line per move ("" none).
    static String recheck(File mods) throws IOException {
        List<File> folders = new ArrayList<>();
        File[] types = mods.listFiles();
        if (types == null) return "";
        for (File t : types) {
            if (!t.isDirectory() || t.getName().startsWith(".") || t.getName().equalsIgnoreCase("PacOverlay")) continue;
            File[] ms = t.listFiles();
            if (ms == null) continue;
            for (File m : ms)
                if (m.isDirectory() && !m.getName().startsWith(".") && !new File(m, "disabled").exists()
                        && (new File(m, "pack.txt").isFile() || new File(m, "moves/pack.txt").isFile()))
                    folders.add(m);
        }
        final java.util.Map<File, Long> at = new java.util.HashMap<>();
        for (File f : folders) at.put(f, installedAt(f));
        folders.sort((x, y) -> {
            int c = Long.compare(at.get(x), at.get(y));
            return c != 0 ? c : x.getAbsolutePath().compareToIgnoreCase(y.getAbsolutePath());
        });
        List<Use> uses = new ArrayList<>();
        for (int i = 0; i < folders.size(); i++) {
            List<Pack> packs = modPacks(folders.get(i));
            int from = uses.size();
            addUses(folders.get(i), packs, uses);
            for (int k = from; k < uses.size(); k++) uses.get(k).rank = i;
        }
        StringBuilder report = new StringBuilder();
        for (int i = 0; i < folders.size(); i++) {
            boolean clash = false;
            for (Use u : uses) {
                if (u.rank != i) continue;
                for (Use x : uses)
                    if (x.rank < i && x.kind == u.kind && x.base == u.base && x.hash != u.hash) { clash = true; break; }
                if (clash) break;
            }
            if (!clash) continue;
            File f = folders.get(i);
            List<Move> mv = new ArrayList<>();
            String one = fixCore(f, f, manifestName(f), uses, i, i, true, mv);
            for (Use u : uses)   // (a moved script hashes as before: the move followed on)
                if (u.rank == i)
                    for (Move m : mv)
                        if (u.kind == m.kind && u.base == m.from) { u.base = m.to; break; }
            if (!one.isEmpty()) {
                if (report.length() > 0) report.append('\n');
                report.append(one);
            }
        }
        return report.toString();
    }
}
