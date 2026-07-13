# posthog-c

This is an **unofficial** small, embeddable **PostHog SDK** for C and C++.

> **Status:** pre-1.0. Built and tested on Windows, macOS, and Linux in CI, including a live end-to-end test that delivers a real event over each platform's TLS backend.

**What it's for:** Native apps that can't take a telemetry stall on a hot thread - game engines, audio and creative tools, desktop and embedded software. `ph_capture()` copies your event into a fixed-size ring and returns; on the calling thread it never allocates, reads the wall clock, uses RNG, or touches the network. Serialization, gzip, HTTP, and retries all run on a background sender thread. That guarantee is the reason this exists instead of a few lines of `curl` against PostHog's HTTP API.

This is a source-distributed SDK: compile it and its headers with your application. No prebuilt binaries or cross-release source/binary compatibility are planned.

## Highlights

- **Native and WASM backends.** native provides HTTP transport + a background sender thread + an on-disk offline queue; wasm is a synchronous shim over a helper-initialized posthog-js client.
- **Native has no SDK dependency.** It follows PostHog's raw HTTP endpoints; WASM deliberately delegates delivery to host-loaded posthog-js.
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

For WASM, initialize posthog-js through the supported host helper before calling
`ph_init()`. The helper publishes a frozen, versioned descriptor containing the
actual client reference, so named posthog-js instances work and Closure cannot
rename the host ABI:

```js
import posthog from "posthog-js"
import { initPostHogC } from "./posthog-c/wasm/posthog-c-host.mjs"

initPostHogC(posthog, {
  api_key: "phc_your_project_key",
  api_host: "https://us.i.posthog.com",
  distinct_id: installId,
  person_profiles: "identified_only", // or "always" / "never"
  preload_flags: true,
  send_feature_flag_events: true,
  release: "myapp@1.2.3",
  posthog_config: { autocapture: false },
})
```

The C config must carry the same API key, normalized host, distinct ID, profile,
flag, release, and GeoIP policy. `ph_init()` validates all of them before it
commits callbacks or enabled state. A disabled C initialization performs no
descriptor reads and makes no calls to the already-loaded posthog-js client.
Call the helper on a fresh default or named client: it rejects posthog-js's
otherwise-silent reinitialization no-op, forces the bootstrap ID to remain
anonymous, and verifies the live client config before publishing the
descriptor. While the C SDK is initialized, do not use `client.set_config()` to
replace the token, host, profile, flag, or `before_send` settings. Every bridge
call rechecks those fields and fails closed if the validated privacy pipeline
was changed; `ph_shutdown()` releases the module's pinned descriptor.

### Backend contract

| Behavior | Native | WASM |
|---|---|---|
| Delivery | SDK sender, retry, gzip, offline spill | host-loaded posthog-js |
| `ph_capture` | bounded enqueue; no heap/wall clock/RNG | synchronous JS bridge; may allocate |
| privacy stages | C denylist + `before_send`; sender thread (exceptions pre-enqueue) | C denylist + `before_send` on caller, then host final scrub after browser enrichment |
| flags | SDK `/flags/` cache | posthog-js flag cache |
| `release` | optional serializer enrichment | host finalizer enrichment |
| `disable_geoip` | forced on events and `/flags/` | forced on events; `/flags/` requires an acknowledged proxy policy |
| flag exposure policy | SDK emits deduped event | passed as per-read `{ send_event }` |
| numeric rate limit | posthog-c token bucket | ignored; descriptor acknowledges posthog-js ownership |
| stats, dropped count | implemented by posthog-c | ignored / returns 0 |
| `ph_flush`, `ph_shutdown` | flushes; shutdown gets one request-timeout drain budget, then spill/drop | no-op / releases shim state only |

Both backends share the fixed C API, typed property encoding, denylist behavior,
and event/control-property privacy tests. Timestamps, UUIDs, automatic
properties, batching, profiles, flag evaluation, retry, and persistence belong
to each backend's delivery owner and are not byte-for-byte equivalent.

