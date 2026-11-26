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
#include "guest/vsh.h"
#include "gfx/xtex.h"
#include "guest/guest.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <math.h>
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
#define SPAN_MAX 64             /* a screen-space draw measured at most at this many vertices (mdr_gpu_vsh.span) */

extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static Uint64 epoch;              /* time of vblank 0 */
static SDL_AtomicInt last_called;  /* last vblank whose callback has run */
static uint32_t vblank_data;       /* guest D3DVBLANKDATA */
static int swap_interval = 1;      /* vblanks per Present; 0 = immediate */
static void wide_frame(void);        /* HUD Edges: the frame's long pieces start over (hud_squeeze_of) */
static bool race_now, race_last;     /* the race's progress line drawn this frame, the last (submit_draw) */
static bool backdrop_frame;        /* widescreen: a full-screen 2D backdrop this frame (hud_squeeze_of) */


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
    mdr_gfx_set_race(race_now || race_last);    /* frame interpolation: races only */
    backdrop_frame = false;
    wide_frame();
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
    {
        /* MDR_REBOOT_AT=frame,...: boot the title again in place at those
         * frames (counted from the first, across boots), with no launch
         * data, as a fresh start would: a test of what a level load does. */
        static const char *at;
        static bool read;
        if (!read) {
            read = true;
            at = getenv("MDR_REBOOT_AT");
        }
        for (const char *c = at; c && *c; ) {
            char *end;
            unsigned long long f = strtoull(c, &end, 10);
            if (end == c)
                break;
            if (f == n) {
                SDL_Log("d3d: MDR_REBOOT_AT: rebooting in place at frame %llu", (unsigned long long)n);
                mdr_guest_reboot(NULL, 0);
            }
            c = *end == ',' ? end + 1 : end;
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

/* Tracing hook for the generated entry hooks in d3d_calltrace.c.
 * MDR_D3D_TRACE=first-last logs every Direct3D call made during those frames
 * (MDR_D3D_TRACE=1 means frames 0-2): name, stack arguments, ecx and edx
 * (some XDK functions are __fastcall). */
void d3d_trace_call(uint32_t va, const char *name, int args)
{
    static int parsed;
    static uint64_t first, last;
    uint64_t frame;
    char line[512];
    int len;

    if (!parsed) {
        const char *v = getenv("MDR_D3D_TRACE");
        parsed = 1;
        if (v && *v && *v != '0') {
            parsed = 2;
            first = 0, last = 2;
            if (strchr(v, '-'))
                first = strtoull(v, NULL, 10), last = strtoull(strchr(v, '-') + 1, NULL, 10);
        }
    }
    if (parsed != 2)
        return;
    frame = mdr_gfx_frame_count();
    if (frame < first || frame > last)
        return;
    len = snprintf(line, sizeof line, "d3d %llu: %s(", (unsigned long long)frame, name);
    for (int i = 0; i < args && len < (int)sizeof line - 16; i++)
        len += snprintf(line + len, sizeof line - (size_t)len, "%s0x%x", i ? ", " : "", MEM32(g_esp + 4 + 4 * i));
    snprintf(line + len, sizeof line - (size_t)len, ") ecx=0x%x edx=0x%x [0x%08x] from 0x%08x", g_ecx, g_edx, va,
             MEM32(g_esp));
    SDL_Log("%s", line);
}

/* ── The GPU state the game writes itself ─────────────────────────────────
 * Mad Dash's display-list player (0x001ECBA0) writes the vertex arrays and
 * texture stages as NV2A methods into the push buffer, through the device's
 * cursor, and calls D3D only for the draw: no SetStreamSource or SetTexture
 * reaches us. So at each draw the methods written since the last one (by
 * the game or by D3D) are read for those two groups alone; everything else
 * is still taken at the API. The frame is recorded into a push buffer of
 * its own (BeginPushBuffer each frame, 0x001E7B10/0x001E7BA0), so the
 * stream is read where it is written, never executed. */

static uint32_t pb_mark;          /* guest address read up to */
static void pb_trace_draw(const char *what, uint32_t type, uint32_t count);

static struct {
    uint32_t va_offset[16], va_format[16];  /* NV097_SET_VERTEX_DATA_ARRAY_OFFSET/FORMAT */
    uint32_t tex[4][16];                    /* NV097_SET_TEXTURE_* by stage, 0x1B00 + 64 s + 4 k */
    bool     tex_api[4];                    /* the stage's last setting was SetTexture's */
} nv;

/* The simple render states (57-81) the game writes as NV2A methods itself
 * (the display list's blend and alpha test: 0x0300, 0x0304, 0x033C on),
 * back into D3D's table under the state whose method it is (the library's
 * table 0x002A29A8; a simple state's value is the NV2A's). */
static void nv_render_state(uint32_t m, uint32_t v)
{
    static uint16_t state_of[0x800];    /* method / 4 -> state, 0 none */
    static bool ready;

    if (!ready) {
        for (uint32_t i = 57; i < 82; i++) {
            uint32_t h = MEM32(0x002A29A8u + 4u * i) & 0x1FFCu;
            if (h)
                state_of[h / 4] = (uint16_t)i;
        }
        ready = true;
    }
    if (m < 0x2000 && state_of[m / 4])
        MEM32(0x0024BE88u + 4u * state_of[m / 4]) = v;
}

/* Culling as the NV2A has it (what SetRenderState_CullMode, 0x0023C3E0,
 * writes): CULL_FACE_ENABLE 0x308, CULL_FACE 0x39C (0x404 front, 0x405
 * back), FRONT_FACE 0x3A0 (0x900 CW, 0x901 CCW). Back into D3D's
 * CULLMODE (127: the winding culled, 0 for none) and FRONTFACE (126), for
 * whoever wrote them, the game or the library. */
static uint32_t nv_cull_enable = 1, nv_cull_face = 0x405;

static void nv_cull(uint32_t m, uint32_t v)
{
    uint32_t front;

    if (m == 0x308)
        nv_cull_enable = v;
    else if (m == 0x39C)
        nv_cull_face = v;
    else
        MEM32(0x0024BE88u + 4u * 126) = v;          /* FRONTFACE */
    front = MEM32(0x0024BE88u + 4u * 126);
    if (front != 0x900 && front != 0x901)
        front = 0x900;
    MEM32(0x0024BE88u + 4u * 127) = !nv_cull_enable ? 0
        : nv_cull_face == 0x404 ? front : (front ^ 1);   /* the winding culled */
}

/* Vertex-shader constants and programs as the NV2A takes them, whoever
 * wrote them (SetVertexShaderConstant, LoadVertexShaderProgram, or the
 * game's own methods): SET_TRANSFORM_CONSTANT_LOAD 0x1EA4 picks a slot,
 * SET_TRANSFORM_CONSTANT 0x0B80-0x0BFC streams words into it and the next;
 * SET_TRANSFORM_PROGRAM_LOAD 0x1E9C and SET_TRANSFORM_PROGRAM 0x0B00-0x0B7C
 * likewise for program memory. Constants go to the library's copy that the
 * draws read (VSH_SHADOW), programs to prog_mem. */
static uint32_t nv_const_slot, nv_const_word, nv_prog_slot, nv_prog_word;
/* The window clip (what SetScissors writes, and any display list): its type
 * (SET_WINDOW_CLIP_TYPE 0x2B4: 0 draws inside, 1 outside) and the first
 * window (SET_WINDOW_CLIP_HORIZONTAL 0x2C0, VERTICAL 0x2E0: the first and
 * last pixel, low and high 12 bits). Applied as the draw's scissor
 * (fill_draw_state); the game sets the whole screen most of the time. */
static uint32_t nv_clip_type, nv_clip_h = 0x027F0000u, nv_clip_v = 0x01DF0000u;
static void nv_constant_word(uint32_t v);
static void nv_program_word(uint32_t v);

static void nv_method(uint32_t m, uint32_t v)
{
    if (m == 0x1EA4) {
        nv_const_slot = v;
        nv_const_word = 0;
    } else if (m >= 0x0B80 && m < 0x0C00) {
        nv_constant_word(v);
    } else if (m == 0x1E9C) {
        nv_prog_slot = v;
        nv_prog_word = 0;
    } else if (m >= 0x0B00 && m < 0x0B80) {
        nv_program_word(v);
    } else if (m == 0x308 || m == 0x39C || m == 0x3A0)
        nv_cull(m, v);
    else if (m == 0x2B4)
        nv_clip_type = v;
    else if (m == 0x2C0)
        nv_clip_h = v;
    else if (m == 0x2E0)
        nv_clip_v = v;
    else if (m >= 0x0300 && m < 0x0A00)
        nv_render_state(m, v);
    else if (m >= 0x1720 && m < 0x1760)
        nv.va_offset[(m - 0x1720) / 4] = v;
    else if (m >= 0x1760 && m < 0x17A0)
        nv.va_format[(m - 0x1760) / 4] = v;
    else if (m >= 0x1B00 && m < 0x1C00) {
        nv.tex[(m - 0x1B00) / 64][((m - 0x1B00) % 64) / 4] = v;
        nv.tex_api[(m - 0x1B00) / 64] = false;
    }
}

/* Read the methods from pb_mark to the cursor.
 *
 * In the device's ring (dev+0x10 to dev+0x14) the reader follows the jump
 * MakeSpace (0x00240810) writes when the ring wraps (start | 1, a physical
 * address) back to the start. It stopped there before and took the cursor
 * as read: whatever the game wrote after a wrap until its next draw was
 * lost, bone matrices among it, and those vertices went to the world's
 * origin ("tethers" off the characters). Elsewhere (a buffer being
 * recorded), a jump or call ends the read, as before. */
static void pb_read_to(uint32_t cur)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR), p, lo, hi, walked = 0;
    bool ring;

    if (!dev)
        return;
    p = pb_mark;
    lo = MEM32(dev + 0x10);
    hi = MEM32(dev + 0x14);
    ring = lo < hi && hi - lo <= 0x4000000u && p >= lo && p < hi && cur >= lo && cur <= hi;
    if (!p || (!ring && (cur < p || cur - p > 0x400000u))) {
        pb_mark = cur;
        return;
    }
    while (p != cur && walked <= (ring ? hi - lo : 0x400000u)) {
        uint32_t h = MEM32(p), m, n, end;
        if ((h & 3) == 1 && ring) {         /* the wrap: jump to the start */
            uint32_t t = (h & ~3u) | 0x80000000u;
            static int said;
            if (t < lo || t >= hi)
                break;
            if (said < 2 && ++said)
                SDL_Log("d3d: push buffer wrapped (0x%08x -> 0x%08x), read on", p, t);
            walked += 4;
            p = t;
            continue;
        }
        if ((h & 3) || (h & 0xE0000000u) == 0x20000000u)
            break;                          /* another jump, or a call */
        p += 4;
        m = h & 0x1FFCu;
        n = (h >> 18) & 0x7FFu;
        end = p + 4 * n;
        if ((cur >= p && end > cur) || (ring && end > hi))
            break;                          /* a method not finished yet */
        for (uint32_t k = 0; k < n; k++)
            nv_method((h & 0x40000000u) ? m : m + 4 * k, MEM32(p + 4 * k));
        p = end;
        walked += 4 + 4 * n;
    }
    pb_mark = cur;
}

static void pb_catch_up(void)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR);
    if (dev)
        pb_read_to(MEM32(dev));
}

/* Recording a push buffer moves the cursor away from the ring and back.
 * The reader follows it without a gap: what the game (and the lazy state
 * flush, SetStateVB, both calls make first) wrote to the ring before the
 * switch is read up to the ring cursor Begin saves (dev+0x400), and after
 * End the ring is read on from where it left off. Before, each switch took
 * the new cursor as read, and methods written just before one were lost:
 * a character's bone matrices among them, drawn with the last model's
 * ("tethers"; Orta's were lost another way, its commit d072caa). */
#define DEV_RING_SAVED 0x400u

/* D3DDevice_BeginPushBuffer(pPushBuffer): `ret 4`. The cursor moves to the
 * buffer's data: read from there. */
extern void sub_002466B0_gen(void);
void sub_002466B0(void)
{
    uint32_t dev;

    sub_002466B0_gen();
    dev = MEM32(D3D_DEVICE_PTR);
    if (dev) {
        pb_read_to(MEM32(dev + DEV_RING_SAVED));
        pb_mark = MEM32(dev);
    }
}

/* D3DDevice_EndPushBuffer(): `ret`. The recorded buffer read to its end,
 * then the ring from where it was left. */
extern void sub_00246960_gen(void);
void sub_00246960(void)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR), ring = dev ? MEM32(dev + DEV_RING_SAVED) : 0;

    pb_catch_up();
    sub_00246960_gen();
    if (dev && ring == MEM32(dev))
        pb_mark = ring;
}

/* ── State capture ────────────────────────────────────────────────────────
 * The XDK's own setters still run (wrapped below), so the library's
 * bookkeeping and its state tables stay exactly as on the console; we only
 * note what each draw needs. Render and texture-stage states are read from
 * the library's tables at draw time (D3D__RenderState, D3D__TextureState),
 * which also covers the game's inline writes to them. */

#define RS_BASE 0x0024BE88u   /* D3D_g_RenderState (XbSymbolDatabase) */
#define TS_BASE 0x0024BC88u   /* D3D_g_DeferredTextureState[4][32] (XbSymbolDatabase) */
#define RS(i)    MEM32(RS_BASE + 4u * (i))
#define TS(s, i) MEM32(TS_BASE + 128u * (s) + 4u * (i))

/* D3DRENDERSTATETYPE in 3925, as far as used here (docs/maddash-facts.md):
 * simple states 0-81 (57-81 as in 4134 and 4928), deferred 82-115 (4928's
 * minus 10: FOGENABLE 82 is XbSymbolDatabase's D3DRS_FogEnable, the point
 * states 106-113 are what LazySetPointParams reads), complex from 116, each
 * the slot its setter writes (ZENABLE 123, STENCILENABLE 124, STENCILFAIL
 * 125, CULLMODE 127, TEXTUREFACTOR 128, SHADOWFUNC 135, FOGCOLOR 118). */
enum {
    RS_ZFUNC = 57, RS_ALPHAFUNC = 58, RS_ALPHABLENDENABLE = 59, RS_ALPHATESTENABLE = 60,
    RS_ALPHAREF = 61, RS_SRCBLEND = 62, RS_DESTBLEND = 63, RS_ZWRITEENABLE = 64,
    RS_COLORWRITEENABLE = 67, RS_STENCILZFAIL = 68, RS_STENCILPASS = 69, RS_STENCILFUNC = 70,
    RS_STENCILREF = 71, RS_STENCILMASK = 72, RS_STENCILWRITEMASK = 73, RS_BLENDOP = 74,
    RS_POLYGONOFFSETZSLOPESCALE = 77, RS_POLYGONOFFSETZOFFSET = 78, RS_SOLIDOFFSETENABLE = 81,
    RS_FOGENABLE = 82, RS_FOGTABLEMODE = 83, RS_FOGSTART = 84, RS_FOGEND = 85, RS_FOGDENSITY = 86,
    RS_LIGHTING = 92, RS_SPECULARENABLE = 93, RS_COLORVERTEX = 95,
    RS_SPECULARMATERIALSOURCE = 100, RS_DIFFUSEMATERIALSOURCE = 101,
    RS_AMBIENTMATERIALSOURCE = 102, RS_EMISSIVEMATERIALSOURCE = 103, RS_AMBIENT = 105,
    RS_POINTSIZE = 106, RS_POINTSIZE_MIN = 107, RS_POINTSPRITEENABLE = 108,
    RS_POINTSCALEENABLE = 109, RS_POINTSCALE_A = 110, RS_POINTSIZE_MAX = 113,
    RS_FOGCOLOR = 118,
    RS_ZENABLE = 123, RS_STENCILENABLE = 124, RS_STENCILFAIL = 125, RS_CULLMODE = 127,
    RS_TEXTUREFACTOR = 128,
    RS_SHADOWFUNC = 135,
};

/* D3DDevice_SetRenderState_Simple(method header in ecx, value in edx):
 * writes one NV2A method to the push buffer. The XDK's inline
 * SetRenderState stores the value in the table first, but the game calls it
 * directly too (0x001F0720, 0x001F0E50), so the table we read at draw time
 * would miss those: record the value under the state whose method it is,
 * from the library's own table (0x002A29A8: the full header, for states
 * 0-81). */
#define RS_ENCODE 0x002A29A8u
void sub_0023C250_enter(void)
{
    static uint32_t method[82];
    static bool ready;

    d3d_trace_call(0x0023C250u, "D3DDevice_SetRenderState_Simple", 0);
    if (!ready) {
        for (int i = 0; i < 82; i++)
            method[i] = MEM32(RS_ENCODE + 4u * (uint32_t)i);
        ready = true;
    }
    for (int i = 57; i < 82; i++)
        if (method[i] == g_ecx) {
            RS(i) = g_edx;
            return;
        }
}
/* D3DDevice_SetRenderState_Simple(method in ecx, value in edx): writes one
 * NV2A method to the push buffer. The XDK's inline SetRenderState stores the
 * value in D3D__RenderState before calling it, but Orta's renderer object calls
 * it directly (0x00279C70 sets COLORWRITEENABLE this way, others blending and
 * the depth test), so the table we read at draw time never saw those states:
 * blending stayed off and text boxes came out opaque. Record the value under
 * the state whose method it is, from the XDK's own encode table (states 57-83;
 * the values of simple states are the NV2A ones). */

