# The replay panel

While a solve is running the screen is mostly black: candidate replays run in the fast loop and only the
overlay is drawn. The replay panel uses that dead time. It plays **GD's own record of the deepest verified
attempt** in the free area from the middle of the screen to the top-right, looping, so you can watch how far the
run has really got.

## When it is there, and the keys

It is on by default, **only while a solve runs and the screen is dark** (render off, which is how a solve runs).
The showing of the solution at the end is a replay and the panel is not drawn there; with the screen turned on (F5)
it is hidden -- not emptied: its mode, place, stop and pace are kept, and it comes back as it was when the screen
goes off again. Both ends of a solve empty it, so the next level never starts with the last one's picture.

`Tab` (Geode setting `key-replaypanel`) goes round **the whole path** (every stretch the run has got through, end to end;
the default) -> **its last stretch** (`replaypanelspan` ticks, 20 s) -> **off** -> the whole path. `replaypanel` in
`autorun.cfg` picks where it starts: 0 off, 1 the whole path, 2 the last stretch (3, which used to be a mode of the model's
plan, is read as 1). `F1` does not hide it: the panel only shows during a solve, and `F1` does not hide the overlay then either.

**The model's plan** is not a mode of its own: it is the end of the path. When the solver has found a plan, the path played is
GD's own verified one up to the anchor of the solver's latest search (drawn white as ever) and, from the anchor on, the walk
the model made of the plan it found, in blue -- the path, the player and the bar all change colour where the plan begins, a
ring marks the anchor, and the status line says "planned path (the model)". The whole path therefore ends with the plan, and
the last stretch is the last `replaypanelspan` ticks of it. The model may be wrong about what GD does; what the blue part
shows is how it *believes* the plan flies, which is the part of the run nothing else shows while the search is working. The
solver writes the walk next to the plan (`dp_tail.txt.trace.csv`); the panel reads it again when it changes, on a background
thread (polled once a second, whenever a solve runs), and a newer plan takes over when the replay starts over, so a viewer is
not taken off one half way (a walk of fewer than 120 ticks -- a search that is doomed from its anchor writes one -- does not
replace a longer plan). The walk stops at the model's own death, if the plan has one.

The last few plans of the model (`replaypanelplans`, default 8) are drawn together: the deepest is the path's end and what the
camera follows, the others are **ghosts** beside it -- each a faint line over its whole walk, a cross where the walk ends, and,
where its plan has the player at the tick on screen, a figure of that player in the shape of its mode (cube, ship, ball, ufo,
wave, robot, spider, swing), as faint as the line, the way the path's own player is drawn. The plan of a previous level is never
shown: the file as it stands when a solve or a level begins is not read until a search writes it again.

