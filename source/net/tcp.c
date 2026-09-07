#include "tcp.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Only for MBEDTLS_ERR_NET_SEND_FAILED / _RECV_FAILED, which the BIO callbacks
// below return to abort a cancelled transfer. tcp.h pulls in the ssl and x509
// headers but not this one, and none of net_sockets.c's own POSIX socket
// helpers are used - the 3DS BIO is hand-written against soc:U.
#include <mbedtls/net_sockets.h>

#include "../store/diag.h"

// devkitPro's examples all use these two values. The buffer must be aligned and
// is not part of the app's own heap accounting, so there is nothing to gain by
// shaving it.
#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

// Long enough for a slow station on a slow connection to answer, short enough
// that a dead host does not feel like a hang. The kernel's own TCP timeout is
// around seventy seconds, which is far too long to leave the player stuck.
#define CONNECT_TIMEOUT_MS 8000
#define HANDSHAKE_TIMEOUT_MS 12000

// How long swConnWrite will keep trying while making NO progress. This is an
// idle timeout, not a total one - every byte accepted resets it - because the
// only thing being written here is a request header of a few hundred bytes,
// and a peer that has not taken a single byte of that in eight seconds is not
// slow, it is broken or hostile.
//
// This existed as no timeout at all until an audit found the hole. Everything
// else that blocks in this file is bounded (CONNECT_TIMEOUT_MS,
// HANDSHAKE_TIMEOUT_MS, RESOLVE_TIMEOUT_MS, swConnRead's caller-supplied one),
// and the write path was the single exception. It mattered because two of its
// callers cannot be rescued from outside: swHttpGetText and
// swHttpGetTextBounded build a private SwHttp inside get_text(), so no other
// thread holds a handle and swHttpCancel has nothing to act on. A directory
// mirror that completed the TCP connect and then advertised a zero receive
// window would have pinned that thread forever - no timeout, no error, no
// cancel - which is the same shape as the blocking service call that once
// froze this console past the point where even HOME could exit it.
#define WRITE_IDLE_TIMEOUT_MS 8000

// DNS lookups are ordinarily tens of milliseconds, even over a slow or
// congested link - this only ever matters when the resolver is stuck, and
// there is no way to tell "slow" from "stuck" ahead of time (see resolve()
// below). Kept shorter than CONNECT_TIMEOUT_MS so a hung resolve does not
// cost the user more patience than a hung connect already would, on top of
// whichever comes after it.
#define RESOLVE_TIMEOUT_MS 5000

// How many abandoned resolver threads (see resolve() below) are allowed to be
// alive at once before a new resolve is refused outright rather than starting
// a thread that would just become one more of them.
//
// The number comes from this app's own budget, not a guess at a system-wide
// figure: cia/skywave.rsf sets HandleTableSize to 0x200 (512), and every
// thread this process creates costs one entry in that table for as long as it
// is alive - on top of the 4 KB stack noted at resolve()'s threadCreate call.
// The app's steady-state handle use (service handles for soc:U/ssl:C/hid/gsp/
// dsp/fs/apt, plus the audio and network worker threads and their sync
// objects) is nowhere near 512, so this cap is sized against the failure mode
// it exists to prevent, not against how many the table could theoretically
// still hold: eight abandoned threads is already far more than a user mashing
// play across a station list would produce before the earliest ones finish
// giving up on their own, so a ninth is a sign something is stuck, not a
// legitimate ninth station - and the cost of getting the number wrong low
// (32 KB of stack, eight handles) rather than high.
#define RESOLVE_MAX_OUTSTANDING 8

// While connecting or handshaking, how often to look at the cancel flag.
#define POLL_SLICE_MS 100

// The community convention, set by Luma3DS and followed by curl, 3ds-hbmenu and
// Universal-Updater. Checked first so a user can update the bundle without
// waiting for a Skywave release.
#define SD_CA_PATH "sdmc:/config/ssl/cacert.pem"
// Shipped inside the app, so verification works on a console that has never
// installed anything else.
#define ROMFS_CA_PATH "romfs:/cacert.pem"

static u32 *s_soc_buffer;
static bool s_soc_up;
static bool s_sslc_up;

