# Contributing

Thanks for helping improve posthog-c. This is a source-distributed C11 SDK with
one public C interface and native/WASM backends. Please read `AGENTS.md` and
`DESIGN.md` before changing behavior; they document the module map and the
load-bearing delivery/wire invariants.

## Development setup

Use Zig 0.15.2. The normal verification commands are:

```sh
zig build
zig build test
zig build -Doptimize=ReleaseSafe test
zig build fuzz
zig build test-wasm # requires emsdk and Node
```

Keep changes focused and add regression coverage in the matching `tests/`
module. Serializer or WASM changes need both native and WASM tests. Network
tests should use the mock transport unless they are explicitly opt-in contract
tests with user-provided credentials.

## Invariants worth rechecking

- Native `ph_capture()` does not allocate, read wall time, or use an RNG.
- `/batch/` puts `distinct_id` inside `properties` and preserves the documented
  control-event shapes.
- `enabled = 0` creates no queue or sender and every data-plane call is quiet.
- Never take the queue lock and then the flush lock.
- Fatal-signal code uses only fixed storage and remains explicitly best-effort;
  do not broaden its async-signal-safety claims.

Run `git diff --check` before submitting. Explain user-visible/API changes in
`CHANGELOG.md`, and keep public documentation accurate for both backends.

## Supported development targets

- Windows x86-64: native HTTP and WinHTTP HTTPS; primary runtime target.
- Linux x86-64 and macOS: native plaintext HTTP for local/trusted proxies;
  HTTPS is not implemented yet.
- Emscripten/WebAssembly: posthog-js bridge, tested with emsdk 3.1.64.

Lifecycle is externally serialized: stop and join SDK callers before
`ph_shutdown()`. Callbacks must not call `ph_flush()` or `ph_shutdown()`; those
calls are ignored to prevent reentrant teardown. See `posthog.h` for the full
threading contract.

## Dependency updates

- Zig is pinned to 0.15.2 in CI, `build.zig.zon`, and the README. Upgrade all
  three together and run the entire verification matrix plus consumer smoke.
- emsdk is pinned to 3.1.64 in CI. Upgrade it only with a green WASM harness on
  the new version.
- GitHub Actions use immutable commit SHAs with their moving major tag in a
  comment. Resolve and review a new upstream tag before replacing a SHA.
- `third_party/sdefl/sdefl.h` is the dual MIT/public-domain sdefl 1.0 snapshot.
  Updates are manual: review the complete single-file diff, retain its license,
  then run gzip unit tests, sanitizers, and fuzzing.

## Release checklist

1. Close or explicitly defer every ship blocker in `TODO.md`.
2. Update `PH_VERSION_*`, `PH_VERSION_STRING`, `build.zig.zon`, and this
   changelog in one commit.
3. Run Debug and ReleaseSafe native tests on Windows/Linux/macOS, Linux
   ASan/UBSan, bounded fuzzing, WASM, and the downstream consumer smoke test.
4. Run the opt-in `Live PostHog contract` workflow with a disposable project to
   verify batch ingestion, flags, payloads, and exposure events. Quota response
   behavior stays in deterministic mock/parser tests; deliberately exhausting a
   live project's quota is neither safe nor repeatable.
5. Inspect the packaged source set, create a signed tag, and publish release
   notes that call out platform gaps and any source/API incompatibility.
