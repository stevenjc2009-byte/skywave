#include "player.h"

#include <3ds.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ring.h"
#include "aac_bridge.h"
#include "../net/http.h"
#include "../net/icy.h"
#include "../store/diag.h"

// ---------------------------------------------------------------- constants

// Eight seconds of a 128 kbps stream. This is the whole reason playback
// survives a wifi wobble: the decoder keeps eating out of here while the
// network thread has nothing to give it. Must stay a power of two - Ring
// masks rather than divides, which is what makes it lock-free.
#define RING_BYTES     131072

// What one swHttpRead call asks for off the socket. Small enough that the stop
// flag is checked often (a 128 kbps stream delivers this in a quarter of a
// second) and large enough not to be all syscall overhead.
#define NET_CHUNK      4096

// Four DSP buffers of 32 KB. At 44.1 kHz stereo that is 0.19 s each, so the DSP
// always has most of a second queued ahead of it - enough that a slow decode
// pass cannot produce a click.
#define WBUF_COUNT     4
#define WBUF_BYTES     32768

#define FEED_CHUNK     4096      // ring -> decoder, per call

// Prebuffer is computed from the station's own bitrate rather than fixed,
// because a fixed byte count means three seconds at 128 kbps and half a minute
// at 24 kbps. Clamped so a station that lies about its bitrate cannot ask for
// more than the ring holds.
#define PREBUFFER_SECS 3
#define PREBUFFER_MIN  24576
#define PREBUFFER_MAX  (RING_BYTES / 2)

// ------------------------------------------------------------------- state

// Which decoder the current station's bytes belong to. Decided once per
// station, the moment the first audio bytes are in hand - see fill()'s
// comment further down for why that has to be by sniffing rather than at
// connect time.
typedef enum {
    SW_CODEC_UNKNOWN = 0,   // not decided yet; no audio bytes seen yet
    SW_CODEC_MP3,
    SW_CODEC_AAC
} SwCodec;

typedef struct {
    bool  inited;

    // DSP side
    ndspWaveBuf   wbuf[WBUF_COUNT];
    u8           *wbuf_mem[WBUF_COUNT];
    int           rate;             // what the channel is currently set to
    int           channels;
    int           volume;           // 0..100

    // Decoder
    mpg123_handle *mh;
    AacBridge     *ab;              // Helix AAC wrapper - see aac_bridge.h
    u8            *feed;            // ring -> decoder staging, heap not stack
    SwCodec        codec;           // which of the two is decoding this station
    size_t         feed_len;        // leftover bytes already pulled out of the
                                     // ring by codec sniffing, not yet decoded
    size_t         aac_dry;         // bytes fed to Helix since it last produced
                                     // any PCM at all - see AAC_UNDECODABLE_BYTES
    size_t         mp3_dry;         // the same patience counter for mpg123 - see
                                     // MP3_UNDECODABLE_BYTES

    // Transport
    SwHttp     http;

    // The register-play ping gets its own handle rather than borrowing g.http,
    // which is still open and being read when the ping fires, and rather than a
    // local, because SwHttp is 17,584 bytes and the thread that fires it has a
    // 16 KB stack (see swDirRegisterPlayH in directory.h). Living here is also
    // what lets swPlayerStop reach it to cancel it - see net_main.
    SwHttp     ping;

    Ring       ring;
    u8        *ring_mem;
    IcyDemux   demux;
    u8        *net_in;              // raw bytes off the socket
    u8        *net_out;             // the same bytes with metadata stripped

    // Threads
    Thread  net_thread;
    Thread  dec_thread;
    volatile bool running;          // both threads exit when this clears

    // Shared, small, read by the UI every frame
    volatile SwPlayState state;
    volatile int         prebuffer; // bytes wanted before sound starts
    volatile bool        registered;// told the directory this station played

    SwStation  station;
    bool       have_station;

    // Both threads write `state`, and one of the values means something the
    // other must not undo - see advance_state below for the whole story.
    LightLock  state_lock;

    LightLock  text_lock;           // guards `title` and `error`
    char       title[ICY_TITLE_MAX];
    char       error[96];
} Player;

static Player g;

// ---------------------------------------------------------------- utilities

static void set_error(const char *msg)
{
    // Every route into SW_PLAY_ERROR goes through here, so this one line
    // records all of them - and records them even when the message ends up
    // empty, which is the case that put "Stopped" on screen with nothing under
    // it and no way to tell what had happened.
    swDiagf("ERROR: '%s'", msg ? msg : "(null)");

    LightLock_Lock(&g.text_lock);
    snprintf(g.error, sizeof(g.error), "%s", msg);
    LightLock_Unlock(&g.text_lock);

    // Under state_lock so it cannot be lost to the decode thread's own
    // progression - see advance_state. The text is written first and outside
    // this lock so that the moment the UI sees SW_PLAY_ERROR the reason is
    // already there to print.
    LightLock_Lock(&g.state_lock);
    g.state = SW_PLAY_ERROR;
    LightLock_Unlock(&g.state_lock);
}

