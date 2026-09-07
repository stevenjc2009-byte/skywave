#include "update.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../net/tcp.h"
#include "../version.h"

// Where the releases live. Both URLs are built from the same two constants so
// a fork only has to change these, and so the tag read and the download can
// never end up pointing at different repositories.
#define GH_OWNER "stevenjc2009-byte"
#define GH_REPO  "skywave"

// The web /releases/latest URL, not the API one. It answers with a redirect to
// /releases/tag/<tag>, which means the newest version can be read straight out
// of a Location header - no JSON parser needed here, and no api.github.com rate
// limit to run into. That limit is 60 an hour and it is counted per IP address
// shared by everyone behind it, so an app that used the API would fail for
// reasons the user could neither see nor fix.
#define URL_LATEST "https://github.com/" GH_OWNER "/" GH_REPO "/releases/latest"

// Release assets are named for their version, so the download URL is built
// from the tag once it is known. This has to keep matching what the release
// process actually uploads - see tools/make_cia.sh.
#define URL_ASSET_FMT \
    "https://github.com/" GH_OWNER "/" GH_REPO "/releases/download/v%s/skywave%s.cia"

// The User-Agent GitHub insists on, and the redirect following, and the six-hop
// limit, all now live in source/net/http.c - this file shares the app's one HTTP
// client rather than keeping a second one of its own. What is different here,
// and the reason the distinction is worth the trouble, is that this path asks
// for certificate verification and never falls back to plain http: what arrives
// is an executable the console is about to install.
//
// URLs use SW_URL_MAX, which is generous on purpose. A release asset does not
// redirect to a tidy URL: it redirects to a signed one on
// release-assets.githubusercontent.com carrying a JWT and an expiry, which
// measured 900-odd characters when this was written. A buffer that merely looks
// big enough truncates it and the download fails with what looks exactly like
// the network being down.
#define CHUNK_BYTES  (32 * 1024)

// ------------------------------------------------------------------- version

// Compares two dotted numeric versions. Returns <0, 0 or >0 the way strcmp
// does. Anything non-numeric ends that version: "1.2.0-rc1" compares as 1.2.0,
// which is deliberate - a release candidate should not read as newer than the
// release, and it should not read as older either.
static int version_cmp(const char *a, const char *b)
{
    for (int part = 0; part < 4; part++) {
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') va = va * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') vb = vb * 10 + (*b++ - '0');
        if (va != vb) return va < vb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

// ---------------------------------------------------------------- the check

// Reads the newest release's tag out of the redirect /releases/latest answers
// with. The body is never downloaded.
//
// Takes the SwHttp handle rather than owning one, so a caller with a
// cancellable check - sw_update_check_h, ultimately app.c's startup check -
// can reach in and stop this mid-flight. sw_update_check's own blocking path
// still gets there, just through a handle it mallocs and frees itself; see
// that function.
static bool read_latest_tag(SwHttp *h, char *tag, size_t cap)
{
    // On the heap: a URL is 2 KB and this runs on the app's worker thread.
    char *loc = (char *)malloc(SW_URL_MAX);
    if (!loc) return false;

    if (swHttpGetLocationH(h, URL_LATEST, loc, SW_URL_MAX) != SW_HTTP_OK) {
        free(loc);
        return false;
    }

    const char *marker = "/releases/tag/";
    const char *p = strstr(loc, marker);
    if (!p) { free(loc); return false; }
    p += strlen(marker);
    if (*p == 'v' || *p == 'V') p++;

    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != '/' && i + 1 < cap) {
        tag[i] = p[i];
        i++;
    }
    tag[i] = 0;
    free(loc);
    return i > 0;
}

// -------------------------------------------------------------- the install

