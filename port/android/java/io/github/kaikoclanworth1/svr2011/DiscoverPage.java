// WWE SmackDown vs. Raw 2011 - Discovery on the phone (the PC launcher's
// Mods -> Discovery, launcher/discover_tab.c): players' .svrmod mods on the
// Community Creations server set on the Online tab (docs/DISCOVERY_API.md).
// Two views on one page: the list (words, type and order; 24 a page, each with
// its first picture) and a mod's page (its four pictures, what it is, Download
// and install). A download goes to the app's cache as discovery_<id>.part with
// a bar and Cancel; a break is tried again (6 tries, 2 / 5 / 10 / 30 s apart)
// from where it broke when the server answers a Range with 206, else from the
// start, and a busy server (502 / 503 / 504) the same. Only a complete file
// goes to the Mods tab's install (ModsPage.install: the custom-moves question,
// needs=). Each mod downloaded is remembered (its "updated", else "created",
// and the folder it went to) so the list marks a newer version: "Update".
// The player's own mod (the server's "mine" on its page, asked with the token
// the Online tab signed in with - checked again whenever the tab is shown, and
// never for another token): Edit (name, version, tags, description, types),
// the picture shown replaced or made the first, a new version of the file
// (a new version number with it) and Delete (docs/DISCOVERY_API.md).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;

final class DiscoverPage {
    static final int kPerPage = 24, kTries = 6;
    static final int[] kBackoff = {2, 5, 10, 30};  // (seconds between tries; then 30 s each)
    static final String[] kTypes = {"", "entrance", "superstar", "arena", "moveset"};
    static final String[] kTypeNames = {"All types", "Entrance", "Superstar", "Arena", "Move-set"};
    static final String[] kSorts = {"new", "updated", "popular", "name"};
    static final String[] kSortNames = {"Newest", "Recently updated", "Most downloaded", "Name"};

    private final LauncherActivity a_;
    private final ModsPage mods_;
    private LinearLayout grid_, list_, page_, buttons_, progress_;
    private EditText words_;
    private Spinner type_, sort_;
    private Button prev_, next_, download_, back_;
    private TextView pageLabel_, info_, desc_, picLabel_, progressText_;
    private ImageView picture_;
    private ProgressBar bar_;
    private int page_n_, total_, pic_;
    private JSONObject cur_;                         // (the mod whose page is shown)
    private final Bitmap[] pics_ = new Bitmap[4];
    private final List<JSONObject> mods_list_ = new ArrayList<>();
    private volatile int gen_;                      // (a newer list or page: an older one's answers are dropped)
    private volatile boolean cancel_;
    private boolean downloading_, shown_;
    private long lastVisit_;                        // (the previous visit: "Updated" since then)
    private LinearLayout owner_, edit_;             // (the player's own mod: its buttons; the edit form)
    private EditText eName_, eVersion_, eTags_, eDesc_;
    private final CheckBox[] eTypes_ = new CheckBox[4];
    private boolean mine_, busy_;                   // (the server said it's the player's; a change is being sent)
    private String mineToken_ = "";                 // (the token the page was asked with)
    private String note_;                           // (what a change said: kept over the next page's "Looking…")

    DiscoverPage(LauncherActivity a, ModsPage mods) {
        a_ = a;
        mods_ = mods;
    }

    SharedPreferences prefs() { return a_.getSharedPreferences("discovery", android.content.Context.MODE_PRIVATE); }

    String base() {
        return OnlinePage.base(a_.settings().getString("online_server", OnlinePage.kDefaultServer));
    }

    String token() { return a_.settings().getString("online_token", ""); }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Mods other players made with the SvR2011 Mod Maker, from the Community Creations "
            + "server set on the Online tab. Download one and it's installed on the Mods tab; it loads the next time "
            + "the game starts.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);

        // The list: words, type, order; the mods; the pages.
        grid_ = new LinearLayout(a_);
        grid_.setOrientation(LinearLayout.VERTICAL);
        c.addView(grid_);
        LinearLayout find = a_.card(grid_, "Find mods");
        words_ = a_.field("Name, author or tag", 120, InputType.TYPE_CLASS_TEXT);
        words_.setGravity(Gravity.START);
        words_.setImeOptions(EditorInfo.IME_ACTION_SEARCH);
        words_.setOnEditorActionListener((v, id, e) -> {
            page_n_ = 0;
            search();
            return true;
        });
        find.addView(words_, a_.fullWidth(6));
        type_ = spinner(kTypeNames);
        a_.row(find, "Type", null, type_);
        sort_ = spinner(kSortNames);
        a_.row(find, "Order", null, sort_);
        Button go = a_.button("Search", LauncherActivity.kRed);
        go.setOnClickListener(v -> {
            page_n_ = 0;
            search();
        });
        grid_.addView(go, a_.fullWidth(10));
        list_ = a_.card(grid_, "Mods");
        prev_ = a_.button("Previous", LauncherActivity.kCard);
        next_ = a_.button("Next", LauncherActivity.kCard);
        prev_.setOnClickListener(v -> {
            if (page_n_ > 0) page_n_--;
            search();
        });
        next_.setOnClickListener(v -> {
            page_n_++;
            search();
        });
        grid_.addView(a_.pair(prev_, next_), a_.fullWidth(10));
        pageLabel_ = a_.text("", 13, LauncherActivity.kDim);
        pageLabel_.setGravity(Gravity.CENTER);
        grid_.addView(pageLabel_, a_.fullWidth(4));

