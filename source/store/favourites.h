#pragma once

// Saved stations, kept on the SD card at sdmc:/3ds/skywave/favourites.tsv.
//
// The whole list lives in memory - it is at most 40 stations of a few hundred
// bytes - and is written out in full whenever it changes. Rewriting the file on
// every edit rather than appending means the on-disk copy is never a partial
// picture, which matters on a handheld that gets closed mid-thought.
//
// The format is one tab-separated line per station, on purpose. It survives
// being opened in a text editor, it survives this struct gaining a field, and a
// user who wants to move their list to another console can just copy it.

#include <stdbool.h>

#include "../net/directory.h"

// Must be called once at startup. Loads the file if there is one; an absent or
// unreadable file is not an error, it is a new user.
void swFavLoad(void);

// The favourites, in the order the user added them.
const SwStationList *swFavList(void);

// True if a station with this UUID (or, for stations the directory has no UUID
// for, this URL) is already saved.
bool swFavContains(const SwStation *st);

// Adds or removes. Both write the file immediately and return true if the list
// changed. Adding when the list is full returns false.
bool swFavAdd(const SwStation *st);
bool swFavRemove(const SwStation *st);

// Adds if absent, removes if present. Returns true if the station is saved
// after the call.
bool swFavToggle(const SwStation *st);
