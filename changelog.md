# v0.7.0

 * **Geometry Dash 2.2091 (Windows)**, as v0.6.0. No Android build.
 * **All 22 main levels clear on 2.2091**, cold, in one game session: in 17
   repairs for the end of the level (v0.6.0: 27) and in 27 with every coin (v0.6.0:
   33), in 8 and 10.5 minutes in all; the 17 spin-off levels clear too, every coin
   included (docs/RESULTS.md). Of the 126 Daily, Weekly and Event levels of the
   v0.6.0 survey, 77 clear in 20 minutes (v0.6.0: 74; docs/CUSTOM_LEVELS.md).

### The model: on by default

 * **Several dozen model fixes are on by default**, each found where the game's
   replay and the model's walk of one plan came apart, and measured by putting the
   player into the game at that state. With them the official 22 clear in one
   session with the level record unchanged, each saved solution clears again from
   the start in a fresh game, and long custom levels that stopped at a wall (a
   long wave level, a long ship level, a long two-body level) clear. By family:
   * **Triggers:** a Player Control trigger's Stop Jump releases the held button;
     a Reverse trigger turns the player round; a teleport trigger fires once; a
     Move or Toggle on a trigger channel waits for its channel, and in a rotated
     gameplay frame is crossed along the channel's axis; an Advanced Follow along x
     (player 1) moves what it targets; several Moves locking one object to the
     player, several Taps feeding one Count, an Area Move on top of Moves and one
     centred on the screen are all taken.
   * **Ramps:** the launch off a ramp's end (on the underside too, and under a
     Time Warp), a jump on the landing tick, a seat that carries the player past a
     block, a ball on a ceiling ramp, a robot on a ramp, several ramps met in one
     tick, and a rider on a solid that is being scaled.
   * **The wave:** its kill margins at plain ramps, peaks and ceiling ramp ends;
     dart-slide blocks; a wave portal taken on a ramp; turned portals after a size
     portal; the band's ceiling under a camera zoom.
   * **Orbs, pads, rings, portals and dashes:** a ring on the tick of a speed
     portal, a ring pressed while riding or by both bodies of a dual, spider orbs,
     long turned pads, turned mode portals, more remembered portals, the dash
     release angle of the cube and the ball, the force-box push of a ship or UFO,
     the robot's held button and hover, a reverse ring's correction of x on a level
     with Reverse Sync on.
   * **Moving parts:** recorded rows read a tick late or cut on long levels, an
     anchor that missed a Count or a Tap, and parts that move on another clock than
     the recording: long levels with such parts solve against where the game puts
     them.
 * The fixes turned on in the last days before this release keep a `--no-<name>`
   off arm; the older ones are now always on (see Removed).
 * **The search dates moving parts by each state's own path** (`--statecross`,
   `--crossbehind`), places objects only where they can change (`--incanchor`),
   probes each rung of the repair ladder first (`--rlbound 1500`), and at a wall
   that has killed the plan three times keeps every class's extremes and asks
   whether a portal was missed (`dpcapenvrepeat`, `dpmissedportalrepeat`).

### Features

 * **Two-player levels (experimental).** The second player's button is planned: each
   body is searched against the other's plan and the two are merged into one plan
   whose input mask carries both buttons (bit 1 is player 2's), which the game
   replays with its own call for the second player. The model gives the second
   body its own button, its own gravity and the birth the game gives it at a dual
   portal. A level where one player's press is a switch that moves the other's
   world is not solved yet (`dpswitch`, off, is the attempt), and the section
   solver is never used on a two-player level. The play menu names such a level in
   yellow, not red. See docs/TWO_PLAYER.md.
 * **The replay panel shows the model's plan.** Past the verified path the panel
   plays, in blue, how the model believes the plan it has just found flies, from
   the search's anchor on; the last eight plans are drawn beside it as faint
   ghosts, each with a figure of the player in its mode. The level bar has a
   second fill to where the plan reaches ("level 43.0% (plan 49.5%)"). A click or
   a drag on the panel's bar moves its playhead, and the panel keeps moving
   through a flight and a section search instead of standing still. See
   docs/REPLAY_PANEL.md.
 * **The Search option says what answers.** With Beam or Monte Carlo the log says
   once per solve which search answers the calls. Monte Carlo cannot follow a coin
   route, so with Coins on the DP answers every call and a warning line says so;
   the option's (i) text names the calls that fall back.
 * **Fixed replay step.** Replay's Options has a box that advances a replay in
   fixed steps of 1/60 s, so the motion no longer depends on the length of the
   frames. Off by default.
 * **For developers:** `py/cold_regress.py --fresh-verify` (and
   `--fresh-verify-one-session`) plays each saved solution once from the start in
   a fresh game; cfg `stagecuts` solves a long level in pieces in one game
   (docs/LEVEL_SLICE.md); cfg `nodeath` can pick one body. New experimental
   switches, all off: `--crosscx`, `--rlspread0`, `--followtouch`, `--obbfloat`,
   `--modeswitchkeep`, `--p2switch`, `--p2tappress`, `--pinrows`, `--p2secx`,
   `--p2taps`, cfg `dpbirthrung`.

### Fixed

 * **A solution found in the fast loop could die when played at normal speed.**
   The fast loop gave the game's animations unscaled time under a Time Warp, and
   the scheduler the screen's time instead of its own step (cfg `vtscale`,
   `vtime`). The level dumps carry nine significant digits, not six.
 * **A plan could lose a press at a re-anchor.** A solver's tail was read without
   the buttons its anchor held, so a solution that cleared in the loop could fail
   from the start.
 * **The game could crash on leaving some levels**, two-player ones above all: a
   portal's shine effect outlived its level (cfg `stopfollowers`, on).
 * **The play menu's notes read levels made before 2.0** (stored in another
   compressed form, they read as "none"), and "some follow triggers" is named only
   when the group they move holds something the player meets.
 * **F6 shows the hitboxes on GD 2.209 again**, outside practice mode too, and
   **the song stays with the picture** after a seek in a level with a Song Offset.
 * **A solve started from the play menu replays faster.** With the screen off it
   still walked every object's visibility on each step of a replay; it now skips
   that until the screen is turned on (F5). A long level replays its candidates
   about six times faster (a 189,000-object level: 134 s to 21 s of replay).
 * **The search's small steps use the whole thread pool**: about a fifth faster on
   a heavy call, with the same plan.

