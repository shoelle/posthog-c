# posthog-c

This is an **unofficial** small, embeddable **PostHog SDK** for C and C++.

> **Status:** pre-1.0, lightly tested on Windows.

This is a source-distributed SDK: compile it and its headers with your application. No prebuilt binaries or cross-release source/binary compatibility are promised while the project remains unofficial and 0.x.

## Details

- **Native and WASM backends.** native provides HTTP transport + a background sender thread + an on-disk offline queue; wasm is a thin shim over the browser's already-loaded `window.posthog`.
- **No dependency on other PostHog SDKs.** Follows PostHog's documented HTTP endpoints (`/batch/`, `/i/v0/e/`, `/flags/`).
- **Never on the hot path.** `ph_capture()` copies into a bounded ring and returns - real work (JSON, networking, allocation, clock read, RNG) happens on the background worker thread. Should be safe to call from a real time / simulation loop.
- **Privacy-first.** Anonymous by default, a `before_send` scrubber hook, a property denylist, and a master kill-switch (see "Privacy & reliability" in [DESIGN.md](DESIGN.md)).

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

See [`examples/quickstart.c`](examples/quickstart.c) and [`examples/quickstart.cpp`](examples/quickstart.cpp).

## Build

Requires [Zig](https://ziglang.org) 0.15.2.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # build and run the native test suite
zig build test-wasm    # build the WASM backend (emcc) + run the Node parity harness
zig build fuzz         # fuzz the two network-facing parsers (JSON + HTTP)
zig build run-example  # run the C quickstart
```

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

## Roadmap

TODO: Linux/macOS TLS. See [TODO.md](TODO.md) for more.

## License

[MIT](LICENSE).
