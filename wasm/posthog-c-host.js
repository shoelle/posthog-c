/*
 * Supported posthog-js bootstrap for the posthog-c WASM backend.
 *
 * The C shim intentionally validates this small, versioned descriptor instead
 * of inspecting posthog-js internals. All ABI-owned property names use bracket
 * notation so Closure optimization cannot rename the host boundary. This file
 * is deliberately a classic script as well as a CommonJS module; the ESM entry
 * point is the adjacent posthog-c-host.mjs wrapper.
 */

(function (root) {
"use strict";

const DESCRIPTOR_KEY = "__posthog_c_v1";
const PROFILE_NAMES = ["identified_only", "always", "never"];
const CLIENT_METHODS = [
    "capture",
    "identify",
    "alias",
    "get_distinct_id",
    "reset",
    "group",
    "getFeatureFlag",
    "getFeatureFlagPayload",
    "reloadFeatureFlags",
    "register",
    "setPersonPropertiesForFlags",
];

function requireString(value, name) {
    if (typeof value !== "string" || value.length === 0) {
        throw new TypeError(`${name} must be a non-empty string`);
    }
    return value;
}

function normalizeHttpHost(value, name, defaultValue) {
    const raw = value == null || value === ""
        ? (defaultValue == null ? requireString(value, name) : defaultValue)
        : requireString(value, name);
    if (raw.trim() !== raw ||
        (!raw.startsWith("http://") && !raw.startsWith("https://"))) {
        throw new TypeError(`${name} must be an absolute http(s) URL without surrounding whitespace`);
    }
    const parsed = new URL(raw);
    if (!parsed.hostname || (parsed.protocol !== "http:" && parsed.protocol !== "https:")) {
        throw new TypeError(`${name} must contain a host name`);
    }
    let normalized = raw;
    while (normalized.endsWith("/")) normalized = normalized.slice(0, -1);
    return normalized;
}

function normalizeApiHost(value) {
    return normalizeHttpHost(value, "api_host", "https://us.i.posthog.com");
}

function normalizeProfile(value) {
    if (value == null) return PROFILE_NAMES[0];
    if (Number.isInteger(value) && value >= 0 && value < PROFILE_NAMES.length) {
        return PROFILE_NAMES[value];
    }
    if (PROFILE_NAMES.includes(value)) return value;
    throw new TypeError("person_profiles must be identified_only, always, never, or the matching C enum value");
}

function appendFunctions(out, value, name) {
    if (value == null) return;
    const values = Array.isArray(value) ? value : [value];
    for (const fn of values) {
        if (typeof fn !== "function") throw new TypeError(`${name} must contain functions`);
        out.push((event) => {
            try {
                return fn(event);
            } catch (_) {
                return null; /* final privacy hooks fail closed */
            }
        });
    }
}

function appendDenylist(out, value, name) {
    if (value == null) return;
    if (!Array.isArray(value)) throw new TypeError(`${name} must be an array`);
    for (const key of value) {
        if (typeof key !== "string" || key.length === 0) {
            throw new TypeError(`${name} must contain non-empty strings`);
        }
        out.add(key);
    }
}

function installDescriptor(descriptor) {
    globalThis[DESCRIPTOR_KEY] = descriptor;
    const win = globalThis["window"];
    if (win && win !== globalThis) win[DESCRIPTOR_KEY] = descriptor;
}

/*
 * Initialize one posthog-js client and publish the descriptor ph_init()
 * validates. `posthog` may be the default singleton or another object exposing
 * posthog-js's public init() method; `spec.name` therefore supports named
 * instances without relying on window.posthog.
 */
function initPostHogCInternal(posthog, spec, asyncReady) {
    if (!posthog || typeof posthog["init"] !== "function") {
        throw new TypeError("posthog must expose init()");
    }
    if (!spec || typeof spec !== "object") throw new TypeError("spec is required");

    const apiKey = requireString(spec["api_key"], "api_key");
    const apiHost = normalizeApiHost(spec["api_host"]);
    const distinctId = requireString(spec["distinct_id"], "distinct_id");
    const personProfiles = normalizeProfile(spec["person_profiles"]);
    const preloadFlags = spec["preload_flags"] !== false;
    const sendFeatureFlagEvents = spec["send_feature_flag_events"] !== false;
    const release = spec["release"] == null ? "" : String(spec["release"]);
    const disableGeoip = !!spec["disable_geoip"];
    const flagsGeoipPolicy = spec["geoip_flags"] || "default";
    if (disableGeoip && flagsGeoipPolicy !== "proxy-inject-v1") {
        throw new TypeError("disable_geoip requires geoip_flags: 'proxy-inject-v1'");
    }
    const flagsApiHost = disableGeoip
        ? normalizeHttpHost(spec["flags_api_host"], "flags_api_host", null)
        : null;

    const base = spec["posthog_config"] == null
        ? {}
        : { ...spec["posthog_config"] };
    if (base["loaded"] != null && typeof base["loaded"] !== "function") {
        throw new TypeError("posthog_config.loaded must be a function");
    }
    for (const key of ["advanced_disable_flags", "advanced_disable_decide",
                       "advanced_disable_feature_flags",
                       "advanced_only_evaluate_survey_feature_flags"]) {
        if (base[key]) {
            throw new TypeError(`${key} is incompatible with the posthog-c feature-flag API`);
        }
    }

    let client = null;
    const beforeSend = [];
    const denylist = new Set();
    const profileByEvent = new WeakMap();
    const profileMarker = Symbol("posthog-c-profile-policy");
    const aliasStack = [];

    /* posthog-js calculates this policy field before its before_send pipeline.
     * Snapshot it before user hooks so the final stage can restore the SDK-owned
     * value even when a denylist or scrubber tries to remove/replace it. */
    beforeSend.push((event) => {
        if (event && typeof event === "object") {
            const props = event["properties"];
            profileByEvent.set(event, props && props["$process_person_profile"]);
            try { event[profileMarker] = props && props["$process_person_profile"]; }
            catch (_) {}
        }
        return event;
    });

    /* Release is optional enrichment: the config value is a default that a
     * caller-supplied event `release` overrides (matching the native serializer,
     * which suppresses the config release when the event carries its own). A
     * final scrubber may still remove it. */
    if (release) {
        beforeSend.push((event) => {
            const props = event && event["properties"];
            if (props && props["release"] === undefined) props["release"] = release;
            return event;
        });
    }
    appendFunctions(beforeSend, base["before_send"], "posthog_config.before_send");
    appendFunctions(beforeSend, spec["final_before_send"], "final_before_send");
    appendDenylist(denylist, base["property_denylist"], "posthog_config.property_denylist");
    appendDenylist(denylist, spec["final_property_denylist"], "final_property_denylist");
    if (denylist.size) {
        beforeSend.push((event) => {
            const props = event && event["properties"];
            if (props) for (const key of denylist) delete props[key];
            return event;
        });
    }

    /* This validator is deliberately after every user-provided scrubber. An
     * invalid ingestion envelope is dropped as a whole, never partially sent.
     * Identity/profile/GeoIP policy is restored after those hooks so it cannot
     * be redirected or weakened by caller properties or browser scrubbers. */
    beforeSend.push((event) => {
        const props = event && event["properties"];
        let currentDistinctId;
        try {
            const pendingAlias = aliasStack.length
                ? aliasStack[aliasStack.length - 1]
                : null;
            currentDistinctId = pendingAlias && event["event"] === "$create_alias" &&
                    props && props["alias"] === pendingAlias["alias"]
                ? pendingAlias["original"]
                : (client
                    ? String(client["get_distinct_id"]())
                    : distinctId); /* synchronous events emitted from inside init() */
        } catch (_) {
            return null;
        }
        if (!event || typeof event["event"] !== "string" || !event["event"] ||
            !props || props["token"] !== apiKey ||
            typeof currentDistinctId !== "string" || !currentDistinctId ||
            typeof props["distinct_id"] !== "string" ||
            !props["distinct_id"] || props["distinct_id"] !== currentDistinctId) {
            return null;
        }

        let processProfile;
        if (personProfiles === "never") {
            processProfile = false;
        } else if (personProfiles === "always") {
            processProfile = true;
        } else if (profileByEvent.has(event)) {
            processProfile = profileByEvent.get(event);
        } else if (event && typeof event[profileMarker] === "boolean") {
            processProfile = event[profileMarker];
        }
        if (typeof processProfile !== "boolean") return null;
        try { delete event[profileMarker]; } catch (_) {}
        props["$process_person_profile"] = processProfile;
        if (disableGeoip) props["$geoip_disable"] = true;
        return event;
    });

    const bootstrap = {
        ...(base["bootstrap"] || {}),
        "distinctID": distinctId,
        "isIdentifiedID": false,
    };
    const config = {
        ...base,
        "api_host": apiHost,
        "bootstrap": bootstrap,
        "person_profiles": personProfiles,
        "advanced_disable_feature_flags_on_first_load": !preloadFlags,
        "before_send": beforeSend,
    };
    if (disableGeoip) config["flags_api_host"] = flagsApiHost;
    Object.freeze(beforeSend);

    let completedClient = null;
    let completionError = null;
    let completionFailed = false;
    let initReturned = false;
    let loadedClient = null;
    let loadedSeen = false;
    let returnedClient = null;
    const finishClient = (candidate) => {
        if (completedClient) {
            if (candidate !== completedClient) {
                throw new Error("posthog-js loaded and returned different clients");
            }
            return completedClient;
        }
        if (completionFailed) throw completionError;
        try {
            client = candidate;
            if (!client || typeof client["get_distinct_id"] !== "function" ||
                String(client["get_distinct_id"]()) !== distinctId) {
                throw new Error("posthog-js did not adopt the configured bootstrap distinct id");
            }

            const live = client["config"];
            const methodRefs = Object.create(null);
            for (const name of CLIENT_METHODS) {
                if (typeof client[name] !== "function") {
                    throw new Error(`posthog-js client is missing ${name}()`);
                }
                methodRefs[name] = client[name];
            }
            const hasLiveContract = () => {
                if (!live || client["config"] !== live ||
                    live["token"] !== apiKey || live["api_host"] !== apiHost ||
                    live["person_profiles"] !== personProfiles ||
                    !!live["advanced_disable_feature_flags_on_first_load"] !== !preloadFlags ||
                    live["before_send"] !== beforeSend ||
                    !!live["advanced_disable_flags"] ||
                    !!live["advanced_disable_decide"] ||
                    !!live["advanced_disable_feature_flags"] ||
                    !!live["advanced_only_evaluate_survey_feature_flags"] ||
                    (disableGeoip && live["flags_api_host"] !== flagsApiHost)) {
                    return false;
                }
                for (const name of CLIENT_METHODS) {
                    if (client[name] !== methodRefs[name]) return false;
                }
                return true;
            };
            if (!hasLiveContract()) {
                throw new Error("posthog-js init did not apply the requested host contract");
            }
            const checkedClient = () => {
                try {
                    return hasLiveContract() ? client : null;
                } catch (_) {
                    return null;
                }
            };
            const withAlias = (alias, original, operation) => {
                if (typeof alias !== "string" || !alias ||
                    typeof original !== "string" || !original ||
                    typeof operation !== "function") {
                    throw new TypeError("invalid alias bridge operation");
                }
                aliasStack.push({ "alias": alias, "original": original });
                try {
                    return operation();
                } finally {
                    aliasStack.pop();
                }
            };

            const descriptor = {
                "abi": 1,
                "client": client,
                "checked_client": checkedClient,
                "with_alias": withAlias,
                "api_key": apiKey,
                "api_host": apiHost,
                "person_profiles": personProfiles,
                "preload_flags": preloadFlags,
                "send_feature_flag_events": sendFeatureFlagEvents,
                "rate_policy": "posthog-js",
                "release": release,
                "finalizer_version": 1,
                "final_scrubber": "posthog-js-before-send-v1",
                "geoip_events": disableGeoip ? "force-disable-v1" : "default",
                "geoip_flags": disableGeoip ? flagsGeoipPolicy : "default",
                "flags_api_host": flagsApiHost,
            };
            Object.defineProperty(descriptor, "distinct_id", {
                "enumerable": true,
                "get": () => {
                    const checked = checkedClient();
                    if (!checked) throw new Error("posthog-js host contract was mutated");
                    return String(checked["get_distinct_id"]());
                },
            });
            Object.freeze(descriptor);
            installDescriptor(descriptor);
            completedClient = client;
            return client;
        } catch (error) {
            completionError = error;
            completionFailed = true;
            throw error;
        }
    };

    if (asyncReady) {
        const userLoaded = base["loaded"] || null;
        config["loaded"] = function (readyClient) {
            try {
                if (loadedSeen) {
                    if (readyClient !== loadedClient) {
                        throw new Error("posthog-js loaded more than one client");
                    }
                    return;
                }
                loadedSeen = true;
                loadedClient = readyClient;
                client = readyClient;
                if (userLoaded) userLoaded.call(this, readyClient);
                if (initReturned) {
                    if (returnedClient && readyClient !== returnedClient) {
                        throw new Error("posthog-js loaded and returned different clients");
                    }
                    asyncReady["resolve"](finishClient(readyClient));
                }
            } catch (error) {
                completionError = error;
                completionFailed = true;
                asyncReady["reject"](error);
            }
        };
    }

    let immediate;
    try {
        immediate = posthog["init"](apiKey, config, spec["name"]);
    } catch (error) {
        if (!asyncReady) throw error;
        completionError = error;
        completionFailed = true;
        asyncReady["reject"](error);
        return null;
    }
    returnedClient = immediate || null;
    initReturned = true;

    if (completionFailed) throw completionError;
    if (loadedSeen && returnedClient && loadedClient !== returnedClient) {
        throw new Error("posthog-js loaded and returned different clients");
    }

    if (asyncReady) {
        if (!loadedSeen) return null;
        const completed = finishClient(loadedClient);
        asyncReady["resolve"](completed);
        return completed;
    }

    if (immediate) {
        const completed = finishClient(immediate);
        return completed;
    }
    throw new Error("posthog-js init did not return a client");
}

function initPostHogC(posthog, spec) {
    return initPostHogCInternal(posthog, spec, null);
}

/* Resolve application-owned async loading without choosing a CDN, package
 * version, or network policy for the host. Dynamic import() returns a module
 * namespace in common bundlers, so accept its default export as a convenience. */
async function initPostHogCAsync(posthogOrLoader, spec) {
    let loaded = typeof posthogOrLoader === "function"
        ? posthogOrLoader()
        : posthogOrLoader;
    loaded = await loaded;
    if ((!loaded || typeof loaded["init"] !== "function") &&
        loaded && loaded["default"] &&
        typeof loaded["default"]["init"] === "function") {
        loaded = loaded["default"];
    }
    return new Promise((resolve, reject) => {
        try {
            initPostHogCInternal(loaded, spec, {
                "resolve": resolve,
                "reject": reject,
            });
        } catch (error) {
            reject(error);
        }
    });
}

const api = Object.freeze({
    "DESCRIPTOR_KEY": DESCRIPTOR_KEY,
    "initPostHogC": initPostHogC,
    "initPostHogCAsync": initPostHogCAsync,
});
root["PostHogC"] = api;
if (typeof module === "object" && module && module["exports"]) {
    module["exports"] = api;
}
})(globalThis);