/* D3DTEXTURESTAGESTATETYPE: as in 4134 (SetTextureState_TexCoordIndex,
 * 0x002AFF70, writes index 28). */
enum {
    TSS_ADDRESSU = 0, TSS_ADDRESSV = 1, TSS_MAGFILTER = 3, TSS_MINFILTER = 4, TSS_MIPFILTER = 5,
    TSS_COLOROP = 12, TSS_COLORARG0 = 13, TSS_COLORARG1 = 14, TSS_COLORARG2 = 15,
    TSS_ALPHAOP = 16, TSS_ALPHAARG0 = 17, TSS_ALPHAARG1 = 18, TSS_ALPHAARG2 = 19,
    TSS_RESULTARG = 20, TSS_TEXTURETRANSFORMFLAGS = 21, TSS_BUMPENVMAT00 = 22,
    TSS_TEXCOORDINDEX = 28,
};
/* D3DTRANSFORMSTATETYPE on the Xbox: VIEW 0, PROJECTION 1, TEXTURE0-3 2-5,
 * WORLD 6, WORLD1-3 7-9. */
enum { TS_VIEW = 0, TS_PROJECTION = 1, TS_TEXTURE0 = 2, TS_WORLD = 6, TS_COUNT = 10 };

static struct {
    uint32_t tex[4];
    uint32_t vb, stride;          /* stream 0 */
    uint32_t vbs[16], strides[16];
    uint32_t ib, base_vertex;
    uint32_t vshader;
    uint32_t pshader;             /* SetPixelShader handle; 0 = fixed function */
    float    mat[TS_COUNT][16];
    float    material[4][4];      /* D3DMATERIAL8: diffuse, ambient, specular, emissive */
    float    power;
    bool     have_viewport;
    uint32_t vp_x, vp_y, vp_w, vp_h;
    float    vp_minz, vp_maxz;
} st;

static void ident(float *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void state_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    for (int i = 0; i < TS_COUNT; i++)
        ident(st.mat[i]);
}

#define WRAP_BEGIN(n) uint32_t a[n]; for (int i_ = 0; i_ < (n); i_++) a[i_] = MEM32(g_esp + 4 + 4 * i_)

/* ── Render targets ───────────────────────────────────────────────────────
 * Mad Dash's title draws to the back buffer CreateDevice set, which is
 * where cur_rt starts (data 0). SetRenderTarget is not hooked yet: no
 * other target is known. D3DSurface: Common, Data +4, Lock +8,
 * Format +0xC, Size +0x10, Parent +0x14. */

#define MAX_BACKBUFFERS 8
static uint32_t backbuffers[MAX_BACKBUFFERS];
static int nbackbuffers;
static mdr_surface cur_rt;                           /* data 0: back buffer */
static uint32_t cur_zformat;                         /* D3DFORMAT of the depth surface */

static void target_size(int *w, int *h)
{
    mdr_gfx_surface_size(&cur_rt, w, h);
}

static mdr_surface surface_of(uint32_t surf)
{
    mdr_surface s = { 0, 0, 0 };

    if (!surf)
        return s;
    for (int i = 0; i < nbackbuffers; i++)
        if (backbuffers[i] == surf || MEM32(backbuffers[i] + 4) == MEM32(surf + 4))
            return s;
    s.data = MEM32(surf + 4);
    s.format_word = MEM32(surf + 0xC);
    s.size_word = MEM32(surf + 0x10);
    return s;
}

/* ── Fixed-function lighting state ─────────────────────────────────────── */

static void unpack_d3dcolor(uint32_t c, float out[4]);
static uint32_t pack_color(const float *c);


/* The lights switched on, as the device keeps them: LightEnable links each
 * one into a list (any index; Orta uses 0-9 and 33-35), and each entry
 * starts with the D3DLIGHT8 SetLight stored. Like the library's upload, at
 * most 8. Returns how many were written to out. (Orta's; see below.) */
static int enabled_lights(uint32_t out[8])
{
    (void)out;
    return 0;       /* 3925's device light list not found yet; programs light themselves */
}

