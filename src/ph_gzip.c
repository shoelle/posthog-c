#include "ph_gzip.h"

#define SDEFL_IMPLEMENTATION
#include "sdefl.h"

#include <stdlib.h>

/* Standard CRC-32 (IEEE 802.3, poly 0xEDB88320) - the checksum gzip trailers
 * carry. Table built once; only ever called from the single sender thread. */
static unsigned long crc32_ieee(const unsigned char *data, size_t len) {
    static unsigned long table[256];
    static int built = 0;
    unsigned long crc;
    size_t i;
    if (!built) {
        int j;
        for (i = 0; i < 256; i++) {
            unsigned long c = (unsigned long)i;
            for (j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    crc = 0xFFFFFFFFUL;
    for (i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

int ph_gzip(const char *in, size_t in_len, char **out, size_t *out_len) {
    struct sdefl *s;
    unsigned char *buf;
    int bound, dlen;
    size_t total;
    unsigned long crc, isize;

    *out = NULL;
    *out_len = 0;
    if (!in || in_len > 0x7fffffffUL) return -1; /* sdeflate takes an int length */

    bound = sdefl_bound((int)in_len);
    if (bound < 0) return -1;

    buf = (unsigned char *)malloc(10 + (size_t)bound + 8);
    if (!buf) return -1;
    /* ~260 KB of hash/window tables - heap, never the stack. */
    s = (struct sdefl *)calloc(1, sizeof(struct sdefl));
    if (!s) {
        free(buf);
        return -1;
    }

    /* gzip header (RFC 1952): magic, CM=deflate(8), no flags, mtime=0, XFL=0,
     * OS=unknown(255). */
    buf[0] = 0x1f;
    buf[1] = 0x8b;
    buf[2] = 8;
    buf[3] = 0;
    buf[4] = buf[5] = buf[6] = buf[7] = 0;
    buf[8] = 0;
    buf[9] = 0xff;

    dlen = sdeflate(s, buf + 10, in, (int)in_len, SDEFL_LVL_DEF);
    free(s);
    if (dlen < 0) {
        free(buf);
        return -1;
    }

    total = 10 + (size_t)dlen;
    crc = crc32_ieee((const unsigned char *)in, in_len);
    isize = (unsigned long)(in_len & 0xFFFFFFFFUL);

    /* CRC-32 then ISIZE, both little-endian. */
    buf[total + 0] = (unsigned char)(crc & 0xFF);
    buf[total + 1] = (unsigned char)((crc >> 8) & 0xFF);
    buf[total + 2] = (unsigned char)((crc >> 16) & 0xFF);
    buf[total + 3] = (unsigned char)((crc >> 24) & 0xFF);
    buf[total + 4] = (unsigned char)(isize & 0xFF);
    buf[total + 5] = (unsigned char)((isize >> 8) & 0xFF);
    buf[total + 6] = (unsigned char)((isize >> 16) & 0xFF);
    buf[total + 7] = (unsigned char)((isize >> 24) & 0xFF);
    total += 8;

    *out = (char *)buf;
    *out_len = total;
    return 0;
}
