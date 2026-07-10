# TODO

## Pre-publication audit (2026-07-10)

The items below came from a full source, API, concurrency, privacy, transport,
WASM, packaging, and release-hygiene review. They are ordered by the risk of
publishing without them. Check an item only when its regression tests and the
relevant native/WASM verification are green.

### Ship blockers

- [x] **Keep feature flags scoped to the current identity and groups.**
  `ph_reset()` must invalidate the previous user's cache; `ph_identify()` and
  `ph_group()` must not leave a window where an old value is exposed under a new
  identity. Define an explicit readiness/loading contract, preserve last-known
  values only for the same evaluation context, merge partial
  `errorsWhileComputingFlags` responses, and do not erase the cache on a
  quota-limited response. Keep `$feature_flag_called` dedupe keyed by
  `(distinct_id, flag, value)` across ordinary reloads.
- [x] **Make JSON number encoding and parsing locale-independent.** The host may
  call `setlocale(LC_NUMERIC, ...)`; doubles must still serialize with `.` and
  parse according to JSON rather than the process locale. Cover native and WASM
  under a comma-decimal locale, and tighten the parser to reject malformed
  numbers, raw control characters, unpaired surrogates, and trailing garbage.
- [ ] **Make the lifecycle contract true.** Either synchronize init/shutdown
  against every public call or explicitly require externally serialized
  lifecycle with all callers quiesced. Document and enforce callback
  reentrancy: sender-thread `on_log`/`on_stats` callbacks must not deadlock by
  calling `ph_flush(-1)` or `ph_shutdown()`. Apply the same stated contract to
  pthread-enabled WASM builds or narrow it by backend.
- [x] **Eliminate compile-time capacity ABI traps.** The public `ph_props` layout
  depends on `PH_MAX_PROPS`/`PH_KEY_CAP`/`PH_VAL_CAP`, but the recommended Zig
  dependency artifact currently has no way to receive matching overrides.
  Either expose dependency build options and a consumer helper that applies the
  same defines, or remove the documented override promise / make the ABI opaque.
- [ ] **Define a stable anonymous-identity contract.** Native currently creates
  a fresh memory-only ID on every launch while WASM requires a host-persisted
  ID. Either require `distinct_id` consistently or provide an explicit
  persistence/load-save mechanism; make the quickstarts demonstrate stable
  identity and flag assignment.
- [ ] **Fail initialization when the sender cannot run.** Thread creation and
  reusable scratch allocation failures must propagate from `ph_init()` instead
  of leaving an enabled SDK that accepts events but can never deliver them.
  Validate numeric configuration before using it for allocations.

### Correctness, privacy, and reliability

- [ ] **Make offline persistence report and preserve reality.** Propagate open,
  allocation, and short-write failures; update failure/drop statistics instead
  of logging a successful spill; rewrite via temp file + flush/fsync + atomic
  rename; secure POSIX directories/files as 0700/0600; document that the file is
  plaintext, contains the project token and event data, and is single-process
  unless file locking is added.
- [ ] **Apply privacy hooks to every caller-controlled property.** Identify
  `$set` and group `$group_set` values currently bypass the denylist and
  `before_send` on both backends. Preserve explicit event properties when the
  combined super/event set exceeds `PH_MAX_PROPS`; enabling privacy must not
  silently change otherwise-valid event contents.
- [ ] **Bound and correctly frame `/flags/` HTTP responses.** Do not accumulate
  an unbounded network body before copying to `PH_FLAGS_RESP_CAP`; stream within
  the caller's cap, decode chunked transfer encoding, honor Content-Length, and
  distinguish an oversized/truncated response from valid JSON.
- [ ] **Make `request_timeout_ms` an end-to-end deadline.** DNS, connect, send,
  and receive must share one deadline so `ph_flush()`/`ph_shutdown()` cannot
  stall past the documented request timeout. Make shutdown interrupt in-flight
  retry/backoff and document any resolver limitation that cannot be cancelled.
- [ ] **Identify native flag requests as a PostHog SDK runtime.** Send an
  appropriate `User-Agent`/SDK header so PostHog runtime filtering does not
  classify posthog-c as unknown and omit native/server-scoped flags.
- [ ] **Resolve native/WASM semantic parity honestly.** Today only scalar
  property JSON is shared: WASM delegates timestamps, UUIDs, auto-properties,
  person profiles, flags, retries, and delivery to posthog-js; it also allocates
  and runs `before_send` on the caller thread, ignores the native rate limiter
  and stats, and has different init requirements. Either share full event
  shaping/semantics or narrow the parity and hot-path claims everywhere.
- [x] **Reject over-cap identity/config strings instead of silently splitting
  identity.** In particular, `$identify` currently uses the original long ID
  while later events use the stored 127-byte truncation. Validate API key, host,
  release, offline path, distinct IDs, group keys/types, and event names with
  clear result codes or documented truncation behavior.
- [ ] **Make diagnostics complete.** Count successful offline replays, permanent
  replay rejects, over-cap spill drops, and persistence failures consistently;
  define whether `ph_dropped_events()` includes `before_send` and delivery loss;
  keep `on_stats` and log messages aligned with those definitions.