static float lf(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Direct3D fixed-function lighting for one vertex, world-space position p
 * and normal n (unit, or zero without normals): emissive + global ambient
 * x ambient + per light (ambient + diffuse N.L) x attenuation, with
 * material colors taken from the vertex where the material sources say
 * so. Specular highlights are not computed. Returns a D3DCOLOR. */
static uint32_t light_vertex(const float p[3], const float n[3], uint32_t vdiffuse, uint32_t vspecular,
                             bool has_diffuse, bool has_specular, const uint32_t *lights, int nlights)
{
    float mat[4][4], amb[4], out[4];
    bool cv = RS(RS_COLORVERTEX) != 0;
    static const int src_rs[4] = { RS_DIFFUSEMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE,
                                   RS_SPECULARMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE };

    memcpy(mat, st.material, sizeof mat);
    for (int k = 0; k < 4; k++) {
        uint32_t src = RS(src_rs[k]);   /* D3DMCS_MATERIAL 0, COLOR1 1, COLOR2 2 */
        if (cv && src == 1 && has_diffuse)
            unpack_d3dcolor(vdiffuse, mat[k]);
        else if (cv && src == 2 && has_specular)
            unpack_d3dcolor(vspecular, mat[k]);
    }
    unpack_d3dcolor(RS(RS_AMBIENT), amb);
    for (int c = 0; c < 3; c++)
        out[c] = mat[3][c] + amb[c] * mat[1][c];
    for (int i = 0; i < nlights; i++) {
        uint32_t L[26], type;
        float ldir[3], att = 1.0f, ndotl;

        for (int k = 0; k < 26; k++)
            L[k] = MEM32(lights[i] + 4 * k);
        type = L[0];
        /* D3DLIGHT8 in words: Type 0, Diffuse 1-4, Specular 5-8, Ambient 9-12,
         * Position 13-15, Direction 16-18, Range 19, Falloff 20,
         * Attenuation0-2 21-23, Theta 24, Phi 25. */
        if (type == 3) {                          /* directional */
            /* The library normalizes it on upload (0x002B8055). */
            float len = 0;
            for (int c = 0; c < 3; c++) {
                ldir[c] = -lf(L[16 + c]);
                len += ldir[c] * ldir[c];
            }
            len = sqrtf(len);
            if (len > 0)
                for (int c = 0; c < 3; c++)
                    ldir[c] /= len;
        } else {                                  /* point, spot */
            float d2 = 0, d, range = lf(L[19]);
            for (int c = 0; c < 3; c++) {
                ldir[c] = lf(L[13 + c]) - p[c];
                d2 += ldir[c] * ldir[c];
            }
            d = sqrtf(d2);
            if (range > 0 && d > range)
                continue;
            if (d > 0)
                for (int c = 0; c < 3; c++)
                    ldir[c] /= d;
            att = lf(L[21]) + lf(L[22]) * d + lf(L[23]) * d2;
            att = att > 0 ? 1.0f / att : 1.0f;
            if (type == 2) {                      /* spot: inner and outer cone */
                float dl = sqrtf(lf(L[16]) * lf(L[16]) + lf(L[17]) * lf(L[17]) + lf(L[18]) * lf(L[18]));
                float rho = 0, ct = cosf(lf(L[24]) * 0.5f), cp = cosf(lf(L[25]) * 0.5f);
                for (int c = 0; c < 3 && dl > 0; c++)
                    rho -= ldir[c] * lf(L[16 + c]) / dl;
                if (rho <= cp)
                    att = 0;
                else if (rho < ct && ct > cp)
                    att *= powf((rho - cp) / (ct - cp), lf(L[20]));
            }
        }
        ndotl = n[0] * ldir[0] + n[1] * ldir[1] + n[2] * ldir[2];
        if (ndotl < 0)
            ndotl = 0;
        for (int c = 0; c < 3; c++)
            out[c] += att * (lf(L[9 + c]) * mat[1][c] + ndotl * lf(L[1 + c]) * mat[0][c]);
    }
    out[3] = mat[0][3];
    return pack_color(out);
}


/* ── Vertex shader programs ───────────────────────────────────────────────
 * Constants: SetVertexShaderConstant(Register, pConstantData, Count) puts
 * register r at NV2A slot r + 96 and, unless the device is pure, keeps a
 * copy in the device at +0xCC0 + 16 * slot (0x00249B50 for Mad Dash's
 * static device). Our hook writes the copy too (shadow_constants), for a
 * pure device. It is read at each draw. */

#define VSH_SHADOW 0x00249B50u

static float vconst[MDR_VSH_CONSTANTS][4];

/* A pure device (Orta's was) skips the library's copy: every constant
 * setter also writes it here; a pure device never reads it back. */
static void shadow_constants(uint32_t slot, uint32_t src, uint32_t dwords)
{
    if (slot >= MDR_VSH_CONSTANTS || !src)
        return;
    if (dwords > (MDR_VSH_CONSTANTS - slot) * 4)
        dwords = (MDR_VSH_CONSTANTS - slot) * 4;
    for (uint32_t i = 0; i < dwords; i++)
        MEM32(VSH_SHADOW + 16 * slot + 4 * i) = MEM32(src + 4 * i);
}


/* Programs: LoadVertexShaderProgram(pFunction, Address) copies a token
 * stream (header 0x2078 | count << 16, then four words an instruction)
 * into the NV2A's 136-slot program memory at Address; SelectVertexShader
 * (Handle, Address) starts the next draws at Address, running to the first
 * instruction with the end flag (word 3, bit 0). Each program so found is
 * parsed once (src/guest/vsh.c) and kept by its start and its words. */
#define PROG_SLOTS 136
static uint32_t prog_mem[PROG_SLOTS][4];
static uint32_t prog_start;
static bool prog_selected;

/* Programs found, kept by their words alone: Mad Dash loads the same ones
 * at other places in program memory as a race goes on (a 12-instruction
 * one at 66, 67 and 118), and keyed by place as well they filled the table
 * with copies; full, every new one took slot 0, so programs were parsed and
 * their shaders made again and again. Full now, the oldest goes. */
#define MAX_PROGRAMS 1024
static struct { uint32_t count, hash; mdr_vsh_program *prog; } programs[MAX_PROGRAMS];
static int programs_next;

static void nv_constant_word(uint32_t v)
{
    if (nv_const_slot < MDR_VSH_CONSTANTS)
        MEM32(VSH_SHADOW + 16 * nv_const_slot + 4 * nv_const_word) = v;
    if (++nv_const_word == 4) {
        nv_const_word = 0;
        nv_const_slot++;
    }
}

static void nv_program_word(uint32_t v)
{
    if (nv_prog_slot < PROG_SLOTS)
        prog_mem[nv_prog_slot][nv_prog_word] = v;
    if (++nv_prog_word == 4) {
        nv_prog_word = 0;
        nv_prog_slot++;
    }
}

static mdr_vsh_program *current_program(void)
{
    uint32_t func[1 + 4 * PROG_SLOTS], n = 0, hash = 2166136261u;
    int slot = -1;
    mdr_vsh_program *p;

    if (!prog_selected || prog_start >= PROG_SLOTS)
        return NULL;
    while (prog_start + n < PROG_SLOTS) {
        for (int k = 0; k < 4; k++) {
            func[1 + 4 * n + (uint32_t)k] = prog_mem[prog_start + n][k];
            hash = (hash ^ prog_mem[prog_start + n][k]) * 16777619u;
        }
        n++;
        if (prog_mem[prog_start + n - 1][3] & 1)
            break;
    }
    for (int i = 0; i < MAX_PROGRAMS; i++)
        if (programs[i].prog && programs[i].hash == hash && programs[i].count == n)
            return programs[i].prog;
    func[0] = 0x2078u | (n << 16);
    p = SDL_malloc(sizeof *p);
    if (!p || !mdr_vsh_parse(p, func, 1 + 4 * n, NULL, 0)) {
        SDL_Log("d3d: vertex program at %u (%u instructions) not handled", prog_start, n);
        SDL_free(p);
        return NULL;
    }
    slot = programs_next;
    programs_next = (programs_next + 1) % MAX_PROGRAMS;
    if (programs[slot].prog && !programs_next)
        SDL_Log("d3d: more than %d vertex programs: the oldest are replaced", MAX_PROGRAMS);
    programs[slot].count = n;
    programs[slot].hash = hash;
    programs[slot].prog = p;
    SDL_Log("d3d: vertex program at %u, %u instructions", prog_start, n);
    return p;
}

/* The program's inputs are the NV2A's vertex arrays: input r is array r,
 * its own stream, at offset 0, of the array's type (an Xbox D3DVSDT is the
 * NV2A's size << 4 | type, the format word's low byte). */
static void program_inputs(mdr_vsh_program *p)
{
    for (int r = 0; r < MDR_VSH_INPUTS && r < 16; r++) {
        uint32_t f = nv.va_format[r];
        p->input[r].present = ((f >> 4) & 0xF) != 0 && nv.va_offset[r] != 0;
        p->input[r].stream = (uint8_t)r;
        p->input[r].offset = 0;
        p->input[r].type = (uint8_t)(f & 0xFF);
    }
}

/* D3DDevice_SetPixelShaderConstant(Register, pConstantData, ConstantCount):
 * float4 constants 0-15, routed to a stage's C0/C1 by the definition's
 * PSC0Mapping/PSC1Mapping (a nibble per stage, 0xF = the definition's own
 * PSConstant value). */
static float psconst[16][4];
static bool psconst_set[16];


/* D3DDevice_SetPixelShader(Handle): `ret 4`. 0 = fixed-function stages,
 * otherwise the definition at handle + 8 drives the register combiners in
 * ff.frag (SetPixelShaderProgram, 0x00247530, makes such a handle in the
 * device at +0x464 for a bare definition, and calls this).
 * Each new one is logged with its definition. */
extern void sub_00247570_gen(void);
void sub_00247570(void)
{
    WRAP_BEGIN(1);
    sub_00247570_gen();
    if (a[0] != st.pshader && a[0]) {
        static uint32_t seen[64];
        static int nseen;
        int i;
        for (i = 0; i < nseen && seen[i] != a[0]; i++)
            ;
        if (i == nseen && nseen < 64) {
            uint32_t def = MEM32(a[0] + 8);
            seen[nseen++] = a[0];
            SDL_Log("d3d: pixel shader 0x%08x set (%d distinct so far), definition:", a[0], nseen);
            for (int r = 0; def && r < 60; r += 10)
                SDL_Log("  ps[%2d] %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", r,
                        MEM32(def + 4 * r), MEM32(def + 4 * r + 4), MEM32(def + 4 * r + 8),
                        MEM32(def + 4 * r + 12), MEM32(def + 4 * r + 16), MEM32(def + 4 * r + 20),
                        MEM32(def + 4 * r + 24), MEM32(def + 4 * r + 28), MEM32(def + 4 * r + 32),
                        MEM32(def + 4 * r + 36));
        }
    }
    st.pshader = a[0];
    /* The XDK's SetPixelShader loads every constant from the definition
     * into the combiners; values set earlier with SetPixelShaderConstant
     * do not survive it. Kept, a constant meant for another shader replaced
     * the graffiti shader's channel selectors and hid layers of the tag. */
    memset(psconst_set, 0, sizeof psconst_set);
}

/* ── Draws ────────────────────────────────────────────────────────────── */

static void mat_mul(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}

static float f32(uint32_t va)
{
    uint32_t bits = MEM32(va);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void unpack_d3dcolor(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(c & 0xFF) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}

static void fill_draw_state(mdr_draw *d, bool pretransformed)
{
    int rtw, rth;

    memset(d, 0, sizeof *d);
    target_size(&rtw, &rth);
    {   /* who drew it (captures): the D3D call's return, then up the ebp chain */
        uint32_t bp = g_ebp;
        d->caller[0] = MEM32(g_esp);
        for (int k = 1; k < 3; k++) {
            if (bp < 0x00010000u || bp >= 0x04000000u || (bp & 3))
                break;
            d->caller[k] = MEM32(bp + 4);
            bp = MEM32(bp);
        }
    }
    d->vertex_info[0] = st.vshader;
    d->vertex_info[1] = RS(RS_LIGHTING);
    {
        uint32_t lights[8];
        d->vertex_info[2] = (RS(RS_COLORVERTEX) & 0xFF) | (uint32_t)enabled_lights(lights) << 8;
    }
    d->vertex_info[3] = (RS(RS_DIFFUSEMATERIALSOURCE) & 0xF) << 12 | (RS(RS_AMBIENTMATERIALSOURCE) & 0xF) << 8 |
                        (RS(RS_SPECULARMATERIALSOURCE) & 0xF) << 4 | (RS(RS_EMISSIVEMATERIALSOURCE) & 0xF);
    d->vertex_info[4] = RS(RS_AMBIENT);
    d->vertex_info[5] = pack_color(st.material[0]);
    d->vertex_info[6] = pack_color(st.material[1]);
    d->vertex_info[7] = pack_color(st.material[3]);
    d->ps.blend_enable = RS(RS_ALPHABLENDENABLE) != 0;
    d->ps.src_blend = (uint16_t)RS(RS_SRCBLEND);
    d->ps.dst_blend = (uint16_t)RS(RS_DESTBLEND);
    d->ps.blend_op = (uint16_t)RS(RS_BLENDOP);
    d->ps.z_enable = RS(RS_ZENABLE) != 0;
    d->ps.z_write = RS(RS_ZWRITEENABLE) != 0;
    d->ps.z_func = (uint16_t)RS(RS_ZFUNC);
    d->ps.color_write = RS(RS_COLORWRITEENABLE);
    /* D3DCULL_NONE 0, D3DCULL_CW 0x900, D3DCULL_CCW 0x901 (GL-valued). */
    d->ps.cull = RS(RS_CULLMODE) == 0x900 ? 1 : RS(RS_CULLMODE) == 0x901 ? 2 : 0;
    /* Stencil (STENCILENABLE 144 and STENCILFAIL 145: their setters write
     * there; the rest sit with the simple states). */
    if (RS(RS_STENCILENABLE)) {
        d->ps.stencil_enable = 1;
        d->ps.stencil_func = (uint16_t)RS(RS_STENCILFUNC);
        d->ps.stencil_fail = (uint16_t)RS(RS_STENCILFAIL);
        d->ps.stencil_zfail = (uint16_t)RS(RS_STENCILZFAIL);
        d->ps.stencil_pass = (uint16_t)RS(RS_STENCILPASS);
        d->ps.stencil_mask = (uint8_t)RS(RS_STENCILMASK);
        d->ps.stencil_write_mask = (uint8_t)RS(RS_STENCILWRITEMASK);
        d->stencil_ref = (uint8_t)RS(RS_STENCILREF);
    }
    /* Polygon offset, as D3DRS_ZBIAS leaves it: offset -ZBias depth units,
     * slope scale -ZBias times a constant. Our depth is 24-bit; a 16-bit
     * depth surface's unit (Orta's shadow map, LIN_D16) is 256 of ours, and
     * left unscaled the shadow pass's offset was too small to keep a lit
     * surface from shadowing itself. */
    if (RS(RS_SOLIDOFFSETENABLE)) {
        uint32_t o = RS(RS_POLYGONOFFSETZOFFSET), sl = RS(RS_POLYGONOFFSETZSLOPESCALE);
        memcpy(&d->ps.depth_bias, &o, 4);
        memcpy(&d->ps.depth_slope, &sl, 4);
        if (cur_zformat == 0x2C || cur_zformat == 0x2D || cur_zformat == 0x30 || cur_zformat == 0x31)
            d->ps.depth_bias *= 256.0f;
    }

    for (int s = 0; s < 4; s++) {
        /* The stage as the game wrote it to the NV2A (pb_catch_up): OFFSET,
         * FORMAT (the D3D resource's format word), ADDRESS, CONTROL0 (bit
         * 30: enabled), CONTROL1 (pitch << 16), FILTER, IMAGE_RECT. */
        const uint32_t *t = nv.tex[s];
        uint32_t api = nv.tex_api[s] ? st.tex[s] : 0;      /* a resource from SetTexture */
        bool on = nv.tex_api[s] ? api != 0 : t[0] != 0 && t[1] != 0;
        d->ff.color_op[s][0] = (int32_t)TS(s, TSS_COLOROP);
        d->ff.color_op[s][1] = (int32_t)TS(s, TSS_COLORARG1);
        d->ff.color_op[s][2] = (int32_t)TS(s, TSS_COLORARG2);
        d->ff.color_op[s][3] = (int32_t)TS(s, TSS_COLORARG0);
        d->ff.alpha_op[s][0] = (int32_t)TS(s, TSS_ALPHAOP);
        d->ff.alpha_op[s][1] = (int32_t)TS(s, TSS_ALPHAARG1);
        d->ff.alpha_op[s][2] = (int32_t)TS(s, TSS_ALPHAARG2);
        d->ff.alpha_op[s][3] = (int32_t)TS(s, TSS_ALPHAARG0);
        d->ff.stage_misc[s][0] = (int32_t)TS(s, TSS_RESULTARG);
        d->ff.stage_misc[s][1] = on;
        d->ff.stage_misc[s][2] = s;
        d->ff.tc_scale[s][0] = d->ff.tc_scale[s][1] = 1.0f;
        for (int i = 0; i < 4; i++) {
            uint32_t m = TS(s, TSS_BUMPENVMAT00 + i);
            memcpy(&d->ff.bump_mat[s][i], &m, 4);
        }
        for (int i = 0; i < 2; i++) {
            uint32_t m = TS(s, TSS_BUMPENVMAT00 + 4 + i);
            memcpy(&d->ff.bump_lum[s][i], &m, 4);
        }
        if (on) {
            mdr_xtex_info ti;
            uint32_t size_word = 0, rect = t[7], pitch = t[4] >> 16;
            static const uint8_t addr_mode[8] = { 1, 1, 2, 3, 4, 3, 1, 1 };     /* NV2A -> D3DTADDRESS */
            if (rect && pitch && mdr_xtex_is_linear(t[1]))  /* D3D's Size word, from the rect and pitch */
                size_word = ((rect >> 16) - 1) | ((rect & 0xFFFF) - 1) << 12 | (pitch / 64 - 1) << 24;
            if (api)
                size_word = MEM32(api + 16);    /* D3DResource: Common, Data, Lock, Format, Size */
            mdr_xtex_describe(api ? MEM32(api + 12) : t[1], size_word, &ti);
            if (ti.pitch && ti.width && ti.height) {    /* linear: texel coordinates */
                d->ff.tc_scale[s][0] = 1.0f / (float)ti.width;
                d->ff.tc_scale[s][1] = 1.0f / (float)ti.height;
            }
            switch (ti.format) {
            case 0x03: case 0x05: case 0x07: case 0x11: case 0x1C: case 0x1E:
                d->ff.tc_scale[s][2] = 1.0f;
                break;
            case 0x28:      /* V8U8 */
                d->ff.tc_scale[s][3] = 1.0f;
                break;
            }
            d->tex[s].data = api ? MEM32(api + 4) : t[0] & 0x03FFFFFFu;
            d->tex[s].format_word = api ? MEM32(api + 12) : t[1];
            d->tex[s].size_word = size_word;
            d->tex[s].address_u = api ? (uint8_t)TS(s, TSS_ADDRESSU) : addr_mode[t[2] & 7];
            d->tex[s].address_v = api ? (uint8_t)TS(s, TSS_ADDRESSV) : addr_mode[(t[2] >> 8) & 7];
            /* FILTER: min bits 16-23 (1 nearest, 2 linear, 3-6 with mips),
             * mag bits 24-27; as D3DTEXF (1 point, 2 linear). */
            d->tex[s].mag_filter = ((t[5] >> 24) & 0xF) == 1 ? 1 : 2;
            if (api) {
                d->tex[s].mag_filter = (uint8_t)TS(s, TSS_MAGFILTER);
                d->tex[s].min_filter = (uint8_t)TS(s, TSS_MINFILTER);
                d->tex[s].mip_filter = (uint8_t)TS(s, TSS_MIPFILTER);
            } else {
                uint32_t mn = (t[5] >> 16) & 0xFF;
                d->tex[s].min_filter = mn == 1 || mn == 3 || mn == 5 ? 1 : 2;
                d->tex[s].mip_filter = mn <= 2 ? 0 : mn <= 4 ? 1 : 2;
            }
        }
    }
    {
        uint32_t tf = RS(RS_TEXTUREFACTOR);
        d->ff.tfactor[0] = (float)((tf >> 16) & 0xFF) / 255.0f;
        d->ff.tfactor[1] = (float)((tf >> 8) & 0xFF) / 255.0f;
        d->ff.tfactor[2] = (float)(tf & 0xFF) / 255.0f;
        d->ff.tfactor[3] = (float)(tf >> 24) / 255.0f;
    }
    if (st.pshader) {
        /* D3DPIXELSHADERDEF, at handle + 8 (4134 and 4928 alike), in words:
         * 0 PSAlphaInputs[8], 8 FinalCombinerInputsABCD, 9 ...EFG,
         * 10 PSConstant0[8], 18 PSConstant1[8], 26 PSAlphaOutputs[8],
         * 34 PSRGBInputs[8], 42 CompareMode, 43/44 FinalCombinerConstant0/1,
         * 45 PSRGBOutputs[8], 53 PSCombinerCount, 54 PSTextureModes,
         * 57/58 PSC0Mapping/PSC1Mapping, 59 PSFinalCombinerConstants. */
        uint32_t def = MEM32(st.pshader + 8);
        if (def) {
            for (int i = 0; i < 8; i++) {
                d->ff.ps_a_in[i] = (int32_t)MEM32(def + 4 * (0 + i));
                d->ff.ps_a_out[i] = (int32_t)MEM32(def + 4 * (26 + i));
                d->ff.ps_rgb_in[i] = (int32_t)MEM32(def + 4 * (34 + i));
                d->ff.ps_rgb_out[i] = (int32_t)MEM32(def + 4 * (45 + i));
                unpack_d3dcolor(MEM32(def + 4 * (10 + i)), d->ff.ps_c0[i]);
                unpack_d3dcolor(MEM32(def + 4 * (18 + i)), d->ff.ps_c1[i]);
            }
            {   /* Constants routed from SetPixelShaderConstant. */
                uint32_t m0 = MEM32(def + 4 * 57), m1 = MEM32(def + 4 * 58);
                for (int i = 0; i < 8; i++) {
                    uint32_t k0 = (m0 >> (4 * i)) & 0xF, k1 = (m1 >> (4 * i)) & 0xF;
                    if (k0 < 0xF && psconst_set[k0])
                        memcpy(d->ff.ps_c0[i], psconst[k0], sizeof psconst[k0]);
                    if (k1 < 0xF && psconst_set[k1])
                        memcpy(d->ff.ps_c1[i], psconst[k1], sizeof psconst[k1]);
                }
            }
            d->ff.ps_final[0] = (int32_t)MEM32(def + 4 * 8);
            d->ff.ps_final[1] = (int32_t)MEM32(def + 4 * 9);
            d->ff.ps_final[2] = (int32_t)MEM32(def + 4 * 53);
            d->ff.ps_final[3] = (int32_t)MEM32(def + 4 * 54);
            unpack_d3dcolor(MEM32(def + 4 * 43), d->ff.ps_fc0);
            unpack_d3dcolor(MEM32(def + 4 * 44), d->ff.ps_fc1);
            {   /* 59 PSFinalCombinerConstants: which SetPixelShaderConstant
                 * register feeds the final combiner's C0 (bits 0-3) and C1
                 * (bits 4-7); 0xF for none. */
                uint32_t fm = MEM32(def + 4 * 59), k0 = fm & 0xF, k1 = (fm >> 4) & 0xF;
                if (k0 < 0xF && psconst_set[k0])
                    memcpy(d->ff.ps_fc0, psconst[k0], sizeof psconst[k0]);
                if (k1 < 0xF && psconst_set[k1])
                    memcpy(d->ff.ps_fc1, psconst[k1], sizeof psconst[k1]);
            }
        }
    }
    /* Fog (FOGCOLOR is state 138 in 4928: its setter writes there). */
    if (RS(RS_FOGENABLE)) {
        uint32_t v;
        d->ff.fog_mode[0] = 1;
        d->ff.fog_mode[1] = pretransformed ? 0 : (int32_t)RS(RS_FOGTABLEMODE);
        unpack_d3dcolor(RS(RS_FOGCOLOR), d->ff.fog_color);
        v = RS(RS_FOGSTART);   memcpy(&d->ff.fog_params[0], &v, 4);
        v = RS(RS_FOGEND);     memcpy(&d->ff.fog_params[1], &v, 4);
        v = RS(RS_FOGDENSITY); memcpy(&d->ff.fog_params[2], &v, 4);
    }
    d->ff.alpha_test[0] = RS(RS_ALPHATESTENABLE) != 0;
    d->ff.alpha_test[1] = (int32_t)RS(RS_ALPHAFUNC);
    d->ff.alpha_test[2] = (int32_t)(RS(RS_ALPHAREF) & 0xFF);
    d->ff.alpha_test[3] = RS(RS_SPECULARENABLE) != 0;

    {   /* The window clip: none when it takes in the whole target, or
         * draws outside the window (no game seen doing that). */
        int x0 = (int)(nv_clip_h & 0xFFF), x1 = (int)((nv_clip_h >> 16) & 0xFFF) + 1;
        int y0 = (int)(nv_clip_v & 0xFFF), y1 = (int)((nv_clip_v >> 16) & 0xFFF) + 1;
        d->scissor[0] = d->scissor[1] = d->scissor[2] = d->scissor[3] = 0;
        if (!nv_clip_type && x1 > x0 && y1 > y0 && (x0 > 0 || y0 > 0 || x1 < rtw || y1 < rth)) {
            static int said;
            d->scissor[0] = (int16_t)x0;
            d->scissor[1] = (int16_t)y0;
            d->scissor[2] = (int16_t)x1;
            d->scissor[3] = (int16_t)y1;
            if (said < 3 && ++said)
                SDL_Log("d3d: a scissor at %d,%d-%d,%d (frame %llu)", x0, y0, x1, y1,
                        (unsigned long long)mdr_gfx_frame_count());
        }
    }
    if (pretransformed || !st.have_viewport) {
        d->viewport[2] = (float)rtw;
        d->viewport[3] = (float)rth;
        d->viewport[5] = 1.0f;
    } else {
        d->viewport[0] = (float)st.vp_x;
        d->viewport[1] = (float)st.vp_y;
        d->viewport[2] = (float)st.vp_w;
        d->viewport[3] = (float)st.vp_h;
        d->viewport[4] = st.vp_minz;
        d->viewport[5] = st.vp_maxz;
    }
}

/* D3DTSS_TEXTURETRANSFORMFLAGS: stage s's coordinates times its texture
 * matrix (D3DTS_TEXTURE0 + s), as a row vector. Low byte: how many output
 * components the rasterizer uses (0 disables); 256 (PROJECTED): divide by
 * the last of them. The input is padded the way D3D pads it: the first
 * missing component is 1, so a 2D coordinate (u, v, 1, 0) picks up the
 * matrix's third row as a translation -- how textures are scrolled. */
static void texture_transform(int s, uint32_t n, float *tc)
{
    uint32_t flags = TS(s, TSS_TEXTURETRANSFORMFLAGS), count = flags & 0xFF;
    const float *m = st.mat[TS_TEXTURE0 + s];
    float in[4] = { 0, 0, 0, 0 }, out[4];

    if (!count)
        return;
    for (uint32_t c = 0; c < n && c < 4; c++)
        in[c] = tc[c];
    if (n < 4)
        in[n] = 1.0f;
    for (int c = 0; c < 4; c++)
        out[c] = in[0] * m[c] + in[1] * m[4 + c] + in[2] * m[8 + c] + in[3] * m[12 + c];
    if ((flags & 256) && count >= 2 && count <= 4 && out[count - 1] != 0.0f)
        for (uint32_t c = 0; c + 1 < count; c++)
            out[c] /= out[count - 1];
    for (int c = 0; c < 4; c++)
        tc[c] = out[c];
    /* q for the rasterizer's divide: only a fourth output component that
     * was not divided here. */
    if (count != 4 || (flags & 256))
        tc[3] = 0.0f;
}

/* D3D point size in pixels: the vertex's own (FVF PSIZE) or POINTSIZE; with
 * POINTSCALEENABLE, scaled by distance d from the eye as
 * Vh * size * sqrt(1 / (A + B d + C d^2)); then clamped to MIN..MAX. */
static float point_size(float size, float d, bool scale)
{
    float mn = lf(RS(RS_POINTSIZE_MIN)), mx = lf(RS(RS_POINTSIZE_MAX));

    if (scale) {
        float a = lf(RS(RS_POINTSCALE_A)), b = lf(RS(RS_POINTSCALE_A + 1)), c = lf(RS(RS_POINTSCALE_A + 2));
        float q = a + b * d + c * d * d;
        size = q > 0.0f ? (float)st.vp_h * size * sqrtf(1.0f / q) : 0.0f;
    }
    if (size < mn) size = mn;
    if (mx > 0.0f && size > mx) size = mx;
    return size;
}

/* Where an FVF puts each element in a vertex. */
typedef struct fvf_layout {
    bool     pre, has_diffuse, has_specular, has_normal;
    uint32_t normal_off, psize_off, diffuse_off, specular_off, size;
    uint32_t ntc, tc_off[4], tc_n[4];
} fvf_layout;

static bool layout_fvf(uint32_t fvf, fvf_layout *l)
{
    uint32_t pos = fvf & 0x00E, off;

    memset(l, 0, sizeof *l);
    l->pre = pos == 0x004;
    l->has_diffuse = (fvf & 0x40) != 0;
    l->has_specular = (fvf & 0x80) != 0;
    l->has_normal = (fvf & 0x10) != 0;
    l->ntc = (fvf >> 8) & 0xF;
    if (pos == 0x002) off = 12;
    else if (pos == 0x004) off = 16;
    else if (pos >= 0x006 && pos <= 0x00C) off = 12 + 4 * ((pos - 0x004) / 2);
    else return false;
    if (fvf & 0x010) { l->normal_off = off; off += 12; }   /* normal */
    if (fvf & 0x020) { l->psize_off = off; off += 4; }     /* point size */
    if (l->has_diffuse) { l->diffuse_off = off; off += 4; }
    if (l->has_specular) { l->specular_off = off; off += 4; }
    for (uint32_t i = 0; i < l->ntc && i < 4; i++) {
        static const uint32_t sizes[4] = { 2, 3, 4, 1 };
        l->tc_n[i] = sizes[(fvf >> (16 + 2 * i)) & 3];
        l->tc_off[i] = off;
        off += 4 * l->tc_n[i];
    }
    l->size = off;
    return true;
}

/* D3DTSS_TEXCOORDINDEX: the coordinate set in the low word; the high word
 * asks for generated coordinates instead (D3DTSS_TCI_*): 1 camera-space
 * normal, 2 camera-space position; others are not handled (0). */
static void stage_coords(bool pre, uint32_t set_of[4], uint32_t gen[4])
{
    for (int s = 0; s < 4; s++) {
        set_of[s] = TS(s, TSS_TEXCOORDINDEX) & 0xFFFF;
        gen[s] = pre ? 0 : TS(s, TSS_TEXCOORDINDEX) >> 16;
        if (gen[s] > 2) {
            static uint32_t warned;
            if (!(warned & (1u << (gen[s] & 31)))) {
                warned |= 1u << (gen[s] & 31);
                SDL_Log("d3d: texture coordinate generation 0x%x not handled", gen[s] << 16);
            }
            gen[s] = 0;
        }
    }
}

/* The normal the hardware holds. Each vertex attribute's register keeps
 * the value of the last vertex a draw streamed (the last index, for an
 * indexed draw; xemu does the same), and a draw whose format has no normal
 * is lit with that one (a JSRF map panel, lit FVF 0x102
 * drawn after FVF 0x112 pins, was black with a zero normal). Set by
 * latch_normal after every draw and by SetVertexData*(D3DVSDE_NORMAL). */
static float latched_normal[3];

/* Fetch vertices [first, first + count) of an FVF stream into our layout. */
static bool fetch_fvf(uint32_t fvf, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                      mdr_vertex *out, bool *pretransformed)
{
    fvf_layout l;
    uint32_t ntc, *tc_off = l.tc_off, *tc_n = l.tc_n;
    uint32_t diffuse_off, specular_off, normal_off, psize_off, set_of[4], gen[4];
    bool has_diffuse, has_specular, has_normal;
    bool lit, scale;
    uint32_t lights[8];
    int nlights = 0;
    float wvp[16], wv[16];
    int rtw, rth;

    if (!layout_fvf(fvf, &l))
        return false;
    *pretransformed = l.pre;
    ntc = l.ntc;
    diffuse_off = l.diffuse_off; specular_off = l.specular_off;
    normal_off = l.normal_off; psize_off = l.psize_off;
    has_diffuse = l.has_diffuse; has_specular = l.has_specular; has_normal = l.has_normal;
    if (!stride)
        stride = l.size;
    stage_coords(l.pre, set_of, gen);

    target_size(&rtw, &rth);
    lit = !*pretransformed && RS(RS_LIGHTING);
    if (lit)
        nlights = enabled_lights(lights);
    scale = !*pretransformed && RS(RS_POINTSCALEENABLE);
    mat_mul(st.mat[TS_WORLD], st.mat[TS_VIEW], wv);
    mat_mul(wv, st.mat[TS_PROJECTION], wvp);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = base + (first + i) * stride;
        mdr_vertex *o = &out[i];
        float x = f32(v), y = f32(v + 4), z = f32(v + 8);

        memset(o, 0, sizeof *o);
        if (*pretransformed) {
            float rhw = f32(v + 12), w = rhw != 0.0f ? 1.0f / rhw : 1.0f;
            o->pos[0] = (x / (float)rtw * 2.0f - 1.0f) * w;
            o->pos[1] = (1.0f - y / (float)rth * 2.0f) * w;
            o->pos[2] = z * w;
            o->pos[3] = w;
        } else {
            for (int c = 0; c < 4; c++)
                o->pos[c] = x * wvp[c] + y * wvp[4 + c] + z * wvp[8 + c] + wvp[12 + c];
        }
        o->diffuse = has_diffuse ? MEM32(v + diffuse_off) : 0xFFFFFFFFu;
        o->specular = has_specular ? MEM32(v + specular_off) : 0;
        {
            float e[3] = { 0, 0, 0 };
            if (scale)
                for (int c = 0; c < 3; c++)
                    e[c] = x * wv[c] + y * wv[4 + c] + z * wv[8 + c] + wv[12 + c];
            o->psize = point_size(fvf & 0x020 ? f32(v + psize_off) : lf(RS(RS_POINTSIZE)),
                                  sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), scale);
        }
        if (lit) {
            const float *w = st.mat[TS_WORLD];
            float wp[3], wn[3] = { 0, 0, 0 };
            for (int c = 0; c < 3; c++)
                wp[c] = x * w[c] + y * w[4 + c] + z * w[8 + c] + w[12 + c];
            {   /* without a normal in the stream: the latched one */
                float nx = latched_normal[0], ny = latched_normal[1], nz = latched_normal[2], len;
                if (has_normal)
                    nx = f32(v + normal_off), ny = f32(v + normal_off + 4), nz = f32(v + normal_off + 8);
                for (int c = 0; c < 3; c++)
                    wn[c] = nx * w[c] + ny * w[4 + c] + nz * w[8 + c];
                len = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                if (len > 0)
                    for (int c = 0; c < 3; c++)
                        wn[c] /= len;
            }
            o->diffuse = light_vertex(wp, wn, o->diffuse, o->specular, has_diffuse, has_specular, lights, nlights);
            o->specular = 0;
        }
        /* Fog coordinate: eye depth for table fog, else the specular
         * alpha as the factor (no specular: no fog). */
        if (!*pretransformed && RS(RS_FOGTABLEMODE))
            o->fog = x * wv[2] + y * wv[6] + z * wv[10] + wv[14];
        else
            o->fog = has_specular ? (float)(o->specular >> 24) / 255.0f : 1.0f;
        for (int s = 0; s < 4; s++) {
            uint32_t set = set_of[s];
            if (gen[s]) {
                /* TCI_CAMERASPACENORMAL (1): the unit normal in camera
                 * space; TCI_CAMERASPACEPOSITION (2): the position. Three
                 * components, then the stage's texture transform. */
                float g[3] = { 0, 0, 0 }, len;
                if (gen[s] == 2) {
                    for (int c = 0; c < 3; c++)
                        g[c] = x * wv[c] + y * wv[4 + c] + z * wv[8 + c] + wv[12 + c];
                } else if (has_normal) {
                    float nx = f32(v + normal_off), ny = f32(v + normal_off + 4), nz = f32(v + normal_off + 8);
                    for (int c = 0; c < 3; c++)
                        g[c] = nx * wv[c] + ny * wv[4 + c] + nz * wv[8 + c];
                    len = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    if (len > 0)
                        for (int c = 0; c < 3; c++)
                            g[c] /= len;
                }
                memcpy(o->tc[s], g, sizeof g);
                texture_transform(s, 3, o->tc[s]);
                continue;
            }
            if (set < ntc && set < 4)
                for (uint32_t c = 0; c < tc_n[set]; c++)
                    o->tc[s][c] = f32(v + tc_off[set] + 4 * c);
            if (!*pretransformed && set < ntc && set < 4)
                texture_transform(s, tc_n[set], o->tc[s]);
        }
    }
    return true;
}

