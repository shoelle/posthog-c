/* ESM facade over the classic-script/CommonJS host helper. */
import "./posthog-c-host.js";

const api = globalThis["PostHogC"];
const DESCRIPTOR_KEY = api["DESCRIPTOR_KEY"];
const initPostHogC = api["initPostHogC"];
const initPostHogCAsync = api["initPostHogCAsync"];

export { DESCRIPTOR_KEY, initPostHogC, initPostHogCAsync };