static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool                     s_rng_up;

static mbedtls_x509_crt s_ca;
static bool             s_ca_up;
static bool             s_ca_tried;
static const char      *s_ca_source = "none";

// The About screen's answer to "which bundle would be used", worked out by
// looking for the files rather than by parsing them. Written only by the main
// thread, in probe_trust_store_source below.
static const char      *s_ca_probed;

// Guards s_ca / s_ca_up / s_ca_tried during the first-time load in
// ensure_trust_store. Initialised once in swNetInit, before any connection
// can exist to race it.
static LightLock s_ca_lock;

// Guards s_resolve_outstanding, the count of resolver threads (see
// resolve() below) that are currently alive - either still being waited on
// or already abandoned and running down on their own. Initialised once in
// swNetInit, same as s_ca_lock and for the same reason: a lock created by
// the call it guards would leave the window before it exists unprotected,
// and swConnOpen can be reached from a worker thread the moment init returns.
//
// A dedicated lock rather than reusing s_ca_lock or folding this into each
// job's own per-job lock: s_ca_lock protects an unrelated one-time load, and
// a job's lock is scoped to that one job, not to the file-wide count of all
// of them. A plain atomic increment/decrement was not enough either - the
// cap check has to be "is the count still below the limit, and if so take a
// slot", which is a check-then-act pair that needs to happen as one step or
// two callers could both see a free slot and both take it, which a bare
// atomic add cannot do on its own.
static LightLock s_resolve_lock;
// The count itself. Read and written only under s_resolve_lock - see resolve()
// and resolve_release_slot() below for the two places that change it.
static int s_resolve_outstanding;

// Failures that happen before any SwConn exists to hold them: the DRBG seed
// in swNetInit, and the trust store's own parse failure (the store is global,
// not per-connection - see ensure_trust_store). Anything that fails on behalf
// of a specific connection stores its code on that connection's last_error
// instead, so two connections failing on different threads cannot stomp on
// each other's error.
static int s_last_error;

static bool load_trust_store(void)
{
    mbedtls_x509_crt_init(&s_ca);

    // A partial parse is normal and fine: mbedtls returns the number of
    // certificates it could not parse as a positive value, and a bundle where
    // most parsed is still a working trust store. Only a negative result means
    // nothing usable came out.
    int rc = mbedtls_x509_crt_parse_file(&s_ca, SD_CA_PATH);
    if (rc >= 0 && s_ca.version != 0) { s_ca_source = SD_CA_PATH; s_ca_up = true; return true; }

    rc = mbedtls_x509_crt_parse_file(&s_ca, ROMFS_CA_PATH);
    if (rc >= 0 && s_ca.version != 0) { s_ca_source = ROMFS_CA_PATH; s_ca_up = true; return true; }

    s_last_error = rc;
    mbedtls_x509_crt_free(&s_ca);
    return false;
}

// Loaded on demand, not at startup.
//
// The bundle is 121 certificates and 189 KB of base64, and parsing it means
// decoding and building an X.509 structure for every one of them. On an Old 3DS
// at 268 MHz that is time the user would spend staring at a black screen before
// the app appears, for something only the updater ever uses - streams do not
// verify. So the cost is paid by the update check, which is already a network
// operation the user has chosen to wait for.
//
// Called from the connection path (swConnOpen, on the app's worker thread)
// and from the error-message / About-screen path (swNetHaveTrustStore, on the
// main thread) - so it IS reachable from two threads at once. mbedtls's
// x509_crt structures have no internal locking, and load_trust_store below
// builds one into the shared s_ca on the first call; two threads racing that
// first call would both mbedtls_x509_crt_init/parse_file the same linked
// list, which is heap corruption, not just a wrong result. The lock makes the
// first-time load atomic; every call after that is just a read of s_ca_up.
static bool ensure_trust_store(void)
{
    LightLock_Lock(&s_ca_lock);
    bool ok;
    if (s_ca_up) {
        ok = true;
    } else if (s_ca_tried) {
        ok = false;   // it failed once; it will fail the same way
    } else {
        s_ca_tried = true;
        ok = load_trust_store();
    }
    LightLock_Unlock(&s_ca_lock);
    return ok;
}

