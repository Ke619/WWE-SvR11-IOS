# Controls: keyboard, mouse and controller

MY WWE -> OPTIONS -> CONTROLS has three tabs (LB / RB, or Tab). On a phone only CONTROLLER shows; the on-screen
controller has its own editor.

| Tab | What it changes | Where it's kept |
| --- | --- | --- |
| KEYBOARD | The keys for each input | `keybind_*` |
| MOUSE | Mouse buttons for each input, mouse look, mouse speed | `keybind_*` ("LMB", "RMB", "MMB"), `mnk_mouse`, `mnk_sensitivity` |
| CONTROLLER | Each player's buttons and triggers, vibration | `pad_map_p1` .. `pad_map_p4` ("a=x,x=a": game input = physical input), `vibration` |

The sticks can't be remapped, and the driver has no mouse side buttons or wheel.

## Clashes

- **Controller:** the two buttons swap. A layout is always a whole permutation, so every action stays reachable. A
  setting that isn't one (edited by hand) is ignored and logged.
- **Keyboard and mouse:** the key or button moves to the new input, with a message under the list.

## The way back

- Keyboard: A, B, START and the left stick directions always keep a key. Clearing one, or moving away its only key, is
  refused with a message. A settings file without one gets that input's default keys at the start.
- The page itself always takes Enter, Esc, the arrows and the real pad buttons.
- Controller: a layout only applies in play, so menus always get the real A, B and START.

## In play

`PadMapApply` (pad_map.cpp) runs in the game's pad read (frame_rate.cpp, sub_82905058) when the player is in a match and
no pause menu is up (`PauseMenuUp`, report_page.cpp). A button still held when play resumes waits to be released, so the
A of RETURN TO GAME doesn't become a press of another button.

The pause menu itself keeps the real buttons. The game's pages opened from it, such as CONTROL SETTINGS, keep the match
clock running, so they count as play: their buttons and icons both follow the layout (with A and X swapped, the bar says
X SELECT and X selects). A layout is a permutation, so these pages can always be used and left.

## Online

The layout is local input. It changes the pad state before the game reads it, so the match recording and the game's
own input messages hold the game's buttons. Peers never see or need the layout, and it decides nothing else in the
game. `svrtest lan` with a layout on one game compares the two games' states.

## Prompts

- Keyboard and mouse players: the bound key's cap, as before (pad_icons.cpp; "LMB" and so on have caps).
- Controller players with a layout, in play:
  - Text icons: `MakeLaidOut` gives each button's glyph the place of the button that plays it.
  - Match prompt pictures: `LaidOutVersion` paints that button into the picture's button box, from another prompt with
    that button, else from the icon pages. Its boxes are the `kbbox` lines in pad_icons.txt.

Test aids: `SVR2011_TEST_PAD_MAP_LOG=1` (in or out of play, pause screen states), `SVR2011_TEST_PAD_MAP_ICONS=1` (icons
follow the layout in menus too), `SVR2011_PAD_ICONS_DUMP=<folder>` (prompts as made), `SVR2011_TEST_PAGE_KEYS=<file>`
(page keys, also "LMB", "RMB", "MMB"), script `set pad_map_p1 a=x,x=a`.
