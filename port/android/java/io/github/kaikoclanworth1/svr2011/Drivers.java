// WWE SmackDown vs. Raw 2011 - custom GPU drivers in the launcher (Settings):
// driver packages (Mesa Turnip and the like: a zip with meta.json - name,
// description, author, driverVersion, libraryName - and the driver's .so)
// unpacked into the app's own storage (libadrenotools can't load a driver
// from shared storage), and the chosen one's .so written to the game's
// gpu_driver setting (src/gpu_driver.cpp loads it; empty: the phone's own).
// Packages without meta.json, or a bare .so, are taken too (the library is
// found by name). Driver variables (gpu_driver_env, e.g. Turnip's
// FD_DEV_FEATURES) are set by the game before it loads the driver.
//
// Built-in drivers (the APK's assets/drivers, android/drivers in the source):
// Mesa Turnip builds players found good on Adreno 710/720/722, whose own
// drivers are too old for the native renderer. The card offers them on those
// GPUs (once by itself, then in the card). Mali GPUs (GpuInfo): alpha mode,
// off until the player turns it on (the card, or when they press Play).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class Drivers {
    static final String kKey = "gpu_driver";
    static final String kEnvKey = "gpu_driver_env";
    // HyperOS 3 (Xiaomi): graphical glitches with Turnip, fixed by this hint.
    static final String kHyperOsFix = "FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1";
    static final String kMaliKey = "mali_alpha";

    // A driver package in the APK (assets/drivers) and the Adreno GPUs it is for.
    static final class BuiltIn {
        final String asset, name;
        final int[] adreno;
        final boolean recommended;

        BuiltIn(String asset, String name, int[] adreno, boolean recommended) {
            this.asset = asset;
            this.name = name;
            this.adreno = adreno;
            this.recommended = recommended;
        }

        boolean fits(int model) {
            for (int m : adreno)
                if (m == model) return true;
            return false;
        }
    }
    // (Turnip by vauzi, Mesa 26.3 - Vulkan 1.4; players' reports: both run the game well)
    static final BuiltIn[] kBuiltIn = {
        new BuiltIn("Turnip-710-720-722-v4.1.zip", "Turnip 710/720/722 v4.1", new int[] {710, 720, 722}, true),
        new BuiltIn("Turnip-710-720-722-v4.0.zip", "Turnip 710/720/722 v4.0", new int[] {710, 720, 722}, false),
    };

    static final class Driver {
        File dir, library;
        String name, version, author, description;
    }

    private final LauncherActivity a_;
    private final GameSettings settings_;
    private LinearLayout card_;

    Drivers(LauncherActivity a, GameSettings settings) {
        a_ = a;
        settings_ = settings;
    }

    File root() { return new File(a_.getFilesDir(), "drivers"); }

    static Driver read(File dir) {
        try {
            JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(dir, "meta.json").toPath()),
                StandardCharsets.UTF_8));
            Driver d = new Driver();
            d.dir = dir;
            d.name = m.optString("name", dir.getName());
            d.version = m.optString("driverVersion", "");
            d.author = m.optString("author", "");
            d.description = m.optString("description", "");
            d.library = new File(dir, m.getString("libraryName"));
            return d.library.isFile() ? d : null;
        } catch (Exception e) {
            return null;
        }
    }

    List<Driver> installed() {
        List<Driver> list = new ArrayList<>();
        File[] dirs = root().listFiles(File::isDirectory);
        if (dirs == null) return list;
        Arrays.sort(dirs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File d : dirs) {
            Driver dr = read(d);
            if (dr != null) list.add(dr);
        }
        return list;
    }

    String gpu() { return GpuInfo.renderer(a_); }

    // The built-in drivers for this phone's GPU (recommended first).
    List<BuiltIn> builtInsHere() {
        List<BuiltIn> list = new ArrayList<>();
        int model = GpuInfo.adrenoModel(gpu());
        for (BuiltIn b : kBuiltIn)
            if (model != 0 && b.fits(model)) list.add(b);
        return list;
    }

    // The Settings card.
    void build(LinearLayout column) {
        if (GpuInfo.isMali(gpu())) buildMali(column);
        card_ = a_.card(column, "Graphics driver");
        Button add = a_.button("Add a driver (.zip or .so)…", LauncherActivity.kCard);
        add.setOnClickListener(v -> add());
        column.addView(add, a_.fullWidth(10));
        // Driver variables: NAME=value, several separated by ';'.
        TextView varsLabel = a_.text("Driver variables (NAME=value; several separated by ;)", 13, LauncherActivity.kText);
        varsLabel.setPadding(a_.dp(4), a_.dp(12), a_.dp(4), 0);
        column.addView(varsLabel);
        android.widget.EditText vars = a_.field(kHyperOsFix, 300, android.text.InputType.TYPE_CLASS_TEXT
                                                    | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        vars.setGravity(android.view.Gravity.START);
        a_.textSetting(vars, kEnvKey, "Driver variables", "");
        column.addView(vars, a_.fullWidth(4));
        Button hyper = a_.button("Add the HyperOS 3 fix", LauncherActivity.kCard);
        hyper.setOnClickListener(v -> {
            String cur = vars.getText().toString().trim();
            if (!cur.contains("enable_tp_ubwc_flag_hint")) vars.setText(cur.isEmpty() ? kHyperOsFix : cur + ";" + kHyperOsFix);
        });
        column.addView(hyper, a_.fullWidth(6));
        String g = gpu();
        TextView note = a_.text((g.isEmpty() ? "" : "This phone's GPU: " + g + ".\n")
            + "For Adreno GPUs: driver packages such as Mesa Turnip or Qualcomm drivers (the zips "
            + "other emulators use, or the driver's .so). A driver that doesn't work falls back to the phone's own. "
            + "Phones with HyperOS 3 that show glitches: use a Turnip driver with the HyperOS 3 fix above. Applies "
            + "the next time the game starts.", 12, LauncherActivity.kDim);
        note.setPadding(a_.dp(4), a_.dp(8), a_.dp(4), 0);
        column.addView(note);
        refresh();
    }

    void refresh() {
        card_.removeAllViews();
        String current = settings_.getString(kKey, "");
        List<RadioButton> radios = new ArrayList<>();
        RadioButton phone = radio("The phone's own driver", null, current.isEmpty() || !new File(current).isFile());
        phone.setOnClickListener(v -> choose(null));
        radios.add(phone);
        card_.addView(phone);
        for (Driver d : installed()) {
            String sub = (d.version.isEmpty() ? "" : d.version) + (d.author.isEmpty() ? "" : " · " + d.author);
            RadioButton r = radio(d.name, sub, d.library.getPath().equals(current));
            r.setOnClickListener(v -> choose(d));
            r.setOnLongClickListener(v -> {
                remove(d);
                return true;
            });
            radios.add(r);
            card_.addView(r);
        }
        if (radios.size() > 1) {
            TextView hint = a_.text("Hold a driver to remove it.", 12, LauncherActivity.kDim);
            hint.setPadding(0, 0, 0, a_.dp(8));
            card_.addView(hint);
        }
        // Built-in drivers for this GPU (not added yet, or added and not chosen: a button each).
        List<BuiltIn> here = builtInsHere();
        if (!here.isEmpty()) {
            TextView about = a_.text("Built in for " + gpu() + ": Mesa Turnip drivers players found to run the "
                + "game well on Adreno 710, 720 and 722 (the phone's own driver is often too old for the Native "
                + "renderer).", 12, LauncherActivity.kDim);
            about.setPadding(0, a_.dp(4), 0, a_.dp(6));
            card_.addView(about);
        }
        for (BuiltIn b : here) {
            Driver installed = read(new File(root(), folderName(b.name)));
            if (installed != null && installed.library.getPath().equals(current)) continue;
            Button use = a_.button("Use " + b.name + (b.recommended ? " (recommended)" : ""),
                                   b.recommended ? LauncherActivity.kRed : LauncherActivity.kCard);
            use.setOnClickListener(v -> useBuiltIn(b));
            card_.addView(use, a_.fullWidth(6));
        }
    }

    // Adds a built-in driver (from the APK) and chooses it.
    void useBuiltIn(BuiltIn b) {
        a_.status("Adding " + b.name + "…");
        final Driver[] added = new Driver[1];
        final String[] error = new String[1];
        a_.background(() -> {
            File tmp = new File(a_.getCacheDir(), "driver.zip");
            try (InputStream in = a_.getAssets().open("drivers/" + b.asset);
                 OutputStream out = new FileOutputStream(tmp)) {
                FileOps.copy(in, out);
            } catch (IOException e) {
                error[0] = e.getMessage();
                return;
            }
            try {
                added[0] = unpackFile(tmp, b.name);
            } catch (IOException e) {
                error[0] = e.getMessage();
            }
        }, () -> {
            if (added[0] == null) {
                a_.status(b.name + " can't be added: " + error[0]);
                return;
            }
            choose(added[0]);
        });
    }

    // Once, on a GPU a built-in driver is for, with the phone's own driver in use: offer it.
    void offerBuiltIn() {
        List<BuiltIn> here = builtInsHere();
        if (here.isEmpty() || !settings_.getString(kKey, "").isEmpty()) return;
        android.content.SharedPreferences prefs = a_.getPreferences(android.content.Context.MODE_PRIVATE);
        if (prefs.getBoolean("builtin_driver_offered", false)) return;
        prefs.edit().putBoolean("builtin_driver_offered", true).apply();
        BuiltIn b = here.get(0);
        new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("A better graphics driver for " + gpu())
            .setMessage("On Adreno 710, 720 and 722 the phone's own driver is often too old for the Native renderer, "
                + "so the game falls back to the slower Emulated one. Players found the built-in " + b.name
                + " driver (Mesa Turnip) runs the game well on these GPUs.\n\nUse it? You can change it any time in "
                + "Settings, Graphics driver - and if it doesn't work the game goes back to the phone's own.")
            .setPositiveButton("Use " + b.name, (dlg, w) -> useBuiltIn(b))
            .setNegativeButton("Not now", null)
            .show();
    }

    // ── Mali (alpha) ──────────────────────────────────────────────────────

    static final String kMaliAbout = "Mali GPUs aren't supported yet: the game may show graphics problems, run "
        + "slowly or not start. Alpha mode lets it try (the emulator's geometry shaders and line drawing - which "
        + "Mali lacks - are skipped, and textures the GPU can't read are converted). If you try it, please send a "
        + "problem report (Play tab, Report a problem) whether it works or not: it says what your GPU needs.";

    void buildMali(LinearLayout column) {
        // (alpha mode turned on by an older launcher: its settings brought up to date)
        if (settings_.getBool(kMaliKey, false)) {
            setMali(true);
            settings_.save();
        }
        LinearLayout card = a_.card(column, "Mali GPU (alpha)");
        android.widget.Switch sw = new android.widget.Switch(a_);
        sw.setText("Mali alpha mode");
        sw.setTextColor(LauncherActivity.kText);
        sw.setTextSize(15);
        sw.setChecked(settings_.getBool(kMaliKey, false));
        sw.setOnCheckedChangeListener((v, on) -> {
            setMali(on);
            a_.status(settings_.save() ? "Mali alpha mode " + (on ? "on" : "off") + ". It applies the next time "
                                             + "the game starts."
                                       : "Could not save the settings.");
        });
        card.addView(sw);
        TextView note = a_.text("This phone's GPU: " + gpu() + ". " + kMaliAbout, 12, LauncherActivity.kDim);
        note.setPadding(0, a_.dp(6), 0, a_.dp(4));
        card.addView(note);
    }

    // What Mali lacks (vulkan.gpuinfo.org, Mali-G57 to G925): line drawing and
    // vertex shader stores (every driver) - the emulator's fallbacks instead;
    // and its geometry shaders are slow, so the emulator expands points,
    // rectangles and quads without them.
    static final String[] kMaliOff = {"vulkan_require_geometry_shader", "vulkan_require_fill_mode_non_solid",
                                      "vulkan_require_vertex_pipeline_stores_and_atomics"};
    static final String[] kMaliOn = {"vulkan_force_expand_point_sprites_in_vs",
                                     "vulkan_force_expand_rectangle_lists_in_vs",
                                     "vulkan_force_convert_quad_lists_to_triangle_lists"};

    void setMali(boolean on) {
        settings_.setBool(kMaliKey, on);
        for (String k : kMaliOff) settings_.setBool(k, !on);
        for (String k : kMaliOn) settings_.setBool(k, on);
        // Mali GPUs are in slow phones and tablets: the scene at half resolution
        // and small textures (memory), no real-time shadows; the crowd is left
        // out in Mali mode too (arena_crowd auto). Off: full resolution and
        // shadows again.
        settings_.setDouble("native_render_scale", on ? 0.5 : 1.0);
        settings_.setInt("native_scene_height", 0);  // (an in-game 900P / 1080P would win over it)
        if (on) settings_.setString("native_texture_quality", "low");
        settings_.setBool("native_shadows", !on);  // (no real-time shadows: fewer draws)
        // 30 frames a second unless the player chose: on the Galaxy A51 (Mali-G72) the 60 setting
        // gave 33 fps at 45-50/60 game speed with 30-65 ms swings, the 30 setting a steady 29.4 at
        // 57/60 (the game's logic thread is the limit at half resolution, not the GPU). The
        // launcher's Frame rate choice (Display) keeps whatever the player picks.
        if (on && settings_.getString("frame_rate", "").isEmpty()) settings_.setInt("frame_rate", 30);
    }

    // Before the game starts on a Mali GPU: the warning (each time, until the
    // player ticks "Don't warn me again"). Continuing turns alpha mode on - the
    // game can't start on Mali without it. True: go on.
    boolean readyToPlay(Runnable play) {
        if (!GpuInfo.isMali(gpu())) return true;
        if (maliAccepted_) {  // (the Play this dialog's "Continue anyway" started)
            maliAccepted_ = false;
            return true;
        }
        android.content.SharedPreferences prefs = a_.getPreferences(android.content.Context.MODE_PRIVATE);
        if (prefs.getBoolean("mali_warning_off", false) && settings_.getBool(kMaliKey, false)) return true;
        android.widget.CheckBox quiet = new android.widget.CheckBox(a_);
        quiet.setText("Don't warn me again");
        quiet.setTextColor(LauncherActivity.kText);
        quiet.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        LinearLayout box = new LinearLayout(a_);
        box.setPadding(a_.dp(20), a_.dp(4), a_.dp(20), 0);
        box.addView(quiet);
        new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Unsupported GPU: " + (gpu().isEmpty() ? "Mali" : gpu()))
            .setMessage("This phone has a Mali GPU, which the game doesn't support for the moment. You can still "
                + "play: it may show graphics problems, run slowly or not start.\n\nContinuing turns on Mali alpha "
                + "mode (Settings, Mali GPU), which the game needs to start on Mali. If you try it, please send a "
                + "problem report (Play tab, Report a problem) - it tells us what your GPU needs.")
            .setView(box)
            .setPositiveButton("Continue anyway", (dlg, w) -> {
                if (quiet.isChecked()) prefs.edit().putBoolean("mali_warning_off", true).apply();
                if (!settings_.getBool(kMaliKey, false)) setMali(true);
                settings_.save();
                maliAccepted_ = true;
                play.run();
            })
            .setNegativeButton("Cancel", null)
            .show();
        return false;
    }
    private boolean maliAccepted_;

    RadioButton radio(String title, String sub, boolean checked) {
        RadioButton r = new RadioButton(a_);
        r.setText(sub == null || sub.isEmpty() ? title : title + "\n" + sub);
        r.setTextColor(LauncherActivity.kText);
        r.setTextSize(15);
        r.setChecked(checked);
        r.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        r.setPadding(a_.dp(8), a_.dp(10), 0, a_.dp(10));
        return r;
    }

    // The game stopped (a native crash: a driver that loses the GPU ends there)
    // since a custom driver was chosen: offer the phone's own driver back.
    void checkCrash() {
        if (checkStuck()) return;
        String current = settings_.getString(kKey, "");
        if (current.isEmpty() || GameActivity.running) return;
        String name = new File(current).getParentFile().getName();
        for (Driver d : installed())
            if (d.library.getPath().equals(current)) name = d.name;
        // The game's note (src/gpu_driver.cpp): the driver didn't start, it used the phone's.
        File failed = new File(a_.getCacheDir(), "gpu_driver_failed.txt");
        if (failed.isFile()) {
            String why = "";
            try {
                String[] lines = new String(Files.readAllBytes(failed.toPath()), StandardCharsets.UTF_8).split("\n");
                if (lines.length > 1) why = " (" + lines[1].trim() + ")";
            } catch (IOException e) {
            }
            failed.delete();
            ask(name + " didn't work on this phone" + why + ": the game used the phone's own driver. Go back to "
                + "the phone's own driver?", name);
            return;
        }
        if (android.os.Build.VERSION.SDK_INT < 30) return;  // (the exit reasons: Android 11 and later)
        android.content.SharedPreferences prefs = a_.getPreferences(android.content.Context.MODE_PRIVATE);
        long since = prefs.getLong("driver_chosen_at", 0);
        android.app.ActivityManager am = a_.getSystemService(android.app.ActivityManager.class);
        // (newest first; later exits - an update, the app closed - don't hide the crash)
        android.app.ApplicationExitInfo crash = null;
        for (android.app.ApplicationExitInfo e : am.getHistoricalProcessExitReasons(null, 0, 16)) {
            if (e.getTimestamp() <= since) break;
            if (e.getReason() == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE) {
                crash = e;
                break;
            }
        }
        if (crash == null) return;
        prefs.edit().putLong("driver_chosen_at", crash.getTimestamp()).apply();  // (asked once per crash)
        ask("The game stopped while using the graphics driver " + name + ". Go back to the phone's own driver?",
            name);
    }

    // The game's note (src/gpu_driver.cpp): no frame a minute after the start -
    // the graphics driver got stuck. Offer the phone's own driver, or (already
    // on it) say plainly that this GPU may not work yet. True: a note was shown.
    boolean checkStuck() {
        // The game's note (native_renderer.cpp): its graphics couldn't start or stopped -
        // there is no other renderer to fall back on.
        File failed = new File(a_.getCacheDir(), "native_failed.txt");
        if (failed.isFile() && !GameActivity.running) {
            String why = "";
            try {
                why = new String(Files.readAllBytes(failed.toPath()), StandardCharsets.UTF_8).trim();
            } catch (IOException e) {
            }
            failed.delete();
            String driver = settings_.getString(kKey, "");
            String g = gpu().isEmpty() ? "this phone's GPU" : gpu();
            new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle("The game's graphics couldn't run")
                .setMessage("Last time the game's graphics couldn't run on " + g
                    + (why.isEmpty() ? "" : " (" + why + ")") + ". "
                    + (driver.isEmpty() ? "You can try a Mesa Turnip driver (Settings, Graphics driver). "
                                        : "You can try the phone's own driver or another one (Settings, Graphics driver). ")
                    + "Please send a problem report (Play tab, Report a problem) - it tells us what your phone needs.")
                .setPositiveButton("OK", null)
                .show();
            return true;
        }
        File note = new File(a_.getCacheDir(), "gpu_stuck.txt");
        if (!note.isFile() || GameActivity.running) return false;
        String driver = "";
        try {
            String[] lines = new String(Files.readAllBytes(note.toPath()), StandardCharsets.UTF_8).split("\n");
            if (lines.length > 0) driver = lines[0].trim();
        } catch (IOException e) {
        }
        note.delete();
        String g = gpu().isEmpty() ? "this phone's GPU" : gpu();
        if (!driver.isEmpty()) {
            String name = new File(driver).getParentFile() != null ? new File(driver).getParentFile().getName() : driver;
            for (Driver d : installed())
                if (d.library.getPath().equals(driver)) name = d.name;
            ask("Last time the game got stuck starting its graphics (no picture after a minute) with the graphics "
                + "driver " + name + " on " + g + ". Go back to the phone's own driver?", name);
        } else {
            new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle("The game couldn't start its graphics")
                .setMessage("Last time the game got stuck starting its graphics (no picture after a minute) with "
                    + "the phone's own driver on " + g + ". This GPU may not be able to run the game yet. You can "
                    + "try a Mesa Turnip driver (Settings, Graphics driver), and please send a problem report "
                    + "(Play tab, Report a problem) - it tells us what your phone needs.")
                .setPositiveButton("OK", null)
                .show();
        }
        return true;
    }

    void ask(String message, String name) {
        new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setMessage(message)
            .setPositiveButton("Use the phone's driver", (dlg, w) -> choose(null))
            .setNegativeButton("Keep " + name, null)
            .show();
    }

    void choose(Driver d) {
        a_.getPreferences(android.content.Context.MODE_PRIVATE).edit()
            .putLong("driver_chosen_at", System.currentTimeMillis()).apply();
        settings_.setString(kKey, d == null ? "" : d.library.getPath());
        a_.status(settings_.save() ? (d == null ? "The phone's own driver." : d.name + " chosen.")
                                         + " It applies the next time the game starts."
                                   : "Could not save the settings.");
        refresh();
    }

    void remove(Driver d) {
        a_.confirm("Remove the driver " + d.name + "?", "Remove", () -> {
            if (d.library.getPath().equals(settings_.getString(kKey, ""))) {
                settings_.setString(kKey, "");
                settings_.save();
            }
            FileOps.deleteTree(d.dir);
            a_.status("Removed " + d.name + ".");
            refresh();
        });
    }

    void add() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        // (any type: many file managers label downloaded zips octet-stream, and a .so has none)
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] result = new String[1];
            a_.background(() -> {
                try {
                    result[0] = "Added " + unpack(uri).name + ". Choose it above to use it.";
                } catch (Exception e) {
                    result[0] = "That driver can't be added: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }

    String zipName(Uri uri) {
        try (android.database.Cursor c = a_.getContentResolver().query(uri,
                 new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null) return n.toLowerCase().endsWith(".zip") ? n.substring(0, n.length() - 4) : n;
            }
        } catch (Exception e) {
        }
        return "driver";
    }

    // A folder name from a driver's name.
    static String folderName(String name) {
        StringBuilder folder = new StringBuilder();
        for (char c : name.toCharArray()) folder.append(Character.isLetterOrDigit(c) || c == '-' || c == '.' ? c : '_');
        return folder.length() == 0 ? "driver" : folder.toString();
    }

    // Is it an ELF file (a bare .so)?
    static boolean isElf(File f) {
        byte[] head = new byte[4];
        try (InputStream in = new java.io.FileInputStream(f)) {
            return in.read(head) == 4 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
        } catch (IOException e) {
            return false;
        }
    }

    // The library in a zip without meta.json: the shallowest .so with "vulkan" in its name, else the only .so.
    static ZipEntry findLibrary(ZipFile zip) {
        ZipEntry best = null, only = null;
        int count = 0;
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
            ZipEntry z = e.nextElement();
            String n = z.getName();
            if (z.isDirectory() || n.contains("__MACOSX/") || !n.toLowerCase().endsWith(".so")) continue;
            count++;
            only = z;
            String base = n.substring(n.lastIndexOf('/') + 1).toLowerCase();
            if (base.contains("vulkan") && (best == null || n.length() < best.getName().length())) best = z;
        }
        return best != null ? best : count == 1 ? only : null;
    }

    Driver unpack(Uri uri) throws IOException {
        File tmp = new File(a_.getCacheDir(), "driver.zip");
        FileOps.copyIn(a_.getContentResolver(), uri, tmp);
        return unpackFile(tmp, zipName(uri));
    }

    // A driver package (or bare .so) copied to `tmp` (deleted after);
    // `fallbackName` names it when it doesn't say.
    Driver unpackFile(File tmp, String fallbackName) throws IOException {
        if (isElf(tmp)) {
            // A bare driver .so: set up as a package of its own.
            String name = fallbackName;
            if (name.toLowerCase().endsWith(".so")) name = name.substring(0, name.length() - 3);
            File dir = new File(root(), folderName(name));
            FileOps.deleteTree(dir);
            dir.mkdirs();
            File lib = new File(dir, "vulkan.custom.so");
            try (InputStream in = new java.io.FileInputStream(tmp); OutputStream os = new FileOutputStream(lib)) {
                FileOps.copy(in, os);
            } finally {
                tmp.delete();
            }
            try {
                JSONObject m = new JSONObject();
                m.put("name", name);
                m.put("libraryName", lib.getName());
                Files.write(new File(dir, "meta.json").toPath(), m.toString(2).getBytes(StandardCharsets.UTF_8));
            } catch (org.json.JSONException e) {
                throw new IOException("can't set the driver up");
            }
            Driver d = read(dir);
            if (d == null) throw new IOException("can't set the driver up");
            return d;
        }
        try (ZipFile zip = new ZipFile(tmp)) {
            ZipEntry metaEntry = null;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                // (not a Mac's resource-fork copy, __MACOSX/._meta.json; the shallowest one)
                String n = z.getName();
                if (n.startsWith("__MACOSX/") || !(n.equals("meta.json") || n.endsWith("/meta.json"))) continue;
                if (metaEntry == null || n.length() < metaEntry.getName().length()) metaEntry = z;
            }
            String prefix, metaText;
            if (metaEntry == null) {
                // No meta.json: find the driver library by its name and describe it.
                ZipEntry lib = findLibrary(zip);
                if (lib == null) throw new IOException("no driver library (.so) found in it");
                String n = lib.getName();
                prefix = n.substring(0, n.lastIndexOf('/') + 1);
                try {
                    JSONObject m = new JSONObject();
                    m.put("name", fallbackName);
                    m.put("libraryName", n.substring(prefix.length()));
                    metaText = m.toString(2);
                } catch (org.json.JSONException e) {
                    throw new IOException("can't set the driver up");
                }
            } else {
                prefix = metaEntry.getName().substring(0, metaEntry.getName().length() - "meta.json".length());
                try (InputStream in = zip.getInputStream(metaEntry)) {
                    java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                    FileOps.copy(in, b);
                    metaText = b.toString("UTF-8");
                }
            }
            String name, library;
            try {
                JSONObject m = new JSONObject(metaText);
                name = m.optString("name", "");
                if (name.isEmpty()) name = fallbackName;  // (some packages carry only libraryName)
                library = m.getString("libraryName");
            } catch (org.json.JSONException e) {
                throw new IOException("its meta.json can't be read");
            }
            if (zip.getEntry(prefix + library) == null) throw new IOException("it doesn't contain " + library);
            File dir = new File(root(), folderName(name));
            FileOps.deleteTree(dir);
            dir.mkdirs();
            String destPath = dir.getCanonicalPath() + File.separator;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (z.isDirectory() || !z.getName().startsWith(prefix) || z.getName().contains("__MACOSX/")) continue;
                File out = new File(dir, z.getName().substring(prefix.length()));
                if (!out.getCanonicalPath().startsWith(destPath)) throw new IOException("bad path in the package");
                out.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(z); OutputStream os = new FileOutputStream(out)) {
                    FileOps.copy(in, os);
                }
            }
            if (metaEntry == null)
                Files.write(new File(dir, "meta.json").toPath(), metaText.getBytes(StandardCharsets.UTF_8));
            if (library.equals("vulkan.adreno.so")) {
                // Qualcomm's own drivers keep the phone driver's file name: the app's UI has
                // that one loaded already, and loading it again just gives the phone's back.
                File renamed = new File(dir, "vulkan.custom.so");
                if (!new File(dir, library).renameTo(renamed)) throw new IOException("can't set the driver up");
                try {
                    JSONObject m = new JSONObject(metaText);
                    m.put("libraryName", renamed.getName());
                    Files.write(new File(dir, "meta.json").toPath(), m.toString(2).getBytes(StandardCharsets.UTF_8));
                } catch (org.json.JSONException e) {
                    throw new IOException("its meta.json can't be read");
                }
            }
            Driver d = read(dir);
            if (d == null) throw new IOException("its meta.json can't be read");
            return d;
        } finally {
            tmp.delete();
        }
    }
}
