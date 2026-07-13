/*
 * Node harness for the WASM backend. Mocks window.posthog + the host-bootstrapped
 * install id, runs the compiled shim, and asserts each call reached posthog-js
 * with the right event/props - including that the shared serializer produced
 * parity values (1.5 stays 1.5, bools stay booleans).
 *
 * Run via `zig build test-wasm`, which passes the normal or Closure-built
 * module path as argv[2].
 */
const calls = [];
let currentDistinctId = "install-abc";
globalThis.window = {
    __posthog_c_distinct_id: "install-abc",
    posthog: {
        capture: (event, props) => calls.push({ fn: "capture", event, props }),
        identify: (id, props) => calls.push({ fn: "identify", id, props }),
        group: (type, key, props) => calls.push({ fn: "group", type, key, props }),
        reset: () => { currentDistinctId = "reset-anon-1"; calls.push({ fn: "reset" }); },
        get_distinct_id: () => currentDistinctId,
        register: (props) => calls.push({ fn: "register", props }),
        alias: (a, b) => calls.push({ fn: "alias", a, b }),
        captureException: (err, props) => calls.push({ fn: "captureException", name: err.name, message: err.message, props }),
        isFeatureEnabled: (key) => key === "missing" ? undefined : key === "off" ? false : true,
        getFeatureFlag: (key) => key === "off" ? false : undefined,
        getFeatureFlagPayload: () => null,
    },
};

let checks = 0;
let failures = 0;
function check(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        console.log("  FAIL: " + msg);
    }
}

const modulePath = process.argv[2] || "./test_wasm.mjs";
const { default: createPH } = await import(modulePath);
const Module = await createPH();
const cFailures = Module._wasm_run_test();
check(cFailures === 0, "C-side lifecycle and return-code checks passed");

const cap = calls.find((c) => c.fn === "capture" && c.event === "level_started");
check(cap, "capture reached window.posthog");
check(cap && cap.event === "level_started", "event name = level_started");
check(cap && cap.props.weapon === "sword", "string prop weapon=sword");
check(cap && cap.props.level === 3, "int prop level=3");
check(cap && cap.props.score === 1.5, "double prop score=1.5 (serializer parity)");
check(cap && cap.props.alive === true, "bool prop alive=true");
check(cap && cap.props.super_keep === "yes", "WASM super prop merged before scrub");
check(cap && cap.props.scrubbed === true, "before_send added scrubbed=true");
check(cap && !("token" in cap.props), "denylist stripped token");
check(cap && !("secret" in cap.props), "before_send stripped secret");
check(!calls.some((c) => c.event === "drop_me"), "before_send dropped drop_me");
check(calls.some((c) => c.event === "distinct_id_getter_ok"), "current distinct id is readable");
check(!calls.some((c) => c.event === "disabled_init" ||
                         c.event === "failed_badarg_init" ||
                         c.event === "failed_denylist_init" ||
                         c.event === "failed_identity_init"),
      "failed/disabled initialization emitted no events");
check(!calls.some((c) => c.event === "oom_capture"),
      "capture serialization failure emitted no event");

const id = calls.find((c) => c.fn === "identify");
check(id && id.id === "acct-9", "identify id=acct-9");
check(id && id.props && id.props.plan === "pro", "identify $set prop plan=pro");
check(id && id.props && !("token" in id.props), "identify props passed through denylist");
check(!calls.some((c) => c.fn === "identify" && c.id === "oom-identify"),
      "identify serialization failure emitted no host call");

const g = calls.find((c) => c.fn === "group");
check(g && g.type === "game" && g.key === "asteroids", "group game/asteroids");
check(g && g.props && g.props.players === 4, "group properties preserved");
check(g && g.props && !("token" in g.props), "group props passed through denylist");
check(!calls.some((c) => c.fn === "group" && c.key === "oom-group"),
      "group serialization failure emitted no host call");

check(calls.some((c) => c.event === "missing_fallback_true"), "missing flag honored fallback=true");
check(calls.some((c) => c.event === "false_flag_ok"), "false flag resolved as PH_OK value");

const exc = calls.find((c) => c.fn === "capture" && c.event === "$exception");
const excEntry = exc && exc.props && exc.props.$exception_list && exc.props.$exception_list[0];
const excFrames = excEntry && excEntry.stacktrace && excEntry.stacktrace.frames;
check(exc && exc.props.$exception_level === "warning", "handled exception level is warning");
check(exc && exc.props.scrubbed === true, "exception props passed through before_send");
check(exc && exc.props.exception_keep === "yes", "exception extra property preserved");
check(exc && !("token" in exc.props) && !("secret" in exc.props),
      "exception extra/super properties passed through privacy scrub");
check(excEntry && excEntry.type === "NativeAssertion", "structured exception type preserved");
check(excEntry && excEntry.value === "redacted", "structured exception value uses reviewed text");
check(excEntry && excEntry.mechanism && excEntry.mechanism.handled === true,
      "structured exception handled mechanism preserved");
check(excEntry && excEntry.mechanism && excEntry.mechanism.synthetic === true,
      "structured exception synthetic mechanism preserved");
check(excEntry && excEntry.stacktrace && excEntry.stacktrace.type === "raw",
      "structured exception uses raw stacktrace shape");
check(excFrames && excFrames.length === 2, "caller-supplied exception frames preserved");
check(excFrames && excFrames[0].platform === "custom" && excFrames[0].lang === "cpp",
      "exception frame platform/language preserved");
check(excFrames && excFrames[0].function === "sim::step" &&
                   excFrames[0].filename === "sim.cpp" &&
                   excFrames[0].lineno === 412 &&
                   excFrames[0].in_app === true &&
                   excFrames[0].resolved === true,
      "first exception frame fields preserved");
check(excFrames && excFrames[1].function === "main" &&
                   excFrames[1].filename === "main.cpp" &&
                   excFrames[1].lineno === 20,
      "second exception frame fields preserved");
check(excFrames && excFrames.every((f) => !("module" in f)),
      "exception frame denylist removes module fields");
check(!calls.some((c) => c.fn === "captureException"),
      "WASM does not synthesize a browser Error/stack");
check(!calls.some((c) => c.fn === "capture" && c.event === "$exception" &&
                         c.props.$exception_list?.[0]?.type === "OOMException"),
      "exception serialization failure emitted no event");

console.log(`\nwasm harness: ${calls.length} posthog calls, ${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
