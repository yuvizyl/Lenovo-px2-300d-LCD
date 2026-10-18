/* base64.c - minimal base64 decoder, no external deps */
#include "base64.h"
#include <string.h>

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_buf_size)
{
    size_t i = 0, j = 0;
    int have[4];
    int pad = 0;

    while (i < in_len) {
        int k = 0;
        while (k < 4 && i < in_len) {
            char c = in[i++];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
                continue;
            if (c == '=') {
                have[k++] = -2; /* pad marker */
                pad++;
            } else {
                int v = b64_val(c);
                if (v < 0) return -1;
                have[k++] = v;
            }
        }
        if (k == 0) break;

        uint32_t triple = 0;
        for (int idx = 0; idx < 4; idx++) {
            int v = (idx < k) ? have[idx] : -2;
            triple = (triple << 6) | (v < 0 ? 0 : (uint32_t)v);
        }

        /* valid base64 groups: 4 chars; may end with 1 or 2 '=' */
        if (pad > 2) return -1;

        int bytes = 3 - pad;
        for (int b = 0; b < bytes; b++) {
            if (j >= out_buf_size) return -1;
            out[j++] = (uint8_t)((triple >> (16 - 8 * b)) & 0xFF);
        }
        pad = 0;
    }
    return (int)j;
}
