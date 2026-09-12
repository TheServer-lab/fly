#include "flyrt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

// ---- Cross-platform socket plumbing (Windows portability pass) --------
// Everything below uses `fly_sock_t` (SOCKET on Windows, int elsewhere)
// and the closeSocket() helper so the shared connection/handle/error logic
// stays in ONE code path and the platform differences are confined to the
// matching "_WIN32 / #else" blocks. Winsock differs from POSIX sockets in
// exactly the ways addressed here: there is no SIGPIPE (a send() to a
// closed peer returns WSAECONNRESET from Winsock instead of killing the
// process), every Winsock call needs a one-time WSAStartup first, and
// errors come back from WSAGetLastError() rather than errno.
#ifdef _WIN32
typedef SOCKET fly_sock_t;
#define FLY_INVALID_SOCKET INVALID_SOCKET
static inline int closeSocket(fly_sock_t fd) { return closesocket(fd); }
#else
typedef int fly_sock_t;
#define FLY_INVALID_SOCKET (-1)
static inline int closeSocket(fly_sock_t fd) { return close(fd); }
#endif

static inline int sockValid(fly_sock_t fd) { return fd != FLY_INVALID_SOCKET; }

#ifdef _WIN32
static int wsaToErrno(int wsaErr); // forward decl; defined with the error helpers below
#endif

// The current socket error as an errno value: WSAGetLastError() translated
// through wsaToErrno() on Windows, raw errno elsewhere. This keeps the
// socket logic's error bookkeeping identical across platforms -- it always
// works in errno terms (see the Windows portability note on throwNetError).
static int sockErrNo(void) {
#ifdef _WIN32
    return wsaToErrno(WSAGetLastError());
#else
    return errno;
#endif
}

#ifdef _WIN32
// Winsock requires WSAStartup before ANY Winsock call (socket(),
// gethostname(), getaddrinfo(), ...). Done once, process-wide, via a
// constructor -- deliberately WITHOUT a matching WSACleanup(): this
// runtime is a library inside a long-lived process and there is no good
// point at which "the whole process is done with Winsock" can be known
// (skipping WSACleanup is the universally recommended pattern for exactly
// this reason; calling it at the wrong time is what causes flaky crashes).
__attribute__((constructor))
static void flyNetWsaStartup(void) {
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
}
#else
// Any write to a socket the peer has already closed raises SIGPIPE by
// default, whose default disposition is to KILL THE PROCESS outright --
// not a catchable Fly error, a hard crash, regardless of any do/grabe
// around the net.* call that triggered it. Plain TCP net_send() already
// worked around this locally via MSG_NOSIGNAL (turning it into an EPIPE
// errno instead), but OpenSSL's internal handshake/read/write calls
// aren't under our control the same way -- a peer that closes the
// connection mid-TLS-handshake (exactly the "malformed/failed handshake"
// case this milestone's tests exercise) can raise SIGPIPE from inside
// SSL_connect() itself. The standard, safe fix for any network-touching
// program: ignore SIGPIPE process-wide once, so every write-to-a-closed-
// socket becomes a normal EPIPE errno (and a normal catchable Fly error
// via fly_rt_throw) instead of an uncatchable crash. Runs once, the first
// time any net.* code path executes, via a constructor so it's not
// possible to forget to call it. (Windows has no SIGPIPE -- see the
// typedef block above -- so this guard is POSIX-only.)
__attribute__((constructor))
static void flyNetIgnoreSigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}
#endif

// §5.6 (flyrt.h). Plain blocking TCP + getaddrinfo-based DNS resolution,
// plus (TLS/HTTPS follow-up milestone) real client-side TLS on top of the
// same connections, via OpenSSL (see CMakeLists.txt's OpenSSL section for
// why OpenSSL specifically). See flyrt.h's §5.6 header comment for the
// overall design/scope notes; this file is "just" the sockets/TLS
// plumbing behind that contract.
//
// ---- TLS handle design ----------------------------------------------------
// A plain TCP connection is just its fd, stored directly as `num` (see the
// milestone-7 header comment this file already had). A TLS connection
// needs an fd AND an OpenSSL `SSL*` together, so it can't be represented
// the same bare way -- but net.send()/net.receive()/net.close() (the
// milestone-7 API) should still work UNCHANGED on either kind, per this
// milestone's brief ("reuse existing connection/handle/error concepts...
// do not create a second unrelated networking API"). The fix: TLS
// connections are tracked in a small process-local table here, and the
// `num` handle Fly code holds is really an opaque index into that table,
// offset by TLS_HANDLE_BASE -- comfortably outside the range any real fd
// can ever have (fds are small, densely-allocated small integers) -- so
// fly_rt_net_send/receive/close can tell "is this a TLS handle or a raw
// fd" apart with one comparison and dispatch accordingly. Fly source
// never needs to know the difference.
#define TLS_HANDLE_BASE ((int64_t)1 << 32)