/* The same draw for shaders/fvf.vert: the layout, transforms, texture
 * stages, material and lights as uniforms, and the game's vertex bytes
 * from the draw's first vertex on. False for an FVF fetch_fvf rejects. */
static bool fvf_gpu_job(uint32_t fvf, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                        mdr_gpu_fvf *g)
{
    static const int src_rs[4] = { RS_DIFFUSEMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE,
                                   RS_SPECULARMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE };
    mdr_fvf_uniforms *u = &g->u;
    fvf_layout l;
    uint32_t set_of[4], gen[4], lights[8];
    int rtw, rth, flags = 0, nlights = 0;

    if (!layout_fvf(fvf, &l))
        return false;
    if (!stride)
        stride = l.size;
    /* Lights and material only for a lit draw (filled below); the shader
     * reads them only then, and they are most of the block. */
    memset(g, 0, offsetof(mdr_gpu_fvf, u) + offsetof(mdr_fvf_uniforms, material));
    g->data = (const uint8_t *)XBOX_PTR(base + first * stride);
    g->bytes = count * stride + 64;     /* as for vertex programs */

    if (l.pre) flags |= MDR_FVF_PRE;
    if (l.has_diffuse) flags |= MDR_FVF_DIFFUSE;
    if (l.has_specular) flags |= MDR_FVF_SPECULAR;
    if (l.has_normal) flags |= MDR_FVF_NORMAL;
    if (!l.pre && RS(RS_LIGHTING)) flags |= MDR_FVF_LIT;
    if (!l.pre && RS(RS_FOGTABLEMODE)) flags |= MDR_FVF_FOGTABLE;
    u->layout0[1] = (int32_t)stride;
    u->layout0[2] = flags;
    u->layout0[3] = (int32_t)l.ntc;
    u->offs[0] = (int32_t)l.normal_off;
    u->offs[1] = (int32_t)l.diffuse_off;
    u->offs[2] = (int32_t)l.specular_off;
    stage_coords(l.pre, set_of, gen);
    for (int s = 0; s < 4; s++) {
        u->tc_off[s] = (int32_t)l.tc_off[s];
        u->tc_n[s] = (int32_t)l.tc_n[s];
        u->stage_set[s] = (int32_t)set_of[s];
        u->stage_gen[s] = (int32_t)gen[s];
        u->stage_tt[s] = (int32_t)TS(s, TSS_TEXTURETRANSFORMFLAGS);
        memcpy(u->tex[s], st.mat[TS_TEXTURE0 + s], sizeof u->tex[s]);
    }
    target_size(&rtw, &rth);
    u->rt[0] = (float)rtw;
    u->rt[1] = (float)rth;
    mat_mul(st.mat[TS_WORLD], st.mat[TS_VIEW], u->wv);
    mat_mul(u->wv, st.mat[TS_PROJECTION], u->wvp);
    /* Reversed depth (renderer.c): w - z as its own column, in double from
     * the projection's columns, so the shader's 1 - z does not come from
     * subtracting two nearly equal floats. */
    for (int i = 0; i < 4; i++) {
        double acc = 0.0;
        for (int k = 0; k < 4; k++)
            acc += (double)u->wv[i * 4 + k] *
                   ((double)st.mat[TS_PROJECTION][k * 4 + 3] - (double)st.mat[TS_PROJECTION][k * 4 + 2]);
        u->rz[i] = (float)acc;
    }
    memcpy(u->world, st.mat[TS_WORLD], sizeof u->world);
    if (!(flags & MDR_FVF_LIT))
        return true;

    memset(u->material, 0, sizeof *u - offsetof(mdr_fvf_uniforms, material));
    memcpy(u->material, st.material, sizeof u->material);
    memcpy(u->normal, latched_normal, sizeof latched_normal);
    for (int k = 0; k < 4; k++)
        u->mat_src[k] = (int32_t)RS(src_rs[k]);
    unpack_d3dcolor(RS(RS_AMBIENT), u->ambient);
    nlights = enabled_lights(lights);
    u->light_info[0] = nlights;
    u->light_info[1] = RS(RS_COLORVERTEX) != 0;
    for (int i = 0; i < nlights; i++) {
        float (*o)[4] = u->light[i];
        uint32_t L[26];
        float dl;

        /* D3DLIGHT8 in words, as light_vertex reads them */
        for (int k = 0; k < 26; k++)
            L[k] = MEM32(lights[i] + 4 * k);
        dl = sqrtf(lf(L[16]) * lf(L[16]) + lf(L[17]) * lf(L[17]) + lf(L[18]) * lf(L[18]));
        o[0][0] = (float)L[0];
        o[0][1] = lf(L[19]);
        o[0][2] = lf(L[20]);
        for (int c = 0; c < 3; c++) {
            o[1][c] = lf(L[1 + c]);
            o[2][c] = lf(L[9 + c]);
            o[3][c] = lf(L[13 + c]);
            o[4][c] = lf(L[16 + c]);
        }
        if (L[0] == 3)                  /* directional: unit, towards the light */
            for (int c = 0; c < 3; c++)
                o[4][c] = dl > 0 ? -lf(L[16 + c]) / dl : -lf(L[16 + c]);
        o[5][0] = lf(L[21]);
        o[5][1] = lf(L[22]);
        o[5][2] = lf(L[23]);
        o[5][3] = dl;
        o[6][0] = cosf(lf(L[24]) * 0.5f);
        o[6][1] = cosf(lf(L[25]) * 0.5f);
    }
    return true;
}

static uint32_t pack_color(const float *c)
{
    uint32_t r[4];
    for (int i = 0; i < 4; i++) {
        float v = c[i] != c[i] ? 0.0f : c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        r[i] = (uint32_t)(v * 255.0f + 0.5f);
    }
    return (r[3] << 24) | (r[0] << 16) | (r[1] << 8) | r[2];
}

/* Array r's CPU address (its offset is physical: the contiguous alias at
 * 0x80000000), or 0. Begin/End's staged arrays (in the guest heap, not in
 * that alias) are used as they are, while nv_staged is set. */
static bool nv_staged;

static uint32_t nv_array_base(int r)
{
    if (!((nv.va_format[r] >> 4) & 0xF) || !nv.va_offset[r])
        return 0;
    return nv_staged ? nv.va_offset[r] : (nv.va_offset[r] & 0x03FFFFFFu) | 0x80000000u;
}

/* Run the current vertex program over vertices [first, first + count).
 * base/stride describe stream 0 (a user-pointer draw passes its own); the
 * other streams come from SetStreamSource.
 *
 * The XDK ends every program with its viewport transform: oPos.xyz =
 * clip.xyz / w * c[58] + c[59] (D3D's c[-38]/c[-37]), which the hardware
 * mirrors from the viewport. We supply those two constants and undo the
 * transform to get the clip-space position the renderer takes. */
typedef struct vsh_job {
    const mdr_vsh_program *p;
    uint32_t handle;
    uint32_t sbase[16], sstride[16], first;
    float scale[3], offset[3], zs;
    mdr_vertex *out;
} vsh_job;

/* One vertex's inputs, as its declaration describes them. */
static void vsh_inputs(const vsh_job *j, uint32_t i, float in[MDR_VSH_INPUTS][4])
{
    const mdr_vsh_program *p = j->p;

    for (int r = 0; r < MDR_VSH_INPUTS; r++) {
        const mdr_vsh_input *in_r = &p->input[r];
        if (!(p->inputs_read & (1u << r)))
            continue;
        if (in_r->present && j->sbase[in_r->stream]) {
            uint32_t va = j->sbase[in_r->stream] + (j->first + i) * j->sstride[in_r->stream] + in_r->offset;
            mdr_vsh_fetch(in_r->type, (const uint8_t *)XBOX_PTR(va), in[r]);
        } else {
            in[r][0] = in[r][1] = in[r][2] = 0.0f;
            in[r][3] = 1.0f;
        }
    }
}

