/* Normal + Closure behavioral harness for the versioned WASM host contract. */
import { initPostHogC, initPostHogCAsync } from "../../wasm/posthog-c-host.mjs";

const calls = [];
const initCalls = [];
let hostConfig = null;
let currentDistinctId = "install-abc";

function runFinalizers(event, props) {
    let data = {
        event,
        properties: {
            "token": "phc_wasm",
            "distinct_id": currentDistinctId,
            "$browser_secret": "automatic-browser-value",
            ...props,
        },
    };
    const fns = Array.isArray(hostConfig["before_send"])
        ? hostConfig["before_send"]
        : [hostConfig["before_send"]];
    for (const fn of fns) {
        data = fn(data);
        if (data == null) return null;
    }
    return data;
}

function makeClient() {
    return {
        "config": hostConfig,
        "capture": (event, props) => {
            const data = runFinalizers(event, props || {});
            if (data) calls.push({ fn: "capture", event: data.event, props: data.properties });
            return data;
        },
        "identify": (id, props) => {
            currentDistinctId = id;
            calls.push({ fn: "identify", id, props });
        },
        "group": (type, key, props) => calls.push({ fn: "group", type, key, props }),
        "reset": () => {
            currentDistinctId = "reset-anon-1";
            calls.push({ fn: "reset" });
        },
        "get_distinct_id": () => currentDistinctId,
        "register": (props) => {
            if (props && props["distinct_id"]) currentDistinctId = String(props["distinct_id"]);
            calls.push({ fn: "register", props });
        },
        "setPersonPropertiesForFlags": (props, reload) =>
            calls.push({ fn: "setPersonPropertiesForFlags", props, reload }),
        "alias": (a, b) => calls.push({ fn: "alias", a, b }),
        "getFeatureFlag": (key, options) => {
            calls.push({ fn: "getFeatureFlag", key, options });
            return key === "missing" ? undefined : key === "off" ? false : true;
        },
        "getFeatureFlagPayload": () => null,
        "reloadFeatureFlags": () => calls.push({ fn: "reloadFeatureFlags" }),
    };
}

const posthogRoot = {
    "init": (apiKey, config, name) => {
        config["token"] = apiKey; /* posthog-js writes this during _init() */
        initCalls.push({ apiKey, config, name });
        hostConfig = config;
        currentDistinctId = config["bootstrap"]["distinctID"];
        const client = makeClient();
        if (typeof config["loaded"] === "function") config["loaded"].call(config, client);
        return client;
    },
};

/* The helper accepts both C enum values and posthog-js profile strings. These
 * probes also prove it returns/records the named client rather than assuming a
 * global posthog object. The final call installs the descriptor used by C. */
initPostHogC(posthogRoot, {
    "api_key": "probe",
    "distinct_id": "probe-identified",
    "person_profiles": 0,
    "preload_flags": false,
});
initPostHogC(posthogRoot, {
    "api_key": "probe",
    "distinct_id": "probe-always",
    "person_profiles": "always",
});

let asyncLoaderCalls = 0;
await initPostHogCAsync(async () => {
    asyncLoaderCalls++;
    return { "default": posthogRoot };
}, {
    "api_key": "probe",
    "distinct_id": "probe-async",
    "person_profiles": "identified_only",
    "preload_flags": true,
});

let queuedInit = null;
let queuedLoadedContract = false;
let queuedClient = null;
const descriptorBeforeQueuedInit = globalThis.__posthog_c_v1;
const queuedReady = initPostHogCAsync({
    "init": (apiKey, config, name) => {
        queuedInit = { apiKey, config, name };
        return undefined; /* official queueing snippets do not return a client */
    },
}, {
    "api_key": "probe",
    "distinct_id": "probe-queued",
    "posthog_config": {
        "loaded": function (loadedClient) {
            queuedLoadedContract = loadedClient === queuedClient &&
                this === queuedInit.config &&
                globalThis.__posthog_c_v1 === descriptorBeforeQueuedInit;
        },
    },
});
await Promise.resolve();
const queuedInitStayedTransactional =
    queuedInit && globalThis.__posthog_c_v1 === descriptorBeforeQueuedInit;