bool swNetInit(void)
{
    if (s_soc_up) return true;

    // Once only, here, and never inside ensure_trust_store itself - a lock
    // created by the call it guards would not cover the race it exists for.
    LightLock_Init(&s_ca_lock);
    // Same reasoning, for the same reason: see s_resolve_lock above.
    LightLock_Init(&s_resolve_lock);

    s_soc_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!s_soc_buffer) return false;

    if (R_FAILED(socInit(s_soc_buffer, SOC_BUFFERSIZE))) {
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        return false;
    }
    s_soc_up = true;

    // Not for TLS - for entropy. See the header: without this the DRBG is seeded
    // from an untouched buffer and says nothing is wrong.
    s_sslc_up = R_SUCCEEDED(sslcInit(0));
    if (!s_sslc_up) {
        socExit();
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        s_soc_up = false;
        return false;
    }

    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);

    static const char kPers[] = "skywave";
    int rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                   (const unsigned char *)kPers, sizeof(kPers) - 1);
    if (rc != 0) {
        s_last_error = rc;
        mbedtls_ctr_drbg_free(&s_drbg);
        mbedtls_entropy_free(&s_entropy);
        sslcExit();
        socExit();
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        s_soc_up = s_sslc_up = false;
        return false;
    }
    s_rng_up = true;
    return true;
}

void swNetExit(void)
{
    if (s_ca_up)   { mbedtls_x509_crt_free(&s_ca); s_ca_up = false; }
    s_ca_tried  = false;
    s_ca_probed = NULL;
    if (s_rng_up)  {
        mbedtls_ctr_drbg_free(&s_drbg);
        mbedtls_entropy_free(&s_entropy);
        s_rng_up = false;
    }
    if (s_sslc_up) { sslcExit(); s_sslc_up = false; }
    if (s_soc_up)  { socExit();  s_soc_up  = false; }
    if (s_soc_buffer) { free(s_soc_buffer); s_soc_buffer = NULL; }
}

bool swNetHaveTrustStore(void) { return ensure_trust_store(); }

// Which bundle the loader would pick, decided by looking for the files in the
// order load_trust_store tries them. Opening a file is one FS request; parsing
// one is 121 certificates, so this is the version the About screen can afford.
//
// The answer is kept because the About row is rebuilt on every frame, and even
// one sdmc open per frame is an IPC round trip for a string that cannot change
// while the app is running - the user cannot swap the SD card underneath it.
//
// What this reports is where the bundle would come FROM, not that it parses.
// If the SD file exists but is unusable the real load falls back to romfs, and
// swNetTrustStoreSource prefers the loaded answer once there is one, so the row
// corrects itself rather than staying wrong.
static const char *probe_trust_store_source(void)
{
    if (s_ca_probed) return s_ca_probed;

    FILE *f = fopen(SD_CA_PATH, "rb");
    if (f) { fclose(f); return (s_ca_probed = SD_CA_PATH); }

    f = fopen(ROMFS_CA_PATH, "rb");
    if (f) { fclose(f); return (s_ca_probed = ROMFS_CA_PATH); }

    return (s_ca_probed = "none");
}

const char *swNetTrustStoreSource(void)
{
    // Once the load has been attempted, its result is the truth - including the
    // "none" a bundle that would not parse leaves behind - so it wins over the
    // probe's guess.
    //
    // TryLock rather than Lock: this runs on the main thread every frame, and
    // the worker holds s_ca_lock for the whole of the 121-certificate parse.
    // Waiting for it here would hand the About screen exactly the freeze the
    // lazy load exists to avoid. Missing the lock costs at most a frame or two
    // of the probe's answer while that parse is in flight.
    const char *known = NULL;
    if (LightLock_TryLock(&s_ca_lock) == 0) {
        if (s_ca_tried) known = s_ca_source;
        LightLock_Unlock(&s_ca_lock);
    }

    return known ? known : probe_trust_store_source();
}

