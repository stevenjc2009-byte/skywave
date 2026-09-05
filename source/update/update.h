// Checking GitHub for a newer Skywave and installing it.
//
// Copied in from Hotswap (Documents/3ds-project-folder/lllllll/source/update.c)
// rather than shared, and renamed hs_* -> sw_*. It is a proven implementation -
// it is what actually ships and self-updates on this console - and the two
// projects are better off diverging freely than coupled through a path.
//
// The whole thing is one blocking call, on purpose. An update is not something
// the user does in the background while listening - they ask for it, they wait
// for it, and then either the app restarts or it tells them why it did not.
//
// Nothing here touches playback or the favourites file. The worst an
// interrupted update can do is leave the release it was fetching un-installed.

#ifndef SKYWAVE_UPDATE_H
#define SKYWAVE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SW_UPDATE_INSTALLED = 0,   // a newer release was fetched and installed
    SW_UPDATE_CURRENT,         // this build is already the newest release
    SW_UPDATE_ERR_NET,         // GitHub could not be reached
    SW_UPDATE_ERR_PARSE,       // it answered with something unreadable
    SW_UPDATE_ERR_NO_ASSET,    // the newest release ships no .cia
    SW_UPDATE_ERR_DOWNLOAD,    // the .cia started arriving and then did not
    SW_UPDATE_ERR_INSTALL      // the system refused to install it
} sw_update_t;

// Asks GitHub for the newest release, and installs it if it is newer than this
// build. `tag` receives the release's version with any leading "v" removed -
// filled in whenever one was read, including when the answer is
// SW_UPDATE_CURRENT, so the caller can say which version it compared against.
sw_update_t sw_update_run(char *tag, size_t tag_cap);

// Arms a jump back into Skywave itself, for use straight after an install.
// Returns false when this build is running from the homebrew launcher, where
// there is no title to jump back into - the caller then has to ask the user to
// reopen it by hand. The jump itself happens during the normal shutdown.
bool sw_update_relaunch(void);

const char *sw_update_str(sw_update_t r);

#endif
