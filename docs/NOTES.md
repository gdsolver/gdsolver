# On the citations to notes that are not here

Comments throughout `dp/` and `src/` cite a measurement log (`docs/findings.md`),
a handover file (`docs/HANDOFF.md`) and a few campaign notes. Those are the
author's private development notes. They are **not** part of this repository and
are not going to be: they live in a separate lab tree alongside the official
levels' data dumps and the one-off analysis scripts.

The citations are left as they are on purpose, because they say *when and how* a
constant or a rule was measured, and that is what makes a comment auditable
rather than merely confident. Where a rule matters for reading the code, the
comment beside it states the measurement itself — the level, the tick, the
numbers — so nothing you need in order to understand or change the code lives in
the private notes. If you find a comment where that is not true, it is a bug in
the comment.

What is published here:

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — how the solver, the mod and the tools
  fit together, and what the section solver does.
* [`../README.md`](../README.md) — what it is, what it solves, how to watch it
  run, and the safety rules.
* the comments in `dp/` and `src/`, which carry the measured physics.

The acceptance criteria a change has to meet — byte-identical solver output, and
an unchanged iteration count per level — are in `CLAUDE.md`, rule 3.
