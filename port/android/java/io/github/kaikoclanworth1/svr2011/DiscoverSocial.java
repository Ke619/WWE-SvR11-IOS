// WWE SmackDown vs. Raw 2011 - Android launcher: a Discovery mod's stars and comments (docs/DISCOVERY_API.md,
// "Comments and ratings"). Under the description on a mod's page: the average and the player's own stars, then the
// comments (newest first, 20 a page) with a box to write one. Reading needs no account; the rest needs the Online
// tab's sign-in (signed out: "Sign in to ..." opens the Online tab). Each part hides while the server has it off.
// Every refusal shows the server's own words ("error"). Comment text is shown as plain text only (no links).
package io.github.kaikoclanworth1.svr2011;

import android.graphics.Typeface;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;

final class DiscoverSocial {
    static final int kMaxChars = 500, kMaxLines = 8;

    private final LauncherActivity a_;
    private final DiscoverPage page_;
    private LinearLayout rateCard_, commentCard_, stars_, commentList_, writeBox_;
    private TextView avg_, rateNote_, commentHead_, counter_, noComment_;
    private Button rateSignIn_, commentSignIn_, send_, more_;
    private final Button[] starButtons_ = new Button[5];
    private EditText box_;
    private int id_, gen_, myRating_, total_, page_n_;
    private boolean busy_;
    private String token_ = "";  // (the token the page was asked with: its can_* and mine are for it)

    DiscoverSocial(LauncherActivity a, DiscoverPage page) {
        a_ = a;
        page_ = page;
    }

    // The stars' card (under the description) and the comments' card (under the page's buttons).
    void buildRating(LinearLayout parent) {
        rateCard_ = a_.card(parent, "Rating");
        avg_ = a_.text("", 15, LauncherActivity.kText);
        avg_.setPadding(0, a_.dp(10), 0, a_.dp(6));
        rateCard_.addView(avg_);
        stars_ = new LinearLayout(a_);
        for (int k = 0; k < 5; k++) {
            final int n = k + 1;
            Button b = a_.button("☆", LauncherActivity.kCard);
            b.setTextSize(22);
            b.setOnClickListener(v -> rate(n));
            starButtons_[k] = b;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
            lp.rightMargin = a_.dp(k < 4 ? 6 : 0);
            stars_.addView(b, lp);
        }
        rateCard_.addView(stars_, a_.fullWidth(4));
        rateNote_ = a_.text("", 13, LauncherActivity.kDim);
        rateNote_.setPadding(0, a_.dp(4), 0, 0);
        rateCard_.addView(rateNote_);
        rateSignIn_ = a_.button("Sign in to rate it", LauncherActivity.kCard);
        rateSignIn_.setOnClickListener(v -> a_.goOnline());
        rateCard_.addView(rateSignIn_, a_.fullWidth(6));
        hint(rateCard_);
        showCard(rateCard_, false);
    }

