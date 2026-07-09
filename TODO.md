# TODO / roadmap

The forward-looking backlog: what's next and why. The staged "what each stage
adds" view and the architecture behind it live in [DESIGN.md](DESIGN.md); this
file is the actionable list and the reasoning. Shipped work lives in the git
log, not here.

Priorities: 🔴 high · 🟡 medium · 🟢 low.

## signal_crash follow-ups 🟡

v0.6 shipped the in-process `signal_crash` handler - POSIX signals
(`SIGSEGV`/`SIGABRT`/`SIGBUS`/...) and the Windows unhandled-exception filter ->
a persisted `$exception` replayed on the next launch, with module+offset frames.
See "Native crash capture" in [DESIGN.md](DESIGN.md). What's left on this path:

- **Out-of-process `minidump_crash` 🟡.** In-process capture can't survive heap
  corruption and takes the loader lock during the per-frame module lookup (a
  crash *inside* the loader can stall the handler). The robust answer is a
  Crashpad-style out-of-process handler writing a minidump, symbolicated
  server-side by the separate **`posthog-crash`** service against a symbol store -
  that is where function *names* come from (we emit module+offset). Pick the
  backend at compile time. Prior art:
  [Breakpad](https://chromium.googlesource.com/breakpad/breakpad/),
  [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/).
- **Crash-time timestamps 🟢.** The replayed `$exception` is stamped at
  next-launch time, not when the crash happened. Record a wall-clock estimate in
  the crash record and inject it as the event `timestamp` at replay (needs an
  explicit-timestamp path through the serializer).
- **POSIX runtime verification 🟢.** The Windows SEH path is validated against a
  real fault end-to-end; the POSIX `sigaction` path is compile-checked (Linux
  cross-build) and still needs a real-fault integration test on the Linux/macOS
  CI runners (a subprocess that faults, mirroring the Windows one). glibc
  `backtrace()` is absent on musl - document or guard.

## Client-side drop reporting 🟡

`ph_dropped_events()` is a single local counter. Emit a periodic self-report of
what we discarded, broken down by reason (rate-limited / queue-overflow /
`before_send`) and category, so client-side loss is visible on the server
instead of silently invisible. Low risk; purely additive telemetry.

## Linux/macOS TLS 🟢

v0.2 shipped WinHTTP on Windows; Linux (vendored BearSSL) and macOS (Secure
Transport / NSURLSession) are stubbed - `ph_tls_send`/`ph_tls_fetch` return `-1`
off-Windows. Can't be verified from a Windows dev box. See the TLS-library
tradeoff in DESIGN.md.

## Deliberately out of scope (don't re-litigate)

- **Elaborate on-disk run directories, lockfiles, and tiered cache pruning** (as
  full crash-reporting stacks carry): they exist to persist crash state +
  minidump attachments and to tell a crashed process from a running one. Our
  single bounded NDJSON spill plus a one-shot crash record is the right size for
  in-process `signal_crash`; richer on-disk state is a `minidump_crash` concern.
- **Discarding 5xx**: we retry 5xx with backoff - the better default for a
  transient server blip. Keep it.
- **Session replay**: no DOM/canvas from C, and a privacy liability. Excluded
  (see the session-replay tradeoff in DESIGN.md).

## Keep strong (don't regress)

Where posthog-c is deliberately stronger than a general-purpose telemetry SDK,
for its game-engine niche:
- **Hot-path-safe capture** - no `malloc`/clock/RNG on the caller thread. The
  common design allocates, hits an RNG (uuid + sampling), and runs `before_send`
  inline behind locks; we defer all of that to the sender.
- **Bounded drop-oldest ring** - predictable memory, versus an unbounded
  in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead
  of special-casing it in the sender - the 429/quota hold would then compose as
  "route to the disk transport while blocked".
- **Generated string<->enum converters** for public enums - cheap human-readable
  logging across the ABI.

## Further prior-art dives (if useful)

Open-source projects that stress axes worth studying: **Tracy** (fanatical
lock-free hot-path capture, game-relevant), **librdkafka** (background producer +
backpressure/queue semantics), **Steamworks / Discord Game SDK** (game-platform
call-and-pump C ABI), **OpenTelemetry C++** (the genericity counterpoint).
