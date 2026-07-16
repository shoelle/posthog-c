"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const modulePath = process.argv[2];
const hostPath = process.argv[3];
const host = require(hostPath);
assert.equal(require("./package.json").type, "module");
assert.equal(require(path.join(path.dirname(hostPath), "package.json")).type,
             "commonjs");
assert.equal(typeof host.initPostHogC, "function");
assert.equal(typeof host.initPostHogCAsync, "function");
assert.equal(globalThis.PostHogC, host);
assert.equal(Object.isFrozen(host), true);

/* The same source must also parse without a module loader and publish the
 * documented classic-script global. */
const classicGlobal = { URL };
classicGlobal.globalThis = classicGlobal;
vm.runInNewContext(fs.readFileSync(hostPath, "utf8"), classicGlobal, {
    filename: hostPath,
});
assert.equal(typeof classicGlobal.PostHogC.initPostHogC, "function");
assert.equal(typeof classicGlobal.PostHogC.initPostHogCAsync, "function");

const createPostHogConsumer = require(modulePath);
assert.equal(typeof createPostHogConsumer, "function");
Promise.resolve(createPostHogConsumer()).catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
