// WWE SmackDown vs. Raw 2011 - "My suggestions" on the phone, as the PC
// launcher's Suggestions tab (launcher/suggestions_tab.c): Send an idea (a
// title, what it would do, up to 3 pictures) - a report of kind "suggestion"
// (ReportSend, no zip) - and the ideas this game folder sent (Receipts,
// UserData/suggestions.json, the PC's file too) with our answer: Sure
// (planned), No (not planned), or Done in <version>, each with an optional
// note. Asked each time it opens.
package io.github.kaikoclanworth1.svr2011;

import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

final class SuggestionsList {
    static final int kMaxPictures = 3;
    // (on the page, above the form, and again before Send - as the PC launcher's Suggestions tab)
    static final String kAbout = "Suggestions are for the game and its tools (the launcher, the Mod Maker, the porters)"
        + " - not requests for mods. Ask mod makers for mods on Discord.";

    static void show(LauncherActivity a) {
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text(kAbout, 14, LauncherActivity.kText));
        v.addView(a.text("We read every one and answer it here: Sure, No (not planned), or Done in an update.", 13,
            LauncherActivity.kDim));
        Button send = a.button("Send an idea…", LauncherActivity.kRed);
        v.addView(send, a.fullWidth(8));
        TextView status = a.text("", 13, LauncherActivity.kDim);
        v.addView(status);
        LinearLayout rows = new LinearLayout(a);
        rows.setOrientation(LinearLayout.VERTICAL);
        v.addView(rows);
        send.setOnClickListener(x -> form(a, rows, status));
        ScrollView scroll = new ScrollView(a);
        scroll.addView(v);
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("My suggestions")
            .setView(scroll)
            .setPositiveButton("Refresh", null)
            .setNegativeButton("Close", null)
            .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> load(a, rows, status)));
        d.show();
        load(a, rows, status);
    }

    static void load(LauncherActivity a, LinearLayout rows, TextView status) {
        File game = InstallActivity.gameFolder();
        String server = ReportForm.server(a), token = a.settings().getString("online_token", "");
        List<Receipts.Item> items = Receipts.load(game, Receipts.kSuggestions);
        fill(a, rows, items);
        if (items.isEmpty() && token.isEmpty()) {
            status.setText("");
            return;
        }
        status.setText("Asking the server…");
        final String[] why = new String[1];
        a.background(() -> why[0] = Receipts.status(server, token, items, "suggestion"), () -> {
            int answered = 0;
            for (Receipts.Item it : items) if (!it.state.isEmpty() && !it.state.equals("received")) answered++;
            status.setText(why[0] != null ? "Couldn't ask the server: " + why[0]
                : answered > 0 ? answered + " of your ideas " + (answered == 1 ? "has" : "have") + " an answer." : "");
            fill(a, rows, items);
        });
    }

    static void fill(LauncherActivity a, LinearLayout rows, List<Receipts.Item> items) {
        rows.removeAllViews();
        if (items.isEmpty()) {
            TextView t = a.text("Ideas you send show up here, with our answer.", 14, LauncherActivity.kDim);
            t.setGravity(Gravity.CENTER);
            t.setPadding(0, a.dp(24), 0, a.dp(24));
            rows.addView(t);
            return;
        }
        for (Receipts.Item it : items) {
            it.kind = "suggestion";
            LinearLayout row = new LinearLayout(a);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(0, a.dp(12), 0, a.dp(4));
            String from = it.app.equals("launcher") ? " (PC)" : "";
            TextView title = a.text(it.date + " - " + it.problem + from, 15, LauncherActivity.kText);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            row.addView(title);
            if (!it.what.isEmpty()) row.addView(a.text("“" + it.what + "”", 13, LauncherActivity.kDim));
            int color = it.state.equals("planned") || it.state.equals("fixed") ? ReportsList.kGreen
                : it.state.equals("not_planned") ? LauncherActivity.kDim : LauncherActivity.kText;
            row.addView(a.text(Receipts.badge(it) + " - " + Receipts.line(it), 14, color));
            if (it.fromAccount) row.addView(a.text("(Sent from another device with your online account.)", 12,
                LauncherActivity.kDim));
            rows.addView(row);
        }
    }

    // Send an idea: a title, the idea, up to 3 pictures.
    static void form(LauncherActivity a, LinearLayout rows, TextView listStatus) {
        final List<File> pics = new ArrayList<>();
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text(kAbout, 13, LauncherActivity.kDim));
        EditText title = new EditText(a);
        title.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        title.setTextColor(LauncherActivity.kText);
        title.setHint("Title");
        title.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(110)});
        v.addView(title);
        EditText text = new EditText(a);
        text.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        text.setMinLines(4);
        text.setGravity(Gravity.TOP | Gravity.START);
        text.setTextColor(LauncherActivity.kText);
        text.setHint("Your idea - what it would do, and where");
        text.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(3000)});
        v.addView(text);
        EditText discord = new EditText(a);   // (2.2.1: needed - the contact, only we see it)
        discord.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        discord.setTextColor(LauncherActivity.kText);
        discord.setHint("Your Discord name (needed - only we see it)");
        discord.setText(ReportSend.discordLoad(InstallActivity.gameFolder()));
        v.addView(discord);
        if (a.settings().getString("online_token", "").isEmpty())
            v.addView(a.text("Not signed in: sign in (or create an account) on the Online page first - ideas come "
                + "from an online account.", 14, LauncherActivity.kRed));
        TextView picsLine = a.text("", 13, LauncherActivity.kDim);
        v.addView(picsLine);
        Button addPics = a.button("Add a picture…", LauncherActivity.kBackground);
        v.addView(addPics, a.fullWidth(6));
        TextView error = a.text("", 14, LauncherActivity.kRed);
        error.setVisibility(View.GONE);
        v.addView(error);
        Runnable showPics = () -> {
            StringBuilder names = new StringBuilder();
            for (File p : pics) names.append(names.length() > 0 ? ", " : "").append(p.getName());
            picsLine.setText(pics.isEmpty() ? "No pictures (up to " + kMaxPictures + ", optional)." : pics.size()
                + " picture" + (pics.size() == 1 ? "" : "s") + ": " + names);
            addPics.setEnabled(pics.size() < kMaxPictures);
        };
        showPics.run();
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
                    if (pics.size() >= kMaxPictures) break;
                    File f = ReportForm.copyPicture(a, u, 80 + pics.size());   // (80+: not the report form's names)
                    String no = f == null ? "A picture couldn't be read, or it's over 5 MB." : ReportSend.imageRefused(f);
                    if (no != null) {
                        why = no;
                        if (f != null) f.delete();
                        continue;
                    }
                    pics.add(f);
                }
                error.setText(why == null ? "" : why);
                error.setVisibility(why == null ? View.GONE : View.VISIBLE);
                showPics.run();
            });
        });
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Send an idea")
            .setView(v)
            .setPositiveButton("Send idea", null)
            .setNegativeButton("Cancel", null)
            .create();
        d.setOnDismissListener(x -> {
            for (File p : pics) p.delete();   // (the copies in the cache)
        });
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            String t = title.getText().toString().trim(), w = text.getText().toString().trim();
            if (t.isEmpty() || w.isEmpty()) {
                error.setText(t.isEmpty() ? "Give your idea a short title first."
                    : "Write a line or two about your idea: what it would do, and where.");
                error.setVisibility(View.VISIBLE);
                return;
            }
            if (ReportSend.hasUrl(t) || ReportSend.hasUrl(w)) {
                error.setText(ReportSend.kNoUrlsIdeas);
                error.setVisibility(View.VISIBLE);
                return;
            }
            String[] name = new String[1];
            String bad = ReportSend.discordWhy(discord.getText().toString(), name);
            if (bad != null) {
                error.setText(bad);
                error.setVisibility(View.VISIBLE);
                discord.requestFocus();
                return;
            }
            ReportSend.authToken = a.settings().getString("online_token", "");
            if (ReportSend.authToken.isEmpty()) {
                d.dismiss();
                a.goOnline();
                a.status("Sign in (or create an account), then send your idea.");
                return;
            }
            Runnable go = () -> sendIdea(a, d, b, error, rows, listStatus, pics, t, w, name[0]);
            new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)   // (the reminder again before it goes)
                .setTitle("Before you send it")
                .setMessage(kAbout + "\n\nSend this idea?")
                .setPositiveButton("Send", (x2, y2) -> go.run())
                .setNegativeButton("Cancel", null)
                .show();
        }));
        d.show();
    }

    static void sendIdea(LauncherActivity a, AlertDialog d, View b, TextView error, LinearLayout rows,
                         TextView listStatus, List<File> pics, String t, String w, String contact) {
        {
            ReportSend.Answers ans = new ReportSend.Answers();
            ans.kind = "suggestion";
            ans.contact = contact;
            ans.version = a.versionName();
            ans.os = android.os.Build.VERSION.RELEASE + " (API " + android.os.Build.VERSION.SDK_INT + "), "
                + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL;
            ans.problem = t;
            ans.what = w;
            ReportSend.Upload u = new ReportSend.Upload();
            u.answers = ans;
            u.images.addAll(pics);
            String server = ReportForm.server(a);
            File game = InstallActivity.gameFolder();
            b.setEnabled(false);
            error.setVisibility(View.GONE);
            final ReportSend.Result[] got = new ReportSend.Result[1];
            a.background(() -> {
                got[0] = ReportSend.send(server, u);
                if (got[0].ok && !got[0].secret.isEmpty())
                    Receipts.add(Receipts.kSuggestions, game, ans.app, got[0].id, got[0].secret, t, w, false);
                if (got[0].ok) ReportSend.discordSave(game, ans.contact);
            }, () -> {
                if (got[0].ok) {
                    d.dismiss();
                    load(a, rows, listStatus);
                } else {
                    b.setEnabled(true);
                    error.setText("Not sent: " + got[0].why);
                    error.setVisibility(View.VISIBLE);
                }
            });
        }
    }
}