typedef struct {
    fly_sock_t fd;
    SSL* ssl;
    int used;
} TlsConn;

static TlsConn* g_tlsConns = NULL;
static size_t g_tlsConnsCap = 0;

static int tlsHandleIsTls(int64_t handle) { return handle >= TLS_HANDLE_BASE; }

static TlsConn* tlsConnAt(int64_t handle) {
    size_t idx = (size_t)(handle - TLS_HANDLE_BASE);
    if (idx >= g_tlsConnsCap || !g_tlsConns[idx].used) fly_rt_throw("net.*: not a valid TLS connection handle (already closed?)");
    return &g_tlsConns[idx];
}

static int64_t tlsConnAlloc(fly_sock_t fd, SSL* ssl) {
    for (size_t i = 0; i < g_tlsConnsCap; i++) {
        if (!g_tlsConns[i].used) {
            g_tlsConns[i].fd = fd; g_tlsConns[i].ssl = ssl; g_tlsConns[i].used = 1;
            return TLS_HANDLE_BASE + (int64_t)i;
        }
    }
    size_t newCap = g_tlsConnsCap == 0 ? 8 : g_tlsConnsCap * 2;
    TlsConn* grown = (TlsConn*)realloc(g_tlsConns, newCap * sizeof(TlsConn));
    if (!grown) fly_rt_throw("out of memory allocating a TLS connection handle");
    g_tlsConns = grown;
    for (size_t i = g_tlsConnsCap; i < newCap; i++) g_tlsConns[i].used = 0;
    size_t idx = g_tlsConnsCap;
    g_tlsConnsCap = newCap;
    g_tlsConns[idx].fd = fd; g_tlsConns[idx].ssl = ssl; g_tlsConns[idx].used = 1;
    return TLS_HANDLE_BASE + (int64_t)idx;
}

static void tlsConnFree(int64_t handle) {
    size_t idx = (size_t)(handle - TLS_HANDLE_BASE);
    g_tlsConns[idx].used = 0;
    g_tlsConns[idx].ssl = NULL;
    g_tlsConns[idx].fd = FLY_INVALID_SOCKET;
}

// One shared client SSL_CTX, created lazily and reused for every
// net.connect_tls() call (this is the normal OpenSSL usage pattern -- an
// SSL_CTX holds configuration/session-cache state, not a specific
// connection). SSL_VERIFY_PEER + the default (system) trust store is the
// baseline -- see the header comment above fly_rt_net_connect_tls for the
// FLY_TLS_CA_FILE test-only escape hatch, which is additive (an extra
// trust anchor for test fixtures), never a way to turn verification off.
static SSL_CTX* g_clientCtx = NULL;

static void throwSslError(const char* op, const char* detail) {
    unsigned long e = ERR_get_error();
    char reason[256];
    if (e != 0) {
        ERR_error_string_n(e, reason, sizeof(reason));
    } else {
        snprintf(reason, sizeof(reason), "unknown TLS error");
    }
    char buf[768];
    snprintf(buf, sizeof(buf), "net.%s(%s): %s", op, detail, reason);
    fly_rt_throw(buf);
}