// Moves `state` from `from` to `to`, and only from `from`. Returns whether it
// moved.
//
// This exists for the decode thread, which walks buffering -> playing ->
// buffering on its own and knows nothing about the network thread giving up on
// the station in between. It used to read `state` into a local, then act on
// that local several calls later - a ringUsed() and an ndspChnIsPlaying() is a
// wide window on the other core - and write the result back unconditionally. A
// station that died during that window had its SW_PLAY_ERROR overwritten with
// "playing" or "buffering": the real reason was sitting in g.error, already
// written, and the user watched a dead station buffer for ever instead of
// being told. Reading and writing under one lock makes it a single decision on
// one value rather than two decisions on two, and set_error takes the same
// lock, so there is no ordering in which the loser gets to win.
//
// An error is terminal for the current station by design. Only swPlayerPlay and
// swPlayerStop clear it, and both of them run with these threads already gone.
static bool advance_state(SwPlayState from, SwPlayState to)
{
    bool moved = false;

    LightLock_Lock(&g.state_lock);
    if (g.state == from) { g.state = to; moved = true; }
    LightLock_Unlock(&g.state_lock);

    return moved;
}

static void clear_text(void)
{
    LightLock_Lock(&g.text_lock);
    g.title[0] = 0;
    g.error[0] = 0;
    LightLock_Unlock(&g.text_lock);
}

static void apply_volume(void)
{
    // ndsp mixes with a 12-entry matrix; the first two are the front pair and
    // the rest are surround/aux outputs this app never uses.
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = (float)g.volume / 100.0f;
    ndspChnSetMix(0, mix);
}

// Reconfigures the DSP channel to whatever the decoder says the stream is.
// Called on MPG123_NEW_FORMAT, which fires once at the start of a stream and
// again if a station splices in an ad at a different sample rate.
static void set_format(long rate, int channels)
{
    if (rate == g.rate && channels == g.channels) return;

    g.rate     = (int)rate;
    g.channels = channels;

    ndspChnSetRate(0, (float)rate);
    ndspChnSetFormat(0, channels == 1 ? NDSP_FORMAT_MONO_PCM16
                                      : NDSP_FORMAT_STEREO_PCM16);
}

static int bytes_per_frame(void)
{
    return g.channels * 2;          // 16-bit samples
}

// ------------------------------------------------------------ network thread

// How long a station may hold the connection open without ever giving us enough
// audio to start playing.
//
// Added in v1.0.4. Until now the only thing that could end a connected stream
// was a read error: swHttpRead returning 0 is normal (a stream is paced by the
// clock, so "nothing ready yet" is the usual answer), so a server that accepted
// the connection and then trickled - or sent a slow drip that never reached the
// three-second cushion - buffered forever. No error, no timeout, and no way for
// a user to tell it apart from their own WiFi being slow. swHttpRead's own
// BODY_IDLE_MS only fires on a total stall between bytes; this is the
// cumulative case it cannot see.
//
// Measured against the real thing before choosing the number: Capital's 128 kbps
// MP3 mount delivered 371,947 bytes in 8 seconds from a cold start, because
// these servers burst a priming buffer before settling into real time. A healthy
// station therefore clears a 24-65 KB prebuffer in well under a second, and 20
// seconds is far beyond any honest slow start.
#define STALL_MS 20000

