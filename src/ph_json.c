#include "ph_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

static const char k_hex[] = "0123456789abcdef";

static void normalize_decimal_point(char *buf) {
    const char *point = localeconv()->decimal_point;
    size_t n;
    char *at;
    if (!point || strcmp(point, ".") == 0) return;
    n = strlen(point);
    if (n == 0 || !(at = strstr(buf, point))) return;
    *at = '.';
    if (n > 1) memmove(at + 1, at + n, strlen(at + n) + 1);
}

static int utf8_seq_len(const unsigned char *s, size_t n) {
    unsigned char c;
    if (n == 0) return 0;
    c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF)
        return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0) return s[1] >= 0xA0 && s[1] <= 0xBF ? 3 : 0;
        if (c == 0xED) return s[1] >= 0x80 && s[1] <= 0x9F ? 3 : 0;
        return (s[1] & 0xC0) == 0x80 ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0) return s[1] >= 0x90 && s[1] <= 0xBF ? 4 : 0;
        if (c == 0xF4) return s[1] >= 0x80 && s[1] <= 0x8F ? 4 : 0;
        return (s[1] & 0xC0) == 0x80 ? 4 : 0;
    }
    return 0;
}

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
                } else if (c < 0x80) {
                    ph_strbuf_append_char(out, (char)c);
                } else {
                    int seq = utf8_seq_len((const unsigned char *)s + i, n - i);
                    if (seq > 0) {
                        ph_strbuf_append(out, s + i, (size_t)seq);
                        i += (size_t)seq - 1;
                    } else {
                        /* JSON text is Unicode. Replace malformed input rather
                         * than emitting a byte stream the server/JSON.parse rejects. */
                        ph_strbuf_append(out, "\xEF\xBF\xBD", 3);
                    }
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

#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
#elif !defined(__EMSCRIPTEN__)
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t old_locale = (locale_t)0;
    if (c_locale) old_locale = uselocale(c_locale);
#endif

    /* Shortest round-tripping decimal: the smallest precision whose output
     * parses back to the exact same double. Keeps "0.1" from becoming
     * "0.10000000000000001" while staying lossless. */
    for (prec = 1; prec < 17; prec++) {
#if defined(_WIN32)
        if (c_locale)
            _snprintf_l(buf, sizeof(buf), "%.*g", c_locale, prec, d);
        else
            snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if ((c_locale ? _strtod_l(buf, NULL, c_locale) : strtod(buf, NULL)) == d) break;
#else
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
#endif
    }
    if (prec >= 17) {
#if defined(_WIN32)
        if (c_locale)
            _snprintf_l(buf, sizeof(buf), "%.17g", c_locale, d);
        else
            snprintf(buf, sizeof(buf), "%.17g", d);
#else
        snprintf(buf, sizeof(buf), "%.17g", d);
#endif
    }
#if defined(_WIN32)
    if (c_locale)
        _free_locale(c_locale);
    else
        normalize_decimal_point(buf);
#elif !defined(__EMSCRIPTEN__)
    if (c_locale) {
        uselocale(old_locale);
        freelocale(c_locale);
    } else {
        normalize_decimal_point(buf);
    }
#else
    normalize_decimal_point(buf);
#endif
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