// Waits for the socket to become readable or writable, in slices, so the cancel
// flag is looked at regularly. Returns 1 ready, 0 timed out, -1 cancelled or
// error.
static int wait_ready(SwConn *c, bool want_write, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        if (c->cancelled) return -1;

        int slice = timeout_ms - waited;
        if (slice > POLL_SLICE_MS) slice = POLL_SLICE_MS;
        if (slice <= 0) return 0;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(c->fd, &set);

        struct timeval tv;
        tv.tv_sec  = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;

        int r = select(c->fd + 1, want_write ? NULL : &set,
                       want_write ? &set : NULL, NULL, &tv);
        if (r > 0)  return 1;
        if (r < 0 && errno != EINTR) return -1;

        waited += slice;
        if (waited >= timeout_ms) return 0;
    }
}

// mbedtls calls these for every record. They must never block, or the cancel
// flag would not be looked at until the peer sent something.
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    SwConn *c = (SwConn *)ctx;
    if (c->cancelled) return MBEDTLS_ERR_NET_SEND_FAILED;

    int n = (int)send(c->fd, buf, len, 0);
    if (n >= 0) return n;
    if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    SwConn *c = (SwConn *)ctx;
    if (c->cancelled) return MBEDTLS_ERR_NET_RECV_FAILED;

    int n = (int)recv(c->fd, buf, len, 0);
    if (n > 0)  return n;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;   // orderly close
    if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

// A literal IPv4 address needs no lookup at all. This is worth checking for
// on its own merits, not as an optimisation of the path below: libctru's
// getaddrinfo (2.7.0, services/soc/soc_getaddrinfo.c) does NOT special-case a
// numeric node - every call it makes is the same synchronous IPC to soc:U,
// literal or not - so without this, a plain IP address in a station URL would
// still pay for the round trip and the thread in resolve() below for an
// answer that inet_pton can work out by itself, in-process, immediately.
static bool numeric_addrinfo(const char *host, const char *port,
                             struct addrinfo *out, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) return false;

    addr->sin_family = AF_INET;
    addr->sin_port   = htons((uint16_t)atoi(port));   // port is always numeric here - see url.c

    memset(out, 0, sizeof(*out));
    out->ai_family   = AF_INET;
    out->ai_socktype = SOCK_STREAM;
    out->ai_addr     = (struct sockaddr *)addr;
    out->ai_addrlen  = sizeof(*addr);
    return true;
}

// Hands a real (non-numeric) lookup to its own thread and waits on that
// thread instead of on getaddrinfo directly, because getaddrinfo itself
// cannot be bounded or interrupted: it is a single svcSendSyncRequest of
// soc:U command 0x000F0106, whose four normal parameters are the node,
// service, hints and result lengths - there is no timeout among them, no
// socket to select() on, and libctru exposes nothing that interrupts a
// synchronous IPC. Once that call is made, it runs until the system resolver
// itself gives up, on its own schedule, and nothing in userland can shorten
// that or get it to notice a cancel.
//
// So this waits on the thread instead of the call: RESOLVE_TIMEOUT_MS and
// c->cancelled are checked every POLL_SLICE_MS, same cadence as wait_ready
// uses for the connect and handshake, and the moment either fires this
// returns without waiting for getaddrinfo to actually finish.
//
// The hazard in that is the abandoned thread: it is still in there, still
// going to write into whatever result buffer it was given, on its own time.
// If this function had passed it a stack pointer or anything else tied to
// this call's lifetime, an abandoned resolve would write into memory this
// function had already handed back and moved on from - a use-after-return,
// on a random schedule, for however long the real resolve takes to give up.
// Everything the thread touches is therefore heap-allocated as its own
// ResolveJob and owned EITHER by this function OR by the thread, never both,
// with which one decided under job->lock at the one moment that matters: the
// thread checks job->abandoned right after getaddrinfo returns, and this
// function sets it, under the same lock, only if the thread had not already
// finished. Whichever of those two things happens first wins the job and is
// the one that frees it; the other touches it once more (to read the
// verdict) and then never again. There is no interleaving where both sides
// free it, and none where either side reads it after the other has.
//
// The thread is created detached, so libctru frees its own Thread control
// object the moment resolve_thread returns - there is no threadJoin here on
// purpose, since joining would just be the same unbounded wait under another
// name. What detachment does NOT free is the thread's kernel resources while
// it is still parked inside svcSendSyncRequest: an abandoned resolve stays
// alive, using a thread slot and its stack, until the system resolver it is
// waiting on eventually answers or gives up on its own. That is a real, bounded-
// but-unknown-duration leak under repeated abandonment (e.g. the user retrying
// a dead station over and over faster than the system resolver times out), not
// an unlimited one - see the tcp.c change report for why this was judged an
// acceptable trade against the alternative, which is the console appearing dead.
//
// "Bounded but unknown" still needed a ceiling on how many of these can be
// alive together, since nothing here controls how fast the caller can be
// asked to abandon a new one - see RESOLVE_MAX_OUTSTANDING, s_resolve_lock
// and resolve_release_slot below for where that ceiling is kept and enforced.
typedef struct {
    LightLock        lock;
    // Set by this function, under lock, to tell the thread nobody is
    // listening for its answer any more. Read and written only under lock.
    bool             abandoned;
    // Set by the thread, under lock, once getaddrinfo has returned and
    // res/rc are both valid to read. Read without the lock only as a cheap
    // "not yet" peek to decide whether to bother taking the lock at all;
    // every actual read of res/rc happens after a lock/unlock pair, which is
    // what makes those two fields' values visible across the two threads.
    volatile bool    done;
    char            *host;   // the job's own copy, so it outlives this call
    char            *port;   // even if this call abandons the job and returns
    struct addrinfo  hints;
    struct addrinfo *res;
    int              rc;
} ResolveJob;