// Streams an already-open response straight into a CIA install. The file is
// never held in RAM or written to the SD card first: there is no need for a
// copy, and a half-written one lying about after a lost connection is exactly
// the kind of thing a user would later try to install by hand.
static sw_update_t install_response(SwHttp *h)
{
    // How many bytes are supposed to arrive. Without this there is no way to
    // tell a finished download from a connection that died three quarters of the
    // way through - both simply stop producing bytes - and the difference is
    // whether a truncated executable gets committed.
    char len[32];
    long total = swHttpHeader(h, "Content-Length", len, sizeof(len))
                    ? strtol(len, NULL, 10) : 0;

    // A chunked body carries its own end marker, so it is allowed to have no
    // length; completeness is then judged by whether the terminating chunk
    // arrived. GitHub sends a length today, but the signed asset host is free to
    // change its mind and this costs nothing.
    if (total <= 0 && !h->chunked) return SW_UPDATE_ERR_DOWNLOAD;

    Handle cia = 0;
    if (R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &cia)))
        return SW_UPDATE_ERR_INSTALL;

    // Static rather than automatic: 32 KB of stack inside a call this deep is
    // how an app that works on a New 3DS crashes on an Old one.
    static u8 chunk[CHUNK_BYTES];
    u64 written = 0;
    int idle = 0;

    for (;;) {
        int got = swHttpRead(h, chunk, sizeof(chunk));
        if (got < 0) break;                 // ended, cleanly or otherwise

        if (got == 0) {
            // A quiet moment is normal; a quiet minute is a dead connection.
            if (++idle > 300) {
                AM_CancelCIAInstall(cia);
                return SW_UPDATE_ERR_DOWNLOAD;
            }
            continue;
        }
        idle = 0;

        u32 wrote = 0;
        if (R_FAILED(FSFILE_Write(cia, &wrote, written, chunk, (u32)got,
                                  FS_WRITE_FLUSH)) || wrote != (u32)got) {
            AM_CancelCIAInstall(cia);
            return SW_UPDATE_ERR_INSTALL;
        }
        written += (u32)got;
    }

    // Checked before anything is committed, because a connection that dies
    // mid-transfer looks exactly like a finished one to the loop above.
    bool complete = h->chunked ? h->chunk.done : (written == (u64)total);
    if (!complete) {
        AM_CancelCIAInstall(cia);
        return SW_UPDATE_ERR_DOWNLOAD;
    }

    if (R_FAILED(AM_FinishCiaInstall(cia))) return SW_UPDATE_ERR_INSTALL;
    return SW_UPDATE_INSTALLED;
}

static sw_update_t fetch_and_install(const char *tag)
{
    // Both on the heap: SwHttp is around 17 KB and a URL is 2 KB, and this runs
    // on the app's worker thread rather than the main one.
    SwHttp *h   = (SwHttp *)malloc(sizeof(SwHttp));
    char   *url = (char *)malloc(SW_URL_MAX);
    if (!h || !url) { free(h); free(url); return SW_UPDATE_ERR_DOWNLOAD; }
    memset(h, 0, sizeof(*h));

    snprintf(url, SW_URL_MAX, URL_ASSET_FMT, tag, tag);

    SwHttpResult r = swHttpOpenVerified(h, url);
    free(url);

    if (r != SW_HTTP_OK) {
        int status = h->status;
        swHttpClose(h);
        free(h);

        // A download that could not be authenticated is its own answer, and it
        // arrives here for two different reasons: a certificate that failed to
        // check out, or a redirect that tried to drop the connection to plain
        // http (refused in open_url). Reporting either as "could not reach
        // GitHub" would send the user to look at their wifi, and reporting it
        // as a download failure would invite them to just try again forever.
        if (r == SW_HTTP_ERR_CERT) return SW_UPDATE_ERR_UNVERIFIED;

        // A release that exists but has no matching .cia is a different problem
        // from a network that is down, and the user can act on the difference.
        if (r == SW_HTTP_ERR_STATUS)
            return status == 404 ? SW_UPDATE_ERR_NO_ASSET : SW_UPDATE_ERR_NET;

        // Distinct from the tag read's failure on purpose: GitHub has already
        // been reached by this point, so "could not reach GitHub" would send the
        // user off checking their wifi over a problem with the file.
        return SW_UPDATE_ERR_DOWNLOAD;
    }

    sw_update_t out = install_response(h);
    swHttpClose(h);
    free(h);
    return out;
}

