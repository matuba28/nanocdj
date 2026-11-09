/* Small libm for the freestanding NanoApps build. Accuracy is plenty for UI maths
 * (jog angles, platter marker, decays); the audio path itself only needs floor(). */
#include "compat.h"

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x)
{
    if (x >= 9.0e15 || x <= -9.0e15) return x;
    long long i = (long long)x;
    double d = (double)i;
    return (d > x) ? d - 1.0 : d;
}

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
long lround(double x) { return (long)round(x); }

double fmod(double x, double y)
{
    if (y == 0) return 0;
    double q = x / y;
    q = q < 0 ? -floor(-q) : floor(q);   /* truncate toward zero */
    return x - q * y;
}

double sqrt(double x)
{
    if (x <= 0) return 0;
#if defined(__arm__) && defined(__ARM_FP) && (__ARM_FP & 8)
    double r;
    __asm__("vsqrt.f64 %P0, %P1" : "=w"(r) : "w"(x));
    return r;
#else
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 30; i++) r = 0.5 * (r + x / r);
    return r;
#endif
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }

double exp(double x)
{
    if (x < -700) return 0;
    if (x > 700) x = 700;
    /* exp(x) = 2^k * exp(r), |r| <= ln2/2 */
    const double LN2 = 0.6931471805599453;
    int k = (int)(x / LN2 + (x < 0 ? -0.5 : 0.5));
    double r = x - k * LN2, term = 1, sum = 1;
    for (int i = 1; i < 14; i++) { term *= r / i; sum += term; }
    while (k > 0) { sum *= 2; k--; }
    while (k < 0) { sum *= 0.5; k++; }
    return sum;
}

static double reduce_pi(double x)   /* to [-pi, pi] */
{
    const double TWO_PI = 2 * M_PI;
    x = fmod(x, TWO_PI);
    if (x > M_PI) x -= TWO_PI;
    if (x < -M_PI) x += TWO_PI;
    return x;
}

double sin(double x)
{
    x = reduce_pi(x);
    if (x > M_PI / 2) x = M_PI - x;          /* fold into [-pi/2, pi/2] */
    if (x < -M_PI / 2) x = -M_PI - x;
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 10; i++) { term *= -x2 / ((2 * i) * (2 * i + 1)); sum += term; }
    return sum;
}

double cos(double x) { return sin(x + M_PI / 2); }

static double atan_small(double z)   /* |z| <= 1 */
{
    /* atan(z) = 2*atan(z / (1 + sqrt(1 + z^2))) brings |z| <= 0.414, then a series */
    double t = z / (1 + sqrt(1 + z * z));
    double t2 = t * t, term = t, sum = t;
    for (int i = 1; i < 18; i++) { term *= -t2; sum += term / (2 * i + 1); }
    return 2 * sum;
}

double atan2(double y, double x)
{
    if (x == 0 && y == 0) return 0;
    double ax = fabs(x), ay = fabs(y), a;
    if (ay <= ax) a = atan_small(ay / ax);
    else a = M_PI / 2 - atan_small(ax / ay);
    if (x < 0) a = M_PI - a;
    return y < 0 ? -a : a;
}

/* double -> 64-bit integer conversions. libgcc's versions are Thumb code containing
 * VFP instructions, which trap on this firmware (see hb_sdk.h), so provide ARM-state
 * replacements; the linker then never pulls libgcc's in. */
unsigned long long __aeabi_d2ulz(double x)
{
    if (!(x > 0)) return 0;
    if (x >= 18446744073709551615.0) return ~0ull;
    const double TWO32 = 4294967296.0;
    unsigned hi = (unsigned)(x / TWO32);          /* 32-bit conversions are single VFP instructions */
    double lo = x - (double)hi * TWO32;
    if (lo < 0) { hi--; lo += TWO32; }
    if (lo >= TWO32) { hi++; lo -= TWO32; }
    return ((unsigned long long)hi << 32) | (unsigned)lo;
}

long long __aeabi_d2lz(double x)
{
    return x < 0 ? -(long long)__aeabi_d2ulz(-x) : (long long)__aeabi_d2ulz(x);
}
