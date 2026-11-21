/* MDR_BINK_DIFF=n: a test of the recompiled Bink decoder. At the n-th call
 * of Bink's frame decoder (0x00254590, `ret 0x38`: BinkDoFrame 0x0024CD40
 * calls it with the previous and next planes once the frame's data is in,
 * and it decodes each plane through 0x002527D0) the guest's RAM and the contiguous window are saved
 * before and after the call (bink_before.bin, bink_after.bin, 64 MB each
 * twice) with the stack pointer and return address (bink_regs.txt), so the
 * same call can be run again from the original x86 in an emulator and the
 * two compared (the movies' wrong 8x8 blocks). Nothing happens without it. */

#include "recomp_types.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

extern void sub_00254590_gen(void);
static int in_diff;     /* inside the decode being saved */

static void save(const char *name)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    fwrite((const void *)((uintptr_t)0 + g_xbox_mem_offset), 1, 64u << 20, f);
    fwrite((const void *)((uintptr_t)0x80000000u + g_xbox_mem_offset), 1, 64u << 20, f);
    fclose(f);
}

/* Bink's frame decoder (14 arguments) */
void sub_00254590(void)
{
    static int n = -1, calls;

    if (n < 0) {
        const char *v = getenv("MDR_BINK_DIFF");
        n = v ? atoi(v) : 0;
    }
    if (n <= 0 || ++calls != n) {
        sub_00254590_gen();
        return;
    }
    {
        FILE *r = fopen("bink_regs.txt", "w");
        if (r) {
            fprintf(r, "esp %08x ret %08x arg %08x fs %08x\n", g_esp, MEM32(g_esp), MEM32(g_esp + 4), g_fs_base);
            fclose(r);
        }
    }
    save("bink_before.bin");
    in_diff = 1;
    sub_00254590_gen();
    in_diff = 0;
    save("bink_after.bin");
    {
        FILE *r = fopen("bink_regs.txt", "a");
        if (r) {
            fprintf(r, "eax %08x esp %08x\n", g_eax, g_esp);
            fclose(r);
        }
    }
    SDL_Log("bink: decode %d saved before and after", n);
}

/* MDR_BINK_BLOCK=va (hex): the inputs of Bink's 8x8 inverse DCT (0x0025ACF0:
 * coefficients at eax, 64 shorts; the dequantizer at edx, 64 ints; the
 * destination block its stack argument) for the block written at va. */
void sub_0025ACF0_enter(void)
{
    static long want = -1;
    char line[1024];
    int n = 0;

    if (want < 0) {
        const char *v = getenv("MDR_BINK_BLOCK");
        want = v ? strtol(v, NULL, 16) : 0;
    }
    if (!want || MEM32(g_esp + 4) != (uint32_t)want || (getenv("MDR_BINK_DIFF") && !in_diff))
        return;
    for (int i = 0; i < 64; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %d", (int16_t)MEM16(g_eax + 2u * (uint32_t)i));
    SDL_Log("bink block %08x coefficients:%s", (unsigned)want, line);
    n = 0;
    for (int i = 0; i < 64; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %d", (int32_t)MEM32(g_edx + 4u * (uint32_t)i));
    SDL_Log("bink block %08x dequantizer:%s", (unsigned)want, line);
    {   /* the frame it will use: 0x13C bytes below the return address */
        FILE *f = fopen("bink_block_stack.bin", "wb");
        if (f) {
            fwrite((const void *)((uintptr_t)(g_esp - 0x140u) + g_xbox_mem_offset), 1, 0x144, f);
            fclose(f);
        }
        SDL_Log("bink block %08x esp %08x", (unsigned)want, g_esp);
    }
}
