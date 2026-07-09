/*
 * Node harness for the WASM backend. Mocks window.posthog + the host-bootstrapped
 * install id, runs the compiled shim, and asserts each call reached posthog-js
 * with the right event/props - including that the shared serializer produced
 * parity values (1.5 stays 1.5, bools stay booleans).
 *
 * Run via `zig build test-wasm` (which emcc-compiles ./test_wasm.mjs first).
 */
const calls = [];
globalThis.window = {
    __posthog_c_distinct_id: "install-abc",
    posthog: {
        capture: (event, props) => calls.push({ fn: "capture", event, props }),
        identify: (id, props) => calls.push({ fn: "identify", id, props }),
        group: (type, key, props) => calls.push({ fn: "group", type, key, props }),
        reset: () => calls.push({ fn: "reset" }),
        register: (props) => calls.push({ fn: "register", props }),
        alias: (a, b) => calls.push({ fn: "alias", a, b }),
        captureException: (err, props) => calls.push({ fn: "captureException", name: err.name, message: err.message, props }),
        isFeatureEnabled: (key) => key === "missing" ? undefined : key === "off" ? false : true,
        getFeatureFlag: (key) => key === "off" ? false : undefined,
        getFeatureFlagPayload: () => null,
    },
};

import createPH from "./test_wasm.mjs";

let checks = 0;
let failures = 0;
function check(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        console.log("  FAIL: " + msg);
    }
}

const Module = await createPH();
Module._wasm_run_test();

const cap = calls.find((c) => c.fn === "capture");
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

const id = calls.find((c) => c.fn === "identify");
check(id && id.id === "acct-9", "identify id=acct-9");
check(id && id.props && id.props.plan === "pro", "identify $set prop plan=pro");

const g = calls.find((c) => c.fn === "group");
check(g && g.type === "game" && g.key === "asteroids", "group game/asteroids");

check(calls.some((c) => c.event === "missing_fallback_true"), "missing flag honored fallback=true");
check(calls.some((c) => c.event === "false_flag_ok"), "false flag resolved as PH_OK value");

const exc = calls.find((c) => c.fn === "captureException");
check(exc && exc.name === "NativeAssertion", "exception type reached captureException");
check(exc && exc.message === "redacted", "exception message was scrubbed");
check(exc && exc.props && exc.props.scrubbed === true, "exception props were scrubbed");

console.log(`\nwasm harness: ${calls.length} posthog calls, ${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