queuedInit.config["token"] = queuedInit.apiKey;
hostConfig = queuedInit.config;
currentDistinctId = queuedInit.config["bootstrap"]["distinctID"];
queuedClient = makeClient();
queuedInit.config["loaded"](queuedClient);
const queuedResult = await queuedReady;
const queuedInitPublished = queuedResult === queuedClient &&
    globalThis.__posthog_c_v1.client === queuedClient;

const descriptorAfterQueuedInit = globalThis.__posthog_c_v1;
let delayedInit = null;
let delayedClient = null;
let delayedSettled = false;
const delayedReady = initPostHogCAsync({
    "init": (apiKey, config) => {
        config["token"] = apiKey;
        hostConfig = config;
        currentDistinctId = config["bootstrap"]["distinctID"];
        delayedInit = { config };
        delayedClient = makeClient();
        return delayedClient;
    },
}, {
    "api_key": "probe",
    "distinct_id": "probe-delayed-loaded",
    "posthog_config": {
        "loaded": function () { this["before_send"] = []; },
    },
});
delayedReady.then(
    () => { delayedSettled = true; },
    () => { delayedSettled = true; },
);
await Promise.resolve();
const delayedLoadedStayedPending = delayedInit && !delayedSettled &&
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;
delayedInit.config["loaded"](delayedClient);
let delayedLoadedRejected = false;
try {
    await delayedReady;
} catch (_) {
    delayedLoadedRejected = true;
}
const delayedLoadedStayedTransactional =
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;
let mutatingInit = null;
const mutatingReady = initPostHogCAsync({
    "init": (apiKey, config) => {
        mutatingInit = { apiKey, config };
        return undefined;
    },
}, {
    "api_key": "probe",
    "distinct_id": "probe-mutating-loaded",
    "posthog_config": {
        "loaded": function () {
            this["before_send"] = [];
        },
    },
});
await Promise.resolve();
mutatingInit.config["token"] = mutatingInit.apiKey;
hostConfig = mutatingInit.config;
currentDistinctId = mutatingInit.config["bootstrap"]["distinctID"];
mutatingInit.config["loaded"](makeClient());
let mutatingLoadedRejected = false;
try {
    await mutatingReady;
} catch (_) {
    mutatingLoadedRejected = true;
}
const mutatingLoadedStayedTransactional =
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;

let mismatchedClientsRejected = false;
try {
    await initPostHogCAsync({
        "init": (apiKey, config) => {
            config["token"] = apiKey;
            hostConfig = config;
            currentDistinctId = config["bootstrap"]["distinctID"];
            const callbackClient = makeClient();
            config["loaded"](callbackClient);
            return makeClient();
        },
    }, {
        "api_key": "probe",
        "distinct_id": "probe-mismatched-client",
    });
} catch (_) {
    mismatchedClientsRejected = true;
}
const mismatchedClientsStayedTransactional =
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;

let callbackThenThrowRejected = false;
try {
    await initPostHogCAsync({
        "init": (apiKey, config) => {
            config["token"] = apiKey;
            hostConfig = config;
            currentDistinctId = config["bootstrap"]["distinctID"];
            config["loaded"](makeClient());
            throw new Error("init failed after loaded callback");
        },
    }, {
        "api_key": "probe",
        "distinct_id": "probe-callback-then-throw",
    });
} catch (_) {
    callbackThenThrowRejected = true;
}
const callbackThenThrowStayedTransactional =
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;

let falsyLoadedThrowRejected = false;
try {
    await initPostHogCAsync({
        "init": (apiKey, config) => {
            config["token"] = apiKey;
            hostConfig = config;
            currentDistinctId = config["bootstrap"]["distinctID"];
            const readyClient = makeClient();
            config["loaded"](readyClient);
            return readyClient;
        },
    }, {
        "api_key": "probe",
        "distinct_id": "probe-falsy-loaded-throw",
        "posthog_config": {
            "loaded": function () { throw null; },
        },
    });
} catch (error) {
    falsyLoadedThrowRejected = error === null;
}
const falsyLoadedThrowStayedTransactional =
    globalThis.__posthog_c_v1 === descriptorAfterQueuedInit;

let invalidLoadedRejected = false;
try {
    await initPostHogCAsync(posthogRoot, {
        "api_key": "probe",
        "distinct_id": "probe-invalid-loaded",
        "posthog_config": { "loaded": true },
    });
} catch (_) {
    invalidLoadedRejected = true;
}

