# Level slice

A heavy level is solved on a copy of itself that keeps only the objects the
player's run can depend on, and the plan that clears the copy is verified on
the level itself before anything is filed. The copy is one more approximation
in front of the game, like the model: a wrong cut costs rounds, never a wrong
answer, because a solution is only ever the plan the original level cleared.

Code: `src/solver/slice.hpp` (the rule and the cut string) and
`src/mod/level_slice.hpp` (the session: when to slice, the swaps, the
verification and the add-back).

## Why

What a solve costs grows with the level's object count: the game's own update
(moving decorations along with the gameplay they share nothing with), the
recording of the moving geometry, the input the model is built from, and the
section search's restore of the whole level. On a heavy custom level most of
those objects are decorations nothing in the run reads.

## Which objects are kept

Every area trigger, keyframe point and every object the game does not class as
a decoration or a trigger is kept, and so is every trigger except those that
can only act on what the run cannot depend on (below). A decoration is kept
only when something reads its group as a **position**:

- a trigger's centre group or its move-to-target reference, and for a
  move-to-target or dynamic move the moved group itself (its reference point can
  be any member) — counted only for a trigger acting on a relevant group;
- an area trigger's target group (the effect is timed off the group as a whole);
- a teleport's destination;
- a camera trigger's target (edge, static, guide), read from the level string as
  well, since the static camera keeps its centre group in a key of its own; in
  free mode the camera sets the flight band;
- the level's spawn group (the player appears at its object's height).

A decoration is also kept when it is a group parent or when it sets the level's
extent. Sharing a group with a gameplay object is not a reason to keep it: a
Move, Rotate, Scale, Alpha, Toggle or Area effect applies to each member on its
own. "Relevant" groups start as every group a trigger or a gameplay object is in
and grow, to a fixed point, by the positions read by triggers acting on them.

