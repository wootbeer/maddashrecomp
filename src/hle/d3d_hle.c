/*
 * Direct3D for Mad Dash Racing, replaced at its API (bring-up).
 *
 * The title links the XDK's Direct3D 8 (D3D8 3925), which drives the NV2A
 * GPU directly. Its entry points are replaced here one by one. Carried over
 * from Orta Recomp's src/hle/d3d_hle.c (now src/hle/orta/), re-addressed:
 * every address is Mad Dash's, from Cxbx-Reloaded's XbSymbolDatabase run on
 * the player's XBE, each checked in Mad Dash's own code (`ret N`, arguments).
 * Where 3925 differs from Orta's 4928 it says so.
 *
 * Conventions: __stdcall, arguments at g_esp + 4 + 4n, each function pops
 * exactly what the original's `ret N` did.
 *
 * So far: the frame clock (BlockUntilVerticalBlank and the vblank callback),
 * CreateDevice (opens our window), Clear, Present (the end of a frame: 3925
 * has no Swap), CDevice::KickOff and D3D_BlockOnTime (no GPU to wait for).
 *
 * Not yet: draws. Mad Dash's display-list player (0x001ECBA0, with
 * 0x001ECAD0 and 0x001ECB30) writes NV2A methods into the push buffer
 * itself -- vertex arrays (0x1720/0x1760), texture stages (0x1B00..), a few
 * states -- through the device's write cursor (the device's first word,
 * 0x00248E90), and calls D3D only for the draws, vertex shader constants
 * and textures. See docs/STATE.md.
 */

#include "recomp_types.h"

#include "config.h"
#include "gfx/renderer.h"
#include "gfx/vsh_spec.h"
#include "guest/guest.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D3D_g_pDevice (XbSymbolDatabase). The device itself is a static struct
 * at 0x00248E90 in 3925; Direct3D_CreateDevice points this at it. */
#define D3D_DEVICE_PTR      0x0024BA20u
/* D3DDevice::m_VBlankCallback (XbSymbolDatabase: offset 0x24EC; Orta's 4928
 * had it at 0x2550). */
#define DEV_VBLANK_CALLBACK 0x24ECu

#define VBLANK_NS 16683333ull

extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static Uint64 epoch;              /* time of vblank 0 */
static SDL_AtomicInt last_called;  /* last vblank whose callback has run */
static uint32_t vblank_data;       /* guest D3DVBLANKDATA */
static int swap_interval = 1;      /* vblanks per Present; 0 = immediate */


/* Draw calls since the last fps line: made, drawn, and dropped (no vertex
 * data, an unhandled FVF or a program never created). */
/* Only guest code touches these (draws and Present), and the guest CPU lock
 * runs it one thread at a time: plain counters. */
static uint32_t draws_called, draws_drawn, draws_dropped;

static uint32_t take(uint32_t *v)
{
    uint32_t t = *v;
    *v = 0;
    return t;
}

/* Call a guest __cdecl function taking one pointer argument. */
static void call_guest_cdecl1(uint32_t fn_va, uint32_t arg)
{
    recomp_func_t fn = recomp_lookup_manual(fn_va);

    if (!fn)
        fn = recomp_lookup(fn_va);
    if (!fn) {
        SDL_Log("d3d: vblank callback 0x%08x has no recompiled function", fn_va);
        return;
    }
    g_esp -= 4;
    MEM32(g_esp) = arg;
    g_esp -= 4;
    MEM32(g_esp) = 0;   /* return address; the callee pops it */
    fn();
    g_esp += 4;         /* __cdecl: the caller pops the argument */
}

/* D3DDevice_BlockUntilVerticalBlank(): no arguments.
 *
 * On the console this waits for the GPU's vblank interrupt, whose handler
 * also runs the title's vertical-blank callback. JSRF paced everything from
 * it (a thread waited here and woke the main thread); Orta's game loop does
 * the same through Swap's presentation interval; Mad Dash through
 * Present's. So this is the frame clock,
 * and the place a different refresh rate will be decided later. */
/* Sleep until the next vblank on the shared timeline; returns its index.
 * Vblanks fall on one fixed timeline and every waiter wakes at the next one,
 * as on hardware. */
/* MDR_FAST=n: vblanks come n times as often, so a debugging run reaches a
 * point of the game sooner (the game is paced on vblanks; sound and other
 * clocks are not, so timing-sensitive behaviour changes). */
static Uint64 vblank_ns(void)
{
    static Uint64 ns;
    if (!ns) {
        const char *e = getenv("MDR_FAST");
        int n = e ? atoi(e) : 1;
        ns = VBLANK_NS / (Uint64)(n > 1 ? n : 1);
    }
    return ns;
}

