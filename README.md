# gdsolver

**An automatic TAS solver for Geometry Dash** (2.209, Windows / Steam). It
solves a level by playing it — inside the game, from cold.

**[Download `gdsolver.solver.geode`](https://github.com/gdsolver/gdsolver/releases/latest)**
from the latest release, drop it into Geometry Dash's `geode/mods/` folder and
start the game. [Geode](https://geode-sdk.org) has to be installed first; see
[Installing](#installing). This release has no Android build
([docs/ANDROID.md](docs/ANDROID.md)).

<p align="center">
  <img src="docs/media/solve-to-replay.webp" width="720"
       alt="gdsolver solving Bloodbath: the dark fast loop with the overlay counters climbing, then the screen coming back for the solution replayed at 1x, and its wave part">
</p>

<p align="center"><sub>
  The last four seconds of solving <b>Bloodbath</b>, the first three of the
  replay, then ten of its wave part. The overlay is the loop's own: which round
  it is on, how far the best plan has reached, how far the search has got. The
  screen is dark because the search is running — the frames go to the fast loop
  instead — and it comes back when a candidate has actually cleared.
</sub></p>

## A few of the Extreme Demons it has solved

<p align="center">
  <a href="https://youtu.be/1ORlnG4zFfc"><img src="https://img.youtube.com/vi/1ORlnG4zFfc/maxresdefault.jpg" width="32%" alt="Video: Bloodbath solved by gdsolver"></a>
  <a href="https://youtu.be/lPco2GpVe2c"><img src="https://img.youtube.com/vi/lPco2GpVe2c/maxresdefault.jpg" width="32%" alt="Video: Antarctic Lights solved by gdsolver"></a>
  <a href="https://youtu.be/b1taOeFYbY0"><img src="https://img.youtube.com/vi/b1taOeFYbY0/maxresdefault.jpg" width="32%" alt="Video: Amethyst solved by gdsolver"></a>
</p>
<p align="center">
  <a href="https://youtu.be/TIzsdUE_3t0"><img src="https://img.youtube.com/vi/TIzsdUE_3t0/maxresdefault.jpg" width="32%" alt="Video: Thinking Space II solved by gdsolver"></a>
  <a href="https://youtu.be/NCWuxt7xN5w"><img src="https://img.youtube.com/vi/NCWuxt7xN5w/maxresdefault.jpg" width="32%" alt="Video: Society solved by gdsolver"></a>
  <a href="https://youtu.be/kCB-GeGtaD0"><img src="https://img.youtube.com/vi/kCB-GeGtaD0/maxresdefault.jpg" width="32%" alt="Video: Grief solved by gdsolver"></a>
</p>

| Level | Uploaded by |
|---|---|
| Bloodbath | Riot |
| Antarctic Lights | declanlc |
| Amethyst | iMist |
| Thinking Space II | Cairox |
| Society | Neomarbilan |
| Grief | IcEDCave |

Each of these six was solved cold, the way any level is — the play button,
**Solve**, and nothing else: no seeds, no previous solution, no hints, nothing but
the level. The videos were recorded on the builds that were current at the time
(v0.5.0 to v0.7.0); the v0.7.0 build clears all six.

These are other people's levels, solved by a bot. A solution is a TAS, not a
completion: while the mod drives nothing is recorded, and its replays are marked as
bot input (see [Safety](#safety--community-notes)). They are also custom levels,
and unlike the official levels they are not guaranteed to solve: on v0.5.0, of
100 rated levels drawn at random, 93 cleared within 20 minutes each, and of the 29
of those saved in 2.2, 26. Of 126 levels the game itself features (Daily, Weekly
and Event), 77 cleared on v0.7.0: 42 of 50 Daily, 24 of 50 Weekly and 11 of 26
Event levels ([docs/CUSTOM_LEVELS.md](docs/CUSTOM_LEVELS.md)).

Give it a level and it looks for an input sequence that clears it. It does not
re-implement the game's physics: an approximate, measured-where-it-matters model
drives a **reachability DP** (which `(y, velocity)` states are reachable at every
physics tick), and the game itself — running inside a **Geode mod** — is the
verifier that decides whether a candidate plan really clears. Where the replay
diverges from the model, the solver re-anchors on the game's own state and
continues from there; where the model cannot get through a stretch at all, it
searches the game itself there.

**All 22 official levels, and the 17 levels of the spin-off games, are solved
cold** — no seeds, no previous solutions, no external ground truth — by the loop
inside the game, with nothing installed but Geometry Dash, Geode and this mod.
Press a level's play button, pick **Solve** in the menu that opens, and it builds
the level, searches, replays the candidate, and repairs the plan wherever the
game disagrees with it; no external process is involved. Most levels take under
a minute. The solutions of the 22 are tracked in [`data/`](data/) as
`solution_lv<N>_dp.txt`, one `input=<tick>,<0|1>` line per press or release.

**With every coin, too.** Switch **Coins** on in that menu and the goal becomes
the end of the level *and* every coin in it. The 22 official levels and the six
spin-off levels that have coins (Meltdown's three and SubZero's three) are solved
that way as well, cold, and each of those solutions replays to a clear with every
coin counted by the game itself; the 22 are tracked beside the others as
`solution_lv<N>_coins.txt`. Coins picked up while the mod drives are never
awarded (see [Safety](#safety--community-notes)) — a coin route is a TAS result,
not progress on a save file.

Those levels are also the supported set — see [What's next](#whats-next) for
where custom levels and platformer mode stand.

The per-level results — repairs and times for the 22 official levels and the 17
spin-off levels, with and without coins, and how they compare with v0.4.0 — are in
[docs/RESULTS.md](docs/RESULTS.md).

## How it works

```mermaid
flowchart LR
    LV["a level<br/>objects, triggers, moving geometry"]
    subgraph GD["Geometry Dash + Geode"]
        direction TB
        MOD["src/ — the mod<br/>exports the level, replays a plan in a fast<br/>headless loop, dumps the state every tick"]
        DP["dp/ — reachability DP<br/>layered search over 240 Hz ticks<br/>exact states, dedupe on a quantised key"]
        MOD -- "level model" --> DP
        DP -- "candidate plan" --> MOD
    end
    PY["py/ — starts the game and measures it<br/>cold regression, replay verification,<br/>fidelity diagnostics, calibration rigs"]
    LV --> GD
    GD --> OUT["solution<br/>one line per press and release"]
    PY -.-> GD
```

Geometry Dash gives the player no steering input: the level sets the forward speed,
so the only decision per physics tick is *pressed or not*. Clearing a level is
then a reachability question over `(tick, y, vy, mode, gravity, size, ...)`, and
two properties make the game usable as the oracle for it: the physics are
deterministic under *click on steps*, and x is essentially a clock, so re-anchoring
the search on a later tick costs nothing. (*Essentially*, because stair snapping
leaves a sub-pixel phase and a rotation section turns the frame — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#1-the-problem).) That is the whole loop:

```mermaid
flowchart TD
    S["build the level model from the loaded level"]
    S --> D["solve from the current anchor"]
    D --> R{"replay the plan in the game"}
    R -- clears --> W["solution saved"]
    R -- dies --> F["find the first tick where the game and the model<br/>disagree, and record it as a fixup"]
    F --> A["re-anchor: take the player state the game really had<br/>a few ticks before the death"]
    A --> D
```

A *fixup* is the model agreeing to be wrong: from then on, whenever it meets
that same state again it applies the game's result instead of its own. Dead
bands, touch triggers that must be entered, ladders of anchor depths and
phantom-death detection exist for the other cases where the model is blind.
Every run is cold, and a run's fixups live only in that run.

How far each solve looks ahead follows the game's last verdict: while replays keep
dying soon after their anchor, the loop plans 3,000 ticks at a time and lets the
game check each step; once a plan outlives its step, it plans the rest of the
level. And a repair does not have to solve everything past the death again: when
the new search comes back onto the trajectory of the plan that died, it stops
there and keeps that plan's inputs from that point on.

Around that loop:

* **Section solving.** Where the repairs are not getting the plan across a wall,
  the loop replays to a practice checkpoint before it, searches the game itself
  from there, and splices back what the game reproduces —
  [docs/SECTION_SOLVE.md](docs/SECTION_SOLVE.md).
* **Coins.** With coins on, the collected set is part of the search state and a
  state that leaves a coin behind is dead; nothing steers the search towards a
  coin, the goal simply is not reached without it. Coins that wait for an item
  counter, a key or a switch are routed through what turns them on —
  [docs/COINS.md](docs/COINS.md).
* **Heavy levels** are solved on a copy without the objects the run cannot depend
  on, and every plan is verified on the level itself before it is filed —
  [docs/LEVEL_SLICE.md](docs/LEVEL_SLICE.md).
* **Moving geometry** is the game's own recording of each replay, joined at the
  tick the last replay died to a recording of the level played with no input.
  Some of it the game draws from random seeds (an Area Move's distance, Random
  triggers). While the bot drives, the seeds are set to the same values before
  every attempt, so every attempt meets the same draw and the model plans against
  the positions the recording shows; a solution is one for that draw, and played
  without the mod it can meet other positions.

* **`dp/`** — the solver core (`leveldp`, also linked into the mod). States are
  *exact*; the per-layer hash only deduplicates and never snaps a state, so every
  surviving path is a replayable plan. The DP is a candidate generator — the
  proof is always a plain replay in the game. Its physics rules are measured
  against the game (calibration levels, traces, disassembly), not fitted.
* **`src/`** — the Geode mod: level export, input injection, the per-tick state
  dump, moving-geometry recording, the fast headless loop, and the repair loop
  itself (`src/mod/repair.hpp`). It also blocks achievements and statistics
  while it is driving.
* **`py/`** — development tools; nothing here solves. The cold regression over
  all levels (`cold_regress.py`) and the manifest that says what a run measured
  (`cold_manifest.py`), replay verification, fidelity diagnostics,
  calibration-level generation and tests, and an MCP server for probing the
  running game.

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) has the long version.

## What's next

**Custom levels — most solve now; some of the newest gimmicks do not yet.**
Nothing in the loop is specific to the official levels, and any level can be
picked. On the v0.5.0 release build, 100 rated levels drawn at random were each
given up to 20 minutes, and 93 cleared cold (67 on v0.4.0). Levels built only from parts
that existed before 2.0 mostly solve as they are; most of this release's model
work went into the 2.1 and 2.2 gimmicks, and of the levels saved in those
versions 46 of 49 and 26 of 29 cleared (35 and 11 on v0.4.0).
The regression covers the official levels only, so a custom level's result is
outside what it keeps honest. The play menu says so up front: a yellow line on a
custom level that uses objects from 2.0 on, and a red one when a level holds
something the model cannot express at all.
[docs/CUSTOM_LEVELS.md](docs/CUSTOM_LEVELS.md) has the breakdown, which gimmicks
the model does not cover, and what a failure looks like. The rule stays to clear
what the game lets the loop clear: where the model has no answer, the section
solver searches the game itself, and a plan the game clears is a solution however
it was found.

The work on custom levels:

* **Model divergences** — the rules that disagree with what the game measures are
  fixed one at a time; this never stops.
* **The search's logic** — plans that die where the model is right; most of the
  official levels' repairs are of this kind ([docs/RESULTS.md](docs/RESULTS.md)).
  Also continuous.
* **Custom levels in the regression**, so that a change is measured on them too.
* **Heavy levels** — the speed of levels with very many objects.

**The next milestone: two-player levels, the gimmicks the model does not cover
yet, and divergences.**

* **Two-player levels.** The solver plans the second player's button too: each
  body is searched against the other's plan, and a plan carries both buttons (bit
  0 is player 1's jump, bit 1 player 2's, the other bits are reserved). A level
  where one player's press is a switch that moves the other's world is not solved
  yet ([docs/TWO_PLAYER.md](docs/TWO_PLAYER.md)).
* **The gimmicks the play menu names in red** — teleports that move you sideways
  or have several exits, orbs that push you, triggers set off by touch or a spawn,
  and the rest ([docs/CUSTOM_LEVELS.md](docs/CUSTOM_LEVELS.md)) — each measured
  against the game before it is modelled.
* **Divergences**, where the model and the game come apart on what it does cover.

**The search as a plug-in — partly done.** v0.5.0 separates the search from the
physics model: the model steps states and fires the triggers, the rules credit,
kill and score, and a search only decides which states to keep and which inputs
to try. Two more searches sit behind that interface beside the reachability DP —
Beam and Monte Carlo, experimental, picked in the play menu's Options — since one
search tuned for every level may not exist. What is left is the rest of the
interface, the parts the searches still read directly, and a way to measure a
search without running the game. The search is the part that needs no knowledge
of Geometry Dash's physics, so it is where contributions are most welcome.

**Platformer levels are outside the formulation, not merely unmeasured.** The
search rests on there being no steering input; a platformer level hands the player
one, so forward position stops being driven by the level and the decision each
tick stops being one bit. No measurement closes that — it is a different search
problem, and the section solver, which also chooses one bit per tick, cannot
search it either. So the mod refuses one instead of trying: it reads both the
layer's flag and the level's own, and takes the refusal if the two ever disagree.
Safety is unaffected either way — the record gate asks whether an automated session is open, never which
mode is running.

## Installing

Grab **[`gdsolver.solver.geode` from the latest release](https://github.com/gdsolver/gdsolver/releases/latest)**
and drop it into Geometry Dash's `geode/mods/` folder, then start the game — the
usual place is

```
C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods\
```

Geode itself has to be installed first ([geode-sdk.org](https://geode-sdk.org)).
The build is for **Windows and GD 2.209**, and for the executable of the Steam build it was made for:
Geometry Dash 2.209 exists as two executables with one version string (the first Steam build and a hotfix
that moved nearly every function), Geode checks the version string only, and a mod built for the one hooks
the middle of other functions in the other. So the mod checks the executable itself (the build stamps of
GeometryDash.exe, libcocos2d.dll and libExtensions.dll) before it hooks anything; on a game it was not
built for it stays off, writes one line to the log and shows one line on screen, and the game plays as if
it were not installed. Nothing else is needed — no Python, no external process, no second copy of the game.

**Android** (android64, GD 2.2081; not part of the 2.209 release): an Android build, if one is published, comes on the releases
page as a separate pre-release, as an experiment. It is not supported: the model's physics is Windows GD's, and
whether Android GD behaves the same has not been measured, so a result reached on
Android is Android's alone. [docs/ANDROID.md](docs/ANDROID.md) says how the port
differs.

The mod announces itself in the log the moment it loads, and says what it
suppresses:

```
gdsolver: loaded. While the bot drives -- solving or replaying -- achievements,
statistics, coins and the level's own record are all blocked. Your own attempts,
with the bot idle, record as usual.
```

Installing it does **not** cost you your progress: the block is tied to an
automated session being open, not to the mod being present. See
[Safety](#safety--community-notes).

## Building

Requirements: Visual Studio 2022 (C++ desktop workload), CMake ≥ 3.21, and for the
mod the [Geode SDK](https://geode-sdk.org) (`GEODE_SDK` environment variable) and
Geode CLI. The 2.209 build below sets up its own patched copy of the SDK; the
build for another game uses the SDK as it is.

```powershell
# solver CLI only (no Geode needed)
cmake -S . -B build-dp -DGDSOLVER_BUILD_MOD=OFF
cmake --build build-dp --config Release        # -> build-dp/dp/Release/leveldp.exe

# the mod for Geometry Dash 2.209 (what the release is): the Geode SDK with tools/geode-sdk-2209.patch,
# bindings that carry bindings/2.209, GEODE_COMP_GD_VERSION 22090 (the first 2.209 executable) or 22091 (the hotfix);
# the script does all of it from nothing and is what CI runs
# (in Windows PowerShell 5.1 do not redirect it with *>&1: CMake's stderr notices become errors and stop it; redirect from cmd, or use pwsh 7)
./scripts/ci-build-2209.ps1 -BindingsRepo <fork of geode-sdk/bindings with 2.209> -BindingsRef <ref>

# the mod for another game: the package takes that version into its own mod.json (mod.json itself names 2.209)
cmake -S . -B build -DGEODE_GD_VERSION=2.2081 -DGEODE_COMP_GD_VERSION=22081   # with the 2.2081 bindings
```

## Watching a run

A level's play button opens a menu offering three modes: **Normal**, **Replay** (play a stored
solution) and **Solve** (solve the level in-process), and a **Coins** switch: with it on, Solve
routes through every coin and files the result as `solution_lv<N>_coins.txt`, and Replay plays
that file and reports the coins the game counted.
A solve runs
fast, dark and silent until a candidate clears, and then plays that one through at 1x with the
artwork and the music. While a level is on screen an overlay reports what the loop is doing, and
F10 draws the iteration map: where the repair rounds went, and where the model — rather than
the plan — was wrong. While a solve runs with the screen dark, the replay panel (`Tab`) plays
GD's own record of the deepest attempt the game has verified, in the free area to the right.
Under Solve, Options holds Section search, Avoid glitches (Off, Glitches only, or Glitches + wave)
and the search itself, each with an (i) button saying what its values mean. Leaving a level ends
its solve.

The overlay has no key that lets a player do what the game does not allow: no warp, no noclip,
no forced mode, no infinite jump. It is for watching a run, not for changing one.

[docs/RUNNING.md](docs/RUNNING.md) has the play menu, the overlay, the full key table and the seek
bar; [docs/ITERMAP.md](docs/ITERMAP.md) has what the map's marks mean.

## Safety / community notes

This is a TAS and research tool, not a way to fake records.

* **While the mod is driving — solving or replaying — nothing is recorded**:
  achievements, statistics and coins are blocked, and so is the level's own
  record (completion percentage, normal/practice best, attempts, jumps, orbs,
  diamonds, the secret key, coin achievements). GD's own "do not record" path
  is used for the block — the flag it sets when a level is played from a start
  position — and the counters GD advances outside that path (the level's
  attempt and jump counts) are put back afterwards.
* **Playing it yourself records normally.** The suppression is tied to an
  automated session being open, not to the mod being installed, so with the mod
  loaded but idle your own attempts, progress and achievements count as usual.
* On GD 2.209 the same goes for what 2.209 added: the upload of where you die
  (the death tracker), the play time and the death sessions are blocked while the
  mod drives, and nothing it sends leaves the game then.
* Every session ends with the evidence in its log: how many recordings were
  blocked, and a `level record changed:` line that names any field that moved.
  It should read `none`.
* Replays driven by the mod are marked as bot input.
* The level-data dumps of the official levels that the development workflow
  uses are not part of this repository; the mod regenerates them from your own
  copy of the game.

## License

MIT — see [LICENSE](LICENSE).
