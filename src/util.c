#include "util.h"

static size_t put_utf8(uint32_t cp, char *out, size_t pos, size_t cap)
{
    if (cp < 0x80) { if (pos + 1 < cap) out[pos++] = (char)cp; }
    else if (cp < 0x800) { if (pos + 2 < cap) { out[pos++] = (char)(0xc0 | cp >> 6); out[pos++] = (char)(0x80 | (cp & 0x3f)); } }
    else if (cp < 0x10000) { if (pos + 3 < cap) { out[pos++] = (char)(0xe0 | cp >> 12); out[pos++] = (char)(0x80 | (cp >> 6 & 0x3f)); out[pos++] = (char)(0x80 | (cp & 0x3f)); } }
    else { if (pos + 4 < cap) { out[pos++] = (char)(0xf0 | cp >> 18); out[pos++] = (char)(0x80 | (cp >> 12 & 0x3f)); out[pos++] = (char)(0x80 | (cp >> 6 & 0x3f)); out[pos++] = (char)(0x80 | (cp & 0x3f)); } }
    return pos;
}

void utf16_to_utf8(const uint8_t *src, int units, int be, char *out, size_t cap)
{
    size_t pos = 0;
    if (!cap) return;
    for (int i = 0; i < units; i++) {
        uint32_t u = be ? (uint32_t)(src[2 * i] << 8 | src[2 * i + 1]) : (uint32_t)(src[2 * i] | src[2 * i + 1] << 8);
        if (u == 0) break;
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < units) {
            uint32_t lo = be ? (uint32_t)(src[2 * i + 2] << 8 | src[2 * i + 3]) : (uint32_t)(src[2 * i + 2] | src[2 * i + 3] << 8);
            if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); i++; }
        }
        pos = put_utf8(u, out, pos, cap);
    }
    out[pos] = 0;
}

uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s;
    if (!c) return 0;
    int n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (n) c &= 0x3f >> n;
    s++;
    for (int i = 0; i < n && (*s & 0xc0) == 0x80; i++) c = c << 6 | (*s++ & 0x3f);
    *ps = (const char *)s;
    return c;
}