/* Where the game thread's time goes, summed over the fps line's period:
 * draws (of which vertex programs and fixed-function vertices), the
 * renderer's frame (of which textures), and waiting for vblank. */
enum { PERF_DRAWS, PERF_VSH, PERF_FVF, PERF_PRESENT, PERF_VBLANK, PERF_LOCK, PERF_COUNT };
static Uint64 perf_ns[PERF_COUNT];

int guest_cpu_yield_begin(void);       /* xboxrecomp, patch 0023 */
void guest_cpu_yield_end(int held);

/* MDR_BENCHMARK=1: as fast as the PC goes. The vblank timeline is moved on
 * to the next vblank instead of slept to, so every frame is still one vblank
 * to the game (its logic steps the same way) but frames come as fast as
 * they are made: the game runs faster than real time and its sound and
 * streaming, timed by other clocks, fall out of step. Loading screens wait
 * in real time (wait_vblank_for). For measuring only. */
static bool benchmark(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MDR_BENCHMARK");
        on = e && atoi(e) != 0;
        if (on)
            SDL_Log("d3d: MDR_BENCHMARK: frames unpaced, the game runs faster than real time");
    }
    return on != 0;
}

static Uint64 skipped_ns;          /* benchmark: vblank time jumped over */

/* Stopped while the options overlay holds the game: vblanks that went on
 * coming had CRI's per-vblank server threads (in JSRF) see the
 * paused mixer's music not advance and drop it. */
static Uint64 vblank_clock(void)
{
    return SDL_GetTicksNS() + skipped_ns - mdr_gfx_paused_ns();
}

/* skip: a frame's own wait (Present), which benchmark mode moves past instead
 * of sleeping. Other waits stay in real time: loading screens wait for
 * vblanks in a loop while other threads load, and must let them run. */
static Uint64 wait_vblank_for(bool skip)
{
    Uint64 now, index, at;

    if (mdr_gfx_paused()) {     /* no vblank comes while the overlay is open */
        int held = guest_cpu_yield_begin();
        while (mdr_gfx_paused())
            SDL_Delay(5);
        guest_cpu_yield_end(held);
    }
    now = vblank_clock();

    if (!epoch)
        epoch = now;
    if (now < epoch)            /* never before vblank 0 (see current_vblank) */
        now = epoch;
    index = (now - epoch) / vblank_ns() + 1;
    at = epoch + index * vblank_ns();
    if (at > now && skip && benchmark()) {
        skipped_ns += at - now;
    } else if (at > now) {
        /* A kernel wait on the console: the other threads run meanwhile
         * (the guest CPU lock, see guest.c). */
        int held = guest_cpu_yield_begin();
        Uint64 back;
        SDL_DelayPrecise(at - now);
        back = SDL_GetTicksNS();
        guest_cpu_yield_end(held);
        /* Taking the guest CPU back: another guest thread (sound,
         * streaming) may still be running when the vblank comes, and the
         * game waits for it -- counted apart, it was in "vblank wait". */
        perf_ns[PERF_LOCK] += SDL_GetTicksNS() - back;
        perf_ns[PERF_VBLANK] += vblank_clock() - now;   /* same clock as now (benchmark) */
    }
    return index;
}

static Uint64 wait_vblank(void)
{
    return wait_vblank_for(false);
}

/* A clock read before epoch would wrap to a vblank so far ahead that Present
 * then waits for it for good: count it as vblank 0. */
static Uint64 current_vblank(void)
{
    Uint64 now = vblank_clock();

    return epoch && now > epoch ? (now - epoch) / vblank_ns() : 0;
}

/* The title's vertical-blank callback (SetVerticalBlankCallback), which the
 * console's vblank interrupt runs every vblank, whether anyone waits or not.
 * Nothing interrupts the recompiled code, so it runs from the waits here:
 * once for each vblank since it last ran, as the interrupt would have (a
 * title that counts vblanks with it, sees them all); more than
 * a second behind, only the latest. Whoever reaches a vblank first runs it.
 * Whether Mad Dash registers one: logged below when it runs. */
static void run_vblank_callbacks(Uint64 index)
{
    for (;;) {
        int prev = SDL_GetAtomicInt(&last_called);
        Uint64 next;
        uint32_t dev, cb;

        if ((Uint64)prev >= index)
            return;
        next = index - (Uint64)prev > 60 ? index : (Uint64)prev + 1;
        if (!SDL_CompareAndSwapAtomicInt(&last_called, prev, (int)next))
            continue;
        dev = MEM32(D3D_DEVICE_PTR);
        cb = dev ? MEM32(dev + DEV_VBLANK_CALLBACK) : 0;
        if (cb) {
            static int said;
            if (!said++)
                SDL_Log("d3d: the title's vblank callback is 0x%08x", cb);
            if (!vblank_data)
                vblank_data = xbox_HeapAlloc(12, 4);
            if (vblank_data) {
                MEM32(vblank_data + 0) = (uint32_t)next;   /* VBlank */
                MEM32(vblank_data + 4) = 0;                /* Swap */
                MEM32(vblank_data + 8) = 0;                /* Flags */
                call_guest_cdecl1(cb, vblank_data);
            }
        }
        if (next <= 3 || next % 600 == 0)
            SDL_Log("d3d: vblank %llu", (unsigned long long)next);
    }
}