// ---------------------------------------------------------------------- api

sw_update_t sw_update_check_h(SwHttp *h, char *tag, size_t tag_cap)
{
    if (tag && tag_cap) tag[0] = 0;

    // No service to bring up here any more. Sockets, the random number generator
    // and TLS are started once in main.c and stay up for the life of the app -
    // the old httpcInit/httpcExit pair around this function existed because
    // httpc was reference-counted and this was a nested user of it.

    // Checked before anything is fetched. Without a trust store nothing can be
    // verified, and this path exists precisely so that an executable is never
    // installed off an unauthenticated connection - so it stops here rather
    // than at the certificate check, where the message would be less clear.
    if (!swNetHaveTrustStore()) return SW_UPDATE_ERR_UNVERIFIED;

    char latest[32] = {0};
    sw_update_t out;

    if (!read_latest_tag(h, latest, sizeof(latest))) {
        out = SW_UPDATE_ERR_NET;
    } else {
        if (tag && tag_cap) snprintf(tag, tag_cap, "%s", latest);
        out = version_cmp(latest, SKYWAVE_VERSION) > 0
                ? SW_UPDATE_AVAILABLE
                : SW_UPDATE_CURRENT;
    }

    return out;
}

sw_update_t sw_update_check(char *tag, size_t tag_cap)
{
    // On the heap: SwHttp is about 17 KB. Private to this call, so nothing
    // outside it can reach in and cancel it either - fine here, because
    // sw_update_run (the only caller of this path) is one blocking call the
    // user is already waiting on and can simply retry. Not fine for a check
    // that runs unattended while the app stays usable, which is exactly what
    // app.c's startup check does - see sw_update_check_h, and app.c's
    // g.upd_http, for the caller that needs to be able to cancel this.
    SwHttp *h = (SwHttp *)malloc(sizeof(SwHttp));
    if (!h) return SW_UPDATE_ERR_NET;
    memset(h, 0, sizeof(*h));

    sw_update_t out = sw_update_check_h(h, tag, tag_cap);

    free(h);
    return out;
}

sw_update_t sw_update_run(char *tag, size_t tag_cap)
{
    // The tag is read into a local rather than out of the caller's buffer,
    // because the caller is allowed to pass none. Before the check and the
    // install were split apart this function held the tag itself and the
    // install could not be reached without one; routing the install through
    // the caller's pointer would have made a NULL tag install nothing.
    char latest[32] = {0};

    sw_update_t r = sw_update_check(latest, sizeof(latest));
    if (tag && tag_cap) snprintf(tag, tag_cap, "%s", latest);
    return r == SW_UPDATE_AVAILABLE ? fetch_and_install(latest) : r;
}

bool sw_update_relaunch(void)
{
    // Under the homebrew launcher this process is not a title, so there is
    // nothing to come back to - the freshly installed CIA is a different thing
    // entirely and jumping into it from here is not what "restart" means.
    if (envIsHomebrew()) return false;

    aptSetChainloaderToSelf();
    return true;
}

const char *sw_update_str(sw_update_t r)
{
    switch (r) {
        case SW_UPDATE_INSTALLED:     return "Update installed.";
        case SW_UPDATE_CURRENT:       return "Already up to date.";
        case SW_UPDATE_ERR_NET:       return "Could not reach GitHub.";
        case SW_UPDATE_ERR_PARSE:     return "GitHub's answer made no sense.";
        case SW_UPDATE_ERR_NO_ASSET:  return "That release has no CIA to install.";
        case SW_UPDATE_ERR_DOWNLOAD:  return "The download did not finish.";
        case SW_UPDATE_ERR_INSTALL:   return "The console refused to install it.";
        case SW_UPDATE_ERR_UNVERIFIED:
            return "Could not verify the download. Not installing it.";
        case SW_UPDATE_AVAILABLE:    return "Update available.";
    }
    return "The update could not be completed.";
}