Native `ph_reload_feature_flags_async()` coalesces work on the sender and
returns a bounded-history request ID whose status distinguishes pending,
success, failure, and an identity/group context that superseded the request.
The legacy `ph_reload_feature_flags()` remains the blocking wrapper. posthog-js
does not expose compatible per-request completion, so enabled WASM returns
`PH_ERR` from the asynchronous form and keeps the existing host reload call in
the void wrapper.

WASM configuration is intentionally split by owner. The C shim owns `enabled`,
the C `before_send`/denylist stage, `on_log`, super properties, and whether a
flag read asks posthog-js to emit an exposure. The host helper owns posthog-js
bootstrap, identity/profile/preload parity, release enrichment, and the final
browser scrubber. Native delivery knobs (`flush_at`, intervals, batch/queue and
body caps, request timeout/retries, gzip, offline/crash settings, numeric rate
limit, and stats callbacks) are ignored on WASM because posthog-js owns that
pipeline.

The two scrubber stages see different data. The C hook runs first and sees only
bounded C caller/super/control properties. `final_before_send` and
`final_property_denylist` passed to `initPostHogC()` run later, after posthog-js
adds browser properties; they are the privacy boundary for automatic browser
enrichment. Removing the required event name or token, or changing/removing the
SDK-selected distinct ID, drops the whole event. The validated person-profile
bit and a configured GeoIP event opt-out are restored after those hooks, so
neither policy can be weakened accidentally. On native, `before_send` likewise
runs before optional automatic `$os`, `arch`, and `release` enrichment; use the
C denylist to suppress those automatic fields. SDK-owned identity, library, and
person-profile fields cannot be replaced by caller, super, or scrubber props.

For strict WASM GeoIP opt-out, route posthog-js through a proxy that injects
`geoip_disable: true` into `/flags/` requests, then declare that capability:

```js
initPostHogC(posthog, {
  // ...matching fields above...
  disable_geoip: true,
  geoip_flags: "proxy-inject-v1",
})
```

The helper forces `$geoip_disable: true` on events and rejects initialization
without that explicit flags capability. The capability is a host assertion; the
C/WASM module cannot inspect or enforce reverse-proxy behavior itself.

## Build

Requires [Zig](https://ziglang.org) 0.16.0.

```sh
zig build              # static lib (zig-out/lib) + headers + examples
zig build test         # build and run the native test suite
zig build test-wasm    # strict-C11 normal + Closure WASM behavioral/consumer tests
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

The real host-finalizer contract is kept separate from the fast mock harness
and pins the exact posthog-js version used by CI:

```sh
cd tests/wasm/host-contract
npm ci --ignore-scripts --no-audit --no-fund
cd ../../..
zig build test-wasm-host
```

For source consumption with Emscripten, [`wasm/posthog-wasm.rsp`](wasm/posthog-wasm.rsp)
is the canonical source-and-compile-flag recipe. Invoke it from the posthog-c
checkout root, then add your application's sources and link/runtime choices:

```sh
emcc @wasm/posthog-wasm.rsp /absolute/path/to/app.c -O2 -o app.js
```

The recipe keeps the backend on strict C11 and is also what `zig build
test-wasm` consumes. [`tests/wasm/consumer/main.c`](tests/wasm/consumer/main.c)
is the minimal public-header-only consumer fixture. For a C++ application,
compile the SDK recipe as C in a separate step, then link those objects into the
C++ module; the recipe's intentional `-std=c11` flag is not a C++ driver flag.

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
exe.root_module.linkLibrary(ph.artifact("posthog")); // static lib; bundles its OS libs
exe.root_module.addIncludePath(ph.path("include"));  // <posthog.h> / <posthog.hpp>
// Linux: link the system OpenSSL (libssl-dev) yourself - the SDK leaves its one
// third-party shared dependency to your final binary.
if (target.result.os.tag == .linux) {
    exe.root_module.linkSystemLibrary("ssl", .{});
    exe.root_module.linkSystemLibrary("crypto", .{});
}
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
