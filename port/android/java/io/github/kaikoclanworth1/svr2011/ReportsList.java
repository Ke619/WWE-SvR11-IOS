// WWE SmackDown vs. Raw 2011 - "My reports" on the phone, as the PC launcher's
// Reports tab (launcher/reports_tab.c): the reports this game folder sent
// (Receipts - the phone's, and the PC's when it's the same folder) and what
// became of them: received, being looked at, fixed for the next update, or a
// question, which can be answered here. A report can be changed until it's done
// or a duplicate (Edit / add pictures: text to add, pictures - 5 a report in
// all); the game's "add pictures later" ones say "Add your pictures". Asked each
// time it opens.
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

final class ReportsList {
    static final int kGreen = 0xFF34C759, kAmber = 0xFFF0A028;

    static void show(LauncherActivity a) {
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        TextView status = a.text("Asking the server…", 13, LauncherActivity.kDim);
        v.addView(status);
        LinearLayout rows = new LinearLayout(a);
        rows.setOrientation(LinearLayout.VERTICAL);
        v.addView(rows);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(v);
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("My reports")
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
        List<Receipts.Item> items = Receipts.load(game);
        fill(a, rows, status, items);
        if (items.isEmpty() && token.isEmpty()) {
            status.setText("");
            return;
        }
        status.setText("Asking the server…");
        final String[] why = new String[1];
        a.background(() -> why[0] = Receipts.status(server, token, items), () -> {
            int asked = 0, pictures = 0;
            for (Receipts.Item it : items) {
                if (it.state.equals("needs_info")) asked++;
                if (Receipts.wantsPictures(it)) pictures++;
            }
            status.setText(why[0] != null ? "Couldn't ask the server: " + why[0]
                : asked > 0 ? asked + " report" + (asked == 1 ? " needs" : "s need") + " your answer."
                : pictures > 0 ? pictures + " report" + (pictures == 1 ? " waits" : "s wait") + " for your pictures." : "");
            fill(a, rows, status, items);
        });
    }

    static void fill(LauncherActivity a, LinearLayout rows, TextView status, List<Receipts.Item> items) {
        rows.removeAllViews();
        if (items.isEmpty()) {
            TextView t = a.text("Reports you send from Report a problem show up here.", 14, LauncherActivity.kDim);
            t.setGravity(Gravity.CENTER);
            t.setPadding(0, a.dp(24), 0, a.dp(24));
            rows.addView(t);
            return;
        }
        for (Receipts.Item it : items) {
            LinearLayout row = new LinearLayout(a);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(0, a.dp(12), 0, a.dp(4));
            String from = it.app.equals("modmaker") ? " (Mod Maker)" : it.app.equals("launcher") ? " (PC)"
                : it.app.equals("game") ? " (game)" : "";
            TextView title = a.text(it.date + " - " + it.problem + from, 15, LauncherActivity.kText);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            row.addView(title);
            if (!it.what.isEmpty()) row.addView(a.text("“" + it.what + "”", 13, LauncherActivity.kDim));
            int color = it.state.equals("fixed") ? kGreen : it.state.equals("needs_info") ? kAmber : LauncherActivity.kText;
            if (Receipts.wantsPictures(it))
                row.addView(a.text("Add your pictures: you sent this from the game without them.", 14, kAmber));
            row.addView(a.text(Receipts.badge(it) + " - " + Receipts.line(it), 14, color));
            if (it.edits > 0) row.addView(a.text("You changed it " + it.edits + " time" + (it.edits == 1 ? "" : "s")
                + " - it has " + it.pictures + " picture" + (it.pictures == 1 ? "." : "s."), 12, LauncherActivity.kDim));
            if (it.fromAccount) row.addView(a.text("(Sent from another device with your online account.)", 12,
                LauncherActivity.kDim));
            boolean asked = it.state.equals("needs_info") || (it.state.equals("replied") && it.replies < 5);
            if (asked) {
                Button answer = a.button(it.state.equals("replied") ? "Add to your answer" : "Answer", LauncherActivity.kRed);
                answer.setOnClickListener(x -> answer(a, it, rows, status));
                row.addView(answer, a.fullWidth(6));
            }
            if (Receipts.editable(it)) {
                Button change = a.button("Edit / add pictures", Receipts.wantsPictures(it) ? LauncherActivity.kRed
                    : LauncherActivity.kBackground);
                change.setOnClickListener(x -> edit(a, it, rows, status));
                row.addView(change, a.fullWidth(6));
            }
            rows.addView(row);
        }
    }

