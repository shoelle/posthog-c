#include "posthog.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

void suite_props(void) {
    ph_props p;
    ph_result r;
    int i;

    ph_props_init(&p);
    CHECK(p.count == 0);

    r = ph_props_set_str(&p, "name", "hero"); CHECK(r == PH_OK); CHECK(p.count == 1);
    r = ph_props_set_int(&p, "level", 3);     CHECK(r == PH_OK);
    r = ph_props_set_double(&p, "score", 1.5); CHECK(r == PH_OK);
    r = ph_props_set_bool(&p, "alive", 1);    CHECK(r == PH_OK); CHECK(p.count == 4);

    CHECK(p.items[0].type == PH_T_STR); CHECK(strcmp(p.items[0].val.str, "hero") == 0);
    CHECK(p.items[1].type == PH_T_INT); CHECK(p.items[1].val.i64 == 3);
    CHECK(p.items[3].type == PH_T_BOOL); CHECK(p.items[3].val.boolean == 1);

    /* bad args are rejected, not stored */
    r = ph_props_set_str(&p, NULL, "x"); CHECK(r == PH_ERR_BADARG);
    r = ph_props_set_str(&p, "", "x");   CHECK(r == PH_ERR_BADARG);
    CHECK(p.count == 4);

    /* fill to capacity, then overflow */
    ph_props_init(&p);
    for (i = 0; i < PH_MAX_PROPS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        CHECK(ph_props_set_int(&p, k, i) == PH_OK);
    }
    CHECK(p.count == PH_MAX_PROPS);
    r = ph_props_set_int(&p, "overflow", 1);
    CHECK(r == PH_ERR_FULL);
    CHECK(p.dropped >= 1);

    /* an over-long value is truncated to the cap, not rejected */
    ph_props_init(&p);
    {
        char big[PH_VAL_CAP + 50];
        for (i = 0; i < (int)sizeof(big) - 1; i++) big[i] = 'x';
        big[sizeof(big) - 1] = '\0';
        r = ph_props_set_str(&p, "k", big);
        CHECK(r == PH_ERR_TRUNCATED);
        CHECK((int)strlen(p.items[0].val.str) == PH_VAL_CAP - 1);
    }
}
