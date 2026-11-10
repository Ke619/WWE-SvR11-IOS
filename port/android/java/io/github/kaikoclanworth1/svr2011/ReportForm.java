// WWE SmackDown vs. Raw 2011 - the launcher's Report a problem, as the PC
// launcher's (launcher/svr2011_launcher.c rep_page1 / rep_page3): it asks what
// kind of problem, how often, what happened and what came just before (that
// goes at the top of the zip's report.txt, ProblemReport), takes a recorded
// match, its mods, the main save and pictures, and sends it all to the online
// server (ReportSend). Reports are only sent - nothing is saved for posting
// elsewhere; a send that fails says why and offers Try again.
package io.github.kaikoclanworth1.svr2011;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class ReportForm {
    static final String[] kKinds = {"The game crashed (closed by itself)", "The game froze or hung",
        "Graphics look wrong", "Sound or music", "Controller or keyboard", "Online", "Mods",
        "Saves or created content", "The launcher", "Something else"};
    static final String[] kAgain = {"Every time", "Sometimes", "It happened once"};

    // The form's state while it's open: the picked match's mods (checked
    // against the server in the background), the pictures (copied into the cache).
    static final class State {
        List<File> matches = new ArrayList<>();
        File match;
        List<ReportSend.Mod> mods = new ArrayList<>();
        boolean modsBusy, sending;
        int modsGen;
        String modsNote = "";
        List<File> pics = new ArrayList<>();
    }

    static File cache(Context c) {
        File d = new File(c.getCacheDir(), "report");
        d.mkdirs();
        return d;
    }

    // Everything a report made in the cache (zip, mod zips, copied pictures): gone once it's sent or the form closes.
    static void clean(Context c) {
        File[] files = cache(c).listFiles();
        if (files != null) for (File f : files) f.delete();
    }

    static String server(LauncherActivity a) {
        String test = ReportSend.testRun ? ReportSend.testServer : null;  // (a test's own server)
        if (test != null && !test.isEmpty()) return test;
        return a.settings().getString("online_server", OnlinePage.kDefaultServer);
    }

    static void show(LauncherActivity a) {
        clean(a);
        State st = new State();
        File game = InstallActivity.gameFolder();
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text("Tell us what went wrong. The report also takes your newest game logs, crash reports and "
            + "settings (your online password and token are left out). If you can, run the game and make it happen "
            + "once more first: the newest log is the one that counts.", 14, LauncherActivity.kDim));
        Spinner kind = spinner(a, kKinds);
        Spinner again = spinner(a, kAgain);
        EditText what = box(a, 4);
        EditText before = box(a, 3);
        EditText contact = box(a, 1);
        contact.setInputType(InputType.TYPE_CLASS_TEXT);
        label(a, v, "What kind of problem?");
        v.addView(kind);
        label(a, v, "Does it happen again?");
        v.addView(again);
        label(a, v, "What happened?");
        v.addView(what);
        label(a, v, "What were you doing just before? (menu or mode, match type, superstars, mods)");
        v.addView(before);
        label(a, v, "Your Discord name (needed - we answer there; only we see it)");
        contact.setText(ReportSend.discordLoad(game));
        v.addView(contact);
        if (a.settings().getString("online_token", "").isEmpty()) {   // (2.2.1: reports come from an online account)
            TextView signIn = a.text("Not signed in: sign in (or create an account) on the Online page first - reports "
                + "come from an online account.", 14, LauncherActivity.kRed);
            v.addView(signIn);
        }
        // the match (the newest one picked, as on the PC) and its mods
        st.matches = ReportSend.matches(game);
        List<String> labels = new ArrayList<>();
        labels.add(st.matches.isEmpty() ? "None (no recorded matches)" : "None");
        for (File m : st.matches) labels.add(matchLabel(m));
        label(a, v, "The match it happened in (it's recorded: we can play it back)");
        Spinner match = spinner(a, labels.toArray(new String[0]));
        v.addView(match);
        TextView modsLine = a.text("", 13, LauncherActivity.kDim);
        v.addView(modsLine);
        CheckBox withMods = check(a, "Include the mods used in this match (helps us reproduce it)", true);
        v.addView(withMods);
        // (the main save: on unless the player turns it off - ProblemReport.addMainSave)
        long saveBytes;
        try {
            saveBytes = ProblemReport.addMainSave(null, game);
        } catch (Exception e) {
            saveBytes = 0;
        }
        CheckBox withSave = check(a, String.format(Locale.US, "Include my main save (story / Universe progress and "
            + "options - not my created superstars or Paint Tool)%s", saveBytes > 0 ? String.format(Locale.US,
            ", %.1f MB", saveBytes / 1048576.0) : ""), saveBytes > 0);
        withSave.setEnabled(saveBytes > 0);
        v.addView(withSave);
        // pictures
        label(a, v, "Pictures (up to 5): screenshots of the problem help a lot.");
        LinearLayout picRows = new LinearLayout(a);
        picRows.setOrientation(LinearLayout.VERTICAL);
        v.addView(picRows);
        Button addPics = a.button("Add pictures…", LauncherActivity.kBackground);
        v.addView(addPics, a.fullWidth(6));
        TextView summary = a.text("", 13, LauncherActivity.kDim);
        summary.setPadding(0, a.dp(10), 0, 0);
        v.addView(summary);
        TextView status = a.text("", 14, LauncherActivity.kRed);
        status.setPadding(0, a.dp(6), 0, 0);
        status.setVisibility(View.GONE);
        v.addView(status);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(v);
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Report a problem")
            .setView(scroll)
            .setPositiveButton("Send report", null)
            .setNegativeButton("Cancel", null)
            .create();
        d.setCanceledOnTouchOutside(false);
        Runnable[] refresh = new Runnable[1];
        refresh[0] = () -> {
            int send = 0;
            long bytes = 0;
            for (ReportSend.Mod m : st.mods)
                if (m.folder != null && !m.onServer) {
                    send++;
                    bytes += m.size;
                }
            withMods.setEnabled(!st.modsBusy && send > 0 && !st.sending);
            modsLine.setText(st.match == null ? "No match picked: the report goes without one." : st.modsNote);
            summary.setText(String.format(Locale.US, "Sends: your answers, the report zip (newest logs, crash reports, "
                + "settings)%s%s%s, %d picture%s - no password, token or internet addresses.",
                withSave.isChecked() ? ", your main save" : "", st.match != null ? ", the match recording" : "",
                st.match != null && withMods.isChecked() && send > 0 ? String.format(Locale.US, ", %d mod%s (%.1f MB)",
                send, send == 1 ? "" : "s", bytes / 1048576.0) : "", st.pics.size(), st.pics.size() == 1 ? "" : "s"));
            picRows.removeAllViews();
            for (File p : st.pics) {
                LinearLayout row = new LinearLayout(a);
                row.setGravity(Gravity.CENTER_VERTICAL);
                TextView name = a.text(p.getName(), 13, LauncherActivity.kText);
                row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
                Button del = a.button("Remove", LauncherActivity.kBackground);
                del.setTextSize(13);
                del.setMinHeight(a.dp(36));
                del.setEnabled(!st.sending);
                del.setOnClickListener(x -> {
                    st.pics.remove(p);
                    p.delete();
                    picRows.post(refresh[0]);
                });
                row.addView(del);
                picRows.addView(row, a.fullWidth(4));
            }
            addPics.setEnabled(st.pics.size() < ReportSend.kMaxImages && !st.sending);
        };
        withSave.setOnCheckedChangeListener((x, on) -> refresh[0].run());
        withMods.setOnCheckedChangeListener((x, on) -> refresh[0].run());
        match.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View view, int pos, long id) {
                pickMatch(a, st, pos == 0 ? null : st.matches.get(pos - 1), refresh[0]);
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        match.setSelection(st.matches.isEmpty() ? 0 : 1);
        if (st.matches.isEmpty()) pickMatch(a, st, null, refresh[0]);
        addPics.setOnClickListener(x -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("image/*");
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"image/png", "image/jpeg", "image/webp"});
            pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            a.startForResult(pick, (code, data) -> {
                if (code != android.app.Activity.RESULT_OK || data == null) return;
                List<Uri> uris = new ArrayList<>();
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null) uris.add(data.getData());
                String why = null;
                for (Uri u : uris) {
                    if (st.pics.size() >= ReportSend.kMaxImages) break;
                    File f = copyPicture(a, u, st.pics.size());
                    String no = f == null ? "A picture couldn't be read, or it's over 5 MB." : ReportSend.imageRefused(f);
                    if (no != null) {
                        why = no;
                        if (f != null) f.delete();
                        continue;
                    }
                    st.pics.add(f);
                }
                status.setTextColor(LauncherActivity.kRed);
                status.setText(why == null ? "" : why);
                status.setVisibility(why == null ? View.GONE : View.VISIBLE);
                refresh[0].run();
            });
        });
        d.setOnDismissListener(x -> {
            st.modsGen++;  // (a mod check still running: its answer is dropped)
            if (!st.sending) clean(a);
        });
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            if (st.sending) return;
            status.setTextColor(LauncherActivity.kRed);
            if (st.modsBusy) {
                status.setText("One moment - the match's mods are still being checked.");
                status.setVisibility(View.VISIBLE);
                return;
            }
            String happened = what.getText().toString().trim();
            if (happened.isEmpty()) {
                status.setText("Please say what happened first.");
                status.setVisibility(View.VISIBLE);
                what.requestFocus();
                return;
            }
            ReportSend.Answers ans = new ReportSend.Answers();
            ans.version = a.versionName();
            ans.os = android.os.Build.VERSION.RELEASE + " (API " + android.os.Build.VERSION.SDK_INT + "), "
                + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL;
            ans.problem = kKinds[kind.getSelectedItemPosition()];
            ans.happens = kAgain[again.getSelectedItemPosition()];
            ans.what = happened;
            ans.before = before.getText().toString().trim();
            if (ReportSend.hasUrl(ans.what) || ReportSend.hasUrl(ans.before)) {
                status.setText(ReportSend.kNoUrls);
                status.setVisibility(View.VISIBLE);
                (ReportSend.hasUrl(ans.what) ? what : before).requestFocus();
                return;
            }
            String[] name = new String[1];
            String bad = ReportSend.discordWhy(contact.getText().toString(), name);
            if (bad != null) {
                status.setText(bad);
                status.setVisibility(View.VISIBLE);
                contact.requestFocus();
                return;
            }
            ans.contact = name[0];
            ReportSend.authToken = a.settings().getString("online_token", "");
            if (ReportSend.authToken.isEmpty()) {
                status.setText("Sign in first: the Online page (Sign in or Create account) - then send.");
                status.setVisibility(View.VISIBLE);
                d.dismiss();
                a.goOnline();
                a.status("Sign in (or create an account), then send your report.");
                return;
            }
            boolean save = withSave.isChecked(), mods = withMods.isChecked() && withMods.isEnabled();
            st.sending = true;
            d.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            match.setEnabled(false);
            refresh[0].run();
            status.setTextColor(LauncherActivity.kDim);
            status.setText("Sending…");
            status.setVisibility(View.VISIBLE);
            final ReportSend.Result[] got = new ReportSend.Result[1];
            File pickedMatch = st.match;
            List<ReportSend.Mod> matchMods = new ArrayList<>(st.mods);
            List<File> pics = new ArrayList<>(st.pics);
            String srv = server(a);
            a.background(() -> got[0] = send(a, srv, game, ans, save, pickedMatch, matchMods, mods, pics,
                (done, total) -> a.runOnUiThread(() -> {
                    if (st.sending) status.setText(String.format(Locale.US, "Sending… %d%% of %.1f MB",
                        total > 0 ? (int) (done * 100 / total) : 0, total / 1048576.0));
                })), () -> {
                st.sending = false;
                ReportSend.Result r = got[0];
                if (r.ok) {
                    clean(a);
                    if (d.isShowing()) d.dismiss();
                    sent(a, r);
                    return;
                }
                if (!d.isShowing()) {  // (closed while sending)
                    clean(a);
                    a.status("The report was not sent: " + r.why);
                    return;
                }
                d.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                d.getButton(AlertDialog.BUTTON_POSITIVE).setText("Try again");
                match.setEnabled(true);
                status.setTextColor(LauncherActivity.kRed);
                status.setText("Not sent: " + r.why + (r.code.isEmpty() ? " Check you're online, then try again." : ""));
                if (ReportSend.needsSignIn(r)) a.status("Sign in again on the Online page, then try again.");
                refresh[0].run();
            });
        }));
        refresh[0].run();
        d.show();
    }

    // A match picked: its mods hashed and checked against the server (in the background).
    static void pickMatch(LauncherActivity a, State st, File m, Runnable refresh) {
        st.match = m;
        st.mods = new ArrayList<>();
        int gen = ++st.modsGen;
        if (m == null) {
            st.modsBusy = false;
            refresh.run();
            return;
        }
        st.modsBusy = true;
        st.modsNote = "Checking the match's mods…";
        refresh.run();
        String srv = server(a);
        File game = InstallActivity.gameFolder();
        final List<List<ReportSend.Mod>> mods = new ArrayList<>();
        final String[] why = new String[1];
        a.background(() -> {
            mods.add(ReportSend.matchMods(game, m));
            why[0] = ReportSend.refuses(srv) != null ? "" : ReportSend.modsCheck(srv, mods.get(0));
        }, () -> {
            if (gen != st.modsGen) return;
            st.modsBusy = false;
            st.mods = mods.get(0);
            st.modsNote = modsText(st.mods, why[0]);
            refresh.run();
        });
    }

    static String modsText(List<ReportSend.Mod> mods, String why) {
        if (mods.isEmpty()) return "This match used no mods.";
        int gone = 0, on = 0, send = 0;
        long bytes = 0;
        for (ReportSend.Mod m : mods) {
            if (m.folder == null) {
                gone++;
            } else if (m.onServer) {
                on++;
            } else {
                send++;
                bytes += m.size;
            }
        }
        if ("off".equals(why)) return String.format(Locale.US, "This match used %d mod%s - the server takes no mods right "
            + "now: the report goes without them.", mods.size(), mods.size() == 1 ? "" : "s");
        StringBuilder t = new StringBuilder(String.format(Locale.US, "This match used %d mod%s", mods.size(),
            mods.size() == 1 ? "" : "s"));
        if (send > 0) t.append(String.format(Locale.US, ": %d to send (%.1f MB)", send, bytes / 1048576.0));
        if (on > 0) t.append(String.format(Locale.US, "%s%d already on the server", send > 0 ? ", " : ": ", on));
        if (gone > 0) t.append(String.format(Locale.US, "%s%d not installed any more", send + on > 0 ? ", " : ": ", gone));
        t.append('.');
        if (why != null && !why.isEmpty()) t.append(" (").append(why).append(")");
        return t.toString();
    }

    // The send (a worker): the zip, the signature, the match and the mods the server lacks, then ReportSend.send.
    static ReportSend.Result send(Context c, String server, File game, ReportSend.Answers ans, boolean withSave,
                                  File match, List<ReportSend.Mod> mods, boolean withMods, List<File> pics,
                                  ReportSend.Progress progress) {
        return send(cache(c), server, game, ans, withSave, match, mods, withMods, pics, progress, false);
    }

    // ... into `cache` (the game's own form: GameReport), the receipt marked picturesLater when the player adds
    // pictures later from the launcher.
    static ReportSend.Result send(File cache, String server, File game, ReportSend.Answers ans, boolean withSave,
                                  File match, List<ReportSend.Mod> mods, boolean withMods, List<File> pics,
                                  ReportSend.Progress progress, boolean picturesLater) {
        cache.mkdirs();
        ReportSend.Result r = new ReportSend.Result();
        String refused = ReportSend.refuses(server);
        if (refused != null) {
            r.why = refused;
            return r;
        }
        String description = "SvR 2011 PC port " + ans.version + " (Android)\n"
            + "Problem: " + ans.problem + "\n"
            + "Happens: " + ans.happens + "\n\n"
            + "What happened:\n" + ans.what + "\n\n"
            + "Just before:\n" + (ans.before.isEmpty() ? "(not said)" : ans.before) + "\n";
        ReportSend.Upload u = new ReportSend.Upload();
        try {
            u.zip = ProblemReport.make(cache, game, ans.version, description, withSave).zip;
        } catch (Exception e) {
            r.why = "The report couldn't be made: " + e.getMessage();
            return r;
        }
        ans.signature = ReportSend.signature(game);
        u.answers = ans;
        u.images = pics;
        u.match = match;
        u.mods = mods;
        u.withMods = withMods;
        u.progress = progress;
        if (withMods)
            for (ReportSend.Mod m : mods) {
                if (m.folder == null || m.onServer) continue;
                File z = new File(cache, "mod-" + m.hash.substring(0, 16) + ".zip");
                if (ReportSend.zipDir(m.folder, z)) m.zip = z;
            }
        try {
            ReportSend.Result got = ReportSend.send(server, u);
            // (its receipt: the reports list asks the server with it - Receipts, the PC's UserData/reports.json)
            if (got.ok && !got.secret.isEmpty())
                Receipts.add(game, ans.app, got.id, got.secret, ans.problem, ans.what, picturesLater);
            if (got.ok) ReportSend.discordSave(game, ans.contact);
            return got;
        } finally {
            if (u.zip != null) u.zip.delete();
            for (ReportSend.Mod m : mods) {
                if (m.zip != null) m.zip.delete();
                m.zip = null;
            }
        }
    }

    // Sent: the id (for a post about it), how often it was seen before.
    static void sent(LauncherActivity a, ReportSend.Result r) {
        a.status("Report sent: " + r.id);
        StringBuilder t = new StringBuilder("Your report reached us. Report id:  " + r.id + "\n\n");
        if (r.seenBefore > 0)
            t.append(String.format(Locale.US, "This problem was already reported %d time%s - yours adds to it, thank "
                + "you.\n\n", r.seenBefore, r.seenBefore == 1 ? "" : "s"));
        if (r.modsDropped || !r.modsWhy.isEmpty())
            t.append("The match's mods didn't go with it: ").append(r.modsWhy).append(".\n\n");
        t.append("If you post about it on our Discord (bug-svr11 forum), give this id.");
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text(t.toString(), 14, LauncherActivity.kText));
        new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Report sent")
            .setView(v)
            .setPositiveButton("Copy the id", (x, w) -> {
                ClipboardManager cm = (ClipboardManager) a.getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("SvR 2011 report id", r.id));
                a.status("Report id copied: " + r.id);
            })
            .setNegativeButton("Close", null)
            .show();
    }

    // A picked picture (a content URI) copied into the cache, 5 MB at most (more: null).
    static File copyPicture(Context c, Uri uri, int n) {
        File out = new File(cache(c), "picture" + n + "-" + System.currentTimeMillis() % 100000);
        try (InputStream in = c.getContentResolver().openInputStream(uri); OutputStream o = new FileOutputStream(out)) {
            if (in == null) return null;
            byte[] b = new byte[1 << 16];
            long total = 0;
            int k;
            while ((k = in.read(b)) > 0) {
                total += k;
                if (total > (5 << 20)) break;
                o.write(b, 0, k);
            }
            if (total > (5 << 20)) {
                o.close();
                out.delete();
                return null;
            }
        } catch (Exception e) {
            out.delete();
            return null;
        }
        String type;
        try {
            type = ReportSend.imageType(ProblemReport.read(out, 16));
        } catch (Exception e) {
            type = null;
        }
        File named = new File(out.getPath() + (type == null ? "" : "." + type.substring(6).replace("jpeg", "jpg")));
        return out.renameTo(named) ? named : out;
    }

    // A match folder's picker line (as report_match_label): people, arena, rule, when, length; "(crashed)" / "(unfinished)".
    static String matchLabel(File dir) {
        StringBuilder people = new StringBuilder();
        String arena = "", rule = "", start = "", result = "";
        int secs = 0;
        try {
            JSONObject j = new JSONObject(new String(ProblemReport.read(new File(dir, "match.json"), 4 << 20),
                StandardCharsets.UTF_8));
            JSONArray pp = j.optJSONArray("people");
            for (int i = 0; pp != null && i < pp.length(); i++) {
                JSONObject p = pp.optJSONObject(i);
                if (p == null || p.optInt("person", 0) >= 6 || p.optString("name", "").isEmpty()) continue;
                people.append(people.length() > 0 ? " vs " : "").append(p.optString("name"));
            }
            JSONObject ar = j.optJSONObject("arena"), ru = j.optJSONObject("rule");
            arena = ar != null ? ar.optString("name", "") : "";
            rule = ru != null ? ru.optString("name", "") : "";
            start = j.optString("start", "");
            secs = j.optInt("length_s", 0);
            result = j.optString("result", "");
        } catch (Exception ignored) {
        }
        StringBuilder t = new StringBuilder(people.length() > 0 ? people.toString() : dir.getName());
        if (!arena.isEmpty()) t.append(" - ").append(arena);
        if (!rule.isEmpty()) t.append(", ").append(rule);
        t.append(" - ").append(start.length() >= 16 ? start.substring(5, 10) + " " + start.substring(11, 16) : "?");
        if (secs > 0) t.append(String.format(Locale.US, ", %d:%02d", secs / 60, secs % 60));
        if (result.equals("crash")) t.append(" (crashed)");
        else if (!new File(dir, "done.txt").isFile()) t.append(" (unfinished)");
        else if (!result.isEmpty() && !result.equals("ended") && !result.equals("running")) t.append(" (").append(result).append(")");
        return t.toString();
    }

    // ── the test aid (LauncherActivity's report_send extra) ──────────────────

    // A send with no window, from an answers file (as the PC launcher's
    // --report-send): UTF-8 lines server= problem= happens= what= before=
    // contact= image=<path> (up to 5) match=<folder | latest | none> mods=1|0
    // save=1|0. server= must be given, and only a test server takes it
    // (ReportSend.refuses). The outcome goes to logcat (SvR2011Report) and
    // <game>/logs/report_send_result.txt.
    static void testSend(LauncherActivity a, File answers) {
        ReportSend.testRun = true;
        a.background(() -> {
            StringBuilder out = new StringBuilder();
            ReportSend.Answers ans = new ReportSend.Answers();
            ans.version = a.versionName();
            ans.os = android.os.Build.VERSION.RELEASE + " (API " + android.os.Build.VERSION.SDK_INT + ")";
            ans.problem = "Something else";
            ans.happens = "Every time";
            String server = "", matchArg = "latest";
            boolean mods = true, save = true;
            List<File> pics = new ArrayList<>();
            File game = InstallActivity.gameFolder();
            try {
                String text = new String(ProblemReport.read(answers, 1 << 20), StandardCharsets.UTF_8);
                if (text.startsWith("﻿")) text = text.substring(1);
                for (String raw : text.split("\n")) {
                    String line = raw.replace("\r", "");
                    int eq = line.indexOf('=');
                    if (eq < 0) continue;
                    String k = line.substring(0, eq), v = line.substring(eq + 1);
                    if (k.equals("server")) server = v;
                    else if (k.equals("problem")) ans.problem = v;
                    else if (k.equals("happens")) ans.happens = v;
                    else if (k.equals("what")) ans.what = v;
                    else if (k.equals("before")) ans.before = v;
                    else if (k.equals("contact")) ans.contact = v;
                    else if (k.equals("token")) ReportSend.authToken = v;   // (a test account's sign-in)
                    else if (k.equals("image") && pics.size() < ReportSend.kMaxImages) pics.add(new File(v));
                    else if (k.equals("match")) matchArg = v;
                    else if (k.equals("mods")) mods = !v.equals("0");
                    else if (k.equals("save")) save = !v.equals("0");
                }
            } catch (Exception e) {
                out.append("not sent: can't read ").append(answers).append('\n');
            }
            if (out.length() == 0 && server.isEmpty())
                out.append("not sent: no server= line in ").append(answers).append(" (give the test server)\n");
            if (out.length() == 0) {   // (2.2.1: the Discord name as the form checks it; else the saved sign-in)
                String[] name = new String[1];
                String bad = ReportSend.discordWhy(ans.contact, name);
                if (bad != null) out.append("not sent: ").append(bad).append('\n');
                else ans.contact = name[0];
                if (ReportSend.authToken.isEmpty()) ReportSend.authToken = a.settings().getString("online_token", "");
                if (bad == null) out.append("signed in: ").append(ReportSend.authToken.isEmpty() ? "no" : "yes").append('\n');
            }
            // (sends when nothing above said "not sent" - the "signed in" line isn't a refusal: with the
            // out.length() test the hook stopped there and never sent)
            if (out.indexOf("not sent") < 0) {
                File match = null;
                if (matchArg.equals("latest")) {
                    List<File> all = ReportSend.matches(game);
                    match = all.isEmpty() ? null : all.get(0);
                } else if (!matchArg.isEmpty() && !matchArg.equals("none")) {
                    match = new File(matchArg).isDirectory() ? new File(matchArg) : null;
                }
                List<ReportSend.Mod> list = new ArrayList<>();
                if (match != null) {
                    list = ReportSend.matchMods(game, match);
                    String why = ReportSend.refuses(server) != null ? "" : ReportSend.modsCheck(server, list);
                    out.append("match: ").append(matchLabel(match)).append('\n').append(modsText(list, why)).append('\n');
                } else {
                    out.append("match: none\n");
                }
                ReportSend.Result r = send(a, server, game, ans, save, match, list, mods, pics, null);
                out.append(r.ok ? "sent " + r.id + " seen_before " + r.seenBefore : "not sent: " + r.why + " (HTTP "
                    + r.status + ")").append('\n');
                if (r.ok && match != null)
                    out.append("match ").append(r.matchStored ? "stored" : "not stored").append(", mods sent ")
                        .append(r.modsSent).append(", stored ").append(r.modsStored)
                        .append(r.modsWhy.isEmpty() ? "" : " (" + r.modsWhy + ")").append('\n');
            }
            android.util.Log.i("SvR2011Report", out.toString());
            File logs = new File(game, "logs");
            logs.mkdirs();
            try (OutputStream o = new FileOutputStream(new File(logs, "report_send_result.txt"))) {
                o.write(out.toString().getBytes(StandardCharsets.UTF_8));
            } catch (Exception ignored) {
            }
        }, () -> a.status("Report test: done (logs/report_send_result.txt)"));
    }

    static void label(LauncherActivity a, LinearLayout v, String s) {
        TextView t = a.text(s, 14, LauncherActivity.kText);
        t.setPadding(0, a.dp(14), 0, a.dp(2));
        v.addView(t);
    }

    static CheckBox check(LauncherActivity a, String s, boolean on) {
        CheckBox c = new CheckBox(a);
        c.setText(s);
        c.setTextColor(LauncherActivity.kText);
        c.setChecked(on);
        c.setPadding(0, a.dp(10), 0, 0);
        return c;
    }

    static Spinner spinner(LauncherActivity a, String[] items) {
        Spinner sp = new Spinner(a, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> ad = new ArrayAdapter<>(a, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        return sp;
    }

    static EditText box(LauncherActivity a, int lines) {
        EditText e = new EditText(a);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        e.setMinLines(lines);
        e.setGravity(Gravity.TOP | Gravity.START);
        e.setTextColor(LauncherActivity.kText);
        return e;
    }
}