static SSL_CTX* getClientCtx(void) {
    if (g_clientCtx) return g_clientCtx;

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) throwSslError("connect_tls", "SSL_CTX_new");

    // Modern-TLS-only, matches this milestone's "secure protocol
    // negotiation" requirement -- TLS 1.2 is the realistic floor for
    // "genuinely secure" in 2026; TLS_client_method() already excludes
    // SSLv3/TLS1.0/1.1 by OpenSSL 3.0's own default `SSL_OP_NO_*` set, but
    // pin it explicitly rather than relying on that default silently.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    // Milestone 8 §4: "if a test needs a local TLS server, generate/use a
    // deterministic local test certificate and explicitly configure trust
    // for the test fixture rather than weakening production verification".
    // FLY_TLS_CA_FILE is that explicit, opt-in mechanism: when set, its
    // CA is trusted IN ADDITION TO (not instead of) attempting to load the
    // normal system trust store below -- there is no environment variable
    // or code path anywhere in this file that disables verification.
    const char* extraCa = getenv("FLY_TLS_CA_FILE");
    if (extraCa && extraCa[0] != '\0') {
        if (!SSL_CTX_load_verify_locations(ctx, extraCa, NULL)) {
            SSL_CTX_free(ctx);
            throwSslError("connect_tls", "FLY_TLS_CA_FILE");
        }
    }
    if (!SSL_CTX_set_default_verify_paths(ctx) && !extraCa) {
        // Only fatal if we have no other trust source at all (e.g. no
        // FLY_TLS_CA_FILE either) -- a container image legitimately
        // missing a system CA bundle should fail loudly here rather than
        // silently accepting everything later.
        SSL_CTX_free(ctx);
        fly_rt_throw("net.connect_tls: could not load a trusted CA store (no system trust store found, and FLY_TLS_CA_FILE is not set)");
    }

    g_clientCtx = ctx;
    return ctx;
}

static const char* textCStr(FlyValue v, const char* what) {
    if (v.tag != FLY_TEX) {
        char buf[128];
        snprintf(buf, sizeof(buf), "type error: net.* %s must be tex", what);
        fly_rt_throw(buf);
    }
    return fly_rt_text_data(v);
}

static long portNum(FlyValue v) {
    if (v.tag != FLY_NUM) fly_rt_throw("type error: net.* port must be a num");
    int64_t p = v.payload;
    if (p < 1 || p > 65535) fly_rt_throw("net.*: port must be between 1 and 65535");
    return (long)p;
}

static int64_t fdNum(FlyValue v, const char* what) {
    if (v.tag != FLY_NUM) {
        char buf[128];
        snprintf(buf, sizeof(buf), "type error: net.* %s must be a num (a connection handle)", what);
        fly_rt_throw(buf);
    }
    return v.payload;
}

#ifdef _WIN32
// Winsock sets NO errno (that's a POSIX-ism) -- errors come back from
// WSAGetLastError(). This maps the error to errno too (via wsaToErrno,
// below) so the shared errno-based control flow above works unchanged,
// but FORMATS the Fly error message from the real Winsock text
// (FormatMessageA handles WSA error codes) the same way the POSIX build
// formats strerror(errno), so do/grabe sees a useful message either way.
// Inverse map of wsaToErrno, used by throwWsaError() below. Several failure
// paths here decode the socket problem from SO_ERROR (getsockopt) and hand
// it back purely as errno, leaving the thread's WSA-last-error slot at 0;
// mapping back to a WSA code lets FormatMessageA render the same text the
// failing Winsock call itself would have produced.
static int errnoToWsa(int e) {
    switch (e) {
    case EINTR: return WSAEINTR;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEINPROGRESS;
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return 0;
    }
}

static void throwWsaError(const char* op, const char* detail) {
    int wsaErr = WSAGetLastError();
    if (wsaErr == 0)
        wsaErr = errnoToWsa(errno); // error came back via SO_ERROR, slot unset
    errno = wsaToErrno(wsaErr);

    char sysmsg[256];
    DWORD n = 0;
    if (wsaErr != 0) {
        n = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            (DWORD)wsaErr,
            0,
            sysmsg,
            (DWORD)sizeof(sysmsg),
            NULL
        );
    }
    if (n > 0) {
        while (n > 0 && (sysmsg[n - 1] == '\r' || sysmsg[n - 1] == '\n'))
            sysmsg[--n] = '\0';
    } else if (wsaErr != 0) {
        snprintf(sysmsg, sizeof(sysmsg), "Winsock error %d", wsaErr);
    } else if (errno != 0) {
        snprintf(sysmsg, sizeof(sysmsg), "%s", strerror(errno));
    } else {
        snprintf(sysmsg, sizeof(sysmsg), "unknown socket error");
    }
    char buf[640];
    snprintf(buf, sizeof(buf), "net.%s(%s): %s", op, detail, sysmsg);
    fly_rt_throw(buf);
}
#else
static void throwErrno(const char* op, const char* detail) {
    char buf[640];
    snprintf(buf, sizeof(buf), "net.%s(%s): %s", op, detail, strerror(errno));
    fly_rt_throw(buf);
}
#endif

// Everything in this file that needs "throw the last socket error" uses
// this one name; the implementation is the platform-different one above.
static void throwNetError(const char* op, const char* detail) {
#ifdef _WIN32
    throwWsaError(op, detail);
#else
    throwErrno(op, detail);
#endif
}