let missingGeoipPolicyRejected = false;
try {
    initPostHogC(posthogRoot, {
        "api_key": "probe",
        "distinct_id": "probe-geo",
        "disable_geoip": true,
    });
} catch (_) {
    missingGeoipPolicyRejected = true;
}

let missingFlagsHostRejected = false;
try {
    initPostHogC(posthogRoot, {
        "api_key": "probe",
        "distinct_id": "probe-flags-host",
        "disable_geoip": true,
        "geoip_flags": "proxy-inject-v1",
    });
} catch (_) {
    missingFlagsHostRejected = true;
}

let disabledFlagsRejected = false;
try {
    initPostHogC(posthogRoot, {
        "api_key": "probe",
        "distinct_id": "probe-disabled-flags",
        "posthog_config": { "advanced_disable_flags": true },
    });
} catch (_) {
    disabledFlagsRejected = true;
}

globalThis.window = {};
initPostHogC(posthogRoot, {
    "api_key": "phc_wasm",
    "api_host": "https://us.i.posthog.com///",
    "distinct_id": "install-abc",
    "person_profiles": 2,
    "preload_flags": true,
    "send_feature_flag_events": false,
    "release": "wasm-release@1",
    "disable_geoip": true,
    "geoip_flags": "proxy-inject-v1",
    "flags_api_host": "https://flags-proxy.example/GeoIP///",
    "name": "wasm-client",
    "final_property_denylist": ["$browser_secret", "$process_person_profile"],
    "final_before_send": (data) => {
        data.properties["host_scrub_saw_auto"] = "$browser_secret" in data.properties;
        data.properties["$geoip_disable"] = false; /* final policy must win */
        data.properties["$process_person_profile"] = true;
        if (data.event === "host_drop_envelope") delete data.properties["token"];
        if (data.event === "host_redirect_identity")
            data.properties["distinct_id"] = "other-id";
        return data;
    },
});

let checks = 0;
let failures = 0;
function check(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        console.log("  FAIL: " + msg);
    }
}

check(initCalls[0].config.person_profiles === "identified_only",
      "host helper maps PH_IDENTIFIED_ONLY");
check(initCalls[0].config.advanced_disable_feature_flags_on_first_load === true,
      "host helper maps preload_flags=false");
check(initCalls[1].config.person_profiles === "always",
      "host helper maps PH_ALWAYS");
check(missingGeoipPolicyRejected,
      "host helper rejects GeoIP event-only opt-out without flags proxy policy");
check(missingFlagsHostRejected,
      "host helper rejects a flags proxy policy without an explicit flags host");
check(disabledFlagsRejected,
      "host helper rejects a posthog-js config that disables the public flag API");
check(initCalls.at(-1).name === "wasm-client",
      "host helper initializes the requested named client");
check(globalThis.__posthog_c_v1 === globalThis.window.__posthog_c_v1 &&
      Object.isFrozen(globalThis.__posthog_c_v1),
      "host helper publishes one frozen descriptor");
check(globalThis.__posthog_c_v1.api_host === "https://us.i.posthog.com",
      "host helper normalizes trailing host slashes");
check(initCalls.at(-1).config.flags_api_host === "https://flags-proxy.example/GeoIP" &&
      globalThis.__posthog_c_v1.flags_api_host === "https://flags-proxy.example/GeoIP",
      "host helper normalizes, configures, and attests the explicit flags host");
hostConfig["flags_api_host"] = "https://wrong-flags-proxy.example";
check(globalThis.__posthog_c_v1.checked_client() === null,
      "changing the live flags host invalidates the host contract");
hostConfig["flags_api_host"] = "https://flags-proxy.example/GeoIP";
check(globalThis.__posthog_c_v1.checked_client() === globalThis.__posthog_c_v1.client,
      "restoring the attested flags host restores the host contract");

check(initCalls.at(-1).config.bootstrap.isIdentifiedID === false,
      "host helper keeps the bootstrap install id anonymous");
check(Object.isFrozen(initCalls.at(-1).config.before_send),
      "host helper freezes the validated finalizer pipeline");

const modulePath = process.argv[2] || "./test_wasm.mjs";
const { default: createPH } = await import(modulePath);
const Module = await createPH();
const cFailures = Module._wasm_run_test();
check(cFailures === 0, "C-side lifecycle, config, and return-code checks passed");