        // A mod's page: its pictures, what it is, Download and install.
        page_ = new LinearLayout(a_);
        page_.setOrientation(LinearLayout.VERTICAL);
        page_.setVisibility(View.GONE);
        c.addView(page_);
        back_ = a_.button("Back to the list", LauncherActivity.kCard);
        back_.setOnClickListener(v -> showList());
        page_.addView(back_, a_.fullWidth(6));
        picture_ = new ImageView(a_);
        picture_.setScaleType(ImageView.ScaleType.FIT_CENTER);
        picture_.setBackground(a_.rounded(LauncherActivity.kCard, 12));
        picture_.setOnClickListener(v -> showPic(pic_ + 1));
        page_.addView(picture_, a_.fullWidth(10));
        Button pp = a_.button("<", LauncherActivity.kCard), pn = a_.button(">", LauncherActivity.kCard);
        pp.setOnClickListener(v -> showPic(pic_ + 3));
        pn.setOnClickListener(v -> showPic(pic_ + 1));
        LinearLayout pics = a_.pair(pp, pn);
        picLabel_ = a_.text("1 / 4", 14, LauncherActivity.kDim);
        picLabel_.setGravity(Gravity.CENTER);
        pics.addView(picLabel_, 1, new LinearLayout.LayoutParams(a_.dp(64), ViewGroup.LayoutParams.WRAP_CONTENT));
        page_.addView(pics, a_.fullWidth(6));
        LinearLayout about2 = a_.card(page_, "The mod");
        info_ = a_.text("", 15, LauncherActivity.kText);
        info_.setPadding(0, a_.dp(10), 0, a_.dp(10));
        about2.addView(info_);
        desc_ = a_.text("", 14, LauncherActivity.kDim);
        desc_.setPadding(0, 0, 0, a_.dp(10));
        about2.addView(desc_);
        buttons_ = new LinearLayout(a_);
        buttons_.setOrientation(LinearLayout.VERTICAL);
        page_.addView(buttons_);
        download_ = a_.button("Download and install", LauncherActivity.kRed);
        download_.setOnClickListener(v -> download());
        buttons_.addView(download_, a_.fullWidth(10));
        ownerViews();
        a_.onRefresh(this::updateOwner);  // (the settings read again - the app back from another: signed out?)

