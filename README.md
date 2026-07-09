# posthog-c

A small, embeddable **PostHog SDK for C and C++** - for native apps and game
engines that PostHog's existing libraries don't cover. PostHog ships JS,
Python, Go, Rust, .NET, and mobile SDKs, but nothing for a C++ engine; this
fills that gap on top of PostHog's raw ingestion API.

> **Status:** pre-1.0 and developed openly, but already broad. The C ABI,
> non-blocking capture, a background sender, and `/batch/` delivery over
> plaintext HTTP (for a local dev proxy) are working and tested - as are
> anonymous-by-default identity, a `before_send` scrubber, a property denylist,
> a capture rate limiter, server backpressure (honoring `429`/`Retry-After` and
> PostHog's quota signal), an offline disk queue (spill/replay), and gzip'd
> batches (`Content-Encoding: gzip`). HTTPS works
> on Windows (validated via WinHTTP against real `us.i.posthog.com`), and the
> **WebAssembly backend** (a shim over the browser's `window.posthog`) is
> verified for native/wasm parity under Node. Native errors ship as full PostHog
> `$exception` events (raw stack frames + mechanism); an **opt-in crash handler**
> turns a fatal native crash (POSIX signal / Windows SEH) into a `$exception`
> replayed on the next launch; and **feature flags** evaluate remotely with a
> local cache. Linux/macOS TLS and out-of-process minidump symbolication are the
> main gaps left on the [roadmap](#roadmap). This is an exploratory build,
> developed openly; see [DESIGN.md](DESIGN.md) for the plan.

## Why

- **One library, two transports.** A single public C ABI with two
  compile-time backends - **native** (owns HTTP + a background sender thread +
  an on-disk offline queue) and **wasm** (a thin shim over the browser's
  already-loaded `window.posthog`). The split hides behind one `#if`.
- **No dependency on another PostHog SDK.** Everything rides PostHog's
  documented HTTP endpoints (`/batch/`, `/i/v0/e/`, `/flags/`).
- **Never on the hot path.** `ph_capture()` copies into a bounded ring and
  returns - no JSON, no network, no `malloc`, no wall-clock read, no RNG on the
  caller thread. All I/O happens on a background thread. That keeps it safe to
  call from a game engine's simulation loop.
- **Privacy-first.** Anonymous by default, a `before_send` scrubber hook, a
  property denylist, and a master kill-switch (see "Privacy & reliability" in
  [DESIGN.md](DESIGN.md)).

## Quick start (C)

```c
#include "posthog.h"

ph_config cfg;
ph_config_defaults(&cfg);
cfg.api_key = "phc_your_project_key";
cfg.api_host = "http://localhost:8000"; // dev proxy (http); or https://us.i.posthog.com on Windows
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

See [`examples/quickstart.c`](examples/quickstart.c) and
[`examples/quickstart.cpp`](examples/quickstart.cpp).

## Build

Requires [Zig](https://ziglang.org) 0.15+ (its bundled clang builds the C, so
no separate toolchain is needed). Zig is the one build entry point.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # build and run the native test suite
zig build test-wasm    # build the WASM backend (emcc) + run the Node parity harness
zig build fuzz         # fuzz the two network-facing parsers (JSON + HTTP)
zig build run-example  # run the C quickstart
```

The native library builds on Windows, macOS, and Linux. `http://` (dev proxy)
works everywhere; `https://` currently works on Windows (WinHTTP) - Linux/macOS
TLS is on the roadmap. `zig build test-wasm` needs [emsdk](https://emscripten.org)
(auto-detected via `$EMSDK` or `~/emsdk`); it's skipped if emcc isn't found.

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs the native
suite on Windows/Linux/macOS and the WASM harness on every push.

### Consuming it

Vendor this repo as a git submodule and link the static library. From another
Zig build, `@import` this `build.zig` and call the exported helpers:

```zig
const posthog = @import("third_party/posthog-c/build.zig");
const ph = posthog.create(b, target, optimize);
exe.linkLibrary(ph);
posthog.addIncludes(b, exe); // makes <posthog.h> / <posthog.hpp> available
```

A CMake `add_subdirectory` shim can wrap the same sources for non-Zig builds.

## Layout

```
posthog-c/
├── include/       # public API: posthog.h (C ABI) + posthog.hpp (C++ wrapper)
├── src/           # implementation: native backend + ph_wasm.c (window.posthog shim)
├── tests/         # native suite (zig build test) + tests/wasm Node parity harness
├── examples/      # quickstart.c / quickstart.cpp
├── third_party/   # vendored sdefl (single-file gzip compressor; MIT / public domain)
├── build.zig      # single build entry point (also a consumable module)
├── DESIGN.md      # architecture, event/wire model, roadmap, tradeoffs
├── TODO.md        # roadmap + backlog
├── CLAUDE.md      # working notes: conventions + module map
└── AGENTS.md      # short guide for coding agents
```

## Roadmap

The native capture pipeline, privacy/reliability layer (including server
backpressure), Windows TLS, WASM shim, error tracking, feature flags, and
in-process crash capture (native signals / Windows SEH → a `$exception` on the
next launch) are in; Linux/macOS TLS and the out-of-process minidump pipeline
(the separate `posthog-crash` service) are the main open items. See
[TODO.md](TODO.md) for what's next and why, and [DESIGN.md](DESIGN.md) for the
staged plan and the architecture behind it.

## License

[MIT](LICENSE). Uses only PostHog's public HTTP API.