const cap = calls.find((c) => c.fn === "capture" && c.event === "level_started");
check(cap, "capture reached descriptor.client");
check(cap && cap.props.weapon === "sword", "string prop weapon=sword");
check(cap && cap.props.level === 3, "int prop level=3");
check(cap && cap.props.score === 1.5, "double prop score=1.5 (serializer parity)");
check(cap && cap.props.alive === true, "bool prop alive=true");
check(cap && cap.props.super_keep === "yes", "WASM super prop merged before scrub");
check(cap && cap.props.scrubbed === true, "C before_send added scrubbed=true");
check(cap && "token" in cap.props, "required ingestion token survives");
check(cap && !("secret" in cap.props), "C before_send stripped caller secret");
check(cap && !("$browser_secret" in cap.props) && cap.props.host_scrub_saw_auto === true,
      "host final scrubber sees then removes posthog-js enrichment");
check(cap && cap.props.release === "wasm-release@1",
      "host finalizer injects configured release");
check(cap && cap.props.$geoip_disable === true,
      "GeoIP event opt-out is forced after host scrubbers");
check(cap && cap.props.$process_person_profile === false,
      "PH_NEVER is restored after host scrubbers and denylist");
check(!calls.some((c) => c.event === "drop_me"), "C before_send dropped drop_me");
check(!calls.some((c) => c.event === "host_drop_envelope"),
      "host finalizer dropped an invalid ingestion envelope");
check(!calls.some((c) => c.event === "host_redirect_identity"),
      "a host final-scrubber identity rewrite is still dropped");
const redirect = calls.find((c) => c.event === "caller_redirect_identity");
check(redirect && redirect.props.distinct_id === "install-abc",
      "caller distinct_id shadow is stripped (native parity); real identity wins");
check(redirect && redirect.props.$lib !== "shadow-lib",
      "caller $lib shadow is stripped from wasm capture props");
const relWin = calls.find((c) => c.event === "caller_release_wins");
check(relWin && relWin.props.release === "caller-release@9",
      "a caller-supplied event release overrides the configured release");
check(calls.some((c) => c.event === "distinct_id_getter_ok"),
      "current distinct id is readable through descriptor client");
check(!calls.some((c) => ["disabled_init", "failed_badarg_init",
                           "failed_denylist_init", "failed_throwing_host_init",
                           "failed_identity_init"].includes(c.event)),
      "failed/disabled initialization emitted no events");
check(!calls.some((c) => c.event === "oom_capture"),
      "capture serialization failure emitted no event");

const identify = calls.find((c) => c.fn === "capture" && c.event === "$identify");
check(identify && identify.props.distinct_id === "acct-9" &&
      identify.props.$anon_distinct_id === "install-abc",
      "PH_NEVER identify switches identity with the anonymous id preserved");
check(identify && identify.props.$set && identify.props.$set.plan === "pro",
      "PH_NEVER identify preserves reviewed flag/person properties");
check(identify && identify.props.$process_person_profile === false,
      "PH_NEVER identify cannot create a profile");
check(!calls.some((c) => c.fn === "identify"),
      "PH_NEVER bypasses posthog-js identify no-op");
check(calls.some((c) => c.fn === "setPersonPropertiesForFlags" &&
                        c.props.plan === "pro" && c.reload === false),
      "PH_NEVER identify updates feature-flag evaluation properties");
check(calls.some((c) => c.fn === "register" && c.props.distinct_id === "acct-9"),
      "PH_NEVER identify updates the host distinct id");

const alias = calls.find((c) => c.fn === "capture" && c.event === "$create_alias");
check(alias && alias.props.alias === "alias-9" &&
      alias.props.distinct_id === "acct-9" &&
      alias.props.$process_person_profile === false,
      "PH_NEVER alias uses a raw profile-suppressed control event");
check(!calls.some((c) => c.fn === "alias"),
      "PH_NEVER bypasses posthog-js alias no-op");
check(calls.some((c) => c.fn === "capture" && c.event === "$create_alias" &&
                        c.props.alias === "alias-legacy" &&
                        c.props.distinct_id === "legacy-old"),
      "explicit alias originals remain valid SDK-owned identities");
check(calls.filter((c) => c.fn === "capture" && c.event === "$create_alias").length === 2,
      "empty aliases never reach the host");
check(calls.some((c) => c.event === "restored_flags_host") &&
      !calls.some((c) => c.event === "mutated_flags_host"),
      "bridge rejects flags-host drift and recovers after restoration");

