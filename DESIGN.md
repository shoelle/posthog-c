# posthog-c - Design

The architecture of the SDK and the reasoning behind it. This documents the
shape as built and where each remaining roadmap stage plugs in. It is the
decision-oriented companion to the code; the public contract is
[`include/posthog.h`](include/posthog.h).

## 0. Goals

- A single **public C ABI** any language can bind, stable across versions.
- **Two transports, one API.** `native` brings its own HTTP + threading; `wasm`
  reuses the browser's `window.posthog`. Callers can't tell which is compiled
  in. Only the delivery mechanism differs.
- **Ride the raw ingestion API** (`/batch/`, `/i/v0/e/`, `/flags/`) - no
  dependency on another PostHog SDK.
- **Embeddable and hot-path-safe.** POD configs, fixed-capacity buffers, no
  exceptions/RTTI in the core, and a capture path that never allocates so it
  can be called from a deterministic simulation loop.

## 1. One ABI, two transports

```
             +--------------------------+
   caller -> |  posthog.h  (public ABI) |
             +------------+-------------+
                          |
          #if !__EMSCRIPTEN__      #if __EMSCRIPTEN__
                 native                   wasm
        (own HTTP + sender thread)  (window.posthog shim)
```

A bare C++ app has no HTTP or TLS, so the **native** backend brings its own -
plus a background sender thread and an on-disk spill queue. A browser page has
already loaded posthog-js, so the **wasm** backend is a ~30-line `EM_ASM` shim
that calls straight into the live `window.posthog`, reusing its batching,
retry, and offline machinery rather than shipping a second network stack.

The event model and JSON shape are shared, so the two backends emit
byte-comparable events. That parity is enforced against the serializer
([`src/ph_serialize.c`](src/ph_serialize.c)), which is pure and directly
unit-tested.

## 2. Native delivery pipeline

```
ph_capture (caller thread)              background sender thread
------------------------                ------------------------
pack event -> bounded ring  --enqueue-> drain <= max_batch/POST at
(no malloc / clock / RNG)                flush_at or flush_interval_ms
                                         serialize -> gzip -> POST {host}/batch/
                                         2xx: drop batch
                                         else: spill to disk, replay on reconnect
```

- **Capture is copy-and-return.** The caller builds a POD `ph_props` on the
  stack; `ph_capture` snapshots identity + super-properties + group scoping
  under one mutex and packs a self-contained event into a preallocated ring
  slot. It reads only a cheap monotonic tick and bumps an atomic sequence - no
  wall clock, no RNG, no heap. The ring is a fixed array sized at init; when it
  fills, the oldest event is dropped and a counter bumped, so a producer that
  outruns the network degrades gracefully instead of blocking. Drop-oldest biases
  toward recency (the common case, where the sender keeps up, and the right bias
  for crash/exception context); the tradeoff is that under *sustained* overload it
  is the start of a session that gets shed, not the latest events.
- **Packing.** A ring slot holds a compact `[name][distinct_id][packed props]`
  blob rather than a fat property struct, keeping the default 1000-event ring
  near ~2 MB instead of ~6 MB. The sender walks the blob to emit JSON. This is
  the one non-obvious internal format; see [`src/ph_queue.h`](src/ph_queue.h).
- **The sender** parks on the queue's condition variable until it has
  `flush_at` events or `flush_interval_ms` elapses, then drains everything
  present in `max_batch`-sized POSTs (a batch whose serialized body would exceed
  `max_batch_bytes` is split further, so no single POST trips the ingestion size
  limit). `ph_flush` wakes it and blocks on a drained handshake; `ph_shutdown`
  flushes, joins, and frees.
- **Transport seam.** Delivery goes through a small vtable
  ([`ph_transport`](src/ph_internal.h)). The default is plaintext HTTP; tests
  swap in a capturing mock. TLS (v0.2) slotted in as a second transport, not a
  rewrite.

## 3. Identity, baked at capture

Each event bakes its own `distinct_id` and anonymity decision at capture time,
so the sender needs no shared identity state and the ordering rule "events
captured before `ph_identify` keep the anonymous id" falls out for free.