/* One vertex's outputs into the renderer's vertex. */
static void vsh_unpack(const vsh_job *j, uint32_t i, float in[MDR_VSH_INPUTS][4], float o[VSH_OUTPUTS][4])
{
    const mdr_vsh_program *p = j->p;
    mdr_vertex *v = &j->out[i];
    float cw = o[VSH_OPOS][3];

    {   /* MDR_VSH_BAD=n: for the first n vertices that land far outside
         * the screen, log the inputs and re-run with an instruction trace. */
        static int budget = -1;
        static SDL_AtomicInt left;
        float x = (o[VSH_OPOS][0] - j->offset[0]) / j->scale[0];
        if (budget < 0) {
            budget = getenv("MDR_VSH_BAD") ? atoi(getenv("MDR_VSH_BAD")) : 0;
            SDL_SetAtomicInt(&left, budget);
        }
        if (budget > 0 && (fabsf(x) > 50.0f || !(cw == cw)) && SDL_AddAtomicInt(&left, -1) > 0) {
            static SDL_Mutex *mx;
            float o2[VSH_OUTPUTS][4];
            if (!mx) mx = SDL_CreateMutex();
            SDL_LockMutex(mx);
            SDL_Log("vsh bad: program 0x%08x vertex %u (%u insns) oPos %g %g %g %g frame %llu", j->handle,
                    j->first + i, p->count, o[VSH_OPOS][0], o[VSH_OPOS][1], o[VSH_OPOS][2], o[VSH_OPOS][3],
                    (unsigned long long)mdr_gfx_frame_count());
            for (int r = 0; r < MDR_VSH_INPUTS; r++)
                if (p->inputs_read & (1u << r))
                    SDL_Log("vsh bad:   v%d type 0x%02x stream %u +%u = %g %g %g %g", r, p->input[r].type,
                            p->input[r].stream, p->input[r].offset, in[r][0], in[r][1], in[r][2], in[r][3]);
            mdr_vsh_trace = true;
            mdr_vsh_run(p, (const float (*)[4])in, vconst, o2);
            mdr_vsh_trace = false;
            SDL_UnlockMutex(mx);
        }
    }
    if (i == 0 && mdr_gfx_capturing()) {
        /* A capture: for a program that reads constants through a0 (the
         * characters' skinning), the constant rows that are all zero and the
         * first vertex's inputs and instruction trace. */
        bool rel = false;
        for (uint32_t k = 0; k < p->count; k++)
            rel |= p->op[k].rel != 0;
        if (rel) {
            static SDL_Mutex *mx;
            char zero[512];
            int len = 0;
            float o2[VSH_OUTPUTS][4];
            if (!mx) mx = SDL_CreateMutex();
            SDL_LockMutex(mx);
            for (int c = 0; c < MDR_VSH_CONSTANTS && len < (int)sizeof zero - 16; ) {
                int e = c;
                while (e < MDR_VSH_CONSTANTS && !vconst[e][0] && !vconst[e][1] && !vconst[e][2] && !vconst[e][3])
                    e++;
                if (e > c)
                    len += SDL_snprintf(zero + len, sizeof zero - (size_t)len, " %d-%d", c, e - 1);
                c = e + 1;
            }
            zero[len] = 0;
            SDL_Log("capture:     a0 program 0x%08x (%u insns), %u vertices; zero constants:%s", j->handle,
                    p->count, 0u, len ? zero : " none");
            for (int r = 0; r < MDR_VSH_INPUTS; r++)
                if (p->inputs_read & (1u << r))
                    SDL_Log("capture:     a0 v%d type 0x%02x stream %u +%u = %g %g %g %g", r, p->input[r].type,
                            p->input[r].stream, p->input[r].offset, in[r][0], in[r][1], in[r][2], in[r][3]);
            mdr_vsh_trace = true;
            mdr_vsh_run(p, (const float (*)[4])in, vconst, o2);
            mdr_vsh_trace = false;
            SDL_UnlockMutex(mx);
        }
    }
    v->pos[0] = (o[VSH_OPOS][0] - j->offset[0]) / j->scale[0] * cw;
    v->pos[1] = (o[VSH_OPOS][1] - j->offset[1]) / j->scale[1] * cw;
    v->pos[2] = (o[VSH_OPOS][2] - j->offset[2]) / j->zs * cw;
    v->pos[3] = cw;
    v->diffuse = pack_color(o[VSH_OD0]);
    v->specular = pack_color(o[VSH_OD1]);
    v->fog = o[VSH_OFOG][0];
    v->psize = point_size(o[VSH_OPTS][0], 0.0f, false);
    for (int t = 0; t < 4; t++)
        memcpy(v->tc[t], o[VSH_OT0 + t], sizeof v->tc[t]);
}

/* Vertices [lo, hi) of a job: fetch the inputs, run the program, unpack.
 * Four at a time (mdr_vsh_run4), the last group padded with its last
 * vertex; a program that writes constants one by one, in order. */
static void vsh_range(void *ctx, uint32_t lo, uint32_t hi)
{
    const vsh_job *j = ctx;
    const mdr_vsh_program *p = j->p;
    float in[4][MDR_VSH_INPUTS][4], o[4][VSH_OUTPUTS][4];

    for (int l = 0; l < 4; l++)
        for (int r = 0; r < MDR_VSH_INPUTS; r++) {
            in[l][r][0] = in[l][r][1] = in[l][r][2] = 0.0f;
            in[l][r][3] = 1.0f;
        }
    if (p->writes_constants) {
        for (uint32_t i = lo; i < hi; i++) {
            vsh_inputs(j, i, in[0]);
            mdr_vsh_run(p, (const float (*)[4])in[0], vconst, o[0]);
            vsh_unpack(j, i, in[0], o[0]);
        }
        return;
    }
    for (uint32_t i = lo; i < hi; i += 4) {
        uint32_t n = hi - i < 4 ? hi - i : 4;
        for (uint32_t l = 0; l < 4; l++)
            vsh_inputs(j, i + (l < n ? l : n - 1), in[l]);
        mdr_vsh_run4(p, (const float (*)[MDR_VSH_INPUTS][4])in, vconst, o);
        for (uint32_t l = 0; l < n; l++)
            vsh_unpack(j, i + l, in[l], o[l]);
    }
}

/* Run the current vertex program over vertices [first, first + count).
 * base/stride describe stream 0 (a user-pointer draw passes its own); the
 * other streams come from SetStreamSource. Vertices are independent, so a
 * large draw is spread over worker threads, unless the program writes the
 * constant file (that persists from vertex to vertex, so order matters).
 *
 * The XDK ends every program with its viewport transform: oPos.xyz =
 * clip.xyz / w * c[58] + c[59] (D3D's c[-38]/c[-37]), which the hardware
 * mirrors from the viewport. We supply those two constants and undo the
 * transform to get the clip-space position the renderer takes. */
/* The constant file a program runs with: the game's (the library's shadow
 * of it), and in c[58] and c[59] the viewport transform the XDK ends every
 * program with, as the hardware mirrors it from the viewport. Returns that
 * transform's scale and offset, and the depth scale to undo it with. */
/* Whether a program reads c[58] or c[59] directly: the viewport transform
 * the XDK's compiled programs end with. Mad Dash's skinning program reads
 * its bone matrices through a0 across the whole file, c[57-59] the
 * twentieth of them; supplied there, the viewport transform flung that
 * bone's vertices millions of units off (the tethers to the right of the
 * characters). */
static bool reads_viewport(const mdr_vsh_program *p)
{
    for (uint32_t k = 0; p && k < p->count; k++) {
        const mdr_vsh_op *op = &p->op[k];
        if (op->rel || (op->cidx != 58 && op->cidx != 59))
            continue;
        for (int i = 0; i < 3; i++)
            if ((op->use & (1u << i)) && op->src[i].mux == 3)
                return true;
    }
    return false;
}

static void vsh_constants(const mdr_vsh_program *p, float scale[3], float offset[3], float *zs_out)
{
    float x = 0, y = 0, w = 640, h = 480, zs = 16777215.0f, zo = 0;
    int rtw, rth;

    memcpy(vconst, (const void *)XBOX_PTR(VSH_SHADOW), sizeof vconst);
    target_size(&rtw, &rth);
    if (st.have_viewport) {
        x = (float)st.vp_x; y = (float)st.vp_y; w = (float)st.vp_w; h = (float)st.vp_h;
        zs = 16777215.0f * (st.vp_maxz - st.vp_minz);
        zo = 16777215.0f * st.vp_minz;
    } else {
        w = (float)rtw; h = (float)rth;
    }
    scale[0] = w * 0.5f; scale[1] = -h * 0.5f; scale[2] = zs;
    offset[0] = x + w * 0.5f + 0.53125f; offset[1] = y + h * 0.5f + 0.53125f; offset[2] = zo;
    if (reads_viewport(p))
        for (int k = 0; k < 3; k++) {
            vconst[58][k] = scale[k];
            vconst[59][k] = offset[k];
        }
    *zs_out = zs != 0.0f ? zs : 1.0f;
}

/* The same draw for shaders/vsh.vert: the constants, the viewport
 * transform, and the streams the program reads as the game's bytes from
 * the draw's first vertex on. */
/* Text the game placed through the camera (camera_placed): the point it
 * was placed by, from its rows -- vertices at one height, x lowest and
 * highest. Rows of different widths that start together are left-aligned
 * text, that end together right-aligned, else centred; one row (or rows
 * all as wide), its middle. In x as given (pixels or clip). */
typedef struct text_rows { float y[16], lo[16], hi[16]; int n; } text_rows;

static void rows_add(text_rows *t, float x, float y)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (SDL_fabsf(t->y[i] - y) < 0.5f)
            break;
    if (i == t->n) {
        if (t->n == 16)
            return;
        t->y[i] = y;
        t->lo[i] = t->hi[i] = x;
        t->n++;
    }
    t->lo[i] = SDL_min(t->lo[i], x);
    t->hi[i] = SDL_max(t->hi[i], x);
}

static float rows_anchor(const text_rows *t, float tol)
{
    bool left = true, right = true, wide = false;
    float lo = 3.4e38f, hi = -3.4e38f;
    for (int i = 0; i < t->n; i++) {
        lo = SDL_min(lo, t->lo[i]);
        hi = SDL_max(hi, t->hi[i]);
        left = left && SDL_fabsf(t->lo[i] - t->lo[0]) <= tol;
        right = right && SDL_fabsf(t->hi[i] - t->hi[0]) <= tol;
        wide = wide || SDL_fabsf((t->hi[i] - t->lo[i]) - (t->hi[0] - t->lo[0])) > 2.0f * tol;
    }
    if (wide && left)
        return lo;
    if (wide && right)
        return hi;
    return (lo + hi) * 0.5f;
}

static bool want_text_anchor;   /* submit_draw: the next vsh_gpu_job finds it (text_anchor) */
static float text_anchor;       /* pixels */

static void vsh_gpu_job(const mdr_vsh_program *p, uint32_t base, uint32_t stride, uint32_t first,
                        uint32_t count, mdr_gpu_vsh *g)
{
    uint32_t sbase[16], sstride[16];
    float zs;

    memset(g, 0, sizeof *g);
    vsh_constants(p, g->vp_scale, g->vp_offset, &zs);
    g->vp_offset[3] = zs;
    g->constants = (const float (*)[4])vconst;
    g->consts_read = p->consts_read;
    g->program = p->gpu_program;
    g->count = p->count;
    for (int s = 0; s < 16; s++) {
        sbase[s] = nv_array_base(s);
        sstride[s] = nv.va_format[s] >> 8;
    }
    (void)base;
    (void)stride;
    for (int r = 0; r < MDR_VSH_INPUTS; r++) {
        const mdr_vsh_input *in = &p->input[r];
        uint32_t s = in->stream & 15;
        g->input[r][0] = -1;
        if (!(p->inputs_read & (1u << r)) || !in->present || !sbase[s])
            continue;
        if (!g->stream_bytes[s]) {
            g->stream_data[s] = (const uint8_t *)XBOX_PTR(sbase[s] + first * sstride[s]);
            /* the last vertex's attributes may run past its stride */
            g->stream_bytes[s] = count * sstride[s] + 64;
        }
        g->input[r][0] = (int32_t)s;
        g->input[r][1] = (int32_t)sstride[s];
        g->input[r][2] = in->offset;
        g->input[r][3] = in->type;
    }
    /* Where the draw is: its first and last vertex through the program on
     * the CPU (a GPU program writes no constants, so the file is safe).
     * Frame interpolation pairs alike draws by these: episode 4's alike
     * battleships read all 192 registers through a0, and by constants a
     * ship looked no nearer its own last frame than another ship. */
    if (count) {
        vsh_job j;
        float in[MDR_VSH_INPUTS][4], out[VSH_OUTPUTS][4];
        memset(&j, 0, sizeof j);
        j.p = p;
        j.first = first;
        memcpy(j.sbase, sbase, sizeof sbase);
        memcpy(j.sstride, sstride, sizeof sstride);
        for (int k = 0; k < 2; k++) {
            vsh_inputs(&j, k ? count - 1 : 0, in);
            mdr_vsh_run(p, (const float (*)[4])in, vconst, out);
            memcpy(g->anchor[k], out[VSH_OPOS], sizeof g->anchor[k]);
        }
        g->anchored = true;
        /* A screen-space draw's extent over all its vertices (widescreen,
         * hud_squeeze_of): by the first and last alone, a quad from its
         * top left to its bottom left looked no wider than a line, and
         * the full-screen quad that darkens Mad Dash's stencil shadows
         * was squeezed to the middle -- the shadows cut off straight. */
        g->spanned = false;
        if (SDL_fabsf(g->anchor[0][3] - 1.0f) < 1e-4f && SDL_fabsf(g->anchor[1][3] - 1.0f) < 1e-4f) {
            /* Every vertex up to SPAN_MAX, else that many spread over the
             * draw (a line of text: four vertices a letter). */
            uint32_t step = count <= SPAN_MAX ? 1 : (count + SPAN_MAX - 1) / SPAN_MAX;
            g->span[0] = SDL_min(g->anchor[0][0], g->anchor[1][0]);
            g->span[1] = SDL_max(g->anchor[0][0], g->anchor[1][0]);
            g->span[2] = SDL_min(g->anchor[0][1], g->anchor[1][1]);
            g->span[3] = SDL_max(g->anchor[0][1], g->anchor[1][1]);
            for (uint32_t i = step; i + 1 < count; i += step) {
                vsh_inputs(&j, i, in);
                mdr_vsh_run(p, (const float (*)[4])in, vconst, out);
                g->span[0] = SDL_min(g->span[0], out[VSH_OPOS][0]);
                g->span[1] = SDL_max(g->span[1], out[VSH_OPOS][0]);
                g->span[2] = SDL_min(g->span[2], out[VSH_OPOS][1]);
                g->span[3] = SDL_max(g->span[3], out[VSH_OPOS][1]);
            }
            g->spanned = true;
            if (want_text_anchor) {     /* every vertex: its rows */
                text_rows t;
                t.n = 0;
                for (uint32_t i = 0; i < count && i < 4096; i++) {
                    vsh_inputs(&j, i, in);
                    mdr_vsh_run(p, (const float (*)[4])in, vconst, out);
                    rows_add(&t, out[VSH_OPOS][0], out[VSH_OPOS][1]);
                }
                text_anchor = t.n ? rows_anchor(&t, 1.5f) : (g->span[0] + g->span[1]) * 0.5f;
            }
        }
    }
}

/* Vertex programs go to the GPU when the setting asks for it, and the
 * program can (it does not write constants) and has been handed over. */
