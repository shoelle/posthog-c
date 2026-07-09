/*
 * ph_json.h - a write-only JSON encoder over ph_strbuf.
 *
 * We only ever *write* JSON (the ingestion API is fire-and-forth), and the
 * schema is small and fixed, so a hand-rolled writer beats a parser
 * dependency. Callers are responsible for structural punctuation ({}, [], :,
 * ,); these helpers emit correctly-escaped leaf values and quoted strings.
 */
#ifndef PH_JSON_H
#define PH_JSON_H

#include "ph_str.h"

#include <stddef.h>
#include <stdint.h>

/* Emit a JSON string literal (with surrounding quotes), escaping ", \, and
 * control characters per RFC 8259. UTF-8 bytes pass through unchanged. */
void ph_json_str(ph_strbuf *out, const char *s, size_t n);
void ph_json_cstr(ph_strbuf *out, const char *s);

/* Emit a JSON number. Non-finite doubles become `null` (JSON has no NaN/Inf).
 * Doubles use the shortest representation that round-trips. */
void ph_json_double(ph_strbuf *out, double d);
void ph_json_int(ph_strbuf *out, int64_t i);

/* Emit `true` or `false`. */
void ph_json_bool(ph_strbuf *out, int b);

#endif /* PH_JSON_H */