// Gives back the one s_resolve_outstanding slot the caller reserved before
// creating this thread (see resolve() below). Called exactly once per thread
// that actually got created, right before that thread's last return -
// never by resolve() itself once threadCreate has succeeded, since from that
// point on this thread is the only thing still deciding when it is done,
// whether that is the ordinary finish below or the abandoned-job cleanup.
static void resolve_release_slot(void)
{
    LightLock_Lock(&s_resolve_lock);
    s_resolve_outstanding--;
    LightLock_Unlock(&s_resolve_lock);
}

static void resolve_thread(void *arg)
{
    ResolveJob *job = (ResolveJob *)arg;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(job->host, job->port, &job->hints, &res);

    LightLock_Lock(&job->lock);
    if (job->abandoned) {
        // Nobody is coming back for this - we are the only owner left, so we
        // free everything: the result getaddrinfo just handed us (if any),
        // the host/port copies, and the job struct itself.
        LightLock_Unlock(&job->lock);
        if (rc == 0 && res) freeaddrinfo(res);
        free(job->host);
        free(job->port);
        free(job);
        resolve_release_slot();
        return;
    }
    job->res  = res;
    job->rc   = rc;
    job->done = true;
    LightLock_Unlock(&job->lock);
    // The waiter owns the job from here; this thread must not touch it again.
    // It does not own the outstanding-count slot, though - that was reserved
    // for this thread's whole lifetime, not for the job's, so it is released
    // here regardless of which side ends up freeing the job.
    resolve_release_slot();
}

