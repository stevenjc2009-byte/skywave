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

// Forward-declared, not #included: the full SwHttp definition lives behind
// http.h -> tcp.h, which pulls in mbedtls, and this header has never needed
// either. sw_update_check_h only ever hands `h` on to swHttpGetLocationH in
// update.c, so a pointer to an incomplete type is all this file needs - see
// directory.h's identical forward declaration for swDirRegisterPlayH, the
// same trade for the same reason.
typedef struct SwHttp SwHttp;

typedef enum {
    SW_UPDATE_INSTALLED = 0,   // a newer release was fetched and installed
    SW_UPDATE_CURRENT,         // this build is already the newest release
    SW_UPDATE_ERR_NET,         // GitHub could not be reached
    SW_UPDATE_ERR_PARSE,       // it answered with something unreadable
    SW_UPDATE_ERR_NO_ASSET,    // the newest release ships no .cia
    SW_UPDATE_ERR_DOWNLOAD,    // the .cia started arriving and then did not
    SW_UPDATE_ERR_INSTALL,     // the system refused to install it
    SW_UPDATE_ERR_UNVERIFIED,  // it could not be authenticated, so it was refused
    SW_UPDATE_AVAILABLE        // a newer release exists but has not been fetched
} sw_update_t;

// Asks GitHub for the newest release, and installs it if it is newer than this
// build. `tag` receives the release's version with any leading "v" removed -
// filled in whenever one was read, including when the answer is
// SW_UPDATE_CURRENT, so the caller can say which version it compared against.
sw_update_t sw_update_run(char *tag, size_t tag_cap);

// Asks GitHub which release is newest and compares it against this build.
// Installs nothing. Returns SW_UPDATE_AVAILABLE (tag written to `tag`),
// SW_UPDATE_CURRENT, or an error.
sw_update_t sw_update_check(char *tag, size_t tag_cap);

// Same as sw_update_check, but reads the redirect into a caller-owned handle
// `h` instead of a private one, so the caller can hold a pointer to `h` and
// call swHttpCancel(h) from another thread to unblock this check - the same
// mechanism swPlayerStop already uses on the player's stream handle. This is
// what app.c's startup update check needs and sw_update_check cannot offer:
// that check runs unattended on its own thread while the app is otherwise
// usable, and if it is still in flight when the app is asked to close, a
// join with no way to cancel it waits out this call's own worst case (8s
// connect + 12s TLS handshake + 10s HEAD, ~30s), or longer still on a DNS
// lookup that never times out at all. See app.c's g.upd_http and
// update_check_main for the caller this exists for.
//
// `h` must not be a stack local: sizeof(SwHttp) is 17,584 bytes, and the
// intended caller is a thread with a 16 KB stack. Give it a static or a
// zeroed heap block - see swHttpGetLocationH in http.h for exactly what
// "zeroed" buys and why it only matters on the first call through a given
// `h`. app.c gives it a member of the app's global state, exactly as
// player.c does with g.ping.
sw_update_t sw_update_check_h(SwHttp *h, char *tag, size_t tag_cap);

// Arms a jump back into Skywave itself, for use straight after an install.
// Returns false when this build is running from the homebrew launcher, where
// there is no title to jump back into - the caller then has to ask the user to
// reopen it by hand. The jump itself happens during the normal shutdown.
bool sw_update_relaunch(void);

const char *sw_update_str(sw_update_t r);

#endif
