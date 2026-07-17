# posthog-c on WASM

The WASM backend behind the shared C API: `src/ph_wasm.c` compiles with
Emscripten and bridges to a host-initialized posthog-js client. This document
covers everything WASM-specific - bootstrapping, the validated host contract,
configuration ownership, privacy staging, GeoIP, and the build recipe. The C
API itself is identical to native; the behavioral differences are summarized
in the backend-contract table in the [main README](../README.md), and the
design reasoning lives in [DESIGN.md](../DESIGN.md).

## How the backend works

There is no network stack in the module: posthog-js owns batching, retry,
persistence, timestamps, UUIDs, automatic browser properties, and flag
evaluation. The shim validates the descriptor published by the host helper,
pins it, serializes `ph_props` with the same encoder the native serializer
uses - so a given property set produces byte-identical property JSON on both
backends - and calls the pinned client synchronously. `ph_flush()` is a no-op
and `ph_shutdown()` releases only shim-owned state.

## Bootstrapping posthog-js

Initialize posthog-js through the host helper before calling `ph_init()`. The
helper publishes a frozen, versioned descriptor containing the actual client
reference, so named posthog-js instances work and Closure cannot rename the
host ABI:

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

The helper has matching entry points for the common host module shapes:

- `posthog-c-host.mjs` exports `initPostHogC` and `initPostHogCAsync` for ESM.
- `posthog-c-host.js` is both a CommonJS module and a plain script. It exports
  the same functions from `require()` and publishes the frozen
  `globalThis.PostHogC` object.

Use `initPostHogC()` when the posthog-js library is loaded and the selected
default or named client is still fresh. Use `initPostHogCAsync()` with a
promise, an async loader, or an ESM module namespace:

```js
await initPostHogCAsync(() => import("posthog-js"), {
  // ...the same host fields as above...
})
```

The async form also accepts the official queueing snippet's `window.posthog`.
That stub's `init()` does not return a client, so the helper waits for
`posthog_config.loaded`, invokes any user-supplied `loaded` callback with its
normal client argument and config binding, then validates and publishes the
resulting live contract. It does not poll, impose a timeout, or choose how the
posthog-js script is fetched. Do not call `ph_init()` or start an Emscripten
factory whose `main()` calls it until the returned promise resolves.

For a vendored or CDN-loaded classic-script setup, install the queueing
snippet's loader stub, but replace its final `posthog.init(...)` call with the
helper so posthog-c remains the sole initialization owner:

```html
<script src="/vendor/posthog-c-host.js"></script>
<script>
  const hostReady = PostHogC.initPostHogCAsync(window.posthog, hostSpec)
  hostReady.then(function () { return createApp() })
</script>
```

## The validated host contract

The C config must carry the same API key, normalized host, distinct ID,
profile, flag, release, and GeoIP policy as the host spec. `ph_init()`
validates all of them before it commits callbacks or enabled state. A disabled
C initialization performs no descriptor reads and makes no calls to the
already-loaded posthog-js client.

The synchronous helper requires a fresh default or named client; the async
helper applies the same checks once a queued client loads. Both reject
posthog-js's otherwise-silent reinitialization no-op, force the bootstrap ID
to remain anonymous, and verify the live client config before publishing the
descriptor.

While the C SDK is initialized, do not use `client.set_config()` to replace
the token, host, profile, flag, or `before_send` settings. Every bridge call
rechecks those fields and fails closed if the validated privacy pipeline was
changed; `ph_shutdown()` releases the module's pinned descriptor.

## Configuration ownership

WASM configuration is split by owner. The C shim owns `enabled`, the C
`before_send`/denylist stage, `on_log`, super properties, and whether a flag
read asks posthog-js to emit an exposure. The host helper owns posthog-js
bootstrap, identity/profile/preload parity, release enrichment, and the final
browser scrubber. Native delivery knobs (`flush_at`, intervals, batch/queue
and body caps, request timeout/retries, gzip, offline/crash settings, numeric
rate limit, and stats callbacks) are ignored because posthog-js owns that
pipeline.

## Privacy: two scrubber stages

The two scrubber stages see different data. The C `before_send` hook runs
first and sees only bounded C caller/super/control properties.
`final_before_send` and `final_property_denylist` passed to `initPostHogC()`
run later, after posthog-js adds browser properties; they are the privacy
boundary for automatic browser enrichment. (On native, `before_send` likewise
runs before the optional automatic `$os`, `arch`, and `release` enrichment;
the C denylist suppresses those automatic fields.)

Removing the required event name or token, or changing/removing the
SDK-selected distinct ID, drops the whole event. The validated person-profile
bit and a configured GeoIP event opt-out are restored after the final hooks,
so neither policy can be weakened accidentally. SDK-owned identity, library,
and person-profile fields cannot be replaced by caller, super, or scrubber
props.

## Strict GeoIP opt-out

`disable_geoip` forces `$geoip_disable: true` on events. Extending the opt-out
to `/flags/` evaluation requires routing posthog-js through a proxy that
injects `geoip_disable: true` into `/flags/` requests, then declaring that
capability:

```js
initPostHogC(posthog, {
  // ...matching fields above...
  disable_geoip: true,
  geoip_flags: "proxy-inject-v1",
  flags_api_host: "https://flags-proxy.example",
})
```

The helper rejects initialization without that explicit flags capability and a
separately specified `flags_api_host`. It never derives the flags route from
`api_host`; it normalizes and attests the exact proxy base, then fails closed
if the live posthog-js config changes it. The injection capability remains a
host assertion: the C/WASM module cannot inspect or enforce reverse-proxy
behavior itself.

## Feature-flag reload status

posthog-js does not expose per-request reload completion, so enabled WASM
returns `PH_ERR` from `ph_reload_feature_flags_async()` and reports `UNKNOWN`
for query tokens. The void `ph_reload_feature_flags()` schedules a posthog-js
reload.

## Building with Emscripten

For source consumption with Emscripten, [`posthog-wasm.rsp`](posthog-wasm.rsp)
is the canonical source-and-compile-flag recipe. Invoke it from the posthog-c
checkout root, then add your application's sources and link/runtime choices:

```sh
emcc @wasm/posthog-wasm.rsp /absolute/path/to/app.c -O2 -o app.js
```

The recipe keeps the backend on strict C11 and is also what `zig build
test-wasm` consumes. For a C++ application, compile the SDK recipe as C in a
separate step, then link those objects into the C++ module; the recipe's
intentional `-std=c11` flag is not a C++ driver flag.

An Emscripten `--pre-js` file cannot import the ESM helper. Put that ordering
in the host entry module instead, and instantiate the C/WASM module only after
the descriptor is ready:

```js
import posthog from "posthog-js"
import { initPostHogCAsync } from "./posthog-c-host.mjs"

await initPostHogCAsync(posthog, hostSpec)
const { default: createApp } = await import("./app.mjs")
await createApp()
```

The standalone consumer fixture pairs
[`main.c`](../tests/wasm/consumer/main.c) with
[`run.cjs`](../tests/wasm/consumer/run.cjs) to load the CommonJS/classic
helper before a modularized Emscripten factory.

## Testing

`zig build test-wasm` compiles the backend with emcc (normal + Closure builds)
and runs the Node behavioral/consumer harness against a mock host. The real
host-finalizer contract is kept separate and pins the exact posthog-js version
used by CI:

```sh
cd tests/wasm/host-contract
npm ci --ignore-scripts --no-audit --no-fund
cd ../../..
zig build test-wasm-host
```