static bool use_gpu_vsh(mdr_vsh_program *p)
{
    if (!mdr_settings_get()->gpu_vertex || mdr_gfx_vertex_shader_failed(false) || p->writes_constants)
        return false;
    if (p->gpu_program < 0) {
        static uint32_t code[8 * MDR_VSH_MAX_INSNS];
        if (!mdr_vsh_gpu_code(p, code))
            return false;
        p->gpu_program = mdr_gfx_vsh_program(code, p->count);
        if (p->gpu_program < 0) {
            p->gpu_program = -2;        /* the renderer cannot */
            return false;
        }
    }
    return p->gpu_program >= 0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* A capture: the two vertices of a draw of a program that reads constants
 * through a0 (skinning) that land furthest from the draw's median position,
 * their inputs and an instruction trace (the tethers). */
static void capture_outliers(const vsh_job *j, uint32_t count)
{
    const mdr_vsh_program *p = j->p;
    bool rel = false;
    float *c, med[3], worst[2] = { -1.0f, -1.0f };
    uint32_t at[2] = { 0, 0 };

    for (uint32_t k = 0; k < p->count; k++)
        rel |= p->op[k].rel != 0;
    if (!rel || !(c = SDL_malloc(count * sizeof *c)))
        return;
    for (int a = 0; a < 3; a++) {
        for (uint32_t i = 0; i < count; i++)
            c[i] = j->out[i].pos[a];
        qsort(c, count, sizeof *c, cmp_float);
        med[a] = c[count / 2];
    }
    SDL_free(c);
    for (uint32_t i = 0; i < count; i++) {
        float d = 0.0f;
        for (int a = 0; a < 3; a++)
            d += fabsf(j->out[i].pos[a] - med[a]);
        if (!(d == d))
            d = 1e30f;
        if (d > worst[0]) {
            worst[1] = worst[0]; at[1] = at[0];
            worst[0] = d; at[0] = i;
        } else if (d > worst[1]) {
            worst[1] = d; at[1] = i;
        }
    }
    SDL_Log("capture:     a0 outliers of %u vertices (median %g %g %g): %u at %g, %u at %g", count, med[0], med[1],
            med[2], at[0], worst[0], at[1], worst[1]);
    for (int k = 0; k < 2; k++) {
        float in[MDR_VSH_INPUTS][4], o[VSH_OUTPUTS][4];
        vsh_inputs(j, at[k], in);
        SDL_Log("capture:     a0 outlier %u pos %g %g %g %g", at[k], j->out[at[k]].pos[0], j->out[at[k]].pos[1],
                j->out[at[k]].pos[2], j->out[at[k]].pos[3]);
        for (int r = 0; r < MDR_VSH_INPUTS; r++)
            if (p->inputs_read & (1u << r))
                SDL_Log("capture:     a0 v%d = %g %g %g %g", r, in[r][0], in[r][1], in[r][2], in[r][3]);
        mdr_vsh_trace = true;
        mdr_vsh_run(p, (const float (*)[4])in, vconst, o);
        mdr_vsh_trace = false;
    }
}

static bool fetch_vsh(const mdr_vsh_program *p, uint32_t base, uint32_t stride,
                      uint32_t first, uint32_t count, mdr_vertex *out)
{
    vsh_job j;
    uint64_t t0;

    vsh_constants(p, j.scale, j.offset, &j.zs);
    j.p = p;
    j.handle = prog_start;
    j.first = first;
    j.out = out;
    for (int s = 0; s < 16; s++) {
        j.sbase[s] = nv_array_base(s);
        j.sstride[s] = nv.va_format[s] >> 8;
    }
    (void)base;
    (void)stride;

    t0 = SDL_GetPerformanceCounter();
    if (p->writes_constants)
        vsh_range(&j, 0, count);
    else
        mdr_vsh_parallel(count, vsh_range, &j);
    if (mdr_gfx_capturing() && count > 8)
        capture_outliers(&j, count);

    /* Once a second: how many vertices the programs ran on, and the time. */
    {
        static uint64_t ticks, verts, since;
        uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        ticks += now - t0;
        verts += count;
        if (!since)
            since = now;
        if (now - since >= f) {
            SDL_Log("vsh: %llu vertices/s, %.1f ms/s in vertex programs",
                    (unsigned long long)(verts * f / (now - since)),
                    1000.0 * (double)ticks / (double)(now - since));
            ticks = verts = 0;
            since = now;
        }
    }
    return true;
}

/* Turn a D3DPRIMITIVETYPE run of n vertices into list indices. Returns the
 * topology (0 triangles, 1 lines, 2 points, 3 triangle strip) and the index
 * count. Strips go to the GPU as strips: the same triangles, with the same
 * alternating winding, from a third of the indices. */
static uint32_t build_indices(uint32_t type, const uint32_t *in, uint32_t n, uint32_t *out, uint8_t *topology)
{
    uint32_t k = 0;
    *topology = 0;
    switch (type) {
    case 1: *topology = 2; for (uint32_t i = 0; i < n; i++) out[k++] = in[i]; break;
    case 2: *topology = 1; for (uint32_t i = 0; i + 1 < n; i += 2) { out[k++] = in[i]; out[k++] = in[i + 1]; } break;
    case 3: case 4:
        *topology = 1;
        for (uint32_t i = 0; i + 1 < n; i++) { out[k++] = in[i]; out[k++] = in[i + 1]; }
        if (type == 3 && n > 2) { out[k++] = in[n - 1]; out[k++] = in[0]; }
        break;
    case 5: for (uint32_t i = 0; i + 2 < n; i += 3) { out[k++] = in[i]; out[k++] = in[i + 1]; out[k++] = in[i + 2]; } break;
    case 6:
        *topology = 3;
        if (n >= 3) {
            memcpy(out, in, n * sizeof *out);
            k = n;
        }
        break;
    case 7: case 10:
        for (uint32_t i = 1; i + 1 < n; i++) { out[k++] = in[0]; out[k++] = in[i]; out[k++] = in[i + 1]; }
        break;
    case 8:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            out[k++] = in[i]; out[k++] = in[i + 1]; out[k++] = in[i + 2];
            out[k++] = in[i]; out[k++] = in[i + 2]; out[k++] = in[i + 3];
        }
        break;
    case 9:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            out[k++] = in[i];     out[k++] = in[i + 1]; out[k++] = in[i + 2];
            out[k++] = in[i + 1]; out[k++] = in[i + 3]; out[k++] = in[i + 2];
        }
        break;
    default:
        break;
    }
    return k;
}

/* Draw a point list as quads of each vertex's psize pixels, square on the
 * screen, the way the hardware rasterizes points. With POINTSPRITEENABLE the
 * texture coordinates run from (0, 0) at the top left to (1, 1). */
static void draw_points(mdr_draw *d, const mdr_vertex *v, const uint32_t *order, uint32_t n)
{
    static mdr_vertex *qv;
    static uint32_t *qi, cap;
    bool sprite = RS(RS_POINTSPRITEENABLE) != 0;
    float vw = d->viewport[2] > 0.0f ? d->viewport[2] : 1.0f, vh = d->viewport[3] > 0.0f ? d->viewport[3] : 1.0f;

    if (n > cap) {
        cap = n * 2;
        qv = SDL_realloc(qv, cap * 4 * sizeof *qv);
        qi = SDL_realloc(qi, cap * 6 * sizeof *qi);
    }
    for (uint32_t i = 0; i < n; i++) {
        const mdr_vertex *p = &v[order[i]];
        /* Half the size in clip units: 2 / viewport pixels per pixel. */
        float hx = p->psize / vw * p->pos[3], hy = p->psize / vh * p->pos[3];
        for (int c = 0; c < 4; c++) {
            mdr_vertex *o = &qv[i * 4 + c];
            float sx = (c & 1) ? 1.0f : 0.0f, sy = (c & 2) ? 1.0f : 0.0f;
            *o = *p;
            o->pos[0] += (sx * 2.0f - 1.0f) * hx;
            o->pos[1] -= (sy * 2.0f - 1.0f) * hy;
            if (sprite)
                for (int s = 0; s < 4; s++) {
                    o->tc[s][0] = sx;
                    o->tc[s][1] = sy;
                    o->tc[s][2] = 0.0f;
                    o->tc[s][3] = 1.0f;
                }
        }
        qi[i * 6 + 0] = i * 4;     qi[i * 6 + 1] = i * 4 + 1; qi[i * 6 + 2] = i * 4 + 2;
        qi[i * 6 + 3] = i * 4 + 1; qi[i * 6 + 4] = i * 4 + 3; qi[i * 6 + 5] = i * 4 + 2;
    }
    d->ps.topology = 0;
    d->ps.cull = 0;             /* points have no facing */
    mdr_gfx_draw(d, qv, n * 4, qi, n * 6);
}

static bool fvf_on_gpu_job(uint32_t base, uint32_t stride, uint32_t first, uint32_t count, mdr_gpu_fvf *g)
{
    Uint64 t = SDL_GetTicksNS();
    bool ok = fvf_gpu_job(st.vshader, base, stride, first, count, g);
    perf_ns[PERF_FVF] += SDL_GetTicksNS() - t;
    return ok;
}

static bool timed_fvf(uint32_t fvf, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                      mdr_vertex *out, bool *pre)
{
    Uint64 t = SDL_GetTicksNS();
    bool ok = fetch_fvf(fvf, base, stride, first, count, out, pre);
    perf_ns[PERF_FVF] += SDL_GetTicksNS() - t;
    return ok;
}

/* Widescreen (src/guest/widescreen.c): the game lays out its 2D (HUD,
 * menus, the title's logo) for 4:3 in a frame the window shows wider, so
 * each 2D draw is squeezed back by 4:3 over the screen's shape: the HUD at
 * its own proportions. A draw is 2D when it is screen space: fixed-function
 * pre-transformed vertices (XYZRHW), or a vertex program whose output w is
 * 1 for its first and last vertex (a perspective draw's w is its depth).
 * One as wide as the screen (a backdrop, a fade) stays as it is.
 *
 * About the middle (Display > HUD Centered: the 4:3 layout in the middle of
 * the screen), or with Edges, in a race (its progress line drawn this frame
 * or the last; menus stay as laid out), for a piece in the top third and the
 * left half: about the left edge, so the group keeps its spacing (Mad
 * Dash's gauge, item box and counter), unless it lies over a piece kept in
 * the middle (a pause panel; over_wide). The rest of the race HUD (the
 * place, the progress line and its racers, the timer) stays as laid out:
 * pinned to the edges, the place's figure left its bubble. A frame that
 * has drawn a 2D backdrop as wide and as high as the screen is a menu or a
 * loading screen: everything in it about the middle, as laid out. The extent is over all the draw's
 * vertices, up to SPAN_MAX. */
typedef struct squeeze_t { float k, at; } squeeze_t;    /* factor (1: none); anchor, clip x */

/* HUD Edges: the 2D pieces kept about the middle, this frame's and the
 * last's, so a top-left piece lying over one stays with it, whatever the
 * order they are drawn in (a panel's lines over its background). */
#define WIDE_MAX 32
typedef struct wide_set { float r[WIDE_MAX][4]; int n; } wide_set;  /* lo, hi, top, bot */
static wide_set wide_now, wide_last;

static void boxes_frame(void);

static void wide_frame(void)
{
    boxes_frame();
    wide_last = wide_now;
    wide_now.n = 0;
    race_last = race_now;
    race_now = false;
}

static void wide_add(float lo, float hi, float top, float bot)
{
    if (wide_now.n < WIDE_MAX) {
        float *r = wide_now.r[wide_now.n++];
        r[0] = lo, r[1] = hi, r[2] = top, r[3] = bot;
    }
}

static bool over_wide(float lo, float hi, float top, float bot)
{
    const wide_set *b[2] = { &wide_now, &wide_last };
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < b[k]->n; i++) {
            const float *r = b[k]->r[i];
            if (lo < r[1] && hi > r[0] && top < r[3] && bot > r[2])
                return true;
        }
    return false;
}

static squeeze_t hud_squeeze_of(float x0, float x1, float y0, float y1)
{
    squeeze_t q = { 1.0f, 0.0f };
    float lo = SDL_min(x0, x1), hi = SDL_max(x0, x1), top = SDL_min(y0, y1), bot = SDL_max(y0, y1), mid;
    int rtw, rth;

    if (!mdr_widescreen_active())
        return q;
    target_size(&rtw, &rth);
    if (lo <= 1.0f && hi >= (float)rtw - 2.0f) {
        /* Not one through the stencil: the quad that darkens the
         * stencil shadows covers every race frame. */
        if (top <= 1.0f && bot >= (float)rth - 2.0f && !RS(RS_STENCILENABLE))
            backdrop_frame = true;
        return q;
    }
    q.k = (4.0f / 3.0f) / mdr_widescreen_ratio();
    if (mdr_settings_get()->hud_edges && !backdrop_frame && (race_now || race_last)) {
        mid = (float)rtw * 0.5f;
        if (!(bot <= (float)rth / 3.0f && hi <= mid))
            wide_add(lo, hi, top, bot);
        else if (!over_wide(lo, hi, top, bot))
            q.at = -1.0f;
    }
    return q;
}

/* 2D the game placed through the camera: the menus' queued quads (Records'
 * figures, Options' values, the character select's name and description),
 * drawn at the frame's end (0x00121E60 -> 0x001E8E80) through the display
 * list's immediate mode (Begin/End, returning to 0x001ED0F5). Their places
 * come from the projection, whose x scale the wide camera narrows by
 * 4:3 over the screen's shape (0x00305418: 2.80 at 4:3, 2.10 at 16:9), so
 * they arrive narrowed already; squeezed as well, they sat inside their 3D
 * frames by that much again. Their size is the game's pixels, though: such
 * a draw is narrowed about the point it was placed by (rows_anchor: the
 * left edge of left-aligned lines, the middle of centred ones), which stays
 * where the camera put it. Fixed 2D (the race HUD, the title) comes from
 * the display list's vertex arrays, and the only such quad in a race is the
 * full-screen one that darkens the stencil shadows (left as it is). Text
 * only -- two quads or more, and not on a fixed panel (on_box): the same
 * path draws the save popup's box, its edge lines and its text, all laid
 * out fixed (left where the camera would have put them, the lines stood off
 * the box and the text off its middle). */
static bool queued_quads(const mdr_draw *d)
{
    return d->caller[0] == 0x001ED0F5u && d->caller[1] == 0x00121F7Cu;
}

static bool camera_placed(const mdr_draw *d, uint32_t count)
{
    return queued_quads(d) && count >= 8;
}

/* Panels laid out fixed on the same path: one quad, at least 64 x 16
 * pixels (the save popup's box), this frame's and the last's. What lies on
 * one (its text, its edge lines) is fixed 2D as well, squeezed as any: the
 * menus' camera-placed text sits on 3D frames, not on such a box. */
#define BOX_MAX 8
static struct { float r[BOX_MAX][4]; int n; } boxes_now, boxes_last;

static void boxes_frame(void)
{
    boxes_last = boxes_now;
    boxes_now.n = 0;
}

static void box_note(const mdr_draw *d, uint32_t count, float lo, float hi, float top, float bot)
{
    if (!queued_quads(d) || count >= 8 || hi - lo < 64.0f || bot - top < 16.0f || boxes_now.n == BOX_MAX)
        return;
    boxes_now.r[boxes_now.n][0] = lo;
    boxes_now.r[boxes_now.n][1] = hi;
    boxes_now.r[boxes_now.n][2] = top;
    boxes_now.r[boxes_now.n][3] = bot;
    boxes_now.n++;
}

static bool on_box(float lo, float hi, float top, float bot)
{
    for (int k = 0; k < 2; k++) {
        const float (*r)[4] = k ? boxes_last.r : boxes_now.r;
        int n = k ? boxes_last.n : boxes_now.n;
        for (int i = 0; i < n; i++)
            if (lo < r[i][1] && hi > r[i][0] && top < r[i][3] && bot > r[i][2])
                return true;
    }
    return false;
}

/* A squeezed draw's scissor, squeezed with it (about the same point, in
 * the target's pixels): a window around a piece of HUD or menu stays
 * around it. */
static void squeeze_scissor(mdr_draw *d, squeeze_t q)
{
    int rtw, rth;
    float a;

    if (q.k == 1.0f || d->scissor[2] <= d->scissor[0])
        return;
    target_size(&rtw, &rth);
    a = (q.at + 1.0f) * 0.5f * (float)rtw;
    d->scissor[0] = (int16_t)SDL_floorf(a + ((float)d->scissor[0] - a) * q.k);
    d->scissor[2] = (int16_t)SDL_ceilf(a + ((float)d->scissor[2] - a) * q.k);
}

/* A clip-space vertex squeezed: x about at * w. */
static void squeeze_vertex(mdr_vertex *v, squeeze_t q)
{
    float a = q.at * v->pos[3];
    v->pos[0] = a + (v->pos[0] - a) * q.k;
}

/* A fixed-function draw's squeeze: pre-transformed ones only, by the
 * extent of their vertices. */
static squeeze_t fvf_squeeze(uint32_t base, uint32_t stride, uint32_t first, uint32_t count)
{
    float lx = 1e9f, hx = -1e9f, ly = 1e9f, hy = -1e9f;
    squeeze_t none = { 1.0f, 0.0f };

    if (!count || !mdr_widescreen_active() || (st.vshader & 0x00E) != 0x004)
        return none;
    for (uint32_t i = 0; i < count; i++) {
        float x = lf(MEM32(base + (first + i) * stride)), y = lf(MEM32(base + (first + i) * stride + 4));
        lx = SDL_min(lx, x);
        hx = SDL_max(hx, x);
        ly = SDL_min(ly, y);
        hy = SDL_max(hy, y);
    }
    return hud_squeeze_of(lx, hx, ly, hy);
}

