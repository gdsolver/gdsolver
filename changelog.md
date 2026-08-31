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
