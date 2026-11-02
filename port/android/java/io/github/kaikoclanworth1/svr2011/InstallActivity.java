// WWE SmackDown vs. Raw 2011 - first run on the phone.
//
// The PC launcher's "Create APK Package" makes the APK and a zip of the
// installed game. This activity gets all-files access (the game folder is
// in shared storage, so the files stay reachable from a PC over USB), finds
// that zip in the games or Download folder and extracts it into
// games/<kFolderName>, then opens the launcher (LauncherActivity).

package io.github.kaikoclanworth1.svr2011;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
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
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class InstallActivity extends Activity {
    static final String kFolderName = "WWE SmackDown vs. Raw 2011";
    static final String kPackageName = "SvR2011-Game.zip";
    static final String kTag = "SvR2011";

    private TextView text_;
    private ProgressBar progress_;
    private Button button_;
    private Thread worker_;

    // The app's own storage (filesDir/game), set by each activity at its start.
    static File privateFolder_;

    static void init(android.content.Context c) {
        if (privateFolder_ == null) privateFolder_ = new File(c.getFilesDir(), "game");
    }

    // <shared storage>/games/WWE SmackDown vs. Raw 2011 - or the app's own
    // storage when the game is there (an app moved to an SD card adopted as
    // internal storage: the shared storage stays on the full built-in memory).
    static File gameFolder() {
        if (privateFolder_ != null && new File(privateFolder_, "default.xex").isFile()) return privateFolder_;
        return new File(new File(Environment.getExternalStorageDirectory(), "games"), kFolderName);
    }

    // An automated test's start (tools/phone_session.ps1, android_night.ps1): "args" or an SVR2011_* extra.
    static boolean testExtras(android.os.Bundle extras) {
        if (extras == null) return false;
        for (String key : extras.keySet())
            if (key.equals("args") || key.startsWith("SVR2011_")) return true;
        return false;
    }

    static boolean installed() {
        return new File(gameFolder(), "default.xex").isFile();
    }

    // The package the launcher made, where the player may have copied it.
    static File findPackage() {
        File root = Environment.getExternalStorageDirectory();
        File[] places = {
            new File(root, "games"),
            new File(root, Environment.DIRECTORY_DOWNLOADS),
            gameFolder(),
            root,
        };
        for (File dir : places) {
            File zip = new File(dir, kPackageName);
            if (zip.isFile()) return zip;
        }
        return null;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        init(this);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.BLACK);
        layout.setPadding(64, 64, 64, 64);
        text_ = new TextView(this);
        text_.setTextColor(Color.WHITE);
        text_.setTextSize(20);
        text_.setGravity(Gravity.CENTER);
        progress_ = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setMax(1000);
        progress_.setVisibility(ProgressBar.GONE);
        button_ = new Button(this);
        button_.setVisibility(Button.GONE);
        layout.addView(text_);
        layout.addView(progress_, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        layout.addView(button_);
        setContentView(layout);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (worker_ == null && getIntent().getStringExtra("svr2011_import") != null) {
            importFiles(new File(getIntent().getStringExtra("svr2011_import")));
            return;
        }
        if (worker_ == null) next();
    }

    // Test aid (adb can't reach the storage of an app on an adopted SD card):
    // moves a shared-storage folder's files into the app's own game folder,
    // then writes <folder>/.imported and closes.
    private void importFiles(File from) {
        show("Importing game files...", null, null);
        worker_ = new Thread(() -> {
            String result = "ok";
            try {
                move(from, privateFolder_);
            } catch (IOException e) {
                result = "failed: " + e;
                Log.e(kTag, "import failed", e);
            }
            try (OutputStream o = new FileOutputStream(new File(from, ".imported"))) {
                o.write(result.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (IOException ignored) {
            }
            runOnUiThread(this::finish);
        });
        worker_.start();
    }

    private static void move(File from, File to) throws IOException {
        File[] list = from.listFiles();
        if (list == null) return;
        to.mkdirs();
        for (File f : list) {
            if (f.getName().equals(".imported")) continue;
            File out = new File(to, f.getName());
            if (f.isDirectory()) {
                move(f, out);
                f.delete();
                continue;
            }
            try (InputStream in = new FileInputStream(f); OutputStream o = new FileOutputStream(out)) {
                FileOps.copy(in, o);
            }
            f.delete();
        }
    }

    // The next first-run step, or the game.
    private void next() {
        if (android.os.Build.VERSION.SDK_INT < 30) {
            // Android 10: the old storage permission covers the games folder.
            if (checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                show("SvR 2011 keeps the game in the device's games folder:\n"
                    + "games/" + kFolderName + "\n\nAllow access to files on the next screen.",
                    "Allow access", () -> requestPermissions(new String[] {
                        android.Manifest.permission.READ_EXTERNAL_STORAGE,
                        android.Manifest.permission.WRITE_EXTERNAL_STORAGE }, 1));
                return;
            }
        } else if (!Environment.isExternalStorageManager()) {
            show("SvR 2011 keeps the game in the phone's games folder:\n"
                + "games/" + kFolderName + "\n\nAllow access to all files on the next screen.",
                "Allow access", () -> startActivity(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()))));
            return;
        }
        if (installed()) {
            // The launcher; automated tests (extras for the game: "args" or SVR2011_*) go straight to the game.
            // (Only those: some home screens add extras of their own when they open an app, and those phones
            // skipped the launcher every time - and with it the Mali alpha-mode check: a Mali-G610 started the
            // game without it, which closed at once - 2.1.2 report.)
            Intent next;
            if (testExtras(getIntent().getExtras())) {
                next = new Intent(this, GameActivity.class);
                next.putExtras(getIntent().getExtras());
            } else {
                next = new Intent(this, LauncherActivity.class);
                // (the launcher's own test extras - svr2011_test, svr2011_report_server, report_send, tab: adb
                // can't start it, it isn't exported)
                android.os.Bundle in = getIntent().getExtras();
                if (in != null)
                    for (String key : in.keySet())
                        if (key.startsWith("svr2011_") || key.equals("report_send") || key.equals("tab"))
                            next.putExtra(key, in.getString(key));
            }
            startActivity(next);
            finish();
            return;
        }
        File zip = findPackage();
        if (zip == null) {
            // The launcher's Install tab: from the disc image, or a package elsewhere.
            Intent launcher = new Intent(this, LauncherActivity.class);
            launcher.putExtra("tab", "Install");
            startActivity(launcher);
            finish();
            return;
        }
        install(zip);
    }

    private void show(String message, String action, Runnable onClick) {
        text_.setText(message);
        progress_.setVisibility(ProgressBar.GONE);
        if (action != null) {
            button_.setText(action);
            button_.setOnClickListener(v -> onClick.run());
            button_.setVisibility(Button.VISIBLE);
        } else {
            button_.setVisibility(Button.GONE);
        }
    }

    private void install(File zip) {
        show("Installing the game from\n" + zip.getAbsolutePath() + " ...", null, null);
        progress_.setProgress(0);
        progress_.setVisibility(ProgressBar.VISIBLE);
        worker_ = new Thread(() -> {
            String error = extract(zip, gameFolder());
            runOnUiThread(() -> {
                worker_ = null;
                if (error != null) {
                    show("Installing failed:\n" + error, "Try again", this::next);
                } else {
                    Shaders.install(this);
                    show("Installed to games/" + kFolderName + ".\n\n"
                        + "You can delete " + zip.getName() + " now to free up space.",
                        "Play", this::next);
                }
            });
        }, "install");
        worker_.start();
    }

    // Extracts every entry of zip into dest (default.xex last, so an
    // interrupted install is redone). Returns null or the error.
    private String extract(File zip, File dest) {
        try (ZipFile file = new ZipFile(zip)) {
            long total = 0, done = 0;
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                total += Math.max(0, e.nextElement().getSize());
            }
            String destPath = dest.getCanonicalPath() + File.separator;
            ZipEntry xex = null;
            byte[] buffer = new byte[1 << 20];
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                if (entry.getName().equals("default.xex")) {
                    xex = entry;
                    continue;
                }
                done = extractEntry(file, entry, dest, destPath, buffer, done, total);
            }
            if (xex == null) return "the package has no default.xex";
            extractEntry(file, xex, dest, destPath, buffer, done, total);
            return null;
        } catch (IOException e) {
            Log.e(kTag, "install", e);
            return e.toString();
        }
    }

    private long extractEntry(ZipFile file, ZipEntry entry, File dest, String destPath,
                              byte[] buffer, long done, long total) throws IOException {
        File out = new File(dest, entry.getName());
        if (!out.getCanonicalPath().startsWith(destPath)) {
            throw new IOException("bad path in package: " + entry.getName());
        }
        if (entry.isDirectory()) {
            out.mkdirs();
            return done;
        }
        out.getParentFile().mkdirs();
        try (InputStream in = file.getInputStream(entry);
             OutputStream os = new FileOutputStream(out)) {
            int n;
            long last = 0;
            while ((n = in.read(buffer)) > 0) {
                os.write(buffer, 0, n);
                done += n;
                if (done - last > (16 << 20)) {
                    last = done;
                    final int p = total > 0 ? (int) (done * 1000 / total) : 0;
                    runOnUiThread(() -> progress_.setProgress(p));
                }
            }
        }
        return done;
    }
}
