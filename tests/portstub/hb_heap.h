#include <stdint.h>
void *hb_os_alloc(uint32_t n);
void hb_os_free(void *p);
uint32_t hb_os_heap_largest(void);
