# posthog-c

This is an **unofficial** small, embeddable **PostHog SDK** for C and C++.

> **Status:** pre-1.0. Built and tested on Windows, macOS, and Linux in CI, including a live end-to-end test that delivers a real event over each platform's TLS backend.

**What it's for:** Native apps that can't take a telemetry stall on a hot thread - game engines, audio and creative tools, desktop and embedded software. `ph_capture()` copies your event into a fixed-size ring and returns; on the calling thread it never allocates, reads the wall clock, uses RNG, or touches the network. Serialization, gzip, HTTP, and retries all run on a background sender thread. That guarantee is the reason this exists instead of a few lines of `curl` against PostHog's HTTP API.

This is a source-distributed SDK: compile it and its headers with your application. No prebuilt binaries or cross-release source/binary compatibility are planned.

## Highlights

- **Native and WASM backends.** native provides HTTP transport + a background sender thread + an on-disk offline queue; wasm is a thin shim over the browser's already-loaded `window.posthog`.
- **Native has no SDK dependency.** It follows PostHog's raw HTTP endpoints; WASM deliberately delegates to the page's already-loaded posthog-js.
- **Native hot-path-safe capture.** `ph_capture()` copies into a bounded ring and returns; JSON, networking, allocation, wall-clock reads, and RNG happen on the worker. WASM calls posthog-js and `before_send` synchronously, so this guarantee is native-only.
- **Privacy-first.** Anonymous by default, a `before_send` scrubber hook, a property denylist, and a master kill-switch (see [DESIGN.md](DESIGN.md)).

## Quick start (C)

```c
#include "posthog.h"

ph_config cfg;
ph_config_defaults(&cfg);
cfg.api_key = "phc_your_project_key";
cfg.api_host = "http://localhost:8000"; // dev proxy (http); or https://us.i.posthog.com
cfg.distinct_id = "install-id-from-your-storage"; // create once and persist
ph_init(&cfg);

ph_props p;
ph_props_init(&p);
ph_props_set_str(&p, "panel", "code");
ph_props_set_int(&p, "count", 3);
ph_capture("editor_panel_opened", &p);

ph_identify("user-123", NULL); // later events attach to this identity

ph_flush(3000);  // block until the queue drains (or 3s)
ph_shutdown();
```

The equivalent through the header-only C++ wrapper:

```cpp
#include "posthog.hpp"

posthog::Session session(cfg); // ph_init here; flush + shutdown on scope exit
posthog::capture("editor_panel_opened",
                 posthog::Props().str("panel", "code").i64("count", 3));
posthog::identify("user-123");
```

See [`examples/quickstart.c`](examples/quickstart.c) and [`examples/quickstart.cpp`](examples/quickstart.cpp).

`distinct_id` is required. Supply a random install/device ID loaded from your
application's durable settings, then replace it with `ph_identify()` after sign
in. `ph_reset()` deliberately rolls a new anonymous ID on logout; read it with
`ph_get_distinct_id()` and persist it before the next launch.

For WASM, initialize posthog-js first with that same ID and expose it as
`window.__posthog_c_distinct_id` before calling `ph_init()`. The shim rejects a
mismatch so browser and native activity cannot silently split identities.

### Backend contract

| Behavior | Native | WASM |
|---|---|---|
| Delivery | SDK sender, retry, gzip, offline spill | host-loaded posthog-js |
| `ph_capture` | bounded enqueue; no heap/wall clock/RNG | synchronous JS bridge; may allocate |
| `before_send` | sender thread (exceptions pre-enqueue) | caller thread |
| flags | SDK `/flags/` cache | posthog-js flag cache |
| rate limit, stats, dropped count | implemented by posthog-c | ignored / returns 0 |
| `ph_flush`, `ph_shutdown` | drains and owns lifecycle | no-op / releases shim state only |

Both backends share the fixed C API, typed property encoding, denylist behavior,
and event/control-property privacy tests. Timestamps, UUIDs, automatic
properties, batching, profiles, flag evaluation, retry, and persistence belong
to each backend's delivery owner and are not byte-for-byte equivalent.

## Build

Requires [Zig](https://ziglang.org) 0.16.0.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # build and run the native test suite
zig build test-wasm    # build the WASM backend (emcc) + run the Node parity harness
zig build fuzz         # fuzz the two network-facing parsers (JSON + HTTP)
zig build run-example  # run the C quickstart
```

`zig build live-contract` is intentionally separate: it sends a real event and
reads a flag against a live project, so it needs `POSTHOG_API_KEY` (use a
disposable one). Copy [`.env.example`](.env.example) to `.env` (gitignored) and
run [`scripts/live-contract.ps1`](scripts/live-contract.ps1) on Windows or
`scripts/live-contract.sh` elsewhere. HTTPS delivery works on all three desktops
- WinHTTP (Windows), Secure Transport (macOS), OpenSSL (Linux, needs `libssl-dev`).

CI runs this same live contract on Windows, macOS, and Linux on every push to
`main`, so each TLS backend is exercised end to end against a real project - not
just compiled. The build and mock-transport suite run on all three platforms on
every push; ASan/UBSan, the parser fuzzers, a downstream-consumer build, and the
WASM parity harness run on Linux.

WASM build requires [emsdk](https://emscripten.org) (auto-detected via `$EMSDK` or `~/emsdk`); skipped if emcc isn't found.

### Using it

Add posthog-c as a Zig dependency:

```zig
// your build.zig.zon
.dependencies = .{
    .posthog_c = .{ .path = "third_party/posthog-c" },
    // or fetched: .{ .url = "https://github.com/<you>/posthog-c/archive/<rev>.tar.gz", .hash = "..." },
},
```

```zig
// your build.zig
const ph = b.dependency("posthog_c", .{ .target = target, .optimize = optimize });
exe.linkLibrary(ph.artifact("posthog")); // static lib; carries its own platform libs
exe.addIncludePath(ph.path("include"));  // <posthog.h> / <posthog.hpp>
```

The native SDK owns threads and mutexes and is not fork-safe after `ph_init()`.
On POSIX, shut it down before `fork()` and initialize a fresh instance in each
process that will emit events; do not call the inherited instance in the child.

## Roadmap

Cross-platform TLS (Windows/macOS/Linux), feature flags, offline spill, and
signal-crash capture are in. Next: out-of-process crash handling with
server-side symbolication, and crash-time timestamps. See [TODO.md](TODO.md).

## License

[MIT](LICENSE).