#ifdef _WIN32
// Maps the Winsock error codes that actually arise in this file onto the
// matching errno values. Winsock's own numbering (10000-series) shares
// nothing with POSIX errno numbers, but production code here only ever
// COMPARES errnos (ETIMEDOUT for the timeout tests) or FALLS THROUGH to
// the message formatter, so a small explicit table is all that is needed;
// anything unlisted maps to a generic EIO rather than lying.
static int wsaToErrno(int wsaErr) {
    switch (wsaErr) {
    case WSAEINTR: return EINTR;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL: return EINVAL;
    case WSAEMFILE: return EMFILE;
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    default: return EIO;
    }
}
#endif

static void throwGai(const char* op, const char* host, int gaiErr) {
    char buf[640];
    snprintf(buf, sizeof(buf), "net.%s(%s): %s", op, host, gai_strerror(gaiErr));
    fly_rt_throw(buf);
}

// Shared by resolve/connect: runs getaddrinfo for `host`:`portStr` (TCP,
// either address family), throwing on failure. Caller must freeaddrinfo().
static struct addrinfo* resolveAddr(const char* op, const char* host, const char* portStr) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = NULL;
    int rc = getaddrinfo(host, portStr, &hints, &result);
    if (rc != 0) throwGai(op, host, rc);
    return result;
}

// ---- Timeouts (milestone 8 follow-up) --------------------------------
//
// Neither a blocking connect() nor a blocking SSL_connect() has any
// built-in time limit -- against a host/port that never responds (a
// firewall silently dropping SYNs, a closed port behind a black-holing
// router, or a TLS peer that accepts the TCP connection but never speaks)
// either call can block forever, which means any test exercising a
// "connection refused"/"handshake failed" path can hang the whole test
// run rather than failing fast. These two knobs put a hard ceiling on
// both phases:
//
//   FLY_NET_CONNECT_TIMEOUT_MS   -- ceiling on the TCP three-way handshake
//   FLY_NET_TLS_HANDSHAKE_TIMEOUT_MS -- ceiling on the TLS handshake that
//                                       follows a successful TCP connect
//
// Both default to 10 seconds (generous for any real network, short
// enough that a deliberately-unresponsive test fixture -- e.g. a closed
// localhost port, or a listener that accepts but never completes a TLS
// handshake -- fails predictably instead of hanging). Tests that want a
// tight, fast-failing bound can override either via the environment
// (see tests/e2e/net_tls/README, added alongside this).
//
// This does NOT change net.send()/net.receive()'s existing "blocking
// primitive" contract (flyrt.h §5.6, stdlib/net.fly's header comment) --
// once a connection is established (plain or TLS), reads/writes on it
// still block indefinitely, same as milestone 7. Only the two phases that
// can hang against a genuinely unresponsive/black-holed peer -- before
// any application-level exchange has even started -- get a timeout here.
#define FLY_NET_DEFAULT_CONNECT_TIMEOUT_MS 10000
#define FLY_NET_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS 10000

static long envTimeoutMs(const char* envVar, long fallbackMs) {
    const char* s = getenv(envVar);
    if (!s || !s[0]) return fallbackMs;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return fallbackMs; // malformed/non-positive -- fall back rather than 0-timeout-forever or garbage
    return v;
}

static long connectTimeoutMs(void) {
    return envTimeoutMs("FLY_NET_CONNECT_TIMEOUT_MS", FLY_NET_DEFAULT_CONNECT_TIMEOUT_MS);
}

static long tlsHandshakeTimeoutMs(void) {
    return envTimeoutMs("FLY_NET_TLS_HANDSHAKE_TIMEOUT_MS", FLY_NET_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS);
}

