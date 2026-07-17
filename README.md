# posthog-c

[![CI](https://github.com/shoelle/posthog-c/actions/workflows/ci.yml/badge.svg)](https://github.com/shoelle/posthog-c/actions/workflows/ci.yml) [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE) ![C11](https://img.shields.io/badge/C-C11-informational.svg) ![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux%20%7C%20WASM-lightgrey.svg)

**Product analytics, feature flags, error tracking, and crash capture for C and C++ - safe to call from a frame loop.**

posthog-c is an unofficial, embeddable [PostHog](https://posthog.com) SDK for native applications: game engines, audio and creative tools, desktop and embedded software - anywhere telemetry must never stall a hot thread. `ph_capture()` copies the event into a preallocated ring and returns; JSON, gzip, HTTP, retries, and offline replay all run on a background sender thread. That guarantee is the reason this exists instead of a few lines of `curl` against PostHog's HTTP API.

> **Status:** pre-1.0, source-distributed - compile the SDK and headers with your application. No prebuilt binaries or cross-release compatibility promises yet.

## Highlights

- **Hot-path-safe capture.** No allocation, wall-clock read, RNG, disk, or network on the calling thread - a bounded copy under two short mutexes. (Not lock-free, and honest about it: see [DESIGN.md](DESIGN.md).)
- **Zero third-party dependencies.** Rides PostHog's raw HTTP API over each platform's own TLS - WinHTTP, Secure Transport, OpenSSL.
- **Built for bad networks.** Bounded drop-oldest ring, retries with backoff, 429/Retry-After and quota backpressure, and an on-disk offline queue that replays on reconnect.
- **Crash and error tracking.** Structured PostHog `$exception` events, plus an opt-in signal/SEH crash handler that persists a fatal crash and ships it on the next launch.
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

The helper verifies the live client and publishes a frozen descriptor; `ph_init()` - called with a matching C config - validates and pins it, and bridge calls fail closed if the host contract changes afterwards. Bootstrap variants (async loaders, the queueing snippet, classic scripts) and the full integration guide live in [`wasm/README.md`](wasm/README.md).

### What's different on WASM

Same C API, different delivery owner - native runs the whole pipeline itself, WASM stays a thin bridge over the page's posthog-js. In practice:

- **Capture is a synchronous JS call.** The native hot-path guarantee does not apply.
- **Delivery knobs are native-only.** Batching, timeouts, retries, gzip, offline spill, rate limiting, and drop/stats counters belong to posthog-js on WASM; `ph_flush()` is a no-op and `ph_shutdown()` just releases the bridge.
- **Privacy scrubs in two stages.** Both backends run your C denylist + `before_send` and shape properties identically (same serializer); WASM adds a final host-side scrub after posthog-js's browser enrichment.
- **Flags work on both.** Native keeps its own `/flags/` cache; WASM reads posthog-js's. `ph_reload_feature_flags()` blocks on native, schedules a reload on WASM.
- **Envelopes are equivalent, not byte-identical.** Timestamps, UUIDs, and automatic properties come from whichever side delivers.

The field-by-field contract - GeoIP, release enrichment, exposure events, and the rest - is tabulated in [`wasm/README.md`](wasm/README.md).

## Build

Builds with [Zig](https://ziglang.org) 0.16.0 out of the box. Ask an agent to adapt it to your toolchain of choice.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # native test suite (mock transport, no network)
zig build test-wasm    # WASM backend: strict-C11 normal + Closure builds (needs emsdk)
zig build fuzz         # fuzz the two network-facing parsers (JSON + HTTP)
zig build run-example  # run the C quickstart
```

The WASM build requires [emsdk](https://emscripten.org) (auto-detected via `$EMSDK` or `~/emsdk`) and is skipped if emcc isn't found. Consuming the SDK from an Emscripten application - the source recipe, module ordering, and the pinned posthog-js contract test - is documented in [`wasm/README.md`](wasm/README.md).

### Setting up a Zig dependency


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

## License

[MIT](LICENSE)