static void submit_draw(uint32_t type, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                        const uint32_t *order, uint32_t n)
{
    static mdr_vertex *vbuf;
    static uint32_t *ibuf, vcap, icap;
    mdr_draw d;
    bool pre;
    uint32_t ni;
    const mdr_vsh_program *gpu_p = NULL;    /* the program, when it runs on the GPU */
    static mdr_gpu_fvf gpu_f;
    bool fvf_on_gpu = false;                /* or the fixed-function vertices do */
    bool cpu_too = true;                    /* and the CPU makes the vertices as well */
    squeeze_t squeeze = { 1.0f, 0.0f };     /* a 2D draw's on a wider screen (hud_squeeze_of) */

    state_init();
    if (count > vcap) {
        vcap = count * 2;
        vbuf = SDL_realloc(vbuf, vcap * sizeof *vbuf);
    }
    if (n * 6 > icap) {
        icap = n * 6;
        ibuf = SDL_realloc(ibuf, icap * sizeof *ibuf);
    }
    if (prog_selected) {
        mdr_vsh_program *p = current_program();
        if (!p) {
            draws_dropped++;
            return;
        }
        program_inputs(p);
        {
            Uint64 t = SDL_GetTicksNS();
            if (use_gpu_vsh(p)) {
                gpu_p = p;
                /* A capture keeps the CPU's vertices too, to log and to
                 * draw the frame both ways (MDR_CAPTURE_CPUVSH); so does a
                 * program whose own shader's pipelines are still being
                 * made, drawn from them meanwhile (renderer.c, settled).
                 * A point list is drawn on the GPU alone (its squares are
                 * made there; the CPU's vertices are points): its draws
                 * wait for its pipelines instead. */
                cpu_too = mdr_gfx_capturing() || (type != 1 && mdr_gfx_vsh_needs_cpu(p->gpu_program));
            }
            if (cpu_too)
                fetch_vsh(p, base, stride, first, count, vbuf);
            perf_ns[PERF_VSH] += SDL_GetTicksNS() - t;
            {   /* Why the CPU ran vertex programs, every 5 s (the battery
                 * and the game thread's time: only some of it is needed) */
                static uint64_t why[5], last;
                if (cpu_too)
                    why[type == 1 ? 0 : p->writes_constants ? 1 : !gpu_p ? 2
                        : mdr_gfx_capturing() ? 3 : 4] += count;
                if (mdr_gfx_frame_count() >= last + 300) {
                    last = mdr_gfx_frame_count();
                    if (why[0] + why[1] + why[2] + why[3] + why[4])
                        SDL_Log("d3d: vertex-program vertices made on the CPU in 300 frames: %llu point lists, "
                                "%llu programs that write constants, %llu with the GPU's off or unable, "
                                "%llu for a capture, %llu while their own pipeline is made (or one of them)",
                                (unsigned long long)why[0], (unsigned long long)why[1], (unsigned long long)why[2],
                                (unsigned long long)why[3], (unsigned long long)why[4]);
                    SDL_memset(why, 0, sizeof why);
                }
            }
        }
        pre = false;
        {   /* How many program draws also have a pixel shader (not drawn yet). */
            static uint32_t draws, with_ps;
            static uint64_t last;
            draws++;
            with_ps += st.pshader != 0;
            if (mdr_gfx_frame_count() >= last + 300) {
                SDL_Log("d3d: %u vertex-program draws, %u with a pixel shader", draws, with_ps);
                draws = with_ps = 0;
                last = mdr_gfx_frame_count();
            }
        }
    } else if (type != 1 && (squeeze = fvf_squeeze(base, stride, first, count)).k == 1.0f &&
               mdr_settings_get()->gpu_fixed && !mdr_gfx_vertex_shader_failed(true) &&
               fvf_on_gpu_job(base, stride, first, count, &gpu_f)) {
        fvf_on_gpu = true;
        pre = (gpu_f.u.layout0[2] & MDR_FVF_PRE) != 0;
        cpu_too = mdr_gfx_capturing();
        if (cpu_too)
            timed_fvf(st.vshader, base, stride, first, count, vbuf, &pre);
    } else if (!timed_fvf(st.vshader, base, stride, first, count, vbuf, &pre)) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: FVF 0x%x not handled", st.vshader);
        draws_dropped++;
        return;
    } else if (squeeze.k != 1.0f) {
        for (uint32_t i = 0; i < count; i++)
            squeeze_vertex(&vbuf[i], squeeze);
    }
    fill_draw_state(&d, pre);
    if (d.tex[0].data) {
        /* The race's progress line, its texture 512 x 8: a race on screen
         * (HUD Edges, hud_squeeze_of). */
        mdr_xtex_info ti;
        mdr_xtex_describe(d.tex[0].format_word, d.tex[0].size_word, &ti);
        if (ti.known && ti.width == 512 && ti.height == 8)
            race_now = true;
    }
    {   /* MDR_CAPTURE=frame: the render-state table around fog, once. */
        static long long cap = -2;
        if (cap == -2)
            cap = getenv("MDR_CAPTURE") ? atoll(getenv("MDR_CAPTURE")) : -1;
        if (cap >= 0 && mdr_gfx_frame_count() == (uint64_t)cap) {
            cap = -1;
            for (int r = 76; r < 124; r += 8)
                SDL_Log("capture: RS[%d..] %08x %08x %08x %08x %08x %08x %08x %08x", r, RS(r), RS(r + 1),
                        RS(r + 2), RS(r + 3), RS(r + 4), RS(r + 5), RS(r + 6), RS(r + 7));
        }
    }
    {   /* MDR_PS_DEBUG=frame: describe the pixel-shader draws of that frame. */
        static long dbg = -2;
        if (dbg == -2) {
            const char *e = getenv("MDR_PS_DEBUG");
            dbg = e ? atol(e) : -1;
        }
        if (dbg >= 0 && (long)mdr_gfx_frame_count() == dbg && st.pshader && cpu_too) {
            SDL_Log("ps draw: ps 0x%08x vs 0x%08x %u verts; v0 diffuse %08x spec %08x",
                    st.pshader, st.vshader, count, vbuf[0].diffuse, vbuf[0].specular);
            for (int s = 0; s < 4; s++) {
                mdr_xtex_info ti;
                mdr_xtex_describe(d.tex[s].format_word, d.tex[s].size_word, &ti);
                if (d.tex[s].data && ti.known && ti.width <= 1024 && ti.height <= 1024) {
                    /* Save the decoded texture as ps_tex_<address>.bmp. */
                    uint8_t *rgba = SDL_malloc((size_t)ti.width * ti.height * 4);
                    if (rgba) {
                        char name[64];
                        SDL_Surface *sf;
                        mdr_xtex_decode(&ti, (const uint8_t *)XBOX_PTR(d.tex[s].data | 0x80000000u), rgba);
                        sf = SDL_CreateSurfaceFrom((int)ti.width, (int)ti.height, SDL_PIXELFORMAT_RGBA32,
                                                   rgba, (int)ti.width * 4);
                        snprintf(name, sizeof name, "ps_tex_%08x.bmp", d.tex[s].data);
                        if (sf) { SDL_SaveBMP(sf, name); SDL_DestroySurface(sf); }
                        SDL_free(rgba);
                    }
                }
                SDL_Log("  T%d: data %08x %s %ux%u pitch %u scale %g,%g tc %g,%g,%g,%g "
                        "c0 %.2f,%.2f,%.2f,%.2f c1 %.2f,%.2f,%.2f,%.2f",
                        s, d.tex[s].data, d.tex[s].data ? mdr_xtex_format_name(ti.format) : "-",
                        ti.width, ti.height, ti.pitch, d.ff.tc_scale[s][0], d.ff.tc_scale[s][1],
                        vbuf[0].tc[s][0], vbuf[0].tc[s][1], vbuf[0].tc[s][2], vbuf[0].tc[s][3],
                        d.ff.ps_c0[s][0], d.ff.ps_c0[s][1], d.ff.ps_c0[s][2], d.ff.ps_c0[s][3],
                        d.ff.ps_c1[s][0], d.ff.ps_c1[s][1], d.ff.ps_c1[s][2], d.ff.ps_c1[s][3]);
            }
        }
    }
    if (type == 1 && gpu_p && !cpu_too) {
        /* A point list's squares made by the vertex shader: four vertices
         * a point, numbered point * 4 + corner (shaders/vsh.vert). The
         * CPU ran each particle's program and made its square (20-120
         * thousand vertices a second in Orta's episode 1 on the RP6). */
        static uint32_t *qi, qcap;
        mdr_gpu_vsh g;
        if (n > qcap) {
            qcap = n * 2;
            qi = SDL_realloc(qi, qcap * 6 * sizeof *qi);
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = order[i] << 2;
            qi[i * 6 + 0] = v;     qi[i * 6 + 1] = v | 1; qi[i * 6 + 2] = v | 2;
            qi[i * 6 + 3] = v | 1; qi[i * 6 + 4] = v | 3; qi[i * 6 + 5] = v | 2;
        }
        vsh_gpu_job(gpu_p, base, stride, first, count, &g);
        g.points = RS(RS_POINTSPRITEENABLE) ? 2 : 1;
        g.point_min = lf(RS(RS_POINTSIZE_MIN));
        g.point_max = lf(RS(RS_POINTSIZE_MAX));
        g.point_vp[0] = d.viewport[2] > 0.0f ? d.viewport[2] : 1.0f;
        g.point_vp[1] = d.viewport[3] > 0.0f ? d.viewport[3] : 1.0f;
        d.ps.topology = 0;
        d.ps.cull = 0;
        mdr_gfx_draw_vsh(&d, &g, count * 4, qi, n * 6, NULL);
        draws_drawn++;
        return;
    }
    if (type == 1) {
        draw_points(&d, vbuf, order, n);
        return;
    }
    ni = build_indices(type, order, n, ibuf, &d.ps.topology);
    if (gpu_p) {
        mdr_gpu_vsh g;
        want_text_anchor = mdr_widescreen_active() && camera_placed(&d, count);
        vsh_gpu_job(gpu_p, base, stride, first, count, &g);
        if (g.spanned) {
            squeeze = hud_squeeze_of(g.span[0], g.span[1], g.span[2], g.span[3]);
            box_note(&d, count, g.span[0], g.span[1], g.span[2], g.span[3]);
            if (want_text_anchor && on_box(g.span[0], g.span[1], g.span[2], g.span[3]))
                want_text_anchor = false;   /* text on a fixed panel */
        }
        if (g.spanned && squeeze.k != 1.0f && want_text_anchor) {
            int rtw, rth;
            target_size(&rtw, &rth);
            squeeze.at = text_anchor * 2.0f / (float)rtw - 1.0f;   /* about the point it was placed by */
        }
        want_text_anchor = false;
        if (squeeze.k != 1.0f) {
            /* The renderer takes the viewport's transform back off oPos
             * (shaders/vsh.vert, to_clip: x = (oPos.x - offset) / scale,
             * w being 1): scale / k narrows the picture by k about the
             * viewport's middle, and the offset moved by at (1 - k) scale
             * / k puts the point it is narrowed about at clip x at. */
            g.vp_offset[0] -= squeeze.at * (1.0f - squeeze.k) * g.vp_scale[0] / squeeze.k;
            g.vp_scale[0] /= squeeze.k;
            if (cpu_too)
                for (uint32_t i = 0; i < count; i++)
                    squeeze_vertex(&vbuf[i], squeeze);
        }
        squeeze_scissor(&d, squeeze);
        mdr_gfx_draw_vsh(&d, &g, count, ibuf, ni, cpu_too ? vbuf : NULL);
    } else if (fvf_on_gpu) {
        squeeze_scissor(&d, squeeze);
        if (!mdr_gfx_draw_fvf(&d, &gpu_f, count, ibuf, ni, cpu_too ? vbuf : NULL)) {
            if (!cpu_too)
                timed_fvf(st.vshader, base, stride, first, count, vbuf, &pre);
            mdr_gfx_set_draw_source((const void *)XBOX_PTR(base + first * stride));
            mdr_gfx_draw(&d, vbuf, count, ibuf, ni);
        }
    } else {
        if (prog_selected && count && mdr_widescreen_active() &&
            SDL_fabsf(vbuf[0].pos[3] - 1.0f) < 1e-4f && SDL_fabsf(vbuf[count - 1].pos[3] - 1.0f) < 1e-4f) {
            /* A program run on the CPU: screen space as on the GPU path,
             * by the extent of all its vertices. */
            int rtw, rth;
            bool camera;
            float lx = 1e9f, hx = -1e9f, ly = 1e9f, hy = -1e9f;
            target_size(&rtw, &rth);
            for (uint32_t i = 0; i < count; i++) {
                lx = SDL_min(lx, vbuf[i].pos[0]);
                hx = SDL_max(hx, vbuf[i].pos[0]);
                ly = SDL_min(ly, vbuf[i].pos[1]);
                hy = SDL_max(hy, vbuf[i].pos[1]);
            }
            squeeze = hud_squeeze_of((lx + 1.0f) * 0.5f * (float)rtw, (hx + 1.0f) * 0.5f * (float)rtw,
                                     (1.0f - hy) * 0.5f * (float)rth, (1.0f - ly) * 0.5f * (float)rth);
            {
                float plo = (lx + 1.0f) * 0.5f * (float)rtw, phi = (hx + 1.0f) * 0.5f * (float)rtw;
                float ptop = (1.0f - hy) * 0.5f * (float)rth, pbot = (1.0f - ly) * 0.5f * (float)rth;
                box_note(&d, count, plo, phi, ptop, pbot);
                camera = camera_placed(&d, count) && !on_box(plo, phi, ptop, pbot);
            }
            if (squeeze.k != 1.0f && camera) {   /* about the point it was placed by (clip x) */
                text_rows t;
                t.n = 0;
                for (uint32_t i = 0; i < count; i++)
                    rows_add(&t, vbuf[i].pos[0], vbuf[i].pos[1] * (float)rth * 0.5f);
                squeeze.at = rows_anchor(&t, 3.0f / (float)rtw);
            }
            if (squeeze.k != 1.0f)
                for (uint32_t i = 0; i < count; i++)
                    squeeze_vertex(&vbuf[i], squeeze);
        }
        squeeze_scissor(&d, squeeze);
        /* The game's vertex data, so interpolation can find this draw in
         * the next frame. */
        mdr_gfx_set_draw_source((const void *)XBOX_PTR(base + first * stride));
        mdr_gfx_draw(&d, vbuf, count, ibuf, ni);
    }
    draws_drawn++;
}

/* After a draw: the normal of its last vertex, if it streamed one, is what
 * the hardware now holds (see latched_normal). */
static void latch_normal(uint32_t base, uint32_t stride, uint32_t first, const uint32_t *order, uint32_t n)
{
    uint32_t vi = first + order[n - 1];

    if (prog_selected) {
        uint32_t sb = nv_array_base(2), ss = nv.va_format[2] >> 8;
        float v[4];
        if (!sb || !((nv.va_format[2] >> 4) & 0xF))
            return;
        if (!sb)
            return;
        mdr_vsh_fetch(nv.va_format[2] & 0xFF, (const uint8_t *)XBOX_PTR(sb + vi * ss), v);
        memcpy(latched_normal, v, sizeof latched_normal);
    } else {
        fvf_layout l;
        if (!layout_fvf(st.vshader, &l) || !l.has_normal)
            return;
        if (!stride)
            stride = l.size;
        for (int c = 0; c < 3; c++)
            latched_normal[c] = f32(base + vi * stride + l.normal_off + 4 * c);
    }
}

static void submit(uint32_t type, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                   const uint32_t *order, uint32_t n)
{
    Uint64 t = SDL_GetTicksNS();
    submit_draw(type, base, stride, first, count, order, n);
    if (n)
        latch_normal(base, stride, first, order, n);
    perf_ns[PERF_DRAWS] += SDL_GetTicksNS() - t;
}

static uint32_t *sequential(uint32_t n)
{
    static uint32_t *seq, cap;
    if (n > cap) {
        cap = n * 2;
        seq = SDL_realloc(seq, cap * sizeof *seq);
        for (uint32_t i = 0; i < cap; i++)
            seq[i] = i;
    }
    return seq;
}



/* The smallest and largest of n 16-bit indices: eight at a time with SSE2
 * (signed 16-bit min/max on values offset by 0x8000), the rest one by one. */
static void index_range(const uint16_t *idx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
    uint32_t i = 0, l = 0xFFFF, h = 0;
#ifdef HAVE_SSE2
    if (n >= 16) {
        const __m128i bias = _mm_set1_epi16((short)0x8000);
        __m128i vl = _mm_set1_epi16(0x7FFF), vh = _mm_set1_epi16((short)0x8000);
        uint16_t a[8], b[8];
        for (; i + 8 <= n; i += 8) {
            __m128i v = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(idx + i)), bias);
            vl = _mm_min_epi16(vl, v);
            vh = _mm_max_epi16(vh, v);
        }
        _mm_storeu_si128((__m128i *)a, _mm_xor_si128(vl, bias));
        _mm_storeu_si128((__m128i *)b, _mm_xor_si128(vh, bias));
        for (int k = 0; k < 8; k++) {
            l = a[k] < l ? a[k] : l;
            h = b[k] > h ? b[k] : h;
        }
    }
#endif
    for (; i < n; i++) {
        l = idx[i] < l ? idx[i] : l;
        h = idx[i] > h ? idx[i] : h;
    }
    *lo = l;
    *hi = h;
}

/* ── Immediate mode: Begin / SetVertexData* / End ────────────────────────
 * JSRF drew its fades and message boxes this way. Attributes are latched
 * per register (D3DVSDE_*); writing the position emits a vertex with the
 * latched attributes, as on the hardware. Positions are interpreted by the
 * current FVF: screen space (XYZRHW) or transformed by the matrices. */

enum { VDE_POSITION = 0, VDE_NORMAL = 2, VDE_DIFFUSE = 3, VDE_SPECULAR = 4, VDE_TEXCOORD0 = 9 };

static struct {
    bool       active;
    uint32_t   type;
    float      diffuse[4], specular[4], tc[4][4];
    mdr_vertex *v;
    uint32_t   n, cap;
} imm;

static void imm_emit(float x, float y, float z, float w)
{
    mdr_vertex *o;
    int rtw, rth;

    if (imm.n == imm.cap) {
        imm.cap = imm.cap ? imm.cap * 2 : 64;
        imm.v = SDL_realloc(imm.v, imm.cap * sizeof *imm.v);
    }
    o = &imm.v[imm.n++];
    memset(o, 0, sizeof *o);
    target_size(&rtw, &rth);
    if ((st.vshader & 0x00E) == 0x004) {
        float iw = w != 0.0f ? 1.0f / w : 1.0f;     /* w carries rhw */
        o->pos[0] = (x / (float)rtw * 2.0f - 1.0f) * iw;
        o->pos[1] = (1.0f - y / (float)rth * 2.0f) * iw;
        o->pos[2] = z * iw;
        o->pos[3] = iw;
    } else {
        float wv[16], wvp[16];
        mat_mul(st.mat[TS_WORLD], st.mat[TS_VIEW], wv);
        mat_mul(wv, st.mat[TS_PROJECTION], wvp);
        for (int c = 0; c < 4; c++)
            o->pos[c] = x * wvp[c] + y * wvp[4 + c] + z * wvp[8 + c] + w * wvp[12 + c];
    }
    o->diffuse = pack_color(imm.diffuse);
    o->specular = pack_color(imm.specular);
    o->fog = 1.0f;
    o->psize = point_size(lf(RS(RS_POINTSIZE)), 0.0f, false);
    for (int s = 0; s < 4; s++) {
        uint32_t set = TS(s, TSS_TEXCOORDINDEX) & 0xFFFF;
        if (set < 4)
            memcpy(o->tc[s], imm.tc[set], sizeof o->tc[s]);
    }
}

