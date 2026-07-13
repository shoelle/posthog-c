import assert from "node:assert/strict";

/* posthog-js captures the platform fetch function when its modules load. The
 * pinned client's one startup remote-config request is intercepted and checked;
 * captures below use an instance-level dispatch stub and never reach a network. */
const unexpectedRequests = [];
globalThis["fetch"] = (url) => {
    unexpectedRequests.push(String(url));
    throw new Error("unexpected network request in WASM host contract test");
};

const [posthogModule, { initPostHogC }] = await Promise.all([
    import("posthog-js"),
    import("../../../wasm/posthog-c-host.mjs"),
]);
const posthog = posthogModule["posthog"];

const dispatched = [];
const scrubbed = [];
const client = initPostHogC(posthog, {
    "name": "posthog_c_contract",
    "api_key": "phc_contract",
    "api_host": "https://proxy.example/CaseSensitive/Ingest///",
    "distinct_id": "contract-install-id",
    "person_profiles": "never",
    "preload_flags": false,
    "send_feature_flag_events": false,
    "release": "contract@1.0.0",
    "disable_geoip": true,
    "geoip_flags": "proxy-inject-v1",
    "final_property_denylist": ["$process_person_profile"],
    "final_before_send": (event) => {
        const props = event && event["properties"];
        scrubbed.push({
            "event": event && event["event"],
            "lib": props && props["$lib"],
            "release": props && props["release"],
        });
        if (!props) return event;
        delete props["$lib"];
        delete props["release"];
        props["$geoip_disable"] = false;
        props["$process_person_profile"] = true;
        if (event["event"] === "invalid_token") delete props["token"];
        if (event["event"] === "invalid_distinct_id") props["distinct_id"] = "other-id";
        return event;
    },
    "posthog_config": {
        "persistence": "memory",
        "request_batching": false,
        "autocapture": false,
        "capture_pageview": false,
        "capture_pageleave": false,
        "disable_session_recording": true,
        "disable_surveys": true,
        "disable_web_experiments": true,
        "disable_external_dependency_loading": true,
    },
});
const requestsAfterInit = unexpectedRequests.length;

assert.equal(client["get_distinct_id"](), "contract-install-id");
assert.equal(client["config"]["api_host"], "https://proxy.example/CaseSensitive/Ingest");
assert.equal(client["config"]["person_profiles"], "never");
assert.equal(client["config"]["advanced_disable_feature_flags_on_first_load"], true);
assert.equal(client["config"]["bootstrap"]["isIdentifiedID"], false);
assert.equal(Object.isFrozen(client["config"]["before_send"]), true);

/* request_batching=false routes captures here after posthog-js has calculated
 * automatic properties and run the helper's complete before_send pipeline. */
assert.equal(typeof client["_send_request"], "function");
client["_send_request"] = (request) => dispatched.push(request["data"]);

client["capture"]("kept", { "fixture": true });
client["capture"]("invalid_token", { "fixture": true });
client["capture"]("invalid_distinct_id", { "fixture": true });
client["capture"]("caller_spoof", { "fixture": true, "distinct_id": "other-id" });

assert.equal(requestsAfterInit, 1, "the pinned client attempted its remote-config request once");
assert.match(unexpectedRequests[0],
    /^https:\/\/proxy\.example\/CaseSensitive\/Ingest\/array\/phc_contract\/config\?/,
    "the intercepted remote-config request used the configured proxy");
assert.equal(unexpectedRequests.length, requestsAfterInit,
    "capture dispatch used the instance stub and attempted no additional network requests");
assert.equal(scrubbed.length, 4, "the final host scrubber saw every capture envelope");
for (const seen of scrubbed) {
    assert.equal(seen["lib"], "web", "posthog-js automatic $lib exists before the final scrubber");
    assert.equal(seen["release"], "contract@1.0.0", "release injection runs before the final scrubber");
}

assert.equal(dispatched.length, 1, "invalid token/distinct_id envelopes were dropped as whole events");
const kept = dispatched[0];
assert.equal(kept["event"], "kept");
assert.equal(kept["properties"]["fixture"], true);
assert.equal(kept["properties"]["$lib"], undefined, "the final scrubber can remove automatic properties");
assert.equal(kept["properties"]["release"], undefined, "the final scrubber can remove release");
assert.equal(kept["properties"]["$geoip_disable"], true,
    "the required GeoIP opt-out is forced after the final scrubber");
assert.equal(kept["properties"]["$process_person_profile"], false,
    "PH_NEVER is restored after the final scrubber and denylist");

const descriptor = globalThis["__posthog_c_v1"];
const savedWarn = console.warn;
let reinitRejected = false;
try {
    console.warn = () => {};
    initPostHogC(posthog, {
        "name": "posthog_c_contract",
        "api_key": "phc_different",
        "api_host": "https://proxy.example/CaseSensitive/Ingest",
        "distinct_id": "contract-install-id",
        "person_profiles": "never",
        "preload_flags": false,
        "send_feature_flag_events": false,
        "disable_geoip": true,
        "geoip_flags": "proxy-inject-v1",
    });
} catch (_) {
    reinitRejected = true;
} finally {
    console.warn = savedWarn;
}
assert.equal(reinitRejected, true, "an already-loaded client cannot be relabeled by a new descriptor");
assert.equal(globalThis["__posthog_c_v1"], descriptor,
    "a rejected posthog-js reinitialization leaves the validated descriptor intact");
assert.equal(descriptor["checked_client"](), client);
client["set_config"]({ "before_send": [] });
assert.equal(descriptor["checked_client"](), null,
    "mutating the final privacy pipeline invalidates future bridge calls");

console.log("posthog-js host contract: 1 dispatched, 3 invalid dropped, finalizer ordering verified");
/* posthog-js owns long-lived browser timers; this one-shot Node contract has
 * no page lifecycle to stop them. All assertions have completed at this point. */
process.exit(0);
