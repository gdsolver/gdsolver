#pragma once
// The game's own global variables the mod reads and writes: they are not members of any class, so no bindings carry them.
// Module-base-relative, for the GD 2.2091 executable only; used only after exe_guard accepted that executable.
//
// The trigger random stream (an LCG) and its variation table: cfg `rngfix` / `rngseed` fix them on every attempt, which is what
// makes a level with random triggers repeatable (the solver's determinism rests on it).
#include <cstdint>

// (inside namespace gdoff, from gd_offsets.hpp)
inline constexpr std::uintptr_t kSeedTriggerRva = GDOFF(0x6c2e90, GD209(0x906f68, 0x90bf68), 0x1241830);
inline constexpr std::uintptr_t kSeedVarIndexRva = GDOFF(0x6c2ee0, GD209(0x906fd8, 0x90bfd8), 0x1242080);
inline constexpr std::uintptr_t kSeedVarTableRva = GDOFF(0x6c2ef8, GD209(0x906ff0, 0x90bff0), 0x1242da0);
