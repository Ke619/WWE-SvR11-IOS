// WWE SmackDown vs. Raw 2011 - the launcher's Online page: play online, the
// server (Default: the port's own, its address not shown; Custom: one the
// player types) and the account on it - sign in, create one (name, password,
// invite code), forgot password (the reset code the server owner made for the
// account, and a new password), sign out (<server>/api/login, /api/register,
// /api/logout, /api/password/reset; JSON). Signing in writes online_token,
// online_xuid, online_name and online_enabled for the game. An account the
// server keeps off online (2.1.6 with its account checks on, server.py
// signup.py) shows its step: the re-activation code from the server owner
// (accounts from before 2.1.6; /api/account, /api/reactivate). Codes only - no
// email.
// Friends: the account's list on the server (/api/friends) - who's online (in
// the game's ONLINE menus) or in an online match, and who added the player;
// refreshed every 20 seconds while the page shows.
// Connection: the check (NatCheck) when asked for, and the last match's path from the game's log.

package io.github.kaikoclanworth1.svr2011;

import android.text.InputType;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class OnlinePage {
    static final String kDefaultServer = "https://sho-ti.me/svr";
    static final String kCustomServer = "127.0.0.1:8411";
    // The account card's modes, signed out.
    static final int kSignIn = 0, kSignUp = 1, kForgot = 2;
    int invite_ = -1;  // (sign-up: the server needs an invite code 1, doesn't 0, didn't say -1)

    private final LauncherActivity a_;
    private Spinner kind_;
    private LinearLayout addressBox_, signedIn_, nameRow_, passwordRow_, codeRow_;
    private EditText address_, name_, password_, code_;
    private TextView who_, accountStatus_;
    private Button signIn_, create_, signOut_, forgot_;
    private boolean busy_;
    private int mode_ = kSignIn;
    // (signed in: the server says the account still needs its re-activation code; checked once a start)
    private boolean needsCode_, checked_;
    private String note_ = "";
    private LinearLayout friendsCard_, friendRows_;
    private View friendsHeading_;
    private EditText friendName_;
    private TextView friendsStatus_;
    private boolean friendsBusy_;
    private final android.os.Handler timer_ = new android.os.Handler(android.os.Looper.getMainLooper());
    private TextView natText_;
    private Button natCheck_;

    OnlinePage(LauncherActivity a) { a_ = a; }

    GameSettings settings() { return a_.settings(); }

    // A field's row in its own box (shown or hidden with the card's mode).
    LinearLayout fieldRow(LinearLayout card, String label, EditText field) {
        LinearLayout r = new LinearLayout(a_);
        r.setOrientation(LinearLayout.VERTICAL);
        a_.row(r, label, null, field);
        card.addView(r);
        return r;
    }

    View build() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Community Creations: share Created Superstars, Paint Tool logos, highlight reels "
            + "and more with other players, through a Community Creations server (the original servers closed in "
            + "2014). Online matches too: Player Match and Royal Rumble.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);

        LinearLayout play = a_.card(c, "Online");
        a_.toggle(play, "Play online", null, "online_enabled", false);

        // Connection: only when the player asks.
        LinearLayout connection = a_.card(c, "Connection");
        natText_ = a_.text("Can your phone play other players directly, without the relay?", 14, LauncherActivity.kDim);
        natText_.setPadding(0, a_.dp(10), 0, a_.dp(10));
        connection.addView(natText_);
        natCheck_ = a_.button("Check my connection", LauncherActivity.kCard);
        natCheck_.setOnClickListener(v -> checkConnection());
        LinearLayout natRow = new LinearLayout(a_);
        natRow.setPadding(0, 0, 0, a_.dp(10));
        natRow.addView(natCheck_, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));
        connection.addView(natRow);

        // Server: Default / Custom (which one: from the address saved).
        LinearLayout server = a_.card(c, "Server");
        kind_ = new Spinner(a_, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> kinds = new ArrayAdapter<String>(a_, android.R.layout.simple_spinner_item,
            new String[] {"Default", "Custom"}) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(LauncherActivity.kText);
                t.setTextSize(15);
                return t;
            }
        };
        kinds.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        kind_.setAdapter(kinds);
        a_.row(server, "Server", null, kind_);
        addressBox_ = new LinearLayout(a_);
        addressBox_.setOrientation(LinearLayout.VERTICAL);
        address_ = a_.field(kCustomServer, 120, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        a_.row(addressBox_, "Address", "host:port (or the URL) of a Community Creations server", address_);
        View sep = new View(a_);
        sep.setBackgroundColor(LauncherActivity.kLine);
        addressBox_.addView(sep, 0, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1));
        server.addView(addressBox_);
        // (only the player's own choice is saved: a spinner also reports layouts)
        final boolean[] touched = {false};
        kind_.setOnTouchListener((v, e) -> {
            touched[0] = true;
            return false;
        });
        kind_.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                boolean custom = position == 1;
                addressBox_.setVisibility(custom ? View.VISIBLE : View.GONE);
                if (a_.loading() || !touched[0] || custom == isCustom(server())) return;
                if (custom && address_.getText().toString().trim().isEmpty()) address_.setText(kCustomServer);
                saveServer();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        address_.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override
            public void afterTextChanged(android.text.Editable s) {
                if (!a_.loading() && kind_.getSelectedItemPosition() == 1) saveServer();
            }
        });

        // Account on that server.
        LinearLayout account = a_.card(c, "Account");
        signedIn_ = new LinearLayout(a_);
        signedIn_.setOrientation(LinearLayout.VERTICAL);
        who_ = a_.text("", 16, LauncherActivity.kText);
        who_.setPadding(0, a_.dp(12), 0, a_.dp(12));
        signedIn_.addView(who_);
        account.addView(signedIn_);
        name_ = a_.field("Name", 15, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        nameRow_ = fieldRow(account, "Name", name_);
        password_ = a_.field("Password", 100, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        passwordRow_ = fieldRow(account, "Password", password_);
        code_ = a_.field("Code", 20, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        codeRow_ = fieldRow(account, "Code", code_);
        accountStatus_ = a_.text("", 13, LauncherActivity.kDim);
        accountStatus_.setPadding(0, a_.dp(6), 0, a_.dp(10));
        account.addView(accountStatus_);

        signIn_ = a_.button("Sign in", LauncherActivity.kRed);
        signIn_.setOnClickListener(v -> primary());
        create_ = a_.button("Create account", LauncherActivity.kCard);
        create_.setOnClickListener(v -> secondary());
        LinearLayout buttons = a_.pair(signIn_, create_);
        c.addView(buttons, a_.fullWidth(10));
        forgot_ = a_.button("Forgot password?", LauncherActivity.kCard);
        forgot_.setOnClickListener(v -> {
            mode_ = kForgot;
            note_ = "";
            a_.status("");  // (the bottom line: no older "Server saved." through the account steps)
            refreshAccount();
        });
        c.addView(forgot_, a_.fullWidth(10));
        signOut_ = a_.button("Sign out", LauncherActivity.kCard);
        signOut_.setOnClickListener(v -> signOut());
        c.addView(signOut_, a_.fullWidth(10));

        // Friends (signed in only).
        friendsCard_ = a_.card(c, "Friends");
        friendsHeading_ = c.getChildAt(c.indexOfChild(friendsCard_) - 1);
        friendRows_ = new LinearLayout(a_);
        friendRows_.setOrientation(LinearLayout.VERTICAL);
        friendsCard_.addView(friendRows_);
        friendsStatus_ = a_.text("", 13, LauncherActivity.kDim);
        friendsStatus_.setPadding(0, a_.dp(8), 0, a_.dp(4));
        friendsCard_.addView(friendsStatus_);
        friendName_ = a_.field("Player name", 15, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        a_.row(friendsCard_, "Player", "Type a name, or tap a friend", friendName_);
        Button add = a_.button("Add friend", LauncherActivity.kRed), remove = a_.button("Remove", LauncherActivity.kCard);
        add.setOnClickListener(v -> changeFriend(true));
        remove.setOnClickListener(v -> changeFriend(false));
        LinearLayout friendButtons = a_.pair(add, remove);
        friendButtons.setPadding(0, 0, 0, a_.dp(10));
        friendsCard_.addView(friendButtons);
        final View page = c;
        Runnable tick = new Runnable() {
            @Override
            public void run() {
                if (page.isShown() && !steps()) fetchFriends(null);
                timer_.postDelayed(this, 20000);
            }
        };
        timer_.postDelayed(tick, 20000);

        a_.onRefresh(this::refresh);
        return c;
    }

    void checkConnection() {
        natCheck_.setEnabled(false);
        natText_.setText("Checking your connection\u2026");
        final String stun = settings().getString("p2p_stun", "");
        final int port = settings().getInt("p2p_port", 36000);
        final NatCheck[] result = new NatCheck[1];
        a_.background(() -> result[0] = NatCheck.run(stun, null, null, null), () -> {
            natCheck_.setEnabled(true);
            natCheck_.setText("Check again");
            natText_.setText(result[0] != null ? result[0].text(port) : "The check couldn't run.");
        });
    }

    // body null: the list; else {"add": name} / {"remove": name}.
    void fetchFriends(String body) {
        final String token = settings().getString("online_token", "");
        if (token.isEmpty() || friendsBusy_) return;
        friendsBusy_ = true;
        final String url = base(server()) + "/api/friends?client=2";
        final Object[] result = new Object[1];
        a_.background(() -> {
            try {
                result[0] = post(url, body, token);
            } catch (Exception e) {
                result[0] = e;
            }
        }, () -> {
            friendsBusy_ = false;
            showFriends(result[0]);
        });
    }

    void showFriends(Object result) {
        if (settings().getString("online_token", "").isEmpty()) return;
        JSONObject r = result instanceof JSONObject ? (JSONObject) result : null;
        JSONArray friends = r != null ? r.optJSONArray("friends") : null;
        if (friends == null) {
            int code = r != null ? r.optInt("_status", 0) : 0;
            friendsStatus_.setText(code == 404 && r.optString("error", "").isEmpty()
                ? "This server has no friends list yet." : error(result, false));
            return;
        }
        friendRows_.removeAllViews();
        int online = 0;
        for (int i = 0; i < friends.length(); i++) {
            JSONObject f = friends.optJSONObject(i);
            if (f == null) continue;
            String status = f.optString("status"), label = "Offline";
            if (status.equals("playing")) { label = "In an online match"; online++; }
            else if (status.equals("online")) { label = "Online"; online++; }
            else if (!f.optBoolean("mutual")) label = "Offline (hasn't added you)";
            friendRow(f.optString("name"), label, !status.equals("offline"));
        }
        JSONArray added = r.optJSONArray("added_you");
        for (int i = 0; added != null && i < added.length(); i++)
            friendRow(added.optString(i), "Added you - add them back", false);
        int now = r.optInt("online_now", -1);
        friendsStatus_.setText((friends.length() == 0 ? "No friends yet: type a player's name and add them."
            : online + " of " + friends.length() + " friend" + (friends.length() == 1 ? "" : "s") + " online.")
            + (now >= 0 ? "  " + now + " player" + (now == 1 ? "" : "s") + " playing now." : ""));
    }

    void friendRow(String name, String status, boolean on) {
        LinearLayout row = new LinearLayout(a_);
        row.setGravity(android.view.Gravity.CENTER_VERTICAL);
        row.setPadding(0, a_.dp(10), 0, a_.dp(10));
        TextView n = a_.text(name, 16, LauncherActivity.kText);
        row.addView(n, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        TextView st = a_.text(status, 14, on ? 0xFF4CD964 : LauncherActivity.kDim);
        row.addView(st);
        row.setOnClickListener(v -> friendName_.setText(name));
        if (friendRows_.getChildCount() > 0) {
            View sep = new View(a_);
            sep.setBackgroundColor(LauncherActivity.kLine);
            friendRows_.addView(sep, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1));
        }
        friendRows_.addView(row);
    }

    void changeFriend(boolean add) {
        String name = friendName_.getText().toString().trim();
        if (name.isEmpty()) {
            friendsStatus_.setText(add ? "Type the player's name to add." : "Tap a friend (or type the name) to remove.");
            return;
        }
        try {
            JSONObject body = new JSONObject();
            body.put(add ? "add" : "remove", name);
            fetchFriends(body.toString());
            if (add) friendName_.setText("");
        } catch (org.json.JSONException e) {
        }
    }

    String server() { return settings().getString("online_server", kDefaultServer); }

    static boolean isCustom(String server) { return !server.isEmpty() && !server.equals(kDefaultServer); }

    void saveServer() {
        boolean custom = kind_.getSelectedItemPosition() == 1;
        String value = custom ? address_.getText().toString().trim() : kDefaultServer;
        if (custom && value.isEmpty()) return;
        if (value.equals(server())) return;
        settings().setString("online_server", value);
        a_.saved("Server");
        refreshAccount();
    }

    void refresh() {
        String s = server();
        boolean custom = isCustom(s);
        kind_.setSelection(custom ? 1 : 0, false);
        addressBox_.setVisibility(custom ? View.VISIBLE : View.GONE);
        address_.setText(custom ? s : kCustomServer);
        refreshAccount();
    }

    boolean signedIn() { return !settings().getString("online_token", "").isEmpty(); }

    // (signed in, an account the server keeps off online: its step shows)
    boolean steps() { return signedIn() && needsCode_; }

    static void show(View v, boolean on) { v.setVisibility(on ? View.VISIBLE : View.GONE); }

    void refreshAccount() {
        boolean in = signedIn(), steps = steps();
        String name = settings().getString("online_name", "");
        show(signedIn_, in);
        who_.setText("Signed in as " + name + (isCustom(server()) ? " on " + server() : "") + "."
            + (steps ? " Online needs the re-activation code the server owner made for your account." : ""));
        if (!in && name_.getText().length() == 0 && mode_ == kSignIn) name_.setText(name);
        show(nameRow_, !in);
        show(passwordRow_, !in);
        show(codeRow_, (!in && mode_ != kSignIn && !(mode_ == kSignUp && invite_ == 0)) || steps);
        name_.setHint(mode_ == kSignUp ? "3-15 characters" : "Name");
        password_.setHint(mode_ == kSignIn ? "Password" : "8 or more characters");
        code_.setHint(steps ? "Re-activation code" : mode_ == kSignUp ? (invite_ == 1 ? "Invite code" : "Invite code (if the server asks)") : "Reset code");
        String primary = "Sign in", secondary = "Create account";
        if (steps) {
            primary = "Re-activate";
        } else if (mode_ == kSignUp) {
            primary = "Create account";
            secondary = "Back";
        } else if (mode_ == kForgot) {
            primary = "Set password";
            secondary = "Back";
        }
        signIn_.setText(primary);
        create_.setText(secondary);
        show(signIn_, !in || steps);
        show(create_, !in);
        show((View) signIn_.getParent(), !in || steps);
        show(forgot_, !in && mode_ == kSignIn);
        show(signOut_, in);
        String fallback = "";
        if (!in && mode_ == kSignIn) fallback = "Sign in, or create an account (some servers ask for an invite code).";
        else if (!in && mode_ == kSignUp) fallback = invite_ == 0 ? "Choose a name and a password."
            : invite_ == 1 ? "A new account needs an invite code from the server owner."
            : "The invite code: only if the server owner gave you one.";
        else if (!in) fallback = "Ask the server owner for a reset code for your account (it works once, for 24 "
            + "hours), then choose a new password.";
        accountStatus_.setText(note_.isEmpty() ? fallback : note_);
        // (an account works on the server that made it: sign out to change servers)
        kind_.setEnabled(!in);
        address_.setEnabled(!in);
        boolean enabled = !busy_;
        signIn_.setEnabled(enabled);
        create_.setEnabled(enabled);
        forgot_.setEnabled(enabled);
        signOut_.setEnabled(enabled);
        show(friendsCard_, in && !steps);
        show(friendsHeading_, in && !steps);
        if (in && !checked_) {
            checked_ = true;
            checkAccount();
        }
        if (in && !steps) fetchFriends(null);
        else friendRows_.removeAllViews();
    }

    static boolean validName(String n) {
        if (n.length() < 3 || n.length() > 15) return false;
        for (char ch : n.toCharArray())
            if (!(Character.isLetterOrDigit(ch) && ch < 128) && ch != ' ' && ch != '_' && ch != '.' && ch != '-')
                return false;
        return true;
    }

    // An account answer's step (api/login, api/account, ...).
    void readSteps(JSONObject r) { needsCode_ = r.optBoolean("needs_code"); }

    interface Done { void run(JSONObject r); }

    // A request to <server><path> (body null: a GET; token: the account's), then done(answer) on the UI thread -
    // an answer that didn't come: the status line says so.
    void call(String path, JSONObject body, boolean token, Done done) {
        if (busy_) return;
        busy_ = true;
        refreshAccount();
        final String url = base(server()) + path;
        final String t = token ? settings().getString("online_token", "") : null;
        final Object[] result = new Object[1];
        a_.background(() -> {
            try {
                result[0] = post(url, body != null ? body.toString() : null, t);
            } catch (Exception e) {
                result[0] = e;
            }
        }, () -> {
            busy_ = false;
            if (result[0] instanceof JSONObject) done.run((JSONObject) result[0]);
            else note_ = error(result[0], false);
            if (!note_.isEmpty()) a_.status(note_);  // (the bottom line: the step's result, not an older "Server saved.")
            refreshAccount();
        });
    }

    void primary() {
        a_.status("");  // (a step pressed: no older "Server saved." under it)
        if (steps()) step();
        else if (mode_ == kSignIn || mode_ == kSignUp) signIn(mode_ == kSignUp);
        else forgotReset();
    }

    void secondary() {
        mode_ = mode_ == kSignIn ? kSignUp : kSignIn;
        note_ = "";
        if (mode_ == kSignUp) askInvite();
        a_.status("");
        refreshAccount();
    }

    // The server's word on invite codes (its status's invite_needed; an older server: -1, the box shown).
    void askInvite() {
        invite_ = -1;
        final String url = base(server()) + "/api/status";
        final Object[] result = new Object[1];
        a_.background(() -> {
            try {
                result[0] = post(url, null, null);
            } catch (Exception e) {
                result[0] = e;
            }
        }, () -> {
            JSONObject r = result[0] instanceof JSONObject ? (JSONObject) result[0] : null;
            if (r != null && r.has("invite_needed")) invite_ = r.optBoolean("invite_needed") ? 1 : 0;
            if (invite_ == 0) code_.setText("");  // (hidden: nothing typed earlier goes with the sign-up)
            refreshAccount();
        });
    }

    void signIn(boolean create) {
        if (busy_) return;
        final String name = name_.getText().toString().trim(), password = password_.getText().toString();
        final String invite = code_.getText().toString().trim();
        if (!validName(name)) {
            note_ = "Names are 3-15 letters, digits, spaces or _ . -";
            refreshAccount();
            return;
        }
        if (create && (password.length() < 8 || (invite_ == 1 && invite.isEmpty()))) {
            note_ = invite_ == 1 ? "Type a password (8 or more characters) and your invite code."
                                 : "Type a password (8 or more characters).";
            refreshAccount();
            return;
        }
        if (password.isEmpty()) {
            note_ = "Type your password.";
            refreshAccount();
            return;
        }
        note_ = create ? "Creating the account…" : "Signing in…";
        JSONObject body = new JSONObject();
        try {
            body.put("name", name);
            body.put("password", password);
            body.put("client", 2);  // (this app walks the account's step; older ones get a text to update)
            if (create) body.put("invite", invite);
        } catch (org.json.JSONException e) {
            return;
        }
        call(create ? "/api/register" : "/api/login", body, false, r -> {
            if (r.optBoolean("ok") && !r.optString("token").isEmpty()) {
                settings().setString("online_token", r.optString("token"));
                settings().setString("online_xuid", r.optString("xuid"));
                settings().setString("online_name", r.optString("name", name));
                settings().setBool("online_enabled", true);
                password_.setText("");
                code_.setText("");
                readSteps(r);
                checked_ = true;
                mode_ = kSignIn;
                note_ = !settings().save() ? "Could not save the settings."
                    : steps() ? "" : (create ? "Account made. " : "") + "Signed in - online play is on.";
                a_.refresh();
            } else {
                note_ = error(r, create);
            }
        });
    }

    // The account's step: its re-activation code.
    void step() {
        final String code = code_.getText().toString().trim();
        if (code.isEmpty()) {
            note_ = "Type the code.";
            refreshAccount();
            return;
        }
        JSONObject body = new JSONObject();
        try {
            body.put("code", code);
        } catch (org.json.JSONException e) {
            return;
        }
        call("/api/reactivate", body, true, r -> {
            if (r.optBoolean("ok")) {
                readSteps(r);
                code_.setText("");
                note_ = steps() ? "" : "Account re-activated: you can play online.";
                if (!steps()) fetchFriends(null);
            } else {
                note_ = error(r, false);
            }
        });
    }

    // The signed-in account's step, from the server: once a start. A token the server ended (the password changed
    // or was reset) signs out.
    void checkAccount() {
        call("/api/account", null, true, r -> {
            int code = r.optInt("_status", 0);
            if (code == 200) {
                readSteps(r);
            } else if (code == 401) {
                settings().setString("online_token", "");
                settings().setString("online_xuid", "");
                settings().save();
                needsCode_ = false;
                note_ = "Signed out: your password was changed or reset. Sign in again.";
            }
        });
    }

    // A forgotten password: the reset code the server owner made for the account, and a new one.
    void forgotReset() {
        final String name = name_.getText().toString().trim(), code = code_.getText().toString().trim();
        final String password = password_.getText().toString();
        if (name.isEmpty() || code.isEmpty() || password.isEmpty()) {
            note_ = "Type your account's name, the reset code and a new password.";
            refreshAccount();
            return;
        }
        JSONObject body = new JSONObject();
        try {
            body.put("name", name);
            body.put("code", code);
            body.put("password", password);
        } catch (org.json.JSONException e) {
            return;
        }
        call("/api/password/reset", body, false, r -> {
            if (r.optBoolean("ok")) {
                mode_ = kSignIn;
                password_.setText("");
                code_.setText("");
                note_ = r.optString("message", "Your password is set: sign in with it.");
            } else {
                note_ = error(r, false);
            }
        });
    }

    void signOut() {
        if (busy_) return;
        final String token = settings().getString("online_token", "");
        final String base = base(server());
        busy_ = true;
        refreshAccount();
        // (signed out here whatever the server says)
        a_.background(() -> {
            try {
                post(base + "/api/logout", "{}", token);
            } catch (Exception e) {
            }
        }, () -> {
            busy_ = false;
            settings().setString("online_token", "");
            settings().setString("online_xuid", "");
            needsCode_ = false;
            mode_ = kSignIn;
            note_ = settings().save() ? "Signed out." : "Could not save the settings.";
            a_.status(note_);
            refreshAccount();
        });
    }

    // A bare host:port is plain http.
    static String base(String server) {
        String b = server.contains("://") ? server : "http://" + server;
        while (b.endsWith("/")) b = b.substring(0, b.length() - 1);
        return b;
    }

    static String error(Object r, boolean create) {
        if (r instanceof JSONObject) {
            String e = ((JSONObject) r).optString("error", "");
            if (!e.isEmpty()) return e;
            int code = ((JSONObject) r).optInt("_status", 0);
            if (code == 403) return create ? "That name is taken." : "Wrong name or password.";
            if (code == 429) return "Too many tries - wait a little and try again.";
            return "The server said no (" + code + ").";
        }
        return "Could not reach the server" + (r instanceof Exception && ((Exception) r).getMessage() != null
            ? ": " + ((Exception) r).getMessage() : ".");
    }

    // POSTs JSON (body null: a GET); the answer's JSON (any status), with "_status" added.
    static JSONObject post(String url, String body, String token) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(10000);
            c.setReadTimeout(20000);
            c.setRequestMethod(body != null ? "POST" : "GET");
            if (token != null) c.setRequestProperty("Authorization", "Bearer " + token);
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json");
                byte[] out = body.getBytes(StandardCharsets.UTF_8);
                c.setFixedLengthStreamingMode(out.length);
                try (OutputStream os = c.getOutputStream()) {
                    os.write(out);
                }
            }
            int code = c.getResponseCode();
            InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream();
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream is = in) {
                    FileOps.copy(is, b);
                }
            }
            JSONObject r;
            try {
                r = new JSONObject(b.toString("UTF-8"));
            } catch (org.json.JSONException e) {
                r = new JSONObject();
            }
            r.put("_status", code);
            return r;
        } finally {
            c.disconnect();
        }
    }
}
