# TODO

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
- **Allocation-free native capture** - no `malloc`, wall-clock read, RNG, disk, or network on the caller thread. Preserve that invariant and do not describe the mutex-backed path as non-blocking or hard-real-time-safe. The common design allocates, hits an RNG (uuid + sampling), and runs `before_send` inline; we defer that work to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead of special-casing it in the sender - the 429/quota hold would then compose as "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable logging across the C interface.
