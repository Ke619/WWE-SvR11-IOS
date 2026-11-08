package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

// The game's own REPORT A PROBLEM (src/report_page.cpp): its answers sent as the launcher's form sends them
// (ReportForm.send - the zip, the match and its mods, the pictures), from the game's own thread (JNI). The receipt
// goes into the same UserData/reports.json (Receipts), app "game".
final class GameReport {
    private GameReport() {}

    // The Discord name the game's form starts with (report_page.cpp SavedDiscordName): the one sent last here.
    static String discordName() {
        try {
            return ReportSend.discordLoad(InstallActivity.gameFolder());
        } catch (Throwable e) {
            return "";
        }
    }

    // answers: problem, happens, what, before, contact. match / picture: a path or "". test: the game's test run (a
    // server not on this phone refused, as ReportSend.refuses). Returns "ok\t<id>\t<seen before>" or "fail\t<why>".
    static String send(String server, String gameFolder, String version, String[] answers, boolean withSave,
                       boolean withMods, String match, String picture, boolean picturesLater, boolean test) {
        try {
            if (test) ReportSend.testRun = true;
            File game = new File(gameFolder);
            ReportSend.Answers a = new ReportSend.Answers();
            a.app = "game";
            a.version = version;
            a.os = "Android " + android.os.Build.VERSION.RELEASE;
            a.problem = answers[0];
            a.happens = answers[1];
            a.what = answers[2];
            a.before = answers[3];
            String[] name = new String[1];
            String why = ReportSend.discordWhy(answers[4], name);
            if (why != null) return "account\t" + why;
            a.contact = name[0];
            // (2.2.1: the app's saved sign-in - the game folder's svr2011.toml, as OnlinePage writes it)
            ReportSend.authToken = new GameSettings(game).getString("online_token", "");
            if (ReportSend.authToken.isEmpty())
                return "account\tSign in first: the app's Online page (Sign in or Create account).";
            File m = match == null || match.isEmpty() ? null : new File(match);
            List<ReportSend.Mod> mods = new ArrayList<>();
            if (m != null) {
                mods = ReportSend.matchMods(game, m);
                if (withMods) ReportSend.modsCheck(server, mods);
            }
            List<File> pics = new ArrayList<>();
            if (picture != null && !picture.isEmpty()) pics.add(new File(picture));
            File cache = new File(game, "Reports/.upload");
            ReportSend.Result r = ReportForm.send(cache, server, game, a, withSave, m, mods, withMods, pics, null,
                                                  picturesLater);
            if (r.ok) {
                ReportSend.discordSave(game, a.contact);
                return "ok\t" + r.id + "\t" + r.seenBefore;
            }
            if (!r.code.isEmpty()) return "account\t" + r.why;
            return "fail\t" + (r.why == null || r.why.isEmpty() ? "No answer from the server." : r.why);
        } catch (Throwable e) {
            return "fail\t" + e;
        }
    }
}
