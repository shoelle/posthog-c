# TODO / roadmap

The forward-looking backlog: what's next and why. The staged "what each stage
adds" view and the architecture behind it live in [DESIGN.md](DESIGN.md); this
file is the actionable list and the reasoning. Shipped work lives in the git
log, not here.

Priorities: 🔴 high · 🟡 medium · 🟢 low.

## Crash handling (v0.6) 🔴

Native crash capture — POSIX signals (`SIGSEGV`/`SIGABRT`/`SIGBUS`/…) and the
Windows unhandled-exception filter — that turns a crash into a persisted
`$exception` and sends it on the next run. It builds on the existing
`ph_capture_exception` → `$exception_list` path (v0.5); the new work is the OS
hook, done **async-signal-safely**.

Prior art / techniques to lean on:
- **In-process vs out-of-process.** In-process signal handlers are the simplest
  first cut; an out-of-process handler (the model [Google
  Breakpad](https://chromium.googlesource.com/breakpad/breakpad/) and its
  successor [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/) use)
  survives heap corruption and yields minidumps — a reasonable later escalation.
  Pick the backend at compile time.
- **Async-signal-safety** (`man 7 signal-safety`): no `malloc`/locks/stdio inside
  the handler. The usual trick is a pre-`mmap`'d page allocator for any
  unavoidable allocation.
- **Two-phase handler**: the signal handler only snapshots the crash context and
  hands off to a thread; unwinding, serialization, and writing happen off-signal.
- **Pre-serialized breadcrumbs**: append recent breadcrumbs to a rotating file
  during the normal run so crash time needs no lock — the same lock-free instinct
  as our capture path.
- Crash → envelope on disk → replayed next run; our offline spill already does
  the replay half.

## Client-side drop reporting 🟡

`ph_dropped_events()` is a single local counter. Emit a periodic self-report of
what we discarded, broken down by reason (rate-limited / queue-overflow /
`before_send`) and category, so client-side loss is visible on the server
instead of silently invisible. Low risk; purely additive telemetry.

## Linux/macOS TLS 🟢

v0.2 shipped WinHTTP on Windows; Linux (vendored BearSSL) and macOS (Secure
Transport / NSURLSession) are stubbed — `ph_tls_send`/`ph_tls_fetch` return `-1`
off-Windows. Can't be verified from a Windows dev box. See DESIGN.md §7.1.

## Deliberately out of scope (don't re-litigate)

- **Elaborate on-disk run directories, lockfiles, and tiered cache pruning** (as
  full crash-reporting stacks carry): they exist to persist crash state +
  minidump attachments and to tell a crashed process from a running one. Our
  single bounded NDJSON spill is the right size until crash handling lands.
- **Discarding 5xx**: we retry 5xx with backoff — the better default for a
  transient server blip. Keep it.
- **Session replay**: no DOM/canvas from C, and a privacy liability. Excluded
  (DESIGN.md §7.6).

## Keep strong (don't regress)

Where posthog-c is deliberately stronger than a general-purpose telemetry SDK,
for its game-engine niche:
- **Hot-path-safe capture** — no `malloc`/clock/RNG on the caller thread. The
  common design allocates, hits an RNG (uuid + sampling), and runs `before_send`
  inline behind locks; we defer all of that to the sender.
- **Bounded drop-oldest ring** — predictable memory, versus an unbounded
  in-memory worker queue.

## Nice-to-have

- **Model offline-spill as a transport** behind the `ph_transport` seam instead
  of special-casing it in the sender — the 429/quota hold would then compose as
  "route to the disk transport while blocked".
- **Generated string↔enum converters** for public enums — cheap human-readable
  logging across the ABI.

## Further prior-art dives (if useful)

Open-source projects that stress axes worth studying: **Tracy** (fanatical
lock-free hot-path capture, game-relevant), **librdkafka** (background producer +
backpressure/queue semantics), **Steamworks / Discord Game SDK** (game-platform
call-and-pump C ABI), **OpenTelemetry C++** (the genericity counterpoint).
