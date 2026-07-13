# TODO

## Correctness and API contracts

- **Make WASM initialization transactional and singleton.** Mirror the public lifecycle contract: a disabled initialization still owns the singleton; a second `ph_init()` cannot reconfigure live state; arguments and host identity are validated before callbacks, denylist, super properties, or enabled state are committed; failure leaves a clean retryable state; and only `ph_shutdown()` permits re-init. Cover invalid config, identity mismatch, failed-init retry, rejected live reconfiguration, disabled init, and shutdown/re-init.
- **Fail closed on WASM bridge serialization failure.** `props_to_json()` currently checks `data` but not the latched `oom`, so an early allocation failure becomes `{}` and a later one can reach `JSON.parse` as partial JSON. Suppress the posthog-js operation, return `PH_ERR` where the C API permits, route void-operation failures through the chosen WASM diagnostic contract, and add deterministic allocation-failure tests.
- **Honor the structured exception contract on WASM.** Reuse the bounded `$exception_list` builder and capture the C-supplied type, value, mechanism, and frames directly. Do not discard `synthetic`/frames or invent a browser `Error` stack when no frame was supplied. Test frame denylisting and `before_send` redaction in the WASM harness.
- **Make person-profile policy mean the same thing everywhere.** Map and test all `ph_person_profiles` modes across native and the posthog-js host. In particular, native currently forces `$identify` to create a profile even under `PH_NEVER`; `PH_NEVER` must remain never for capture and control events on both backends.
- **Make `ph_config` truthful on WASM.** Publish a field-ownership matrix and stop silently accepting ineffective settings. Honor shim-owned fields such as `release`, diagnostics, and flag-exposure policy; validate the API key, normalized host, identity, profile mode, flag preload, and rate-limit settings supplied by a supported Closure-safe host bootstrap descriptor; label native delivery fields as ignored; and guarantee that `enabled = 0` leaves the host posthog-js instance untouched.
- **Add an explicit GeoIP opt-out.** Preserve today's behavior by default. When enabled, native must emit `$geoip_disable: true` on every event and `geoip_disable: true` on `/flags/`; WASM must document and validate the host/project policy needed to cover posthog-js-owned requests rather than pretending the C shim controls them.
- **Define and test the scrubber boundary.** Document which ingestion-envelope fields are mandatory and separate them from optional automatic enrichment. On native, configured `release` and optional `$os`/`arch` properties must have defined precedence and honor the denylist; document that the C `before_send` hook runs before serialization and before posthog-js enrichment. When final browser properties must be scrubbed, require a host posthog-js `before_send` and verify the two-stage contract against a pinned posthog-js, not only a mock.

## Embedding and lifecycle

- **Ship and test a supported Emscripten integration surface.** Publish one machine-consumable WASM source/flag recipe and consume it from both `test-wasm` and a standalone consumer fixture. Move the inline bridge to typed `EM_JS` (or an equivalent isolated bridge) so the C sources compile with explicit `-std=c11`; preserve the external `window['posthog']` ABI with quoted property access or externs; and run the behavioral harness in ordinary and optimized `--closure 1` builds. A successful link is insufficient - the current Closure build runs but makes zero host calls.
- **Make the native capture contract exact.** Bound the caller-provided event-name scan instead of using unbounded `strlen`, then audit the public header, README, DESIGN, examples, and inline comments. Capture avoids allocation, wall-clock/RNG, disk, and network work, but it reads the monotonic clock, increments an atomic, and acquires both SDK-state and queue mutexes; it is neither non-blocking/lock-free nor hard-real-time-safe.
- **Define bounded native shutdown semantics.** A stop can currently start one request for every offline and queued batch, multiplying `request_timeout_ms`. Add an explicit timed drain/persist/drop policy driven by one monotonic deadline; do not start new work after that deadline; document that an in-progress libc resolver still cannot be cancelled; and test multi-batch memory queues, offline replay, an in-flight request, and drop accounting.
- **Expose sender-queued feature-flag refresh.** Reuse and coalesce the existing refetch generations behind a non-blocking public request, with queryable completion that distinguishes success, failure, and context supersession. Keep `ph_reload_feature_flags()` as the blocking wrapper.

