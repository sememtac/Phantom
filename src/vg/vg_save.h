#pragma once

// Everything that has to outlive a power cycle: the bank, who you are, and
// whether you have ever taken a tournament.
//
// Deliberately small and deliberately explicit. There is no autosave and no
// dirty flag -- saves happen at four named moments where something the player
// would be annoyed to lose has just changed. Flash writes are cheap but not
// free, and a save every frame would be both wasteful and impossible to reason
// about when something comes back wrong.

// Pull the record out of storage and into `vg`. Silently does nothing if there
// is no save, or if the one found is from an incompatible build -- defaults
// already in place simply stand.
void vg_save_load(void);

// Write the current values out. Called on: entering a tournament (identity),
// winning a round (purse), buying repairs (spend), and taking the tournament
// (champion).
void vg_save_store(void);