const group = calls.find((c) => c.fn === "group");
check(group && group.type === "game" && group.key === "asteroids",
      "group delegates to the descriptor client");
check(group && group.props.players === 4 && !("token" in group.props),
      "group properties pass through the C privacy stage");
check(!calls.some((c) => c.fn === "group" && (!c.type || !c.key)),
      "empty group identifiers never reach the host");
check(calls.some((c) => c.event === "restored_host_contract") &&
      !calls.some((c) => c.event === "mutated_host_contract"),
      "bridge rejects a mutated finalizer and recovers after restoration");
check(!calls.some((c) => c.fn === "group" && c.key === "oom-group"),
      "group serialization failure emitted no host call");

const flagReads = calls.filter((c) => c.fn === "getFeatureFlag");
check(flagReads.length >= 2 && flagReads.every((c) => c.options.send_event === false),
      "send_feature_flag_events=false reaches every posthog-js flag read");
check(calls.some((c) => c.event === "missing_fallback_true"),
      "missing flag honored fallback=true");
check(calls.some((c) => c.event === "false_flag_ok"),
      "false flag resolved as a present value");

const exc = calls.find((c) => c.fn === "capture" && c.event === "$exception");
const excEntry = exc && exc.props.$exception_list && exc.props.$exception_list[0];
const excFrames = excEntry && excEntry.stacktrace && excEntry.stacktrace.frames;
check(exc && exc.props.$exception_level === "warning", "handled exception level is warning");
check(exc && exc.props.scrubbed === true, "exception props passed through C before_send");
check(exc && exc.props.exception_keep === "yes", "exception extra property preserved");
check(exc && !("secret" in exc.props), "exception properties passed through privacy scrub");
check(exc && exc.props.release === "wasm-release@1" &&
      exc.props.$geoip_disable === true,
      "host release and GeoIP finalizers cover structured exceptions");
check(excEntry && excEntry.type === "NativeAssertion" && excEntry.value === "redacted",
      "structured exception type and reviewed value preserved");
check(excEntry && excEntry.mechanism.handled === true &&
      excEntry.mechanism.synthetic === true,
      "structured exception mechanism preserved");
check(excFrames && excFrames.length === 2, "caller-supplied exception frames preserved");
check(excFrames && excFrames[0].function === "sim::step" &&
      excFrames[0].filename === "sim.cpp" && excFrames[0].lineno === 412,
      "structured exception frame fields preserved");
check(excFrames && excFrames.every((f) => !("module" in f)),
      "exception frame denylist removes module fields");
check(!calls.some((c) => c.fn === "capture" && c.event === "$exception" &&
                         c.props.$exception_list?.[0]?.type === "OOMException"),
      "exception serialization failure emitted no event");
check(asyncLoaderCalls === 1 && initCalls[2].config.bootstrap.distinctID === "probe-async",
      "async host loader accepts a promised module namespace");
check(queuedInitStayedTransactional,
      "queueing snippet leaves the previous descriptor pinned until its client loads");
check(queuedInitPublished,
      "queueing snippet publishes and resolves after its real client loads");
check(queuedLoadedContract,
      "queueing snippet preserves the user loaded callback's client and binding");
check(delayedLoadedStayedPending && delayedLoadedRejected &&
      delayedLoadedStayedTransactional,
      "async init waits for delayed loaded and revalidates before publication");
check(mutatingLoadedRejected && mutatingLoadedStayedTransactional,
      "a loaded callback cannot weaken the contract before descriptor publication");
check(mismatchedClientsRejected && mismatchedClientsStayedTransactional,
      "callback/return client disagreement fails without replacing the descriptor");
check(callbackThenThrowRejected && callbackThenThrowStayedTransactional,
      "an init throw after loaded cannot publish or resolve the descriptor");
check(falsyLoadedThrowRejected && falsyLoadedThrowStayedTransactional,
      "a falsy loaded-callback throw cannot publish or resolve the descriptor");
check(invalidLoadedRejected,
      "an invalid user loaded callback is rejected before posthog-js init");
check(globalThis.PostHogC.initPostHogC === initPostHogC &&
      globalThis.PostHogC.initPostHogCAsync === initPostHogCAsync &&
      Object.isFrozen(globalThis.PostHogC),
      "ESM facade and classic-script API share one frozen implementation");

console.log(`\nwasm harness: ${calls.length} host calls, ${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
