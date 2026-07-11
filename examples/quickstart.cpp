/*
 * quickstart.cpp - the same flow through the header-only C++ wrapper.
 *
 * Build + run:  zig build run-example-cpp
 *
 * Demonstrates the fluent Props builder and the RAII Session guard, which
 * flushes and shuts down on scope exit.
 */
#include "posthog.hpp"

#include <cstdio>

int main() {
    ph_config cfg;
    ph_config_defaults(&cfg);
    cfg.api_key = "phc_your_project_key";
    cfg.api_host = "http://localhost:8000"; // dev proxy; or https://us.i.posthog.com
    cfg.distinct_id = "install-id-from-your-storage"; // create once + persist in app settings
    cfg.release = "quickstart-cpp@0.1.0";

    posthog::Session session(cfg); // ph_init here; flush + shutdown at scope exit
    if (!session.ok()) {
        std::fprintf(stderr, "posthog init failed\n");
        return 1;
    }
    std::printf("posthog-c %s initialized (C++ wrapper)\n", posthog::version());

    posthog::capture("quickstart_ran",
                     posthog::Props().str("source", "example").i64("answer", 42).boolean("first_run", true));

    posthog::identify("user-123", posthog::Props().str("plan", "free"));
    posthog::capture("did_thing");

    posthog::flush(3000);
    std::printf("done\n");
    return 0;
}
