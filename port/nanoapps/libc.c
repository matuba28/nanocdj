/* Minimal libc pieces for the freestanding NanoApps build: heap, qsort, snprintf. */
#include "compat.h"
#include <string.h>
#include "hb_heap.h"

/* ---------------- heap ----------------
 * The OS allocator, with a header holding the size (for realloc) and links in a
 * list of every live block. The list matters: these blocks live in the OS heap,
 * outside the app image, so when NanoApps unloads the app nothing frees them.
 * libc_free_all() hands them all back from the unload hook; without it every
 * visit to the app leaked ~2.5 MB and the second or third one ran the iPod out
 * of memory (it reboots when an OS allocation fails).
 * Integer-only: libc_free_all runs on the resident's task, which has no FPU. */

typedef struct blk { struct blk *prev, *next; uint32_t size, pad; } blk_hdr;   /* 16 bytes: 8-byte aligned */
static blk_hdr *live;
static uint32_t live_count;

void *malloc(size_t n)
{
    /* the OS allocator reboots the iPod when it fails: refuse first instead */
    if (n > 0x7fffffffu || (uint32_t)n + 256u * 1024u > hb_os_heap_largest()) return NULL;
    blk_hdr *h = hb_os_alloc((uint32_t)(n + sizeof(blk_hdr)));
    if (!h) return NULL;
    h->size = (uint32_t)n;
    h->prev = NULL; h->next = live;
    if (live) live->prev = h;
    live = h; live_count++;
    return h + 1;
}

void free(void *p)
{
    if (!p) return;
    blk_hdr *h = (blk_hdr *)p - 1;
    if (h->prev) h->prev->next = h->next; else live = h->next;
    if (h->next) h->next->prev = h->prev;
    live_count--;
    hb_os_free(h);
}

/* Free every block still allocated. Returns how many there were. */
uint32_t libc_free_all(void)
{
    uint32_t n = 0;
    while (live) { blk_hdr *h = live; live = h->next; hb_os_free(h); n++; }
    live_count = 0;
    return n;
}

uint32_t libc_live_blocks(void) { return live_count; }

void *calloc(size_t n, size_t m)
{
    void *p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    blk_hdr *h = (blk_hdr *)p - 1;
    if (n <= h->size) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size);
    free(p);
    return q;
}

/* ---------------- qsort (shell sort; our arrays are small) ---------------- */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n)
{
    while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *b = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
                swap_bytes(b + (j - gap) * size, b + j * size, size);
}

/* ---------------- snprintf: %d %i %u %x %s %c %f %%, flags + 0 -, width, .prec, l/ll ---------------- */

typedef struct { char *o; size_t cap, n; } sink;

static void put(sink *s, char c)
{
    if (s->n + 1 < s->cap) s->o[s->n] = c;
    s->n++;
}

static void put_padded(sink *s, const char *body, int len, int width, int left, char padc, char sign)
{
    int total = len + (sign ? 1 : 0);
    if (!left && padc == ' ') for (int i = total; i < width; i++) put(s, ' ');
    if (sign) put(s, sign);
    if (!left && padc == '0') for (int i = total; i < width; i++) put(s, '0');
    for (int i = 0; i < len; i++) put(s, body[i]);
    if (left) for (int i = total; i < width; i++) put(s, ' ');
}

static int utoa_rev(unsigned long long v, unsigned base, char *buf)
{
    int n = 0;
    do { unsigned d = (unsigned)(v % base); buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    return n;
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    sink s = { out, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&s, *fmt); continue; }
        fmt++;
        int left = 0, plus = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l') { lng++; fmt++; }
        char buf[48], tmp[48];
        int len = 0;
        char sign = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
            if (v < 0) sign = '-'; else if (plus) sign = '+';
            int n = utoa_rev(u, 10, tmp);
            for (int i = 0; i < n; i++) buf[len++] = tmp[n - 1 - i];
            put_padded(&s, buf, len, width, left, zero ? '0' : ' ', sign);
            break;
        }
        case 'u': case 'x': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            int n = utoa_rev(v, *fmt == 'x' ? 16 : 10, tmp);
            for (int i = 0; i < n; i++) buf[len++] = tmp[n - 1 - i];
            put_padded(&s, buf, len, width, left, zero ? '0' : ' ', 0);
            break;
        }
        case 'f': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 9) prec = 9;
            if (v < 0) { sign = '-'; v = -v; } else if (plus) sign = '+';
            unsigned long long scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10;
            unsigned long long whole = (unsigned long long)(v * scale + 0.5);
            unsigned long long ip = whole / scale, fp = whole % scale;
            int n = utoa_rev(ip, 10, tmp);
            for (int i = 0; i < n; i++) buf[len++] = tmp[n - 1 - i];
            if (prec) {
                buf[len++] = '.';
                for (int i = prec - 1; i >= 0; i--) { buf[len + i] = (char)('0' + fp % 10); fp /= 10; }
                len += prec;
            }
            put_padded(&s, buf, len, width, left, zero ? '0' : ' ', sign);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int n = (int)strlen(str);
            if (prec >= 0 && n > prec) n = prec;
            if (!left) for (int i = n; i < width; i++) put(&s, ' ');
            for (int i = 0; i < n; i++) put(&s, str[i]);
            if (left) for (int i = n; i < width; i++) put(&s, ' ');
            break;
        }
        case 'c': put(&s, (char)va_arg(ap, int)); break;
        case '%': put(&s, '%'); break;
        default: put(&s, '%'); if (*fmt) put(&s, *fmt); else fmt--; break;
        }
    }
    if (cap) out[s.n < cap ? s.n : cap - 1] = 0;
    return (int)s.n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
