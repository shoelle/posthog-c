# posthog-c - Design

Why the SDK is shaped the way it is. The public contract lives in [`include/posthog.h`](include/posthog.h); this is the reasoning behind it.

## Goals

- **One C interface, two transports.** `native` owns its HTTP + a background sender thread; `wasm` is a synchronous shim over a helper-initialized posthog-js client. Callers never see the split.
- **Hot-path-safe capture (native).** `ph_capture()` copies into a preallocated ring with no allocation, wall-clock read, RNG, disk, or network on the caller. It does read the monotonic clock, increment an atomic, and take SDK/queue mutexes, so it is not lock-free or hard-real-time-safe. Everything expensive runs on the sender thread.
- **No extra dependency.** Native rides PostHog's raw ingestion API (`/batch/`, `/flags/`); the browser backend reuses posthog-js rather than ship a second network stack.
- **Embeddable.** POD config, fixed-capacity buffers, no exceptions/RTTI, a stable C ABI.

## Native pipeline

```
ph_capture (caller thread)          background sender thread
pack event -> bounded ring   --->   drain -> serialize -> gzip -> POST /batch/
(monotonic tick + mutexes)          2xx: drop batch; else spill to disk + replay
```

Identity and super-properties are baked into each event at capture, so the sender needs no shared identity state and "events captured before `ph_identify` stay anonymous" is automatic. A full ring drops the oldest event and counts it. The sender reconstructs wall-clock time from the monotonic tick, so a long-queued or offline-replayed event still reports when it was captured, not when it was sent.

Feature-flag refreshes ride the same sender. Same-context requests coalesce, and an identity/group change supersedes in-flight requests, so a slow response can never overwrite a newer user's flags. The blocking `ph_reload_feature_flags()` polls an internal request ticket and retries transparently when its context is superseded. Shutdown gives queued and offline network work one `request_timeout_ms` budget, then persists what remains (or counts it dropped when no offline path is configured).

## Key decisions

- **C11 core + header-only C++ wrapper** - maximally embeddable, FFI-friendly, no C++ runtime dependency.
- **Zig build**, with the sources exposed so a CMake shim can wrap them later.
- **TLS per platform**: each desktop links its own system TLS - WinHTTP (Windows), Secure Transport (macOS), OpenSSL (Linux) - rather than vendoring a crypto library.
- **Fixed per-event property caps** (part of the `ph_props` ABI): a pathological event drops its overflow properties (and counts them) instead of ever allocating.
- **Privacy**: anonymous by default, and every event passes a `before_send` hook + `property_denylist` scrub. SDK-owned identity/profile/library/group fields cannot be replaced by caller or super props; one shared key list drives that rule on both backends, so they shape a given `ph_props` identically. A master `enabled` kill-switch makes the whole SDK a no-op.
- **Backpressure**: the sender honors HTTP 429/503 `Retry-After` and PostHog's `200`-body quota notice, and a client-side token bucket caps what we emit in the first place.
- **Background thread over call-and-pump (native)**: an inline pump would risk blowing a frame, and the crash handler needs process-global state anyway; `ph_flush()` covers the frame-seam case. WASM instead calls posthog-js synchronously.
- **Out of scope**: session replay (nothing to record from C, and a privacy liability).

## Delivery guarantees

Native capture is at-most-once by design: a full ring drops the oldest event (and counts it), and a token bucket sheds bursts. That is the right trade for product analytics, where aggregates carry the meaning and one lost event among thousands does not. Error and crash reporting want the opposite, at-least-once: the first occurrence of a novel fault is the one event you cannot afford to drop, and drop-oldest would happily evict it to keep a repeat. The design carves out the critical case - a crash is persisted once and replayed next launch, never left in the volatile ring - but fully serving both domains would tier delivery by event importance (analytics lossy, errors prioritized and spilled, crashes durable) rather than apply one global policy. Noted, not built.

## WASM host contract

The wasm backend trusts posthog-js with delivery but verifies the integration around it. The host helper ([`posthog-c-host.mjs`](wasm/posthog-c-host.mjs), with [`posthog-c-host.js`](wasm/posthog-c-host.js) as the classic-script / CommonJS form) initializes a default or named posthog-js client, checks the live token, host, anonymous bootstrap identity, and profile/flag policy, then publishes a frozen versioned descriptor. `ph_init()` validates and pins that descriptor, and every bridge call fails closed if the live config later diverges. What that buys: accidental misconfiguration - a second `init()`, a stray `set_config()`, a competing snippet - cannot silently break the validated privacy pipeline. What it cannot buy: protection from a hostile page, which owns the JS environment outright. The contract is a guard rail, not a security boundary.

Privacy has two stages on WASM. The C denylist/`before_send` sees bounded C properties on the caller thread. The helper's final hooks see posthog-js browser enrichment, after which the helper restores SDK-owned identity/person-profile policy and optional GeoIP opt-out. Native delivery knobs remain native-only; posthog-js owns browser batching, retry, persistence, and numeric rate policy. Integration and build details live in [`wasm/README.md`](wasm/README.md).

## Crash capture (`signal_crash`, opt-in)

A fatal native fault (POSIX signal / Windows SEH) is persisted as one fixed record and replayed as a `$exception` on the next launch - a crashing process can't reliably reach the network, so the flow is crash -> persist -> replay, reusing the offline queue's replay half. Frames are stored as `(module, offset)` so they survive ASLR; turning them into function names is a future out-of-process job. In-process capture is best-effort - `backtrace`/`dladdr` aren't async-signal-safe - which is the fundamental reason robust capture is ultimately out-of-process (Crashpad).

## Status

Native delivery + bounded shutdown/offline spill, the validated WASM bridge, sender-queued feature flags, error + crash capture, and per-platform TLS (WinHTTP / Secure Transport / OpenSSL) are done. Full roadmap and known limits live in [`TODO.md`](TODO.md).