static SwConnResult resolve(SwConn *c, const char *host, const char *port,
                            const struct addrinfo *hints, struct addrinfo **out_res)
{
    // Reserve a slot before doing anything else: checking the count and
    // taking it happen under the one lock, so two callers racing this can
    // never both see room and both proceed. Everything below this point,
    // up to and including a successful threadCreate, must give the slot
    // back itself if it bails out - once the thread exists, the slot is its
    // to release, not resolve()'s (see resolve_release_slot above).
    LightLock_Lock(&s_resolve_lock);
    if (s_resolve_outstanding >= RESOLVE_MAX_OUTSTANDING) {
        LightLock_Unlock(&s_resolve_lock);
        return SW_CONN_ERR_INTERNAL;
    }
    s_resolve_outstanding++;
    LightLock_Unlock(&s_resolve_lock);

    ResolveJob *job = (ResolveJob *)calloc(1, sizeof(*job));
    if (!job) { resolve_release_slot(); return SW_CONN_ERR_INTERNAL; }

    job->host = strdup(host);
    job->port = strdup(port);
    if (!job->host || !job->port) {
        free(job->host);
        free(job->port);
        free(job);
        resolve_release_slot();
        return SW_CONN_ERR_INTERNAL;
    }
    job->hints = *hints;
    LightLock_Init(&job->lock);

    // Same priority as whoever is asking, so this thread is scheduled the
    // way the caller's own work already is; it spends essentially all of its
    // life blocked in the kernel waiting on soc:U, not competing for CPU.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    // 4 KB: this thread's whole call depth is getaddrinfo plus its own
    // cleanup, nothing else - the same call already runs today inside
    // net_main's 16 KB stack (source/audio/player.c) alongside everything
    // else that thread does, so on its own it needs a small fraction of that.
    Thread t = threadCreate(resolve_thread, job, 4096, prio, -2, true);
    if (!t) {
        free(job->host);
        free(job->port);
        free(job);
        resolve_release_slot();
        return SW_CONN_ERR_INTERNAL;
    }

    int waited = 0;
    for (;;) {
        if (job->done || c->cancelled || waited >= RESOLVE_TIMEOUT_MS) {
            LightLock_Lock(&job->lock);
            bool finished = job->done;
            struct addrinfo *res = finished ? job->res : NULL;
            int              rc  = finished ? job->rc  : 0;
            if (!finished) job->abandoned = true;
            LightLock_Unlock(&job->lock);

            if (!finished) {
                // Lost the race, or genuinely timed out/cancelled: the
                // thread owns the job now. Touch nothing further on it.
                return c->cancelled ? SW_CONN_ERR_CANCELLED : SW_CONN_ERR_RESOLVE;
            }

            free(job->host);
            free(job->port);
            free(job);
            if (rc != 0 || !res) return SW_CONN_ERR_RESOLVE;
            *out_res = res;
            return SW_CONN_OK;
        }

        svcSleepThread((s64)POLL_SLICE_MS * 1000000);
        waited += POLL_SLICE_MS;
    }
}

// Resolves and connects without blocking for the kernel's full TCP timeout.
static SwConnResult tcp_connect(SwConn *c, const char *host, const char *port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;          // the console has no IPv6 stack
    hints.ai_socktype = SOCK_STREAM;

    // Checked before starting a resolve at all, same as it always was: a
    // cancel that arrived while the connection was being set up should not
    // start one. What happens after this point is now bounded either way -
    // see numeric_addrinfo and resolve above.
    if (c->cancelled) return SW_CONN_ERR_CANCELLED;

    struct addrinfo   *res = NULL;
    struct addrinfo    numeric_ai;
    struct sockaddr_in numeric_addr;
    bool                res_is_heap;

    if (numeric_addrinfo(host, port, &numeric_ai, &numeric_addr)) {
        res = &numeric_ai;
        res_is_heap = false;
    } else {
        SwConnResult rr = resolve(c, host, port, &hints, &res);
        if (rr != SW_CONN_OK) return rr;
        res_is_heap = true;
    }

    if (c->cancelled) {
        if (res_is_heap) freeaddrinfo(res);
        return SW_CONN_ERR_CANCELLED;
    }

    SwConnResult out = SW_CONN_ERR_CONNECT;

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            // Recorded because this failure is otherwise invisible. If every
            // address fails here, `out` is still the initialised
            // SW_CONN_ERR_CONNECT and last_error is still 0, so a soc:U handle
            // or buffer exhaustion reaches the user as "Could not reach this
            // station. (0)" - indistinguishable from a station that is simply
            // down. The errno is the only thing that tells them apart.
            swDiagf("socket() failed for %s:%s, errno=%d", host, port, errno);
            continue;
        }

        // Checked, unlike almost any other F_SETFL call, because everything
        // this function does to avoid hanging depends on it having worked. If
        // the socket is left blocking, the connect() below stops being a
        // pollable operation and becomes the kernel's own ~70-second TCP
        // timeout on the calling thread - the exact stall CONNECT_TIMEOUT_MS
        // exists to prevent, and it would happen with no error anywhere. Better
        // to abandon this address and try the next one from getaddrinfo().
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }

        c->fd = fd;
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);

        if (rc == 0) { out = SW_CONN_OK; break; }

        if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
            int ready = wait_ready(c, true, CONNECT_TIMEOUT_MS);
            if (ready == 1) {
                // select() says writable for both success and refusal; only
                // SO_ERROR distinguishes them.
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                    out = SW_CONN_OK;
                    break;
                }
            } else if (ready < 0) {
                out = SW_CONN_ERR_CANCELLED;
                close(fd);
                c->fd = -1;
                break;
            }
        }

        close(fd);
        c->fd = -1;
    }

    if (res_is_heap) freeaddrinfo(res);
    return out;
}

