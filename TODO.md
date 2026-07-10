# TODO

## Crash

- **Out-of-process `minidump_crash`.** In-process capture can't survive heap corruption and takes the loader lock during the per-frame module lookup (a crash *inside* the loader can stall the handler). The robust answer is a Crashpad-style out-of-process handler writing a minidump, symbolicated server-side by a separate **`posthog-crash`** service against a symbol store - that is where function *names* come from (we emit module+offset). Pick the backend at compile time. Prior art: [Breakpad](https://chromium.googlesource.com/breakpad/breakpad/), [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/).
- **Crash-time timestamps.** The replayed `$exception` is stamped at next-launch time, not when the crash happened. Record a wall-clock estimate in the crash record and inject it as the event `timestamp` at replay (needs an explicit-timestamp path through the serializer).
- **macOS fault-PC and musl `backtrace()`.** The POSIX `sigaction` path now passes its real-fault integration test on both the Linux and macOS CI runners - a subprocess raises `SIGABRT`, our handler records it and chains to a host handler, and the record is decoded on replay (previously this was only compile-checked off-Windows). Two gaps remain: on macOS the faulting-instruction PC is not extracted (reading it through `uc_mcontext` faulted the arm64 handler, so we fall back to `backtrace()`; restore an alignment-safe read validated on real hardware), and glibc `backtrace()` is absent on musl/Alpine (document or guard).

## Client-side drop reporting

`on_stats` now emits a periodic local JSON snapshot broken down by reason. The
remaining work is to decide whether an opt-in SDK health event should send that
snapshot to PostHog itself, and to close the accounting gaps listed above.

## Linux/macOS TLS

Linux (vendored BearSSL) and macOS (Secure Transport / NSURLSession) are stubbed - `ph_tls_send`/`ph_tls_fetch` return `-1` off-Windows. See the TLS-library tradeoff in DESIGN.md.

## Deliberately out of scope (don't re-litigate)

- **Elaborate on-disk run directories, lockfiles, and tiered cache pruning** (as full crash-reporting stacks carry): they exist to persist crash state + minidump attachments and to tell a crashed process from a running one. Our single bounded NDJSON spill plus a one-shot crash record is the right size for in-process `signal_crash`; richer on-disk state is a `minidump_crash` concern.
- **Discarding 5xx**: we retry 5xx with backoff - the better default for a transient server blip. Keep it.
- **Session replay**: no DOM/canvas from C, and a privacy liability. Excluded (noted as out of scope in DESIGN.md).

## Keep strong (don't regress)

Where posthog-c is deliberately stronger than other telemetry SDKs:
- **Hot-path-safe capture** - no `malloc`/clock/RNG on the caller thread. The common design allocates, hits an RNG (uuid + sampling), and runs `before_send` inline behind locks; we defer all of that to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead of special-casing it in the sender - the 429/quota hold would then compose as "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable logging across the C interface.

