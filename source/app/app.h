#pragma once

// The application: everything that decides what happens, joined to nothing that
// decides how it looks.
//
// swAppRun owns the main loop. It holds the four independent pieces - the
// directory, the player, the favourites file and the updater - and is the only
// place that knows they exist together. Each of them can be changed or tested
// without the others because none of them refer to each other.

#include <stdbool.h>

// Runs until the user exits or the console asks the app to close. Assumes the
// services it needs are already up; see main.c for what those are and why.
void swAppRun(void);
