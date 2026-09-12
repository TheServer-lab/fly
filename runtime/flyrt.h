#pragma once
#include <stdint.h>
#include <stddef.h>

// This header is the seam between fly-cc's IRGen and libflyrt
// (docs/architecture.md §4): as long as this contract holds, the compiler
// and the runtime can evolve independently.
//
// FlyValue layout MUST match what codegen.cpp builds as the LLVM struct
// type: { i8 tag, i64 payload }. See docs/architecture.md §2.1 for why the
// full design calls for a 16-byte { i8, [7 x i8] pad, i64 } — this milestone
// uses the same conceptual layout but lets the C compiler pick the exact
// padding, since codegen.cpp queries the LLVM struct layout at IR-build
// time rather than hardcoding an offset.

typedef enum {
    FLY_NUM   = 0,
    FLY_DEC   = 1,
    FLY_YN    = 2,
    FLY_EMP   = 3,
    FLY_TEX   = 4,
    FLY_COLL  = 5,
    FLY_BOARD = 6,
} FlyTag;

typedef struct {
    int64_t tag;
    int64_t payload; // num: as-is. dec: bit-cast double. yn: 0/1.
                      // tex/coll/board: pointer value (heap object, see below).
} FlyValue;

// ---- Heap objects (docs/architecture.md §2.2/§2.3) ------------------------
//
// MVP simplification vs. the full design: `refcount` is a plain (not
// _Atomic) uint32_t. The full design makes it atomic because concurrency
// (§9) lets a FlyValue be shared across tasks; concurrency isn't
// implemented yet in this milestone (no spawn/task keyword exists), so
// there is nothing that can race on it today. Swapping in
// _Atomic/fetch_add/fetch_sub is a pure follow-up once tasks land, not a
// layout change (see §9.7).
typedef struct {
    uint32_t refcount;
} FlyObjHeader;

typedef struct {
    FlyObjHeader hdr;
    size_t len;   // byte length (UTF-8), NOT including the NUL terminator
    char*  data;  // owned, NUL-terminated
} FlyText;

typedef struct {
    FlyObjHeader hdr;
    size_t len, cap;
    FlyValue* items;
} FlyColl;

typedef struct {
    FlyObjHeader hdr;
    size_t len, cap;
    FlyValue* keys;
    FlyValue* vals;   // parallel arrays; linear scan (§2.2's hash-table
                       // upgrade above a size threshold is a documented,
                       // not-yet-built optimization -- correctness doesn't
                       // depend on it, same "prove the need first" bias
                       // architecture.md applies elsewhere).
} FlyBoard;