static void net_main(void *arg)
{
    (void)arg;

    g.state = SW_PLAY_CONNECTING;

    swDiagf("net thread: opening %s", g.station.url);

    if (swHttpOpenStream(&g.http, g.station.url) != SW_HTTP_OK) {
        // The specific reason, not a generic one: on a console this line is the
        // whole diagnostic record, and "could not connect" is unreportable.
        char why[sizeof(g.error)];
        swHttpErrorText(&g.http, why, sizeof(why));

        // Close even though the open failed, and BEFORE reporting, because
        // swHttpClose is the only thing in the codebase that clears
        // conn.cancelled - open_common deliberately preserves it (a cancel
        // aimed at the connection about to be opened must not be erased by the
        // open itself) and swConnOpen carries it across its own memset. So the
        // flag is sticky by design, and the one place it gets cleared is the
        // owning thread finishing with the connection. That is exactly here.
        //
        // Without this, the flag survives on g.http for the rest of the
        // session, and g.http is reused for every station. The way it gets set
        // is ordinary: swPlayerStop calls swHttpCancel(&g.http) to unblock this
        // thread, which is precisely what a user pressing Stop or Back while
        // the screen still says "Connecting..." does. That cancel makes the
        // open fail, this branch runs, and the old `return` left cancelled
        // true - so the next station's swHttpOpenStream aborted in swConnOpen
        // before it opened a socket, and so did every station after it. One
        // stop during a slow connect and playback was dead until the app was
        // restarted, with an error that names the symptom and not the cause.
        //
        // swHttpClose on a handle whose connection is already down is
        // documented safe and does not touch err/rc/status, which is why the
        // message is taken above and is still accurate below.
        swHttpClose(&g.http);

        set_error(why);
        return;
    }

    swDiagf("stream open ok: metaint=%d icy-br=%d dir-br=%d",
            (int)g.http.metaint, g.http.bitrate, g.station.bitrate);

    // metaint of 0 means the station sends no metadata, which icyFeed handles
    // as a pure passthrough. Nothing special to do here either way.
    icyInit(&g.demux, g.http.metaint);

    // Size the prebuffer from what the station says it sends. The directory's
    // figure is the fallback for stations that omit icy-br.
    int kbps = g.http.bitrate > 0 ? g.http.bitrate : g.station.bitrate;
    if (kbps <= 0) kbps = 128;
    int want = (kbps * 1000 / 8) * PREBUFFER_SECS;
    if (want < PREBUFFER_MIN) want = PREBUFFER_MIN;
    if (want > PREBUFFER_MAX) want = PREBUFFER_MAX;
    g.prebuffer = want;

    // Publish the size before the state that makes the decode thread go and
    // read it. This is the same discipline ring.c uses around head/tail (its
    // RING_BARRIER is exactly this __dmb; the macro is private to that file
    // because it also has to compile away in the host test build, which never
    // sees player.c) and it is here for exactly the same reason: `volatile`
    // orders the compiler, not the two physical ARM11s. The decode thread is
    // on core 0 and this is core 1, so without this it can observe
    // SW_PLAY_BUFFERING while still holding the old g.prebuffer - the
    // PREBUFFER_MIN swPlayerPlay left there - and start playing on 24 KB of
    // cushion instead of the three seconds this just computed.
    __dmb();

    g.state = SW_PLAY_BUFFERING;

    swDiagf("prebuffer=%d bytes, state=BUFFERING", want);

    u64 buffering_since = osGetTime();

    // First-read and first-play markers only. Logged once each per tune-in, not
    // per iteration - a line per read would be thousands of file writes a
    // minute and would itself become the reason the stream could not keep up.
    bool logged_first_read = false;
    bool logged_playing    = false;

    while (g.running) {
        // Reset the clock whenever audio is actually flowing, so this only ever
        // measures an unbroken stretch of not-playing. A station that plays and
        // then rebuffers mid-song gets the full allowance again rather than
        // inheriting time already spent.
        if (g.state == SW_PLAY_PLAYING) {
            buffering_since = osGetTime();
            if (!logged_playing) {
                logged_playing = true;
                swDiagf("state=PLAYING (audio is being sent to the DSP)");
            }
        } else if (osGetTime() - buffering_since > STALL_MS) {
            swDiagf("STALL: %d ms without reaching PLAYING, ring=%u/%d",
                    STALL_MS, (unsigned)ringUsed(&g.ring), g.prebuffer);
            set_error("This station is not sending audio fast enough to play.");
            break;
        }

        int n = swHttpRead(&g.http, g.net_in, NET_CHUNK);

        if (n > 0 && !logged_first_read) {
            logged_first_read = true;
            swDiagf("first %d bytes read from the station", n);
        }

        if (n < 0) {
            // Only report it if we are stopping for the station's reasons
            // rather than because the user pressed stop - swHttpCancel makes
            // a normal stop look exactly like a failed read.
            if (g.running) set_error("The station stopped sending.");
            break;
        }
        if (n == 0) {
            // A stream is paced by the clock, so nothing ready is normal.
            svcSleepThread(5 * 1000 * 1000ULL);
            continue;
        }

        size_t audio = icyFeed(&g.demux, g.net_in, (size_t)n, g.net_out);

        char t[ICY_TITLE_MAX];
        if (icyTakeTitle(&g.demux, t, sizeof(t))) {
            LightLock_Lock(&g.text_lock);
            snprintf(g.title, sizeof(g.title), "%s", t);
            LightLock_Unlock(&g.text_lock);
        }

        // Every audio byte must reach the ring: MP3 is a chain of frames and a
        // dropped run does not resynchronise cleanly, it makes a noise. So when
        // the ring is full this waits rather than discarding - the decoder is
        // draining it at a fixed rate, so the wait is always short.
        size_t written = 0;
        while (written < audio && g.running) {
            written += ringWrite(&g.ring, g.net_out + written, audio - written);
            if (written < audio) svcSleepThread(5 * 1000 * 1000ULL);
        }

        // Tell the directory the station was played, once, and only after it
        // has actually proven it works. Done here rather than at connect time
        // so that a round trip to the API never delays the first sound.
        //
        // This is a synchronous HTTP round trip to a mirror that may be slow or
        // may never answer, made on the very thread swPlayerStop then blocks on
        // in threadJoin(U64_MAX). Through swDirRegisterPlay - whose handle is
        // private to itself and reachable by nobody - a stop landing here had
        // nothing to cancel: the join waited the ping out, and a mirror that
        // stalled froze the whole app with no way out, not even HOME. So the
        // ping goes through swDirRegisterPlayH on g.ping, and it is guarded
        // twice over, because one guard alone is not enough:
        //
        //   * g.running immediately before, so a stop already in flight skips
        //     the ping outright rather than starting one nobody wants. The
        //     courtesy of a play count is worth nothing next to a stop that
        //     answers, and this is the common case.
        //   * swPlayerStop cancels &g.ping alongside &g.http, for the gap
        //     between that check and the connect. A cancel that arrives before
        //     the open survives it (swConnOpen keeps a flag that arrived
        //     first), and one that arrives during unblocks the socket.
        if (!g.registered && ringUsed(&g.ring) >= (size_t)g.prebuffer) {
            g.registered = true;

            if (!g.running) break;      // stop pending: don't start the ping
            swDirRegisterPlayH(&g.ping, g.station.uuid);
            if (!g.running) break;      // stop landed while it was in flight
        }
    }

    swDiagf("net thread exiting (running=%d, state=%d)", (int)g.running, (int)g.state);

    swHttpClose(&g.http);
}