// Applies (or clears, when timeoutMs <= 0) a read+write timeout on `fd` via
// SO_RCVTIMEO/SO_SNDTIMEO. Used to bound the TLS handshake's own
// reads/writes (OpenSSL just calls read()/write() on the fd underneath --
// there's no separate "handshake timeout" knob in the blocking-BIO API
// this file otherwise uses), and cleared again immediately afterward so
// net.send()/net.receive() keep their documented indefinite-block
// contract once handshake/connect are done.
static void setSocketTimeout(fly_sock_t fd, long timeoutMs) {
#ifdef _WIN32
    // Winsock's SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of MILLISECONDS --
    // not the struct timeval (seconds+microseconds) POSIX sockets use --
    // and a 0 value means "no timeout", exactly like 0/0 timeval does.
    DWORD ms = timeoutMs > 0 ? (DWORD)timeoutMs : 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    if (timeoutMs > 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
    } else {
        tv.tv_sec = 0; tv.tv_usec = 0; // 0/0 means "no timeout" to setsockopt
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// A single candidate address's connect(), bounded by timeoutMs. Returns
// the connected fd, or -1 with errno set (ETIMEDOUT on timeout, whatever
// connect()/getsockopt(SO_ERROR) reported otherwise) -- same "-1 + errno"
// contract as a plain blocking connect() would give the caller, so
// tcpConnect's existing loop/error handling below needs no other changes.
// (POSIX: non-blocking connect + poll(). Windows: ioctlsocket(FIONBIO)
// non-blocking connect + select() -- Windows' poll()/WSAPoll exists, but
// select() is the portable, Microsoft-blessed single-socket wait.)
static int connectOneWithTimeout(fly_sock_t fd, const struct sockaddr* addr, socklen_t addrlen, long timeoutMs) {
#ifdef _WIN32
    unsigned long nonblock = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblock) != 0) return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        unsigned long block = 0;
        ioctlsocket(fd, FIONBIO, &block); // restore blocking -- rest of this file assumes blocking sockets
        return 0;
    }
    int wsaErr = WSAGetLastError();
    if (wsaErr != WSAEWOULDBLOCK) { // real, immediate connect failure
        errno = wsaToErrno(wsaErr);
        return -1;
    }

    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_SET(fd, &wfds);
    FD_ZERO(&efds); FD_SET(fd, &efds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int pr = select(0, NULL, &wfds, &efds, &tv);
    if (pr == 0) { errno = ETIMEDOUT; return -1; }
    if (pr == SOCKET_ERROR) { errno = wsaToErrno(WSAGetLastError()); return -1; }

    // A failed connect reports its real error via SO_ERROR on the fd
    // (as with poll() on POSIX) -- the writable-select bit tells us the
    // connect "completed", either successfully or with that error stuck
    // in SO_ERROR.
    int soErr = 0; socklen_t soErrLen = sizeof(soErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soErr, &soErrLen) == SOCKET_ERROR) {
        errno = wsaToErrno(WSAGetLastError());
        return -1;
    }
    if (soErr != 0) { errno = wsaToErrno(soErr); return -1; }

    unsigned long block = 0;
    ioctlsocket(fd, FIONBIO, &block); // restore blocking -- rest of this file assumes blocking sockets
    return 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        fcntl(fd, F_SETFL, flags); // restore blocking mode -- rest of this file assumes blocking sockets
        return 0;
    }
    if (errno != EINPROGRESS) return -1; // real, immediate connect failure

    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
    int pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr == 0) { errno = ETIMEDOUT; return -1; }
    if (pr < 0) return -1; // poll() itself failed; errno already set

    int soErr = 0; socklen_t soErrLen = sizeof(soErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen) < 0) return -1;
    if (soErr != 0) { errno = soErr; return -1; }

    fcntl(fd, F_SETFL, flags); // restore blocking mode for every subsequent op on this fd
    return 0;
#endif
}

// The plain-TCP-connect loop net.connect() already had, factored out so
// net.connect_tls() can reuse it verbatim instead of duplicating it (TLS
// is TCP plus a handshake, not a different transport). Now bounded by
// connectTimeoutMs() per candidate address (see the timeouts section
// above) so a black-holed host/closed port fails predictably instead of
// blocking forever.
static fly_sock_t tcpConnect(const char* op, const char* host, long port) {
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%ld", port);
    struct addrinfo* res = resolveAddr(op, host, portStr);
    long timeoutMs = connectTimeoutMs();

    fly_sock_t fd = FLY_INVALID_SOCKET;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (!sockValid(fd)) { lastErrno = sockErrNo(); continue; }
        if (connectOneWithTimeout(fd, p->ai_addr, p->ai_addrlen, timeoutMs) == 0) break;
        lastErrno = errno;
        closeSocket(fd);
        fd = FLY_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (!sockValid(fd)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s:%ld", host, port);
        errno = lastErrno ? lastErrno : ECONNREFUSED;
        if (errno == ETIMEDOUT) {
            char buf[640];
            snprintf(buf, sizeof(buf), "net.%s(%s): connection timed out after %ldms", op, detail, timeoutMs);
            fly_rt_throw(buf);
        }
        throwNetError(op, detail);
    }
    return fd;
}

