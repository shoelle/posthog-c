# posthog-c - Design

Why the SDK is shaped the way it is. The public contract lives in [`include/posthog.h`](include/posthog.h); this is the reasoning behind it.

## Goals

- **One C interface, two transports.** `native` owns its HTTP + a background sender thread; `wasm` is a thin shim over the browser's already-loaded `window.posthog`. Callers never see the split.
- **Hot-path-safe capture (native).** `ph_capture()` is safe on a game/audio/render loop: it copies into a preallocated ring and returns - no allocation, wall-clock read, RNG, or network on the caller. Everything expensive runs on the sender thread.
- **No extra dependency.** Native rides PostHog's raw ingestion API (`/batch/`, `/flags/`); the browser backend reuses posthog-js rather than ship a second network stack.
- **Embeddable.** POD config, fixed-capacity buffers, no exceptions/RTTI, a stable C ABI.

## Native pipeline

```
ph_capture (caller thread)          background sender thread
pack event -> bounded ring   --->   drain -> serialize -> gzip -> POST /batch/
(no malloc / clock / RNG)           2xx: drop batch; else spill to disk + replay
```

Identity and super-properties are baked into each event at capture, so the sender needs no shared identity state and "events captured before `ph_identify` stay anonymous" is automatic. A full ring drops the oldest event and counts it. The sender reconstructs wall-clock time from the monotonic tick, so a long-queued or offline-replayed event still reports when it was captured, not when it was sent.

## Key decisions

- **C11 core + header-only C++ wrapper** - maximally embeddable, FFI-friendly, no C++ runtime dependency.
- **Zig build**, with the sources exposed so a CMake shim can wrap them later.
- **TLS per platform**: each desktop links its own system TLS - WinHTTP (Windows), Secure Transport (macOS), OpenSSL (Linux) - rather than vendoring a crypto library.
- **Fixed per-event property caps** (part of the `ph_props` ABI): a pathological event drops its overflow properties (and counts them) instead of ever allocating.
- **Privacy**: anonymous by default; a `before_send` hook + a `property_denylist` scrub every event sender-side; a master `enabled` kill-switch makes the SDK a no-op.
- **Backpressure**: the sender honors HTTP 429/503 `Retry-After` and PostHog's `200`-body quota notice, and a client-side token bucket caps what we emit in the first place.
- **Background thread over call-and-pump**: an inline pump would risk blowing a frame, and the crash handler needs process-global state anyway; `ph_flush()` already covers the frame-seam case.
- **Out of scope**: session replay (nothing to record from C, and a privacy liability).

## Crash capture (`signal_crash`, opt-in)

A fatal native fault (POSIX signal / Windows SEH) is persisted as one fixed record and replayed as a `$exception` on the next launch - a crashing process can't reliably reach the network, so the flow is crash -> persist -> replay, reusing the offline queue's replay half. Frames are stored as `(module, offset)` so they survive ASLR; turning them into function names is a future out-of-process job. In-process capture is best-effort - `backtrace`/`dladdr` aren't async-signal-safe - which is the fundamental reason robust capture is ultimately out-of-process (Crashpad).

## Status

Native delivery + offline spill, the WASM bridge, feature flags, error + crash capture, and per-platform TLS (WinHTTP / Secure Transport / OpenSSL) are done. Full roadmap and known limits live in [`TODO.md`](TODO.md).
