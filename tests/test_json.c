#include "ph_json.h"
#include "ph_str.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>

void suite_json(void) {
    ph_strbuf b;

    ph_strbuf_init(&b);
    ph_json_cstr(&b, "hello");
    CHECK(b.data && strcmp(b.data, "\"hello\"") == 0);
    ph_strbuf_free(&b);

    /* escapes: quote, backslash, newline */
    ph_strbuf_init(&b);
    ph_json_cstr(&b, "a\"b\\c\n");
    CHECK_CONTAINS(b.data, "\\\"");
    CHECK_CONTAINS(b.data, "\\\\");
    CHECK_CONTAINS(b.data, "\\n");
    ph_strbuf_free(&b);

    /* control char -> \u00XX */
    ph_strbuf_init(&b);
    {
        char s[2];
        s[0] = 0x01;
        s[1] = 0;
        ph_json_cstr(&b, s);
    }
    CHECK_CONTAINS(b.data, "\\u0001");
    ph_strbuf_free(&b);

    /* malformed UTF-8 is replaced, never emitted as invalid JSON text */
    ph_strbuf_init(&b);
    {
        const char bad[] = {(char)0xC0, (char)0xAF};
        static const unsigned char expected[] = {
            '"', 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, '"'
        };
        ph_json_str(&b, bad, sizeof(bad));
        CHECK(b.len == sizeof(expected));
        CHECK(memcmp(b.data, expected, sizeof(expected)) == 0);
    }
    ph_strbuf_free(&b);

    /* doubles: shortest round-trip */
    ph_strbuf_init(&b); ph_json_double(&b, 0.1);  CHECK(strcmp(b.data, "0.1") == 0);  ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_double(&b, 1.5);  CHECK(strcmp(b.data, "1.5") == 0);  ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_double(&b, 42.0); CHECK(strcmp(b.data, "42") == 0);   ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_double(&b, -2.25); CHECK(strcmp(b.data, "-2.25") == 0); ph_strbuf_free(&b);

    /* The host's numeric locale must not leak into JSON. Not every CI image has
     * a comma-decimal locale installed, so exercise the assertion when one is. */
    {
        static const char *locales[] = {
            "de-DE", "German_Germany.1252", "de_DE.UTF-8", "fr_FR.UTF-8"
        };
        char saved[128];
        const char *old = setlocale(LC_NUMERIC, NULL);
        size_t i;
        saved[0] = '\0';
        if (old) {
            strncpy(saved, old, sizeof(saved) - 1);
            saved[sizeof(saved) - 1] = '\0';
        }
        for (i = 0; i < sizeof(locales) / sizeof(locales[0]); i++) {
            if (setlocale(LC_NUMERIC, locales[i])) {
                ph_strbuf_init(&b);
                ph_json_double(&b, 1.5);
                CHECK(strcmp(b.data, "1.5") == 0);
                ph_strbuf_free(&b);
                break;
            }
        }
        if (saved[0]) setlocale(LC_NUMERIC, saved);
    }

    /* non-finite -> null (JSON has no NaN/Inf) */
    ph_strbuf_init(&b); ph_json_double(&b, strtod("nan", NULL)); CHECK(strcmp(b.data, "null") == 0); ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_double(&b, strtod("inf", NULL)); CHECK(strcmp(b.data, "null") == 0); ph_strbuf_free(&b);

    ph_strbuf_init(&b); ph_json_int(&b, -5); CHECK(strcmp(b.data, "-5") == 0); ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_int(&b, 9007199254740993LL);
    CHECK(strcmp(b.data, "9007199254740993") == 0); ph_strbuf_free(&b);

    ph_strbuf_init(&b); ph_json_bool(&b, 1); CHECK(strcmp(b.data, "true") == 0); ph_strbuf_free(&b);
    ph_strbuf_init(&b); ph_json_bool(&b, 0); CHECK(strcmp(b.data, "false") == 0); ph_strbuf_free(&b);
}