// ------------------------------------------------------------- decode thread

// Pulls the next chunk of audio bytes to decode: whatever codec-sniffing
// already pulled out of the ring and hasn't been consumed yet (see `fill`
// below), or a fresh read off the ring otherwise. Centralised so neither
// decode path can forget the leftover-from-sniffing case.
static size_t next_chunk(void)
{
    if (g.feed_len) {
        size_t got = g.feed_len;
        g.feed_len = 0;
        return got;
    }
    return ringRead(&g.ring, g.feed, FEED_CHUNK);
}

// Decodes into one DSP buffer until it is full or the ring runs dry.
// Unchanged from before AAC support existed, other than pulling its input
// through next_chunk() instead of calling ringRead() directly, so that the
// one chunk codec-sniffing already consumed from the ring for THIS station
// is not silently dropped. That substitution is a no-op for every call after
// the first: next_chunk() falls straight through to the same ringRead() this
// function used to call itself.
// The MP3 half of AAC_UNDECODABLE_BYTES, and it exists for the same reason.
//
// Added in v1.0.4. fill_aac() got its counter because an AAC stream Helix could
// not decode would sit on "Buffering..." forever without ever saying why - but
// mpg123 was left with no equivalent, on the assumption that MP3 is the safe,
// well-understood path. It is not safe: the codec is SNIFFED (see pick_codec
// below), and a station the sniffer calls MP3 wrongly - an AAC stream with a
// misleading first frame, an HLS playlist, an HTML error page served with an
// audio content-type - lands here and never decodes. mpg123 answers
// MPG123_NEED_MORE to all of it, which this loop treats as "keep going", so the
// failure was indistinguishable from a slow connection: downloading forever,
// decoding nothing, reporting nothing.
//
// Same 256 KB as the AAC side and for the same arithmetic: ~16 seconds of a
// 128 kbps stream, far more than a genuine mid-stream resync needs and far less
// than a user will sit staring at a progress bar.
#define MP3_UNDECODABLE_BYTES (256 * 1024)

static size_t fill_mp3(u8 *out, size_t cap)
{
    size_t done_total = 0;

    for (;;) {
        size_t done = 0;
        int r = mpg123_decode(g.mh, NULL, 0, out + done_total, cap - done_total, &done);
        done_total += done;

        if (r == MPG123_NEW_FORMAT) {
            long rate; int ch, enc;
            mpg123_getformat(g.mh, &rate, &ch, &enc);

            // Whatever is already in this buffer is in the old format. Hand it
            // over as it is and start the next buffer under the new one, rather
            // than gluing two formats into one wave buffer.
            set_format(rate, ch);
            if (done_total) return done_total;
            continue;
        }

        if (r != MPG123_NEED_MORE && r != MPG123_OK) return done_total;
        if (done_total >= cap) return done_total;

        // Out of decoded audio: top the decoder up from the ring.
        size_t got = next_chunk();
        if (got == 0) return done_total;   // underrun; caller decides what to do

        size_t ignored = 0;
        int fr = mpg123_decode(g.mh, g.feed, got, out + done_total,
                               cap - done_total, &ignored);
        done_total += ignored;

        // Patience, measured in input bytes for the same reason as the AAC side:
        // it is the only unit that scales alike for a 24 kbps stream and a 320
        // kbps one. `ignored` is this call's PCM output, so progress on either
        // decode above resets it.
        if (done > 0 || ignored > 0) {
            g.mp3_dry = 0;
        } else {
            g.mp3_dry += got;
            if (g.mp3_dry >= MP3_UNDECODABLE_BYTES) {
                set_error("This station's audio format is not supported.");
                return done_total;
            }
        }

        if (fr == MPG123_NEW_FORMAT) {
            long rate; int ch, enc;
            mpg123_getformat(g.mh, &rate, &ch, &enc);
            set_format(rate, ch);
            if (done_total) return done_total;
        } else if (fr != MPG123_NEED_MORE && fr != MPG123_OK) {
            return done_total;
        }

        if (done_total >= cap) return done_total;
    }
}

