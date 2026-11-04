// WWE SmackDown vs. Raw 2011 - the launcher's Mods tab on the phone (as the PC
// launcher's, arenas branch): installed mods in the game folder's
// Mods/Arenas/<id> (manifest.txt, arena.pac, banner.dds) and Mods/Superstars/<id>
// (manifest.txt, ch.pac, maybe a theme song and an entrance movie); a
// "disabled" file turns one off. A switch for each, Remove, and Add a .svrmod
// (a zip) picked from the phone. Mods are made on the PC with the SvR2011 Mod
// Maker. Match types the port adds are mods too (type=matchtype, a manifest
// only, in Mods/MatchTypes/<id>) so they can be switched off; bundled ones come off.
// Main menu backgrounds are mods too (Mods/Backgrounds/<id>: picture.dds +
// manifest); "Add a background picture" makes one from a photo (Backgrounds.java).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ModsPage {
    private final LauncherActivity a_;
    private LinearLayout installed_;

    ModsPage(LauncherActivity a) { a_ = a; }

    static File arenasFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Arenas"); }
    static File superstarsFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Superstars"); }
    static File signsFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Signs"); }
    static File mediaFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Media"); }
    static File backstageFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Backstage"); }
    static File matchTypesFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "MatchTypes"); }
    static File attiresFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Attires"); }
    static File backgroundsFolder() { return Backgrounds.folder(); }

    // This app's version: the game's (the app installs the game of its own release).
    static String gameVersion(android.content.Context ctx) {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    // A mod's needs= (docs/SVRMOD_FORMAT.md: the oldest game version that plays it - a big model or arena) is newer
    // than this game: older games leave a big model out, and crash loading a big arena (the PC's mods_tab.c).
    static boolean needsNewer(android.content.Context ctx, String needs) {
        String v = gameVersion(ctx);
        return needs != null && !needs.trim().isEmpty() && v != null && !v.isEmpty() && Updater.newer(needs.trim(), v);
    }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Custom arenas and superstars made with the SvR2011 Mod Maker on the PC. Arenas are "
            + "on the arena select pages after the game's own arenas (move right past the last arena); superstars are "
            + "under the M tile of the character select (up to " + superstarLimit() + "). Changes apply the next time the game starts.",
            14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);
        Button add = a_.button("Add a mod (.svrmod)…", LauncherActivity.kRed);
        add.setOnClickListener(v -> pick());
        c.addView(add, a_.fullWidth(14));
        Button picture = a_.button("Add a background picture…", LauncherActivity.kCard);
        picture.setOnClickListener(v -> pickPicture());
        c.addView(picture, a_.fullWidth(8));
        Button bundled = a_.button("Get bundled mods", LauncherActivity.kCard);
        bundled.setOnClickListener(v -> {
            if (!InstallActivity.installed()) a_.status("Install the game first.");
            else BundledMods.offer(a_, true, this::refresh);
        });
        c.addView(bundled, a_.fullWidth(8));
        installed_ = a_.card(c, "Installed mods");
        refresh();
        return c;
    }

    static Map<String, String> manifest(File dir) {
        Map<String, String> m = new HashMap<>();
        File f = new File(dir, "manifest.txt");
        if (!f.isFile()) f = new File(dir, "info.txt");
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException ignored) {
        }
        return m;
    }

    void refresh() {
        installed_.removeAllViews();
        File[] arenas = arenasFolder().listFiles(f -> f.isDirectory() && new File(f, "arena.pac").isFile());
        File[] stars = superstarsFolder().listFiles(f -> f.isDirectory() && new File(f, "ch.pac").isFile());
        File[] signs = signsFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] media = mediaFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] back = backstageFolder().listFiles(f -> f.isDirectory() && new File(f, "arena.pac").isFile());
        File[] types = matchTypesFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] pics = backgroundsFolder().listFiles(f -> f.isDirectory() && hasPicture(f));
        File[] attires = attiresFolder().listFiles(f -> f.isDirectory() && new File(f, "attire.pac").isFile());
        File[] dirs = new File[(arenas == null ? 0 : arenas.length) + (stars == null ? 0 : stars.length)
            + (signs == null ? 0 : signs.length) + (media == null ? 0 : media.length) + (back == null ? 0 : back.length)
            + (types == null ? 0 : types.length) + (pics == null ? 0 : pics.length) + (attires == null ? 0 : attires.length)];
        int k = 0;
        if (arenas != null) for (File f : arenas) dirs[k++] = f;
        if (stars != null) for (File f : stars) dirs[k++] = f;
        if (signs != null) for (File f : signs) dirs[k++] = f;
        if (media != null) for (File f : media) dirs[k++] = f;
        if (back != null) for (File f : back) dirs[k++] = f;
        if (types != null) for (File f : types) dirs[k++] = f;
        if (pics != null) for (File f : pics) dirs[k++] = f;
        if (attires != null) for (File f : attires) dirs[k++] = f;
        if (dirs.length == 0) {
            TextView none = a_.text("None yet.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            installed_.addView(none);
            return;
        }
        Arrays.sort(dirs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        java.util.Set<String> leftOut = superstarsLeftOut(stars);
        if (!leftOut.isEmpty()) {
            int on = 0;
            if (stars != null) for (File f : stars) if (!new File(f, "disabled").exists()) on++;
            TextView over = a_.text((on - leftOut.size()) + " of " + on + " superstar mods fit - " + leftOut.size()
                + " aren't in the game (" + superstarLimit() + " is the limit). Switch some off to choose which play.", 15, LauncherActivity.kRed);
            over.setPadding(0, a_.dp(8), 0, a_.dp(8));
            installed_.addView(over);
        }
        for (File d : dirs) {
            Map<String, String> m = manifest(d);
            String name = m.containsKey("name") ? m.get("name") : d.getName();
            boolean star = new File(d, "ch.pac").isFile();
            boolean signPack = d.getParentFile().getName().equals("Signs");
            boolean mediaPack = d.getParentFile().getName().equals("Media");
            boolean backPack = d.getParentFile().getName().equals("Backstage");
            boolean matchType = d.getParentFile().getName().equals("MatchTypes");
            boolean pic = d.getParentFile().getName().equals("Backgrounds");
            boolean attire = d.getParentFile().getName().equals("Attires");
            String hint = (star && leftOut.contains(d.getName()) ? "NOT IN GAME - over the " + superstarLimit() + " limit. " : "")
                + (attire ? "Attire " + m.get("attire") + " for " + starName(m.get("superstar")) : pic ? "Menu background" : matchType ? (d.getName().equals("free_roaming_backstage") ? "Match type - REMOVED: FREE-ROAMING BACKSTAGE isn't in the game for now" : "Match type") : backPack ? "Backstage area - OFF: backstage mods are switched off, Backstage Brawl plays the stock backstage" : mediaPack ? "Media" : signPack ? "Crowd signs" : star ? "Superstar" : "Arena")
                + (m.containsKey("author") && !m.get("author").isEmpty() ? " by " + m.get("author") : "")
                + (m.containsKey("version") ? ", v" + m.get("version") : "")
                + (needsNewer(a_, m.get("needs")) ? " - NEEDS GAME " + m.get("needs") : "")
                + (signPack || mediaPack || matchType || pic ? "" : ", " + FileOps.human(new File(d, star ? "ch.pac" : attire ? "attire.pac" : "arena.pac").length()));
            LinearLayout controls = new LinearLayout(a_);
            Switch on = new Switch(a_);
            // (red when on, as the Settings switches: the default is hard to see)
            int[][] states = {{android.R.attr.state_checked}, {}};
            on.setThumbTintList(new android.content.res.ColorStateList(states, new int[] {LauncherActivity.kRed, 0xFFB0B0B8}));
            on.setTrackTintList(new android.content.res.ColorStateList(states, new int[] {0x88C8102E, 0xFF4A4A54}));
            on.setChecked(!new File(d, "disabled").exists());
            on.setOnCheckedChangeListener((b, checked) -> {
                File off = new File(d, "disabled");
                try {
                    if (checked) off.delete();
                    else new FileOutputStream(off).close();
                    a_.status(name + (checked ? " is on." : " is off.") + " (from the next game start)");
                    if (attire && checked) {  // (two attire mods on one attire: the newer one moves)
                        String clash = attireGuard(null);
                        if (!clash.isEmpty()) a_.status(clash);
                        installed_.post(this::refresh);
                    }
                    if (star) installed_.post(this::refresh);  // (which superstar mods fit)
                } catch (IOException e) {
                    a_.status("Could not change " + name + ": " + e.getMessage());
                }
            });
            Button remove = a_.button("Remove", LauncherActivity.kCard);
            remove.setOnClickListener(v -> {
                if (matchType) {
                    // (no folder means on - a match type is switched off instead)
                    try {
                        new FileOutputStream(new File(d, "disabled")).close();
                        a_.status("Match types can't be removed - " + name + " is switched off instead. (from the next game start)");
                    } catch (IOException e) {
                        a_.status("Could not change " + name + ": " + e.getMessage());
                    }
                    refresh();
                    return;
                }
                deleteTree(d);
                freeSuperstarSlot(d);
                a_.status("Removed " + name + ".");
                refresh();
            });
            controls.addView(on);
            controls.addView(remove);
            a_.row(installed_, name, hint, controls);
        }
    }

    // A background mod's folder has its picture(s): picture.dds or a pack's *.dds.
    static boolean hasPicture(File dir) {
        File[] d = dir.listFiles(f -> f.isFile() && f.getName().toLowerCase().endsWith(".dds"));
        return d != null && d.length > 0;
    }

    // A removed superstar mod gives its id back: its line ("<folder>\t<id>")
    // leaves Mods/Superstars/slots.txt. (The game keeps a mod's id there so
    // saves stay with it; a line left behind held one of its 72 ids for good,
    // and new mods no longer showed once they were all taken.)
    static void freeSuperstarSlot(File folder) {
        File parent = folder.getParentFile();
        if (parent == null || !parent.getName().equals("Superstars")) return;
        File slots = new File(parent, "slots.txt");
        if (!slots.isFile()) return;
        StringBuilder keep = new StringBuilder();
        boolean dropped = false;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(slots), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                int tab = line.lastIndexOf('\t');
                if (tab >= 0 && line.substring(0, tab).equals(folder.getName())) { dropped = true; continue; }
                keep.append(line).append('\n');
            }
        } catch (IOException e) {
            return;
        }
        if (!dropped) return;
        try (OutputStream o = new FileOutputStream(slots)) {
            o.write(keep.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    // The game's number of superstar mod ids: it writes it to
    // Mods/Superstars/limit.txt at each start; before the first start (a fresh
    // install), the same number as in this build.
    static final int SUPERSTAR_IDS = 137;
    static int superstarLimit() {
        File f = new File(superstarsFolder(), "limit.txt");
        if (f.isFile()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                int n = Integer.parseInt(r.readLine().trim());
                if (n > 0 && n < 1000) return n;
            } catch (IOException | NumberFormatException | NullPointerException ignored) {
            }
        }
        return SUPERSTAR_IDS;
    }

    // The game has its ids for superstar mods (src/superstar_mods.cpp), kept per
    // folder in Mods/Superstars/slots.txt. As the game does: a mod that's on
    // and holds an id keeps it; the others wait, in folder order, for the ids
    // left (those of mods switched off or gone included). -> the waiting ones
    // that don't fit.
    static java.util.Set<String> superstarsLeftOut(File[] stars) {
        java.util.Set<String> out = new java.util.HashSet<>();
        if (stars == null) return out;
        java.util.Set<String> held = new java.util.HashSet<>();
        File slots = new File(superstarsFolder(), "slots.txt");
        if (slots.isFile()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(slots), StandardCharsets.UTF_8))) {
                for (String line; (line = r.readLine()) != null; ) {
                    int tab = line.lastIndexOf('\t');
                    if (tab > 0) held.add(line.substring(0, tab));
                }
            } catch (IOException ignored) {
            }
        }
        int holders = 0;
        java.util.List<String> waiting = new java.util.ArrayList<>();
        for (File f : stars) {
            if (new File(f, "disabled").exists()) continue;
            if (held.contains(f.getName())) holders++;
            else waiting.add(f.getName());
        }
        java.util.Collections.sort(waiting);
        for (int i = superstarLimit() - holders; i < waiting.size(); i++) if (i >= 0) out.add(waiting.get(i));
        return out;
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    void pick() {
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] result = new String[1];
            a_.status("Adding the mod…");
            a_.background(() -> {
                try {
                    result[0] = "Installed " + install(uri) + " (from the next game start).";
                } catch (Exception e) {
                    result[0] = "That mod could not be added: " + e.getMessage();
                }
            }, () -> {
                refresh();
                a_.status(result[0]);
            });
        });
    }

    // A picture for the main menu (Backgrounds.java), named after its file.
    void pickPicture() {
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("image/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            String name = "Background";
            try (android.database.Cursor q = a_.getContentResolver().query(uri,
                    new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (q != null && q.moveToFirst() && q.getString(0) != null) name = q.getString(0);
            } catch (Exception ignored) {
            }
            int dot = name.lastIndexOf('.');
            if (dot > 0) name = name.substring(0, dot);
            final String title = name;
            final String[] result = new String[1];
            a_.status("Converting the picture…");
            a_.background(() -> {
                try (InputStream in = a_.getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new IOException("can't read it");
                    Backgrounds.make(in, title, false);
                    result[0] = "Added " + title + ": the main menu shows it after its own 12 pictures (MY WWE -> "
                        + "OPTIONS -> BACKGROUNDS turns pictures on or off).";
                } catch (Exception e) {
                    result[0] = "That picture could not be added: " + e.getMessage();
                }
            }, () -> {
                refresh();
                a_.status(result[0]);
            });
        });
    }

    // A .svrmod: manifest.txt (type=arena or superstar, id=...) and its files -
    // arena.pac and banner.dds, or ch.pac and the song= / movie= files; a
    // match type (type=matchtype) is the manifest alone and comes switched on.
    String install(Uri uri) throws IOException {
        try (InputStream raw = a_.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("can't read it");
            Installed r = install(a_, raw, false, this::askMoves);
            return r.name + (r.note.isEmpty() ? "" : ". " + r.note);
        }
    }

    // The question before a mod with custom moves is installed (launcher/move_warning.c on PC), from the install's
    // worker thread: a Yes / No box on the UI thread, No focused (a pad's A answers No), "Don't warn me again" kept in
    // the launcher's preferences. True: install it.
    interface MoveQuestion {
        boolean install(String name);
    }

    static final String kMoveWarning = "This mod has custom moves (ported or made by hand). Custom moves are "
        + "experimental and most likely broken: they can land wrong, look odd or freeze a match. Install anyway?";

    boolean askMoves(String name) {
        android.content.SharedPreferences prefs = a_.getPreferences(android.content.Context.MODE_PRIVATE);
        if (prefs.getBoolean("move_warning_off", false)) return true;
        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        boolean[] yes = {false};
        a_.runOnUiThread(() -> {
            if (a_.isFinishing()) {
                done.countDown();
                return;
            }
            android.widget.CheckBox quiet = new android.widget.CheckBox(a_);
            quiet.setText("Don't warn me again");
            quiet.setTextColor(LauncherActivity.kText);
            quiet.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
            LinearLayout box = new LinearLayout(a_);
            box.setPadding(a_.dp(20), a_.dp(4), a_.dp(20), 0);
            box.addView(quiet);
            android.app.AlertDialog d = new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle("\"" + name + "\" has custom moves")
                .setMessage(kMoveWarning)
                .setView(box)
                .setPositiveButton("Yes", (dlg, w) -> {
                    yes[0] = true;
                    if (quiet.isChecked()) prefs.edit().putBoolean("move_warning_off", true).apply();
                })
                .setNegativeButton("No", null)
                .setOnDismissListener(dlg -> done.countDown())
                .show();
            d.getButton(android.content.DialogInterface.BUTTON_NEGATIVE).requestFocus();
        });
        try {
            while (!done.await(1, java.util.concurrent.TimeUnit.SECONDS))
                if (a_.isFinishing() || a_.isDestroyed()) return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return yes[0];
    }

    // The player said No (the custom-moves question): not a failure.
    static final class Declined extends IOException {
        Declined(String why) { super(why); }
    }

    // Where a mod went (Mods/<kind>/<id>), whether it was there already, and
    // for a bundled one whether the player's newer copy was kept instead.
    static final class Installed {
        String name, type;
        File dest;
        boolean existed, kept;
        String note = "";  // (an attire mod moved: what the guard said)
    }

    // Installs the .svrmod read from `raw`. A bundled mod (BundledMods) keeps
    // the player's on / off and never replaces a newer version of it.
    static Installed install(android.content.Context ctx, InputStream raw, boolean bundle) throws IOException {
        return install(ctx, raw, bundle, null);
    }

    // ... `ask`: asked before a mod with custom moves is installed (null: only logged).
    static Installed install(android.content.Context ctx, InputStream raw, boolean bundle, MoveQuestion ask) throws IOException {
        Map<String, File> parts = new HashMap<>();
        File stage = new File(ctx.getCacheDir(), "mod_stage");
        deleteTree(stage);
        stage.mkdirs();
        ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw, 1 << 16));
        ZipEntry e;
        while ((e = zip.getNextEntry()) != null) {
            if (e.isDirectory()) continue;
            // (subfolders kept - a superstar mod's moves/motions/ - but
            // nothing outside the mod's folder)
            String n = e.getName().replace('\\', '/');
            String leaf = n.substring(n.lastIndexOf('/') + 1);
            if (leaf.isEmpty() || leaf.startsWith(".") || n.startsWith("/") || n.contains(":")
                    || ("/" + n + "/").contains("/../"))
                continue;
            File out = new File(stage, n);
            File dir = out.getParentFile();
            if (dir != null) dir.mkdirs();
            try (OutputStream o = new FileOutputStream(out)) {
                FileOps.copy(zip, o);
            }
            parts.put(n, out);
        }
        Map<String, String> m = manifest(stage);
        String type = m.containsKey("type") ? m.get("type").toLowerCase() : "arena";
        boolean star = type.equals("superstar");
        boolean signPack = type.equals("signs");
        boolean mediaPack = type.equals("media");
        boolean backPack = type.equals("backstage");
        boolean matchType = type.equals("matchtype");
        boolean pic = type.equals("background");
        boolean attire = type.equals("attire");
        if (attire && !parts.containsKey("attire.pac")) {
            deleteTree(stage);
            throw new IOException("that attire mod has no attire.pac");
        }
        if (pic && !hasPicture(stage)) {  // (picture.dds, or a pack's NN_<name>.dds)
            deleteTree(stage);
            throw new IOException("it has no pictures (.dds)");
        }
        if (!pic && !attire && !signPack && !mediaPack && !matchType && (star ? !parts.containsKey("ch.pac") : !parts.containsKey("arena.pac"))) {
            deleteTree(stage);
            throw new IOException(star ? "it has no ch.pac" : "it has no arena (not a SvR2011 mod?)");
        }
        // (custom moves - a superstar mod's moves/ pack, ported or made by hand: asked first, as on PC; not the port's
        // bundled mods)
        if (!bundle && new File(stage, "moves/pack.txt").isFile()) {
            String nm = m.containsKey("name") ? m.get("name") : m.containsKey("id") ? m.get("id") : "the mod";
            android.util.Log.w("SvR2011", "mods: \"" + nm + "\" has custom moves (ported or made by hand) - experimental "
                + "and most likely broken" + (ask == null ? " (not asked)" : ""));
            if (ask != null && !ask.install(nm)) {
                deleteTree(stage);
                android.util.Log.i("SvR2011", "mods: \"" + nm + "\" not installed (custom moves: No)");
                throw new Declined("it has custom moves (you chose not to install it)");
            }
        }
        String id = m.containsKey("id") ? m.get("id").replaceAll("[^A-Za-z0-9_\\-]", "_") : "";
        if (id.isEmpty()) id = attire ? "attire" : pic ? "background" : matchType ? "matchtype" : backPack ? "backstage" : mediaPack ? "media" : signPack ? "signs" : star ? "superstar" : "arena";
        File dest = new File(attire ? attiresFolder() : pic ? backgroundsFolder() : matchType ? matchTypesFolder() : backPack ? backstageFolder() : mediaPack ? mediaFolder() : signPack ? signsFolder() : star ? superstarsFolder() : arenasFolder(), id);
        Installed r = new Installed();
        r.name = m.containsKey("name") ? m.get("name") : id;
        r.type = type;
        r.dest = dest;
        r.existed = dest.exists();
        boolean off = false;
        if (bundle && r.existed) {
            String mine = manifest(dest).get("version"), theirs = m.get("version");
            if (mine != null && theirs != null && Updater.newer(mine, theirs)) {
                deleteTree(stage);
                r.kept = true;
                return r;
            }
            off = new File(dest, "disabled").exists();
        }
        // The new copy beside the old one first (the cache and the games folder are different storage: a copy, which
        // can stop half-way - a full card), then the old one renamed aside, the new one renamed in (the old one back
        // if that fails) and only then the old one deleted: a failure leaves the installed copy as it was. (It used
        // to be deleted first: a stopped copy left a folder the mods list showed and the game left out - the PC
        // launcher's 2.1.2 bundled-mod reports, mods_tab.c swap_in.)
        String need = attire ? "attire.pac" : pic ? "manifest.txt" : signPack || mediaPack || matchType ? "manifest.txt" : star ? "ch.pac" : "arena.pac";
        File kind = dest.getParentFile(), mods = kind.getParentFile();
        File fresh = new File(mods, ".new-" + kind.getName() + "-" + id), old = new File(mods, ".old-" + kind.getName() + "-" + id);
        deleteTree(fresh);
        deleteTree(old);
        kind.mkdirs();
        try {
            FileOps.copyTree(stage, fresh);
        } catch (IOException copyFailed) {
            deleteTree(fresh);
            throw new IOException("can't write to the game folder (" + copyFailed.getMessage() + "); the installed copy is unchanged");
        } finally {
            deleteTree(stage);
        }
        if (!new File(fresh, need).isFile()) {
            deleteTree(fresh);
            throw new IOException("can't write to the game folder; the installed copy is unchanged");
        }
        if (dest.exists() && !dest.renameTo(old)) {
            deleteTree(fresh);
            throw new IOException("can't replace the installed copy (it is unchanged)");
        }
        if (!fresh.renameTo(dest)) {
            if (old.exists()) old.renameTo(dest);
            deleteTree(fresh);
            throw new IOException("can't move the new copy in (the installed copy is unchanged)");
        }
        deleteTree(old);
        if (off) new FileOutputStream(new File(dest, "disabled")).close();
        if (attire) {
            try (OutputStream o = new FileOutputStream(new File(dest, ".installed"))) {  // (the install order: the newer moves)
                o.write(Long.toString(System.currentTimeMillis()).getBytes(StandardCharsets.UTF_8));
            }
            r.note = attireGuard(dest);
        }
        if (needsNewer(ctx, m.get("needs")))
            r.note = ("It needs game version " + m.get("needs").trim() + " or newer - this one is " + gameVersion(ctx)
                + ": update the game first. " + r.note).trim();
        return r;
    }

    // ---- Two attire mods on one attire (the PC launcher's mods_tab.c mods_attire_guard; the game's
    // LoadAttireMods). A superstar's attire is free when their own pac (pac/ch/chNNN.pac, EMD entries
    // "%06d%d2") doesn't have it and no switched-on attire mod asks for it. Of two switched-on mods on one
    // attire the one installed first keeps it (`fresh`, just installed, is the newer); the other moves to the
    // next free attire (1-4): its manifest's attire= rewritten, with a "# moved" line. With none free the
    // player is told to switch one off (the game leaves it out). -> what it did / says ("" when nothing).
    static final class AttireMod {
        File dir;
        String name;
        int id, attire;  // attire 1-4, 0 = new
        long born;       // install time (.installed, else the folder's time)
        boolean fresh;
    }

    static String starName(String id) {
        if (id != null) for (String[] s : MoviesPage.kStars) if (s[0].equals(id.trim())) return s[1];
        return "superstar " + id;
    }

    // Attires 1-4 (bits 0-3) the superstar's own pacs have, plus bit 8 when one has other model kinds for attire 1:
    // the entrance gear superstars (Undertaker, CM Punk, Cena, Shawn Michaels, Morrison) - a new attire for them
    // doesn't load in the game yet (src/superstar_mods.cpp ExtraKinds), so only their own can be replaced. Own pacs:
    // the game's pac/ch/chNNN.pac and the installed DLC's pac/ch/ch<id>_dlcNN.pac
    // (UserData/0000000000000000/5451085D/00000002/<package>: a DLC superstar's own, ids 51-58, and DLC attires of
    // disc superstars) - the game counts both.
    static final int kGear = 0x100;

    static int ownAttires(int id) {
        int mask = 0;
        if (id >= 100)
            mask |= pacAttires(new File(new File(new File(InstallActivity.gameFolder(), "pac"), "ch"),
                String.format(java.util.Locale.ROOT, "ch%03d.pac", id)), id);
        File[] pkgs = new File(InstallActivity.gameFolder(), "UserData/0000000000000000/5451085D/00000002").listFiles(File::isDirectory);
        if (pkgs != null)
            for (File pkg : pkgs) {
                String prefix = "ch" + id + "_dlc";
                File[] pacs = new File(pkg, "pac/ch").listFiles(f -> f.isFile() && f.getName().toLowerCase(java.util.Locale.ROOT).startsWith(prefix)
                    && f.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".pac"));
                if (pacs != null) for (File f : pacs) mask |= pacAttires(f, id);
            }
        return mask;
    }

    // A DLC superstar the player has installed (ids below 100: only DLC model pacs).
    static boolean dlcSuperstar(int id) {
        return id > 0 && id < 100 && (ownAttires(id) & 0xF) != 0;
    }

    static int pacAttires(File pac, int id) {
        byte[] d = new byte[0x4000];
        try (FileInputStream in = new FileInputStream(pac)) {
            int n = 0, r;
            while (n < d.length && (r = in.read(d, n, d.length - n)) > 0) n += r;
            if (n < d.length) return 0;
        } catch (IOException e) {
            return 0;
        }
        if (d[0] != 'E' || d[1] != 'P' || d[2] != 'K' || d[3] != '8') return 0;
        byte[] want = String.format(java.util.Locale.ROOT, "%06d", id).getBytes(StandardCharsets.US_ASCII);
        int mask = 0;
        for (int p = 0x800; p + 12 <= d.length; ) {
            if (d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 0) break;
            boolean emd = d[p] == 'E' && d[p + 1] == 'M' && d[p + 2] == 'D' && d[p + 3] == ' ';
            int entries = ((d[p + 4] & 0xFF) | (d[p + 5] & 0xFF) << 8) / 4;
            p += 12;
            for (int k = 0; k < entries && p + 16 <= d.length; k++, p += 16) {
                boolean same = emd;
                for (int i = 0; i < 6 && same; i++) same = d[p + i] == want[i];
                if (same && d[p + 6] >= '0' && d[p + 6] <= '3') {
                    if (d[p + 7] == '2') mask |= 1 << (d[p + 6] - '0');
                    else if (d[p + 6] == '0' && d[p + 7] >= '0' && d[p + 7] <= '9') mask |= kGear;
                }
            }
        }
        return mask;
    }

    static boolean older(AttireMod a, AttireMod b) {
        if (a.fresh != b.fresh) return b.fresh;
        if (a.born != b.born) return a.born < b.born;
        return a.dir.getName().compareTo(b.dir.getName()) < 0;
    }

    static boolean setAttire(File dir, int from, int to, String keeper) {
        File man = new File(dir, "manifest.txt"), tmp = new File(dir, "manifest.new");
        StringBuilder out = new StringBuilder();
        boolean done = false;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(man), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (!done && line.startsWith("attire=")) {
                    out.append("attire=").append(to).append('\n').append("# moved from attire ").append(from)
                        .append(" by the launcher (").append(keeper).append(" has attire ").append(from).append(")\n");
                    done = true;
                } else out.append(line).append('\n');
            }
        } catch (IOException e) {
            return false;
        }
        if (!done) return false;
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(out.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(man) || (man.delete() && tmp.renameTo(man));
    }

    static String attireGuard(File fresh) {
        File[] dirs = attiresFolder().listFiles(f -> f.isDirectory() && !new File(f, "disabled").exists()
            && new File(f, "manifest.txt").isFile());
        if (dirs == null) return "";
        java.util.List<AttireMod> mods = new java.util.ArrayList<>();
        for (File d : dirs) {
            Map<String, String> m = manifest(d);
            if (!"attire".equals(m.get("type"))) continue;
            AttireMod a = new AttireMod();
            a.dir = d;
            a.name = m.containsKey("name") ? m.get("name") : d.getName();
            try {
                a.id = Integer.parseInt(m.containsKey("superstar") ? m.get("superstar").trim() : "0");
                String w = m.containsKey("attire") ? m.get("attire").trim() : "";
                a.attire = w.equals("new") ? 0 : Integer.parseInt(w);
            } catch (NumberFormatException e) {
                continue;
            }
            if (((a.id < 100 || a.id > 321) && !dlcSuperstar(a.id)) || a.attire < 0 || a.attire > 4) continue;
            a.born = d.lastModified();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(new File(d, ".installed")),
                    StandardCharsets.UTF_8))) {
                a.born = Long.parseLong(r.readLine().trim());
            } catch (Exception ignored) {
            }
            a.fresh = fresh != null && d.getAbsolutePath().equals(fresh.getAbsolutePath());
            mods.add(a);
        }
        StringBuilder said = new StringBuilder();
        java.util.Set<Integer> stars = new java.util.TreeSet<>();
        for (AttireMod a : mods) stars.add(a.id);
        for (int id : stars) {
            int own = ownAttires(id), used = own & 0xF, news = 0;
            boolean gear = (own & kGear) != 0;
            for (AttireMod a : mods) {
                if (a.id != id || a.attire == 0) continue;
                boolean best = true;
                for (AttireMod o : mods) if (o != a && o.id == id && o.attire == a.attire && older(o, a)) best = false;
                if (best) used |= 1 << (a.attire - 1);
            }
            if (gear) used = 0xF;  // (no new attires for them: nothing free to move to)
            for (AttireMod a : mods) {
                if (a.id != id) continue;
                if (a.attire == 0) {
                    news++;
                    continue;
                }
                if (gear && (own >> (a.attire - 1) & 1) == 0) {  // (a new attire for a gear superstar: left out by the game)
                    said.append('"').append(a.name).append("\" gives ").append(starName("" + id)).append(" an attire ").append(a.attire)
                        .append(", but they wear entrance gear and new attires for them don't load in the game yet: the game leaves it out.\n");
                    continue;
                }
                AttireMod keeper = null;
                for (AttireMod o : mods)
                    if (o != a && o.id == id && o.attire == a.attire && older(o, a) && (keeper == null || older(o, keeper))) keeper = o;
                if (keeper == null) continue;
                int to = 0;
                for (int k = 0; k < 4 && to == 0; k++) if ((used >> k & 1) == 0) to = k + 1;
                String both = "\"" + keeper.name + "\" and \"" + a.name + "\" both use " + starName("" + id) + "'s attire " + a.attire;
                if (to != 0 && setAttire(a.dir, a.attire, to, keeper.name)) {
                    used |= 1 << (to - 1);
                    said.append(both).append(": \"").append(a.name).append("\" moved to attire ").append(to).append(".\n");
                    a.attire = to;
                } else if (gear) {
                    said.append(both).append(". ").append(starName("" + id)).append(" wears entrance gear, and new attires for them don't load in")
                        .append(" the game yet, so neither can move: switch one off, or the game leaves \"").append(a.name).append("\" out.\n");
                } else {
                    said.append(both).append(" and there is no free attire to move one to (4 at most): switch one off, or the game leaves \"")
                        .append(a.name).append("\" out.\n");
                }
            }
            for (int k = 0; k < 4; k++) if ((used >> k & 1) == 0 && news > 0) news--;
            if (news > 0 && gear)
                said.append(starName("" + id)).append(" wears entrance gear: new attires for them don't load in the game yet, so ")
                    .append(news).append(" attire=new mod").append(news == 1 ? " stays" : "s stay").append(" out: switch ")
                    .append(news == 1 ? "it" : "them").append(" off.\n");
            else if (news > 0)
                said.append(starName("" + id)).append(" has no free attire for ").append(news).append(" attire=new mod")
                    .append(news == 1 ? "" : "s").append(" (4 at most): switch some off.\n");
        }
        return said.toString().trim();
    }

}
