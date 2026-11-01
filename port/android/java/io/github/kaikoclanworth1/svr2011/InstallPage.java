// WWE SmackDown vs. Raw 2011 - the launcher's Install tab on the phone: the
// game from the Xbox 360 disc image (ISO / XISO, read here: XDVDFS, as the PC
// launcher reads it) or from the PC launcher's game package (a zip). The disc
// tree is copied 1:1 into the game folder, resumably (files already there at
// the same size are kept), each file through <name>.part.

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class InstallPage {
    static final long kSector = 2048;
    static final long[] kBases = {0, 0x18300000L, 0xFD90000L, 0x2080000L};  // XISO, XGD2, XGD1, XGD3

    private final LauncherActivity a_;
    private ProgressBar progress_;
    private TextView state_;
    private volatile boolean cancel_;
    private boolean busy_;
    private Button cancelButton_;

    InstallPage(LauncherActivity a) { a_ = a; }

    View view() {
        LinearLayout c = a_.column();
        LinearLayout st = a_.card(c, "Game");
        state_ = a_.text("", 15, LauncherActivity.kText);
        state_.setPadding(0, a_.dp(12), 0, a_.dp(12));
        st.addView(state_);
        TextView about = a_.text("Install from your own copy of the game: its Xbox 360 disc image (ISO), or the "
            + "game package the PC launcher makes (Android Install → Create APK Package). Installing again "
            + "keeps the files already there.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(12), a_.dp(4), a_.dp(4));
        c.addView(about);
        Button iso = a_.button("Install from disc image…", LauncherActivity.kRed);
        iso.setOnClickListener(v -> pickImage());
        c.addView(iso, a_.fullWidth(14));
        Button pkg = a_.button("Install from game package…", LauncherActivity.kCard);
        pkg.setOnClickListener(v -> pickPackage());
        c.addView(pkg, a_.fullWidth(10));
        progress_ = new ProgressBar(a_, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setMax(1000);
        progress_.setVisibility(View.GONE);
        progress_.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        c.addView(progress_, a_.fullWidth(16));
        cancelButton_ = a_.button("Cancel", LauncherActivity.kCard);
        cancelButton_.setOnClickListener(v -> cancel_ = true);
        cancelButton_.setVisibility(View.GONE);
        c.addView(cancelButton_, a_.fullWidth(10));
        refresh();
        return c;
    }

    void refresh() {
        state_.setText(InstallActivity.installed()
            ? "Installed in games/" + InstallActivity.kFolderName + " (" + FileOps.human(free()) + " free)."
            : "Not installed yet.");
    }

    static long free() {
        File dir = InstallActivity.gameFolder();
        dir.mkdirs();
        return new StatFs(dir.getPath()).getAvailableBytes();
    }

    boolean start() {
        if (busy_) {
            a_.status("An install is already running.");
            return false;
        }
        if (GameActivity.running) {
            a_.status("Close the game before installing it again.");
            return false;
        }
        return true;
    }

    // ── disc image ───────────────────────────────────────────────────────

    // One file of the disc.
    static final class Entry {
        String path;
        long sector, size;
        boolean dir;
    }

    // An XDVDFS image read through a seekable channel.
    static final class Disc {
        FileChannel ch;
        long base;
        final List<Entry> entries = new ArrayList<>();

        byte[] read(long at, int n) throws IOException {
            ByteBuffer b = ByteBuffer.allocate(n);
            while (b.hasRemaining()) {
                if (ch.read(b, at + b.position()) <= 0) throw new IOException("the image ends early");
            }
            return b.array();
        }

        static int le16(byte[] b, int at) { return (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8; }

        static long le32(byte[] b, int at) {
            return (b[at] & 0xFFL) | (b[at + 1] & 0xFFL) << 8 | (b[at + 2] & 0xFFL) << 16 | (b[at + 3] & 0xFFL) << 24;
        }

        // Finds the partition (the magic at sector 32) and lists the tree.
        String open(FileChannel channel) throws IOException {
            ch = channel;
            long size = ch.size();
            for (long b : kBases) {
                long at = b + 32 * kSector;
                if (at + 32 > size) continue;
                byte[] vd = read(at, 28);
                if (new String(vd, 0, 20, StandardCharsets.US_ASCII).equals("MICROSOFT*XBOX*MEDIA")) {
                    base = b;
                    dir(le32(vd, 20), le32(vd, 24), "", 0);
                    return null;
                }
            }
            return "this isn't an Xbox 360 disc image (no XDVDFS file system found)";
        }

        // A directory table, in stored order (as the PC launcher walks it).
        void dir(long sector, long size, String prefix, int depth) throws IOException {
            if (depth > 32 || size <= 0 || size > (64 << 20)) return;
            byte[] t = read(base + sector * kSector, (int) size);
            int at = 0;
            while (at + 14 <= t.length) {
                int left = le16(t, at);
                int nameLen = t[at + 13] & 0xFF;
                if (left == 0xFFFF || nameLen == 0) {
                    at = (int) ((at / kSector + 1) * kSector);
                    continue;
                }
                if (at + 14 + nameLen > t.length) break;
                String name = new String(t, at + 14, nameLen, StandardCharsets.ISO_8859_1);
                Entry e = new Entry();
                e.sector = le32(t, at + 4);
                e.size = le32(t, at + 8);
                e.dir = (t[at + 12] & 0x10) != 0;
                if (!name.contains("\\") && !name.contains("/") && !name.contains(":") && !name.equals(".")
                    && !name.equals("..")) {
                    e.path = prefix + name;
                    entries.add(e);
                    if (e.dir) dir(e.sector, e.size, e.path + "/", depth + 1);
                }
                at = (at + 14 + nameLen + 3) & ~3;
            }
        }

        Entry find(String path) {
            for (Entry e : entries) if (e.path.equalsIgnoreCase(path)) return e;
            return null;
        }

        // default.xex: "XEX2" and the execution info's title ID (header 0x00040006).
        String check() throws IOException {
            Entry xex = find("default.xex");
            if (xex == null || xex.dir) return "the image has no default.xex";
            byte[] h = read(base + xex.sector * kSector, 4096);
            if (!new String(h, 0, 4, StandardCharsets.US_ASCII).equals("XEX2")) return "default.xex can't be read";
            int count = DlcPage.be32(h, 20);
            for (int i = 0; i < count && 24 + i * 8 + 8 <= h.length; i++) {
                if (DlcPage.be32(h, 24 + i * 8) == 0x00040006) {
                    int off = DlcPage.be32(h, 24 + i * 8 + 4);
                    byte[] info = read(base + xex.sector * kSector + off, 24);
                    int id = DlcPage.be32(info, 12);
                    if (id != DlcPage.kTitleId) return String.format("this is another game (its title ID is %08X)", id);
                    return supportedRelease(xex);
                }
            }
            return "default.xex has no title ID";
        }

        // The release the port was made from (launcher/game_files_crc.inc: default.xex's size and CRC-32, the US /
        // Europe disc): the port is that xex recompiled, so another release's - the Japanese disc (2.1.8 report
        // 6fd821) - can't run.
        static final long kXexSize = 15020032L;
        static final long kXexCrc = 0xD2719517L;

        String supportedRelease(Entry xex) throws IOException {
            final String other = "this is another release of the game (for example the Japanese disc): the port only "
                + "runs the US / Europe Xbox 360 release (title 5451085D)";
            if (xex.size != kXexSize) return other;
            java.util.zip.CRC32 crc = new java.util.zip.CRC32();
            for (long at = 0; at < xex.size; ) {
                int n = (int) Math.min(4L << 20, xex.size - at);
                crc.update(read(base + xex.sector * kSector + at, n));
                at += n;
            }
            return crc.getValue() == kXexCrc ? null : other;
        }
    }

    void pickImage() {
        if (!start()) return;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            installImage(data.getData());
        });
    }

    void installImage(Uri uri) {
        busy_ = true;
        cancel_ = false;
        progress_.setProgress(0);
        progress_.setVisibility(View.VISIBLE);
            cancelButton_.setVisibility(View.VISIBLE);
        a_.status("Reading the disc image…");
        final String[] result = new String[1];
        a_.background(() -> result[0] = copyImage(uri), () -> {
            busy_ = false;
            progress_.setVisibility(View.GONE);
                cancelButton_.setVisibility(View.GONE);
            a_.status(result[0]);
            refresh();
            // (the disc has the game only: the port's own mods are a download)
            if (InstallActivity.installed() && result[0].startsWith("Installed")) BundledMods.offer(a_, false, null);
        });
    }

    String copyImage(Uri uri) {
        File dest = InstallActivity.gameFolder();
        try (ParcelFileDescriptor pfd = a_.getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd == null) return "The disc image can't be opened.";
            try (FileInputStream fin = new FileInputStream(pfd.getFileDescriptor())) {
                Disc disc = new Disc();
                String err = disc.open(fin.getChannel());
                if (err == null) err = disc.check();
                if (err != null) return "Can't install: " + err + ".";
                long total = 0, need = 0;
                List<Entry> files = new ArrayList<>();
                for (Entry e : disc.entries) {
                    if (e.dir) {
                        new File(dest, e.path).mkdirs();
                        continue;
                    }
                    files.add(e);
                    total += e.size;
                    File f = new File(dest, e.path);
                    if (!(f.isFile() && f.length() == e.size)) need += e.size;
                }
                if (need + (64 << 20) > free()) {
                    return "Not enough free space: " + FileOps.human(need + (64 << 20)) + " needed, "
                        + FileOps.human(free()) + " free.";
                }
                files.sort((x, y) -> Long.compare(x.sector, y.sector));  // (in disc order: faster)
                long done = 0, start = System.nanoTime();
                byte[] buffer = new byte[8 << 20];
                // default.xex last, so an interrupted install is redone.
                Entry xex = disc.find("default.xex");
                files.remove(xex);
                files.add(xex);
                for (Entry e : files) {
                    if (cancel_) return "Stopped. Install again to carry on where it stopped.";
                    File f = new File(dest, e.path);
                    if (f.isFile() && f.length() == e.size) {
                        done += e.size;
                        continue;
                    }
                    f.getParentFile().mkdirs();
                    File part = new File(f.getPath() + ".part");
                    try (OutputStream out = new FileOutputStream(part)) {
                        long at = disc.base + e.sector * kSector, left = e.size;
                        while (left > 0) {
                            if (cancel_) break;
                            int n = (int) Math.min(buffer.length, left);
                            ByteBuffer b = ByteBuffer.wrap(buffer, 0, n);
                            while (b.hasRemaining()) {
                                if (disc.ch.read(b, at + b.position()) <= 0) throw new IOException("the image ends early");
                            }
                            out.write(buffer, 0, n);
                            at += n;
                            left -= n;
                            done += n;
                            report(done, total, start, e.path);
                        }
                    }
                    if (cancel_) {
                        part.delete();
                        return "Stopped. Install again to carry on where it stopped.";
                    }
                    if (f.exists()) f.delete();
                    if (!part.renameTo(f)) throw new IOException("can't write " + e.path);
                }
                Shaders.install(a_);
                return "Installed (" + files.size() + " files, " + FileOps.human(total) + "). Press Play.";
            }
        } catch (Exception e) {
            return "Installing failed: " + e.getMessage();
        }
    }

    void report(long done, long total, long start, String what) {
        final int p = total > 0 ? (int) (done * 1000 / total) : 0;
        double secs = (System.nanoTime() - start) / 1e9;
        double rate = secs > 0.5 ? done / secs : 0;
        String eta = rate > 0 ? String.format(" · %d s left", (long) ((total - done) / rate)) : "";
        String s = String.format("Installing… %d%% · %s of %s%s", p / 10, FileOps.human(done),
            FileOps.human(total), eta);
        a_.runOnUiThread(() -> {
            progress_.setProgress(p);
            a_.status(s);
        });
    }

    // ── game package ─────────────────────────────────────────────────────

    void pickPackage() {
        if (!start()) return;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("application/zip");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            busy_ = true;
            cancel_ = false;
            progress_.setVisibility(View.VISIBLE);
            cancelButton_.setVisibility(View.VISIBLE);
            final String[] result = new String[1];
            a_.background(() -> result[0] = copyPackage(uri), () -> {
                busy_ = false;
                progress_.setVisibility(View.GONE);
                cancelButton_.setVisibility(View.GONE);
                a_.status(result[0]);
                refresh();
            });
        });
    }

    String copyPackage(Uri uri) {
        // ZipFile needs a file: a copy in the app's cache when it isn't one already.
        File tmp = new File(a_.getCacheDir(), "package.zip");
        try {
            a_.runOnUiThread(() -> a_.status("Reading the package…"));
            FileOps.copyIn(a_.getContentResolver(), uri, tmp);
            File dest = InstallActivity.gameFolder();
            String destPath = dest.getCanonicalPath() + File.separator;
            try (ZipFile zip = new ZipFile(tmp)) {
                long total = 0, done = 0, start = System.nanoTime();
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) total += Math.max(0, e.nextElement().getSize());
                ZipEntry xex = null;
                byte[] buffer = new byte[1 << 20];
                List<ZipEntry> all = new ArrayList<>();
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) all.add(e.nextElement());
                for (ZipEntry entry : all) if (entry.getName().equals("default.xex")) xex = entry;
                if (xex == null) return "Can't install: this isn't the game package (no default.xex).";
                all.remove(xex);
                all.add(xex);
                for (ZipEntry entry : all) {
                    if (cancel_) return "Stopped.";
                    File out = new File(dest, entry.getName());
                    if (!out.getCanonicalPath().startsWith(destPath)) throw new IOException("bad path " + entry.getName());
                    if (entry.isDirectory()) {
                        out.mkdirs();
                        continue;
                    }
                    out.getParentFile().mkdirs();
                    try (InputStream in = zip.getInputStream(entry); OutputStream os = new FileOutputStream(out)) {
                        int n;
                        while ((n = in.read(buffer)) > 0) {
                            os.write(buffer, 0, n);
                            done += n;
                        }
                    }
                    report(done, total, start, entry.getName());
                }
            }
            Shaders.install(a_);
            return "Installed from the package. Press Play.";
        } catch (Exception e) {
            return "Installing failed: " + e.getMessage();
        } finally {
            tmp.delete();
        }
    }
}