// AAC equivalent of fill_mp3(), same buffer-filling contract, built on top of
// aac_bridge.h instead of mpg123. The two decoders announce a format change
// differently (see aac_bridge.h's header comment on why), which is why this
// cannot just be a branch inside fill_mp3(): every AACDEC_NEW_FORMAT return
// carries that frame's own PCM already in the new format, so it must be the
// last thing returned this call rather than something to keep looping past -
// and aacBridgeHasPending() has to be checked before ever calling
// aacBridgeDecode() again into the same `out` buffer, or a frame already
// queued up under a new format would get appended after old-format bytes
// still sitting earlier in this same wavebuf.
// How many bytes Helix may swallow without ever handing back a single sample
// before the stream is declared undecodable rather than merely slow to sync.
//
// This loop is NOT unbounded - next_chunk() always consumes from the ring, so
// fill_aac returns as soon as the ring runs dry - but without this counter an
// AAC stream Helix cannot decode never produces an error either. dec_main just
// keeps getting zero bytes back, so the player sits on "Buffering..." forever,
// downloading the whole time and never saying why. That matters more than
// usual here because no AAC stream has ever been decoded on real hardware, so
// an unsupported profile is a live possibility on the first AAC station a user
// tunes to, and "it just buffers forever" is the least diagnosable way for it
// to fail.
//
// 256 KB is about sixteen seconds of a 128 kbps stream: far more than the few
// KB a genuine mid-stream resync needs after a dropout, far less than the user
// spends wondering whether it is their WiFi.
#define AAC_UNDECODABLE_BYTES (256 * 1024)

static size_t fill_aac(u8 *out, size_t cap)
{
    size_t done_total = 0;

    for (;;) {
        if (done_total >= cap) return done_total;

        // A format change was found and deferred on an earlier iteration of
        // THIS call (see aac_bridge.h). Stop here and let the very next
        // fill_aac() call - a fresh wavebuf - pick it up from done_total==0,
        // the same hard boundary fill_mp3() gets from mpg123 itself.
        if (done_total && aacBridgeHasPending(g.ab)) return done_total;

        size_t got = next_chunk();
        if (got == 0 && !aacBridgeHasPending(g.ab)) return done_total;

        size_t done = 0;
        long rate; int channels;
        AacDecStatus st = aacBridgeDecode(g.ab, g.feed, got,
                                          (int16_t *)(out + done_total),
                                          cap - done_total, &done,
                                          &rate, &channels);

        if (st == AACDEC_NEW_FORMAT) {
            set_format(rate, channels);
            done_total += done;
            return done_total;
        }
        if (st == AACDEC_ERROR) return done_total;   // dead stream; caller decides

        // Progress resets the patience counter; a call that swallowed input and
        // returned no samples spends it. Measured in input bytes rather than in
        // iterations or seconds because it is the only one of the three that
        // scales the same way for a 24 kbps stream and a 320 kbps one.
        if (done > 0) {
            g.aac_dry = 0;
        } else {
            g.aac_dry += got;
            if (g.aac_dry >= AAC_UNDECODABLE_BYTES) {
                set_error("This station's audio format is not supported.");
                return done_total;
            }
        }

        done_total += done;
        // AACDEC_OK or AACDEC_NEED_MORE: loop and pull more from the ring.
    }
}

// Decides which codec this station is and dispatches to the matching fill_*.
// The decision is made once, the first time this station has any audio bytes
// to look at, and then remembered in g.codec until the next swPlayerPlay().
//
// Detection is by sniffing the bytes themselves (aacBridgeLooksLikeAdts:
// ADTS's 0xFFF sync with layer bits 00) rather than trusting the HTTP
// Content-Type header. Icecast/Shoutcast Content-Types for AAC streams are
// inconsistent in the wild (audio/aac, audio/aacp, application/octet-stream,
// or simply absent) where MP3's is reliable but sniffing works for both, so
// one mechanism covers both codecs instead of trusting a header for one and
// sniffing the other. The two syncs are mutually exclusive by construction
// (aac_bridge.h), so sniffing can't misclassify an MP3 stream as AAC or vice
// versa on this bit pattern.
static size_t fill(u8 *out, size_t cap)
{
    if (g.codec == SW_CODEC_UNKNOWN) {
        size_t got = next_chunk();
        if (got == 0) return 0;   // nothing to look at yet; try again next call

        g.codec    = aacBridgeLooksLikeAdts(g.feed, got) ? SW_CODEC_AAC : SW_CODEC_MP3;
        g.feed_len = got;         // don't drop the bytes sniffing just consumed
    }

    return g.codec == SW_CODEC_AAC ? fill_aac(out, cap) : fill_mp3(out, cap);
}

