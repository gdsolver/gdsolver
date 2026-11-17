# Human takeover

A debugging instrument. When the repair loop or a section search keeps failing at the same place,
a person can often see a way through that the search does not take. A takeover session lets them
show it: a plan is flown to a chosen point, and from there the jump button is the person's. What
they play is then flown again in GD and in the model, so the place where the two part on that route
can be measured.

It is off unless a session asks for it, and with it off nothing changes.

## What it is not

- **It is never a seed.** Nothing a takeover writes goes where a solve reads: no solution file, no
  resume, no fixups. A person's route is not evidence that the solver can find one, and a level is
  never reported as solved because of one.
- **It is still a bot session.** The session stays open the whole time, so achievements,
  statistics, coins and the level's own record stay blocked, exactly as in a replay; the badge
  says `HUMAN INPUT` and the session ends with `level record changed: none`.

## Running one

```bash
python py/takeover.py play --level 16 --plan data/solution_lv16_dp.txt --at 5200 --worker-id 101
```

The worker is always named (`--worker-id`): which workers are free is the operator's arrangement.

- `--at <tick>` or `--at-x <x>` is the point. The plan is flown there quickly with the picture on,
  the last two seconds at normal speed so you see what is coming, and then the game stops for a
  moment (`--wait`, one second by default) and the input is yours.
- At the point the mod puts the same strict checkpoint the loop's checkpoint flights use
  (`src/mod/cp_flight.hpp` describes what it restores beyond the game's own checkpoint), and
  switches practice mode on. Entering practice mode keeps the run where it is.
- **The game's checkpoints are yours from there, and every one is strict.** The game places them
  the way it always does — its own auto-checkpoint setting, `Z`, a checkpoint object in the level —
  and each gets a strict checkpoint taken where the physics stands. One the game stores at once
  (`Z`, a level checkpoint) is taken at the next update boundary. An auto checkpoint is taken when
  the game *creates* it — the game creates it where it judges you safe and stores it only once you
  have outlived it — and is kept only if the game stores its own. `X` removes the newest, but never
  the one at the start point. When the game takes back a checkpoint it has just placed (it does if
  you are hit right after an auto checkpoint, so as not to respawn you into the hit), the strict
  one goes with it.
- Every jump press is recorded as player 1's, whichever key, button or click it came from: outside
  a two-player level the game sends them all to player 1. In a two-player level the second
  player's presses cannot be carried by a plan; they are counted in the summary.
- Pausing works as in ordinary play while you have the input — the pause menu, or the game's own
  pause when the window loses focus.
- **Every death goes back to the newest strict checkpoint**, never to the start of the level,
  followed by the same short stop. The game's own checkpoint list is emptied at each restore, so
  the game never loads one of its own; its markers stay on screen until then, and the overlay
  counts the strict ones.
- If the button was held where a checkpoint was taken, a release is added there for the attempts
  that start from it: you are not holding the key after a restore, and a hold nobody releases
  would carry into your run.
- **`F8`** (the setting *End the human takeover*, rebindable) ends the session; so does a clear.
  The worker closes, and `play` collects the records and runs `check`. A driver with no keyboard on
  the worker writes `takeoverend` to the data root's `cmd.txt`, which does what the key does.
- `--slowmo N` plays at 1/N speed. It only divides the frame time: the physics steps and the tick
  numbers are unchanged, so a run played slowly replays as it is at full speed.
- Once the input is yours, the seek, speed and replay keys do nothing (up is also the game's jump).
  Before that, in a session with no fixed point (below), the plan plays as an ordinary replay and
  they work.

A custom level is played from its level file: `--level 9001 --levelfile <file>`.

### Taking over wherever the replay stands

```bash
python py/takeover.py play --level 16 --plan data/solution_lv16_dp.txt --worker-id 101
```

With neither `--at` nor `--at-x` (the session's `takeoverkey=1`, off by default) there is no point
fixed ahead. The plan plays as an ordinary replay at the speed you choose (up and down arrows; left
and right seek, `F7` restarts it from the start), and the first press of `F8` takes the input over
where it stands: the strict checkpoint is put at the next update boundary and everything above
applies from there. From then on `F8` ends the session as it does after a fixed point, and the seek,
speed and replay keys are off. A driver with no keyboard writes `takeovernow` to `cmd.txt` for the
first press. A death of the plan before the key is pressed is the replay's own death; it does not
end the session.

## What it writes

Under the run directory (`--out`; by default the untracked `data/takeover/`, a folder nothing
that reads solutions looks in — a run with many attempts writes a dump per attempt, so a roomier
disk is worth naming):

| file | |
|---|---|
| `takeover/base.plan.txt` | what every attempt from the start point flies before the person |
| `takeover/att_NNN.plan.txt` | each attempt, as a whole plan from tick 0 — the inputs up to the checkpoint it started from, then the person's — in the format every replay tool reads |
| `takeover/summary.txt` | one line per event: the start, each checkpoint (its tick, position, the attempt that took it and the checkpoint that attempt started from), each removal, each restore (and whether it landed exactly where the checkpoint was taken), each attempt (outcome, tick, checkpoint, presses), the end |
| `dump.csv`, `result.txt` | the session's own per-tick state and log |
| `run.json` | the level, the plan and its hash, the point, the mod package and its hash, the commit |
| `check.json` | what `check` measured |

A press is recorded at the tick the game applies it, which is the step an injected press of the
same plan is fed at. That is the claim `check` tests rather than assumes.

## Checking a run

```bash
python py/takeover.py check <run directory> --worker-id 101
```

For every attempt, with nobody at the keyboard:

- **replay** — the attempt's plan is flown again from the start in a headless worker and compared
  with the takeover session's own dump, tick by tick, on both players' physics columns, up to the
  earlier of the two ends. Identical means the recording carries the route and the checkpoint
  restores held. A difference names the first tick and column where they part. The camera, the
  flight band and the frame counters are counted separately and do not decide: they carry noise of
  their own, and a person's realtime frames are batched differently from a headless replay's.
- **model** — the same plan is replayed by the model and compared with the headless replay's
  dump, the comparison `py/fidelity_diff.py` uses. It reports the first tick where the model
  leaves the game, and whether that is before the point (the plan's part) or after it (the
  person's). The moving geometry along the route is recorded during the headless replay, so a
  level with moving objects is compared against its own recording.

One worker, one session at a time: the replays run on the same worker after the person's session
has closed.
