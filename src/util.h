#pragma once
#include <stdint.h>
#include <stddef.h>

/* Convert `units` UTF-16 code units (big- or little-endian) to NUL-terminated UTF-8. */
void utf16_to_utf8(const uint8_t *src, int units, int big_endian, char *out, size_t cap);

/* Decode next UTF-8 code point from *s, advancing it. Returns 0 at end. */
uint32_t utf8_next(const char **s);