static void dec_main(void *arg)
{
    (void)arg;

    while (g.running) {
        SwPlayState st = g.state;

        // The other half of net_main's release-store. It publishes with
        // `g.prebuffer = want; __dmb(); g.state = SW_PLAY_BUFFERING;` so the
        // cushion size is in memory before the state that advertises it; this
        // barrier is what makes reading them in that order mean anything on the
        // other core. Without it the pair is only half-synchronised, and the
        // failure would be a stale g.prebuffer from the PREVIOUS station -
        // audible as a stutter in the first second after switching, and
        // invisible to every host test, since player.c is compiled by none of
        // them and RING_BARRIER is a no-op off-console anyway. app.c's
        // job_finish() already pairs its barriers this way; this did not.
        __dmb();

        if (st == SW_PLAY_ERROR || st == SW_PLAY_CONNECTING) {
            svcSleepThread(20 * 1000 * 1000ULL);
            continue;
        }

        // Hold silence until there is a cushion. Starting on the first byte
        // that arrives produces a second of audio and then a stutter, which
        // sounds broken; waiting three seconds sounds like tuning in.
        if (st == SW_PLAY_BUFFERING) {
            if ((int)ringUsed(&g.ring) < g.prebuffer) {
                svcSleepThread(20 * 1000 * 1000ULL);
                continue;
            }
            // `st` was read before that ringUsed call, so it is already history
            // by now; advance_state re-reads and decides in one step. If the
            // station died while the cushion was filling, the answer is no and
            // the next pass round the loop picks the error up properly instead
            // of announcing playback of a stream that has stopped arriving.
            if (!advance_state(SW_PLAY_BUFFERING, SW_PLAY_PLAYING)) continue;
        }

        bool queued_any = false;

        for (int i = 0; i < WBUF_COUNT; i++) {
            u8 status = g.wbuf[i].status;
            if (status != NDSP_WBUF_FREE && status != NDSP_WBUF_DONE) continue;

            size_t bytes = fill(g.wbuf_mem[i], WBUF_BYTES);
            if (bytes == 0) break;

            // The DSP reads this memory directly and does not see the CPU's
            // cache, so the freshly written PCM has to be pushed out to RAM
            // before the buffer is handed over. Skipping this is how you get
            // the previous song playing out of one buffer.
            DSP_FlushDataCache(g.wbuf_mem[i], bytes);

            g.wbuf[i].data_pcm16 = (s16 *)g.wbuf_mem[i];
            g.wbuf[i].nsamples   = (u32)(bytes / (size_t)bytes_per_frame());
            g.wbuf[i].looping    = false;
            ndspChnWaveBufAdd(0, &g.wbuf[i]);
            queued_any = true;
        }

        if (!queued_any) {
            // Nothing could be filled. If the ring has genuinely run dry the
            // stream has fallen behind, so drop back to buffering and rebuild
            // the cushion - one gap of silence beats a minute of stuttering.
            //
            // The two conditions are read first and the state last, through
            // advance_state, because a dry ring and a silent channel are also
            // exactly what a station that has just stopped sending looks like:
            // testing the state separately and then writing it would put
            // "buffering" over the network thread's error most of the time this
            // fires, which is the one moment it must not.
            if (ringUsed(&g.ring) < FEED_CHUNK && !ndspChnIsPlaying(0)) {
                advance_state(SW_PLAY_PLAYING, SW_PLAY_BUFFERING);
            }
            svcSleepThread(5 * 1000 * 1000ULL);
        }
    }
}

// ------------------------------------------------------------------ lifetime

const char *swPlayerInitText(SwPlayerInitResult r)
{
    switch (r) {
        case SW_PLAYER_OK: return "";
        case SW_PLAYER_NO_DSPFIRM:
            return "Sound needs sdmc:/3ds/dspfirm.cdc.\n"
                   "Run DSP1 once to dump it from this console,\n"
                   "then start Skywave again.";
        case SW_PLAYER_NO_NDSP:  return "The sound service would not start.";
        case SW_PLAYER_NO_MEMORY:return "Not enough memory for audio buffers.";
    }
    return "Sound is unavailable.";
}

const char *swPlayerInitTextShort(SwPlayerInitResult r)
{
    switch (r) {
        case SW_PLAYER_OK: return "";
        case SW_PLAYER_NO_DSPFIRM:
            return "No sound: sdmc:/3ds/dspfirm.cdc is missing. Run DSP1 once.";
        case SW_PLAYER_NO_NDSP:   return "No sound: the sound service would not start.";
        case SW_PLAYER_NO_MEMORY: return "No sound: not enough memory for audio buffers.";
    }
    return "No sound.";
}

static bool dspfirm_present(void)
{
    struct stat sb;
    return stat("sdmc:/3ds/dspfirm.cdc", &sb) == 0 && sb.st_size > 0;
}

