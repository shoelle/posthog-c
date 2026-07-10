/*
 * quickstart.c - the smallest useful posthog-c program (C API).
 *
 * Build + run:  zig build run-example
 *
 * api_host can be a local dev proxy over http:// (any platform) or an https://
 * PostHog host - HTTPS works on Windows now (WinHTTP); Linux/macOS TLS is next
 * on the roadmap. With nothing listening on the dev proxy below, the sender
 * logs a failed POST and drops the batch - which is what on_log prints.
 */
#include "posthog.h"

#include <stdio.h>

static void on_log(ph_log_level level, const char *msg, void *user) {
    (void)user;
    static const char *names[] = {"error", "warn", "info", "debug"};
    printf("[posthog:%s] %s\n", names[level], msg);
}

int main(void) {
    ph_config cfg;
    ph_props p;

    static const char *denylist[] = {"$ip", "email", "auth_token"};

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_your_project_key";
    cfg.api_host = "http://localhost:8000"; /* dev proxy; or https://us.i.posthog.com on Windows */
    cfg.release = "quickstart@0.1.0";
    cfg.on_log = on_log;

    /* Privacy + reliability knobs: strip these keys from every event, and cap a
     * runaway loop to 10 events/sec. (A before_send hook can redact/drop too.) */
    cfg.property_denylist = denylist;
    cfg.property_denylist_count = 3;
    cfg.rate_limit_per_sec = 10;

    if (ph_init(&cfg) != PH_OK) {
        fprintf(stderr, "posthog init failed\n");
        return 1;
    }
    printf("posthog-c %s initialized\n", ph_version());

    /* Anonymous product event with a few properties. */
    ph_props_init(&p);
    ph_props_set_str(&p, "source", "example");
    ph_props_set_int(&p, "answer", 42);
    ph_props_set_bool(&p, "first_run", 1);
    ph_props_set_str(&p, "email", "leaked@example.com"); /* denylisted -> stripped */
    ph_capture("quickstart_ran", &p);

    /* Sign the user in; later events attach to this identity. */
    ph_identify("user-123", NULL);
    ph_capture("did_thing", NULL);

    /* Feature flags evaluate remotely and cache locally; the 2nd arg is the
     * fallback used when a flag is unknown or the SDK is offline. */
    if (ph_is_feature_enabled("new-ui", 0))
        ph_capture("new_ui_seen", NULL);

    /* Block until the queue drains (or 3s elapses), then tear down. */
    ph_flush(3000);
    ph_shutdown();

    printf("done\n");
    return 0;
}
