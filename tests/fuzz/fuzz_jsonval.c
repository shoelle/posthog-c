/*
 * Fuzz target: the /flags/ JSON parser (ph_jv_parse) — the SDK's only parser of
 * server-controlled, network-delivered bytes. Parse, walk the tree (to touch
 * node memory + the accessors), free.
 */
#include "fuzz.h"
#include "ph_jsonval.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ph_jv *v = ph_jv_parse((const char *)data, size);
    if (v) {
        int i, n = ph_jv_len(v);
        (void)ph_jv_type_of(v);
        (void)ph_jv_bool(v);
        (void)ph_jv_num(v);
        (void)ph_jv_str(v);
        for (i = 0; i < n && i < 16; i++) {
            (void)ph_jv_str(ph_jv_at(v, i));
            (void)ph_jv_num(ph_jv_val_at(v, i));
            (void)ph_jv_key_at(v, i);
        }
        (void)ph_jv_get(v, "flags");
        ph_jv_free(v);
    }
    return 0;
}

static char deep_small[8000];
static char deep_big[300000];
static fuzz_seed seeds[8];
static size_t g_count;
static int inited;

const fuzz_seed *fuzz_seeds(size_t *count) {
    if (!inited) {
        size_t i = 0;
        static const char s0[] =
            "{\"flags\":{\"my-flag\":{\"enabled\":true,\"variant\":\"test\","
            "\"metadata\":{\"payload\":\"{\\\"k\\\":1}\"}}}}";
        static const char s1[] =
            "[1,2,3,-4.5e10,true,false,null,\"str\",{\"a\":[]}]";
        static const char s2[] = "\"\\u00e9\\uD83D\\uDE00 tail\""; /* escapes + surrogate */
        static const char s3[] = "{\"a\":1,\"b\":[{\"c\":[1,[2,[3]]]}]}";
        static const char s4[] = "   \t\n  {  }  ";
        /* Deeply nested arrays: a malicious /flags/ body. A recursive-descent
         * parser with no depth cap stack-overflows on these. */
        memset(deep_small, '[', sizeof deep_small);
        memset(deep_big, '[', sizeof deep_big);
        seeds[i].data = s0; seeds[i++].len = sizeof s0 - 1;
        seeds[i].data = s1; seeds[i++].len = sizeof s1 - 1;
        seeds[i].data = s2; seeds[i++].len = sizeof s2 - 1;
        seeds[i].data = s3; seeds[i++].len = sizeof s3 - 1;
        seeds[i].data = s4; seeds[i++].len = sizeof s4 - 1;
        seeds[i].data = deep_small; seeds[i++].len = sizeof deep_small;
        seeds[i].data = deep_big; seeds[i++].len = sizeof deep_big;
        g_count = i;
        inited = 1;
    }
    *count = g_count;
    return seeds;
}