SwPlayerInitResult swPlayerInit(void)
{
    memset(&g, 0, sizeof(g));
    LightLock_Init(&g.state_lock);
    LightLock_Init(&g.text_lock);
    g.volume = 80;

    // ndspInit is asked first and the file is only checked to explain a
    // failure. The other way round - refusing to start unless the file is
    // there - is wrong, because emulators provide the DSP themselves and would
    // be locked out of an app that works on them perfectly well.
    if (R_FAILED(ndspInit()))
        return dspfirm_present() ? SW_PLAYER_NO_NDSP : SW_PLAYER_NO_DSPFIRM;

    // Set here, the moment the DSP is genuinely up, rather than at the end of a
    // successful init. This flag is the only thing swPlayerExit looks at to
    // decide whether to call ndspExit, and every `goto nomem` below leaves
    // through swPlayerExit: with it still false at that point a failed init
    // walked away having started the DSP and never stopped it, with wave
    // buffers it had just freed still handed to it. It doubles as the app's
    // "is sound available" flag, but that is not compromised by moving it -
    // the nomem path clears it again on the way out, so a partial init still
    // reports itself unavailable rather than half-working, and nothing else
    // reads it until this function has returned.
    g.inited = true;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);

    // Assume the common case up front; set_format corrects it the moment the
    // decoder reports what the stream actually is.
    g.rate = 44100; g.channels = 2;
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    apply_volume();

    g.ring_mem = malloc(RING_BYTES);
    g.net_in   = malloc(NET_CHUNK);
    g.net_out  = malloc(NET_CHUNK);
    g.feed     = malloc(FEED_CHUNK);
    if (!g.ring_mem || !g.net_in || !g.net_out || !g.feed) goto nomem;

    ringInit(&g.ring, g.ring_mem, RING_BYTES);

    // The DSP reads these buffers itself, so they have to be in linear memory -
    // ordinary malloc'd heap is not addressable by it.
    for (int i = 0; i < WBUF_COUNT; i++) {
        g.wbuf_mem[i] = linearAlloc(WBUF_BYTES);
        if (!g.wbuf_mem[i]) goto nomem;
    }

    if (mpg123_init() != MPG123_OK) goto nomem;

    int err = MPG123_OK;
    g.mh = mpg123_new(NULL, &err);
    if (!g.mh) { mpg123_exit(); goto nomem; }

    // Accept every rate the format supports, in 16-bit, mono or stereo. mpg123
    // does not resample, so forcing one rate here would simply refuse to play
    // the many stations that broadcast at 48 kHz or 22.05 kHz. The DSP is told
    // the real rate instead - see set_format.
    const long *rates; size_t nrates;
    mpg123_rates(&rates, &nrates);
    mpg123_format_none(g.mh);
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(g.mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);

    mpg123_open_feed(g.mh);

    // Allocated up front alongside mpg123, same reasoning: swPlayerPlay() must
    // never fail or allocate on the audio path, only reset already-owned
    // state (aacBridgeReset()) - see aac_bridge.h.
    g.ab = aacBridgeNew();
    if (!g.ab) goto nomem;

    g.state = SW_PLAY_STOPPED;
    return SW_PLAYER_OK;

nomem:
    swPlayerExit();
    return SW_PLAYER_NO_MEMORY;
}

void swPlayerExit(void)
{
    swPlayerStop();

    if (g.mh) { mpg123_close(g.mh); mpg123_delete(g.mh); mpg123_exit(); g.mh = NULL; }
    if (g.ab) { aacBridgeFree(g.ab); g.ab = NULL; }

    for (int i = 0; i < WBUF_COUNT; i++) {
        if (g.wbuf_mem[i]) { linearFree(g.wbuf_mem[i]); g.wbuf_mem[i] = NULL; }
    }

    free(g.ring_mem); g.ring_mem = NULL;
    free(g.net_in);   g.net_in   = NULL;
    free(g.net_out);  g.net_out  = NULL;
    free(g.feed);     g.feed     = NULL;

    if (g.inited) { ndspExit(); g.inited = false; }
}

// ------------------------------------------------------------------ controls

