/*
 * Node harness for the WASM backend. Mocks window.posthog + the host-bootstrapped
 * install id, runs the compiled shim, and asserts each call reached posthog-js
 * with the right event/props — including that the shared serializer produced
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

const id = calls.find((c) => c.fn === "identify");
check(id && id.id === "acct-9", "identify id=acct-9");
check(id && id.props && id.props.plan === "pro", "identify $set prop plan=pro");

const g = calls.find((c) => c.fn === "group");
check(g && g.type === "game" && g.key === "asteroids", "group game/asteroids");

console.log(`\nwasm harness: ${calls.length} posthog calls, ${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
