// The save cleaner (the Saves page's "Clean save of unused mods…"): what the save keeps for superstar mods that
// aren't installed or are turned off, and taking it out - the PC launcher's launcher/save_cleaner.c in Java (no
// Android classes: tested on the PC's JVM against the same saves).
//
// A mod plays on one of the game's 137 placeholder ids (Mods/Superstars/slots.txt says which). SaveData.dat keeps a
// record, profile and 40-byte entry per id; the game writes each id's placeholders at its start, before a save is
// loaded, to Saves/.universe/pool_placeholders.bin (superstar_mods.cpp KeepPoolPlaceholders). An id whose saved
// data is none of its placeholders holds a mod's data; with its .universe/mods.bin entry, its mod_profiles.txt line
// and its Saves/.mods/<folder>.dat (per_mod_saves) it's listed, and taken out: the placeholders back (the save's
// checksum set again), the entry, line and file gone. The game's superstars, Created Superstars and the mods that
// are on aren't touched.
package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class SaveCleaner {
    static final class Item {
        int id, index;
        boolean off, profile, universe, mark, modSave;
        String folder = "", name = "";

        String what() {
            StringBuilder s = new StringBuilder();
            if (profile) s.append("superstar, moves, entrance, Threads");
            if (universe) s.append(s.length() > 0 ? ", Universe" : "Universe");
            if (modSave) s.append(s.length() > 0 ? ", its mod save" : "its mod save");
            return s.length() > 0 ? s.toString() : "a note of it";
        }
    }

    static final class Failure extends Exception {
        Failure(String message) { super(message); }
    }

    private static final int kSumFrom = 0x1C, kSumTo = 0x812DC, kSelectable = 221, kName = 34;

    private static final class Data {
        byte[] pool, save, univ;
        int recSize, profSize, entSize, recOff, profOff, entOff;
        final int[] index = new int[512];
        final List<List<int[]>> versions = new ArrayList<>();  // per id: {record at, profile at, entry at} in pool

        Data() { for (int i = 0; i < 512; i++) versions.add(new ArrayList<>()); }
    }

    private static int le32(byte[] b, int at) {
        return (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8 | (b[at + 2] & 0xFF) << 16 | (b[at + 3] & 0xFF) << 24;
    }

    private static boolean same(byte[] a, int at, byte[] b, int bt, int n) {
        for (int i = 0; i < n; i++) if (a[at + i] != b[bt + i]) return false;
        return true;
    }

    private static byte[] read(File f) {
        try {
            return f.isFile() ? Files.readAllBytes(f.toPath()) : null;
        } catch (IOException e) {
            return null;
        }
    }

    // 2 installed and on, 1 installed and off, 0 not there.
    static int modState(File mods, String folder) {
        if (folder.isEmpty()) return 0;
        File d = new File(mods, folder);
        if (!new File(d, "ch.pac").isFile()) return 0;
        return new File(d, "disabled").exists() ? 1 : 2;
    }

    private static Data load(File saves) throws Failure {
        Data d = new Data();
        d.save = read(new File(saves, "SaveData.dat"));
        if (d.save == null) throw new Failure("There's no main save (SaveData.dat) yet.");
        d.pool = read(new File(saves, ".universe/pool_placeholders.bin"));
        if (d.pool == null || d.pool.length < 36
            || !new String(d.pool, 0, 8, StandardCharsets.US_ASCII).equals("SVRPOOL2"))
            throw new Failure("Start the game once with these saves first: at its start it notes the game's own "
                + "superstar data the cleaner compares the save with (version 2.1.9 and later).");
        d.recSize = le32(d.pool, 8);
        d.profSize = le32(d.pool, 12);
        d.entSize = le32(d.pool, 16);
        d.recOff = le32(d.pool, 20);
        d.profOff = le32(d.pool, 24);
        d.entOff = le32(d.pool, 28);
        int count = le32(d.pool, 32);
        if (d.recSize != 260 || d.profSize != 1056 || d.entSize != 40 || d.save.length < kSumTo
            || d.save.length < d.profOff + 512 * d.profSize || d.save.length < d.entOff + 512 * d.entSize)
            throw new Failure("The save or the game's notes (.universe/pool_placeholders.bin) aren't as expected.");
        int at = 36;
        for (int i = 0; i < count && at + 4 + d.recSize + d.profSize + d.entSize <= d.pool.length; i++) {
            int id = (d.pool[at] & 0xFF) | (d.pool[at + 1] & 0xFF) << 8;
            int index = (d.pool[at + 2] & 0xFF) | (d.pool[at + 3] & 0xFF) << 8;
            if (id < 512 && index < 512) {
                d.index[id] = index;
                d.versions.get(id).add(new int[] {at + 4, at + 4 + d.recSize, at + 4 + d.recSize + d.profSize});
            }
            at += 4 + d.recSize + d.profSize + d.entSize;
        }
        d.univ = read(new File(saves, ".universe/mods.bin"));
        if (d.univ != null && (d.univ.length < 12
                               || !new String(d.univ, 0, 8, StandardCharsets.US_ASCII).equals("SVRUNIV1")))
            d.univ = null;
        return d;
    }

    // The id's record (its selectable byte aside), profile and entry are one of its placeholders.
    private static boolean isPlaceholder(Data d, int id) {
        int ix = d.index[id];
        int rec = d.recOff + ix * d.recSize, prof = d.profOff + ix * d.profSize, ent = d.entOff + ix * d.entSize;
        boolean r = false, p = false, e = false;
        for (int[] v : d.versions.get(id)) {
            r |= same(d.save, rec, d.pool, v[0], kSelectable)
                 && same(d.save, rec + kSelectable + 1, d.pool, v[0] + kSelectable + 1, d.recSize - kSelectable - 1);
            p |= same(d.save, prof, d.pool, v[1], d.profSize);
            e |= same(d.save, ent, d.pool, v[2], d.entSize);
        }
        return r && p && e;
    }

    // "<a>\t<b>" lines -> folder per id: slots.txt "<folder>\t<id>", mod_profiles.txt "<id>\t<folder>\t<fp>".
    private static String[] idFolders(File f, boolean idFirst) {
        String[] out = new String[512];
        byte[] b = read(f);
        if (b == null) return out;
        for (String line : new String(b, StandardCharsets.UTF_8).split("\n")) {
            String[] p = line.replace("\r", "").split("\t");
            if (p.length < 2) continue;
            try {
                int id = Integer.parseInt((idFirst ? p[0] : p[p.length - 1]).trim());
                if (id > 0 && id < 512) out[id] = idFirst ? p[1] : line.substring(0, line.lastIndexOf('\t'));
            } catch (NumberFormatException ignored) {
            }
        }
        return out;
    }

    // Letters and digits, lower case, a leading "the" dropped ("The Sandman" = "sandman", "Bob Orton" = "bob_orton").
    private static String nameKey(String s) {
        String k = s.toLowerCase(Locale.ROOT).replaceAll("[^\\p{L}\\p{N}]", "");
        return k.startsWith("the") && k.length() > 3 ? k.substring(3) : k;
    }

    private static Item itemFor(List<Item> items, int id, String folder) {
        for (Item it : items)
            if ((!folder.isEmpty() && it.folder.equalsIgnoreCase(folder)) || (id != 0 && it.id == id)) return it;
        if (!folder.isEmpty() && id == 0) {  // (a folder only: the save's mod of that name, its folder unknown)
            String a = nameKey(folder);
            for (Item it : items)
                if (it.folder.isEmpty() && it.id != 0 && !a.isEmpty() && a.equals(nameKey(it.name))) {
                    it.folder = folder;
                    return it;
                }
        }
        Item it = new Item();
        it.id = id;
        it.folder = folder;
        items.add(it);
        return it;
    }

    // mods.bin: "SVRUNIV1", u32 n, per mod: u8 length + folder, 24 + 150 bytes; then n x n bytes. Folders, or null
    // when it's cut short.
    private static List<String> univFolders(Data d) {
        List<String> out = new ArrayList<>();
        if (d.univ == null) return out;
        int n = le32(d.univ, 8), at = 12;
        for (int i = 0; i < n; i++) {
            if (at >= d.univ.length) return null;
            int len = d.univ[at] & 0xFF;
            if (at + 1 + len + 174 > d.univ.length) return null;
            out.add(new String(d.univ, at + 1, len, StandardCharsets.UTF_8));
            at += 1 + len + 174;
        }
        return at + (long) n * n <= d.univ.length ? out : null;
    }

    // What `saves` keeps for mods that aren't installed or are off in `mods` (the game's Mods/Superstars).
    static List<Item> scan(File saves, File mods) throws Failure {
        Data d = load(saves);
        List<Item> items = new ArrayList<>();
        String[] slots = idFolders(new File(mods, "slots.txt"), false);
        String[] marks = idFolders(new File(saves, ".universe/mod_profiles.txt"), true);
        for (int id = 1; id < 512; id++) {
            if (slots[id] != null && modState(mods, slots[id]) == 2) continue;  // (a mod that's on holds it)
            String folder = slots[id] != null ? slots[id] : marks[id] != null ? marks[id] : "";
            boolean leftover = !d.versions.get(id).isEmpty() && !isPlaceholder(d, id);
            if (!leftover && marks[id] == null) continue;
            Item it = itemFor(items, id, folder);
            it.id = id;
            it.index = d.index[id];
            it.off = modState(mods, folder) == 1;
            it.profile |= leftover;
            it.mark |= marks[id] != null;
            if (leftover) {
                int at = d.recOff + d.index[id] * d.recSize + kName, n = 0;
                while (n < 32 && d.save[at + n] != 0) n++;
                it.name = new String(d.save, at, n, StandardCharsets.ISO_8859_1);
            }
            if (it.name.isEmpty()) it.name = folder.isEmpty() ? "(a mod)" : folder;
        }
        List<String> univ = univFolders(d);
        if (univ != null)
            for (String f : univ) {
                int st = modState(mods, f);
                if (st == 2) continue;
                Item it = itemFor(items, 0, f);
                it.universe = true;
                it.off = st == 1;
                if (it.name.isEmpty()) it.name = f;
            }
        File[] dats = new File(saves, ".mods").listFiles((dir, n) -> n.toLowerCase(Locale.ROOT).endsWith(".dat"));
        if (dats != null)
            for (File f : dats) {
                String folder = f.getName().substring(0, f.getName().length() - 4);
                int st = modState(mods, folder);
                if (st == 2) continue;
                Item it = itemFor(items, 0, folder);
                it.modSave = true;
                it.off = st == 1;
                if (it.name.isEmpty()) it.name = folder;
            }
        return items;
    }

    private static boolean taken(List<Item> items, boolean withOff, int id, String folder) {
        for (Item it : items)
            if ((!it.off || withOff)
                && ((id != 0 && it.id == id) || (!folder.isEmpty() && it.folder.equalsIgnoreCase(folder))))
                return true;
        return false;
    }

    // Takes the items out (the turned-off ones too with withOff); the caller backs the saves up first. Returns how
    // many mods were taken out.
    static int apply(File saves, List<Item> items, boolean withOff) throws Failure, IOException {
        Data d = load(saves);
        boolean changed = false;
        for (Item it : items) {
            if (!it.profile || (it.off && !withOff) || it.id <= 0 || it.id >= 512) continue;
            List<int[]> vs = d.versions.get(it.id);
            if (vs.isEmpty()) continue;
            int[] v = vs.get(vs.size() - 1);
            int ix = d.index[it.id];
            System.arraycopy(d.pool, v[0], d.save, d.recOff + ix * d.recSize, d.recSize);
            System.arraycopy(d.pool, v[1], d.save, d.profOff + ix * d.profSize, d.profSize);
            System.arraycopy(d.pool, v[2], d.save, d.entOff + ix * d.entSize, d.entSize);
            changed = true;
        }
        if (changed) {
            int sum = 0;
            for (int k = kSumFrom; k < kSumTo; k++) sum += d.save[k] & 0xFF;
            d.save[0x18] = (byte) (sum >>> 24);
            d.save[0x19] = (byte) (sum >>> 16);
            d.save[0x1A] = (byte) (sum >>> 8);
            d.save[0x1B] = (byte) sum;
            // (in place: the saves' hard links stay)
            try (RandomAccessFile f = new RandomAccessFile(new File(saves, "SaveData.dat"), "rw")) {
                f.seek(0);
                f.write(d.save);
                f.setLength(d.save.length);
            }
        }
        List<String> univ = univFolders(d);
        if (univ != null && !univ.isEmpty()) {
            List<Integer> keep = new ArrayList<>();
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            out.write(d.univ, 0, 12);
            int at = 12;
            for (int i = 0; i < univ.size(); i++) {
                int size = 1 + (d.univ[at] & 0xFF) + 174;
                if (!taken(items, withOff, 0, univ.get(i))) {
                    out.write(d.univ, at, size);
                    keep.add(i);
                }
                at += size;
            }
            if (keep.size() < univ.size()) {
                int n = univ.size();
                for (int i : keep)
                    for (int j : keep) out.write(d.univ[at + i * n + j]);
                byte[] b = out.toByteArray();
                int k = keep.size();
                b[8] = (byte) k;
                b[9] = (byte) (k >>> 8);
                b[10] = (byte) (k >>> 16);
                b[11] = (byte) (k >>> 24);
                Files.write(new File(saves, ".universe/mods.bin").toPath(), b);
            }
        }
        File marks = new File(saves, ".universe/mod_profiles.txt");
        byte[] mb = read(marks);
        if (mb != null) {
            StringBuilder kept = new StringBuilder();
            String text = new String(mb, StandardCharsets.UTF_8);
            boolean dropped = false;
            for (String line : text.split("(?<=\n)")) {
                String[] p = line.replace("\r", "").replace("\n", "").split("\t");
                int id = 0;
                try {
                    id = Integer.parseInt(p[0].trim());
                } catch (NumberFormatException ignored) {
                }
                if (p.length >= 2 && taken(items, withOff, id, p[1])) {
                    dropped = true;
                    continue;
                }
                kept.append(line);
            }
            if (dropped) Files.write(marks.toPath(), kept.toString().getBytes(StandardCharsets.UTF_8));
        }
        int done = 0;
        for (Item it : items) {
            if (it.off && !withOff) continue;
            done++;
            if (it.modSave) new File(saves, ".mods/" + it.folder + ".dat").delete();  // (the backup has it; .on stays)
        }
        return done;
    }

    // The PC JVM test: java SaveCleaner <saves> <Mods/Superstars> [--with-off] [--apply]
    public static void main(String[] args) throws Exception {
        boolean withOff = false, apply = false;
        for (int i = 2; i < args.length; i++) {
            withOff |= args[i].equals("--with-off");
            apply |= args[i].equals("--apply");
        }
        List<Item> items = scan(new File(args[0]), new File(args[1]));
        for (Item it : items)
            System.out.println("id " + it.id + "\t" + it.name + "\t" + (it.folder.isEmpty() ? "-" : it.folder) + "\t"
                + (it.off ? "off " : "") + (it.profile ? "profile " : "") + (it.universe ? "universe " : "")
                + (it.mark ? "mark " : "") + (it.modSave ? "mod_save" : ""));
        System.out.println(items.size() + " mods");
        if (apply) System.out.println("cleaned " + apply(new File(args[0]), items, withOff));
    }
}
