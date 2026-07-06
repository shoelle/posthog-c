/*
 * posthog.hpp — a thin, header-only C++ convenience layer over posthog.h.
 *
 * The C ABI stays the source of truth (stable, bindable from any language);
 * this just adds ergonomics for C++ call sites: a fluent Props builder, RAII
 * init/shutdown, and overloads that accept std::string. It adds no state and
 * no allocation of its own — every call forwards straight to the C API.
 *
 * C++11 or later. No exceptions, no RTTI, so it drops cleanly into engines
 * built with -fno-exceptions/-fno-rtti.
 */
#ifndef POSTHOG_HPP
#define POSTHOG_HPP

#include "posthog.h"

#include <cstdint>
#include <string>

namespace posthog {

/* A stack-friendly wrapper around ph_props with a fluent, chainable builder.
 * Distinct method names (str/num/i64/boolean) sidestep the int/bool overload
 * ambiguity a single overloaded set() would create. */
class Props {
public:
    Props() { ph_props_init(&p_); }

    Props &str(const char *key, const char *val) { ph_props_set_str(&p_, key, val); return *this; }
    Props &str(const char *key, const std::string &val) { ph_props_set_str(&p_, key, val.c_str()); return *this; }
    Props &num(const char *key, double val) { ph_props_set_double(&p_, key, val); return *this; }
    Props &i64(const char *key, long long val) { ph_props_set_int(&p_, key, (int64_t)val); return *this; }
    Props &boolean(const char *key, bool val) { ph_props_set_bool(&p_, key, val ? 1 : 0); return *this; }

    const ph_props *raw() const { return &p_; }

private:
    ph_props p_;
};

inline const char *version() { return ph_version(); }

inline ph_result init(const ph_config &cfg) { return ph_init(&cfg); }

inline void capture(const char *event) { ph_capture(event, nullptr); }
inline void capture(const char *event, const Props &props) { ph_capture(event, props.raw()); }

inline void identify(const char *distinct_id) { ph_identify(distinct_id, nullptr); }
inline void identify(const char *distinct_id, const Props &set_props) { ph_identify(distinct_id, set_props.raw()); }

inline void alias(const char *new_id, const char *old_id) { ph_alias(new_id, old_id); }
inline void reset() { ph_reset(); }

inline void group(const char *type, const char *key) { ph_group(type, key, nullptr); }
inline void group(const char *type, const char *key, const Props &set_props) { ph_group(type, key, set_props.raw()); }

inline void register_super(const Props &props) { ph_register(props.raw()); }
inline void unregister(const char *key) { ph_unregister(key); }

inline void capture_exception(const ph_exception &ex) { ph_capture_exception(&ex); }

inline bool is_feature_enabled(const char *key, bool fallback = false) {
    return ph_is_feature_enabled(key, fallback ? 1 : 0) != 0;
}

inline void flush(int timeout_ms) { ph_flush(timeout_ms); }
inline void shutdown() { ph_shutdown(); }

/* RAII lifetime guard: init on construction, flush + shutdown on scope exit.
 * `ok()` reports whether init succeeded. */
class Session {
public:
    explicit Session(const ph_config &cfg) : result_(ph_init(&cfg)) {}
    ~Session() {
        if (result_ == PH_OK) ph_shutdown();
    }
    bool ok() const { return result_ == PH_OK; }
    ph_result result() const { return result_; }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

private:
    ph_result result_;
};

} // namespace posthog

#endif /* POSTHOG_HPP */
