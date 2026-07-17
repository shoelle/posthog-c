# AGENTS.md

Repo-local guidance for coding agents in `posthog-c/` - an embeddable **PostHog
SDK for C/C++**: one public C interface, a native backend (own HTTP + background
sender) and a WASM backend (a validated shim over host posthog-js). This file is
the coding brief - conventions, the module map, and the invariants that must hold.
See [`DESIGN.md`](DESIGN.md) for architecture + roadmap and
[`README.md`](README.md) for the public intro.

## Canonical docs

When these conflict, prefer in order:
1. [`DESIGN.md`](DESIGN.md) - architecture + key design decisions
2. this file - conventions + module map + invariants
3. inline comments near the code
4. [`README.md`](README.md)

The current public source contract is [`include/posthog.h`](include/posthog.h).
Consumers compile the SDK and headers with their application; this unofficial
0.x project does not promise source or binary compatibility between releases.

## Architecture at a glance

```
include/posthog.h        public C API (the current source contract)
include/posthog.hpp      header-only C++ convenience wrapper

src/ph_core.c            SDK instance, lifecycle, and the shared enqueue path (ph__submit_event)
src/ph_exception.c       ph_capture_exception -> $exception event (posthog_exception path)
src/ph_native.c          sender thread + flush handshake + transport + offline spill/replay
src/ph_serialize.c       event record -> /batch/ JSON  (parity-critical, pure)
src/ph_jsonval.c         minimal JSON *parser* (DOM) - used only to read /flags/
src/ph_flags.c           queued /flags/ fetch, cache, request status, accessors, $feature_flag_called
src/ph_http.c            HTTP transport: http over sockets, https delegates to ph_tls
src/ph_tls.c             HTTPS via system TLS: WinHTTP / Secure Transport / OpenSSL
src/ph_gzip.c            gzip /batch/ bodies (Content-Encoding: gzip) via vendored sdefl
src/ph_ratelimit.{h,c}   server-backpressure hold (429/Retry-After + PostHog quota) - pure, unit-tested
src/ph_crash.{h,c}       signal_crash: POSIX signal / Windows SEH handler -> a persisted $exception replayed next run
src/ph_wasm.c            WASM backend: typed EM_JS bridge over the pinned host descriptor
src/ph_queue.{h,c}       bounded drop-oldest ring; owns the ph_event record
src/ph_props.c           ph_props setters + compact packer
src/ph_util.{h,c}        shared ph_props helpers (copy/remove/find/denylist) - native + wasm
src/ph_json.{h,c}        write-only JSON encoder over ph_strbuf
src/ph_str.{h,c}         growable byte buffer
src/ph_time.{h,c}        monotonic/wall clocks, ISO-8601, UUIDv7
src/ph_thread.{h,c}      mutex/cond/thread over Win32 + pthreads
src/ph_internal.h        SDK context, transport seam, cross-file helpers
wasm/posthog-c-host.mjs  supported posthog-js bootstrap + final privacy contract
```