/* Wait for the next vblank and run what the interrupt would have. Also for
 * a title's own vblank waits. */
void mdr_d3d_vblank_wait(void)
{
    run_vblank_callbacks(wait_vblank());
    /* Loading screens and movies wait here for seconds without a Present: keep
     * the window answering (Windows marks it "Not Responding" otherwise). */
    if (!mdr_gfx_pump_events()) {
        SDL_Log("d3d: window closed");
        exit(0);
    }
}
/* D3DDevice_BlockUntilVerticalBlank(): no arguments, `ret`. */
void sub_0023B240(void)
{
    mdr_d3d_vblank_wait();
    g_esp += 4;
}

/* Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 *                       pPresentationParameters, ppReturnedDeviceInterface):
 * `ret 0x18`, as in 4928.
 *
 * The XDK's own CreateDevice still runs: it builds the device structure the
 * rest of the library and the game's inline code read. We open our window
 * at the back buffer size the game asked for. */
extern void sub_0023BFB0_gen(void);
void sub_0023BFB0(void)
{
    uint32_t pp = MEM32(g_esp + 20);
    int w = pp ? (int)MEM32(pp + 0) : 640, h = pp ? (int)MEM32(pp + 4) : 480;

    if (w <= 0 || h <= 0)
        w = 640, h = 480;
    /* FullScreen_PresentationInterval: how many vblanks each Present waits. */
    if (pp) {
        uint32_t interval = MEM32(pp + 48);
        swap_interval = interval == 0x80000000u ? 0          /* IMMEDIATE */
                      : interval == 0 ? 1                    /* DEFAULT = ONE */
                      : (int)(interval & 0xF);
    }
    SDL_Log("d3d: CreateDevice, back buffer %dx%d, presentation interval %d", w, h, swap_interval);
    if (pp)     /* D3DPRESENT_PARAMETERS: BackBufferFormat +8, MultiSampleType +16, Flags +40 */
        SDL_Log("d3d: back buffer format 0x%x, multisample 0x%x, flags 0x%x", MEM32(pp + 8), MEM32(pp + 16),
                MEM32(pp + 40));
    if (!mdr_gfx_init(w, h)) {
        /* No picture can come: say so rather than leave a black screen
         * with sound (the log has SDL's reasons, "gfx:" lines). */
        char msg[512];
        snprintf(msg, sizeof msg, "The graphics could not start on this device's GPU, so the game cannot "
                 "show anything (%s).\n\nMad Dash Recomp needs Vulkan 1.1. The log has the details.", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mad Dash Recomp", msg, NULL);
        exit(1);
    }
    sub_0023BFB0_gen();
    SDL_Log("d3d: CreateDevice returned 0x%08x, device 0x%08x", g_eax, MEM32(D3D_DEVICE_PTR));
}

/* D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil): `ret 0x18`. */
void sub_002418B0(void)
{
    uint32_t count = MEM32(g_esp + 4), flags = MEM32(g_esp + 12);
    uint32_t color = MEM32(g_esp + 16), zbits = MEM32(g_esp + 20), stencil = MEM32(g_esp + 24);
    float z;

    memcpy(&z, &zbits, sizeof z);
    if (count) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: Clear with %u rectangles: clearing the whole target for now", count);
    }
    mdr_gfx_clear(flags, color, z, stencil);
    g_eax = 0;
    g_esp += 4 + 24;
}

/* MDR_PEEK=VA:N[,VA:N...] (hex VA, dword count) logs guest memory with the
 * frame-rate line, for bring-up. */
static void peek_guest(void)
{
    const char *e = getenv("MDR_PEEK");
    while (e && *e) {
        char *end;
        uint32_t va = (uint32_t)strtoul(e, &end, 16), n = 1;
        if (*end == ':')
            n = (uint32_t)strtoul(end + 1, &end, 0);
        for (uint32_t i = 0; i < n; i += 8) {
            char line[128];
            int len = SDL_snprintf(line, sizeof line, "peek %08X:", va + i * 4);
            for (uint32_t j = i; j < n && j < i + 8; j++)
                len += SDL_snprintf(line + len, sizeof line - len, " %08X", MEM32(va + j * 4));
            SDL_Log("%s", line);
        }
        e = *end == ',' ? end + 1 : NULL;
    }
}

