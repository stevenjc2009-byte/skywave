#include "player.h"

#include <3ds.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ring.h"
#include "../net/http.h"
#include "../net/icy.h"

// ---------------------------------------------------------------- constants

// Eight seconds of a 128 kbps stream. This is the whole reason playback
// survives a wifi wobble: the decoder keeps eating out of here while the
// network thread has nothing to give it. Must stay a power of two - Ring
// masks rather than divides, which is what makes it lock-free.
#define RING_BYTES     131072

// What one httpcDownloadData call asks for. Small enough that the stop flag is
// checked often (a 128 kbps stream delivers this in a quarter of a second) and
// large enough not to be all syscall overhead.
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
    u8            *feed;            // ring -> mpg123 staging, heap not stack

    // Transport
    SwHttp     http;
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

    LightLock  text_lock;           // guards `title` and `error`
    char       title[ICY_TITLE_MAX];
    char       error[96];
} Player;

static Player g;

// ---------------------------------------------------------------- utilities

static void set_error(const char *msg)
{
    LightLock_Lock(&g.text_lock);
    snprintf(g.error, sizeof(g.error), "%s", msg);
    LightLock_Unlock(&g.text_lock);
    g.state = SW_PLAY_ERROR;
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

static void net_main(void *arg)
{
    (void)arg;

    g.state = SW_PLAY_CONNECTING;

    if (!swHttpOpenStream(&g.http, g.station.url)) {
        set_error("Could not connect to this station.");
        return;
    }

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

    g.state = SW_PLAY_BUFFERING;

    while (g.running) {
        int n = swHttpRead(&g.http, g.net_in, NET_CHUNK);

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
        if (!g.registered && ringUsed(&g.ring) >= (size_t)g.prebuffer) {
            g.registered = true;
            swDirRegisterPlay(g.station.uuid);
        }
    }

    swHttpClose(&g.http);
}

// ------------------------------------------------------------- decode thread

// Decodes into one DSP buffer until it is full or the ring runs dry.
// Returns the byte count written.
static size_t fill(u8 *out, size_t cap)
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
        size_t got = ringRead(&g.ring, g.feed, FEED_CHUNK);
        if (got == 0) return done_total;   // underrun; caller decides what to do

        size_t ignored = 0;
        int fr = mpg123_decode(g.mh, g.feed, got, out + done_total,
                               cap - done_total, &ignored);
        done_total += ignored;

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

static void dec_main(void *arg)
{
    (void)arg;

    while (g.running) {
        SwPlayState st = g.state;

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
            g.state = SW_PLAY_PLAYING;
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
            if (g.state == SW_PLAY_PLAYING &&
                ringUsed(&g.ring) < FEED_CHUNK &&
                !ndspChnIsPlaying(0)) {
                g.state = SW_PLAY_BUFFERING;
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

static bool dspfirm_present(void)
{
    struct stat sb;
    return stat("sdmc:/3ds/dspfirm.cdc", &sb) == 0 && sb.st_size > 0;
}

SwPlayerInitResult swPlayerInit(void)
{
    memset(&g, 0, sizeof(g));
    LightLock_Init(&g.text_lock);
    g.volume = 80;

    // ndspInit is asked first and the file is only checked to explain a
    // failure. The other way round - refusing to start unless the file is
    // there - is wrong, because emulators provide the DSP themselves and would
    // be locked out of an app that works on them perfectly well.
    if (R_FAILED(ndspInit()))
        return dspfirm_present() ? SW_PLAYER_NO_NDSP : SW_PLAYER_NO_DSPFIRM;

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

    g.inited = true;
    g.state  = SW_PLAY_STOPPED;
    return SW_PLAYER_OK;

nomem:
    swPlayerExit();
    return SW_PLAYER_NO_MEMORY;
}

void swPlayerExit(void)
{
    swPlayerStop();

    if (g.mh) { mpg123_close(g.mh); mpg123_delete(g.mh); mpg123_exit(); g.mh = NULL; }

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
    if (!g.inited || !st || !st->url[0]) return false;

    swPlayerStop();
    clear_text();

    g.station      = *st;
    g.have_station = true;
    g.registered   = false;
    g.prebuffer    = PREBUFFER_MIN;
    g.state        = SW_PLAY_CONNECTING;

    ringReset(&g.ring);
    icyInit(&g.demux, 0);

    // A fresh feed reader per station. Without this the decoder is still
    // holding the tail of the previous stream and the first moments of the new
    // one come out as noise.
    mpg123_close(g.mh);
    mpg123_open_feed(g.mh);

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
    if (!g.net_thread)
        g.net_thread = threadCreate(net_main, NULL, 16 * 1024, prio - 1, 0, false);

    if (!g.dec_thread || !g.net_thread) {
        swPlayerStop();
        set_error("Could not start playback threads.");
        return false;
    }
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

const char *swPlayerError(void) { return g.error; }

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
