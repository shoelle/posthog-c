# posthog-c - working notes

An embeddable **PostHog SDK for C/C++**: one public C ABI, a native backend
(own HTTP + background sender), and a WASM backend (a shim over
`window.posthog`). See [`DESIGN.md`](DESIGN.md) for architecture and the
roadmap, [`README.md`](README.md) for the public intro. This file is the
coding brief: conventions, the module map, and the invariants that must hold.

## Canonical docs

When these conflict, prefer in order:
1. [`DESIGN.md`](DESIGN.md) - architecture + event/wire model + roadmap
2. this file - conventions + module map
3. inline comments near the code
4. [`README.md`](README.md)

The public contract is [`include/posthog.h`](include/posthog.h). Treat it as an
ABI: additive changes only within a major version; never reorder or repurpose
existing struct fields or enum values.

## Architecture at a glance

```
include/posthog.h        public C ABI (the contract)
include/posthog.hpp      header-only C++ convenience wrapper

src/ph_core.c            public API + the shared enqueue path
src/ph_native.c          sender thread + flush handshake + transport + offline spill/replay
src/ph_serialize.c       event record -> /batch/ JSON  (parity-critical, pure)
src/ph_jsonval.c         minimal JSON *parser* (DOM) - used only to read /flags/
src/ph_flags.c           feature flags: /flags/ fetch, cache, $feature_flag_called
src/ph_http.c            HTTP transport: http over sockets, https delegates to ph_tls
src/ph_tls.c             HTTPS via WinHTTP (Windows); BearSSL for Linux/macOS is next
src/ph_gzip.c            gzip /batch/ bodies (Content-Encoding: gzip) via vendored sdefl
src/ph_ratelimit.{h,c}   server-backpressure hold (429/Retry-After + PostHog quota) - pure, unit-tested
src/ph_crash.{h,c}       signal_crash: POSIX signal / Windows SEH handler -> a persisted $exception replayed next run
src/ph_wasm.c            WASM backend: the full API as an EM_ASM shim over window.posthog
src/ph_queue.{h,c}       bounded drop-oldest ring; owns the ph_event record
src/ph_props.c           ph_props setters + compact packer
src/ph_json.{h,c}        write-only JSON encoder over ph_strbuf
src/ph_str.{h,c}         growable byte buffer
src/ph_time.{h,c}        monotonic/wall clocks, ISO-8601, UUIDv7
src/ph_thread.{h,c}      mutex/cond/thread over Win32 + pthreads
src/ph_internal.h        SDK context, transport seam, cross-file helpers
```

Data flow (native): `ph_capture` (caller thread) packs a self-contained event
into the ring → the sender drains it, serializes a batch, and POSTs it. Nothing
but the ring is touched on the caller thread. On **wasm**, `ph_wasm.c`
implements the same API by calling straight into `window.posthog`, reusing
`ph_serialize_props_object` so the property JSON matches the native path
byte-for-byte.

## Coding conventions

- **C11 core, C ABI.** Plain C for maximum embeddability and zero C++ runtime
  dependency. The public header is `extern "C"`-guarded. No exceptions/RTTI
  anywhere (the C++ wrapper is exception-free too), so it links into strict
  engines built with `-fno-exceptions`.
- **POD + fixed capacity.** Config and property structs are POD. Per-event
  storage is bounded (`PH_MAX_PROPS`, `PH_KEY_CAP`, `PH_VAL_CAP`,
  `PH_EVENT_DATA_CAP`), overridable with `-D`. No unbounded growth.
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

These are the things a change must not break - call them out and test both
sides if a change risks them:

1. **Capture is hot-path-safe.** `ph_capture` (and the whole enqueue path) must
   not `malloc`, read the wall clock, or touch an RNG. It may read the
   monotonic tick and take the capture mutex. This is what makes it safe inside
   a game's simulation loop; the wall clock + UUID + JSON all live on the
   sender.
2. **Native/wasm parity.** Both backends must emit the same event JSON. Property
   shaping lives in `ph_serialize.c` (`ph_serialize_props_object`), shared by
   both; keep it pure so it stays directly testable. `zig build test-wasm`
   asserts the wasm shim reaches `window.posthog` with matching props under Node.
3. **The wire shape is fixed by the API, not by us.** `distinct_id` goes
   *inside* `properties` for `/batch/` items; `$set`/`$groups`/`$create_alias`
   have exact shapes (see `ph_serialize.c` + `test_serialize.c`). Don't
   freelance the JSON.
4. **`enabled = 0` is a true no-op** - no thread, no queue, every call returns
   quietly.

## Build and test

```sh
zig build              # static lib + headers + examples
zig build test         # native headless suite (must be green before finishing)
zig build test-wasm    # WASM backend via emcc + Node parity harness (needs emsdk)
zig build fuzz         # mutation-fuzz the /flags/ JSON + HTTP response parsers
zig build run-example  # C quickstart against a dev proxy
```

Tests are non-optional: every module ships coverage in `tests/`. The suite is
one executable driven by counting assert macros (`tests/test_util.h`); a
capturing mock transport (`tests/mock_transport.c`) lets capture tests assert
the exact batch bytes without a socket. If you add behavior, add a test in the
same change. Prefer asserting through the serializer or the mock transport over
standing up a live server.

## Review priorities

- hot-path-safety regressions in `ph_capture` (hidden alloc/clock/RNG)
- wire-shape drift from the ingestion API (invariant 3 above)
- native/wasm parity risk
- thread-safety of the queue + flush handshake (lock ordering: never take the
  queue lock then the flush lock)
- async-signal-safety on the `signal_crash` fault path (no malloc/lock/stdio in
  the handler; see [`ph_crash.c`](src/ph_crash.c))
- unbounded growth / missing fixed caps
