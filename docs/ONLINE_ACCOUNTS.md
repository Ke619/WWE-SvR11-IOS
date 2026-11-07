# Online accounts (2.1.6): sign-up, re-activation and reset codes

The Community Creations server (`port/server/server.py`, with the rules in `signup.py`) uses **codes only, no email**. The server owner makes every code on the dashboard and gives it to the player.

## The switch: account_checks (dashboard → Settings)

- **off** (the default, until 2.1.6 is out): nobody is blocked.
  - 2.1.5 launchers and games sign in, play online and sign up as before.
  - The codes already work, so accounts can be prepared ahead of time.
  - A 2.1.6 launcher signs up with an invite code even while the switch is off.
- **on:** the rules below, at once, with no restart. Before it saves, the dashboard warns how many accounts would be kept off online at that moment.

## The rules (account_checks on)

| Who | To play online |
|---|---|
| A new player | Signs up in the 2.1.6 launcher's (or the Android app's) Online tab with a name, a password and a **one-use invite code** from the server owner. |
| An account from before 2.1.6 | Blocked online until, in the 2.1.6 launcher, the player types the **re-activation code** the server owner made for that one account. |
| The `--admin-account` account | Needs no re-activation code. |
| An account signed up from a 2.1.5 launcher while the switch was off | Treated like an account from before: it needs a re-activation code. |

- **Forgotten password:**
  1. The server owner makes a **reset code** for that account (the Players tab → its row → Reset code).
  2. The player types it in the launcher's Forgot password, with their name and a new password.
  - The code works once, for 24 hours.
  - The new password ends every session of the account.
- **Codes:**
  - They look like `XXXX-XXXX-XXXX`, without 0/O/1/I; letter case and spacing don't matter.
  - Each works once, and the server keeps only a SHA-256 hash.
  - A re-activation or reset code works only for its own account. A sign-up code works only for a new account.
  - Making a new re-activation or reset code stops that account's older open one.
- **Online** means:
  - the game's sign-in (`/api/session`);
  - presence and friends;
  - matches (the relay);
  - Community Creations.

  The launcher's own account requests work before that, so a blocked player can still enter their code.
- **Password change** (`/api/password/change`) ends the account's other sessions.
- **Rate limits** (memory only):
  - sign-up: 5 an hour per IP;
  - sign-in: 10 per 10 minutes per IP and per name;
  - re-activation: 8 per 30 minutes per account;
  - reset: 10 per 30 minutes per name, 20 per IP.
- **Names** are unique in any letter case.
- **Old clients** never fail silently:
  - A 2.1.5 launcher gets "Online needs the 2.1.6 launcher now: update to 2.1.6, then sign in there with the re-activation code the server owner gives you." (or, signing up, the invite-code version).
  - A 2.1.5 game gets the same text from the server, though it shows its own "Sign in…" message.
  - A 2.1.6 game shows the server's reason in the ONLINE dialog.

## The dashboard

**Invites tab:**
- **The account checks line:** the switch's state, and how many accounts are (or would be) kept off online.
- **Make codes:** a note ("who it's for") and how many. They show **once**, so copy them then.
- **Codes:** every sign-up, re-activation and password reset code, with who used it and when (reset codes expire after 24 hours). Revoke an open one.

**Players tab → Accounts:**
- **Online access:** ok, or needs its re-activation code.
- **Re-activation code:** made for that account and shown once. Give it to that player.
- **Activate:** lets the account in without a code.
- **Reset code:** a forgotten password, shown once.

**On the server's machine:** `server.py --data <data> --activate NAME` lets an account in without a code, then exits.

## Netcode (dashboard → Settings → netcode)

- **Values:** `lockstep` (the default) | `lockstep_vclock` | `rollback`.
- **How it reaches the games:** every `/api/friends` config sends it.
  - The host's game latches it into session property 0x1000F005 when it creates the session (0 = lockstep, 2 = lockstep_vclock, 1 = rollback).
  - Joiners follow the session's value. A missing or unknown value means lockstep.
  - Games that don't have a mode play lockstep.

## Deploying

1. **The new files:** `vps_install.sh` installs `signup.py` with the other files.
2. **First start:**
   - The database is backed up to `<data>/backups/community-<time>-before-codes.db`.
   - One column is added (`accounts.activated`) and the `invites` table is made. Nothing is removed.
   - With account_checks off, nothing changes for players.
3. **The storage node** (`--role storage`) has no accounts. It never opens `community.db`: no migration, no backup.
4. **Before the switch goes on** (when 2.1.6 is out): make re-activation codes for the players who keep playing, and invite codes for new ones. Then switch account_checks on.

## Tests

- **`port/server/test_signup.py`:** starts its own server on a temporary data folder with a 2.1.5-style database (port 8561), then checks:
  - the switch off (2.1.5 launcher and game as before);
  - switching it on live;
  - every flow above.
- **`--test-open-accounts`:** the old rules for private test servers on 127.0.0.1 only; it refuses any other `--host`. The repo's test scripts and `tools/test_server.ps1` start their servers with it.
- **The launcher's Online tab:** `--capture online <file.bmp>` with `SVR2011_CAPTURE_ONLINE=<step>;<step>...`:
  - fields: `name=`, `password=`, `code=`;
  - clicks: `click=register|forgot|back|step|signin`.
