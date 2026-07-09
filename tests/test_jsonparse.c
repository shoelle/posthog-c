#include "ph_jsonval.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

void suite_jsonparse(void) {
    /* scalars in an object */
    {
        const char *s = "{\"a\":1,\"b\":true,\"c\":\"hi\",\"d\":null}";
        ph_jv *v = ph_jv_parse(s, strlen(s));
        CHECK(v && ph_jv_type_of(v) == PH_JV_OBJ);
        CHECK(ph_jv_num(ph_jv_get(v, "a")) == 1.0);
        CHECK(ph_jv_bool(ph_jv_get(v, "b")) == 1);
        CHECK(ph_jv_str(ph_jv_get(v, "c")) && strcmp(ph_jv_str(ph_jv_get(v, "c")), "hi") == 0);
        CHECK(ph_jv_type_of(ph_jv_get(v, "d")) == PH_JV_NULL);
        CHECK(ph_jv_get(v, "missing") == NULL);
        ph_jv_free(v);
    }

    /* nested object + array */
    {
        const char *s = "{\"arr\":[10,20,30],\"obj\":{\"x\":\"y\"}}";
        ph_jv *v = ph_jv_parse(s, strlen(s));
        const ph_jv *arr = ph_jv_get(v, "arr");
        CHECK(ph_jv_len(arr) == 3);
        CHECK(ph_jv_num(ph_jv_at(arr, 1)) == 20.0);
        CHECK(strcmp(ph_jv_str(ph_jv_get(ph_jv_get(v, "obj"), "x")), "y") == 0);
        ph_jv_free(v);
    }

    /* escapes, including \uXXXX */
    {
        const char *s = "{\"k\":\"a\\nb\\\"c\\u0041\"}";
        ph_jv *v = ph_jv_parse(s, strlen(s));
        const char *k = ph_jv_str(ph_jv_get(v, "k"));
        CHECK(k && strcmp(k, "a\nb\"cA") == 0); /* A == 'A' */
        ph_jv_free(v);
    }

    /* the real /flags/ response shape */
    {
        const char *s =
            "{\"flags\":{"
            "\"my-flag\":{\"key\":\"my-flag\",\"enabled\":true,"
            "\"metadata\":{\"payload\":\"{\\\"color\\\":\\\"red\\\"}\"}},"
            "\"mv\":{\"key\":\"mv\",\"enabled\":true,\"variant\":\"test-a\"}"
            "},\"errorsWhileComputingFlags\":false}";
        ph_jv *v = ph_jv_parse(s, strlen(s));
        const ph_jv *flags = ph_jv_get(v, "flags");
        const ph_jv *f = ph_jv_get(flags, "my-flag");
        const ph_jv *mv = ph_jv_get(flags, "mv");
        CHECK(ph_jv_len(flags) == 2);
        CHECK(ph_jv_bool(ph_jv_get(f, "enabled")) == 1);
        CHECK(strcmp(ph_jv_str(ph_jv_get(ph_jv_get(f, "metadata"), "payload")),
                     "{\"color\":\"red\"}") == 0);
        CHECK(strcmp(ph_jv_str(ph_jv_get(mv, "variant")), "test-a") == 0);
        CHECK(ph_jv_key_at(flags, 0) != NULL);
        ph_jv_free(v);
    }

    /* malformed inputs return NULL, not a partial tree */
    {
        const char *bad1 = "{\"a\":}";
        const char *bad2 = "{unquoted:1}";
        ph_jv *v1 = ph_jv_parse(bad1, strlen(bad1));
        ph_jv *v2 = ph_jv_parse(bad2, strlen(bad2));
        CHECK(v1 == NULL);
        CHECK(v2 == NULL);
    }

    /* deeply nested input is rejected at the depth cap, not a stack overflow
     * (found by the fuzzer; without the cap this crashes the runner). */
    {
        size_t n = 100000, i;
        char *deep = (char *)malloc(n);
        if (deep) {
            ph_jv *v;
            for (i = 0; i < n; i++) deep[i] = '[';
            v = ph_jv_parse(deep, n);
            CHECK(v == NULL); /* rejected, not crashed */
            ph_jv_free(v);    /* NULL-safe */
            free(deep);
        }
    }
}