        // A download running: its bar, what it's at, Cancel (in the buttons' place; on both views).
        progress_ = new LinearLayout(a_);
        progress_.setOrientation(LinearLayout.VERTICAL);
        progress_.setVisibility(View.GONE);
        bar_ = new ProgressBar(a_, null, android.R.attr.progressBarStyleHorizontal);
        bar_.setMax(1000);
        bar_.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        progress_.addView(bar_, a_.fullWidth(12));
        progressText_ = a_.text("", 13, LauncherActivity.kText);
        progress_.addView(progressText_, a_.fullWidth(4));
        Button cancel = a_.button("Cancel the download", LauncherActivity.kCard);
        cancel.setOnClickListener(v -> {
            cancel_ = true;
            progressText_.setText("Cancelling…");
        });
        progress_.addView(cancel, a_.fullWidth(8));
        c.addView(progress_);
        return c;
    }

    Spinner spinner(String[] names) {
        Spinner s = new Spinner(a_, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> ad = new ArrayAdapter<String>(a_, android.R.layout.simple_spinner_item, names) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(LauncherActivity.kText);
                t.setTextSize(15);
                return t;
            }
        };
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(ad);
        return s;
    }

    // The tab shown: the list the first time (and the visit remembered: "Updated" since the last one).
    void onShow() {
        if (shown_) {  // (again: the page asked once more - signed out or in as someone else meanwhile?)
            updateOwner();
            if (cur_ != null && page_.getVisibility() == View.VISIBLE && !busy_ && !downloading_) openRow(cur_);
            return;
        }
        shown_ = true;
        lastVisit_ = prefs().getLong("last_visit", 0);
        prefs().edit().putLong("last_visit", System.currentTimeMillis() / 1000).apply();
        search();
    }

    // ── answers from the server ──────────────────────────────────────────

    static final class Answer {
        int status;      // (0: no answer)
        byte[] data;
        String why = "";
    }

    // One request; a lost connection or a busy server (502 / 503 / 504) is tried again (kTries in all) with
    // "Reconnecting…" while `gen` is still the page's.
    Answer get(String path, int max, int gen) {
        Answer r = new Answer();
        for (int tries = 0; ; tries++) {
            boolean again = false;
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(base() + path).openConnection();
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);
                String t = token();
                if (!t.isEmpty()) c.setRequestProperty("Authorization", "Bearer " + t);
                r.status = c.getResponseCode();
                InputStream in = r.status < 400 ? c.getInputStream() : c.getErrorStream();
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                if (in != null) {
                    try (InputStream is = in) {
                        byte[] buf = new byte[1 << 15];
                        for (int n; (n = is.read(buf)) > 0; ) {
                            b.write(buf, 0, n);
                            if (b.size() > max) throw new IOException("the answer is too big");
                        }
                    }
                }
                r.data = b.toByteArray();
                again = r.status == 502 || r.status == 503 || r.status == 504;
            } catch (IOException e) {
                r.status = 0;
                r.why = e.getMessage() != null ? e.getMessage() : "no answer";
                again = true;
            } finally {
                if (c != null) c.disconnect();
            }
            if (!again || tries + 1 >= kTries || gen != gen_) return r;
            final String say = (r.status != 0 ? "The server is busy" : "Can't reach the server") + " - reconnecting…";
            a_.runOnUiThread(() -> {
                if (gen == gen_ && !downloading_) a_.status(say);
            });
            sleep(kBackoff[Math.min(tries, 3)] * 1000L, gen);
        }
    }

    void sleep(long ms, int gen) {
        for (long t = 0; t < ms && gen == gen_ && !cancel_; t += 100) {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    static JSONObject json(Answer r) {
        try {
            return r.data != null ? new JSONObject(new String(r.data, StandardCharsets.UTF_8)) : new JSONObject();
        } catch (org.json.JSONException e) {
            return new JSONObject();
        }
    }

    // What to tell the player about an answer that isn't 200.
    static String why(Answer r, String what) {
        if (r.status == 0) return what + ": can't reach the server (" + r.why + "). Check the Online tab's server.";
        String e = json(r).optString("error", "");
        if (!e.isEmpty()) return what + ": " + e;
        if (r.status == 404) return what + ": not found (it may have been removed).";
        if (r.status == 401) return what + ": not signed in - sign in again on the Online tab.";
        if (r.status == 403) return what + ": only the mod's uploader can change it.";
        if (r.status == 429) return what + ": too many changes in an hour - try again later.";
        if (r.status == 502 || r.status == 503 || r.status == 504) return what + ": the server is busy - try again in a minute.";
        return what + ": the server said " + r.status + ".";
    }

    static Bitmap picture(byte[] data, int sample) {
        if (data == null) return null;
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inSampleSize = sample;
        return BitmapFactory.decodeByteArray(data, 0, data.length, o);
    }

    // ── the list ─────────────────────────────────────────────────────────

    void search() {
        final int gen = ++gen_;
        String q;
        try {
            q = URLEncoder.encode(words_.getText().toString().trim(), "UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            q = "";
        }
        final String path = "/api/mods?q=" + q + "&type=" + kTypes[type_.getSelectedItemPosition()]
            + "&sort=" + kSorts[sort_.getSelectedItemPosition()] + "&page=" + page_n_;
        if (!downloading_) a_.status("Looking…");
        final Answer[] r = new Answer[1];
        a_.background(() -> r[0] = get(path, 1 << 20, gen), () -> {
            if (gen != gen_) return;
            listed(r[0], gen);
        });
    }

    void listed(Answer r, int gen) {
        list_.removeAllViews();
        mods_list_.clear();
        if (r.status != 200) {
            if (!downloading_) a_.status(why(r, "Discovery"));
            TextView none = a_.text("The list didn't load.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            list_.addView(none);
            return;
        }
        JSONObject j = json(r);
        total_ = j.optInt("total", 0);
        JSONArray mods = j.optJSONArray("mods");
        final List<ImageView> thumbs = new ArrayList<>();
        for (int i = 0; mods != null && i < mods.length() && i < kPerPage; i++) {
            JSONObject m = mods.optJSONObject(i);
            if (m == null) continue;
            mods_list_.add(m);
            thumbs.add(listRow(m, mods_list_.size() - 1));
        }
        if (mods_list_.isEmpty()) {
            TextView none = a_.text(total_ == 0 && page_n_ == 0 ? "No mods found." : "No more mods.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            list_.addView(none);
        }
        int pages = (total_ + kPerPage - 1) / kPerPage;
        pageLabel_.setText(total_ > 0 ? "Page " + (page_n_ + 1) + " of " + pages : "");
        prev_.setEnabled(page_n_ > 0);
        next_.setEnabled((page_n_ + 1) * kPerPage < total_);
        prev_.setAlpha(prev_.isEnabled() ? 1f : 0.4f);
        next_.setAlpha(next_.isEnabled() ? 1f : 0.4f);
        if (!downloading_) a_.status((note_ != null ? note_ + " " : "") + total_ + " mod" + (total_ == 1 ? "" : "s")
            + " found. Tap one for its page.");
        note_ = null;
        // then the pictures, one after the other
        final List<JSONObject> rows = new ArrayList<>(mods_list_);
        a_.background(() -> {
            for (int i = 0; i < rows.size() && gen == gen_; i++) {
                JSONObject m = rows.get(i);
                Answer p = get("/api/mods/" + m.optInt("id") + "/img/0?v=" + m.optInt("pics"), 2 << 20, gen);
                final Bitmap b = p.status == 200 ? picture(p.data, 4) : null;
                final ImageView v = thumbs.get(i);
                if (b != null) a_.runOnUiThread(() -> {
                    if (gen == gen_) v.setImageBitmap(b);
                });
            }
        }, null);
    }

    // The version downloaded here (its "updated", else "created", then), when its folder is still installed; 0: none.
    long downloaded(int id) {
        SharedPreferences p = prefs();
        long got = p.getLong("mod" + id, 0);
        String dir = p.getString("dir" + id, "");
        return got != 0 && (dir.isEmpty() || new File(dir).isDirectory()) ? got : 0;
    }

    static long version(JSONObject m) {
        long u = m.optLong("updated", 0);
        return u != 0 ? u : m.optLong("created", 0);
    }

    static String date(long unix) {
        return DateFormat.getDateInstance(DateFormat.SHORT).format(new Date(unix * 1000));
    }

    static String types(JSONObject m) {
        JSONArray t = m.optJSONArray("types");
        StringBuilder s = new StringBuilder();
        for (int i = 0; t != null && i < t.length(); i++) {
            String k = t.optString(i);
            for (int n = 1; n < kTypes.length; n++) if (kTypes[n].equals(k)) k = kTypeNames[n];
            s.append(s.length() > 0 ? ", " : "").append(k);
        }
        return s.toString();
    }

    ImageView listRow(JSONObject m, int index) {
        LinearLayout r = new LinearLayout(a_);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, a_.dp(8), 0, a_.dp(8));
        ImageView thumb = new ImageView(a_);
        thumb.setScaleType(ImageView.ScaleType.CENTER_CROP);
        thumb.setBackground(a_.rounded(LauncherActivity.kLine, 6));
        thumb.setClipToOutline(true);
        r.addView(thumb, new LinearLayout.LayoutParams(a_.dp(112), a_.dp(84)));
        LinearLayout texts = new LinearLayout(a_);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(a_.dp(12), 0, 0, 0);
        long got = downloaded(m.optInt("id")), now = version(m);
        String badge = got != 0 && now > got ? "UPDATE  "
            : m.optLong("updated", 0) != 0 && lastVisit_ != 0 && m.optLong("updated") > lastVisit_ ? "UPDATED  "
            : got != 0 ? "INSTALLED  " : "";
        TextView name = a_.text("", 16, LauncherActivity.kText);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        android.text.SpannableStringBuilder s = new android.text.SpannableStringBuilder();
        if (!badge.isEmpty()) {
            s.append(badge);
            s.setSpan(new android.text.style.ForegroundColorSpan(badge.startsWith("INSTALLED") ? LauncherActivity.kDim
                : LauncherActivity.kRed), 0, badge.length(), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            s.setSpan(new android.text.style.RelativeSizeSpan(0.75f), 0, badge.length(), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        s.append(m.optString("name"));
        name.setText(s);
        texts.addView(name);
        texts.addView(a_.text("by " + m.optString("author") + " - " + types(m), 13, LauncherActivity.kDim));
        int dl = m.optInt("downloads");
        texts.addView(a_.text("v" + m.optString("version") + " - " + FileOps.human(m.optLong("size")) + " - " + dl
            + " download" + (dl == 1 ? "" : "s") + " - " + (m.optLong("updated", 0) != 0 ? "updated " : "new ")
            + date(version(m)), 13, LauncherActivity.kDim));
        r.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        r.setOnClickListener(v -> open(index));
        if (list_.getChildCount() > 0) {
            View sep = new View(a_);
            sep.setBackgroundColor(LauncherActivity.kLine);
            list_.addView(sep, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
        }
        list_.addView(r);
        return thumb;
    }

    // ── a mod's page ─────────────────────────────────────────────────────

    void showList() {
        page_.setVisibility(View.GONE);
        grid_.setVisibility(View.VISIBLE);
        cur_ = null;
        search();  // (its badges as they are now)
    }

    void open(int index) {
        if (index >= 0 && index < mods_list_.size()) openRow(mods_list_.get(index));
    }

    void openRow(final JSONObject row) {
        final int gen = ++gen_;
        cur_ = row;
        for (int k = 0; k < 4; k++) pics_[k] = null;
        pic_ = 0;
        picture_.setImageBitmap(null);
        fitPicture();
        picLabel_.setText("1 / 4");
        mine_ = false;  // (only on the server's word, asked with the token signed in now)
        mineToken_ = token();
        edit_.setVisibility(View.GONE);
        updateOwner();
        fillInfo(row, "");
        grid_.setVisibility(View.GONE);
        page_.setVisibility(View.VISIBLE);
        buttons_.setVisibility(downloading_ ? View.GONE : View.VISIBLE);
        scrollTop();
        final String path = "/api/mods/" + row.optInt("id");
        final int v = row.optInt("pics");
        final Answer[] r = new Answer[1];
        a_.background(() -> r[0] = get(path, 1 << 20, gen), () -> {
            if (gen != gen_) return;
            if (r[0].status != 200) {
                a_.status(why(r[0], "The mod's page"));
                return;
            }
            JSONObject m = json(r[0]);
            cur_ = m;
            mine_ = m.optBoolean("mine", false);
            updateOwner();
            fillInfo(m, m.optString("description"));
            if (!downloading_) a_.status(note_ != null ? note_ : "");
            note_ = null;
            a_.background(() -> {
                for (int i = 0; i < 4 && gen == gen_; i++) {
                    Answer p = get(path + "/img/" + i + "?v=" + v, 2 << 20, gen);
                    final Bitmap b = p.status == 200 ? picture(p.data, 1) : null;
                    final int n = i;
                    a_.runOnUiThread(() -> {
                        if (gen != gen_) return;
                        pics_[n] = b;
                        if (n == pic_) showPic(pic_);
                    });
                }
            }, null);
        });
    }

    void scrollTop() {
        View p = (View) page_.getParent();
        while (p != null && !(p instanceof android.widget.ScrollView)) p = p.getParent() instanceof View ? (View) p.getParent() : null;
        if (p != null) ((android.widget.ScrollView) p).scrollTo(0, 0);
    }

    // The picture 4:3 across the page, but never taller than about half the screen (a phone on its side, the cover
    // screen): then narrower, centred.
    void fitPicture() {
        android.util.DisplayMetrics d = a_.getResources().getDisplayMetrics();
        int w = d.widthPixels - a_.dp(32), h = Math.min(w * 3 / 4, d.heightPixels * 11 / 20);
        picture_.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h));
        ((LinearLayout.LayoutParams) picture_.getLayoutParams()).topMargin = a_.dp(10);
    }

    void showPic(int n) {
        pic_ = n % 4;
        picLabel_.setText((pic_ + 1) + " / 4");
        picture_.setImageBitmap(pics_[pic_]);
    }

    void fillInfo(JSONObject m, String desc) {
        StringBuilder s = new StringBuilder();
        s.append(m.optString("name")).append("\nby ").append(m.optString("author")).append("\n\n").append(types(m))
            .append("\nVersion ").append(m.optString("version"))
            .append(m.optLong("updated", 0) != 0 ? "\nUpdated " : "\nNew (").append(date(version(m)))
            .append(m.optLong("updated", 0) != 0 ? "" : ")")
            .append("\n").append(FileOps.human(m.optLong("size")));
        int dl = m.optInt("downloads");
        s.append("\n").append(dl).append(" download").append(dl == 1 ? "" : "s");
        String needs = m.optString("needs", "");
        if (!needs.isEmpty()) {
            s.append("\nNeeds game version ").append(needs).append(" or newer");
            if (ModsPage.needsNewer(a_, needs)) s.append(" - this one is ").append(ModsPage.gameVersion(a_)).append(": update the game first");
        }
        JSONArray tags = m.optJSONArray("tags");
        if (tags != null && tags.length() > 0) {
            s.append("\n\nTags: ");
            for (int i = 0; i < tags.length(); i++) s.append(i > 0 ? ", " : "").append(tags.optString(i));
        }
        info_.setText(s.toString());
        desc_.setText(desc.isEmpty() ? "" : desc);
        long got = downloaded(m.optInt("id"));
        download_.setText(got != 0 && version(m) > got ? "Download the update" : got != 0 ? "Download and install again"
            : "Download and install");
    }

    // ── the download ─────────────────────────────────────────────────────

    void download() {
        if (cur_ == null || downloading_) return;
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        final JSONObject m = cur_;
        final int id = m.optInt("id");
        final String name = m.optString("name");
        downloading_ = true;
        cancel_ = false;
        buttons_.setVisibility(View.GONE);
        progress_.setVisibility(View.VISIBLE);
        bar_.setProgress(0);
        progressText_.setText("Downloading \"" + name + "\"…");
        a_.status("");
        final String[] result = new String[1];
        final boolean[] installed = {false};
        a_.background(() -> {
            File dir = new File(a_.getCacheDir(), "discovery");
            dir.mkdirs();
            File part = new File(dir, "discovery_" + id + ".part"), file = new File(dir, "discovery.svrmod");
            String why = downloadTo("/api/mods/" + id + "/file", part);
            if (!why.isEmpty() || !part.renameTo(file)) {
                part.delete();
                result[0] = cancel_ ? "Download of \"" + name + "\" cancelled - nothing was installed."
                    : "Download of \"" + name + "\" failed (" + (why.isEmpty() ? "the file couldn't be put in place" : why)
                        + ") - nothing was installed. Try again in a moment.";
                return;
            }
            a_.runOnUiThread(() -> progressText_.setText("Installing \"" + name + "\"…"));
            try (InputStream in = new FileInputStream(file)) {
                ModsPage.Installed r = ModsPage.install(a_, in, false, mods_::askMoves);
                prefs().edit().putLong("mod" + id, version(m)).putString("dir" + id, r.dest.getAbsolutePath()).apply();
                installed[0] = true;
                result[0] = "Installed \"" + r.name + "\" - it's on the Mods tab and loads the next time the game starts."
                    + (r.note.isEmpty() ? "" : " " + r.note);
                android.util.Log.i("SvR2011", "discovery: mod " + id + " \"" + name + "\" installed to " + r.dest);
            } catch (ModsPage.Declined e) {
                result[0] = "Downloaded \"" + name + "\" - not installed: " + e.getMessage() + ".";
            } catch (Exception e) {
                result[0] = "Downloaded \"" + name + "\", but it could not be installed: " + e.getMessage();
                android.util.Log.w("SvR2011", "discovery: mod " + id + " not installed: " + e.getMessage());
            } finally {
                file.delete();
            }
        }, () -> {
            downloading_ = false;
            cancel_ = false;
            progress_.setVisibility(View.GONE);
            buttons_.setVisibility(View.VISIBLE);
            if (installed[0]) mods_.refresh();
            if (cur_ != null && page_.getVisibility() == View.VISIBLE) fillInfo(cur_, desc_.getText().toString());
            else search();
            a_.status(result[0]);
        });
    }

    void progress(long have, long total, double speed, String extra) {
        final int at = total > 0 ? (int) (have * 1000 / total) : 0;
        final String t = (total > 0 ? "Downloading " + (have * 100 / total) + "% - " + FileOps.human(have) + " of "
            + FileOps.human(total) : "Downloading - " + FileOps.human(have))
            + (speed > 0 ? String.format(java.util.Locale.ROOT, " - %.1f MB/s", speed / 1048576.0) : "") + extra;
        a_.runOnUiThread(() -> {
            if (!downloading_) return;
            bar_.setProgress(at);
            if (!cancel_) progressText_.setText(t);
        });
    }

    // The file into `part` as it comes, with progress (launcher/discover_tab.c download_to). "" when every byte is in
    // (the Content-Length's, when the server gave one); else why not.
    String downloadTo(String path, File part) {
        long have = 0, total = -1;
        int tries = 0;
        boolean done = false;
        String why = "";
        byte[] buf = new byte[1 << 16];
        while (!done && !cancel_) {
            boolean broke = true, busy = false;
            int status = 0;
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(base() + path).openConnection();
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);  // (30 s without a byte: broken)
                String t = token();
                if (!t.isEmpty()) c.setRequestProperty("Authorization", "Bearer " + t);
                if (have > 0) c.setRequestProperty("Range", "bytes=" + have + "-");
                status = c.getResponseCode();
                long len = c.getContentLengthLong();
                if (status == 206 && have > 0) {  // (the rest, from where it broke)
                    long from = -1;
                    String range = c.getHeaderField("Content-Range");
                    java.util.regex.Matcher mt = range != null
                        ? java.util.regex.Pattern.compile("bytes (\\d+)-").matcher(range) : null;
                    if (mt != null && mt.find()) from = Long.parseLong(mt.group(1));
                    if (from != have) {  // (not the part asked for: from the start)
                        status = 0;
                        have = 0;
                    } else if (len >= 0) total = have + len;
                }
                if (status == 200 || (status == 206 && have == 0)) {  // (the whole file: from the start)
                    have = 0;
                    total = len;
                }
                if (status == 200 || status == 206) {
                    long t0 = System.currentTimeMillis(), last = 0, at0 = have;
                    try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(part, have > 0)) {
                        for (;;) {
                            if (cancel_) break;
                            int n = in.read(buf);
                            if (n < 0) {  // (the end - all of it?)
                                broke = total >= 0 && have < total;
                                break;
                            }
                            try {
                                out.write(buf, 0, n);
                            } catch (IOException e) {
                                why = "the file couldn't be written (storage full?)";
                                broke = false;
                                break;
                            }
                            have += n;
                            long now = System.currentTimeMillis();
                            if (now - last >= 200) {
                                double secs = (now - t0) / 1000.0;
                                last = now;
                                progress(have, total, secs > 0.2 ? (have - at0) / secs : 0, "");
                            }
                        }
                    }
                    if (!cancel_ && !broke && why.isEmpty()) done = true;
                } else if (status == 502 || status == 503 || status == 504) {
                    busy = true;  // (the server is there but its storage is slow just now: tried again)
                } else if (status != 0) {
                    broke = false;  // (the server said no - 404, 403...: not worth trying again)
                    why = "the server said " + status;
                }
            } catch (IOException e) {
                broke = true;  // (no answer, or it stopped part way: worth another try)
            } finally {
                if (c != null) c.disconnect();
            }
            if (done || cancel_ || !broke && !busy || !why.isEmpty()) break;
            if (++tries >= kTries) {
                why = "the connection kept breaking";
                break;
            }
            int wait = kBackoff[Math.min(tries - 1, 3)];
            progress(have, total, 0, " - " + (busy ? "the server is busy" : "connection lost") + ", "
                + (have > 0 ? "resuming" : "starting again") + " in " + wait + " s (try " + (tries + 1) + " of " + kTries + ")");
            android.util.Log.i("SvR2011", "discovery: download " + path + " " + (busy ? "busy (" + status + ")" : "broke")
                + " at " + have + " of " + total + " - try " + (tries + 1));
            for (long w = 0; w < wait * 1000L && !cancel_; w += 100) {
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    break;
                }
            }
        }
        if (cancel_ && !done) why = "cancelled";
        return done ? "" : why.isEmpty() ? "the connection kept breaking" : why;
    }

    // ── the player's own mod ─────────────────────────────────────────────

    void ownerViews() {
        owner_ = new LinearLayout(a_);
        owner_.setOrientation(LinearLayout.VERTICAL);
        owner_.setVisibility(View.GONE);
        buttons_.addView(owner_);
        LinearLayout card = a_.card(owner_, "Your mod");
        TextView hint = a_.text("You uploaded this mod: change it for everyone, or delete it.", 13, LauncherActivity.kDim);
        hint.setPadding(0, a_.dp(8), 0, a_.dp(8));
        card.addView(hint);
        Button edit = a_.button("Edit the details", LauncherActivity.kCard);
        edit.setOnClickListener(v -> editForm());
        Button file = a_.button("New version…", LauncherActivity.kCard);
        file.setOnClickListener(v -> pickNewVersion());
        owner_.addView(a_.pair(edit, file), a_.fullWidth(8));
        Button first = a_.button("Make this picture first", LauncherActivity.kCard);
        first.setOnClickListener(v -> makeFirst());
        Button pic = a_.button("Replace this picture…", LauncherActivity.kCard);
        pic.setOnClickListener(v -> pickPicture());
        owner_.addView(a_.pair(first, pic), a_.fullWidth(8));
        Button delete = a_.button("Delete my mod", 0xFF5A1A22);
        delete.setOnClickListener(v -> deleteMine());
        owner_.addView(delete, a_.fullWidth(8));

        edit_ = new LinearLayout(a_);
        edit_.setOrientation(LinearLayout.VERTICAL);
        edit_.setVisibility(View.GONE);
        buttons_.addView(edit_);
        LinearLayout form = a_.card(edit_, "Edit the details");
        eName_ = formField(form, "Name", 60, InputType.TYPE_CLASS_TEXT);
        eVersion_ = formField(form, "Version", 20, InputType.TYPE_CLASS_TEXT);
        eTags_ = formField(form, "Tags (commas between)", 200, InputType.TYPE_CLASS_TEXT);
        eDesc_ = formField(form, "Description", 2000, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        eDesc_.setSingleLine(false);
        eDesc_.setMinLines(3);
        LinearLayout types = new LinearLayout(a_);
        types.setOrientation(LinearLayout.HORIZONTAL);
        for (int k = 0; k < 4; k++) {
            eTypes_[k] = new CheckBox(a_);
            eTypes_[k].setText(kTypeNames[k + 1]);
            eTypes_[k].setTextColor(LauncherActivity.kText);
            eTypes_[k].setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
            types.addView(eTypes_[k], new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        form.addView(types, a_.fullWidth(6));
        Button save = a_.button("Save", LauncherActivity.kRed), cancel = a_.button("Cancel", LauncherActivity.kCard);
        save.setOnClickListener(v -> saveEdit());
        cancel.setOnClickListener(v -> {
            edit_.setVisibility(View.GONE);
            updateOwner();
        });
        edit_.addView(a_.pair(save, cancel), a_.fullWidth(8));
    }

    EditText formField(LinearLayout card, String label, int max, int type) {
        TextView l = a_.text(label, 13, LauncherActivity.kDim);
        l.setPadding(0, a_.dp(8), 0, 0);
        card.addView(l);
        EditText e = a_.field("", max, type);
        e.setGravity(Gravity.START);
        card.addView(e, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return e;
    }

    // The page shows the player's own mod: the server said so ("mine") for the token signed in now - not for one
    // from before a sign-out or another account's sign-in (its changes would be refused: 403).
    boolean ownerNow() {
        String t = token();
        return mine_ && cur_ != null && !t.isEmpty() && t.equals(mineToken_);
    }

    void updateOwner() {
        boolean mine = ownerNow();
        if (!mine) edit_.setVisibility(View.GONE);
        boolean editing = edit_.getVisibility() == View.VISIBLE;
        owner_.setVisibility(mine && !editing ? View.VISIBLE : View.GONE);
        download_.setVisibility(editing ? View.GONE : View.VISIBLE);
    }

    void editForm() {
        if (!ownerNow() || busy_) return;
        JSONObject m = cur_;
        eName_.setText(m.optString("name"));
        eVersion_.setText(m.optString("version"));
        eTags_.setText(tagText(m));
        eDesc_.setText(m.optString("description"));
        JSONArray ty = m.optJSONArray("types");
        for (int k = 0; k < 4; k++) {
            boolean on = false;
            for (int i = 0; ty != null && i < ty.length(); i++) on |= kTypes[k + 1].equals(ty.optString(i));
            eTypes_[k].setChecked(on);
        }
        edit_.setVisibility(View.VISIBLE);
        updateOwner();
    }

    static String tagText(JSONObject m) {
        JSONArray tags = m.optJSONArray("tags");
        StringBuilder t = new StringBuilder();
        for (int i = 0; tags != null && i < tags.length(); i++) t.append(i > 0 ? ", " : "").append(tags.optString(i));
        return t.toString();
    }

    // Save on the edit form: only what changed (PATCH).
    void saveEdit() {
        if (!ownerNow() || busy_) return;
        JSONObject m = cur_, body = new JSONObject();
        try {
            String name = eName_.getText().toString().trim(), ver = eVersion_.getText().toString().trim();
            if (name.isEmpty() || ver.isEmpty()) {
                a_.status("The name and the version can't be empty.");
                return;
            }
            if (!name.equals(m.optString("name"))) body.put("name", name);
            if (!ver.equals(m.optString("version"))) body.put("version", ver);
            String tags = eTags_.getText().toString().trim();
            if (!tags.equals(tagText(m))) body.put("tags", tags);
            String desc = eDesc_.getText().toString();
            if (!desc.equals(m.optString("description"))) body.put("description", desc);
            JSONArray types = new JSONArray();
            for (int k = 0; k < 4; k++) if (eTypes_[k].isChecked()) types.put(kTypes[k + 1]);
            if (types.length() == 0) {
                a_.status("Choose the mod's type.");
                return;
            }
            if (!types.toString().equals(String.valueOf(m.optJSONArray("types")))) body.put("types", types);
        } catch (org.json.JSONException e) {
            return;
        }
        if (body.length() == 0) {
            a_.status("Nothing changed.");
            return;
        }
        change("Saving", "PATCH", "", "application/json", body.toString().getBytes(StandardCharsets.UTF_8));
    }

    void makeFirst() {
        if (!ownerNow() || busy_) return;
        if (pic_ == 0) {
            a_.status("This picture is the first already.");
            return;
        }
        JSONArray order = new JSONArray();
        order.put(pic_);
        for (int i = 0; i < 4; i++) if (i != pic_) order.put(i);
        String body = "{\"picture_order\":" + order + "}";
        change("Saving", "PATCH", "", "application/json", body.getBytes(StandardCharsets.UTF_8));
    }

    void pickPicture() {
        if (!ownerNow() || busy_) return;
        final int n = pic_;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("image/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            final Uri uri = data.getData();
            final byte[][] jpg = new byte[1][];
            a_.status("Making the picture (1024 x 768)…");
            a_.background(() -> jpg[0] = jpeg43(uri), () -> {
                if (jpg[0] == null) {
                    a_.status("That picture could not be read.");
                    return;
                }
                change("Picture " + (n + 1), "PUT", "/pictures/" + n, "image/jpeg", jpg[0]);
            });
        });
    }

    // A picture cut to 4:3 from its middle, 1024 x 768, a JPEG of 1 MB at most (the server's rule; the PC's fit_43).
    byte[] jpeg43(Uri uri) {
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            try (InputStream in = a_.getContentResolver().openInputStream(uri)) {
                BitmapFactory.decodeStream(in, null, o);
            }
            if (o.outWidth <= 0 || o.outHeight <= 0) return null;
            int sample = 1;
            while (o.outWidth / (sample * 2) >= 1024 && o.outHeight / (sample * 2) >= 768) sample *= 2;
            BitmapFactory.Options o2 = new BitmapFactory.Options();
            o2.inSampleSize = sample;
            Bitmap b;
            try (InputStream in = a_.getContentResolver().openInputStream(uri)) {
                b = BitmapFactory.decodeStream(in, null, o2);
            }
            if (b == null) return null;
            int w = b.getWidth(), h = b.getHeight(), x = 0, y = 0;
            if ((double) w / h > 4.0 / 3.0) {
                int cw = h * 4 / 3;
                x = (w - cw) / 2;
                w = cw;
            } else {
                int ch = w * 3 / 4;
                y = (h - ch) / 2;
                h = ch;
            }
            Bitmap out = Bitmap.createScaledBitmap(Bitmap.createBitmap(b, x, y, w, h), 1024, 768, true);
            for (int q = 90; q >= 40; q -= 10) {
                ByteArrayOutputStream s = new ByteArrayOutputStream();
                out.compress(Bitmap.CompressFormat.JPEG, q, s);
                if (s.size() <= 1 << 20) return s.toByteArray();
            }
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("SvR2011", "discovery: picture not made: " + e);
        }
        return null;
    }

    void pickNewVersion() {
        if (!ownerNow() || busy_) return;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null || !ownerNow()) return;
            final Uri uri = data.getData();
            String name = "mod.svrmod";
            long size = -1;
            try (android.database.Cursor q = a_.getContentResolver().query(uri, new String[] {
                    android.provider.OpenableColumns.DISPLAY_NAME, android.provider.OpenableColumns.SIZE}, null, null, null)) {
                if (q != null && q.moveToFirst()) {
                    if (q.getString(0) != null) name = q.getString(0);
                    if (!q.isNull(1)) size = q.getLong(1);
                }
            } catch (Exception ignored) {
            }
            if (!name.toLowerCase(java.util.Locale.ROOT).endsWith(".svrmod")) {
                a_.status("Choose a .svrmod file (made with the Mod Maker).");
                return;
            }
            final String fileName = name;
            final long fileSize = size;
            final EditText ver = a_.field("New version", 20, InputType.TYPE_CLASS_TEXT);
            ver.setGravity(Gravity.START);
            ver.setText(cur_.optString("version") + ".1");
            LinearLayout box = new LinearLayout(a_);
            box.setPadding(a_.dp(20), a_.dp(4), a_.dp(20), 0);
            box.addView(ver, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle("A new version of \"" + cur_.optString("name") + "\"")
                .setMessage(fileName + (fileSize >= 0 ? " (" + FileOps.human(fileSize) + ")" : "")
                    + "\nIts version number (now " + cur_.optString("version") + "):")
                .setView(box)
                .setPositiveButton("Send", (d, w) -> sendNewVersion(uri, fileName, fileSize, ver.getText().toString().trim()))
                .setNegativeButton("Cancel", null)
                .show();
        });
    }

    void deleteMine() {
        if (!ownerNow() || busy_) return;
        a_.confirm("Delete \"" + cur_.optString("name") + "\" from Discovery for everyone? Its file and pictures go "
            + "too; players who installed it keep their copy.", "Delete", () -> change("Delete", "DELETE", "", null, null));
    }

    // An owner's change (PATCH / PUT a picture / DELETE) to /api/mods/<id><sub>; then the page again (or the list).
    void change(final String what, final String method, final String sub, final String ctype, final byte[] body) {
        if (!ownerNow() || busy_) return;
        busy_ = true;
        final int id = cur_.optInt("id");
        final String t = token();
        a_.status(what + "…");
        final Answer[] r = new Answer[1];
        a_.background(() -> r[0] = send(method, "/api/mods/" + id + sub, ctype, body, t), () -> {
            busy_ = false;
            changed(r[0], what, id, method.equals("DELETE"));
        });
    }

    void changed(Answer r, String what, int id, boolean deleted) {
        if (r.status != 200) {
            a_.status(why(r, what));
            android.util.Log.w("SvR2011", "discovery: mod " + id + " " + what + " refused (" + r.status + ")");
            updateOwner();
            return;
        }
        if (deleted) {
            android.util.Log.i("SvR2011", "discovery: mod " + id + " deleted by its owner");
            note_ = "Deleted from Discovery.";
            showList();
            return;
        }
        edit_.setVisibility(View.GONE);
        JSONObject row = json(r).optJSONObject("mod");
        String warning = json(r).optString("warning", "");
        android.util.Log.i("SvR2011", "discovery: mod " + id + " changed by its owner (" + what + ")");
        note_ = "Saved - everyone sees it now." + (warning.isEmpty() ? "" : " " + warning);
        a_.status(note_);
        if (row != null) openRow(row);
        else if (cur_ != null) openRow(cur_);
    }

    // One request with a body (null: none) and the token; the answer as it came (not tried again: a change).
    Answer send(String method, String path, String ctype, byte[] body, String token) {
        Answer r = new Answer();
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(base() + path).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(60000);
            c.setRequestMethod(method);  // (Android's HttpURLConnection takes PATCH)
            c.setRequestProperty("Authorization", "Bearer " + token);
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", ctype);
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream os = c.getOutputStream()) {
                    os.write(body);
                }
            }
            r.status = c.getResponseCode();
            r.data = readAll(r.status < 400 ? c.getInputStream() : c.getErrorStream());
        } catch (IOException e) {
            r.status = 0;
            r.why = e.getMessage() != null ? e.getMessage() : "no answer";
        } finally {
            if (c != null) c.disconnect();
        }
        return r;
    }

    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        if (in == null) return b.toByteArray();
        try (InputStream is = in) {
            byte[] buf = new byte[1 << 15];
            for (int n; (n = is.read(buf)) > 0 && b.size() <= 1 << 20; ) b.write(buf, 0, n);
        }
        return b.toByteArray();
    }

    // PUT /api/mods/<id>/file: the new .svrmod (multipart: version, svrmod) with the bar; Cancel stops it.
    void sendNewVersion(final Uri uri, final String fileName, final long size, final String version) {
        if (!ownerNow() || busy_ || downloading_) return;
        if (version.isEmpty() || version.equals(cur_.optString("version"))) {
            a_.status("A new version of the file needs a new version number.");
            return;
        }
        busy_ = true;
        downloading_ = true;  // (the bar and Cancel in the buttons' place, as a download's)
        cancel_ = false;
        final int id = cur_.optInt("id");
        final String t = token();
        buttons_.setVisibility(View.GONE);
        progress_.setVisibility(View.VISIBLE);
        bar_.setProgress(0);
        progressText_.setText("Sending the new version…");
        final Answer r = new Answer();
        a_.background(() -> {
            String b = "----svr" + Long.toHexString(System.nanoTime());
            byte[] head = ("--" + b + "\r\nContent-Disposition: form-data; name=\"version\"\r\n\r\n" + version + "\r\n--" + b
                + "\r\nContent-Disposition: form-data; name=\"svrmod\"; filename=\"" + fileName.replace("\"", "")
                + "\"\r\nContent-Type: application/zip\r\n\r\n").getBytes(StandardCharsets.UTF_8);
            byte[] tail = ("\r\n--" + b + "--\r\n").getBytes(StandardCharsets.UTF_8);
            HttpURLConnection c = null;
            try (InputStream in = a_.getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("can't read the file");
                c = (HttpURLConnection) new URL(base() + "/api/mods/" + id + "/file").openConnection();
                c.setConnectTimeout(10000);
                c.setReadTimeout(300000);  // (the server stores it before it answers)
                c.setRequestMethod("PUT");
                c.setDoOutput(true);
                c.setRequestProperty("Authorization", "Bearer " + t);
                c.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + b);
                if (size >= 0) c.setFixedLengthStreamingMode(head.length + size + tail.length);
                else c.setChunkedStreamingMode(1 << 16);
                long sent = 0, last = 0;
                try (OutputStream os = c.getOutputStream()) {
                    os.write(head);
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) {
                        if (cancel_) throw new IOException("cancelled");
                        os.write(buf, 0, n);
                        sent += n;
                        long now = System.currentTimeMillis();
                        if (now - last >= 200) {
                            last = now;
                            final long s0 = sent;
                            final int at = size > 0 ? (int) (s0 * 1000 / size) : 0;
                            a_.runOnUiThread(() -> {
                                bar_.setProgress(at);
                                if (!cancel_) progressText_.setText("Sending the new version - " + FileOps.human(s0)
                                    + (size > 0 ? " of " + FileOps.human(size) : ""));
                            });
                        }
                    }
                    os.write(tail);
                }
                a_.runOnUiThread(() -> progressText_.setText("Sent - the server is storing it…"));
                r.status = c.getResponseCode();
                r.data = readAll(r.status < 400 ? c.getInputStream() : c.getErrorStream());
            } catch (IOException e) {
                r.status = 0;
                r.why = cancel_ ? "cancelled" : e.getMessage() != null ? e.getMessage() : "no answer";
            } finally {
                if (c != null) c.disconnect();
            }
        }, () -> {
            busy_ = false;
            downloading_ = false;
            boolean cancelled = cancel_;
            cancel_ = false;
            progress_.setVisibility(View.GONE);
            buttons_.setVisibility(View.VISIBLE);
            if (cancelled) {
                a_.status("Sending the new version cancelled - the mod is unchanged.");
                updateOwner();
                return;
            }
            changed(r, "The new version", id, false);
        });
    }
}