FlyValue fly_rt_net_resolve(FlyValue hostV) {
    const char* host = textCStr(hostV, "resolve() host");
    struct addrinfo* res = resolveAddr("resolve", host, NULL);

    char ipbuf[INET6_ADDRSTRLEN];
    const void* addrPtr;
    if (res->ai_family == AF_INET)
        addrPtr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
    else
        addrPtr = &((struct sockaddr_in6*)res->ai_addr)->sin6_addr;

    if (!inet_ntop(res->ai_family, addrPtr, ipbuf, sizeof(ipbuf))) {
        freeaddrinfo(res);
        throwNetError("resolve", host);
    }
    freeaddrinfo(res);
    return fly_rt_text_from_cstr(ipbuf);
}

FlyValue fly_rt_net_connect(FlyValue hostV, FlyValue portV) {
    const char* host = textCStr(hostV, "connect() host");
    long port = portNum(portV);
    fly_sock_t fd = tcpConnect("connect", host, port);
    return fly_rt_num((int64_t)fd);
}

// TLS/HTTPS follow-up milestone. Client-side TLS: TCP connect, then a TLS
// handshake with SNI + hostname-verified certificate validation.
// Deliberately the same shape as net.connect() (host, port) -- see this
// file's header comment on why the RESULT is an opaque handle rather than
// a raw fd, and flyrt.h's §5.6 comment for the "reuse net.send/receive/
// close unchanged" design.
static int hostIsIpLiteral(const char* host) {
    struct in_addr a4;
    struct in6_addr a6;
    return inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
}