## Crash

- **Out-of-process `minidump_crash`.** In-process capture can't survive heap corruption and takes the loader lock during the per-frame module lookup (a crash *inside* the loader can stall the handler). The robust answer is a Crashpad-style out-of-process handler writing a minidump, symbolicated server-side by a separate **`posthog-crash`** service against a symbol store - that is where function *names* come from (we emit module+offset). Pick the backend at compile time. Prior art: [Breakpad](https://chromium.googlesource.com/breakpad/breakpad/), [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/).
- **Crash-time timestamps.** The replayed `$exception` is stamped at next-launch time, not when the crash happened. Record a wall-clock estimate in the crash record and inject it as the event `timestamp` at replay (needs an explicit-timestamp path through the serializer).
- **macOS fault-PC and musl `backtrace()`.** The POSIX `sigaction` path now passes its real-fault integration test on both the Linux and macOS CI runners - a subprocess raises `SIGABRT`, our handler records it and chains to a host handler, and the record is decoded on replay (previously this was only compile-checked off-Windows). Two gaps remain: on macOS the faulting-instruction PC is not extracted (reading it through `uc_mcontext` faulted the arm64 handler, so we fall back to `backtrace()`; restore an alignment-safe read validated on real hardware), and glibc `backtrace()` is absent on musl/Alpine (document or guard).

## Client-side drop reporting

`on_stats` now emits a periodic native JSON snapshot broken down by reason. The
remaining work is to decide whether an opt-in SDK health event should send that
snapshot to PostHog itself. WASM delivery and loss accounting remain deliberately
host-owned: posthog-js exposes no equivalent snapshot, so `on_stats` stays
native-only and `ph_dropped_events()` stays zero rather than report partial truth.

## TLS

Each desktop links its own system TLS stack rather than vendoring a crypto
library: WinHTTP (Windows), Secure Transport (macOS, `#if __APPLE__`), and the
system OpenSSL (Linux, `libssl-dev`). All verify the server cert chain and
hostname against the OS trust store. The socket, HTTP framing, and response
parser are shared across the POSIX backends; only the handshake/read/write
differs. The live-contract job runs on all three CI runners, so a push delivers
a real event over each backend and proves them end to end.

Remaining:
- **Other Unix (the BSDs)** has no backend - `ph_tls_available()` returns 0 and
  https:// is refused. Wire OpenSSL (or LibreSSL) there too if it ever matters.
- **macOS Secure Transport is deprecated** in favour of Network.framework. It
  works, but a future migration would silence the deprecation and gain async IO.
- **Linux ships a hard OpenSSL dependency.** Fine for a Linux SDK (it's the
  de-facto system TLS), but a vendored/optional backend would suit a
  no-dependencies embed; left as a deliberate trade, not an oversight.

## Deliberately out of scope (don't re-litigate)

- **Elaborate on-disk run directories, lockfiles, and tiered cache pruning** (as full crash-reporting stacks carry): they exist to persist crash state + minidump attachments and to tell a crashed process from a running one. Our single bounded NDJSON spill plus a one-shot crash record is the right size for in-process `signal_crash`; richer on-disk state is a `minidump_crash` concern.
- **Discarding 5xx**: we retry 5xx with backoff - the better default for a transient server blip. Keep it.
- **Session replay**: no DOM/canvas from C, and a privacy liability. Excluded (noted as out of scope in DESIGN.md).

## Keep strong (don't regress)

Where posthog-c is deliberately stronger than other telemetry SDKs:
- **Allocation-free native capture** - no `malloc`, wall-clock read, RNG, disk, or network on the caller thread. Preserve that invariant while fixing the unbounded event-name scan above, and do not describe the mutex-backed path as non-blocking or hard-real-time-safe. The common design allocates, hits an RNG (uuid + sampling), and runs `before_send` inline; we defer that work to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead of special-casing it in the sender - the 429/quota hold would then compose as "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable logging across the C interface.