- [ ] **Use stronger init-time entropy and document fork behavior.** Prefer OS
  randomness for anonymous IDs and UUID salt on every supported native platform.
  Either make post-fork use safe or state that the SDK must be initialized again
  in the child.
- [ ] **Keep capture timestamps correct across suspend and wall-clock changes.**
  Audit the monotonic-to-wall reconstruction on each platform (Linux
  `CLOCK_MONOTONIC` excludes suspend) and use an appropriate boot-time clock or
  bounded sender-side correction without adding wall-clock work to capture.

### Crash capture

- [ ] **Do not delete a crash record before durable handoff.** Replay currently
  removes the record immediately after enqueue; retain it until it has been
  durably spilled or acknowledged so a second crash/early exit cannot erase the
  first report.
- [ ] **Preserve host crash-handler state.** Save and restore an existing POSIX
  alternate signal stack, avoid clobbering handlers installed after posthog-c,
  and apply equivalent care to the Windows top-level exception filter.
- [ ] **Narrow the async-signal-safety claim.** Warming `backtrace()` does not
  make `backtrace()`/`dladdr()` generally async-signal-safe. Clearly label the
  in-process handler best-effort, keep the known loader/allocation risks visible,
  and ensure macOS either records the real fault PC or documents that it cannot.
- [ ] **Add real-fault subprocess tests on Linux and macOS.** Exercise handler
  chaining, alternate-stack restoration, torn records, replay durability, and
  an actual SIGSEGV/SIGABRT without taking down the test runner.

## Crash

- **Out-of-process `minidump_crash`.** In-process capture can't survive heap corruption and takes the loader lock during the per-frame module lookup (a crash *inside* the loader can stall the handler). The robust answer is a Crashpad-style out-of-process handler writing a minidump, symbolicated server-side by a separate **`posthog-crash`** service against a symbol store - that is where function *names* come from (we emit module+offset). Pick the backend at compile time. Prior art: [Breakpad](https://chromium.googlesource.com/breakpad/breakpad/), [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/).
- **Crash-time timestamps.** The replayed `$exception` is stamped at next-launch time, not when the crash happened. Record a wall-clock estimate in the crash record and inject it as the event `timestamp` at replay (needs an explicit-timestamp path through the serializer).
- **POSIX runtime verification.** The Windows SEH path is validated against a real fault end-to-end; the POSIX `sigaction` path is compile-checked (Linux cross-build) and still needs a real-fault integration test on the Linux/macOS CI runners (a subprocess that faults, mirroring the Windows one). glibc `backtrace()` is absent on musl - document or guard.

## Client-side drop reporting

`on_stats` now emits a periodic local JSON snapshot broken down by reason. The
remaining work is to decide whether an opt-in SDK health event should send that
snapshot to PostHog itself, and to close the accounting gaps listed above.

## Linux/macOS TLS

Linux (vendored BearSSL) and macOS (Secure Transport / NSURLSession) are stubbed - `ph_tls_send`/`ph_tls_fetch` return `-1` off-Windows. See the TLS-library tradeoff in DESIGN.md.

## Deliberately out of scope (don't re-litigate)

- **Elaborate on-disk run directories, lockfiles, and tiered cache pruning** (as full crash-reporting stacks carry): they exist to persist crash state + minidump attachments and to tell a crashed process from a running one. Our single bounded NDJSON spill plus a one-shot crash record is the right size for in-process `signal_crash`; richer on-disk state is a `minidump_crash` concern.
- **Discarding 5xx**: we retry 5xx with backoff - the better default for a transient server blip. Keep it.
- **Session replay**: no DOM/canvas from C, and a privacy liability. Excluded (see the session-replay tradeoff in DESIGN.md).

## Keep strong (don't regress)

Where posthog-c is deliberately stronger than other telemetry SDKs:
- **Hot-path-safe capture** - no `malloc`/clock/RNG on the caller thread. The common design allocates, hits an RNG (uuid + sampling), and runs `before_send` inline behind locks; we defer all of that to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead of special-casing it in the sender - the 429/quota hold would then compose as "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable logging across the C interface.

## Tests, packaging, and public presentation

- [ ] Add ReleaseSafe and Linux ASan/UBSan CI jobs, a bounded fuzz CI job, and a
  package-consumer smoke project. Keep an opt-in live PostHog contract test for
  `/batch/`, `/flags/`, payloads, quota responses, and exposure events.
- [ ] Make an explicitly requested `zig build test-wasm` fail when emcc or Node
  is missing instead of succeeding after a skip; keep ordinary native builds
  independent of emsdk.
- [ ] Include `DESIGN.md` and `TODO.md` in `build.zig.zon` so packaged README
  links work, and reconcile the README's Zig 0.15.2 requirement with the
  manifest's 0.15.0 minimum.
- [ ] Add `SECURITY.md`, `CONTRIBUTING.md`, and `CHANGELOG.md`; document supported
  platforms/toolchains, lifecycle and callback rules, offline-file sensitivity,
  the experimental crash handler, versioning, and the release checklist.
- [ ] Pin GitHub Actions by commit SHA and document the dependency/update policy
  for Zig, emsdk, and vendored sdefl.
- [ ] Revisit the public version before first release. `0.7.0` implies more
  maturity than a new, unofficial, Windows-first SDK; consider publishing as an
  explicitly experimental `0.1.0` until the ship blockers above are closed.