bool swPlayerPlay(const SwStation *st)
{
    // These three used to share one silent `return false`, which made them the
    // only exit in the entire play path that changed neither the state nor the
    // error text. The caller's fallback message ("Could not start playback.")
    // covered it, but named none of the three, and the state stayed STOPPED -
    // so on a console the whole failure was a screen that did not change. Each
    // now says which of the three it was, because on hardware this line IS the
    // diagnostic record; there is nowhere else to look.
    if (!g.inited) {
        set_error("Audio was never started, so there is nothing to play into.");
        return false;
    }
    if (!st) {
        set_error("No station was passed to the player.");
        return false;
    }
    if (!st->url[0]) {
        set_error("That station has no stream address in the directory.");
        return false;
    }

    swDiagf("swPlayerPlay '%s'", st->name);

    swPlayerStop();
    clear_text();

    g.station      = *st;
    g.have_station = true;
    g.registered   = false;
    g.prebuffer    = PREBUFFER_MIN;
    g.state        = SW_PLAY_CONNECTING;

    ringReset(&g.ring);
    icyInit(&g.demux, 0);

    // Back to the zeroed handle swDirRegisterPlayH is documented to want. The
    // field that actually matters is the cancel flag: swPlayerStop cancels
    // g.ping whether or not a ping was ever in flight, and swHttpClose only
    // clears the flag on a handle that was actually opened, so a stop during a
    // station that never got as far as registering would leave it set and kill
    // the next station's ping before it started. Safe to do here and only here:
    // swPlayerStop above has already joined both threads, so nobody owns it.
    memset(&g.ping, 0, sizeof(g.ping));

    // A fresh feed reader per station, for both codecs. Without this the
    // decoder is still holding the tail of the previous stream and the first
    // moments of the new one come out as noise. Codec is rediscovered from
    // scratch too - see fill()'s comment - since nothing says the next
    // station's stream type matches this one's.
    mpg123_close(g.mh);
    mpg123_open_feed(g.mh);
    aacBridgeReset(g.ab);
    g.codec    = SW_CODEC_UNKNOWN;
    g.feed_len = 0;
    g.aac_dry  = 0;    // patience for the new station, not what the last one used
    g.mp3_dry  = 0;

    for (int i = 0; i < WBUF_COUNT; i++) {
        memset(&g.wbuf[i], 0, sizeof(g.wbuf[i]));
        g.wbuf[i].status = NDSP_WBUF_FREE;
    }

    g.running = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    // The network thread goes on core1. That core belongs to the system, and
    // APT_SetAppCpuTimeLimit (called from main) is what buys the share of it
    // this needs - without that call threadCreate on core1 simply fails.
    //
    // The decode thread stays on core0 with the UI but at a higher priority,
    // because it is the one with a hard deadline: miss the DSP and the user
    // hears it, whereas a dropped frame of a mostly static screen is invisible.
    g.dec_thread = threadCreate(dec_main, NULL, 32 * 1024, prio - 2, 0, false);
    g.net_thread = threadCreate(net_main, NULL, 16 * 1024, prio - 1, 1, false);

    // Core 1 is not always available: it depends on the exheader's affinity
    // mask, which under the homebrew launcher belongs to whatever loaded us
    // rather than to Skywave. Falling back to core 0 costs some headroom but
    // still plays; refusing to play at all because of a scheduling preference
    // would be the wrong trade.
    if (!g.net_thread) {
        swDiagf("net thread would not start on core 1, retrying on core 0");
        g.net_thread = threadCreate(net_main, NULL, 16 * 1024, prio - 1, 0, false);
    }

    if (!g.dec_thread || !g.net_thread) {
        swDiagf("thread create FAILED: dec=%p net=%p",
                (void *)g.dec_thread, (void *)g.net_thread);
        swPlayerStop();
        set_error("Could not start playback threads.");
        return false;
    }

    swDiagf("threads up (dec + net), state=CONNECTING");
    return true;
}

void swPlayerStop(void)
{
    if (!g.running) {
        g.state = SW_PLAY_STOPPED;
        return;
    }

    g.running = false;

    // The network thread is almost certainly parked inside a read that will
    // not return until the station sends more. Cancelling is the only way to
    // get it back; without this, stopping a stalled station hangs the app.
    swHttpCancel(&g.http);

    // The one other place that thread can be blocked for an unbounded time is
    // the register-play ping, which is a whole HTTP round trip to a directory
    // mirror - see net_main. Cancelling the stream handle does not reach it,
    // and the threadJoin below is unbounded, so without this a stop that
    // happened to land on the ping waited out the mirror rather than the
    // station: the app froze solid with no way back, HOME included. Safe to
    // call whether or not a ping is in flight - swHttpCancel on an unopened
    // handle only sets the flag - and safe before the flag exists to matter,
    // because a cancel that arrives first survives the open (see swConnOpen).
    swHttpCancel(&g.ping);

    if (g.net_thread) { threadJoin(g.net_thread, U64_MAX); threadFree(g.net_thread); g.net_thread = NULL; }
    if (g.dec_thread) { threadJoin(g.dec_thread, U64_MAX); threadFree(g.dec_thread); g.dec_thread = NULL; }

    if (g.inited) ndspChnWaveBufClear(0);

    ringReset(&g.ring);
    g.have_station = false;
    g.state = SW_PLAY_STOPPED;
}

// -------------------------------------------------------------------- status

SwPlayState swPlayerState(void) { return g.state; }

const SwStation *swPlayerStation(void)
{
    return g.have_station ? &g.station : NULL;
}

void swPlayerNowPlaying(char *dst, size_t cap)
{
    if (!cap) return;
    LightLock_Lock(&g.text_lock);
    snprintf(dst, cap, "%s", g.title);
    LightLock_Unlock(&g.text_lock);
}

void swPlayerError(char *dst, size_t cap)
{
    if (!cap) return;
    // Same reason as swPlayerNowPlaying: set_error() writes this from the
    // network thread under text_lock, and this is called from the main thread
    // every frame, so a raw pointer would be a torn read on an ordinary event
    // rather than a rare one.
    LightLock_Lock(&g.text_lock);
    snprintf(dst, cap, "%s", g.error);
    LightLock_Unlock(&g.text_lock);
}

int swPlayerBufferPercent(void)
{
    if (!g.inited) return 0;
    return (int)((ringUsed(&g.ring) * 100) / (RING_BYTES - 1));
}

void swPlayerSetVolume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g.volume = percent;
    if (g.inited) apply_volume();
}

int swPlayerVolume(void) { return g.volume; }