/* With a vertex program selected, Begin/End's registers are the program's
 * inputs v0-v15: each position write (v0) makes a vertex of all sixteen as
 * last set, staged as float4 arrays and drawn like any other draw (End). */
static struct {
    float    reg[16][4];
    float   *v;                  /* n x 16 x float4 */
    uint32_t n, cap;
    uint16_t set;                /* registers written since Begin */
} immp;

static void imm_set(uint32_t reg, float a, float b, float c, float d)
{
    float v[4] = { a, b, c, d };

    if (prog_selected) {
        if (reg == 0xFFFFFFFFu)
            reg = 0;
        if (reg < 16) {
            memcpy(immp.reg[reg], v, sizeof v);
            immp.set |= (uint16_t)(1u << reg);
        }
        if (reg == 0) {
            if (immp.n == immp.cap) {
                immp.cap = immp.cap ? immp.cap * 2 : 64;
                immp.v = SDL_realloc(immp.v, (size_t)immp.cap * sizeof immp.reg);
            }
            memcpy(immp.v + (size_t)immp.n * 64, immp.reg, sizeof immp.reg);
            immp.n++;
        }
        return;
    }
    if (reg == VDE_POSITION || reg == 0xFFFFFFFFu)
        imm_emit(a, b, c, d);
    else if (reg == VDE_NORMAL)
        memcpy(latched_normal, v, sizeof latched_normal);
    else if (reg == VDE_DIFFUSE)
        memcpy(imm.diffuse, v, sizeof v);
    else if (reg == VDE_SPECULAR)
        memcpy(imm.specular, v, sizeof v);
    else if (reg >= VDE_TEXCOORD0 && reg < VDE_TEXCOORD0 + 4)
        memcpy(imm.tc[reg - VDE_TEXCOORD0], v, sizeof v);
}

/* D3DDevice_Begin(PrimitiveType): `ret 4` */
void sub_00247B30(void)
{
    state_init();
    imm.active = true;
    imm.type = MEM32(g_esp + 4);
    imm.n = 0;
    immp.n = 0;
    immp.set = 0;
    for (int r = 0; r < 16; r++) {
        immp.reg[r][0] = immp.reg[r][1] = immp.reg[r][2] = 0.0f;
        immp.reg[r][3] = 1.0f;
    }
    imm.diffuse[0] = imm.diffuse[1] = imm.diffuse[2] = imm.diffuse[3] = 1.0f;
    memset(imm.specular, 0, sizeof imm.specular);
    memset(imm.tc, 0, sizeof imm.tc);
    g_esp += 4 + 4;
}

/* D3DDevice_SetVertexData2f(Register, a, b): `ret 0xc` */
void sub_00247A40(void)
{
    imm_set(MEM32(g_esp + 4), f32(g_esp + 8), f32(g_esp + 12), 0.0f, 1.0f);
    g_esp += 4 + 12;
}

/* D3DDevice_SetVertexDataColor(Register, D3DCOLOR): `ret 8` */
void sub_00247AE0(void)
{
    float c[4];
    unpack_d3dcolor(MEM32(g_esp + 8), c);
    imm_set(MEM32(g_esp + 4), c[0], c[1], c[2], c[3]);
    g_esp += 4 + 8;
}

/* D3DDevice_SetVertexData4f(Register, a, b, c, d): `ret 0x14` */
void sub_00247A80(void)
{
    imm_set(MEM32(g_esp + 4), f32(g_esp + 8), f32(g_esp + 12), f32(g_esp + 16), f32(g_esp + 20));
    g_esp += 4 + 20;
}

/* D3DDevice_End(): `ret` */
void sub_00247B70(void)
{
    draws_called++;
    pb_catch_up();
    pb_trace_draw("End", imm.type, prog_selected ? immp.n : imm.n);
    if (prog_selected && immp.n && getenv("MDR_PB_TRACE")) {
        static long long f = -2;
        if (f == -2)
            f = atoll(getenv("MDR_PB_TRACE"));
        if (mdr_gfx_frame_count() == (uint64_t)f) {
            mdr_vsh_program *tp = current_program();
            for (uint32_t k = prog_start; k < PROG_SLOTS && k < prog_start + 8; k++) {
                SDL_Log("pb:     prog[%u] %08x %08x %08x %08x", k, prog_mem[k][0], prog_mem[k][1], prog_mem[k][2],
                        prog_mem[k][3]);
                if (prog_mem[k][3] & 1)
                    break;
            }
            if (tp) {
                float in[MDR_VSH_INPUTS][4] = { { 0 } }, o[VSH_OUTPUTS][4], sc[3], of[3], zs;
                vsh_constants(tp, sc, of, &zs);
                for (int r = 0; r < 16 && r < MDR_VSH_INPUTS; r++)
                    memcpy(in[r], immp.v + r * 4, sizeof in[r]);
                mdr_vsh_trace = true;
                mdr_vsh_run(tp, (const float (*)[4])in, vconst, o);
                mdr_vsh_trace = false;
                SDL_Log("pb:     v0 out oPos %g %g %g %g tex0 %g %g", o[VSH_OPOS][0], o[VSH_OPOS][1], o[VSH_OPOS][2],
                        o[VSH_OPOS][3], o[VSH_OT0][0], o[VSH_OT0][1]);
            } else {
                SDL_Log("pb:     no program (selected %d start %u)", prog_selected, prog_start);
            }
        }
        if (mdr_gfx_frame_count() == (uint64_t)f)
            for (uint32_t i = 0; i < immp.n && i < 4; i++)
                for (int r = 0; r < 16; r++)
                    if (immp.set & (1u << r))
                        SDL_Log("pb:     v%u r%d %g %g %g %g", i, r, immp.v[i * 64 + r * 4], immp.v[i * 64 + r * 4 + 1],
                                immp.v[i * 64 + r * 4 + 2], immp.v[i * 64 + r * 4 + 3]);
    }
    if (imm.active && prog_selected && immp.n) {
        /* Stage the vertices in guest memory as the program's arrays, for
         * the draw path, which reads the game's own vertex bytes. */
        static uint32_t stage, stage_bytes;
        uint32_t bytes = immp.n * (uint32_t)sizeof immp.reg, saved_off[16], saved_fmt[16];
        if (bytes > stage_bytes) {
            if (stage)
                xbox_HeapFree(stage);
            stage_bytes = bytes * 2;
            stage = xbox_HeapAlloc(stage_bytes, 16);
        }
        if (stage) {
            memcpy((void *)XBOX_PTR(stage), immp.v, bytes);
            memcpy(saved_off, nv.va_offset, sizeof saved_off);
            memcpy(saved_fmt, nv.va_format, sizeof saved_fmt);
            for (int r = 0; r < 16; r++) {
                /* FLOAT4 (0x42) at stride 256: the NV2A's format word */
                nv.va_format[r] = (immp.set & (1u << r)) ? (256u << 8) | 0x42u : 0;
                nv.va_offset[r] = stage + 16u * (uint32_t)r;
            }
            nv_staged = true;
            submit(imm.type, nv_array_base(0), 256, 0, immp.n, sequential(immp.n), immp.n);
            nv_staged = false;
            memcpy(nv.va_offset, saved_off, sizeof saved_off);
            memcpy(nv.va_format, saved_fmt, sizeof saved_fmt);
        }
        imm.active = false;
        g_esp += 4;
        return;
    }
    if (imm.active && imm.n) {
        static uint32_t *ibuf, icap;
        mdr_draw d;
        if (imm.n * 6 > icap) {
            icap = imm.n * 6;
            ibuf = SDL_realloc(ibuf, icap * sizeof *ibuf);
        }
        fill_draw_state(&d, (st.vshader & 0x00E) == 0x004);
        if (imm.type == 1)
            draw_points(&d, imm.v, sequential(imm.n), imm.n);
        else
            mdr_gfx_draw(&d, imm.v, imm.n,
                         ibuf, build_indices(imm.type, sequential(imm.n), imm.n, ibuf, &d.ps.topology));
    }
    imm.active = false;
    g_esp += 4;
}


/* MDR_PB_TRACE=frame: the arrays and texture stages at each draw of it. */
static void pb_trace_draw(const char *what, uint32_t type, uint32_t count)
{
    static long long first = -2, last;
    char line[512];
    int len;

    if (first == -2) {          /* MDR_PB_TRACE=frame or first-last */
        const char *e = getenv("MDR_PB_TRACE");
        first = e ? atoll(e) : -1;
        last = e && strchr(e, '-') ? atoll(strchr(e, '-') + 1) : first;
    }
    if (first < 0 || mdr_gfx_frame_count() < (uint64_t)first || mdr_gfx_frame_count() > (uint64_t)last)
        return;
    len = SDL_snprintf(line, sizeof line, "pb: frame %llu %s type %u count %u, %s 0x%x; arrays",
                       (unsigned long long)mdr_gfx_frame_count(), what, type, count,
                       prog_selected ? "program at" : "fvf", prog_selected ? prog_start : st.vshader);
    for (int i = 0; i < 16 && len < 400; i++)
        if ((nv.va_format[i] >> 4) & 0xF)
            len += SDL_snprintf(line + len, sizeof line - (size_t)len, " %d:%08x@%08x", i, nv.va_format[i],
                                nv.va_offset[i]);
    SDL_Log("%s", line);
    for (int s = 0; s < 4; s++)
        if (nv.tex[s][0] || nv.tex[s][1])
            SDL_Log("pb:   tex %d offset %08x format %08x address %08x ctl0 %08x ctl1 %08x filter %08x rect %08x",
                    s, nv.tex[s][0], nv.tex[s][1], nv.tex[s][2], nv.tex[s][3], nv.tex[s][4], nv.tex[s][5],
                    nv.tex[s][7]);
}

/* ── The API calls that feed the draw state ───────────────────────────── */

/* D3DDevice_LoadVertexShaderProgram(pFunction, Address): `ret 8`. */
extern void sub_0023D910_gen(void);
void sub_0023D910(void)
{
    uint32_t f = MEM32(g_esp + 4), addr = MEM32(g_esp + 8), n = f ? MEM16(f + 2) : 0;

    for (uint32_t i = 0; i < n && addr + i < PROG_SLOTS; i++)
        for (int k = 0; k < 4; k++)
            prog_mem[addr + i][k] = MEM32(f + 4 + 16 * i + 4 * (uint32_t)k);
    sub_0023D910_gen();
}

/* D3DDevice_SelectVertexShader(Handle, Address): `ret 8`. The next draws
 * run the program at Address (the handle's declaration is the arrays the
 * game writes itself). */
extern void sub_0023D980_gen(void);
void sub_0023D980(void)
{
    prog_start = MEM32(g_esp + 8);
    prog_selected = true;
    st.vshader = MEM32(g_esp + 4);
    sub_0023D980_gen();
}

/* D3DDevice_SetVertexShader(Handle): `ret 4`. An FVF code (fixed function)
 * or a created shader (bit 0); Mad Dash's title draws through programs
 * (SelectVertexShader) only. */
extern void sub_0023DC50_gen(void);
void sub_0023DC50(void)
{
    WRAP_BEGIN(1);
    sub_0023DC50_gen();
    st.vshader = a[0];
    prog_selected = false;
    if (a[0] & 1) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: SetVertexShader with a created shader 0x%08x: not handled yet", a[0]);
    }
}

/* D3DDevice_SetTexture(Stage, pTexture): `ret 8`. Used for the movie's
 * picture (Bink's texture); the library writes the stage to the GPU only
 * at its next draw, which we replace, so the stage is taken from here. */
extern void sub_0023B6F0_gen(void);
void sub_0023B6F0(void)
{
    WRAP_BEGIN(2);
    sub_0023B6F0_gen();
    if (a[0] < 4) {
        st.tex[a[0]] = a[1];
        nv.tex_api[a[0]] = true;
    }
}

/* D3DDevice_SetVertexShaderConstant(Register, pConstantData, ConstantCount):
 * `ret 0xc`; NV2A slot = Register + 96. */
extern void sub_0023DD30_gen(void);
void sub_0023DD30(void)
{
    shadow_constants(MEM32(g_esp + 4) + 96u, MEM32(g_esp + 8), 4u * MEM32(g_esp + 12));
    sub_0023DD30_gen();
}

/* D3DDevice_SetTransform(State, pMatrix): `ret 8` */
extern void sub_0023B120_gen(void);
void sub_0023B120(void)
{
    WRAP_BEGIN(2);
    state_init();
    if (a[0] < TS_COUNT && a[1])
        for (int i = 0; i < 16; i++) {
            uint32_t bits = MEM32(a[1] + 4 * (uint32_t)i);
            memcpy(&st.mat[a[0]][i], &bits, 4);
        }
    sub_0023B120_gen();
}

/* D3DDevice_SetViewport(pViewport): `ret 4`. D3DVIEWPORT8 {X, Y, Width,
 * Height, float MinZ, float MaxZ}. */
extern void sub_0023B590_gen(void);
void sub_0023B590(void)
{
    WRAP_BEGIN(1);
    if (a[0]) {
        uint32_t z0 = MEM32(a[0] + 16), z1 = MEM32(a[0] + 20);
        st.vp_x = MEM32(a[0]);
        st.vp_y = MEM32(a[0] + 4);
        st.vp_w = MEM32(a[0] + 8);
        st.vp_h = MEM32(a[0] + 12);
        memcpy(&st.vp_minz, &z0, 4);
        memcpy(&st.vp_maxz, &z1, 4);
        st.have_viewport = true;
    }
    sub_0023B590_gen();
}

/* ── Draws ───────────────────────────────────────────────────────────── */

/* D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount): `ret 0xc`.
 * Replaced: nothing reads the push buffer, so the library's own draw
 * methods are not needed. */
void sub_00247730(void)
{
    uint32_t type = MEM32(g_esp + 4), start = MEM32(g_esp + 8), n = MEM32(g_esp + 12);

    draws_called++;
    pb_catch_up();
    pb_trace_draw("DrawVertices", type, n);
    if (n)
        submit(type, nv_array_base(0), nv.va_format[0] >> 8, start, n, sequential(n), n);
    g_eax = 0;
    g_esp += 4 + 12;
}

/* D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData):
 * `ret 0xc`; 16-bit indices into the arrays. */
void sub_002477D0(void)
{
    uint32_t type = MEM32(g_esp + 4), n = MEM32(g_esp + 8), pidx = MEM32(g_esp + 12);
    static uint32_t *order, cap;

    draws_called++;
    pb_catch_up();
    pb_trace_draw("DrawIndexedVertices", type, n);
    if (n && pidx) {
        const uint16_t *idx = (const uint16_t *)XBOX_PTR(pidx);
        uint32_t ilo = 0xFFFF, ihi = 0;
        if (n > cap) {
            cap = n * 2;
            order = SDL_realloc(order, cap * sizeof *order);
        }
        index_range(idx, n, &ilo, &ihi);
        for (uint32_t i = 0; i < n; i++)
            order[i] = (uint32_t)idx[i] - ilo;
        submit(type, nv_array_base(0), nv.va_format[0] >> 8, ilo, ihi - ilo + 1, order, n);
    }
    g_eax = 0;
    g_esp += 4 + 12;
}

/* ── Warm reboot (src/guest/guest.c) ──────────────────────────────────────
 * The title boots again in place: everything here that remembers a guest
 * address or the library's state goes back to how it was at start-up. The
 * programs found stay: they are kept by their words, not their address. */
void mdr_d3d_reboot(void)
{
    vblank_data = 0;
    pb_mark = 0;
    memset(&nv, 0, sizeof nv);
    nv_cull_enable = 1;
    nv_cull_face = 0x405;
    nv_const_slot = nv_const_word = nv_prog_slot = nv_prog_word = 0;
    memset(backbuffers, 0, sizeof backbuffers);
    nbackbuffers = 0;
    memset(&cur_rt, 0, sizeof cur_rt);
    cur_zformat = 0;
    memset(vconst, 0, sizeof vconst);
    memset(prog_mem, 0, sizeof prog_mem);
    prog_start = 0;
    prog_selected = false;
    memset(psconst, 0, sizeof psconst);
    memset(psconst_set, 0, sizeof psconst_set);
    memset(latched_normal, 0, sizeof latched_normal);
    nv_staged = false;
    imm.active = false;
    imm.n = 0;
    immp.n = 0;
    immp.set = 0;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < TS_COUNT; i++)
        ident(st.mat[i]);
}