FlyValue fly_rt_net_connect_tls(FlyValue hostV, FlyValue portV) {
    const char* host = textCStr(hostV, "connect_tls() host");
    long port = portNum(portV);

    fly_sock_t fd = tcpConnect("connect_tls", host, port);

    SSL_CTX* ctx = getClientCtx();
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { closeSocket(fd); throwSslError("connect_tls", host); }

    int isIpLiteral = hostIsIpLiteral(host);

    // SNI (which server-side vhost to present a certificate for) --
    // without this, connecting to a host that shares an IP with other
    // TLS-served names (true of essentially all real HTTPS hosting,
    // including GitHub's) can get the wrong certificate entirely. Skipped
    // for a literal IP address, per RFC 6066 §3 (SNI is a hostname
    // extension; a bare IP isn't a valid value for it) -- same behavior
    // real TLS clients (curl, browsers) use.
    if (!isIpLiteral) SSL_set_tlsext_host_name(ssl, host);

    // Hostname-aware certificate verification (milestone 8 §4): tells
    // OpenSSL's automatic verification (SSL_VERIFY_PEER, set on the CTX
    // above) to also check the presented certificate's subject/SAN
    // against THIS host, not just that it chains to a trusted root.
    // Without this call, SSL_VERIFY_PEER alone verifies the chain but NOT
    // that the certificate is actually for the host we asked to connect
    // to -- a certificate for entirely-unrelated-but-still-CA-signed.com
    // would otherwise pass.
    //
    // Two different checks depending on what `host` actually is: a DNS
    // name is matched against the certificate's dNSName SANs
    // (SSL_set1_host); a literal IP address is matched against its
    // iPAddress SANs instead (X509_VERIFY_PARAM_set1_ip_asc) -- these are
    // deliberately NOT interchangeable in OpenSSL's API, and using the
    // DNS-name check for an IP literal would silently never match even a
    // certificate that correctly lists that IP as a SAN.
    int hostCheckOk;
    if (isIpLiteral) {
        hostCheckOk = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host);
    } else {
        hostCheckOk = SSL_set1_host(ssl, host);
    }
    if (!hostCheckOk) {
        SSL_free(ssl); closeSocket(fd);
        throwSslError("connect_tls", host);
    }

    // SSL_set_fd takes an int; Windows SOCKETs are pointer-width, but
    // real socket values are small -- the intptr cast is the documented
    // way to pass them here.
    if (SSL_set_fd(ssl, (int)(intptr_t)fd) != 1) { SSL_free(ssl); closeSocket(fd); throwSslError("connect_tls", host); }

    // Bound the handshake itself (see the timeouts section above
    // tcpConnect): a peer that completes the TCP handshake but then never
    // speaks TLS (or stalls mid-handshake) would otherwise block
    // SSL_connect() forever, since this file uses OpenSSL's plain
    // blocking-BIO mode throughout. SO_RCVTIMEO/SO_SNDTIMEO make the
    // underlying read()/write() calls OpenSSL performs during the
    // handshake time out instead of blocking indefinitely; cleared again
    // below once the handshake is done so net.send()/net.receive() keep
    // their documented indefinite-block behavior afterward. (On POSIX the
    // timeout surfaces as EAGAIN/EWOULDBLOCK from the timed-out recv();
    // on Windows it surfaces as WSAETIMEDOUT -- see the failure check
    // below.)
    long handshakeTimeoutMs = tlsHandshakeTimeoutMs();
    setSocketTimeout(fd, handshakeTimeoutMs);

    int rc = SSL_connect(ssl);
    if (rc != 1) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s:%ld", host, port);
        int sslErr = SSL_get_error(ssl, rc);
        long verifyResult = SSL_get_verify_result(ssl);
        if (verifyResult != X509_V_OK) {
            char buf[768];
            snprintf(buf, sizeof(buf), "net.connect_tls(%s): certificate verification failed: %s",
                     detail, X509_verify_cert_error_string(verifyResult));
            SSL_free(ssl); closeSocket(fd);
            fly_rt_throw(buf);
        }
        // A read()/write() timeout on a BLOCKING socket (via SO_RCVTIMEO/
        // SO_SNDTIMEO, set above) surfaces through OpenSSL's socket BIO as
        // EITHER SSL_ERROR_SYSCALL (older/some code paths) OR
        // SSL_ERROR_WANT_READ/SSL_ERROR_WANT_WRITE (the socket BIO's
        // generic "the underlying recv()/send() returned EAGAIN, please
        // retry" signal, which it can't distinguish from "this is a
        // non-blocking socket and this is normal" -- it just isn't one
        // here) -- both need checking. On POSIX the signal is the
        // underlying errno actually being EAGAIN/EWOULDBLOCK either way;
        // on Windows Winsock DOESN'T set errno for a timed-out recv(), so
        // WSAETIMEDOUT must be checked instead (OpenSSL just forwards the
        // failing recv() result).
        if ((sslErr == SSL_ERROR_SYSCALL || sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE)
            && (errno == EAGAIN || errno == EWOULDBLOCK
#ifdef _WIN32
                || WSAGetLastError() == WSAETIMEDOUT
#endif
                )) {
            char buf[640];
            snprintf(buf, sizeof(buf), "net.connect_tls(%s): TLS handshake timed out after %ldms",
                     detail, handshakeTimeoutMs);
            SSL_free(ssl); closeSocket(fd);
            fly_rt_throw(buf);
        }
        SSL_free(ssl); closeSocket(fd);
        throwSslError("connect_tls", detail);
    }

    // Handshake done -- clear the temporary timeout so send()/receive()
    // below (via net.send/net.receive, unchanged from milestone 7) go back
    // to blocking indefinitely, per their documented contract.
    setSocketTimeout(fd, 0);

    // Belt-and-suspenders: SSL_VERIFY_PEER should already have failed the
    // handshake above on a bad chain/hostname, but explicitly re-check
    // the verify result before handing back a "connected" handle rather
    // than trusting that alone -- this is the actual assertion that
    // matters for "no default 'ignore certificate errors' mode" (milestone
    // 8's hard rule).
    long verifyResult = SSL_get_verify_result(ssl);
    if (verifyResult != X509_V_OK) {
        char buf[768];
        snprintf(buf, sizeof(buf), "net.connect_tls(%s:%ld): certificate verification failed: %s",
                 host, port, X509_verify_cert_error_string(verifyResult));
        SSL_free(ssl); closeSocket(fd);
        fly_rt_throw(buf);
    }

    return fly_rt_num(tlsConnAlloc(fd, ssl));
}

FlyValue fly_rt_net_listen(FlyValue hostV, FlyValue portV) {
    const char* host = textCStr(hostV, "listen() host");
    long port = portNum(portV);
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%ld", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE; // host may be "0.0.0.0"/"::" or a specific bind address

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host, portStr, &hints, &res);
    if (rc != 0) throwGai("listen", host, rc);

    fly_sock_t fd = FLY_INVALID_SOCKET;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (!sockValid(fd)) { lastErrno = sockErrNo(); continue; }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, 16) == 0) break;
        lastErrno = sockErrNo();
        if (lastErrno == 0) lastErrno = EADDRINUSE;
        closeSocket(fd);
        fd = FLY_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (!sockValid(fd)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s:%ld", host, port);
        errno = lastErrno ? lastErrno : EADDRINUSE;
        throwNetError("listen", detail);
    }
    return fly_rt_num((int64_t)fd);
}