- **Anonymous by default** (`person_profiles = PH_IDENTIFIED_ONLY`): events
  carry `$process_person_profile: false` until `ph_identify`, which is ~4x
  cheaper and the right privacy default for un-signed-in users.
- `ph_identify` sets the id + emits `$identify` (with `$set` person
  properties); `ph_alias` emits `$create_alias`; `ph_reset` (logout) rolls a
  fresh anonymous id and clears super-properties + groups.
- **Super properties** (`ph_register`) merge into every event; **groups**
  (`ph_group`) stamp `$groups` on subsequent events and emit `$groupidentify`.

## 4. Event model & wire shape

Straight off the ingestion API. Confirmed against PostHog's capture docs and a
customer's own integration reference:

```json
{ "api_key": "phc_...", "historical_migration": false, "batch": [
  { "event": "level_started",
    "timestamp": "2026-07-06T20:06:41.771Z",
    "uuid": "019f390a-436b-7b6e-94a2-2b19a09d01a9",
    "properties": {
      "distinct_id": "...",            // NOTE: inside properties for batch items
      "$lib": "posthog-c", "$lib_version": "0.7.0", "$lib_backend": "native",
      "$os": "Windows", "arch": "x86_64", "release": "myapp@1.2.3",
      "$process_person_profile": false,
      "weapon": "sword", "level": 3
    } } ] }
```

- **Idempotent by `uuid`.** Every event carries a UUIDv7 minted once from
  `(init-epoch, per-event sequence)` and kept across retries/replay, so at-least-once
  delivery dedups instead of double-counting. To honor the heap/clock-free
  capture path, `ph_capture` records only a monotonic tick; the sender
  reconstructs each event's wall-clock time from a single clock reading taken at
  init - so even a long-queued offline event reports when it actually happened.
- **Auto-properties** (`$lib`, `$lib_version`, `$lib_backend` = native/wasm,
  `$os`, `arch`, `release`) are stamped by the serializer, so dashboards slice
  by version and platform with zero caller effort. This fixed set is posthog-c's
  equivalent of an OpenTelemetry *Resource* (attributes attached to every
  signal): `$lib`/`$lib_version` map to `telemetry.sdk.name`/`.version`, `$os` to
  `os.type`, `arch` to `host.arch`, and `release` to `service.name` +
  `service.version`. We keep the `$`-prefixed names PostHog's ingestion and
  dashboards key on rather than the dotted OTel keys.
- Reserved shapes (`$set`, `$groups`, `$group_set`, `$create_alias`) are
  produced by the serializer from typed helpers, so a caller can't get the JSON
  wrong by hand.

## 5. Privacy & reliability (parity across native + wasm)

- **`before_send(event, props) -> keep|drop`** runs before serialization:
  product events run on the sender thread, while exception events scrub before
  enqueue so structured exception text can be redacted before `$exception_list`
  is built. Mutate `props` to redact, or return 0 to drop the event. Implemented.
  A reference scrubber (strip tokens,
  IPs, emails, URL query strings) with shared test vectors follows, so native
  and wasm can't drift.
- **`property_denylist`** strips named keys from every event in the same
  sender-side scrub pass - the blunt companion to the programmable hook. The
  scrub pass unpacks a ring slot back into a `ph_props`, applies the denylist +
  hook, and repacks, leaving `$groups` untouched.
- **Rate limiter.** A token bucket on the capture path (`rate_limit_per_sec`)
  caps product/exception events so a runaway loop can't flood ingestion. It
  refills from the monotonic tick capture already reads, so the hot path stays
  wall-clock/RNG/heap-free. Rate rejections + ring-overflow drops surface via
  `ph_dropped_events()`; `ph_capture` also returns `PH_ERR_RATE_LIMITED` for the
  rejected call, and when `stats_interval_ms` is set the sender emits a periodic
  `on_stats` JSON snapshot that breaks drops down by reason (rate-limited /
  queue-overflow / before_send) alongside delivered / failed / retry counts.