// The body of swConnOpen. Split out so the public entry point below can record
// one line per attempt from a single place, rather than needing a swDiagf at
// each of this function's nine exits - nine chances to put one on the wrong
// branch, or to miss one entirely.
static SwConnResult conn_open(SwConn *c, const char *host, const char *port,
                              bool tls, bool verify)
{
    if (!c || !host || !port) return SW_CONN_ERR_INTERNAL;

    // Defensive: if this SwConn is somehow still holding a live connection -
    // a caller reopening without closing first - the memset below would drop
    // the fd and the mbedtls contexts on the floor instead of releasing them.
    if (c->open) swConnClose(c);

    bool was_cancelled = c->cancelled;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->cancelled = was_cancelled;   // a cancel that arrived first still counts

    if (!s_soc_up || !s_rng_up) return SW_CONN_ERR_INTERNAL;

    // With verification asked for and no trust store to verify against, this
    // fails rather than quietly connecting to whoever answers. The trust
    // store's own parse error lives in s_last_error (it is not tied to any
    // one connection); copy it onto this connection so swConnLastError can
    // still report it.
    if (tls && verify && !ensure_trust_store()) {
        c->last_error = s_last_error;
        return SW_CONN_ERR_CERT;
    }

    SwConnResult r = tcp_connect(c, host, port);
    if (r != SW_CONN_OK) return r;

    if (!tls) { c->open = true; return SW_CONN_OK; }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);

    int rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) goto tls_failed;

    if (verify) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->conf, &s_ca, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &s_drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) goto tls_failed;

    // Server Name Indication. Not optional in practice: every CDN in the station
    // list serves many hosts from one address and answers with the wrong
    // certificate, or refuses outright, without it.
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) goto tls_failed;

    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

    int waited = 0;
    for (;;) {
        rc = mbedtls_ssl_handshake(&c->ssl);
        if (rc == 0) break;

        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto tls_failed;

        int ready = wait_ready(c, rc == MBEDTLS_ERR_SSL_WANT_WRITE, POLL_SLICE_MS);
        if (ready < 0) {
            mbedtls_ssl_free(&c->ssl);
            mbedtls_ssl_config_free(&c->conf);
            if (c->fd >= 0) { close(c->fd); c->fd = -1; }
            return SW_CONN_ERR_CANCELLED;
        }

        waited += POLL_SLICE_MS;
        if (waited >= HANDSHAKE_TIMEOUT_MS) { rc = MBEDTLS_ERR_SSL_TIMEOUT; goto tls_failed; }
    }

    if (verify) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (flags != 0) {
            c->last_error = (int)flags;
            mbedtls_ssl_free(&c->ssl);
            mbedtls_ssl_config_free(&c->conf);
            close(c->fd);
            c->fd = -1;
            return SW_CONN_ERR_CERT;
        }
    }

    c->tls  = true;
    c->open = true;
    return SW_CONN_OK;

tls_failed:
    c->last_error = rc;
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    return SW_CONN_ERR_TLS;
}

SwConnResult swConnOpen(SwConn *c, const char *host, const char *port,
                        bool tls, bool verify)
{
    SwConnResult r = conn_open(c, host, port, tls, verify);

    // One line per connection attempt, carrying the three things that cannot be
    // recovered from a screenshot afterwards: which host, whether the TLS path
    // was entered at all, and the numeric reason. The directory fetch is always
    // plain HTTP and a station stream may be either, so "tls=" is what
    // distinguishes the two in a log.
    swDiagf("conn %s:%s tls=%d verify=%d -> result=%d last_error=%d",
            host ? host : "(null)", port ? port : "(null)",
            (int)tls, (int)verify, (int)r, c ? c->last_error : 0);

    return r;
}

