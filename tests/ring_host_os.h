/* Simulated RetailOS sound-effect voice for testing audio_ring.c on the Mac.
 * Behaviour modelled on what NanoApps' entrain documents for RetailOS 1.1.2:
 * setSource marks a descriptor playing (+0x64 = 1) and stores the voice at +0x48;
 * at end of buffer the completion handler clears +0x64 and follows +0x54 (next).
 * The mixer pulls in passes (SIM_PASS frames) ahead of the DAC, so a write that
 * lands inside an already-fetched pass is not heard. A block shorter than a pass
 * is counted as an error: that is what crackled and crashed on the device. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SIM_PASS 1024u

typedef struct { uint8_t pad[0x0C]; uint32_t out_rate; uint8_t pad2[0x48 - 0x10]; uint32_t counter; } sim_voice_t;

extern uint32_t sim_now_us;
extern sim_voice_t sim_voice;
extern int sim_voices_started, sim_double_voice;
void sim_play(uint8_t *d);

/* 64-bit host pointers -> 32-bit handles via a registry */
void *sim_ptrs[256]; int sim_nptrs;
static inline uint32_t sim_p2u(void *p) {
    if (!p) return 0;
    for (int i = 0; i < sim_nptrs; i++) if (sim_ptrs[i] == p) return (uint32_t)(i + 1) * 0x10000u;
    sim_ptrs[sim_nptrs++] = p; return (uint32_t)sim_nptrs * 0x10000u;
}
static inline void *sim_u2p(uint32_t u) { return u ? sim_ptrs[u / 0x10000u - 1] : 0; }
#define PTR_TO_U32(p) sim_p2u((void *)(p))
#define U32_TO_PTR(u) sim_u2p(u)

#define OS_DESC_CTOR(d)  memset((d), 0, 0x80)
#define OS_PLAY(d)       sim_play((uint8_t *)(d))
#define OS_NOW_US()      sim_now_us
#define OS_ALLOC(n)      malloc(n)
#define OS_LARGEST()     (64u * 1024u * 1024u)
