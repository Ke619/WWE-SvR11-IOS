// WWE SmackDown vs. Raw 2011 - the launcher's Saves tab on the phone (as the
// PC launcher's): the save files in the game folder's Saves, with back up /
// restore (SaveBackups/<date>), export / import through the system picker,
// and delete (to SaveBackups/<date> (deleted): a phone has no Recycle Bin).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;

final class SavesPage {
    private final LauncherActivity a_;
    private LinearLayout list_;
    private final List<CheckBox> checks_ = new ArrayList<>();
    private final List<File> files_ = new ArrayList<>();

    SavesPage(LauncherActivity a) { a_ = a; }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Your saves: the main save, Created Superstars, Paint Tool logos, replays and "
            + "highlight reels. Back them up, move them to or from another device, or remove some.", 14,
            LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);

        list_ = a_.card(c, "Save files");

        Button backup = a_.button("Back up all", LauncherActivity.kRed);
        backup.setOnClickListener(v -> backupAll());
        Button restore = a_.button("Restore backup…", LauncherActivity.kCard);
        restore.setOnClickListener(v -> restore());
        c.addView(a_.pair(backup, restore), a_.fullWidth(16));
        Button export = a_.button("Export selected…", LauncherActivity.kCard);
        export.setOnClickListener(v -> exportSelected());
        Button imp = a_.button("Import…", LauncherActivity.kCard);
        imp.setOnClickListener(v -> importFiles());
        c.addView(a_.pair(export, imp), a_.fullWidth(10));
        Button importCaw = a_.button("Import Superstar…", LauncherActivity.kCard);
        importCaw.setOnClickListener(v -> importSuperstars());
        Button delete = a_.button("Delete selected", LauncherActivity.kCard);
        delete.setOnClickListener(v -> deleteSelected());
        c.addView(a_.pair(importCaw, delete), a_.fullWidth(10));
        Button clean = a_.button("Clean save of unused mods…", LauncherActivity.kCard);
        clean.setOnClickListener(v -> cleanSave());
        c.addView(clean, a_.fullWidth(10));
        TextView cawHelp = a_.text("Import Superstar: pick someone's Created Superstar files (.cas, from a PC or an "
            + "Xbox 360 save) - add their SaveData.dat too to keep each Superstar's details. Each goes into a free "
            + "slot, and its logos into free Paint Tool slots.", 12, LauncherActivity.kDim);
        cawHelp.setPadding(a_.dp(4), a_.dp(8), a_.dp(4), 0);
        c.addView(cawHelp);
        TextView where = a_.text("Saves: games/" + InstallActivity.kFolderName + "/Saves — backups in "
            + "SaveBackups beside it.", 12, LauncherActivity.kDim);
        where.setPadding(a_.dp(4), a_.dp(12), a_.dp(4), 0);
        c.addView(where);
        refresh();
        return c;
    }

    static String kind(String name) {
        String n = name.toLowerCase();
        if (n.equals("savedata.dat")) return "Main save";
        if (n.endsWith(".cas")) return "Created Superstar";
        if (n.endsWith(".pt")) return "Paint Tool logos";
        if (n.endsWith(".rec")) return "Replay";
        if (n.endsWith(".scn")) return "Highlight reel";
        return "Created content";
    }

    // The name the game shows (the content header's UTF-16BE display name), or null.
    static String displayName(File save) {
        File header = new File(new File(save.getParentFile(), ".info"), save.getName() + ".header");
        if (!header.isFile() || header.length() < 264) return null;
        try {
            byte[] b = new byte[256];
            try (java.io.InputStream in = new java.io.FileInputStream(header)) {
                if (in.skip(8) != 8 || in.read(b) != b.length) return null;
            }
            int len = 0;
            while (len + 1 < b.length && (b[len] != 0 || b[len + 1] != 0)) len += 2;
            String s = new String(b, 0, len, StandardCharsets.UTF_16BE).trim();
            return s.isEmpty() ? null : s;
        } catch (IOException e) {
            return null;
        }
    }

    void refresh() {
        list_.removeAllViews();
        checks_.clear();
        files_.clear();
        File[] all = FileOps.saves().listFiles();
        if (all != null) {
            Arrays.sort(all, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
            for (File f : all) {
                if (!FileOps.isSave(f)) continue;
                CheckBox cb = new CheckBox(a_);
                String shown = displayName(f);
                cb.setText((shown != null ? shown : f.getName()) + "\n" + kind(f.getName()) + " · "
                    + FileOps.human(FileOps.size(f)) + " · "
                    + DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(new Date(f.lastModified())));
                cb.setTextColor(LauncherActivity.kText);
                cb.setTextSize(15);
                cb.setPadding(a_.dp(8), a_.dp(10), 0, a_.dp(10));
                cb.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
                list_.addView(cb);
                checks_.add(cb);
                files_.add(f);
            }
        }
        if (files_.isEmpty()) {
            TextView none = a_.text("No saves yet: the game makes them when it saves.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            list_.addView(none);
        }
    }

    List<File> selected() {
        List<File> s = new ArrayList<>();
        for (int i = 0; i < checks_.size(); i++) if (checks_.get(i).isChecked()) s.add(files_.get(i));
        return s;
    }

    void backupAll() {
        if (!a_.gameClosed()) return;
        a_.status("Backing up…");
        final String[] result = new String[1];
        a_.background(() -> {
            try {
                File dest = FileOps.backup("");
                result[0] = dest == null ? "Nothing was backed up: there are no saves yet."
                                         : "Backed up to SaveBackups/" + dest.getName() + ".";
            } catch (IOException e) {
                result[0] = "The backup failed: " + e.getMessage();
            }
        }, () -> a_.status(result[0]));
    }

    void restore() {
        if (!a_.gameClosed()) return;
        File[] dirs = FileOps.backups().listFiles(File::isDirectory);
        if (dirs == null || dirs.length == 0) {
            a_.status("There are no backups yet (Back up all makes one).");
            return;
        }
        Arrays.sort(dirs, (x, y) -> y.getName().compareTo(x.getName()));
        String[] names = new String[dirs.length];
        for (int i = 0; i < dirs.length; i++) names[i] = dirs[i].getName() + "  (" + FileOps.countSaves(dirs[i]) + " saves)";
        new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Restore a backup")
            .setItems(names, (d, which) -> {
                File from = dirs[which];
                int n = FileOps.countSaves(from);
                a_.confirm("Replace your saves with the " + n + " in " + from.getName()
                    + "? Your current saves are backed up first.", "Restore", () -> restoreFrom(from));
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    void restoreFrom(File from) {
        final String[] result = new String[1];
        a_.status("Restoring…");
        a_.background(() -> {
            try {
                FileOps.backup(" (before restore)");
                File saves = FileOps.saves();
                File[] old = saves.listFiles();
                if (old != null) for (File f : old) if (!f.getName().equals(".mount")) FileOps.deleteTree(f);
                FileOps.copyTree(from, saves);
                result[0] = "Restored " + from.getName() + " (your previous saves are in SaveBackups).";
            } catch (IOException e) {
                result[0] = "Restoring failed: " + e.getMessage();
            }
        }, () -> {
            a_.status(result[0]);
            refresh();
        });
    }

    void exportSelected() {
        List<File> sel = selected();
        if (sel.isEmpty()) {
            a_.status("Tick the saves to export first.");
            return;
        }
        a_.startForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri tree = data.getData();
            final String[] result = new String[1];
            a_.background(() -> {
                int done = 0, skipped = 0;
                try {
                    Uri root = FileOps.treeRoot(tree);
                    for (File f : sel) {
                        if (FileOps.copyOut(a_.getContentResolver(), tree, root, f)) done++;
                        else skipped++;
                    }
                    result[0] = "Exported " + done + (skipped > 0 ? " (some were already there and were skipped)." : ".");
                } catch (Exception e) {
                    result[0] = "Exporting failed: " + e.getMessage();
                }
            }, () -> a_.status(result[0]));
        });
    }

    void importFiles() {
        if (!a_.gameClosed()) return;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null) return;
            List<Uri> uris = new ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (uris.isEmpty()) return;
            List<String> names = new ArrayList<>();
            int replacing = 0;
            for (Uri u : uris) {
                String n = FileOps.displayName(a_.getContentResolver(), u);
                names.add(n);
                if (new File(FileOps.saves(), n).exists()) replacing++;
            }
            Runnable go = () -> doImport(uris, names);
            if (replacing > 0) {
                a_.confirm(replacing + " of the " + uris.size() + " files replace saves you have. Replace them? "
                    + "Your current saves are backed up first.", "Replace", go);
            } else {
                go.run();
            }
        });
    }

    void doImport(List<Uri> uris, List<String> names) {
        final String[] result = new String[1];
        a_.status("Importing…");
        a_.background(() -> {
            try {
                boolean replacing = false;
                for (String n : names) replacing |= new File(FileOps.saves(), n).exists();
                if (replacing) FileOps.backup(" (before import)");
                for (int i = 0; i < uris.size(); i++) {
                    FileOps.copyIn(a_.getContentResolver(), uris.get(i), new File(FileOps.saves(), names.get(i)));
                }
                result[0] = "Imported " + uris.size() + " file" + (uris.size() == 1 ? "" : "s") + ".";
            } catch (IOException e) {
                result[0] = "Importing failed: " + e.getMessage();
            }
        }, () -> {
            a_.status(result[0]);
            refresh();
        });
    }

    // Someone else's Created Superstars (CawImport): the .cas files, with their main save and
    // extra logo files if picked too.
    void importSuperstars() {
        if (!a_.gameClosed()) return;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null) return;
            List<Uri> uris = new ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (uris.isEmpty()) return;
            final String[] result = new String[1];
            a_.status("Importing…");
            a_.background(() -> {
                try {
                    List<CawImport.Picked> picked = new ArrayList<>();
                    for (Uri u : uris) {
                        try (java.io.InputStream in = a_.getContentResolver().openInputStream(u)) {
                            if (in == null) continue;
                            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
                            byte[] buf = new byte[1 << 16];
                            for (int n; (n = in.read(buf)) > 0; ) {
                                out.write(buf, 0, n);
                                if (out.size() > 64 * 1024 * 1024) throw new IOException("a picked file is too big");
                            }
                            picked.add(new CawImport.Picked(FileOps.displayName(a_.getContentResolver(), u), out.toByteArray()));
                        }
                    }
                    FileOps.backup(" (before Superstar import)");
                    result[0] = CawImport.run(FileOps.saves(), picked) + " (Your saves were backed up first.)";
                } catch (IOException | OutOfMemoryError e) {
                    result[0] = "Importing failed: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }

    // "Clean save of unused mods…": what the save keeps for superstar mods that aren't installed or are turned off
    // (SaveCleaner), shown, then - after a backup of the saves - taken out.
    void cleanSave() {
        if (!a_.gameClosed()) return;
        final File saves = FileOps.saves(), mods = new File(InstallActivity.gameFolder(), "Mods/Superstars");
        final List<SaveCleaner.Item> items = new ArrayList<>();
        final String[] failed = new String[1];
        a_.status("Looking through the save…");
        a_.background(() -> {
            try {
                items.addAll(SaveCleaner.scan(saves, mods));
            } catch (SaveCleaner.Failure e) {
                failed[0] = e.getMessage();
            }
        }, () -> {
            if (failed[0] != null) {
                a_.status(failed[0]);
                return;
            }
            if (items.isEmpty()) {
                a_.status("The save has nothing of mods that aren't installed or are turned off.");
                return;
            }
            int off = 0;
            StringBuilder s = new StringBuilder();
            for (SaveCleaner.Item it : items)
                if (it.off && off++ < 25) s.append("  ").append(it.name).append('\n');
            if (off > 25) s.append("  … and ").append(off - 25).append(" more\n");
            if (off == 0) {
                confirmClean(items, false);
                return;
            }
            android.app.AlertDialog.Builder b =
                new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                    .setTitle("Turned-off mods")
                    .setMessage(off + " of these mods are installed but turned off:\n" + s
                        + "\nTake their data out of the save too?")
                    .setPositiveButton("Include them", (dl, w) -> confirmClean(items, true))
                    .setNegativeButton("Cancel", null);
            if (off < items.size()) b.setNeutralButton("Only uninstalled", (dl, w) -> confirmClean(items, false));
            b.show();
        });
    }

    void confirmClean(List<SaveCleaner.Item> items, boolean withOff) {
        StringBuilder s = new StringBuilder("This removes data for mods that aren't installed or are turned off. If "
            + "you turn them back on, their edits are gone. A backup is made first.\n\nTaken out:\n");
        int shown = 0;
        for (SaveCleaner.Item it : items) {
            if (it.off && !withOff) continue;
            if (shown++ >= 30) continue;
            s.append("  ").append(it.name);
            if (!it.folder.isEmpty() && !it.folder.equalsIgnoreCase(it.name)) s.append(" - ").append(it.folder);
            s.append(it.off ? ", turned off" : "").append(" (").append(it.what()).append(")\n");
        }
        if (shown > 30) s.append("  … and ").append(shown - 30).append(" more\n");
        if (shown == 0) return;
        s.append('\n').append(shown).append(shown == 1 ? " mod" : " mods").append(". The game's own superstars, your "
            + "Created Superstars and the mods that are on stay as they are.");
        a_.confirm(s.toString(), "Clean", () -> {
            final String[] result = new String[1];
            a_.status("Cleaning…");
            a_.background(() -> {
                try {
                    File dest = FileOps.backup(" (before cleaning)");
                    if (dest == null) throw new IOException("the saves couldn't be backed up");
                    int n = SaveCleaner.apply(FileOps.saves(), items, withOff);
                    result[0] = "Took out " + n + (n == 1 ? " mod's" : " mods'") + " data. Backup: SaveBackups/"
                        + dest.getName() + " (Restore backup… puts it back).";
                } catch (SaveCleaner.Failure | IOException e) {
                    result[0] = "Not cleaned: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }

    void deleteSelected() {
        if (!a_.gameClosed()) return;
        List<File> sel = selected();
        if (sel.isEmpty()) {
            a_.status("Tick the saves to delete first.");
            return;
        }
        boolean main = false;
        for (File f : sel) main |= f.getName().equalsIgnoreCase("SaveData.dat");
        String msg = "Delete " + sel.size() + " save" + (sel.size() == 1 ? "" : "s") + "? They're moved to "
            + "SaveBackups, so you can bring them back with Restore."
            + (main ? "\n\nThe main save (SaveData.dat) is included: the game starts over." : "");
        a_.confirm(msg, "Delete", () -> {
            final String[] result = new String[1];
            a_.background(() -> {
                try {
                    File dest = new File(FileOps.backups(), FileOps.stamp() + " (deleted)");
                    File info = new File(FileOps.saves(), ".info");
                    for (File f : sel) {
                        FileOps.move(f, new File(dest, f.getName()));
                        for (String ext : new String[] {".header", ".file", ".png"}) {
                            File side = new File(info, f.getName() + ext);
                            if (side.exists()) FileOps.move(side, new File(new File(dest, ".info"), side.getName()));
                        }
                    }
                    result[0] = "Deleted " + sel.size() + " (kept in SaveBackups/" + dest.getName() + ").";
                } catch (IOException e) {
                    result[0] = "Deleting failed: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }
}