    // A change to the report: text to add to what happened and pictures (what the
    // report has room for, 5 in all), sent through the server's name filter.
    static void edit(LauncherActivity a, Receipts.Item it, LinearLayout rows, TextView status) {
        final int room = Math.max(0, Receipts.kMaxEditImages - it.pictures);
        final List<File> pics = new ArrayList<>();
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text(it.problem + (it.what.isEmpty() ? "" : ": “" + it.what + "”"), 14, LauncherActivity.kDim));
        EditText text = new EditText(a);
        text.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        text.setMinLines(4);
        text.setGravity(Gravity.TOP | Gravity.START);
        text.setTextColor(LauncherActivity.kText);
        text.setHint("What you left out (optional)");
        v.addView(text);
        TextView picsLine = a.text("", 13, LauncherActivity.kDim);
        v.addView(picsLine);
        Button addPics = a.button("Add pictures…", LauncherActivity.kBackground);
        v.addView(addPics, a.fullWidth(6));
        TextView error = a.text("", 14, LauncherActivity.kRed);
        error.setVisibility(View.GONE);
        v.addView(error);
        Runnable showPics = () -> {
            StringBuilder names = new StringBuilder();
            for (File p : pics) names.append(names.length() > 0 ? ", " : "").append(p.getName());
            picsLine.setText(room == 0 ? "This report has all 5 pictures it can have."
                : pics.isEmpty() ? "No pictures (up to " + room + ")." : pics.size() + " picture" + (pics.size() == 1 ? "" : "s")
                + ": " + names);
            addPics.setEnabled(pics.size() < room);
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
                    if (pics.size() >= room) break;
                    File f = ReportForm.copyPicture(a, u, 90 + pics.size());   // (90+: not the report form's names)
                    String no = f == null ? "A picture couldn't be read, or it's over 5 MB." : ReportSend.imageRefused(f);
                    if (no != null) {
                        why = no;
                        if (f != null) f.delete();
                        continue;
                    }
                    pics.add(f);
                }
                error.setTextColor(LauncherActivity.kRed);
                error.setText(why == null ? "" : why);
                error.setVisibility(why == null ? View.GONE : View.VISIBLE);
                showPics.run();
            });
        });
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Add to your report")
            .setView(v)
            .setPositiveButton("Save changes", null)
            .setNegativeButton("Cancel", null)
            .create();
        d.setOnDismissListener(x -> {
            for (File p : pics) p.delete();   // (the copies in the cache)
        });
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            String t = text.getText().toString().trim();
            if (t.isEmpty() && pics.isEmpty()) {
                error.setTextColor(LauncherActivity.kRed);
                error.setText("Write what to add, or add a picture.");
                error.setVisibility(View.VISIBLE);
                return;
            }
            b.setEnabled(false);
            addPics.setEnabled(false);
            error.setTextColor(LauncherActivity.kDim);
            error.setText("Saving…");
            error.setVisibility(View.VISIBLE);
            String server = ReportForm.server(a), token = a.settings().getString("online_token", "");
            final String[] why = new String[1];
            final List<File> send = new ArrayList<>(pics);
            a.background(() -> why[0] = Receipts.edit(server, token, it, t, send), () -> {
                if (why[0] == null) {
                    d.dismiss();
                    a.status(send.isEmpty() ? "Thanks - your report was updated." : "Thanks - your report was updated with your pictures.");
                    load(a, rows, status);
                    return;
                }
                b.setEnabled(true);
                showPics.run();
                error.setTextColor(LauncherActivity.kRed);
                error.setText("Not saved: " + why[0]);
            });
        }));
        d.show();
    }

    // Our question, answered: text (sent through the server's name filter).
    static void answer(LauncherActivity a, Receipts.Item it, LinearLayout rows, TextView status) {
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text(it.question, 14, LauncherActivity.kText));
        EditText text = new EditText(a);
        text.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        text.setMinLines(4);
        text.setGravity(Gravity.TOP | Gravity.START);
        text.setTextColor(LauncherActivity.kText);
        text.setHint("Your answer");
        v.addView(text);
        TextView error = a.text("", 14, LauncherActivity.kRed);
        error.setVisibility(View.GONE);
        v.addView(error);
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("We need more information")
            .setView(v)
            .setPositiveButton("Send answer", null)
            .setNegativeButton("Cancel", null)
            .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            String t = text.getText().toString().trim();
            if (t.isEmpty()) {
                error.setText("Write your answer first.");
                error.setVisibility(View.VISIBLE);
                return;
            }
            b.setEnabled(false);
            error.setTextColor(LauncherActivity.kDim);
            error.setText("Sending…");
            error.setVisibility(View.VISIBLE);
            String server = ReportForm.server(a), token = a.settings().getString("online_token", "");
            final String[] why = new String[1];
            a.background(() -> why[0] = Receipts.reply(server, token, it, t, new ArrayList<>()), () -> {
                if (why[0] == null) {
                    d.dismiss();
                    a.status("Thanks - your answer was sent.");
                    load(a, rows, status);
                    return;
                }
                b.setEnabled(true);
                error.setTextColor(LauncherActivity.kRed);
                error.setText("Not sent: " + why[0] + " Check you're online, then try again.");
            });
        }));
        d.show();
    }
}
