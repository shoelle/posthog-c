# AGENTS.md

Repo-local guidance for coding agents working in `posthog-c/`.

## Canonical docs

- [`CLAUDE.md`](CLAUDE.md) - the coding brief: conventions, module map, invariants.
- [`DESIGN.md`](DESIGN.md) - architecture, event/wire model, roadmap, tradeoffs.
- [`README.md`](README.md) - public-facing intro + quick start.
- [`include/posthog.h`](include/posthog.h) - the public C ABI (the contract).

When they conflict, prefer `DESIGN.md`, then `CLAUDE.md`, then inline comments,
then `README.md`.

## Snapshot

- Embeddable **PostHog SDK for C/C++**. One public C ABI; a native backend (own
  HTTP + background sender) and a WASM backend (an `EM_ASM` shim over
  `window.posthog`).
- Implemented in **C11** with a header-only C++ convenience wrapper.
- **Zig** is the single build entry point. Native targets Windows/macOS/Linux;
  WASM via Emscripten (`zig build test-wasm`).
- Status: native capture, privacy/reliability (scrubber, denylist, rate limit,
  server backpressure), offline queue, gzip, HTTPS on Windows, the WASM backend,
  error tracking, feature flags, and an in-process crash handler (signals / SEH)
  are done and tested. Main gaps: Linux/macOS TLS and the out-of-process minidump
  pipeline. See the roadmap in `DESIGN.md`.

## Build and test

```sh
zig build          # static lib + headers + examples
zig build test     # native headless test suite
zig build test-wasm # WASM backend (emcc) + Node parity harness; needs emsdk
```

Get `zig build test` green before finishing any change that touches code. Run
`zig build test-wasm` too when a change touches the shared serializer or the
wasm shim.

## Working rules

- Keep `ph_capture` hot-path-safe: no `malloc`, wall clock, or RNG on the caller
  thread (invariant #1 in `CLAUDE.md`).
- All event JSON shaping lives in `ph_serialize.c` and must stay pure and
  testable - that's how native/wasm parity is guaranteed.
- Don't freelance the wire format; it's fixed by PostHog's ingestion API.
- Public API is `ph_*`; internal cross-file symbols are `ph__*`; file-local
  helpers are `static`. Additive ABI changes only within a major version.
- Add a test in the same change as new behavior. Prefer the serializer or the
  mock transport over a live server.
- One toolchain (Zig). Don't introduce a parallel build system.

## Review priorities

- hot-path allocations/clock/RNG sneaking into capture
- wire-shape drift from the ingestion API
- native/wasm parity risk
- queue + flush thread-safety and lock ordering
- unbounded growth / missing fixed caps