Data flow: native `ph_capture` packs a self-contained event into the ring; the
sender drains it, serializes a batch, and POSTs. The wasm backend validates and
pins the descriptor installed by `wasm/posthog-c-host.mjs`, then calls its
posthog-js client while reusing the shared property serializer. Full flow and
the reasoning behind each piece are in DESIGN.md ("Native pipeline" and "WASM
host contract").

## Coding conventions

- **C11 core, C interface.** Plain C for maximum embeddability and zero C++ runtime
  dependency. The public header is `extern "C"`-guarded. No exceptions/RTTI
  anywhere (the C++ wrapper is exception-free too), so it links into strict
  engines built with `-fno-exceptions`.
- **POD + fixed capacity.** Config and property structs are POD. Per-event
  storage is bounded (`PH_MAX_PROPS`, `PH_KEY_CAP`, `PH_VAL_CAP`,
  `PH_EVENT_DATA_CAP`). Public struct capacities are a fixed ABI; internal
  capacities may be changed only when the SDK and headers are rebuilt together.
  No unbounded growth.
- **One module = one subsystem.** Paired `.h`/`.c`, small and focused. Prefer
  splitting over a catch-all file.
- **`ph_` public prefix, `ph__` internal.** Public API is `ph_*` in
  `posthog.h`. Internal-but-cross-file symbols use `ph__*` (declared in
  `ph_internal.h`). File-local helpers are `static`.
- **One toolchain: Zig.** All builds and tests route through `zig build`. Don't
  add a parallel build system (a thin CMake shim wrapping the same sources is
  the one acceptable exception, for non-Zig consumers).
- **LF line endings**, ASCII source. Match the surrounding comment density and
  naming.

## Load-bearing invariants

Call these out and test both sides if a change risks them; the reasoning is in
DESIGN.md.

1. **Native capture is hot-path-safe.** `ph_capture` and the whole enqueue path
   must not `malloc`, read the wall clock, touch an RNG, or perform I/O. It does
   read the monotonic tick, increment an atomic, and take SDK-state + queue
   mutexes. (Wall clock + UUID + JSON all live on the sender.)
2. **Native/wasm C-property parity.** Typed `ph_props` shaping lives in the pure
   shared serializer and is identical before the host boundary. posthog-js then
   owns timestamps, UUIDs, browser enrichment, and final delivery, so complete
   wire envelopes are intentionally not byte-identical. `zig build test-wasm`
   guards the shared shapes and bridge behavior.
3. **Wire shape is fixed by the API.** `distinct_id` goes *inside* `properties`
   for `/batch/` items; `$set`/`$groups`/`$create_alias` have exact shapes (see
   `test_serialize.c`). Go through the serializer; don't freelance the JSON.
4. **`enabled = 0` is a true no-op** - no thread, no queue, every call returns
   quietly.

## Build and test

```sh
zig build              # static lib + headers + examples
zig build test         # native headless suite (must be green before finishing)
zig build test-wasm    # WASM backend via emcc + Node parity harness (needs emsdk)
zig build test-wasm-host # pinned real posthog-js contract (after fixture npm ci)
zig build fuzz         # mutation-fuzz the /flags/ JSON + HTTP response parsers
zig build run-example  # C quickstart against a dev proxy
```

Get `zig build test` green before finishing any change that touches code; run
`zig build test-wasm` too when a change touches the shared serializer or the wasm
shim. Every module ships coverage in `tests/`; add a test in the same change as
new behavior. The suite is one executable of counting-assert macros
(`tests/test_util.h`), with a capturing mock transport (`tests/mock_transport.c`)
so capture tests assert exact batch bytes without a socket. Prefer the serializer
or the mock over a live server. The one sanctioned live-server test is the opt-in
`zig build live-contract` (needs a disposable `POSTHOG_API_KEY`); CI runs it on
Windows, macOS, and Linux on pushes to `main`, exercising each TLS backend
(WinHTTP / Secure Transport / OpenSSL) end to end.

## Review priorities

- hot-path-safety regressions in `ph_capture` (hidden alloc/clock/RNG)
- wire-shape drift from the ingestion API (invariant 3 above)
- native/wasm parity risk
- thread-safety of the queue + flush handshake (lock ordering: never take the
  queue lock then the flush lock)
- async-signal-safety on the `signal_crash` fault path (no malloc/lock/stdio in
  the handler; see [`ph_crash.c`](src/ph_crash.c))
- unbounded growth / missing fixed caps

## Settled code-level decisions (don't re-litigate)

Review keeps re-flagging these; the current shape is intentional. Product-level
settled decisions live in TODO.md ("Settled decisions").

- **The reload re-entrancy guard stays after the async schedule**
  (`ph_reload_feature_flags`): the `ph__in_callback` / sender-thread guard
  exists to skip the blocking *wait* (which would deadlock on the sender), not
  the enqueue - a sender callback is meant to attach a refresh to the current
  job, and taking `g_ph.lock` briefly to schedule is safe because no callback
  runs while that lock is held.
- **No reserve-then-schedule rework for the reload history** (the
  `PH_ERR_FULL` path): unreachable in practice - coalescing plus supersession
  keep at most ~2 records `PENDING` against 8 slots - and the fallback (an
  untracked but harmless flag refresh) is benign.
- **`post_body`'s shortened-timeout branch has no deterministic test** (a
  second POST that *starts* after shutdown): the mock transport blocks
  in-thread, so driving the branch needs a two-POST-after-stop interleaving
  that hangs the join. Hand-verified (`remaining_ms == 0` yields `-1`,
  otherwise a value in `[1, request_timeout_ms]`); left uncovered by choice.
- **WASM keeps `ph_reload_feature_flags_async` returning `PH_ERR` and query
  tokens `UNKNOWN`**: posthog-js exposes a fire-and-forget reload plus a
  global change listener, not completion correlated to one reload and
  evaluation context - cached/local changes can fire the listener, quota
  failures may not, and queued identity changes expose no generation. Revisit
  only if posthog-js ships a public per-reload completion primitive carrying
  failure and request/context identity.