/* D3DDevice_Present(pSourceRect, pDestRect, hDestWindowOverride,
 * pDirtyRegion): `ret 0x10`. The end of a frame (3925 has no Swap; 4928's
 * Swap(Flags) was Orta's). Like the console, it shows the frame at a
 * vblank, waiting for as many as the presentation interval asks since the
 * previous one. The original's fence, push-buffer kick and buffer rotation
 * are not run: no GPU reads the push buffer, and our renderer keeps its own
 * buffers. */
void sub_002473A0(void)
{
    static Uint64 last_vblank;
    uint64_t n;

    mdr_widescreen_frame();
    if (swap_interval > 0) {
        while (current_vblank() < last_vblank + (Uint64)swap_interval)
            wait_vblank_for(true);
        last_vblank = current_vblank();
        if (!benchmark())
            mdr_gfx_set_frame_time(epoch + last_vblank * vblank_ns() + mdr_gfx_paused_ns());   /* real time */
    }
    /* The callback runs every vblank on the console, waited for or not. */
    run_vblank_callbacks(current_vblank());

    {
        Uint64 t = SDL_GetTicksNS();
        bool open = mdr_gfx_present();
        perf_ns[PERF_PRESENT] += SDL_GetTicksNS() - t;
        if (!open) {
            SDL_Log("d3d: window closed");
            exit(0);
        }
    }
    n = mdr_gfx_frame_count();
    if (n <= 3)
        SDL_Log("d3d: Present, frame %llu", (unsigned long long)n);
    {
        /* Measured frame rate, in vblanks per frame, every 5 seconds. */
        static Uint64 t0, f0, v0;
        Uint64 now = SDL_GetTicksNS();
        if (!t0)
            t0 = now, f0 = n, v0 = current_vblank();
        else if (now - t0 >= 5 * SDL_NS_PER_SECOND) {
            double fps = (double)(n - f0) * SDL_NS_PER_SECOND / (double)(now - t0);
            double k = 1e-6 * SDL_NS_PER_SECOND / (double)(now - t0);   /* ns -> ms per second */
            SDL_Log("d3d: %.2f fps (%.2f vblanks per frame), frame %llu; draws %d, drawn %d, dropped %d", fps,
                    (double)(current_vblank() - v0) / (double)(n - f0 ? n - f0 : 1), (unsigned long long)n,
                    take(&draws_called), take(&draws_drawn), take(&draws_dropped));
            SDL_Log("d3d: ms per second: renderer %.0f, vblank wait %.0f (of it waiting for other game threads %.0f)",
                    perf_ns[PERF_PRESENT] * k, perf_ns[PERF_VBLANK] * k, perf_ns[PERF_LOCK] * k);
            memset(perf_ns, 0, sizeof perf_ns);
            t0 = now, f0 = n, v0 = current_vblank();
            peek_guest();
        }
    }
    g_eax = 0;
    g_esp += 4 + 16;
}

/* D3D8 CDevice::KickOff (thiscall, the device in ecx; `ret`): hand the
 * push buffer written so far to the GPU. Before writing DMA PUT it asks the
 * frame buffer interface to flush (sets 0x10000 in NV2A 0x100410) and spins
 * until the GPU clears the bit -- here, until xboxrecomp's register thread
 * gets round to it. No GPU reads this push buffer, so the flush is done as
 * soon as asked: the same writes, without the wait. The other path (the
 * flag at 0x0024BC80 set) stays the original code. 3925's device: the write
 * cursor at +0 (+0x400 when bit 2 of +0xC is set), the GPU's channel
 * registers at +0x2304 (DMA PUT +0x40), the last kicked-off cursor at +0x18. */
extern void sub_002405D0_gen(void);
void sub_002405D0(void)
{
    uint32_t dev = g_ecx, put;

    if (MEM32(0x0024BC80u)) {
        sub_002405D0_gen();
        return;
    }
    put = (MEM32(dev + 0xC) & 4) ? MEM32(dev + 0x400) : MEM32(dev);
    MEM32(MEM32(dev + 0x2304) + 0x40) = put & 0x03FFFFFFu;     /* DMA PUT */
    MEM32(dev + 0x18) = put;
    g_eax = MEM32(dev + 0x2304);    /* registers as the original leaves them */
    g_ecx = dev;
    g_edx = dev;
    g_esp += 4;
}

/* D3D_BlockOnTime(Time, Flags): `ret 8`. Every GPU wait in the library
 * comes here ("has the GPU got past push-buffer time T yet?"). Our
 * renderer copies what it needs at draw time and presents synchronously,
 * so the GPU is never behind: the answer is always yes. */
void sub_00240720(void)
{
    g_esp += 4 + 8;
}
