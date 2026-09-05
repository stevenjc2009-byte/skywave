#include "update.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

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

// GitHub answers requests with no User-Agent with a 403, so this is not
// decoration.
#define USER_AGENT "Skywave/" SKYWAVE_VERSION " (Nintendo 3DS)"

// Generous on purpose. A release asset does not redirect to a tidy URL: it
// redirects to a signed one on release-assets.githubusercontent.com carrying a
// JWT and an expiry, which measured 900-odd characters when this was written.
// A buffer that merely looks big enough truncates it and the download fails
// with what looks exactly like the network being down.
#define URL_MAX      2048
#define MAX_HOPS     6      // github.com -> the signed asset host, plus slack
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

// ---------------------------------------------------------------------- http

static bool is_redirect(u32 status)
{
    return (status >= 301 && status <= 303) || (status >= 307 && status <= 308);
}

// Opens a GET and reads the status line. On success the context is left open
// and belongs to the caller; on failure nothing is left to close.
//
// Certificate verification is left ON here, unlike the stream path in
// source/net/http.c. What arrives here is an executable that the console will
// then install, so a connection nobody has authenticated is not good enough -
// if the handshake fails against GitHub the update has to fail loudly rather
// than quietly become an unverified download.
static bool http_begin(httpcContext *ctx, const char *url, u32 *status)
{
    if (R_FAILED(httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 1))) return false;

    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(ctx, "User-Agent", USER_AGENT);
    httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");

    if (R_FAILED(httpcBeginRequest(ctx)) ||
        R_FAILED(httpcGetResponseStatusCode(ctx, status))) {
        httpcCloseContext(ctx);
        return false;
    }
    return true;
}

// httpc does not follow redirects itself, so each hop is opened by hand.
// Leaves `ctx` open on the response that finally carried a body.
static bool http_get_following(httpcContext *ctx, const char *url, u32 *status)
{
    char next[URL_MAX];
    snprintf(next, sizeof(next), "%s", url);

    for (int hop = 0; hop < MAX_HOPS; hop++) {
        if (!http_begin(ctx, next, status)) return false;
        if (!is_redirect(*status)) return true;

        bool got = R_SUCCEEDED(httpcGetResponseHeader(ctx, "Location",
                                                      next, sizeof(next)));
        httpcCloseContext(ctx);
        if (!got) return false;
    }
    return false;
}

// ---------------------------------------------------------------- the check

// Reads the newest release's tag out of the redirect /releases/latest answers
// with. The body is never downloaded.
static bool read_latest_tag(char *tag, size_t cap)
{
    httpcContext ctx;
    u32 status = 0;
    if (!http_begin(&ctx, URL_LATEST, &status)) return false;

    char loc[URL_MAX];
    bool got = is_redirect(status) &&
               R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc)));
    httpcCloseContext(&ctx);
    if (!got) return false;

    const char *marker = "/releases/tag/";
    const char *p = strstr(loc, marker);
    if (!p) return false;
    p += strlen(marker);
    if (*p == 'v' || *p == 'V') p++;

    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != '/' && i + 1 < cap) {
        tag[i] = p[i];
        i++;
    }
    tag[i] = 0;
    return i > 0;
}

// -------------------------------------------------------------- the install

// Streams an already-open response straight into a CIA install. The file is
// never held in RAM or written to the SD card first: there is no need for a
// copy, and a half-written one lying about after a lost connection is exactly
// the kind of thing a user would later try to install by hand.
static sw_update_t install_response(httpcContext *ctx)
{
    u32 done = 0, total = 0;
    if (R_FAILED(httpcGetDownloadSizeState(ctx, &done, &total)) || total == 0)
        return SW_UPDATE_ERR_DOWNLOAD;

    Handle cia = 0;
    if (R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &cia)))
        return SW_UPDATE_ERR_INSTALL;

    // Static rather than automatic: 32 KB of stack inside a call this deep is
    // how an app that works on a New 3DS crashes on an Old one.
    static u8 chunk[CHUNK_BYTES];
    u64 written = 0;

    for (;;) {
        u32 got = 0;
        Result rc = httpcDownloadData(ctx, chunk, sizeof(chunk), &got);

        if (got > 0) {
            u32 wrote = 0;
            if (R_FAILED(FSFILE_Write(cia, &wrote, written, chunk, got,
                                      FS_WRITE_FLUSH)) || wrote != got) {
                AM_CancelCIAInstall(cia);
                return SW_UPDATE_ERR_INSTALL;
            }
            written += got;
        }

        if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) continue;
        if (R_FAILED(rc)) {
            AM_CancelCIAInstall(cia);
            return SW_UPDATE_ERR_DOWNLOAD;
        }
        break;
    }

    // A connection that dies mid-transfer looks exactly like a finished one to
    // the loop above, so the length is checked before anything is committed.
    if (written != (u64)total) {
        AM_CancelCIAInstall(cia);
        return SW_UPDATE_ERR_DOWNLOAD;
    }

    if (R_FAILED(AM_FinishCiaInstall(cia))) return SW_UPDATE_ERR_INSTALL;
    return SW_UPDATE_INSTALLED;
}

static sw_update_t fetch_and_install(const char *tag)
{
    char url[URL_MAX];
    snprintf(url, sizeof(url), URL_ASSET_FMT, tag, tag);

    httpcContext ctx;
    u32 status = 0;
    // Distinct from the tag read's failure on purpose: GitHub has already been
    // reached by this point, so "could not reach GitHub" would send the user
    // off checking their wifi over what is actually a problem with the file.
    if (!http_get_following(&ctx, url, &status)) return SW_UPDATE_ERR_DOWNLOAD;

    if (status != 200) {
        httpcCloseContext(&ctx);
        // A release that exists but has no matching .cia is a different problem
        // from a network that is down, and the user can act on the difference.
        return status == 404 ? SW_UPDATE_ERR_NO_ASSET : SW_UPDATE_ERR_NET;
    }

    sw_update_t r = install_response(&ctx);
    httpcCloseContext(&ctx);
    return r;
}

// ---------------------------------------------------------------------- api

sw_update_t sw_update_run(char *tag, size_t tag_cap)
{
    if (tag && tag_cap) tag[0] = 0;

    // httpc is already running for the streams and the directory, so this
    // nests. httpcInit/httpcExit are reference-counted, which is why the pair
    // here is safe rather than something that tears the service out from under
    // whatever else is using it.
    if (R_FAILED(httpcInit(0))) return SW_UPDATE_ERR_NET;

    char latest[32] = {0};
    sw_update_t out;

    if (!read_latest_tag(latest, sizeof(latest))) {
        out = SW_UPDATE_ERR_NET;
    } else {
        if (tag && tag_cap) snprintf(tag, tag_cap, "%s", latest);
        out = version_cmp(latest, SKYWAVE_VERSION) > 0
                ? fetch_and_install(latest)
                : SW_UPDATE_CURRENT;
    }

    httpcExit();
    return out;
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
    }
    return "The update could not be completed.";
}
