/* Standalone source-consumer smoke test for wasm/posthog-wasm.rsp. */
#include "posthog.h"

#include <string.h>

int main(void) {
    ph_config cfg;
    ph_config_defaults(&cfg);
    cfg.enabled = 0;
    if (ph_init(&cfg) != PH_OK) return 1;
    if (strcmp(ph_version(), PH_VERSION_STRING) != 0) return 2;
    if (ph_capture("disabled-consumer-smoke", NULL) != PH_ERR_DISABLED) return 3;
    ph_shutdown();
    return 0;
}
