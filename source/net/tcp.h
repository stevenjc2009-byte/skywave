#pragma once

// The transport under Skywave's HTTP: a socket, optionally wrapped in TLS that
// the app performs itself with mbedtls.
//
// Why the app does its own TLS. libctru's httpc goes through the console's
// ssl:C system module, whose TLS is not merely old but fixed - TLS 1.1, RSA key
// exchange, CBC ciphers, an RSA-only root store - and no libctru option raises
// any of it. Measured against Skywave's own default station list, only 4 of 19
// https hosts would complete a handshake in that shape, and GitHub refuses it on
// every one of its four hosts, which is why the in-app updater could never work.
// mbedtls in userland negotiates TLS 1.2 with ECDHE and AEAD ciphers and reaches
// all of them.
//
// One trap worth knowing before touching this file: devkitPro's mbedtls is built
// with MBEDTLS_NO_PLATFORM_ENTROPY and MBEDTLS_ENTROPY_HARDWARE_ALT, and its
// mbedtls_hardware_poll is a call to sslcGenerateRandomData that reports success
// unconditionally. Without sslcInit the entropy pool is filled with whatever was
// already in the buffer, every session key becomes predictable, and nothing
// anywhere returns an error. So ssl:C is still initialised here - as a random
// number generator, never for a handshake.

#include <stdbool.h>
#include <stddef.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

// Brings up sockets, the RNG and the trust store. Call once at startup, before
// any connection. Returns false if sockets or the RNG could not be started -
// which means no networking at all, and the caller should say so rather than
// failing one station at a time.
bool swNetInit(void);

void swNetExit(void);

// True if a certificate authority bundle was found, so verification is possible.
// When this is false the updater refuses to install rather than trusting an
// unverified executable; streams do not care either way.
bool swNetHaveTrustStore(void);

// Where the trust store was loaded from, or - before anything has loaded it -
// where it would be loaded from, decided by looking for the files rather than
// by parsing them. Never NULL; "none" when neither path holds a bundle.
//
// Written for the About screen, which asks every frame on the main thread: this
// never parses and never waits on a load that is already running.
const char *swNetTrustStoreSource(void);

typedef struct {
    int  fd;
    bool tls;
    bool open;

    // Read by the connection's own thread, written by any other thread that
    // wants the read to stop. Not a lock: the only transition is false -> true,
    // and a reader that sees the old value simply notices one poll later.
    volatile bool cancelled;

    // The mbedtls (or plain errno-shaped) code behind this connection's last
    // failure. Per-connection because two SwConns can be failing on different
    // threads at once - the player stream and an update check, say - and a
    // single shared value would let one connection's error be reported for
    // the other's failure.
    int last_error;

    mbedtls_ssl_context   ssl;
    mbedtls_ssl_config    conf;
} SwConn;

typedef enum {
    SW_CONN_OK = 0,
    SW_CONN_ERR_RESOLVE,     // the name does not resolve
    SW_CONN_ERR_CONNECT,     // no TCP connection: refused, unreachable, timed out
    SW_CONN_ERR_TLS,         // TCP connected but the TLS handshake failed
    SW_CONN_ERR_CERT,        // the handshake completed but the certificate is not trusted
    SW_CONN_ERR_CANCELLED,
    SW_CONN_ERR_INTERNAL,    // out of memory, or the RNG was never started
} SwConnResult;

// Opens a connection. `verify` turns on certificate checking, which needs a
// trust store; with verification on and no trust store, this fails with
// SW_CONN_ERR_CERT rather than quietly connecting to anyone.
//
// The connect is interruptible: it polls rather than blocking, so a host that
// black-holes packets cannot pin the calling thread for the seventy-odd seconds
// a kernel TCP timeout would take, and swConnCancel works during it.
//
// Name resolution is bounded too, but indirectly: libctru's getaddrinfo is one
// synchronous IPC to soc:U with no timeout parameter and nothing to select()
// on, so it cannot be interrupted from the caller's side at all (see tcp.c).
// Instead it runs on a throwaway thread that this call waits on with
// RESOLVE_TIMEOUT_MS and the same cancel flag; if neither fires before the
// resolve finishes the wait just ends with it, and if one fires first the
// thread is abandoned rather than killed - 3DS has no way to force a thread
// out of a blocking syscall - so it is left to finish (or never finish) on
// its own time and clean up after itself (see resolve() in tcp.c). A literal
// IPv4 address skips all of this and resolves in-process, instantly.
SwConnResult swConnOpen(SwConn *c, const char *host, const char *port,
                        bool tls, bool verify);

// The mbedtls error behind the last failure, for the message on screen. Zero
// when there was none.
int swConnLastError(const SwConn *c);

// Writes everything or fails. Returns true on success.
bool swConnWrite(SwConn *c, const void *buf, size_t len);

// Reads what is available, waiting at most `timeout_ms`.
//
// Returns the byte count, 0 if nothing arrived in that window (not an error - a
// live radio stream is paced by the clock and routinely has nothing ready), or
// -1 if the connection failed, ended, or was cancelled.
int swConnRead(SwConn *c, void *buf, size_t cap, int timeout_ms);

// Makes the next read fail, from any thread. Safe on a closed connection and
// safe to call more than once.
void swConnCancel(SwConn *c);

// Closes and frees. Call from the thread that owns the connection.
void swConnClose(SwConn *c);
