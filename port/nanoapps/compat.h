/* Force-included into every nanoCDJ source when building for the iPod (NanoApps).
 * NanoApps apps are freestanding (no libc); libc.c / libm.c provide what we use. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* stdlib */
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void  free(void *p);
uint32_t libc_free_all(void);      /* unload: free every block still allocated */
uint32_t libc_live_blocks(void);
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

/* stdio (formatting only; there is no console on the device) */
int snprintf(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
#define stderr ((void *)0)
#define fprintf(f, ...) ((void)0)
#define printf(...) ((void)0)

/* math */
#define M_PI 3.14159265358979323846
double fabs(double x);
double floor(double x);
double round(double x);
long   lround(double x);
double fmod(double x, double y);
double sqrt(double x);
double hypot(double x, double y);
double exp(double x);
double sin(double x);
double cos(double x);
double atan2(double y, double x);
unsigned long long __aeabi_d2ulz(double x);
long long __aeabi_d2lz(double x);