### Removed

 * **53 solver switches are gone**, the ones that only ever turned off a rule that
   is now always on. A removed switch is refused by name, with the commit that
   fixed its behaviour. The list is dp/removed_flags.txt.

# v0.6.1

 * **The Play menu opens for every online or downloaded level on GD 2.2091** (Geode's name for the hotfix executable of 2.209, the one Steam
   has now). A click on Play could start the level without the mod's menu: the check that decides whether the level page has finished loading
   read a member of the page at the position it has in the first 2.209 executable, and 2.2091 has one more member before it. The mod now reads
   the member where each executable keeps it. The solver is the same as in v0.6.0.

# v0.6.0

 * **Geometry Dash 2.2091 (Windows).** This version is built for GD 2.2091 on Steam,
   Geode's name for the hotfix of 2.209. The first 2.209 executable and the hotfix
   carry one version string, the hotfix moved nearly every function, and Geode checks
   only the version string. So before it hooks anything the mod checks the executable
   it is in (the build stamps of GeometryDash.exe, libcocos2d.dll and
   libExtensions.dll). On a game it was not built for it stays off, writes one line to
   the log, shows one line on screen, and the game plays as if it were not installed.
   Geode does not support 2.2091 yet and runs in its forward-compatibility mode, where
   its own menus are not available, so the mod brings its own popups, scrolling and
   keys. These stopgaps, the executable check among them, go once Geode supports
   2.2091.
 * **No Android build this time.** The Android game is on the new version too, and
   there is no Android port of its bindings yet. v0.5.0's Android pre-release stays
   as it was, for GD 2.2081.
 * **All 22 main levels clear on 2.2091**, cold, in one game session: in 27 repairs
   for the end of the level (v0.5.0 on 2.2081: 35) and in 33 with every coin (v0.5.0:
   60). The paths are the same on the first 2.209 executable.
 * **The levels the game features.** A new survey took 126 custom levels from the
   game's own lists: 50 of the last year's Daily levels, the last 50 Weekly levels
   and every Event level in the Safe list, each solved cold at the release defaults
   in up to 20 minutes, on GD 2.2081 just before this release. 74 cleared: 41 of 50
   Daily, 23 of 50 Weekly (demons) and 10 of 26 Event levels. See
   docs/CUSTOM_LEVELS.md.
 * **Section search can be turned off.** Under Solve, Options has a **Section
   search** box, ticked by default. Unticked, a wall the model cannot get past is
   never handed to the search of the game itself, so a level that needs it is not
   solved. (`dpsecauto=0` is the same switch.)
 * **Avoid glitches is a three-way choice**, with arrows: **Off** (the default),
   **Glitches only** (the old box: the searches drop the routes that rest on
   frame-level glitches), and **Glitches + wave**, experimental. Glitches + wave adds
   the wave's behaviour. The wave's button is not planned to change faster than a
   hand can, the wave keeps a clearance from the floor, the ceiling and solids
   instead of crawling along them, and among the routes left it prefers fewer button
   changes and a line nearer the middle of its corridor. Where no route keeps all
   that, it eases in steps for that stretch only, and takes one step back every
   900 px past the wall. Each step is said on screen. A choice saved from the old
   box reads as Glitches only or Off.
 * **Every option has an (i) button** that says what each of its values means, in a
   popup that scrolls. The menu keeps one line naming what is not at its default.
 * **Hold Play to open the menu.** A box in Options makes a click on a level's play
   button just play it, as without the mod, and opens the menu when the button is
   held (a ring fills around it while you hold). The menu is always one hold away,
   so the setting can always be turned back.
 * **The yellow note names a version.** On a custom level with objects from 2.0 on,
   the play menu now says which version the newest of them came in ("Its newest
   objects are from 2.2"). Which levels get the note is unchanged.
 * **The replay panel** (`Tab`). While a solve runs with the screen dark, the free
   area from the middle of the screen to the top-right plays GD's own record of the
   deepest attempt the game has verified: the level's boxes, the path, the moving
   parts, the corrections. `Tab` goes round the whole verified path, its last
   stretch and off. While it is on screen, `F2`, `F3`, `F4` and the arrows stop,
   step, seek and pace it. It is never drawn over the showing of the solution, and
   it starts empty on every level. See docs/REPLAY_PANEL.md.
 * **Solve faster is gone** from the settings. The cfg key `dpplainbeside=3` still
   does it.
 * **More of the model is on by default.** The rules added since v0.5.0 that showed
   no harm on the main levels, and that did not make the model part from the game
   sooner on the recorded attempts of 124 custom levels, are on: among them the
   speed a wall needs is asked of the model, a long level's late deaths rank, and a
   dozen fidelity rules for rotated frames, dash rings, drop rings, spiders, robots
   and the wave on ramps. Two were measured to do harm and stay off. A rule can
   still change the route a custom level's search takes, and with it the time.
 * **171 solver switches and 17 cfg keys are gone**, the ones that only ever turned
   off a rule that has been on by default for a release or more. A removed switch or
   key is refused by name, with the commit that fixed its behaviour. The list is
   dp/removed_flags.txt.
 * **Fixed:** a level with very many triggers could take tens of gigabytes while
   the solver kept its restart points, and stop the game on a machine short of
   memory (the saved points now keep only what changes). A search step could read
   memory an earlier step had freed, which on some custom levels took the game
   down. `F10` could leave the iteration map's path on the level after it was
   hidden.
 * **Nothing is recorded while it drives, on 2.2091 too.** Since 2.209 the game has
   a death tracker that uploads where players die. While the bot drives, that upload and
   the other new records (play time, the death sessions) are blocked like the rest.
   Each session's log ends with a memory audit of the save, which should read
   `none` for the level's record.
 * **For developers:** py/divergence_view.py draws, in a browser and without the
   game, where the model parts from GD's own record of an attempt.

# v0.5.0

 * **Fewer repairs, and more custom levels clear.** In one game session, cold, all
   22 main levels clear: in 35 repairs for the end of the level (v0.4.0: 54) and in
   60 with every coin (v0.4.0: 70). The 17 spin-off levels all clear in 45 repairs
   (v0.4.0: 80); their six coin levels with every coin in 64 repairs (v0.4.0:
   112). Of the same 100 rated
   custom levels, drawn at random, that v0.4.0 was measured on, 93 cleared within
   the 20 minutes each level is given (v0.4.0: 67); the breakdown is in
   docs/CUSTOM_LEVELS.md, and the per-level tables in
   docs/RESULTS.md.
 * **Most of the model work is fidelity.** More than 200 rules, across nearly every
   gimmick family the levels use, were measured against the game and are on by
   default (the families are listed below). Most of it went into the 2.1 and 2.2
   gimmicks, where the custom levels gained the most.
 * **The play menu has an Options page.** Under Solve, an Options button in the
   top-right corner opens the settings about how the level is searched, which keep
   the last Start's choices: Avoid glitches, moved there from under Coins, and
   Search, picked with arrows — DP (the reachability DP, the default), Beam or
   Monte Carlo. The last two are experimental: while one is chosen, the menu shows
   a red line saying so. The menu itself keeps the mode, Coins and Start.
 * **The seek bar starts from checkpoints.** A plain replay takes a checkpoint
   every 600 ticks (cfg `seekcp=600`, at most 64), and a seek starts from the
   latest one at or before its target, running screen-off up to it (cfg
   `seekblind`). On long levels, back seeks went from 0.5-4.2 s to 0.04-0.18 s and
   forward ones from 1.0-6.2 s to 0.14-1.7 s. A restored run is checked against the
   run from the start on every tick, and the first difference turns the
   checkpoints off for the session. `seekcp=0` and `seekblind=0` turn them off.
 * **Human takeover** (a debugging instrument, off by default). A plan is flown to a
   tick or an x (cfg `takeover`, `takeoverx`, `takeoverwait`), a strict checkpoint
   is taken there, and the jump button is the person's from then on. The game's own
   checkpoints are each made strict, and every death goes back to the newest one;
   `F8` (the keybind *End the human takeover*) or `takeoverend` in `cmd.txt` ends
   it. Each attempt is written as a whole plan, and `py/takeover.py check` flies it
   again in the game and in the model to find where the two part. It stays a bot
   session: the badge reads `HUMAN INPUT`, every record stays blocked, and nothing
   it writes is ever a seed for a solve. See docs/TAKEOVER.md.
 * **The HUD names the loop's phase** in its second row when no measure applies, and
   `result.txt` logs every phase that lasts a second or more.
 * **The play menu's warnings follow the model.** What is now modelled is no longer
   named: a teleport portal's push and kept height, and teleport orbs, teleport
   triggers and the gravity trigger as such. The red line names what is still left
   out — teleports that move you sideways or have several exits, teleport orbs that
   push you, teleport triggers set off by touch or a spawn or keeping an offset,
   gravity triggers set off by touch or a spawn or for player 2, and Follow Player
   Y triggers that are delayed, touch- or spawn-started, or more than two at once —
   and now two-player levels, where the bot presses only player 1's button. The
   gravity-portal count includes the gravity toggle portal.
 * **Plan files.** The input level is a button bitmask: bit 0 is player 1's jump, as
   every plan so far writes it, and the other bits are reserved. A plan holding
   another button is refused with a result line saying why, where it used to replay
   with that button silently dropped. A plan can also carry an attempt sequence
   (`attempt=N` blocks with `until=<tick>`), whose earlier blocks are replayed as
   prelude attempts before the clearing one (cfg `planfile`); `verify_solutions`
   runs such a plan whole.
 * **Levels that change from one attempt to the next** (all off by default). cfg
   `attemptone`: while the mod drives, the attempt count reads what a first attempt
   reads and Item Persistence carries nothing back, so every bot attempt is the
   level's first (Windows only); the play menu then moves such a level from the red
   line to a yellow note, since a level that opens only after dying will not be
   solved. `attemptcount=k` pins the attempt count to k. `persistworld=k` (with
   `persistitems`) starts every head reset from one fixed table of kept items, and
   a clear made that way is judged from a fresh game after k empty attempts.
 * **The session ends where the level does.** Going back to the editor, from the
   pause menu or the end screen, now ends the session as leaving the level does;
   before, the session outlived its level and the next level's record was not
   saved. A death after a session has ended (after a refusal, for instance) no
   longer starts a repair.
 * **Resets and memory.** cfg `singlereset` (on): a reset with no physics since the
   last one does not rebuild the level again, and a head reset clears the spawn
   bookkeeping a checkpoint flight left, so a start trigger no longer misses its
   second reset and kills on the first tick. A finished or abandoned solve frees
   what it held — anchor rows, checkpoint sets, section-search tables, the solver's
   caches — instead of keeping it until the next Solve; a job that outlives its
   session is reaped, and a section search still suspended when its session ends
   is dropped rather than run to its end.
 * **Less time spent outside the search.** The section hooks are enabled only while
   a section search can run (cfg `sectionhooklazy`, on); their dispatch had taken
   about 80% of a screen-off replay of a move-heavy level. Recordings are signed
   while they are written, so a call no longer rescans them (about 0.5 s in the mod
   and 0.3-0.7 s in the core per call on a large recording), and a `--replay` call
   reuses the level the previous one built (0.4-0.9 s per recorder pass).
 * **Settings now on by default** (each `=0` is its off arm), beside `seekcp`,
   `seekblind`, `singlereset` and `sectionhooklazy` above:
   * `snapnodesync`: a screen-off stair snap reads the object where it really is.
   * `headbandfresh`: a head reset starts the band as a fresh attempt does.
   * `anchorboardarm`, `offboardfree`, `offboardband`: the off-board test is the
     death credit's own, skips Free Mode, and ignores a cutscene zoom's band jump.
   * `voidband`: a death after a long run over empty floor closes that run.
   * `cpfollowsection`: a checkpoint restore puts Follow Player Y objects back in
     the collision cells a run from the start has them in.
   * `dpseedpickups`: an anchor past a key opens the box the key switches on.
   * `dpsecrecaway`: a round that died short of a wall is recorded before the
     wall's section search.
   * `obbnolatch`: the level export leaves rotated objects as it found them.
   * `slicekeepgroups`: a sliced copy's placeholders keep their objects' groups.
   * `secwsnapauto` (with `secwsnapset=5`, `secwsnapany`), `secwsnapgate`,
     `secheadaccel`: the section search, below.
 * **Changed default: `areaenv` is now 0.** v0.1.6 made deadly the whole box an Area
   effect's random variance could put an object in, because the game drew it from
   seeds it never resets. Since v0.3.2 those seeds are fixed while the mod drives,
   so the recording holds the positions this game actually uses, and the rest of
   the box is no longer treated as deadly. A plan played without the mod can meet
   such objects elsewhere. `areaenv=1` restores the box.
 * **New settings, off by default**, besides those above: `recprovchain` (a
   checkpoint flight counts as the attempt it flew from when recordings are read),
   `dpunseenfirst` (before a wall's first section search, one more round enters
   the boxes whose effect has not been seen yet), `secsnapring` and
   `seclieskippsnap` (section search), `cpactivation`
   and `cpvisafter` (checkpoint-restore diagnostics), and `worldgates`, a
   measurement that models the item gates without coins.
 * **Model fixes, by gimmick family.** Every rule below was measured against the
   game first — recordings, injected states, calibration levels or the
   disassembly — and is on by default, with a `--no-<flag>` off arm. Five older
   switches are on now as well: `--dualband`, `--dualslide`, `--groupxsplit`, and
   `--heldall` with `--heldkeyread` (the held button is carried in every mode).
   The old positive spellings still parse and change nothing.
   * *Ramps and slopes*: how a body is seated on, carried along, launched from and
     killed by a ramp, moving ramps and a ramp's wall side included; a ship's or
     UFO's hazard hit waits for the ramp pass; a ramp scaled to zero is dropped.
     `--lingerlaunch` `--ceilgracecube` `--rampendclick` `--exitjump`
     `--rampslablift` `--movingwallseat` `--movingwallhead` `--movingwallrise`
     `--slopeunchanged` `--thinlandjump` `--apexreacq` `--apexstick` `--slopestick`
     `--slopelandtol` `--seatspeed` `--ridec6` `--seatc6` `--stickfallback`
     `--ridebound` `--graceendonly` `--ceilstrict` `--slidemini` `--nanslope`
     `--portseatorder` `--flyhazdefer` `--flyhaztele` `--waveramp`
     `--waveslopewall` `--bvar9kill` `--flipceilball` `--ridehold`
   * *Swing*: the flip tick meets solids and pushes in the new gravity, a flipped
     swing lands on the band's top, and a swing on a sinking floor or under the
     band's top moves as in the game. `--flipcollide` `--mpushflip`
     `--swingbandceil` `--swingseatpress` `--swingheadceil` `--swingceilmoved`
   * *Solids that push, crush or bonk*: a face climbing into a cube or robot puts
     it on top; a ceiling coming down holds or pushes the body under it, and a bonk
     uses the outer box; the 1859 and 2866 arms read their box after the move; a
     moving passable solid is a one-way top. `--cubempush` `--cubempushnoground`
     `--mpushupright` `--mpushside` `--headpushhold` `--crushhead` `--dashhead`
     `--bonkouter` `--flipbonk` `--armpost` `--fgpost` `--nocolone`
   * *Dual*: a body born on a gravity portal takes it on its first step, with player
     1's press; pad flips, a ring's gravity turn, drop rings and spent rings reach
     the partner as in the game; each body keeps its own hover, x offset, button in
     a control window and, under Unlink Dual Gravity, its own flip; a solo portal
     ends the dual as the game's two collision passes do; the dual band follows the
     camera zoom; an anchor carries the second body's dash and spent rings.
     `--dualbornportal`
     `--dualbornpress` `--dualpadflip` `--ringtaplink` `--rtlp2ring` `--spentp2`
     `--dualhover` `--p2xoff` `--ctrlwinp2` `--dualunlink` `--p2solo` `--p1soloend`
     `--dualendrot` `--dualzoomband`
   * *Rotation and turned boxes*: gameplay rotation with Dont Slide, and speed
     portals in a turned group; moving or Rotate-turned hazards and rotated rings
     are tested as turned boxes, as the game tests them; Rotate triggers carry on
     past the recording's end; a cube's spin rate is fixed for its size when the
     spin begins, and leaving a flying mode begins one.
     `--rotboost` `--rotspeed` `--dynobbrot` `--hazrectgate` `--rotnospawn`
     `--rotlag` `--rottail` `--rotobb` `--rotobblatch` `--ringrotbox`
     `--ringrotground` `--ringpobb` `--waveringbox` `--waveringmini`
     `--waveringminibox` `--rotlatch`
   * *Hazard tests*: the wave's tests (turned box against box, endpoints, one-way);
     a saw's edge at its radius for the wave, UFO and cube; hazards tested after a
     box-changing portal, once a tick, and a size portal's new box on its own tick.
     `--waveobbaabb` `--wavehazendpoint` `--waveoneway` `--sawexactwave`
     `--sawexactufo` `--sawexactcube` `--hazafterportal` `--hazportalonce`
     `--togglescale`
   * *Portals and the band*: a mode portal's band is centred on its start; portals
     are met in the collision pass's own order, and a later portal meets the new
     box only past a rect re-read; a portal left only in y stays seen; Free Mode
     bands, a fresh attempt's first band pass and a band past the recording's end;
     same-speed portals; Moves do not shift speed portals or triggers in x.
     `--portalstarty` `--portseenx` `--portorder` `--portreread` `--bandsrc`
     `--bandtrackseen` `--flyfree` `--headbandfresh` `--flyclampreach`
     `--bandholdend` `--spdsameonce` `--waveportalfree` `--movelockx`
   * *Gravity*: the gravity toggle portal and the gravity trigger are modelled;
     grouped gravity portals get their latch, and levels with the gravity fix flip
     the ship's and UFO's threshold too. `--gravtoggle` `--gravtrig` `--dyngplatch`
     `--fixgravthr`
   * *Teleports*: the teleport trigger and the teleport orb are modelled; a
     teleport portal's push and kept height; landing in the collision pass's order;
     a teleport over the spawn fires on the first step; targets sideways or ahead
     in x; a teleport an activator switches on. `--teleporttrig` `--teleportorb`
     `--tppush` `--tpkeepy` `--tporder` `--tpseen` `--tpsideways` `--tpportalx`
     `--tpspawn` `--acttele`
   * *Orbs, rings, pads and dashes*: Multi Activate rings fire again on the next
     press; the yellow and pink orbs give a swing its tail; drop rings leave the
     ground flag alone; when a pad touches, refills, is spent or undone, after a
     mode portal or on a size portal's tick; dash rings for the ship, UFO and wave,
     and a dash that lasts through a landing; presses a ring takes first, and a
     press and release on one tick; ball taps, and a ball flapping through a UFO
     portal. `--multiorb` `--orbswing` `--dropground` `--padtouch` `--padrefill`
     `--padafterportal` `--padundoflip` `--padoldbox` `--flypadhead`
     `--spentpadhold` `--ringspendflap` `--earlyring` `--dashclamp` `--flydashring`
     `--wavedash` `--dashground` `--holdseed` `--sametick` `--ballportalflap`
     `--balltaplag` `--ballscaleair` `--bluepadpost`
   * *Spider*: the 2.2 spider pad; spider teleports against moving bands, ramps
     and walls; the spider's inner kill box and its crush grace; spider orbs and
     rings. `--spiderpad` `--spiderband` `--spidercoyote` `--spiderspend`
     `--spiderpresslate` `--spiderbandback` `--spiderinner` `--spidercrushgrace`
     `--spidergraceage` `--spiderbandground` `--spiderpadfirst` `--spiderorbface`
     `--spiderringfirst` `--spiderslopeline` `--flyspiderring`
   * *Toggle blocks, toggle orbs and touch triggers*: toggle blocks and touch
     Toggles switch groups off, or back on; presses that begin inside a box, and
     held buttons; an x-crossing trigger a box's chain has switched off does not
     fire, while a touch Toggle's box still switches an x-crossing Move back on;
     touch-started Options triggers; spawn and toggle orbs take presses as the
     game did. `--boxoff` `--boxoffown` `--touchoff` `--anchorentry` `--pressfresh`
     `--flyheldbox` `--tapdisarm` `--boxarm` `--holdtap` `--ctrltouch`
     `--touchwinahead` `--touchvisual` `--slamdedup` `--holddedup` `--slambox`
     `--boxparts` `--ringpresseat` `--spawnringon` `--offafter` `--acthazoff`
   * *Force circles*: one without range or target mode pushes with a constant force
     inside its circle, and a moving one is met where the recording has it.
     `--forcecircle` `--forcelive`
   * *Area effects* (Area Move, Scale and Rotate): their objects stand where the
     game's area pass puts them for the state's own player. `--areamove`
   * *Follow Player Y*: modelled; its objects meet the player only near the
     collision cell they were loaded in. `--followy` `--followsection`
   * *Time warp*: the ship steps under a time warp, the UFO under a slow one only;
     touch- and spawn-started warps do not fire on the crossing; the cube's air
     spin follows the warp. `--warpfly` `--warpflyufo` `--warpslowonly`
     `--warpqueue` `--warprot`
   * *Camera zoom*: a zoom eases from where the last one had got to. `--zoomchain`
   * *Moving geometry*: queued Spawns start later; x-crossing Stops halt their
     Moves; movers and Rotates carry on past the recording's end, and a gap in a
     fast mover's recording is a stall; formula-driven and lock-to-player-x
     objects; each speed group crosses the autonomous triggers on its own; a
     zero-duration Move lands on the tick after it takes effect; ExpIn easing is 0
     at its start; wide and big static colliders, Extended Collision excluded.
     `--spawnlag` `--autostop` `--reclagup` `--reclagabs` `--moverextend`
     `--recendextend` `--recendresid` `--recendtail` `--recendauto` `--recstall`
     `--recleads` `--recanchorx` `--formularec` `--formularectouch`
     `--formulanolock` `--ctlautotail` `--anchormove` `--lockx` `--lockstop`
     `--locknodx` `--slidelive` `--slideend` `--autoown` `--instantlag`
     `--expinzero` `--escdyn` `--widescan` `--bigsection`
   * *Controls (Options trigger)*: a held button keeps acting on a control window's
     first tick. `--ctrlwinlag`
   * Off by default, and not part of what this release claims: `--tplatchexact`,
     `--bandowntrack`, `--moverextendvel`, `--goalgd` (with `--endportalx`) and
     `--worldgates`, a measurement.
 * **The search is behind an interface.** The layer DP reaches the model and the
   rules only through a contract — the model steps a group of states and fires the
   triggers, the rules credit, kill and score — and answers in one result type, so
   a search can be swapped without touching the physics. This is the first part of
   the plug-in search (What's next, in the README). Two more searches
   implement it: `--search beam` (width `--beam`, deterministic) and
   `--search montecarlo` (seeded random rollouts, reproducible from `--seed`,
   the anchor and the call's index; `--mcsteps` budgets it, `--mcstall` gives up on
   a dead end). Where montecarlo cannot rewind — gameplay rotation, control
   windows, a turned start frame, coins — it declines and the layer DP answers. In
   the loop, cfg `dpsearch=beam|montecarlo` (with `dpseed`) or the play menu's
   Options choose them; the layer DP stays the default. A level's solve is now a run the search is told about (begin,
   every verified flight, end), and a run left open by the previous level makes the
   next one refuse to solve, naming it.
 * **A plan whose own walk dies is searched again before it is flown**
   (`--resimladder`, on). The search keeps one representative of the states it
   merges, so the exact walk of a plan that reaches the goal (or joins an earlier
   plan) can die where the representative lived; the loop used to fly it anyway,
   and the game died where the walk did. Now the call searches again from the
   walk's own state 6, 24, 96 and then 240 ticks before that death, splices each
   answer onto the plan, walks it again from the start, and keeps the plan whose
   walk is clean or dies latest. It is not a guarantee: when four searches give no
   clean walk, the plan that lives longest is flown like any other.
   `--resimladderpartial` (off) extends it to plans that stop short of the goal.
 * **The cap ladder.** `--ladderovercap` (on): a window stuck at `--cap` is tried at
   twice and then four times the cap before the plain search. `--ladderprefix`
   (on): the ladder's next attempt resumes the last one's layers up to the first
   layer whose cap differs. Each rung's cost is printed. `--walkresume` (off) keeps
   witness-walk checkpoints across the ladder's attempts.
 * **A join that dies at once is not flown.** `--rejoinprobe` is 5 by default: a
   plan that rejoins an old one and whose walk dies within 5 ticks of the join is
   searched again without the join (`--no-rejoinprobe` or 0 turns it off).
 * **The section search** puts the section's moving objects back with its snapshot
   (cfg `secwsnapauto`, set 5), but only after the player snapshot alone finds
   nothing (`secwsnapgate`), and takes the head's boost accumulator and pad flag
   from the plain flight (`secheadaccel`). All three are on.
 * **Anchors carry more, and a call is reproducible from what it was given.**
   Anchors carry the second dual body's dash and each body's buffered-jump byte; a
   formula-driven object is placed from a recording only when it is the anchor's
   own attempt's; `--xtrack`
   and `--items` are signed and snapshotted with the other file arguments; each
   call's input signature names the level table it had; portal uids in
   `--spentpad` and `--anchor-state` are listed in uid order; the core's item
   givers and coin uids are emptied per call; and session globals that carried one
   level's value into the next are reset with their session.
 * **For developers.** The solver core is compiled once (`gdsolver_dpcore`) and
   linked by both `leveldp` and the mod. New calibration levels in `data/rigs`
   cover the spider pad, the gravity toggle portal, the gravity trigger, the
   teleport trigger, orb and portal, Follow Player Y and gameplay rotation with
   Dont Slide; the objrects export gains Dont Slide, teleport force and direction,
   and Extended Collision. `py/seqify.py` finds the shortest prelude of empty
   attempts for an attempt-dependent clear, or says it is not a cold success; the
   clear judge replays without the run's solver settings, and a pass that skipped
   the game's deaths is not a clear; `verify_solutions` takes `--cfg`;
   `tools/reset_coverage.py` classifies more of the session globals. The cold
   baselines are re-adopted from this release's one-session runs.
 * **Removed.** Nine `leveldp` switches that were off by default and had been
   superseded: `--ceilpin`, `--groupholdend`, `--hazaabb`, `--ladderx0`,
   `--old-latency`, `--old-slope`, `--pinminnoswing`, `--rotlast` and
   `--touch-from-anchor`. The default behaviour is unchanged, and each is refused
   by name. No `autorun.cfg` key was removed. In plan files, an input level other
   than 0 or 1 is now refused (see Plan files above).
 * **Thanks** to XBribo for PR #2, which raised four of this release's fixes: the
   dual exit (a solo portal the second body takes ends the dual with player 1
   taking its place, `--p2solo`, and when player 1's own pass ends the dual the
   second body is not stepped that tick, `--p1soloend`), ExpIn easing at u == 0
   (`--expinzero`), a death after the session ended no longer starting a repair,
   and going back to the editor ending the session. The commits that carry them
   credit XBribo as co-author.

# v0.4.0

 * **Fewer repairs, in about half the time.** Against the one-session runs made
   before v0.3.2's release, the 22 main levels went from 98 to 54 repairs and from
   1,215 to 780 s without coins, and from 104 to 70 repairs and 2,065 to 1,057 s
   with every coin; the 17 spin-off levels from 116 to 80 repairs without coins,
   and their six coin levels from 204 to 112 repairs and 3,396 to 792 s. Every
   level still clears cold, with every coin in the coin runs. The results (now in
   docs/RESULTS.md) split each repair into a divergence (the model and the game came
   apart before the death) and a search (they agreed, and the plan was wrong):
   on the main levels more than four in five are search.
 * **The model rides ramps the way the game does.** Much of this release's model
   work is on ramps: where two ramps of one surface meet, how a body falling onto a
   descending ramp is seated, a ship's ride across a seam and onto the band's
   floor, flipped ships and cubes on ceiling ramps, the launch off a ramp's end,
   a ball under a ramp's underside, a UFO flapping off a ramp it rode, and the
   kill rules of spiked ramps. Each was measured against the game first. Dual
   sections keep each body's own flip and mode clocks, dash and slide; a spider
   resting on the band's face is supported as the game supports it; a teleport
   portal fires once per attempt.
 * **Replays fly from a checkpoint.** The loop's replays take a checkpoint every
   600 ticks, and a plan that agrees with an earlier attempt up to one of them
   flies from there instead of from the start (cfg `cpflight`, `cpflight=0` turns
   it off). A restore puts back the camera, the area effects and the other state
   a fresh attempt from the checkpoint would see, and a checkpoint is flown only
   when its attempt started from what an attempt starts from now.
 * **Most switches the release runs with are no longer switches.** 276 of
   `leveldp`'s spellings and seven of the mod's `autorun.cfg` keys are gone, so
   most of the rules v0.4.0 is measured with have no off arm any more (a few older
   ones keep theirs: docs/ARCHITECTURE.md, section 4.1). The removed ones are
   refused by name, not ignored: `leveldp` stops with "<spelling> was removed in <commit>; the
   behaviour is fixed (since <commit>)", and an `autorun.cfg` that names a removed
   key makes the game refuse to solve, with both commits in `result.txt`.
 * **The play menu warns before a solve.** A red line names what the model cannot
   express at all — teleports that move you sideways, keep your height, push you
   or have several exits, teleport orbs and triggers, the gravity trigger, more
   gravity portals than the solver can hold, a level that changes from one attempt
   to the next — and a yellow one marks a custom level that uses objects from 2.0
   on (in a survey of 300 custom levels, 206 of the 215 that use nothing newer
   cleared). A level that already has a solution gets neither, and Solve says when
   another version of the mod filed it. The menu fits the screen.
 * **The overlay draws its progress bars** as the game's own level bar: how far the
   run has got, and how far whatever the solve is doing now has got (a search, a
   recording of the moving geometry, a section search, a flight). A coin solve
   shows its best coin count. The rows of the overlay column are placed so that
   none draws over another. The bot badge, which neither F1 nor `hud=0` hides, now
   names the mod's version (`GDSOLVER BOT v0.4.0 - REPLAY`), so a recording says
   what produced the run.
 * **Avoid glitches** (experimental, on the play menu under Solve): the search
   drops routes that rest on frame-level glitches. It narrows the search on
   purpose, so it can lose the only route there is, and the menu says so.
 * **Solve faster (not reproducible)**, in the mod's settings and off by default:
   the plain search starts beside the cap ladder and is taken as soon as it will
   do. Usually faster, but the same level can take a different route and time
   from one solve to the next. Every plan is still flown and judged by the game.
 * **Level slicing keeps every object's id.** The copy a heavy level is solved on
   puts a placeholder where each left-out object was, so every object keeps the
   id it has in the level; area effects that depend on the id order now behave on
   the copy as they do on the level.
 * **A section search no longer takes the game down.** An exception inside one
   gives up that search, not the game, and says where it was thrown; an object at
   a non-finite position ends the search there.
 * **An Android build** (android64, GD 2.2081) is published on the releases page as
   a separate pre-release, as an experiment. It is not supported: the model's physics is Windows GD's, and
   whether Android GD behaves the same has not been measured. See
   [docs/ANDROID.md](docs/ANDROID.md).
 * **For developers.** `py/cold_regress.py --one-session` keeps a per-level
   deadline and a memory floor inside its game launch (`--level-deadline`,
   `--mem-floor-gb`), and the run's manifest names what they stopped and the
   commit its package was built from. The cold baselines are re-taken from the
   v0.4.0 one-session runs.

# v0.3.2

 * **Solving a heavy level from its online page no longer stalls, and Escape no
   longer crashes the game.** A level with 10,000 objects the run cannot depend on
   is solved on a copy (level slicing, v0.3.0). The page of an online level builds
   the level and starts the fade to it a few frames later, and the copy was swapped
   in between: the fade then put the level itself back on screen, where it ran with
   nothing driving it while the solve waited on the copy, and pausing reached for a
   copy that was no longer on screen. The swap now waits until the level it
   replaces is the one on screen, with no scene change pending, for up to 20 s.
 * **Random numbers are fixed while the bot drives.** The game draws Random and
   Advanced Random triggers, spawn delays and area effect variances from seeds it
   either reseeds from the clock on every attempt or never resets, so the same
   plan met a slightly different level each time. While a solve or replay runs,
   the seeds are set to the same values before every attempt; playing yourself is
   untouched. The play menu says when a level has a Random or Advanced Random
   trigger, or an area or enter effect with a random variance (a spawn delay's or
   an Advanced Follow's variance is not detected yet). SubZero's Power Trip with coins,
   which took 102 to 106 rounds depending on the run, now takes the same rounds
   every time.
 * **A spider just after its teleport can pass through a solid's side**, as in the
   game: for 0.04 s after it, the side of a block that does not reach past its
   front does not kill it. The model killed there, and a solve that starts inside
   that window is now told how long ago the teleport was.
 * **A death the model makes and the game does not is learnt.** When the two agreed
   on the state and differed only on the kill, the correction was filed as a no-op
   and never reached the solver, so the same death came back round after round.
 * **Section searches are faster.** The shader layer is no longer refreshed on every
   restore inside a search (a restore went from 74 to 12 us on a heavy level), and
   copying the game state during a search no longer rebuilds its maps. Neither
   changes what the search finds.

# v0.3.1

 * **The release workflow builds again.** v0.3.0 turned the touch mask into a
   fixed-width bit set, and one line still initialised it from `0`. The compiler
   used for desk builds (MSVC) accepted that; the one the release workflow uses
   (clang) does not, so the workflow failed, and the package on the v0.3.0
   release is a desk build rather than one the workflow made from the public
   tree. The line is now standard C++, and the desk build of it is the same
   machine code as v0.3.0's apart from its build timestamp: nothing the solver
   does changes, and the v0.3.0 results stand. This release's package is built
   by the workflow from this commit.

# v0.3.0

 * **The spin-off levels.** Beyond the 22 main levels, the 17 official levels of
   Meltdown, World and SubZero (with The Challenge) are now part of what a release
   claims: all 17 clear cold, and the six that have coins — Meltdown's three and
   SubZero's three — clear with every coin, each counted by the game itself. Their
   results are in the README. The suite reads them from level files (cfg
   `leveldir`), which are not distributed here.
 * **The section solver is part of the loop.** When the loop has spent at a wall
   as much as a section solve costs, or has run out of anchors, it starts one
   itself: the level is replayed to a practice checkpoint before the wall, the
   game is searched directly from there, and a plan the game reproduces is
   spliced back and pinned, and the loop carries on. A section whose prefix does
   not replay the same way twice is given up after two tries. A restore no longer
   gets slower as a search goes on, and a search's memory follows its live
   frontier. See [docs/SECTION_SOLVE.md](docs/SECTION_SOLVE.md).
 * **Coin search.** A clear refused for a missing coin is filed at the attempt's
   closest approach to that coin rather than at the finish, and ranked there
   until a route passes it; a plan that takes a coin the deepest plan missed
   counts as progress; section solves go to the coin wall and must take the coin;
   and a replay that skipped a speed portal is no longer planned against another
   route's moving geometry. The loop also works out what a coin needs first — the
   key, touch box or switch whose chain turns it on — and can route through it
   (cfg `routeprereq`, off by default). See [docs/COINS.md](docs/COINS.md).
 * **Level slicing.** A level with at least 10,000 objects its run cannot depend
   on is solved on a copy without them, and every plan that clears the copy is
   flown on the level itself before it is filed; where the two part, the objects
   around that point go back into the copy. Coin runs are not sliced (the copy
   credits no secret coin). See [docs/LEVEL_SLICE.md](docs/LEVEL_SLICE.md).
 * **The play menu.** A level's play button opens a menu — Normal, Replay, Solve
   and a Coins switch — in place of the panel. It opens on Replay when a solution
   is stored (with Coins set when the coin solution is) and on Solve otherwise.
 * **Leaving a level stops its solve.** A solve left mid-search used to run on in
   the background, and the next level's solve waited behind it while the overlay
   showed the old search's progress. The section solver's settings no longer
   carry from one level to the next either.
 * **Measured model corrections**, among them: a grown body standing on two or
   more supports is lifted on the grow tick; an upside-down cube keeps a moment of
   ground walking off a ledge where the level keeps the old gravity behaviour, as
   the game does; pads and rings a run has already fired are not fired again after
   a re-anchor; the id-1859 arm is read where it rides the player; and an
   autonomous Move whose group is switched off when the player reaches it does not
   fire.
 * **Custom levels** have a page of their own
   ([docs/CUSTOM_LEVELS.md](docs/CUSTOM_LEVELS.md)): what random samples of rated
   levels showed by the version they were built with, the kinds of level that
   usually solve, and the gimmicks the model does not cover. The rule is to clear
   what the game lets the loop clear; they are still not a supported set.
 * **Regression and release tooling.** A cold run's manifest now records every
   cfg value the mod held and the core's built-in defaults, and
   `cold_manifest.py` compares two runs (refusing any difference not named) and
   checks a run against the expected settings before it is blessed. New
   calibration tests pin the grow lift and the upside-down ledge against the
   game's own rows.

# v0.2.0

 * **Coins.** A **Coins** switch on the panel makes the goal the end of the level
   *and* every coin in it (cfg `coinroute`, dp `--coins`). The collected set is
   part of the search state, a coin counts when the player's own box overlaps it
   where its group has put it, and a state that leaves one behind is dead; the
   item counters that gate a coin (Fingerdash's and Dash's third) are modelled,
   and a replay that passes a coin the game did not credit is ended there and
   repaired. All 22 official levels clear cold with 3/3 coins, and those
   solutions are tracked as `data/solution_lv<N>_coins.txt` — each replays to a
   clear with the game's own count at 3/3. Coins collected while the mod drives
   are never awarded.
 * **The seam between recordings.** Past the tick where the last replay died,
   the model plans against a recording of the level played with no input. That
   recording reaches each trigger a little later than a real run, so an object
   still moving at the seam is now joined to it at the shift where the two agree
   (Fingerdash's rotating bars had jumped eight ticks back there); and the tick
   the game killed the player on is now held one row, since the model decides
   that kill on its next row (Deadlocked's spikes follow the player, and in the
   input-free recording they were elsewhere and switched off). Deaths that had
   repeated round after round in both places are gone.
 * **Moving geometry is dated more faithfully** — an object moved by both a
   touch box and an autonomous trigger dates each from its own motion, a touch
   box the attempt entered is dated from its entry tick, and an anchor in a
   rotated section windows the touch boxes by world x — along with many smaller
   measured corrections to the model since v0.1.6. The repair counts moved on
   ten levels as a result (see the table in the README).
 * **Removed switches.** Switches whose off arm had become dead code are gone,
   and passing one is refused by name instead of being ignored: dp exits with
   code 2, and a session whose `autorun.cfg` names a removed key stops before it
   solves.
 * **Regression.** Coin runs have their own baseline
   (`data/cold_baseline_coins.json`, with each level's coin count). Iteration
   counts are reported and no longer fail a run. A run records the game
   profile's resolution and refuses to compare with a baseline measured at
   another one: GD's saw radius follows that setting (one of Deadlocked's saws
   reaches 21.87 px at index 25 and 21.96 at index 8), and the published results
   and solutions were measured at index 25. A baseline can be adopted from a
   reviewed one-session run (`--adopt`) instead of being blessed in the same run.

# v0.1.6

 * **Nothing the game's own random numbers decide is planned against.** An Area
   Move can give its distance, length, offset, angle or x/y moves a variance, and
   the game takes the value from two random seeds it never resets. Such a block
   sits somewhere else on every attempt, after every level played before it in
   the same game, and on every machine that replays the solution: on Dash, 13
   blocks landed up to 278 px apart, and solving Fingerdash first moved them by
   up to 2 px, so the suite in one session and a game per level were not solving
   quite the same level. The mod now records, for such an object, the box it can
   be anywhere in — the game's own arithmetic, read from the binary and checked
   against the game on every object it moves (the `areaenv:` line) — and the
   model treats the whole box as deadly while the object is solid. Dash clears in
   46 repairs instead of 39; the other 21 levels make the same decisions as
   before. cfg `areaenv=0` records the rects as before.
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