FlyValue fly_rt_net_accept(FlyValue serverV) {
    int64_t serverFd = fdNum(serverV, "accept() connection");
    fly_sock_t fd = accept((fly_sock_t)serverFd, NULL, NULL);
    if (!sockValid(fd)) throwNetError("accept", "server socket");
    return fly_rt_num((int64_t)fd);
}

FlyValue fly_rt_net_send(FlyValue connV, FlyValue dataV) {
    int64_t handle = fdNum(connV, "send() connection");
    if (dataV.tag != FLY_TEX) fly_rt_throw("type error: net.send() data must be tex");
    const char* data = fly_rt_text_data(dataV);
    size_t len = fly_rt_text_len(dataV);

    if (tlsHandleIsTls(handle)) {
        TlsConn* c = tlsConnAt(handle);
        size_t sent = 0;
        while (sent < len) {
            int n = SSL_write(c->ssl, data + sent, (int)(len - sent));
            if (n <= 0) throwSslError("send", "TLS connection");
            sent += (size_t)n;
        }
        return fly_rt_num((int64_t)sent);
    }

    fly_sock_t fd = (fly_sock_t)handle;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, (int)(len - sent), 0
#ifdef MSG_NOSIGNAL
            | MSG_NOSIGNAL
#endif
        );
        if (n < 0) {
            if (errno == EINTR) continue;
            throwNetError("send", "connection");
        }
        if (n == 0) break; // shouldn't happen for a blocking TCP send with len > 0, but avoid spinning if it does
        sent += (size_t)n;
    }
    return fly_rt_num((int64_t)sent);
}

FlyValue fly_rt_net_receive(FlyValue connV, FlyValue maxlenV) {
    int64_t handle = fdNum(connV, "receive() connection");
    if (maxlenV.tag != FLY_NUM || maxlenV.payload < 1)
        fly_rt_throw("type error: net.receive() maxlen must be a num >= 1");
    size_t maxlen = (size_t)maxlenV.payload;

    char* buf = (char*)malloc(maxlen);
    if (!buf) fly_rt_throw("out of memory in net.receive()");

    if (tlsHandleIsTls(handle)) {
        TlsConn* c = tlsConnAt(handle);
        int n = SSL_read(c->ssl, buf, (int)maxlen);
        if (n < 0) {
            int sslErr = SSL_get_error(c->ssl, n);
            free(buf);
            if (sslErr == SSL_ERROR_ZERO_RETURN) return fly_rt_text_from_cstr(""); // clean TLS close_notify -- EOF, not an error
            throwSslError("receive", "TLS connection");
        }
        // n == 0: either 0 bytes requested (can't happen, maxlen >= 1) or
        // the underlying transport hit EOF without a clean close_notify;
        // treat like TCP's EOF-is-"" convention (flyrt.h §5.6) either way.
        FlyValue out = fly_rt_text_from_bytes(buf, (size_t)(n < 0 ? 0 : n));
        free(buf);
        return out;
    }

    fly_sock_t fd = (fly_sock_t)handle;
    ssize_t n;
    for (;;) {
        n = recv(fd, buf, (int)maxlen, 0);
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    if (n < 0) throwNetError("receive", "connection");
    FlyValue out = fly_rt_text_from_bytes(buf, (size_t)n); // n == 0 -> "" (peer closed, EOF -- not an error, see flyrt.h)
    free(buf);
    return out;
}

FlyValue fly_rt_net_close(FlyValue connV) {
    int64_t handle = fdNum(connV, "close() connection");

    if (tlsHandleIsTls(handle)) {
        TlsConn* c = tlsConnAt(handle);
        // Best-effort clean shutdown (sends a close_notify); a single
        // attempt is the normal pragmatic choice here -- looping to
        // complete a bidirectional shutdown handshake matters for
        // protocols that reuse the TCP connection afterward, which NET
        // doesn't support (no connection reuse across handles, see
        // flyrt.h §5.6), so there's nothing further to protect by waiting
        // for the peer's own close_notify back.
        SSL_shutdown(c->ssl);
        fly_sock_t fd = c->fd;
        SSL_free(c->ssl);
        tlsConnFree(handle);
        if (closeSocket(fd) != 0) throwNetError("close", "TLS connection");
        return fly_rt_emp();
    }

    if (closeSocket((fly_sock_t)handle) != 0) throwNetError("close", "connection");
    return fly_rt_emp();
}

