#include <stdio.h>
#include <string.h>

#include "golden.h"

#ifndef SQ_GOLDEN_DIR
#define SQ_GOLDEN_DIR "golden"
#endif

static int nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

size_t golden_load(const char *stem, uint8_t *out, size_t cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/blob_%s.hex", SQ_GOLDEN_DIR, stem);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "golden: cannot open %s\n", path);
        return 0;
    }

    size_t n = 0;
    int hi = -1;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        int v = nibble(c);
        if (v < 0) {
            fprintf(stderr, "golden: bad hex char '%c' in %s\n", c, path);
            fclose(f);
            return 0;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) {
                fprintf(stderr, "golden: %s exceeds %zu-byte buffer\n", path, cap);
                fclose(f);
                return 0;
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    fclose(f);

    if (hi >= 0) {
        fprintf(stderr, "golden: odd number of hex digits in %s\n", path);
        return 0;
    }
    return n;
}
