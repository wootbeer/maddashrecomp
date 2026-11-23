/*
 * The game's file stream reader, waited on without a kernel call.
 *
 * Mad Dash reads its .bgx files through a reader object (vtable 0x002B6980)
 * that keeps up to 8 overlapped 32 KB reads in flight, done by a worker
 * thread (0x00180D10). Its consumer (0x00180800) waits for data by calling
 * the reader's slot 3 (0x001814F0: collect finished reads, queue new ones,
 * return how far the buffer is filled) in a loop until the answer moves.
 * While reads are in flight that loop makes no kernel call. On the console
 * the scheduler preempts it and the worker, at a higher priority, finishes
 * its read; under the guest CPU lock (one guest thread at a time, switching
 * at kernel calls) the worker could never get the CPU back from its
 * ReadFile and the two would wait for each other for good. So when a call
 * made no progress, the CPU is handed over for a moment, as a quantum
 * ending would.
 */

#include "recomp_types.h"

#include <SDL3/SDL.h>

int guest_cpu_yield_begin(void);       /* xboxrecomp, patch 0023 */
void guest_cpu_yield_end(int held);

/* Reader::slot 3 (thiscall, the reader in ecx; `ret`): returns the filled
 * position (+0x10); reads collected so far at +0xC8. */
extern void sub_001814F0_gen(void);
void sub_001814F0(void)
{
    uint32_t self = g_ecx;
    uint32_t filled = MEM32(self + 0x10), collected = MEM32(self + 0xC8);

    sub_001814F0_gen();
    if (g_eax == filled && MEM32(self + 0xC8) == collected) {
        int held = guest_cpu_yield_begin();
        SDL_DelayNS(100000);
        guest_cpu_yield_end(held);
    }
}

/* The same reader's "is this read done" (cdecl, the request at [esp+4]:
 * 0 while its OVERLAPPED's status is STATUS_PENDING, 0x103), which
 * 0x00181280 (draining every read in flight, on a seek or a close) calls in
 * a loop with no kernel call: the same wait for the worker as above, met
 * on the next race's loading screen (the main thread spinning here holding
 * the guest CPU, the worker waiting to take it). Not done yet, from that
 * loop (its other callers poll once): the CPU handed over for a moment. */
extern void sub_00180C00_gen(void);
void sub_00180C00(void)
{
    uint32_t ret = MEM32(g_esp);         /* only the draining loop's call spins */

    sub_00180C00_gen();
    if (!g_eax && ret == 0x001812B6u) {
        int held = guest_cpu_yield_begin();
        SDL_DelayNS(100000);
        guest_cpu_yield_end(held);
    }
}
