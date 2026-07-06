#include "ph_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_hex[] = "0123456789abcdef";

void ph_json_str(ph_strbuf *out, const char *s, size_t n) {
    size_t i;
    ph_strbuf_append_char(out, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  ph_strbuf_append(out, "\\\"", 2); break;
            case '\\': ph_strbuf_append(out, "\\\\", 2); break;
            case '\b': ph_strbuf_append(out, "\\b", 2); break;
            case '\f': ph_strbuf_append(out, "\\f", 2); break;
            case '\n': ph_strbuf_append(out, "\\n", 2); break;
            case '\r': ph_strbuf_append(out, "\\r", 2); break;
            case '\t': ph_strbuf_append(out, "\\t", 2); break;
            default:
                if (c < 0x20) {
                    char esc[6];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = k_hex[(c >> 4) & 0xF];
                    esc[5] = k_hex[c & 0xF];
                    ph_strbuf_append(out, esc, 6);
                } else {
                    /* Printable ASCII or a UTF-8 continuation/lead byte:
                     * valid JSON permits raw UTF-8, so pass it through. */
                    ph_strbuf_append_char(out, (char)c);
                }
                break;
        }
    }
    ph_strbuf_append_char(out, '"');
}

void ph_json_cstr(ph_strbuf *out, const char *s) {
    ph_json_str(out, s ? s : "", s ? strlen(s) : 0);
}

void ph_json_double(ph_strbuf *out, double d) {
    char buf[40];
    int prec;

    /* JSON has no NaN/Infinity; emit null rather than an invalid token that
     * PostHog would reject for the whole batch. (d != d) is true only for NaN. */
    if (d != d || d > 1.7976931348623157e308 || d < -1.7976931348623157e308) {
        ph_strbuf_append(out, "null", 4);
        return;
    }

    /* Shortest round-tripping decimal: the smallest precision whose output
     * parses back to the exact same double. Keeps "0.1" from becoming
     * "0.10000000000000001" while staying lossless. */
    for (prec = 1; prec < 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (prec >= 17) snprintf(buf, sizeof(buf), "%.17g", d);
    ph_strbuf_append_cstr(out, buf);
}

void ph_json_int(ph_strbuf *out, int64_t i) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)i);
    ph_strbuf_append_cstr(out, buf);
}

void ph_json_bool(ph_strbuf *out, int b) {
    if (b) ph_strbuf_append(out, "true", 4);
    else ph_strbuf_append(out, "false", 5);
}
