// WWE SmackDown vs. Raw 2011 - the mods that come with the port, on the phone
// (as the PC launcher's mods_install_bundled, launcher/mods_tab.c). The APK
// has no room for them: the release's SvR2011-Mods-v<version>.zip (Bundled
// Mods\*.svrmod) is downloaded from GitHub - offered after a disc install,
// after an app update, and by the Mods tab's "Get bundled mods" - or read from
// a copy in the games or Download folder. Its size is shown first (and mobile
// data said), and it can be stopped.
//
// The rules are the PC's (BundledRules: Mods/.bundled_state, the decisions in
// Mods/.install.log): a bundle installs once, switched off ("disabled"), match
// types too; one the player removed stays removed; a newer version updates
// an unedited copy (its on / off kept), and an edited one is asked about here
// (update, the edited copy to Mods/.backup / keep mine); missing files are
// asked about (fix / not now). Retired bundles are switched off once.

package io.github.kaikoclanworth1.svr2011;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.os.Environment;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class BundledMods {
    // (the app version whose bundles were installed, or offered and put off)
    static final String kPref = "bundled_mods_version";

    private static boolean busy_;
    static boolean busy() { return busy_; }   // (ModsPage.entranceCheckAtStart waits for them)

    static File modsFolder() { return new File(InstallActivity.gameFolder(), "Mods"); }

    // A copy the player put on the phone (SvR2011-Mods*.zip), the newest.
    static File findLocal() {
        File root = Environment.getExternalStorageDirectory();
        File best = null;
        for (File dir : new File[] {new File(root, "games"), new File(root, Environment.DIRECTORY_DOWNLOADS)}) {
            File[] list = dir.listFiles(f -> f.isFile() && f.getName().startsWith("SvR2011-Mods")
                && f.getName().toLowerCase().endsWith(".zip"));
            if (list != null) for (File f : list) if (best == null || f.lastModified() > best.lastModified()) best = f;
        }
        return best;
    }

    // At the launcher's start: once for each app version (an update, or the
    // first start of an app that didn't do this yet).
    static void atStart(LauncherActivity a) {
        if (!InstallActivity.installed()) return;
        String last = a.getPreferences(Context.MODE_PRIVATE).getString(kPref, null);
        if (!a.versionName().equals(last)) offer(a, false, null);
    }

    static void remember(LauncherActivity a) {
        a.getPreferences(Context.MODE_PRIVATE).edit().putString(kPref, a.versionName()).apply();
    }

    // Asks to get them (asked: the player pressed the button - say why not,
    // when not); `after` runs on the UI thread when they're in.
    static void offer(LauncherActivity a, boolean asked, Runnable after) {
        if (busy_) {
            if (asked) a.status("The bundled mods are being installed already.");
            return;
        }
        if (GameActivity.running) {
            if (asked) a.status("Close the game first.");
            return;
        }
        File local = findLocal();
        if (local != null) {
            ask(a, "Install the mods that come with the port from " + local.getAbsolutePath() + " ("
                + FileOps.human(local.length()) + ")? " + rules(), "Install", asked, () -> run(a, null, local, after));
            return;
        }
        if (asked) a.status("Looking for the bundled mods…");
        final Updater.Release[] rel = new Updater.Release[1];
        final String[] err = new String[1];
        a.background(() -> {
            try {
                rel[0] = Updater.release(a.versionName());
            } catch (Exception e) {
                err[0] = e.getMessage();
            }
        }, () -> {
            if (err[0] != null) {
                if (asked) a.status("Couldn't look for the bundled mods: " + err[0] + ".");
                return;
            }
            if (rel[0] == null || rel[0].modsUrl == null) {
                if (asked) a.status("The release has no bundled mods to download.");
                return;
            }
            Updater.Release r = rel[0];
            String size = r.modsSize > 0 ? FileOps.human(r.modsSize) : "size unknown";
            boolean metered = false;
            try {
                ConnectivityManager cm = (ConnectivityManager) a.getSystemService(Context.CONNECTIVITY_SERVICE);
                metered = cm != null && cm.isActiveNetworkMetered();
            } catch (Exception ignored) {
            }
            ask(a, "Download the mods that come with version " + r.version + " (" + size + ")? " + rules()
                + (metered ? "\n\nYou're on mobile data: this downloads " + size + "." : ""),
                metered ? "Download on mobile data" : "Download", asked, () -> run(a, r, null, after));
        });
    }

    static String rules() {
        return "New ones come switched off: switch them on in the Mods tab (while the server doesn't allow mods "
            + "online, a mod that's on keeps you out of online matches). Mods you have keep their on / off, ones you "
            + "removed stay removed, and you're asked before one you edited is updated.";
    }

    static void ask(LauncherActivity a, String message, String action, boolean asked, Runnable onYes) {
        new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Bundled mods")
            .setMessage(message)
            .setPositiveButton(action, (d, w) -> onYes.run())
            .setNegativeButton("Not now", (d, w) -> {
                if (!asked) {
                    remember(a);
                    a.status("The bundled mods can be got later: Mods tab → Get bundled mods.");
                }
            })
            .setCancelable(false)
            .show();
    }

    // Downloads (rel) or reads (local) the zip and installs it, with a dialog
    // showing how far it is and a Cancel.
    static void run(LauncherActivity a, Updater.Release rel, File local, Runnable after) {
        busy_ = true;
        AtomicBoolean cancel = new AtomicBoolean();
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(a.dp(20), a.dp(16), a.dp(20), a.dp(8));
        TextView text = a.text(rel != null ? "Downloading…" : "Installing…", 15, LauncherActivity.kText);
        ProgressBar bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        box.addView(text);
        box.addView(bar, a.fullWidth(10));
        AlertDialog dialog = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Bundled mods")
            .setView(box)
            .setNegativeButton("Cancel", (d, w) -> {
                cancel.set(true);
                a.status("Stopping the bundled mods…");
            })
            .setCancelable(false)
            .show();
        final String[] result = new String[1];
        final boolean[] done = new boolean[1];
        a.background(() -> {
            File tmp = new File(a.getCacheDir(), "bundled_mods.zip");
            try {
                File zip = local;
                if (rel != null) {
                    if (rel.modsSize > 0 && a.getCacheDir().getUsableSpace() < rel.modsSize + (64L << 20))
                        throw new IOException("not enough free space (" + FileOps.human(rel.modsSize + (64L << 20)) + " needed)");
                    download(rel, tmp, (got, total) -> a.runOnUiThread(() -> {
                        if (total > 0) bar.setProgress((int) (got * 1000 / total));
                        text.setText("Downloading… " + FileOps.human(got) + (total > 0 ? " of " + FileOps.human(total) : ""));
                    }), cancel);
                    zip = tmp;
                }
                result[0] = installAll(a, zip, cancel, (i, n) -> a.runOnUiThread(() -> {
                    bar.setProgress(n > 0 ? (int) (i * 1000 / n) : 0);
                    text.setText("Installing the mods… " + i + " of " + n);
                }), q -> askNow(a, q));
                done[0] = !cancel.get();
            } catch (Exception e) {
                result[0] = cancel.get() ? "Stopped: the bundled mods not installed yet are got next time."
                                         : "The bundled mods couldn't be got: " + e.getMessage() + ".";
            } finally {
                tmp.delete();
            }
        }, () -> {
            busy_ = false;
            if (dialog.isShowing()) dialog.dismiss();
            if (done[0]) remember(a);
            a.status(result[0]);
            if (after != null) after.run();
        });
    }

    static void download(Updater.Release rel, File to, Updater.Progress progress, AtomicBoolean cancel) throws IOException {
        HttpURLConnection c = Updater.connect(rel.modsUrl);
        if (c.getResponseCode() != 200) throw new IOException("the download answered " + c.getResponseCode());
        long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : rel.modsSize;
        try (InputStream in = new Updater.CountingStream(c.getInputStream(), total, progress, cancel);
             OutputStream out = new FileOutputStream(to)) {
            FileOps.copy(in, out);
        }
        if (cancel.get()) throw new IOException("stopped");
    }

    interface Step { void at(int i, int n); }

    interface Asker { int ask(BundledRules.Question q); }

    // A bundle's question as a dialog, from the install's thread (it waits for the answer); kNone when the
    // launcher closed meanwhile (asked again next time).
    static int askNow(LauncherActivity a, BundledRules.Question q) {
        boolean update = q.kind == BundledRules.kAskUpdate;
        String msg = update
            ? "A newer version (" + q.ship + ") of the bundled mod \"" + q.name + "\" came with the update, but your "
              + "copy (version " + (q.have.isEmpty() ? "?" : q.have) + ") has been edited.\n\nUpdate: your edited copy "
              + "is kept in Mods/.backup.\nKeep mine: you won't be asked again for this version."
            : "Files of the bundled mod \"" + q.name + "\" are missing (" + q.missing + ") - a copy that stopped "
              + "half-way, or a cleaner app?\n\nFix it: it's installed again, its on / off kept.\nNot now: you're "
              + "asked again next time.";
        CountDownLatch done = new CountDownLatch(1);
        int[] answer = {BundledRules.kNone};
        a.runOnUiThread(() -> {
            if (a.isFinishing()) {
                done.countDown();
                return;
            }
            new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle("Bundled mods")
                .setMessage(msg)
                .setPositiveButton(update ? "Update" : "Fix it", (d, w) -> answer[0] = BundledRules.kYes)
                .setNegativeButton(update ? "Keep mine" : "Not now", (d, w) -> answer[0] = BundledRules.kNo)
                .setOnDismissListener(d -> done.countDown())
                .setCancelable(false)
                .show();
        });
        try {
            while (!done.await(1, TimeUnit.SECONDS))
                if (a.isFinishing() || a.isDestroyed()) return BundledRules.kNone;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return BundledRules.kNone;
        }
        return answer[0];
    }

    // Checks the zip's *.svrmod by the rules (BundledRules) and acts; returns what it did.
    static String installAll(Context ctx, File zipFile, AtomicBoolean cancel, Step step, Asker asker) throws IOException {
        File one = new File(ctx.getCacheDir(), "bundle.svrmod");
        BundledRules rules = new BundledRules(modsFolder(), new BundledRules.Host() {
            @Override public File install(File svrmod) throws IOException {
                try (InputStream in = new FileInputStream(svrmod)) {
                    ModsPage.Installed r = ModsPage.install(ctx, in, true);
                    return r.kept ? null : r.dest;
                }
            }

            @Override public int ask(BundledRules.Question q) { return asker.ask(q); }
        });
        try (ZipFile zip = new ZipFile(zipFile)) {
            List<ZipEntry> all = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (!z.isDirectory() && z.getName().toLowerCase().endsWith(".svrmod")) all.add(z);
            }
            if (all.isEmpty()) throw new IOException("that zip has no mods in it");
            for (int i = 0; i < all.size(); i++) {
                if (cancel.get()) break;
                ZipEntry z = all.get(i);
                String n = z.getName().replace('\\', '/');
                step.at(i, all.size());
                try (InputStream in = zip.getInputStream(z); OutputStream out = new FileOutputStream(one)) {
                    FileOps.copy(in, out);
                } catch (IOException e) {
                    rules.fail(n.substring(n.lastIndexOf('/') + 1), e.getMessage());
                    continue;
                }
                rules.bundle(n.substring(n.lastIndexOf('/') + 1), one);
            }
            step.at(all.size(), all.size());
        } finally {
            one.delete();
            rules.finish();
        }
        return rules.summary(cancel.get());
    }
}