- **Server backpressure.** The client bucket caps what we *emit*; the sender
  also honors what the server *asks for*. On HTTP `429` (or `503` carrying a
  `Retry-After`, per RFC 9110) it parses the header - delay-seconds or an
  HTTP-date - arms a monotonic hold, and skips draining until the window clears,
  so a throttled endpoint isn't hammered batch-after-batch. Held events wait in
  the ring (drop-oldest on overflow); a small clock-derived jitter de-syncs a
  fleet throttled at once. The hold itself is pure and unit-tested
  ([`src/ph_ratelimit.c`](src/ph_ratelimit.c)); transports surface the header and
  a body prefix through the [`ph_transport`](src/ph_internal.h) seam. **Two
  signals feed the same hold:** standard HTTP `429`/`503` (what an `/ingest`
  proxy - Cloudflare, nginx, a gateway - emits), and PostHog's own event-quota
  notice, which its capture endpoint returns as `200` + a
  `{"quota_limited": ["events", ...]}` body - no `429`, no header. On the quota
  body the sender arms the default window; the batch itself was accepted (`200`)
  so it is not resent. The `200`-body detection is PostHog-specific and lives in
  the sender ([`src/ph_native.c`](src/ph_native.c)), keeping `ph_ratelimit` a
  generic RFC-9110 module.
- **Anonymous by default**, `respect_dnt` (planned), and a master `enabled`
  kill-switch (`enabled = 0` makes every call a no-op with no thread).
- Designed for privacy-sensitive apps, including those serving minors: the SDK
  is the data *processor*; the host app is the controller and drives consent via
  `enabled`.

## 6. Roadmap

| Stage | Adds |
|---|---|
| **v0.1** (done) | C ABI, `ph_props`, JSON serializer, ring queue, sender thread, `/batch/` over plaintext, mock-transport tests |
| **Privacy/reliability** (done) | `before_send` scrubber, `property_denylist`, capture rate limiter, server backpressure (429/`Retry-After` + `quota_limited` body), offline disk queue (spill/replay), `ph_dropped_events()` |
| **v0.2 TLS** (Windows, done) | Validated HTTPS via WinHTTP -> real `us.i.posthog.com`; Linux/macOS (vendored BearSSL) pending |
| **v0.3 WASM** (done) | `EM_ASM` shim over window.posthog; parity verified under Node (`zig build test-wasm`) |
| **v0.5 error tracking** (done) | `ph_capture_exception` -> `$exception_list` (mechanism + raw stack frames) |
| **v0.6 crash capture** (done) | in-process `signal_crash` handler (POSIX signals / Windows SEH) -> a persisted `$exception` replayed next launch; module+offset frames |
| **v0.7 feature flags** (done) | remote `/flags/` eval + local cache + deduped `$feature_flag_called` |
| later | out-of-process `minidump_crash` (Crashpad + the separate `posthog-crash` service) |

## 7. Tradeoffs & open questions

1. **TLS library.** Decided per-platform, following "reuse what the host has":
   **WinHTTP on Windows** (done - validated against the OS trust store, no
   vendoring), Secure Transport / NSURLSession on macOS next, and a **vendored
   BearSSL** (tiny, MIT) on Linux, which has no universal HTTPS client. Windows
   already ships a TLS client, so we don't bundle a second one there.
2. **WASM: reuse `window.posthog` vs `emscripten_fetch`.** Reuse wins for a page
   that already loads posthog-js. **Lean:** reuse; add a fetch fallback only for
   a headless consumer.
3. **Build system:** the fuller design sketch defaulted to CMake for
   portability; this repo leads with **Zig** to match its first customer (a game
   engine) and the stated toolchain preference, and exposes the sources so a CMake shim
   can wrap them later. Native + WASM both build in CI.
4. **C core vs C++ core.** Implemented in **C11** - a library literally named
   `posthog-c`, maximally embeddable, with zero C++ runtime dependency - plus a
   header-only C++ convenience wrapper. C ABI buys FFI + ABI stability + the
   broadest reuse.
5. **Fixed capacities.** Per-event property caps and the ring size are
   compile-time constants (overridable with `-D`). Safe because the SDK is
   consumed as source. The tradeoff: a pathological event with many long strings
   has its overflowing properties dropped (and counted), never a heap allocation.
