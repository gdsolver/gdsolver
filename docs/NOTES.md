# On measurements and the notes that are not here

Comments throughout `dp/` and `src/` say *when and how* a constant or a rule was
measured: the date, the level, the tick and the numbers. That is what makes a
comment auditable rather than merely confident, and it is all you need in order to
understand or change the code. The development notes those measurements were first
written into are the author's private notes; they are **not** part of this
repository and are not going to be. If you find a comment that leans on something
it does not state, it is a bug in the comment.

Some measurements were taken on custom levels, and the comments say so ("a custom
level") without naming one: their data is somebody else's work, and the runner
that solves them, its acceptance record and the levels themselves stay outside
this repository. What is published is the physics the measurement produced, which
is the part that applies to every level. Custom levels are not a supported set
yet; see [`CUSTOM_LEVELS.md`](CUSTOM_LEVELS.md).

What is published here:

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — how the solver, the mod and the tools
  fit together, and links to the documents on the section solver, coins and level
  slicing.
* [`../README.md`](../README.md) — what it is, what it solves, how to watch it
  run, and the safety rules.
* [`RESULTS.md`](RESULTS.md) — the cold runs of the official levels,
  level by level.
* the comments in `dp/` and `src/`, which carry the measured physics.

The acceptance criteria a change has to meet — byte-identical solver output, and
an unchanged iteration count per level — are in `CLAUDE.md`, rule 3.
