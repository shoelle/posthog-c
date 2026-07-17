# TODO

## Crash

- **Out-of-process `minidump_crash`.** In-process capture can't survive heap corruption and takes the loader lock during the per-frame module lookup (a crash *inside* the loader can stall the handler). The robust answer is a Crashpad-style out-of-process handler writing a minidump, symbolicated server-side by a separate **`posthog-crash`** service against a symbol store - that is where function *names* come from (we emit module+offset). Pick the backend at compile time. Prior art: [Breakpad](https://chromium.googlesource.com/breakpad/breakpad/), [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/).
- **Crash-time timestamps.** The replayed `$exception` is stamped at next-launch time, not when the crash happened. Record a wall-clock estimate in the crash record and inject it as the event `timestamp` at replay (needs an explicit-timestamp path through the serializer).
- **macOS fault-PC and musl `backtrace()`.** Two known gaps in the POSIX handler: on macOS the faulting-instruction PC is not extracted (reading it through `uc_mcontext` faulted the arm64 handler, so it falls back to `backtrace()`; restore an alignment-safe read validated on real hardware), and glibc `backtrace()` is absent on musl/Alpine (document or guard).

## Client-side drop reporting

Decide whether an opt-in SDK health event should ship the `on_stats` snapshot
(queue depth + drops broken down by reason) to PostHog itself. WASM delivery
and loss accounting stay host-owned: posthog-js exposes no equivalent
snapshot, so `on_stats` is native-only and `ph_dropped_events()` returns zero
rather than report partial truth.

## TLS

- **The BSDs have no TLS backend** - `ph_tls_available()` returns 0 and https:// is refused. Wire OpenSSL (or LibreSSL) there if it ever matters.
- **macOS Secure Transport is deprecated** in favour of Network.framework. It works; migrating would silence the deprecation and gain async IO.
- **Linux ships a hard OpenSSL dependency.** Fine for a Linux SDK (it's the de-facto system TLS), but a vendored or optional backend would suit a no-dependencies embed. A deliberate trade, not an oversight.

## Simplification candidates

Open questions, not defects - each trades a working mechanism for less
surface:

- **Per-bridge-call host revalidation (WASM).** Every bridge call re-verifies the descriptor's method and live-config identity through `checked_client()`. Validating at init plus an explicit revalidate hook would be simpler and cheaper; the per-call check guards against accidental host reconfiguration mid-session (see the WASM host contract in DESIGN.md), not a hostile page.
- **The public flag-reload token API.** `ph_reload_feature_flags_async` + `ph_get_feature_flag_reload_status` is surface no official PostHog SDK exposes, and WASM degrades it to `PH_ERR`/`UNKNOWN`. The internal coalescing/supersession stays regardless (the blocking reload needs it to be correct across identity changes); the rationale for the public form is in DESIGN.md. Demote it to internal if it sees no real use.
- **Config surface.** `ph_config` is 25 fields; each is documented and defensible, but together they are already a mature-SDK surface. Resist further growth.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead of special-casing it in the sender - the 429/quota hold would then compose as "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable logging across the C interface.

## Settled decisions

Deliberate trades, recorded so they aren't re-opened by accident. Code-level
review decisions (reload guard placement, reload-history slots, the
`post_body` timeout branch, WASM reload-status details) live in AGENTS.md.

- **No run directories, lockfiles, or tiered cache pruning** (as full crash-reporting stacks carry): one bounded NDJSON spill plus a one-shot crash record is the right size for in-process `signal_crash`; richer on-disk state is a `minidump_crash` concern.
- **5xx is retried with backoff**, not discarded - the better default for a transient server blip.
- **No session replay**: nothing to record from C, and a privacy liability.
- **WASM `ph_capture` returns `PH_OK` even when the host finalizer drops the event**: capture is fire-and-forget on both backends and `PH_OK` means "accepted for delivery", not "delivered" - native behaves the same for denylist/`before_send`/ring drops.
- **WASM `ph_reload_feature_flags_async` returns `PH_ERR`**: posthog-js exposes no per-reload completion carrying failure and request/context identity, so a token API there would lie; the void reload is the honest portable form (see `wasm/README.md`).

## Keep strong (don't regress)

Where posthog-c is stronger than the common telemetry-SDK design:

- **Allocation-free native capture** - no `malloc`, wall-clock read, RNG, disk, or network on the caller thread. Preserve that invariant and do not describe the mutex-backed path as non-blocking or hard-real-time-safe. The common design allocates, hits an RNG (uuid + sampling), and runs `before_send` inline; we defer that work to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded in-memory worker queue.
- **One serializer, one blob parser, exact-byte batch tests** through the mock transport - wire-shape drift has nowhere to hide.
- **Crash-handler discipline** - strict handler/replay separation, the async-signal-unsafe parts named in place (with the `backtrace` warm-up), altstack install/restore etiquette, no-replace record publication so a second crash cannot erase an unacknowledged first, and handler chaining on both platforms.
- **CI rigor** - actions pinned by SHA, a 3-OS matrix plus a ReleaseSafe re-run, ASan/UBSan, parser fuzzing, a downstream-consumer build, and a live contract gated on the rest of the matrix that skips green on forks.