A trigger is dropped when it can only act on what the run cannot depend on
(cfg `slicetriggers`): a mover — Move, Rotate, Follow, Follow Y, Scale, Alpha,
Pulse, Toggle, Animate, Stop — whose target group holds nothing the run reads,
and a cosmetic trigger that addresses no group (colour, gradient, background,
song, sound, shader). What the run reads starts as the groups of every object
that is neither a decoration nor a trigger, every trigger's centre,
move-to-target reference and spawn remap, every target that is not a mover's,
the level's spawn group and the camera triggers' targets (a Move on a static
camera's target moves the flight band); a kept trigger's own groups and target
join it, to a fixed point, since Spawn, Toggle and Stop address triggers by
group. A trigger in a group that an ordered Spawn fires is kept whatever it
does: such a spawn times its members off their x, so removing one moves the
others (on lv22, dropping one Pulse moved a zoom trigger of the same group by a
tick, and the flight band by up to 22 px).

Types, groups and trigger fields are read from the objects the game built when
it loaded the level; the cut is made in the level string by index, so every kept
object keeps its bytes and its order. An id the loaded level has no object for,
or built with more than one type, is kept — except an id built as a decoration
and as exactly one other type, which is decided per object by No Touch (key
121): the objects carrying it are the decorations, and those can be cut (cfg
`slicenotouch`). That holds only when the level string's count of such objects
equals the game's count of the id's decorations; otherwise the id is kept.

An object the cut leaves out is not removed but replaced, in its place, by a
placeholder that does nothing and takes the same uids (cfg `sliceuids`). The game
numbers a level's objects in string order as it builds them, and its play depends
on those numbers: on a heavy custom level with an area move, removing any one
object before the area trigger moved the player's landing on the platform it
moves. With the placeholders, every object of the copy has the level's uid, and
the session checks that it does (`slice: uids ...`).

A placeholder also keeps its object's groups and the groups it is the parent of
(cfg `slicekeepgroups`). The game reads a group's members, not just the objects
that can touch the player. It sends the only member of a moved group to a
static group (a group of one member). A Move in target mode, a dynamic rotate
and follow speed pick a random member by the group's count and order. The
group's parent comes before both. Without the groups, a spike that shared its
group with three decorations was alone in its group in the copy. The game then
moved and bucketed it differently, and a plan lived past it in the copy and
died on it in the level. The placeholders now move with their groups, which
costs time on levels with many moved decorations (a section search took 30%
longer on one).

Before the port, the rule was checked on the level itself against the cut copy:
the player's state agreed on every tick of known solutions of the four heaviest
official levels and along a 24,733-tick route of a heavy custom level, and a
plan solved on the cut copy of lv22 cleared the level itself. Each fix to
the rule came from a column that disagreed there (the spawn group, the camera
targets, the area-trigger targets). The trigger cut was checked the same way on
known solutions of lv19–22 and a heavy custom route: every column of the dump
matched the uncut level except the camera shake (which two runs of the level
itself do not share), the trigger queue cursor and the snapped object's uid.

## When a level is sliced

When the cut removes at least **10,000** objects (`slicemin`). A run that takes
the level's coins is sliced too; the copy has them (below).

The number is where the copy pays for itself. What an object costs per round
of the loop was measured on a heavy custom level solved twice from the same
build, once on the level (109,663 objects) and once on its cut (4,827). The two
runs made identical decisions for 44 rounds (their `[fp]` lines equal line for
line), so they did the same work and only its price differs:

| | the level | the cut |
|---|---:|---:|
| 39 solver calls | 175 s | 75 s |
| 221,738 attempt ticks | 17.1 s | 10.2 s |

That is about 24 µs per removed object per round: the level the model reads is
built from every object, and the game moves every object a trigger moves. The
section search's restores, which also follow the object count, come on top and
are not counted. What the copy costs is paid once: the cut (tens of
milliseconds; 162 ms on the level above), a second load of the level, and one
flight of the plan on it. On lv22 everything after the last solve returned
— the copy's clearing flight, the swap back, the flight on the level and the
session's end — took under 10 s. A cut of 10,000 objects saves about 0.24 s a
round and has repaid that within some twenty rounds; a solve shorter than that
is a short solve, and the most it loses is those few seconds.

Where the official and spin-off levels stand (the cut's census, `slicecount`),
counted before the trigger and No Touch cuts were on — with them, more is
removable:

| levels | objects | removable | sliced |
|---|---:|---:|---|
| lv1–18 | 1,444–14,400 | 121–9,630 | no |
| lv19 | 16,131 | 11,571 | yes |
| lv20 | 19,494 | 13,179 | yes |
| lv21 | 27,283 | 22,506 | yes |
| lv22 | 18,329 | 11,788 | yes |
| Meltdown 1001, 1002 | 9,694 / 10,746 | 7,107 / 7,766 | no |
| Meltdown 1003 | 15,477 | 11,585 | yes |
| World 2001–2010, 3001 | 839–3,876 | 243–2,747 | no |
| SubZero 4001, 4002, 4003 | 20,576 / 18,317 / 15,269 | 15,649 / 11,915 / 11,616 | yes |

### Coins

The copy is an editor-type level, and the game builds a secret coin (id 142)
only in a main level. `PlayLayer::processCreateObjectsFromSetup` makes each
object of the level string and adds it to the layer unless the object is of the
secret-coin type and the level is not a main one. So a plain copy of an official
level has none of its coins: lv22 with coins on cleared one 103 times, each with
0 of 3 coins by the game's own count, where the level itself credits all three.

In a run that takes coins, a copy therefore builds its objects under the type of
the level it was cut from, and only while it builds them: the type goes back
before `createObjectsFromSetupFinished`, which the last batch of objects calls,
so the finish, `setupHasCompleted` and the play itself see the editor-type level
they always saw. The game's check of a main level's string is in
`PlayLayer::init`, before any object is made, and the copy passes it as an
editor-type level. The one other read of the type inside that window is for the
coin itself — whether the player already holds it — and it gets the answer the
level itself gets. A user coin (id 1329) is built in either type, and the
calibration rigs, which are editor-type levels, credit it. The session reports
what the copy has (`slice: the copy has N of the level's N coin(s)`).

A run that does not take coins builds the copy as before, without them.

Nothing a coin earns is recorded from the copy: the game stores the coins
taken, the completion and the coin achievement at the end of a level only when
the level is not an editor-type one, and the copy plays as one. Those stores are
blocked while the bot drives in any case (see the safety section of the README).

## The flow

1. The session's first solve cuts the level. Below the threshold nothing
   changes. Above it, the level is swapped for an editor-type copy with the
   original's id, song and version (the game turns a modified main level away
   as soon as it starts), and the solve starts there as on any level. The copy
   lives in memory only: it is never added to the player's created levels. The swap
   waits for any scene transition to finish: a level entered with a fade starts
   its first solve while the fade still runs, and the fade's end would put the
   original's scene back over the copy.
2. The plan that clears the copy is not filed. The level itself is swapped back
   in and the plan is flown on it. The copy's clear is not held to the
   "short of the goal" gate either, because the copy's progress percentage is
   not the level's (lv22's copy read 90.73% where the level read 96.44% at
   the same x and tick); the gate judges the flight on the level.
