# v0.1.5

 * **Each repair costs less.** The whole suite in one game session went from 30
   minutes to 19, and Dash from twelve minutes to under four. Two changes to the
   loop, both on by default and both switchable off for a diagnostic run:
   * *The plan length follows the game's last verdict.* While replays keep dying
     within 3,000 ticks of their anchor the loop plans one such step at a time and
     lets the game check it; once a plan outlives its step, or a wall stops the run
     twice, it plans the rest of the level (`dpstephorizon`, `dpadaptivehorizon`).
     A round that flies exactly the previous round's plan and dies on the same
     tick closes that spot at once instead of on the fourth try: the search is
     deterministic, so the repeat is not new evidence (`dpfastveto`).
   * *A repair rejoins the plan that died.* The next search is handed the model's
     trace of that plan, and when a state that did not go through the death comes
     back onto it, the search stops there and the old plan's inputs are kept from
     that point on (`dprejoinwatch`, `dprejoinuse`, `dprejoinchain`). The game
     still flies all of it. On Hexagon Force and Dash, 37-45 % of the search's
     layers had been re-deriving what came after such a point. Its one measured loss is
     Deadlocked, where the old plan's continuation died a few dozen ticks past the
     join, in ground the game had never flown.
 * **Checkpoint flights** (cfg `dpcheck`, off): while a search runs, the game can
   fly the search's checkpoints and cancel it on a death, deterministically. Two
   faults on levels whose first search reaches the goal are fixed -- a completion
   left over from a held flight read as a death at tick 0, and a flight that
   cleared the level left the next level's start waiting forever. It stays off:
   with the new plan length it made the suite slower, not faster.
 * A solved branch that is being followed gets its grace per wall, not per round:
   two tails alternating between a shallow and a deep death reset it forever.
 * The search bar's denominator is the layer the search will actually stop at.
 * Workers run a frozen copy of the game build, checked at every launch, so a
   Steam update cannot change the game under them unnoticed.
 * The iteration map draws every round's tail to its end, the clearing round
   included.
# v0.1.4

 * **Slopes are acquired the way the game acquires them.** There was never a
   contact point: the game probes a pixel either side, tests the rect one pixel
   in, and seats the body on the line extended past the ramp's end. The model now
   does the same, and the ramp kills follow their measured outlines -- a spiked
   ramp kills at a perpendicular distance from the line, not by box overlap, for
   the ship and for a flipped wave as well as the upright wave.
 * **A spawned trigger acts on the group its remap names.** A Spawn can send the
   moves it starts to a different group, and the model used to move the group the
   triggers were written for. On Dash that left a sinking platform under no
   trigger at all, playing to the clock of whichever run recorded it.
 * **A touched platform moves when this run touches it**, not when the run that
   recorded it did -- except where something else also moves it.
 * **A flying band ends where its recording ends.** The band the camera held
   was kept for every tick after the last one recorded, so a mode portal further
   on could not change it: the solver planned against a floor or a ceiling the
   game no longer had, and the plan died in the replay.
 * A swing re-anchored on the tick its press takes effect keeps the flip that
   press started, and a robot re-anchored in mid-air no longer gets a full hover
   it does not have.
 * Every one of these was measured first (calibration levels in `data/rigs`, the
   game's own traces, and the disassembly) and each can still be turned off:
   `--no-slopelaw`, `--no-spawnremap`, `--no-touchretime`, `--no-shipslopekill`,
   `--no-waveflipkill`, and `dpswingpending=0`, `dphoverstrict=0`, `dpbandend=0`.
 * The death counter counts deaths rather than calls to the function that kills,
   and every death names the object that killed and which body died.

# v0.1.3

 * **Solving a second level without restarting the game works.** One level's
   recording of its moving geometry was still being offered to the next one, so
   a level that has no moving parts of its own was planned in the previous
   level's world -- solve Stereo Madness after Clubstep and the search died a
   fifth of the way in, on a level it clears in eight rounds. The recorder now
   publishes "nothing was recorded" as an answer instead of leaving the previous
   answer standing, and a solve deletes the file it would have read.
 * **The music follows the speed again after the screen comes back.** Raising the
   spectating notch and then turning rendering back on left the song playing at
   1x for the rest of the session: resuming rendering restarts the level, the
   game starts the song again unpaused, and the mod had latched on what it last
   asked for rather than on what the song was actually doing.
 * A platformer level is refused rather than answered. It is outside the
   formulation the search rests on -- there is steering, so "the input at tick t"
   does not describe the run -- and it used to be solved anyway, badly and
   silently.
 * The force block is modelled from its own settings rather than from a table of
   the object ids that happened to be measured, and its push is read off the real
   object. Calibration levels for it are in `data/rigs`, one per game mode.
 * The cold regression can solve every level in one game (`--one-session`), and
   blessing a baseline now does. Under one launch per level nothing about a
   second level is ever exercised, which is why the leak above went unseen.

# v0.1.2

 * **The keys can be rebound.** All thirteen are Geode keybind settings now,
   with the keys they already had as defaults, and the on-screen legend reads
   the bindings rather than naming keys it can no longer be sure of. They were
   read straight from the OS before, which meant hardcoded keys and a keystroke
   meant for another window could reach the mod.
 * **A bad number in a config or command file no longer takes the game down.**
   Every setting was parsed with a function that raises on malformed input and
   nothing caught it, so one bad value killed the game at launch or mid-frame.
   A value that will not parse is now left alone and reported.
 * The iteration map keeps the paths each round flew, and is filed however the
   session ended -- leaving a level mid-solve used to discard the whole record.
 * The collision trace can see the second body in a dual, which is the half
   every dual question is actually about.
 * The panel rides Geode's own overlay node instead of following scene changes
   by hand.
 * Releases are built by CI from the public source rather than on a desk.

# v0.1.1

 * **The dual's second body.** The loop was walled on Deadlocked where the game
   kills the second body inside a corridor a mode portal opens. Five holes fed
   it, each measured against the game rather than reasoned from the level: the
   body was born at its resting position instead of the pre-collision one,
   portals fired against the same wrong y, a mode portal did not end a ceiling
   press the way it already ended a slope ride, a ceiling release fired a tick
   early, and the two halves shared one gamemode and one ceiling-press counter
   although each is tested at its own height.
 * **The iteration map (`F10`).** Draws what the rounds cost and where: each
   round's death, the fixups, the vetoes, the re-anchors, and the path each
   round flew. It is there to tell a wall the model gets wrong from a wall with
   nothing through it.
 * **A seek bar**, always on in a replay — click, drag, or arrow keys, stepping
   frames while the game is stopped.
 * **Which plan comes out.** Among the states that reach the end, the one
   emitted is now the one whose route kept the most vertical room, rather than
   whichever was enumerated first.
 * A moving circular hazard takes the same collision branch as a static one.
 * All 22 official levels still solve cold, and the suite now needs **235
   repair rounds instead of 322**. Largest moves: Hexagon Force 102 to 55,
   Blast Processing 5 to 1, Clutterfunk 3 to 1.

# v0.1.0

 * Initial release. All 22 official levels solved cold.