#ifdef __cplusplus
extern "C" {
#endif

void    fly_rt_show(FlyValue v);

// Constructors / bit-reinterpretation helpers (runtime/value.c).
double   fly_rt_as_dec(FlyValue v);
FlyValue fly_rt_dec(double d);
FlyValue fly_rt_num(int64_t n);
FlyValue fly_rt_yn(int b);
FlyValue fly_rt_emp(void);

// ---- ARC (docs/architecture.md §2.3) --------------------------------------
// No-ops for non-heap tags. IRGen (codegen.cpp) is responsible for calling
// these at the scope boundaries described in §2.3: retain on
// assignment/copy into a new binding or container slot, release at
// end-of-scope for locals and when a slot is overwritten.
FlyValue fly_rt_retain(FlyValue v);   // returns v unchanged, for call-chaining convenience
void     fly_rt_release(FlyValue v);

// ---- Reflection -- SLEEP/NET follow-up -------------------------------------
// See runtime/value.c's header comment on fly_rt_typename: reachable from
// Fly source via `native job rt_typename(v)`, returns one of
// "num"/"dec"/"yn"/"emp"/"tex"/"coll"/"board".
FlyValue fly_rt_typename(FlyValue v);

// SLEEP/NET follow-up: byte <-> single-char tex, see runtime/text.c's
// header comment on why these exist (no backslash-escapes in Fly 0.1
// source literals). Reachable via `native job rt_char_from_code(code)` /
// `native job rt_char_code(ch)`.
FlyValue fly_rt_char_from_code(FlyValue code);
FlyValue fly_rt_char_code(FlyValue ch);

FlyValue fly_rt_add(FlyValue a, FlyValue b);
FlyValue fly_rt_sub(FlyValue a, FlyValue b);
FlyValue fly_rt_mul(FlyValue a, FlyValue b);
FlyValue fly_rt_div(FlyValue a, FlyValue b);
FlyValue fly_rt_mod(FlyValue a, FlyValue b);
FlyValue fly_rt_eq(FlyValue a, FlyValue b);
FlyValue fly_rt_neq(FlyValue a, FlyValue b);
FlyValue fly_rt_lt(FlyValue a, FlyValue b);
FlyValue fly_rt_gt(FlyValue a, FlyValue b);
FlyValue fly_rt_le(FlyValue a, FlyValue b);
FlyValue fly_rt_ge(FlyValue a, FlyValue b);
FlyValue fly_rt_and(FlyValue a, FlyValue b);
FlyValue fly_rt_or(FlyValue a, FlyValue b);
FlyValue fly_rt_not(FlyValue a);
FlyValue fly_rt_neg(FlyValue a);

// ---- tex (runtime/text.c) -------------------------------------------------
FlyValue fly_rt_text_from_cstr(const char* s);           // copies s
FlyValue fly_rt_text_from_bytes(const char* s, size_t n); // copies n bytes, need not be NUL-terminated
size_t   fly_rt_text_len(FlyValue v);
const char* fly_rt_text_data(FlyValue v);

// spec §10.3's conversion table: turns ANY FlyValue into its textual
// representation, used both by show() (already existed for scalars) and by
// string interpolation (§3.4's strbuild design). Produces a NEW FlyText
// (retained, caller-owned).
FlyValue fly_rt_to_text(FlyValue v);

// ---- string interpolation buffer builder (runtime/strbuild.c) -------------
// Growable byte buffer; codegen emits one fly_rt_strbuild_new, a sequence
// of append_lit/append_value calls (one per TemplatePart, in source order),
// then fly_rt_strbuild_finish to get the resulting FlyValue{FLY_TEX}.
typedef struct FlyStrBuild FlyStrBuild;
FlyStrBuild* fly_rt_strbuild_new(void);
void         fly_rt_strbuild_append_lit(FlyStrBuild* b, const char* s, size_t n);
void         fly_rt_strbuild_append_value(FlyStrBuild* b, FlyValue v); // uses fly_rt_to_text
FlyValue     fly_rt_strbuild_finish(FlyStrBuild* b); // frees b, returns owned FLY_TEX

// ---- coll (runtime/coll.c) -------------------------------------------------
FlyValue fly_rt_coll_new(size_t cap_hint);
void     fly_rt_coll_push(FlyValue coll, FlyValue v); // used to build coll literals; retains v
FlyValue fly_rt_attach(FlyValue coll, FlyValue v);          // §15.1, mutates in place, returns EMP
FlyValue fly_rt_place(FlyValue container, FlyValue idx, FlyValue v);  // §15.2 coll insert-at-index; also board's set(key, v) -- see coll.c
FlyValue fly_rt_erase(FlyValue container, FlyValue idx);         // §15.3 (coll: index; also usable as generic "remove")
FlyValue fly_rt_count(FlyValue container);                       // §15.4 (coll/board/tex)
FlyValue fly_rt_seek(FlyValue container, FlyValue needle);       // §15.5 (coll/tex)
FlyValue fly_rt_has(FlyValue container, FlyValue needle);        // §15.6 (coll/tex)
FlyValue fly_rt_bind(FlyValue coll, FlyValue sep);                // §15.7
FlyValue fly_rt_sever(FlyValue text, FlyValue sep);                // §15.8

// ---- tex ops (runtime/text.c) ---------------------------------------------
FlyValue fly_rt_cut(FlyValue text);    // §15.9 trim whitespace
FlyValue fly_rt_raise(FlyValue text);  // §15.10 uppercase
FlyValue fly_rt_lower(FlyValue text);  // §15.11 lowercase
FlyValue fly_rt_take(void);            // §18 read a line from stdin as tex

// ---- indexing / slicing (spec §13/§14; runtime/coll.c + text.c) -----------
// `board[key]` is also routed through fly_rt_index as this milestone's
// choice for §12's spec-left-open "access syntax" (returns EMP if absent).
FlyValue fly_rt_index(FlyValue container, FlyValue idx);
FlyValue fly_rt_slice(FlyValue container, FlyValue from, FlyValue to); // from/to may be FLY_EMP for an omitted bound

// ---- board (runtime/board.c) ----------------------------------------------
FlyValue fly_rt_board_new(void);
void     fly_rt_board_put(FlyValue board, FlyValue k, FlyValue v); // used to build board literals; retains k, v
FlyValue fly_rt_board_get(FlyValue board, FlyValue k);   // EMP if absent
FlyValue fly_rt_board_set(FlyValue board, FlyValue k, FlyValue v); // insert-or-overwrite, returns EMP
FlyValue fly_rt_board_erase(FlyValue board, FlyValue k);
FlyValue fly_rt_board_has(FlyValue board, FlyValue k);
FlyValue fly_rt_board_count(FlyValue board);
// Positional key accessor, added for iter.c's board iteration (milestone
// 6): board's storage (parallel key/value arrays, insertion order) is
// otherwise private to board.c like every other board internal, so this is
// the one accessor that exposes "the i-th key" without leaking FlyBoard's
// layout. `i` must be < fly_rt_board_count(board)'s payload (iter.c only
// ever calls this after checking pos < the snapshotted length).
FlyValue fly_rt_board_key_at(FlyValue board, size_t i);

// ---- do/grabe error handling (docs/architecture.md §3.5, milestone 5) ----
// Real unwinding now: fly_rt_throw_value raises the FlyValue as a genuine
// C++ exception (via libstdc++'s __cxa_throw), and fly-cc's codegen emits
// real LLVM `invoke`/`landingpad` around a `do` block's calls, targeting a
// landing pad that unpacks the value back out and binds it to the `grabe`
// variable. See runtime/eh.cpp for the implementation and the ONE
// deliberate MVP simplification it documents (catch-all only -- Fly 0.1
// has no typed errors yet to discriminate between).
void     fly_rt_throw(const char* msg);         // wraps msg as a `tex` FlyValue and throws it
void     fly_rt_throw_value(FlyValue errval);   // throws ANY FlyValue as the Fly error object
void*    fly_rt_begin_catch(void* excObj);      // wraps __cxa_begin_catch (codegen calls this at a landing pad)
void     fly_rt_end_catch(void);                // wraps __cxa_end_catch
FlyValue fly_rt_catch_extract(void* caught);    // pulls the FlyValue back out of what fly_rt_begin_catch returned
// Used by the implicit top-level catch-all landingpad that wraps a whole
// program's top-level statements (codegen.cpp): an error that unwinds all
// the way out uncaught is reported and the process exits nonzero --
// matching §3.5 ("unhandled errors propagate to the top-level program
// entry and terminate with an error message and nonzero exit code")
// exactly, just reached via real unwinding now instead of an immediate
// exit() at the original throw site.
void     fly_rt_report_uncaught(FlyValue errval);
// SLEEP/NET follow-up: user-raised Fly errors from Fly source itself, via
// `native job rt_throwv(err)`. See runtime/eh.cpp's header comment on this
// one entry point.
FlyValue fly_rt_throwv(FlyValue errval);

// ---- casts (docs/architecture.md §4/§4.1 `cast.c/h`, milestone 6) --------
// `num()`/`dec()`/`tex()` (spec §23) as callable conversions. `tex(v)` reuses
// fly_rt_to_text() above (the §10.3 conversion table already defines exactly
// this "turn ANY FlyValue into its textual representation" operation, so a
// separate entry point would just be a trivial wrapper); `num`/`dec` get
// their own entry points here since number parsing/range-checking isn't
// shared with anything else. Both throw a `tex` Fly error (via
// fly_rt_throw) on an unsupported source tag or an unparsable/out-of-range
// tex source, per the runtime's existing "throw on bad input" convention
// (see numNumOp/asText/etc. in arith.c/text.c).
FlyValue fly_rt_cast_num(FlyValue v);
FlyValue fly_rt_cast_dec(FlyValue v);

// ---- iteration (docs/architecture.md §3.4 "FlyIterator protocol",
// §4.1 `iter.c/h`, milestone 6) ---------------------------------------------
// Backs `for x in <container> { ... }` (spec: for/iterators). The iterator
// itself is an opaque native handle (never a FlyValue -- Fly 0.1 doesn't
// expose iterator objects as a first-class value the way coll/board are);
// codegen only ever passes it straight back into these three calls. MVP
// scope, documented rather than hidden: `coll` iterates its elements in
// order, `board` iterates its KEYS in insertion order (the natural "for k in
// board { ... show(board[k]) }" idiom given board's access syntax is
// itself left open by §12), and `tex` iterates one-byte tex substrings
// (UTF-8 codepoint-aware iteration is a documented future refinement, same
// "prove the need first" bias the rest of this runtime already takes with
// e.g. board's linear scan). The container's length is snapshotted at
// fly_rt_iter_new time, so mutating a coll/board while iterating it is a
// documented MVP hazard (matches most scripting-language iterator
// invalidation behavior) rather than a defined, checked error.
void*    fly_rt_iter_new(FlyValue container);   // retains container; throws for a non-iterable tag
int      fly_rt_iter_has_next(void* it);        // 0/1, never throws
FlyValue fly_rt_iter_next(void* it);             // advances and returns the next element/key, retained/fresh; never throws (caller must check has_next first)
void     fly_rt_iter_free(void* it);             // releases the retained container and frees the iterator; never throws

// ---- native system interface (docs/architecture.md §5, milestone 7) ------
//
// Every function below is reachable from Fly source only through a
// `native job rt_<x>(...)` declaration (see ast.h/parser.cpp's
// NativeJobDecl, codegen.cpp's declareNative) -- codegen binds the Fly
// name `rt_<x>` straight to the C symbol `fly_rt_<x>` with NO Fly body in
// between, per §21's now-resolved "native declaration syntax" open item.
// The stdlib/*.fly modules (process/filesystem/environment/system) are
// thin `bring`-able wrappers around exactly these, matching §5's Fly
// examples verbatim. `path.*` (§5.3) is the one exception: it's real
// dotted-call syntax (`path.join(...)`) handled as compiler-known
// builtins directly in codegen.cpp, not via a native declaration, since
// path.join is variadic and there's no variadic native-declaration syntax.
//
// Ownership convention: every FlyValue argument below is BORROWED (read,
// never retained/freed by the callee) -- same convention flyrt.h's §15
// ops already use (see e.g. fly_rt_index's comment). Every FlyValue
// RESULT is a fresh, caller-owned +1 (a brand-new tex/coll, or a
// non-heap scalar tag for which retain/release are no-ops anyway).
// Failures throw a `tex` Fly error via fly_rt_throw (message includes
// strerror(errno) and the offending path/name where applicable) rather
// than returning a sentinel value, so ordinary Fly `do`/`grabe` handles
// them like any other runtime error.

// -- 5.1 Process API (runtime/process.c) ------------------------------------
void     fly_rt_init_args(int argc, char** argv); // called once from generated `main`; NOT native-declared/Fly-callable
FlyValue fly_rt_args(void);                                    // coll of tex: argv[1..] (program name excluded)
FlyValue fly_rt_process_run(FlyValue program, FlyValue args);  // args: coll of tex; blocks; returns num exit code
FlyValue fly_rt_process_spawn(FlyValue program, FlyValue args);// non-blocking; returns num pid ("child" handle)
FlyValue fly_rt_process_wait(FlyValue child);                  // child: num pid from process_spawn; returns num exit code
FlyValue fly_rt_process_exit(FlyValue code);                   // never returns (exit()); FlyValue return type kept only for calling-convention uniformity

// -- 5.2 Filesystem API (runtime/filesystem.c) ------------------------------
FlyValue fly_rt_file_exists(FlyValue path);
FlyValue fly_rt_file_isfile(FlyValue path);
FlyValue fly_rt_file_isdir(FlyValue path);
FlyValue fly_rt_file_read(FlyValue path);              // whole-file contents as tex
FlyValue fly_rt_file_write(FlyValue path, FlyValue text);   // truncate-or-create; returns EMP
FlyValue fly_rt_file_append(FlyValue path, FlyValue text);  // create-if-absent; returns EMP
FlyValue fly_rt_dir_create(FlyValue path);             // mkdir; EEXIST is NOT an error (idempotent create)
FlyValue fly_rt_file_remove(FlyValue path);            // removes a file OR an empty directory
FlyValue fly_rt_dir_list(FlyValue path);               // coll of tex entry names, "." / ".." excluded
FlyValue fly_rt_getcwd(void);
FlyValue fly_rt_chdir(FlyValue path);

// -- 5.3 Path API (runtime/path.c) ------------------------------------------
// Compiler-known builtins reached via `path.<x>(...)` dotted-call syntax
// (codegen.cpp's genCall), NOT native declarations -- see this section's
// header comment. Pure string manipulation; never touches the filesystem
// (path.absolute resolves against fly_rt_getcwd()'s notion of cwd, but
// doesn't require the path to exist).
FlyValue fly_rt_path_join(FlyValue components);   // components: coll of tex, >= 1 element
FlyValue fly_rt_path_basename(FlyValue path);
FlyValue fly_rt_path_dirname(FlyValue path);
FlyValue fly_rt_path_extension(FlyValue path);    // includes the leading '.'; EMP-like "" tex if none
FlyValue fly_rt_path_stem(FlyValue path);         // basename with any extension removed
FlyValue fly_rt_path_absolute(FlyValue path);
FlyValue fly_rt_path_separator(void);             // one-character tex: '/' or '\\'

// -- 5.4 Environment API (runtime/environment.c) ----------------------------
FlyValue fly_rt_environment_get(FlyValue name);   // EMP if unset (matches board_get's "EMP if absent" convention)
FlyValue fly_rt_environment_has(FlyValue name);
FlyValue fly_rt_environment_set(FlyValue name, FlyValue value);

// -- 5.5 System API (runtime/system.c) --------------------------------------
FlyValue fly_rt_os(void);        // "linux" / "macos" / "windows" / uname()'s sysname otherwise
FlyValue fly_rt_arch(void);      // uname()'s machine field (e.g. "x86_64", "arm64")
FlyValue fly_rt_hostname(void);

// -- 5.6 Net API (runtime/net.c) --- SLEEP/NET milestone -------------------
//
// `net` is a BUILT-IN/runtime-backed module (see stdlib/net.fly's header
// comment and docs/architecture.md's module-architecture split): there is
// NO net.fly implementing sockets in Fly source -- `bring net` resolves to
// stdlib/net.fly purely so the `bring` statement has a file to point at
// (exactly like filesystem.fly/path.fly today), and every `net.*` call is
// a compiler-known dotted-call builtin wired straight to the symbols below
// (codegen.cpp's `builtins` table), same mechanism as filesystem.*/path.*.
//
// Connection/listener "handles" are plain OS file descriptors, returned
// and accepted as `num` for plain TCP -- no separate heap-object wrapper,
// matching this runtime's existing `process.spawn`'s "num pid" precedent
// (flyrt.h's §5.1 comment) rather than inventing a new handle
// representation. TLS connections (TLS/HTTPS follow-up milestone) are
// ALSO a plain `num`, but an opaque one rather than a raw fd -- see
// runtime/net.c's header comment on TLS_HANDLE_BASE for why, and why it
// doesn't change anything from the Fly-source side: net.send()/
// net.receive()/net.close() work identically on either kind. Every
// operation is BLOCKING (synchronous) -- Fly 0.1 has no concurrency
// primitives yet (§19 of architecture.md is still open design), so a
// blocking foundational API is the only one that makes sense to build on
// top of right now; non-blocking/async variants are square future work
// once tasks land, not a redesign of this API's shape.
//
// Scope: plain TCP + DNS (milestone 7) plus, as of the TLS/HTTPS follow-up
// milestone, real client-side TLS (see net_connect_tls below and
// runtime/net.c's header comment) -- genuine OpenSSL-backed handshake and
// certificate verification, not a stub. Still no UDP, no server-side TLS,
// no non-blocking I/O, no IPv6-vs-IPv4 selection beyond whatever
// getaddrinfo()'s default hint returns. Each of these remains real,
// documented future work layered on top of this primitive surface (§6's
// "higher-level Fly libraries" built on `bring net`, e.g. stdlib's HTTP
// module riding on net_connect_tls), not a redesign of it.
//
// Ownership/error convention: identical to §5.1-§5.5 above -- FlyValue
// arguments are borrowed, results are a fresh +1, and every failure throws
// a `tex` Fly error via fly_rt_throw (message includes strerror(errno),
// gai_strerror(), or an OpenSSL error/certificate-verification reason, and
// the operation/host/port involved) rather than a sentinel value.
FlyValue fly_rt_net_resolve(FlyValue host);                 // host: tex hostname or dotted-quad; returns tex of the first resolved IPv4/IPv6 address
FlyValue fly_rt_net_connect(FlyValue host, FlyValue port);  // port: num (1-65535); returns num fd, a connected TCP socket
// TLS/HTTPS follow-up milestone: client-side TLS on top of the same TCP
// connect, via OpenSSL (CMakeLists.txt). Real certificate verification
// (chain-of-trust AND hostname match, via SSL_set1_host) against the
// system trust store -- there is no way to disable this from Fly source,
// and no "insecure" flag. Returns an opaque `num` handle, NOT a raw fd
// (see runtime/net.c's header comment on TLS_HANDLE_BASE) -- but
// net.send()/net.receive()/net.close() below accept it exactly like a
// plain net.connect() handle; Fly source never needs to know which kind
// it has. Throws on any TCP failure, handshake failure, or certificate/
// hostname verification failure (with OpenSSL's own reason string, or
// X509_verify_cert_error_string() for a verification failure
// specifically).
FlyValue fly_rt_net_connect_tls(FlyValue host, FlyValue port);
FlyValue fly_rt_net_listen(FlyValue host, FlyValue port);   // host: tex bind address ("0.0.0.0" for all interfaces); returns num fd, a listening TCP socket (SO_REUSEADDR set, backlog 16)
FlyValue fly_rt_net_accept(FlyValue serverConn);            // serverConn: num fd from net_listen; BLOCKS until a client connects; returns num fd, the accepted connection
FlyValue fly_rt_net_send(FlyValue conn, FlyValue data);     // data: tex (raw bytes, not necessarily UTF-8 text); loops until every byte is sent; works on a plain OR a TLS conn; returns num bytes sent (== fly_rt_text_len(data) on success)
FlyValue fly_rt_net_receive(FlyValue conn, FlyValue maxlen);// maxlen: num >= 1; BLOCKS for at least one byte; works on a plain OR a TLS conn; returns tex ("" exactly means the peer closed the connection -- EOF, not an error)
FlyValue fly_rt_net_close(FlyValue conn);                   // conn: num fd/handle from connect/connect_tls/listen/accept; idempotent-ish (closing an already-closed handle throws)

#ifdef __cplusplus
}
#endif