**The level bar** (the row of the overlay column drawn as GD's own level bar) has a second fill behind the verified one, in the
same blue: how far in x the model's deepest plan gets. It shows past the verified point, as the part of the level that is
planned and not yet confirmed, and the row's text says it ("level 43.0% (plan 49.5%)"). It is drawn whether or not the panel is
on screen. Moving parts past the end of the recording stand where the recording left them.

While the panel is on screen with a path to play (the screen dark), the transport keys are **the panel's**, not the
game's -- the level behind a solve is the loop's own verification replay and has nothing to move:

| key | the panel |
|---|---|
| `F2` | stop / start the replay |
| `F3` / `F4` | step it one tick / ten (a step stops it; held down they repeat) |
| `←` `→` | seek back / forward: a tap is five seconds, holding accelerates |
| `↑` `↓` | its pace, one notch (0.25x ... 16x; the game's spectating notches) |

**The mouse.** Click or drag on the panel's bar and the playhead goes to the pointer: the picture is the tick that far along
the stretch being played (the left end of the bar is the start of the stretch, the right end its end). The clock stops under
the pointer while it is held and goes on from where it was put when it is let go -- or stays stopped, if `F2` had stopped it.
The bar is thin, so the strip that takes a press is the bottom quarter of the panel, its whole width; a drag may leave the panel
sideways and keeps its end. Only that strip: a press is claimed while the panel is on screen with a path to play, and every
other click goes to whatever would have had it. It moves the panel's clock and nothing else -- the level behind a solve is
the loop's own replay -- and writes nothing the solver reads.

With the screen on these keys are the game's again (F2 pauses it, the arrows change its speed), as before. The keys
legend on the overlay names whose they are at the moment. The panel keeps its left edge clear of the overlay's own
text (the elapsed time at the end of the first line, the render state at the end of the keys line), moving right when a
line is long, and is held where it is -- marked "[held: the game is busy]" -- while the game takes more than twice the frame budget (80 ms
at least) over a frame (a recording pass, a slice of a section search that is longer than usual), since an animation drawn on frames like those only jerks.
The solve cuts the fast loop's batches at 30 ms (cfg `framebudgetms`), so a flight on a heavy level comes at 30-45 ms a
frame and the panel keeps moving through it; at the old 100 ms every frame of a flight was a held one. A section search hands
the frame back every 20 ms (cfg `secslicems`, 50 elsewhere), so the panel runs at 30 frames a second through one instead of 15.
What is left are the game's own long calls on the main thread (a level reset takes 2-3 s on a heavy level): the panel stands
still for those, because it is drawn by the thread that is in them. The log says how it
kept up, every 30 s: `replaypanel: last 30 s: N frames, M held ..., longest G ms`.

## What you are looking at

* **The path** (white) is the rows the game wrote as each tick of the deepest attempt ended (`anchors::g_deepest`), not
  the model's guess; only the blue end past the anchor is the model's walk of its plan (above). Wherever the model was wrong and GD's state was taken instead (a *fixup*), the path is
  already GD's: nothing has to be corrected on the way out. A bright trail runs behind the player, a faint one
  behind that, and a dim green one shows the road ahead up to the frontier.
* **The level** is the table the session opened (`objrects.txt`'s content, not the sliced copy the scene may
  hold), drawn as the boxes the physics reads: solids, hazards (turned ones as the oriented box of `obb.txt`),
  saws, portals, pads, rings, slopes as the surface line with the solid side filled. Decorations and triggers
  are not drawn.
* **The moving parts** come from the run's own recording of the moving geometry (`dp_groups.txt`) and hold each
  object's last recorded row until the next, the rule the model reads that file by. The status line says whether
  that recording belongs to the attempt on screen (`recorded`), to another one (`another attempt's`: the
  objects are then only close), or whether there is none (`as loaded`: they stand where the level loads them).
* **The corrections** are the run's fixups that lie on this path: a small orange tick on the bar under the
  picture for each, and a ring around the player's position as the path passes it (red for a fixup that records
  *GD ends the run here*).
* **The player** is a plain silhouette per mode (cube, ship, ball, ufo, wave, robot, spider, swing) on its
  hitbox, turned with the gameplay rotation and flipped with gravity. They are not the game's art, only enough
  to tell the modes apart.
* **The bar** along the bottom is the stretch being played, with the playhead on it.

Until the loop has had a death there is nothing verified to show, and the panel says so (a level its first plan
clears never has one: the solver keeps a deepest attempt when an attempt dies).

## Settings

| `autorun.cfg` | |
|---|---|
| `replaypanel=<n>` | where the panel starts: 0 off, **1 the whole path (default)**, 2 its last stretch. The key goes round all three. Workers launched through `gdtas.worker` get 0 |
| `replaypanelspan=<ticks>` | the length of the last stretch: 4800 (20 s) by default |
| `replaypanelplans=<n>` | how many of the model's latest plans are drawn: the deepest as the path's end and the rest as ghosts; 8 by default |
| `replaypanelspeed=<x>` | the pace it starts at, to the nearest of the game's spectating notches: 1 is real time (240 ticks a second) |
| `panelmouse=<s>:<f>[-<f2>],...` | a test hook for the mouse, which a check cannot send: `<s>` seconds after the panel can first take a press, the bar is pressed at fraction `<f>` of its length (dragged to `<f2>` over 8 frames and released there, or released at once when there is none). It goes through the handlers a touch reaches, not through the game's touch dispatcher |

## What it costs, and what it never does

It reads what the solver has already kept and writes nothing back; it is not part of any recorded run. Off, or
outside a solve, a frame costs a few tests. On, the level and the moving-parts recording are read and parsed once each on a background
thread, through a handle that lets the solver replace or delete the file meanwhile, and a frame is a few hundred
primitives into one `CCDrawNode`, clipped to the panel's box on the CPU (a draw node has no scissor). Measured in
the game: about 0.1 ms a frame on an official level, about 0.3 ms on a custom level with 88,000 boxes and 42,000
moving objects, whose level table takes 0.2 s to read and whose recording (190,000 rows) 0.4 s.

## Looking at it without the game

`replay_panel_geom.hpp` has no game in it, so `tools/panel_geom_test.cpp` builds and checks it alone
(`cmake --build build-dp --config Release --target panelgeomtest`): the readers, the clipping, the rotation
against GD's own oriented-box corners, every mode in every gameplay frame, the clock. With `--render` it draws
the frame the game would draw for one tick of a real run as an SVG:

```bash
build-dp/Release/panelgeomtest.exe --render objrects.txt obb.txt groups.txt dump.csv 10804 panel.svg
```

(`-` for a file you have not got.) The same picture, outside the game and with the model's trace beside GD's, is
the [divergence view](DIVERGENCE_VIEW.md).

## Limits

* Past the anchor the path is the model's belief of the plan, not the game: where the model is wrong, the blue part is wrong too.
* The partner of a dual player is placed for an unrotated run only.
* The picture is of the boxes, not the artwork, and the camera is its own (the player a third of the way in,
  ten blocks across), not the game's.
* A level whose recording is another attempt's shows its moving parts only approximately; the status line says so.