3. A clear there is the ordinary clear of a solve: filed and shown under the
   level's own id. The flight is not recorded: nothing reads the recording of a
   flight that clears, and writing it was most of that flight's time on a heavy
   level. A flight that dies this way is flown again, recorded, from a fresh
   swap before the death is read (step 4), so the solve that follows starts from
   what it always started from.
4. A death there means the cut was wrong somewhere. The attempt is compared tick
   by tick with the copy's clearing attempt — the player's position, vertical
   velocity and mode; not the flight band, which the game carries from one
   attempt to the next, so the first attempt on a layer just swapped in starts
   with another band whatever the cut — and the objects dropped within 1,500 px
   of where the two first parted go back into the copy, and the solve goes on
   there from the same plan. After `sliceaddbacks` add-backs, or when the
   parting did not move further on, the solve goes on on the level itself.
5. A copy that is wrong only at a wall would never clear to be verified. So when
   the copy has gone eight rounds without getting deeper, its deepest plan is
   flown on the level itself, once per wall, and compared with the copy's
   attempt of the same plan. A death on the same tick, the same way, means the
   wall is the level's own, and the solve goes back to the copy where it was:
   the check is a flight, not a new run, and the copy is rebuilt from the same
   string, so its objects keep their uids and the loop's recordings, anchors and
   plans stay valid. Otherwise the level's flight is not yet held against the
   cut: the copy's attempt it was compared with is the copy's deepest, which can
   be its hundredth, and what the game carries from one attempt to the next (the
   flight band) can move the player there. So the plan is flown once more on a
   fresh copy, as its first attempt, and the two first attempts are compared
   (cfg `slicefirstref`): if they agree and die on the same tick, the wall is the
   level's own and nothing goes back; where they part, or where the level gets
   past the copy's death, the objects around it go back as in step 4.
6. A solve that gives up on the copy does not end there: giving up is the copy's
   verdict, not the level's. It goes on on the level itself from the copy's
   deepest plan — unless the wall it gave up at is one step 5 found to be the
   level's own. The level would then only be the same wall at the level's price,
   so the solve goes on on a fresh copy instead, with the recordings, anchors and
   seeds taken again as a move to the level would, once per wall (cfg
   `slicegiveupcopy`).

## What carries across a swap

A swap is one run, not a new one: choosing a level, solving it and playing the
solution back are one frame, and a plan is not thrown away because some objects
came back. The plan, the fixups (keyed by the player's state, not by any
object), the phantom vetoes, the coin margin and the round count carry over.
What is keyed by an object's uid does not — the recordings of the moving
geometry, the anchor rows, the pads and rings an attempt spent — even though the
copy now numbers its objects as the level does (`sliceuids`): they are recorded
again on the new level.
The deepest plan is not carried as the deepest: its depth was measured on the
level the run left, so the carried plan is flown first and measured again.

The level record the session reports on (`level record changed:`) is always the
one of the level the session was started on.

## Configuration

| key | default | meaning |
|---|---|---|
| `slice` | 1 | 0 = never slice |
| `slicemin` | 10000 | objects the cut must remove for the copy to be used |
| `sliceaddbacks` | 2 | add-backs before the solve moves to the level itself |
| `slicetriggers` | 1 | drop the triggers that can only act on what the run cannot depend on |
| `sliceuids` | 1 | replace a left-out object by a placeholder with the same uids, so the copy numbers its objects as the level does |
| `slicekeepgroups` | 1 | the placeholders keep their objects' groups and parent groups, so the copy's groups are the level's |
| `slicenotouch` | 1 | decide an id built as a decoration and one other type per object, by No Touch |
| `slicefirstref` | 1 | a wall check that does not end like the copy compares two first attempts (step 5) |
| `slicegiveupcopy` | 1 | giving up at a wall the level shares goes on on a fresh copy, once per wall (step 6) |
| `slicecount` | 0 | 1 = print the cut's census line and end the session (no solve) |
| `slicenoposition` | 0 | 1 = drop the decorations read as positions too: a cut that is wrong on purpose, to exercise the verification and the add-back |
| `sliceverifyrec` | 0 | 1 = the check flights on the level record the moving geometry as every flight does (step 3) |
| `sliceverifybreak` | 0 | n > 0 = the plan handed to the level keeps its first n inputs: a verification that fails on purpose |
| `stagecuts` | (off) | `x1,x2,...`: solve in pieces -- see "Stages" below |
| `stagecutcheck` | 2 | a cut that splits a group a kept trigger reads as a position: 0 = nothing, 1 = a log line, 2 = moved to the nearest x that does not (or dropped) |
| `stagecutwindow` | 3000 | px a cut may move under `stagecutcheck=2` |
| `stagebreak` | 0 | n > 0 = the plan handed from piece to piece keeps its first n inputs: a hand-off that parts on purpose (test) |

