# Community Creations name filter

The server (`port/server/namefilter.py`) checks the names and texts players put on it against a word list the
server owner edits on the dashboard's **Names** tab.

## What is checked

| Where | Texts | A refused one |
| --- | --- | --- |
| The game's Community Creations uploads (Sake `CreateRecord` / `UpdateRecord`) | every text field: the title, its search keys, tags (`UserKey0-2`), comments (`UserComment0-2`) | `NoPermission`; the reason in the `X-Svr-Notice` header - the game (2.1.6 on) shows it on screen |
| Discovery mods (upload, owner's edit, file update) | name, version, tags, description | HTTP 400 `{"error": reason}` - the launcher, Mod Maker and Android show it |
| Entrances (`PUT /api/entrance/<file>`) | the song's playlist and file name, the movie's name | HTTP 400 + `X-Svr-Notice` (on screen in the game) |
| Sign-up (`api/register`, every launcher) | the account name | "That account name isn't allowed - pick another." |
| Every upload above | the uploader's own account name (an account made before the filter, or while it was off) | "Your account name isn't allowed on Community Creations." |

The reason a player sees is always the same short text ("That name isn't allowed on Community Creations."); the
word that matched is only in the server log and on the dashboard.

## The list

`name_filter.json` in the server's data folder: `{"enabled", "words", "allow"}`. A new server makes it from a seed
list kept compressed inside `namefilter.py` (so the repo shows no plain list); from then on the dashboard edits the
file, and a save is used at once (no restart).

A line in the word list:

| Line | Matches |
| --- | --- |
| `hitler` (5+ letters) | inside a word too (`xHitlerx`), and spelled across words (`hit ler`, `h i t l e r`) |
| `fag` (under 5 letters) | a whole word only (and its plural), or spelled out letter by letter (`f a g`, `f.a.g`) - never inside another word, so `Scunthorpe`, `Raccoon` stay fine |
| `=word` | a whole word only, whatever its length |
| `*word` | inside words too, whatever its length |
| `two words` | a phrase: the letters together, across the words |
| `# ...` | a note |

The allow list holds words never flagged though a term is inside them (`snigger`).

How a name is read: Unicode NFKC and case folding; Cyrillic / Greek look-alike letters read as Latin; accents and
invisible characters (zero-width spaces) dropped; then three readings - leetspeak with 1 as `i`, with 1 as `l`
(`0 o, 3 e, 4 a, 5 s, 7 t, 8 b, 9 g, @ a, $ s, ! i, | i, + t` ...), and the letters alone. Anything else separates
words. A term's letters may repeat (`hiiitler`); a doubled letter in the term needs at least two (`coon` is not `con`).

## The dashboard (Names tab)

- **Filter on** - off lets everything through (Flagged names still lists what the list would catch).
- **Words / Allow list** - one per line, **Save list**.
- **Test a name** - would this pass with the saved list, which line matched, how the name was read.
- **Flagged names** - what is on the server now that the list catches: uploads, Discovery mods, entrances,
  accounts. Nothing is deleted on its own: Hide / Delete (and Ban / Delete for accounts) are the same buttons as on
  the Community Creations, Discovery and Players tabs.

API (admin): `GET/POST api/admin/names`, `POST api/admin/names/test {text}`, `GET api/admin/names/flagged`.

## Tests

- `python port/server/test_namefilter.py` - the matching, no server.
- `python port/server/test_names_server.py [--port 8562]` - a server of its own (temporary data): every place above,
  the switch, the dashboard endpoints, nothing deleted by a scan.
