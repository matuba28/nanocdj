/* Host versions of the streaming-file HAL for tests: plain stdio, a µs clock,
 * and an iPod-sized memory figure. Optional throttle: set test_read_limit to cap
 * the bytes each read returns (0 = no cap). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static uint32_t test_read_limit;
static uint32_t test_read_us_per_kb;     /* >0: reads take this long per KB (the iPod: ~1400) */
static uint32_t test_read_max_us;        /* longest single read seen */
static uint32_t test_read_max_bytes;     /* largest single read asked for */
uint32_t hal_time_us(void);
static int test_open_files;
void *hal_file_open(const char *path) { FILE *f = fopen(path, "rb"); if (f) test_open_files++; return f; }
int32_t hal_file_read(void *f, void *buf, uint32_t n)
{
    if (test_read_limit && n > test_read_limit) n = test_read_limit;
    if (n > test_read_max_bytes) test_read_max_bytes = n;
    uint32_t t0 = hal_time_us();
    int32_t r = (int32_t)fread(buf, 1, n, (FILE *)f);
    if (test_read_us_per_kb && r > 0) { uint32_t want = (uint32_t)r * test_read_us_per_kb / 1024u; while (hal_time_us() - t0 < want) {} }
    if (hal_time_us() - t0 > test_read_max_us) test_read_max_us = hal_time_us() - t0;
    return r;
}
void hal_file_close(void *f) { fclose((FILE *)f); test_open_files--; }
uint32_t hal_time_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}
size_t hal_mem_avail(void) { return 4u * 1024u * 1024u; }