## Stages (`stagecuts`)

A development aid for long levels. The level (the slice's copy of it, when the
slice applies; otherwise the level itself) is cut at each x -- everything past
the cut is gone, as `py/cutlevel.py` cuts it -- and solved piece by piece in one
game: the plan that clears a piece is flown first on the next, longer one, and
the last piece is the copy (verified on the level, as ever) or the level itself.
The swap and what carries are the slice's own (see "What carries across a swap");
a piece's clear is handed on, not verified or filed.

The plan one piece hands the next is a seed from the same run, so a solve this
way is not reported as a cold solve. Every piece is solved cold; only the last
starts from the plan in hand. Measured on a long custom level (220,116 objects, slice on,
one game, load from other sessions): cut at x=37,300 476 s; cut at 20,000 and
37,300 392 s; the hand-off between pieces takes about 4 s. A cut inside a cluster
of portals can hand on a lane that dead-ends past it (a long custom level at x=45,400);
the loop then walks back on the longer piece as after any wall. The look-ahead
after a hand-off is the cfg's `dpstephorizon` (6500 in these runs).

### Where a cut may go (`stagecutcheck`)

A piece is the level with everything past the cut gone. Most of a level does not care; a trigger that
reads a group **as a position** does (the reads listed under "Which objects are kept": a Rotate or Scale
about a centre group, a Move to a target group, a Follow, an Area effect). Cut so that the trigger is still
there but the group it reads is partly or wholly gone, and the piece behaves differently from the same
stretch of the level. For one such trigger (at `x_T`, acting on `Gt`, reading `Gp`) a cut `c` is unsafe when
`c >= x_T`, `c <` the largest x of `Gp`'s members, and `c >=` the smallest x of `Gt`'s and `Gp`'s members.
A rotate-gameplay or options trigger (2900, 2899) makes 400 px either side unsafe: the player's x stops
there and a cut has no meaning.

At the start of the stages every cut is checked against this (`stagecutcheck`, default 2; `stagecutwindow`,
default 3000 px): 0 = not checked, 1 = a log line, 2 = a cut that splits a reader moves to the nearest x
that does not (or is dropped when none is within the window). `py/cutsafety.py LEVEL.lvl` is the same rule
on a level file: the unsafe stretches and the gaps between them (`--check`, `--suggest`). It is a
predictor from the string; the check below is the truth.

On a level with rotating geometry in a part of it (12 readers, 4.3% of x unsafe) the
cuts go in the gaps; on one with rotate-gameplay triggers throughout the unsafe share is large (another
level: 23 of them, 44%), and a cut there can be moved a long way or dropped. The predictor reads every mover, and
a decoration group that rotates about its own centre is no hazard to the run: of the cuts it called unsafe on one
level, the check at the hand-off found the one tried consistent. So the predictor errs on the cautious side; the
check is what says whether a cut held.

### The check at a hand-off

The clearing flight of a piece is kept (the player's x, y, vy and mode on every tick), and the first flight
on the next piece -- the same plan -- is compared with it as it ends. Tick for tick they should be the same up
to the cut. The log says, per hand-off:

- `consistent`: the two flights match on every tick to the end of the shorter.
- `parted at the edge of the piece`: they match until the last 150 ticks of the piece (the end of the level, the
  camera and the bands settle there; the next piece's first flight dies a little before the clear as the
  longer piece has objects the shorter did not), or the next flight matches and dies in those ticks.
- `DISAGREE`: they part, or the next piece kills, earlier. Something past the cut reached back: a group read as
  a position, a trigger the dropped part fired. The tick and x are in the line.

`summary()` ends with the three counts. A DISAGREE costs rounds, never a wrong answer: the last piece is the
level, and its clear is verified.

## What the log says

`slice:` lines: the cut (objects, how many the run cannot depend on, per rule),
the copy's size, the copy's clear, the verification's outcome with where the
level parted from the copy, each wall check and add-back, and at the session's
end where it finished (`slice: ended ...`, with the counts of verifications,
wall checks, add-backs and first-attempt flights on a fresh copy).

## Limits

- A wall is checked on the level itself only once the copy has stalled at it
  for eight rounds, so a wrong cut costs those rounds before it is found.
- An object whose id the game rewrites when it loads the level has no loaded
  object of the same id to be classed by, and is kept.
- The add-back puts back a stretch of level around where the runs parted. A cut
  that is wrong in a way that is not local fails that way every time, and the
  solve then moves to the level itself. (The one such case measured — an effect
  that depended on the objects' numbering, which removing any object shifted — is
  what the placeholders of `sliceuids` are for.)

