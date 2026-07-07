/*
 * gzip container checks. Full decompress round-trips are verified against a
 * real gunzip (Python) and a live PostHog POST; here we assert the container
 * structure the packer produces (magic, method, ISIZE trailer, shrinkage).
 */
#include "ph_gzip.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

void suite_gzip(void) {
    char *out = NULL;
    size_t out_len = 0;
    char big[2000];
    int i;

    for (i = 0; i < (int)sizeof(big); i++) big[i] = "abcd"[i % 4];

    CHECK(ph_gzip(big, sizeof(big), &out, &out_len) == 0);
    CHECK(out != NULL);
    if (out) {
        CHECK(out_len >= 18); /* 10-byte header + deflate + 8-byte trailer */
        CHECK((unsigned char)out[0] == 0x1f);  /* gzip magic */
        CHECK((unsigned char)out[1] == 0x8b);
        CHECK((unsigned char)out[2] == 0x08);  /* CM = deflate */
        /* ISIZE trailer (last 4 bytes, little-endian) == input length */
        {
            unsigned long isize = (unsigned long)(unsigned char)out[out_len - 4] |
                                  ((unsigned long)(unsigned char)out[out_len - 3] << 8) |
                                  ((unsigned long)(unsigned char)out[out_len - 2] << 16) |
                                  ((unsigned long)(unsigned char)out[out_len - 1] << 24);
            CHECK(isize == (unsigned long)sizeof(big));
        }
        CHECK(out_len < sizeof(big)); /* repetitive input compresses */
        free(out);
    }

    /* empty input still yields a valid gzip stream */
    out = NULL;
    out_len = 0;
    CHECK(ph_gzip("", 0, &out, &out_len) == 0);
    CHECK(out != NULL);
    if (out) {
        CHECK((unsigned char)out[0] == 0x1f);
        CHECK((unsigned char)out[1] == 0x8b);
        free(out);
    }
}