int swConnLastError(const SwConn *c)
{
    if (!c) return 0;
    return c->last_error;
}

bool swConnWrite(SwConn *c, const void *buf, size_t len)
{
    if (!c || !c->open) return false;

    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    int stalled = 0;

    while (sent < len) {
        if (c->cancelled) return false;

        int n;
        if (c->tls) n = mbedtls_ssl_write(&c->ssl, p + sent, len - sent);
        else        n = bio_send(c, p + sent, len - sent);

        if (n > 0) { sent += (size_t)n; stalled = 0; continue; }

        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (wait_ready(c, n == MBEDTLS_ERR_SSL_WANT_WRITE, POLL_SLICE_MS) < 0)
                return false;

            // Counted per attempt rather than per elapsed millisecond, the same
            // way the TLS handshake loop above does it. If wait_ready returns
            // early the count runs slightly ahead of the wall clock, which errs
            // towards giving up sooner on a peer that is reporting itself
            // writable and then refusing every byte - a state no working server
            // reaches. Progress resets it, so a genuinely slow link is never
            // punished for being slow.
            stalled += POLL_SLICE_MS;
            if (stalled >= WRITE_IDLE_TIMEOUT_MS) return false;
            continue;
        }
        return false;
    }
    return true;
}

int swConnRead(SwConn *c, void *buf, size_t cap, int timeout_ms)
{
    if (!c || !c->open || cap == 0) return -1;
    if (c->cancelled) return -1;

    // Try first without waiting. On a TLS connection mbedtls may already hold a
    // decrypted record, in which case waiting on the socket would sit there
    // while the data the caller wants is in memory.
    for (;;) {
        int n;
        if (c->tls) n = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, cap);
        else        n = bio_recv(c, (unsigned char *)buf, cap);

        if (n > 0) return n;

        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;  // ended

        if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
            c->last_error = n;
            return -1;
        }

        int ready = wait_ready(c, n == MBEDTLS_ERR_SSL_WANT_WRITE, timeout_ms);
        if (ready < 0) return -1;      // cancelled or socket error
        if (ready == 0) return 0;      // nothing this window; not a failure
        // readable now - go round and actually read it
        timeout_ms = 0;                // and do not wait a second time
    }
}

void swConnCancel(SwConn *c)
{
    if (!c) return;
    c->cancelled = true;

    // Waking the socket as well as setting the flag. The flag alone is enough
    // for this app's own polling loops, but shutdown() also unsticks a read that
    // is already inside recv(), which is the case that matters when a station
    // goes quiet mid-song.
    //
    // The barrier publishes `cancelled` before this thread reads the other
    // thread's `open`/`fd`, which are plain fields - only `cancelled` is
    // volatile - so on a second core there is otherwise no ordering between the
    // two at all. `fd` is then read ONCE into a local: reading it twice would
    // let swConnClose null it between the test and the use, and shutdown() on a
    // descriptor number the system has already reissued would tear down some
    // unrelated live connection with no error anywhere. This narrows the window
    // to the interval inside swConnClose between `open = false` and `fd = -1`;
    // closing it completely would need a lock on the close path, which is not
    // worth taking on every teardown for a race whose remaining worst case is
    // the harmless shutdown(-1) the ordering was designed to produce.
    __dmb();

    int fd = c->open ? c->fd : -1;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

void swConnClose(SwConn *c)
{
    if (!c || !c->open) return;

    // `open` is cleared first, and the descriptor is taken out of the struct
    // before it is closed. Both matter because swConnCancel runs on a different
    // thread: this ordering means the worst a cancel racing a close can do is
    // shutdown(-1), which fails harmlessly, rather than shutting down a
    // descriptor number the system has already handed to somebody else.
    if (c->tls) mbedtls_ssl_close_notify(&c->ssl);   // while the socket is still live

    c->open = false;
    __dmb();            // let a cancel on the other core see `open` go false
                        // before the descriptor it would have used disappears
    int fd = c->fd;
    c->fd  = -1;

    if (c->tls) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
    }
    if (fd >= 0) close(fd);
    c->tls = false;
}
