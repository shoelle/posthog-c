# posthog-c

[![CI](https://github.com/shoelle/posthog-c/actions/workflows/ci.yml/badge.svg)](https://github.com/shoelle/posthog-c/actions/workflows/ci.yml) [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE) ![C11](https://img.shields.io/badge/C-C11-informational.svg) ![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux%20%7C%20WASM-lightgrey.svg)

**Product analytics, feature flags, error tracking, and crash capture for C and C++ - safe to call from a frame loop.**

posthog-c is an unofficial, embeddable [PostHog](https://posthog.com) SDK for native applications: game engines, audio and creative tools, desktop and embedded software - anywhere telemetry must never stall a hot thread. `ph_capture()` copies the event into a preallocated ring and returns; JSON, gzip, HTTP, retries, and offline replay all run on a background sender thread. That guarantee is the reason this exists instead of a few lines of `curl` against PostHog's HTTP API.

> **Status:** pre-1.0, source-distributed - compile the SDK and headers with your application. No prebuilt binaries or cross-release compatibility promises yet.

## Highlights

- **Hot-path-safe capture.** No allocation, wall-clock read, RNG, disk, or network on the calling thread - a bounded copy under two short mutexes. (Not lock-free, and honest about it: see [DESIGN.md](DESIGN.md).)
- **Zero third-party dependencies.** Rides PostHog's raw HTTP API over each platform's own TLS - WinHTTP, Secure Transport, OpenSSL. The one vendored file is a small deflate encoder for gzip bodies.
- **Built for bad networks.** Bounded drop-oldest ring, retries with backoff, 429/Retry-After and quota backpressure, and an on-disk offline queue that replays on reconnect.
- **Crash and error tracking.** Structured `$exception` events, plus an opt-in signal/SEH crash handler that persists a fatal crash and ships it on the next launch.
- **Feature flags.** Remote evaluation with a local cache, JSON payloads, and deduped exposure events.
- **Privacy-first.** Anonymous by default, a `before_send` scrubber, a property denylist, and a master kill switch.
- **Native and WASM.** The same C API in the browser, delegating delivery to host-loaded posthog-js.

## Quick start

Point the SDK at your project and give it a stable anonymous id:

```c
#include "posthog.h"

ph_config cfg;
ph_config_defaults(&cfg);        // api_host defaults to https://us.i.posthog.com
cfg.api_key = "phc_your_project_key";
cfg.distinct_id = "install-id-you-persist";
ph_init(&cfg);
```

Capture from anywhere - including the middle of a frame:

```c
ph_props p;
ph_props_init(&p);
ph_props_set_str(&p, "weapon", "railgun");
ph_props_set_int(&p, "combo", 17);
ph_capture("boss_defeated", &p);
// lands in PostHog as "boss_defeated" with your props plus $os, arch, $lib, timestamp, uuid
```

Attach a signed-in identity, then drain on the way out:

```c
ph_identify("user-123", NULL); // later events belong to this person
ph_flush(2000);                // block until the queue drains (or 2s)
ph_shutdown();
```

Or the same through the header-only C++ wrapper:

```cpp
#include "posthog.hpp"

posthog::Session session(cfg); // RAII: ph_init now, bounded-drain shutdown on scope exit
posthog::capture("boss_defeated", posthog::Props().str("weapon", "railgun").i64("combo", 17));
posthog::identify("user-123");
```

Complete runnable programs live in [`examples/quickstart.c`](examples/quickstart.c) and [`examples/quickstart.cpp`](examples/quickstart.cpp). One rule to remember: `distinct_id` is a random install id that *you* persist; `ph_identify()` upgrades it at sign-in, and `ph_reset()` rolls a fresh anonymous id at logout (read it back with `ph_get_distinct_id()` and persist it).

## Flags, errors, and crashes

Feature flags evaluate remotely, cache locally, and emit deduped exposure events:

```c
if (ph_is_feature_enabled("new-renderer", 0)) // 0 = fallback while offline/unknown
    renderer_use_vulkan_path();
```

Caught something worth tracking in PostHog Error Tracking? Ship a structured `$exception`:

```c
ph_exception ex = {0};
ex.type = "AssetLoadFailure";
ex.message = "manifest.json missing from pak 3";
ex.handled = 1;
ph_capture_exception(&ex);
```

And for the errors nobody catches: opt in to the crash handler, and a fatal signal/SEH fault is persisted locally and shipped as a `$exception` on the next launch:

```c
cfg.offline_path = "path/to/writable/dir"; // also enables the offline event queue
cfg.crash_handler = 1;
```

## WASM

The same C API compiles with Emscripten into a shim over a host-loaded posthog-js client: delivery, persistence, and browser enrichment stay in the JS SDK, and typed C properties serialize through the same encoder the native backend uses. Initialize posthog-js through the host helper before `ph_init()`:

```js
import posthog from "posthog-js"
import { initPostHogC } from "./posthog-c/wasm/posthog-c-host.mjs"

initPostHogC(posthog, {
  api_key: "phc_your_project_key",
  api_host: "https://us.i.posthog.com",
  distinct_id: installId,
})
```

The helper verifies the live client, then publishes a frozen descriptor that `ph_init()` - called with a matching C config - validates and pins; bridge calls fail closed if the host contract changes afterwards. Async and classic-script bootstraps, the config ownership split, the two privacy-scrubber stages, strict GeoIP opt-out, and the Emscripten build recipe are covered in [`wasm/README.md`](wasm/README.md).

### Backend contract

| Behavior | Native | WASM |
|---|---|---|
| Delivery | SDK sender, retry, gzip, offline spill | host-loaded posthog-js |
| `ph_capture` | bounded enqueue; no heap/wall clock/RNG | synchronous JS bridge; may allocate |
| privacy stages | C denylist + `before_send`; sender thread (exceptions pre-enqueue) | C denylist + `before_send` on caller, then host final scrub after browser enrichment |
| flags | SDK `/flags/` cache | posthog-js flag cache |
| `release` | optional serializer enrichment | host finalizer enrichment |
| `disable_geoip` | forced on events and `/flags/` | forced on events; `/flags/` requires an explicit proxy host and policy |
| flag exposure policy | SDK emits deduped event | passed as per-read `{ send_event }` |
| numeric rate limit | posthog-c token bucket | ignored; descriptor acknowledges posthog-js ownership |
| stats, dropped count | implemented by posthog-c | ignored / returns 0 |
| `ph_flush`, `ph_shutdown` | flushes; shutdown gets one request-timeout drain budget, then spill/drop | no-op / releases shim state only |

Both backends share the fixed C API, typed property encoding, denylist behavior, and event/control-property privacy tests. Timestamps, UUIDs, automatic properties, batching, profiles, flag evaluation, retry, and persistence belong to each backend's delivery owner and are not byte-for-byte equivalent.

`ph_reload_feature_flags()` re-evaluates flags after an identity or group change: native queues the refresh on the sender and blocks until it completes; WASM schedules a posthog-js reload.

## Build

Requires [Zig](https://ziglang.org) 0.16.0 - the only toolchain, and it brings its own C compiler.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # native test suite (mock transport, no network)
zig build test-wasm    # WASM backend: strict-C11 normal + Closure builds (needs emsdk)
zig build fuzz         # fuzz the two network-facing parsers (JSON + HTTP)
zig build run-example  # run the C quickstart
```

The WASM build requires [emsdk](https://emscripten.org) (auto-detected via `$EMSDK` or `~/emsdk`) and is skipped if emcc isn't found. Consuming the SDK from an Emscripten application - the source recipe, module ordering, and the pinned posthog-js contract test - is documented in [`wasm/README.md`](wasm/README.md).

### Using it

Add posthog-c as a Zig dependency:

```zig
// your build.zig.zon
.dependencies = .{
    .posthog_c = .{ .path = "third_party/posthog-c" },
    // or fetched: .{ .url = "https://github.com/shoelle/posthog-c/archive/<rev>.tar.gz", .hash = "..." },
},
```

```zig
// your build.zig
const ph = b.dependency("posthog_c", .{ .target = target, .optimize = optimize });
exe.root_module.linkLibrary(ph.artifact("posthog")); // static lib; bundles its OS libs
exe.root_module.addIncludePath(ph.path("include"));  // <posthog.h> / <posthog.hpp>
// Linux: link the system OpenSSL (libssl-dev) yourself - the SDK leaves its one
// third-party shared dependency to your final binary.
if (target.result.os.tag == .linux) {
    exe.root_module.linkSystemLibrary("ssl", .{});
    exe.root_module.linkSystemLibrary("crypto", .{});
}
```

The native SDK owns threads and mutexes and is not fork-safe after `ph_init()`. On POSIX, shut it down before `fork()` and initialize a fresh instance in each process that will emit events; do not call the inherited instance in the child.

## How it's tested

Every push builds and runs the mock-transport suite on Windows, macOS, and Linux; ASan/UBSan, parser fuzzing, a downstream Zig-consumer build, and the WASM parity harness run on Linux. Pushes to `main` add the **live contract**: each of the three desktops delivers a real event and reads a real flag against a live PostHog project, so every TLS backend - WinHTTP, Secure Transport, OpenSSL - is proven end to end, not just compiled.

Run the live contract yourself with a disposable `POSTHOG_API_KEY`: copy [`.env.example`](.env.example) to `.env` (gitignored) and run [`scripts/live-contract.ps1`](scripts/live-contract.ps1) on Windows or `scripts/live-contract.sh` elsewhere.

## Roadmap

Cross-platform TLS (Windows/macOS/Linux), feature flags, offline spill, and signal-crash capture are in. Next: out-of-process crash handling with server-side symbolication, and crash-time timestamps. See [TODO.md](TODO.md).

## License

[MIT](LICENSE).
