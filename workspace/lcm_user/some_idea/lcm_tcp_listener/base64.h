/* base64.h - minimal base64 decoder */
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Decode base64 string into out_buf.
 * Returns number of bytes written, or -1 on error.
 * out_buf_size must be at least (in_len * 3) / 4 + 1. */
int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_buf_size);

#endif /* BASE64_H */
