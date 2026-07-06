/*
 * The C side of the WASM behavioral test. Exposed as wasm_run_test() (not main)
 * so the Node harness can install its window.posthog mock, spin up the runtime,
 * then call in — deterministic ordering, no reliance on auto-run timing.
 */
#include "posthog.h"

#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
void wasm_run_test(void) {
    ph_config cfg;
    ph_props p;

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_wasm";
    cfg.distinct_id = "install-abc"; /* must match the harness bootstrap id */
    ph_init(&cfg);

    ph_props_init(&p);
    ph_props_set_str(&p, "weapon", "sword");
    ph_props_set_int(&p, "level", 3);
    ph_props_set_double(&p, "score", 1.5);
    ph_props_set_bool(&p, "alive", 1);
    ph_capture("level_started", &p);

    ph_props_init(&p);
    ph_props_set_str(&p, "plan", "pro");
    ph_identify("acct-9", &p);

    ph_group("game", "asteroids", NULL);
}

int main(void) { return 0; }
