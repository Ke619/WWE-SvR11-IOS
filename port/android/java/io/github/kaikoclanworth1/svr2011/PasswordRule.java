// WWE SmackDown vs. Raw 2011 - Android launcher: the rule for a new online password (2.2.2; the server's own check is
// port/server/signup.py password_problem, the launcher's the same). Shown next to the password field when one is set
// (Create account, Forgot password) and checked as the player types; nothing is sent while there's a problem. The
// server still checks and its refusal ("error") is shown as it is. Never logs a password.
package io.github.kaikoclanworth1.svr2011;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

final class PasswordRule {
    static final int kMin = 8, kMax = 128;
    // (GET /api/status password_rule says the same)
    static final String kRule = "At least 8 characters - use a mix of letters, numbers and symbols; not your name.";
    static final String kOk = "✓ Strong enough";

    // signup.py COMMON_PASSWORDS (lower case; one matches with digits / symbols added at its end too).
    static final Set<String> kCommon = new HashSet<>(Arrays.asList(
        "password", "passw0rd", "password1", "qwerty", "qwertyuiop", "asdfgh", "asdfghjkl", "zxcvbnm",
        "123456", "1234567", "12345678", "123456789", "1234567890", "111111", "000000", "123123", "654321",
        "abc123", "abcd1234", "iloveyou", "letmein", "welcome", "monkey", "dragon", "football", "baseball",
        "basketball", "soccer", "hockey", "master", "shadow", "sunshine", "princess", "superman", "batman",
        "trustno1", "starwars", "whatever", "freedom", "hello", "hellothere", "secret", "login", "admin",
        "administrator", "changeme", "default", "computer", "internet", "michael", "jordan", "charlie",
        "pokemon", "minecraft", "fortnite", "gaming", "gamer", "playstation", "xbox", "xbox360", "nintendo",
        "wwe", "wrestling", "smackdown", "smackdownvsraw", "raw", "svr", "svr2011", "johncena", "undertaker",
        "randyorton", "reymysterio", "thebigshow", "kane", "batista", "edge", "triplehhh", "tripleh", "cmpunk",
        "stonecold", "therock"));

    // The password without the digits and symbols at its end ("password123!" -> "password"), as signup.py's
    // re.sub(r"[^a-z]+$", "", low).
    static String core(String low) {
        int end = low.length();
        while (end > 0) {
            char c = low.charAt(end - 1);
            if (c >= 'a' && c <= 'z') break;
            end--;
        }
        return low.substring(0, end);
    }

    // The first problem with `password` for account `name` (a short line to show), or "" when it can be used. The
    // order and the comparisons are signup.py's.
    static String problem(String password, String name) {
        int n = password.codePointCount(0, password.length());
        if (n < kMin) return (kMin - n) + " more character" + (kMin - n == 1 ? "" : "s") + " needed";
        if (n > kMax) return kMax + " characters at most";
        String low = password.toLowerCase(Locale.ROOT);
        String core = core(low);
        String nm = name == null ? "" : name.trim().toLowerCase(Locale.ROOT);
        if (!nm.isEmpty() && (low.equals(nm) || core.equals(nm) || core.equals(core(nm)))) return "Not your account name";
        if (kCommon.contains(low) || kCommon.contains(core)) return "Too common - pick another one";
        boolean letters = false, digits = false, symbols = false;
        for (int i = 0; i < password.length(); ) {
            int cp = password.codePointAt(i);
            i += Character.charCount(cp);
            if (Character.isLetter(cp)) letters = true;
            else if (Character.isDigit(cp)) digits = true;
            else symbols = true;
        }
        int kinds = (letters ? 1 : 0) + (digits ? 1 : 0) + (symbols ? 1 : 0);
        if (kinds < 2) {
            if (letters) return "Add a number or a symbol";
            if (digits) return "Add a letter or a symbol";
            return "Add a letter or a number";
        }
        return "";
    }
}
