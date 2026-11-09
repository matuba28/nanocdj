/* Host test of the NanoApps port's libc/libm against the system versions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
static int os_live;      /* blocks the "OS" has handed out and not had back */
void *hb_os_alloc(uint32_t n) { os_live++; return malloc(n); }
void hb_os_free(void *p) { if (p) os_live--; free(p); }
uint32_t libc_free_all(void); uint32_t libc_live_blocks(void);
uint32_t hb_os_heap_largest(void) { return 64u * 1024u * 1024u; }
int t_snprintf(char *o, size_t c, const char *f, ...);
void *t_malloc(size_t), *t_realloc(void *, size_t); void t_free(void *);
void t_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
double t_floor(double), t_round(double), t_fmod(double, double), t_sqrt(double), t_exp(double), t_sin(double), t_cos(double), t_atan2(double, double);
long t_lround(double);
unsigned long long __aeabi_d2ulz(double); long long __aeabi_d2lz(double);
static int fails;
#define FMT(...) do { char a[128], b[128]; snprintf(a, sizeof a, __VA_ARGS__); t_snprintf(b, sizeof b, __VA_ARGS__); \
    if (strcmp(a, b)) { fails++; printf("fmt mismatch: '%s' vs '%s'\n", a, b); } } while (0)
#define NEAR(x, y, tol) do { double _x = (x), _y = (y); if (fabs(_x - _y) > (tol)) { fails++; printf("math %s: %.9f vs %.9f\n", #x, _x, _y); } } while (0)
static int ci(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(void)
{
    FMT("%5.1f", 144.26); FMT("%5.1f", 99.96); FMT("%+.2f%%", 2.34); FMT("%+.2f%%", -0.02); FMT("%+.2f%%", 0.0);
    FMT("%s%d:%04.1f", "-", 4, 5.27); FMT("%d:%04.1f", 12, 59.94); FMT("%.1f", 133.0); FMT("x%d", 2);
    FMT("%d%%", 10); FMT("LOOP %d.%02d", 1, 50); FMT("%s", "Sí, idealízame"); FMT("%lld", 26690394LL);
    FMT("%u", 4000000000u); FMT("%.3f", 0.0005); FMT("[%-6s]", "ab"); FMT("[%6s]", "ab"); FMT("%x", 0xbeef);
    char small[5]; int n = t_snprintf(small, sizeof small, "%s", "abcdefgh");
    if (n != 8 || strcmp(small, "abcd")) { fails++; printf("truncation: %d '%s'\n", n, small); }
    for (double v = -7.75; v < 8; v += 0.25) { NEAR(t_floor(v), floor(v), 0); NEAR(t_round(v), round(v), 0); NEAR(t_lround(v), lround(v), 0); NEAR(t_fmod(v, 2.5), fmod(v, 2.5), 1e-12); }
    for (double v = -20; v < 20; v += 0.37) { NEAR(t_sin(v), sin(v), 1e-9); NEAR(t_cos(v), cos(v), 1e-9); }
    for (double v = -12; v < 3; v += 0.31) NEAR(t_exp(v), exp(v), 1e-9 * exp(v) + 1e-15);
    for (double a = -3.1; a < 3.2; a += 0.1) NEAR(t_atan2(sin(a) * 7, cos(a) * 7), atan2(sin(a) * 7, cos(a) * 7), 1e-9);
    NEAR(t_atan2(0, -1), atan2(0, -1), 1e-9); NEAR(t_atan2(-1, 0), atan2(-1, 0), 1e-9); NEAR(t_sqrt(2), sqrt(2), 1e-12);
    double conv[] = { 0, 0.9, 1, 12345.678, 26690394.4, 4294967295.0, 4294967296.0, 4294967297.5, 1e15 + 3, 9007199254740992.0 };
    for (unsigned i = 0; i < sizeof conv / sizeof *conv; i++) {
        if (__aeabi_d2ulz(conv[i]) != (unsigned long long)conv[i]) { fails++; printf("d2ulz %f\n", conv[i]); }
        if (__aeabi_d2lz(-conv[i]) != (long long)-conv[i]) { fails++; printf("d2lz %f\n", -conv[i]); }
    }
    int arr[200]; for (int i = 0; i < 200; i++) arr[i] = (i * 7919) % 211;
    t_qsort(arr, 200, sizeof(int), ci);
    for (int i = 1; i < 200; i++) if (arr[i - 1] > arr[i]) { fails++; printf("qsort\n"); break; }
    char *p = t_malloc(10); strcpy(p, "123456789"); p = t_realloc(p, 1000); if (strcmp(p, "123456789")) { fails++; printf("realloc\n"); } t_free(p);
    /* unload: every block still allocated goes back to the OS, whatever the order */
    {
        int base = os_live;
        void *a = t_malloc(100), *b = t_malloc(2000), *c = t_malloc(30), *d = t_malloc(7);
        t_free(b);                                    /* from the middle of the list */
        c = t_realloc(c, 5000);                       /* moved */
        (void)a; (void)c; (void)d;
        if (libc_live_blocks() != 3) { fails++; printf("live blocks %u\n", libc_live_blocks()); }
        uint32_t n = libc_free_all();
        if (n != 3 || os_live != base || libc_live_blocks() != 0) { fails++; printf("free_all %u, os blocks left %d\n", n, os_live - base); }
        void *e = t_malloc(10); t_free(e);            /* still usable afterwards */
        if (os_live != base) { fails++; printf("after free_all\n"); }
    }
    printf(fails ? "%d FAILED\n" : "port libc/libm tests passed\n", fails);
    return fails != 0;
}