    void buildComments(LinearLayout parent) {
        commentCard_ = a_.card(parent, "Comments");
        commentHead_ = a_.text("", 13, LauncherActivity.kDim);
        commentHead_.setPadding(0, a_.dp(8), 0, a_.dp(4));
        commentCard_.addView(commentHead_);
        writeBox_ = new LinearLayout(a_);
        writeBox_.setOrientation(LinearLayout.VERTICAL);
        commentCard_.addView(writeBox_);
        box_ = a_.field("Write a comment (plain text - no links)", kMaxChars, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        box_.setSingleLine(false);
        box_.setMinLines(2);
        box_.setMaxLines(kMaxLines);
        box_.setGravity(Gravity.START | Gravity.TOP);
        writeBox_.addView(box_, a_.fullWidth(4));
        counter_ = a_.text("0 / " + kMaxChars, 12, LauncherActivity.kDim);
        counter_.setGravity(Gravity.END);
        writeBox_.addView(counter_);
        box_.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) { counter_.setText(s.length() + " / " + kMaxChars); }
        });
        send_ = a_.button("Send", LauncherActivity.kRed);
        send_.setOnClickListener(v -> post());
        writeBox_.addView(send_, a_.fullWidth(6));
        noComment_ = a_.text("Your account can't comment on Discovery.", 13, LauncherActivity.kDim);
        commentCard_.addView(noComment_);
        commentSignIn_ = a_.button("Sign in on the Online tab to write a comment", LauncherActivity.kCard);
        commentSignIn_.setOnClickListener(v -> a_.goOnline());
        commentCard_.addView(commentSignIn_, a_.fullWidth(6));
        commentList_ = new LinearLayout(a_);
        commentList_.setOrientation(LinearLayout.VERTICAL);
        commentCard_.addView(commentList_, a_.fullWidth(6));
        more_ = a_.button("More comments", LauncherActivity.kCard);
        more_.setOnClickListener(v -> load(page_n_ + 1));
        commentCard_.addView(more_, a_.fullWidth(6));
        hint(commentCard_);
        clear();
    }

    private void hint(LinearLayout card) {
        View pad = new View(a_);
        card.addView(pad, new LinearLayout.LayoutParams(1, a_.dp(8)));
    }

    // A card and its heading (the view before it on the page) shown or hidden together.
    private void showCard(LinearLayout card, boolean on) {
        card.setVisibility(on ? View.VISIBLE : View.GONE);
        ViewGroup p = (ViewGroup) card.getParent();
        int i = p.indexOfChild(card);
        if (i > 0) p.getChildAt(i - 1).setVisibility(on ? View.VISIBLE : View.GONE);
    }

    // A new page opening: nothing of the last mod's.
    void clear() {
        id_ = 0;
        busy_ = false;
        commentList_.removeAllViews();
        showCard(rateCard_, false);
        showCard(commentCard_, false);
    }

    // The mod's details came (GET /api/mods/{id}, asked with `token`): its stars, then its first comments.
    void show(JSONObject m, int gen, String token) {
        id_ = m.optInt("id");
        gen_ = gen;
        token_ = token;
        boolean signedIn = !token.isEmpty();
        boolean ratingsOn = m.optBoolean("ratings_on", m.has("rating"));
        showCard(rateCard_, ratingsOn);
        if (ratingsOn) {
            myRating_ = m.optInt("my_rating", 0);
            showAverage(m.optJSONObject("rating"));
            boolean canRate = signedIn && m.optBoolean("can_rate", false);
            stars_.setVisibility(canRate ? View.VISIBLE : View.GONE);
            rateNote_.setText(canRate ? "Tap a star to rate it." : !signedIn ? ""
                : m.optBoolean("mine", false) ? "Your own mod" : "You can't rate mods");
            rateNote_.setVisibility(rateNote_.getText().length() > 0 ? View.VISIBLE : View.GONE);
            rateSignIn_.setVisibility(signedIn ? View.GONE : View.VISIBLE);
            paintStars();
        }
        boolean commentsOn = m.optBoolean("comments_on", true);
        showCard(commentCard_, commentsOn);
        if (commentsOn) {
            canComment(signedIn, m.optBoolean("can_comment", false));
            commentSignIn_.setVisibility(signedIn ? View.GONE : View.VISIBLE);
            commentHead_.setText("Loading the comments…");
            more_.setVisibility(View.GONE);
            load(0);
        }
    }

    private void canComment(boolean signedIn, boolean can) {
        writeBox_.setVisibility(signedIn && can ? View.VISIBLE : View.GONE);
        noComment_.setVisibility(signedIn && !can ? View.VISIBLE : View.GONE);
    }

    private boolean current(int id, int gen) { return id == id_ && id != 0 && page_.genNow() == gen; }

    private void showAverage(JSONObject r) {
        int count = r != null ? r.optInt("count", 0) : 0;
        double avg = r != null ? r.optDouble("avg", 0) : 0;
        avg_.setText(count == 0 ? "No ratings yet"
            : "★ " + String.format(java.util.Locale.US, "%.1f", avg) + "  (" + count + " rating" + (count == 1 ? "" : "s") + ")"
              + (myRating_ > 0 ? "  -  yours: " + myRating_ : ""));
    }

    private void paintStars() {
        for (int k = 0; k < 5; k++) {
            boolean lit = k < myRating_;
            starButtons_[k].setText(lit ? "★" : "☆");
            starButtons_[k].setTextColor(lit ? 0xFFFFC83D : 0xFFFFFFFF);
        }
    }

    // ── stars ──────────────────────────────────────────────────────────────

    // A star tapped: that many; the same again takes the rating back (0).
    void rate(int n) {
        if (busy_ || id_ == 0 || token_.isEmpty()) return;
        final int stars = n == myRating_ ? 0 : n;
        final int id = id_, gen = gen_;
        busy_ = true;
        a_.status("Saving your rating…");
        final byte[] body = ("{\"stars\":" + stars + "}").getBytes(StandardCharsets.UTF_8);
        final DiscoverPage.Answer[] r = new DiscoverPage.Answer[1];
        final String t = token_;
        a_.background(() -> r[0] = page_.send("POST", "/api/mods/" + id + "/rating", "application/json", body, t), () -> {
            busy_ = false;
            if (!current(id, gen)) return;
            if (!refused(r[0], "Rating")) {
                JSONObject j = DiscoverPage.json(r[0]);
                myRating_ = j.optInt("my_rating", stars);
                showAverage(j.optJSONObject("rating"));
                paintStars();
                a_.status(myRating_ == 0 ? "Your rating was taken back." : "Rated " + myRating_ + " star"
                    + (myRating_ == 1 ? "" : "s") + " - tap the same star again to take it back.");
            }
        });
    }

    // ── comments ───────────────────────────────────────────────────────────

    void load(final int n) {
        final int id = id_, gen = gen_;
        if (id == 0) return;
        if (n > 0) {
            more_.setEnabled(false);
            a_.status("Loading more comments…");
        }
        final DiscoverPage.Answer[] r = new DiscoverPage.Answer[1];
        a_.background(() -> r[0] = page_.get("/api/mods/" + id + "/comments?page=" + n, 1 << 20, gen), () -> {
            more_.setEnabled(true);
            if (!current(id, gen)) return;
            if (n > 0) a_.status("");
            if (r[0].status != 200) {
                commentHead_.setText("The comments didn't load.");
                say(r[0], "Comments");
                return;
            }
            JSONObject j = DiscoverPage.json(r[0]);
            if (!j.optBoolean("enabled", true)) {
                showCard(commentCard_, false);
                return;
            }
            page_n_ = n;
            total_ = j.optInt("total", 0);
            if (n == 0) commentList_.removeAllViews();
            JSONArray list = j.optJSONArray("comments");
            for (int i = 0; list != null && i < list.length(); i++) {
                JSONObject c = list.optJSONObject(i);
                if (c != null) commentList_.addView(row(c));
            }
            relines();
            if (j.has("can_comment") && !token_.isEmpty()) canComment(true, j.optBoolean("can_comment"));
            int per = j.optInt("per_page", 20);
            more_.setVisibility((page_n_ + 1) * per < total_ ? View.VISIBLE : View.GONE);
            head();
        });
    }

    private void head() {
        commentHead_.setText("Comments (" + total_ + ")" + (total_ > 0 ? ", newest first" : ""));
    }

    static String age(long created) {
        long s = Math.max(0, System.currentTimeMillis() / 1000 - created);
        if (s < 60) return "just now";
        if (s < 3600) return (s / 60) + " min ago";
        if (s < 86400) return (s / 3600) + " h ago";
        if (s < 30 * 86400L) return (s / 86400) + " day" + (s / 86400 == 1 ? "" : "s") + " ago";
        return DiscoverPage.date(created);
    }

    private View row(final JSONObject c) {
        final LinearLayout r = new LinearLayout(a_);
        r.setOrientation(LinearLayout.VERTICAL);
        View sep = new View(a_);  // (a line above each comment but the first: relines())
        sep.setBackgroundColor(LauncherActivity.kLine);
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1);
        sl.bottomMargin = a_.dp(8);
        r.addView(sep, sl);
        r.setPadding(0, 0, 0, a_.dp(8));
        TextView who = a_.text(c.optString("author") + "  -  " + age(c.optLong("created")), 13, LauncherActivity.kDim);
        who.setTypeface(Typeface.DEFAULT_BOLD);
        r.addView(who);
        TextView text = a_.text(c.optString("text"), 15, LauncherActivity.kText);  // (plain text: no links, no markup)
        text.setAutoLinkMask(0);
        text.setLinksClickable(false);
        text.setTextIsSelectable(true);
        text.setPadding(0, a_.dp(2), 0, a_.dp(4));
        r.addView(text);
        boolean signedIn = !token_.isEmpty();
        boolean canDelete = signedIn && c.optBoolean("can_delete", false);
        boolean canReport = signedIn && !c.optBoolean("mine", false);
        if (canDelete || canReport) {
            LinearLayout acts = new LinearLayout(a_);
            acts.setGravity(Gravity.END);
            if (canReport) acts.addView(small("Report", () -> report(c, r)));
            if (canDelete) acts.addView(small("Delete", () -> delete(c, r)));
            r.addView(acts);
        }
        return r;
    }

    private void relines() {
        for (int i = 0; i < commentList_.getChildCount(); i++)
            ((ViewGroup) commentList_.getChildAt(i)).getChildAt(0).setVisibility(i == 0 ? View.GONE : View.VISIBLE);
    }

    private Button small(String label, Runnable onTap) {
        Button b = a_.button(label, LauncherActivity.kCard);
        b.setTextSize(13);
        b.setMinHeight(a_.dp(36));
        b.setMinimumHeight(a_.dp(36));
        b.setPadding(a_.dp(14), 0, a_.dp(14), 0);
        b.setOnClickListener(v -> onTap.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, a_.dp(36));
        lp.leftMargin = a_.dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    void post() {
        if (busy_ || id_ == 0 || token_.isEmpty()) return;
        final String text = box_.getText().toString().trim();
        if (text.isEmpty()) {
            a_.status("Write something first.");
            return;
        }
        if (text.length() > kMaxChars) {
            a_.status("Comments are " + kMaxChars + " characters at most.");
            return;
        }
        if (text.split("\n", -1).length > kMaxLines) {
            a_.status("Comments are " + kMaxLines + " lines at most.");
            return;
        }
        final int id = id_, gen = gen_;
        final String t = token_;
        busy_ = true;
        send_.setEnabled(false);
        a_.status("Sending your comment…");
        final byte[] body;
        try {
            body = new JSONObject().put("text", text).toString().getBytes(StandardCharsets.UTF_8);
        } catch (org.json.JSONException e) {
            busy_ = false;
            send_.setEnabled(true);
            return;
        }
        final DiscoverPage.Answer[] r = new DiscoverPage.Answer[1];
        a_.background(() -> r[0] = page_.send("POST", "/api/mods/" + id + "/comments", "application/json", body, t), () -> {
            busy_ = false;
            send_.setEnabled(true);
            if (!current(id, gen)) return;
            if (refused(r[0], "Comment")) return;
            JSONObject j = DiscoverPage.json(r[0]);
            JSONObject c = j.optJSONObject("comment");
            box_.setText("");
            total_ = j.optInt("total", total_ + 1);
            if (c != null) commentList_.addView(row(c), 0);
            relines();
            head();
            a_.status("Your comment is up.");
        });
    }

    void delete(final JSONObject c, final View r) {
        if (busy_ || token_.isEmpty()) return;
        a_.confirm(c.optBoolean("mine", false) ? "Delete your comment?" : "Delete this comment from your mod's page?", "Delete", () -> {
            final int id = id_, gen = gen_;
            final String t = token_;
            busy_ = true;
            a_.status("Deleting the comment…");
            final DiscoverPage.Answer[] a = new DiscoverPage.Answer[1];
            a_.background(() -> a[0] = page_.send("DELETE", "/api/mods/" + id + "/comments/" + c.optInt("id"), null, null, t), () -> {
                busy_ = false;
                if (!current(id, gen)) return;
                if (refused(a[0], "Delete")) return;
                commentList_.removeView(r);
                relines();
                total_ = Math.max(0, total_ - 1);
                head();
                a_.status("The comment was deleted.");
            });
        });
    }

    void report(final JSONObject c, final View r) {
        if (busy_ || token_.isEmpty()) return;
        a_.confirm("Report this comment to the online server's moderators? They see it with your account name.", "Report", () -> {
            final int id = id_, gen = gen_;
            final String t = token_;
            busy_ = true;
            a_.status("Reporting the comment…");
            final byte[] body = "{}".getBytes(StandardCharsets.UTF_8);
            final DiscoverPage.Answer[] a = new DiscoverPage.Answer[1];
            a_.background(() -> a[0] = page_.send("POST", "/api/mods/" + id + "/comments/" + c.optInt("id") + "/report",
                    "application/json", body, t), () -> {
                busy_ = false;
                if (!current(id, gen)) return;
                if (!refused(a[0], "Report"))  // (new: false is a success too)
                    a_.status("Reported - the online server's moderators will look at it. Thank you.");
            });
        });
    }

    // A refusal: the server's own words; signed out on the server's side: the "Sign in" buttons.
    private boolean refused(DiscoverPage.Answer r, String what) {
        if (r.status == 200 && DiscoverPage.json(r).optBoolean("ok", true)) return false;
        say(r, what);
        String code = DiscoverPage.json(r).optString("code", "");
        if (code.equals("sign_in_needed")) {
            token_ = "";
            stars_.setVisibility(View.GONE);
            rateSignIn_.setVisibility(View.VISIBLE);
            canComment(false, false);
            rateNote_.setVisibility(View.GONE);
            commentSignIn_.setVisibility(View.VISIBLE);
        } else if (code.equals("comments_off")) {
            showCard(commentCard_, false);
        } else if (code.equals("ratings_off")) {
            showCard(rateCard_, false);
        }
        android.util.Log.i("SvR2011", "discovery: " + what.toLowerCase(java.util.Locale.ROOT) + " on mod " + id_ + " refused ("
            + r.status + (code.isEmpty() ? "" : " " + code) + ")");
        return true;
    }

    private void say(DiscoverPage.Answer r, String what) {
        String e = DiscoverPage.json(r).optString("error", "");
        a_.status(!e.isEmpty() ? e : DiscoverPage.why(r, what));
    }
}