6. **Session replay:** out of scope - no DOM/canvas to record from C, and a
   privacy liability. Explicitly excluded.
7. **Compression dependency.** `/batch/` bodies are gzip'd (`Content-Encoding:
   gzip`, on by default like posthog-js; opt out with `cfg.gzip = 0`). The one
   vendored dependency is `third_party/sdefl` - a single-file, ~525-LoC,
   compress-only DEFLATE lib (MIT / public domain), wrapped in `ph_gzip.c` to
   emit the gzip container. Native only: the wasm backend leaves compression to
   posthog-js. Chosen over miniz for being far smaller and compress-only, which
   is all we need.
8. **Threading: background sender vs. call-and-pump (open).** Today the native
   backend always spawns one background thread that owns all I/O, so `ph_capture`
   just copies and returns. An alternative some engines prefer - and the model
   Steamworks (`SteamAPI_RunCallbacks`) and the Discord Game SDK
   (`Discord_RunCallbacks`) use - is *call-and-pump*: no hidden thread, with the
   host driving the SDK's work from its own frame loop via a `ph_pump()` it calls
   each tick. The catch is that their pump is nearly free only because a resident
   companion process (the Steam client, the Discord app) does the network I/O
   out-of-process; posthog-c owns the HTTP, so a pump that serialized and POSTed
   inline would risk blowing a 16 ms frame. A real pump mode would therefore have
   to drive *non-blocking* I/O under a per-call time budget; otherwise it
   degrades to `ph_flush()` at a frame seam (which already exists). Recorded as a
   considered alternative, not committed - the background thread is the right
   default for most hosts, and the crash handler needs process-global state
   regardless.

## 8. Native crash capture (`signal_crash`, v0.6)

Three origins produce a `$exception`, distinguished by how the crash was caught
(the `posthog-exception` naming convention keeps this straight in the code):

| Origin | What catches it | Where |
|---|---|---|
| `posthog_exception` | app calls `ph_capture_exception` (handled) | `ph_core.c` |
| `signal_crash` | in-process POSIX signal / Windows SEH handler | `ph_crash.c` (v0.6) |
| `minidump_crash` | out-of-process Crashpad minidump | future; the `posthog-crash` service |

`signal_crash` (opt in with `cfg.crash_handler`, requires `offline_path`) turns a
fatal native fault into a `$exception` delivered on the **next** launch - a crashed
process can't reliably reach the network, so the flow is *crash -> persist ->
replay*, which reuses the offline spill's replay half and the `ph_capture_exception`
serializer wholesale. Only two pieces are new: the OS handler and the crash-record
format.

- **The handler stays minimal.** It runs in a dying process, so it snapshots only
  the signal, the faulting address, and the stack, then `write()`s one fixed
  record - no JSON, no `malloc`, on a static scratch buffer and (POSIX) a 64 KB
  `sigaltstack` so a stack-overflow crash still has room. The faulting
  instruction (from the crash context's `RIP`/`PC`) leads the trace, since the
  captured top frames are the handler + dispatcher themselves.
- **Frames are `(module, offset)`, not absolute addresses.** ASLR relocates
  modules between the crash and the replay, so an absolute address from the dead
  process is meaningless in the next one. The handler resolves each address to
  its module base (`dladdr` / `GetModuleHandleEx`) and stores `basename + offset` -
  stable across runs and resolvable by a symbol server. Turning offsets into
  function names needs the debug info, which is the `minidump_crash` server's
  job; the SDK deliberately stops at capture. This also means **no `dbghelp`/
  symbol dependency** in the SDK.
- **Known limits (v0.6).** The per-frame module lookup takes the loader
  lock, so a crash *inside* the loader can stall the handler - the fundamental
  reason robust capture is out-of-process (Crashpad). The replayed event is
  timestamped at next launch, not crash time. Both are documented in
  [`TODO.md`](TODO.md); the answer to both is the `minidump_crash` path, not more
  in-process cleverness. The Windows path is validated against a real fault
  end-to-end; the POSIX path compiles for Linux in CI and is verified there.
