/* Bring-up of the recompiled title on the xboxrecomp kernel layer. */

#include "guest/guest.h"
#include "config.h"

#include "audio/mixer.h"
#include "gfx/renderer.h"
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>

#include <setjmp.h>
#include <stdlib.h>

typedef void (*recomp_func_t)(void);
/* The selected program's (src/guest/program.c). */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int recomp_dispatch_init(void);
extern ptrdiff_t g_xbox_mem_offset;
extern void mdr_crash_report_install(void);

static bool mapped;
static const void *s_xbe;           /* the image, kept for a warm reboot */
static size_t s_xbe_size;

/* ── Record of the game files the title opens ──────────────────────────
 * Every Xbox path the title asks for, once each, in first-use order. Lets a
 * player send just the handful of files needed to reproduce a boot problem
 * rather than the whole disc. Paths are Xbox paths (D:\...), so the
 * list never contains anything about the player's machine. */

#define OPEN_LOG_MAX 4096

static SDL_IOStream *open_log;
static Uint32 open_hashes[OPEN_LOG_MAX];
static int open_count;

static Uint32 path_hash(const char *s)
{
    Uint32 h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (Uint8)SDL_tolower((unsigned char)*s)) * 16777619u;
    return h;
}

static void record_open(const char *xbox_path)
{
    Uint32 h = path_hash(xbox_path);
    int i;

    for (i = 0; i < open_count; i++)
        if (open_hashes[i] == h)
            return;
    if (open_count < OPEN_LOG_MAX)
        open_hashes[open_count++] = h;
    if (open_log) {
        SDL_WriteIO(open_log, xbox_path, SDL_strlen(xbox_path));
        SDL_WriteIO(open_log, "\n", 1);
        SDL_FlushIO(open_log);
    }
}

void mdr_guest_log_opened_files(const char *list_path)
{
    open_log = SDL_IOFromFile(list_path, "w");
    if (!open_log)
        SDL_Log("guest: cannot write %s: %s", list_path, SDL_GetError());
    g_xbox_path_hook = record_open;
}

static char s_game_dir[MDR_PATH_MAX];

const char *mdr_guest_game_dir(void)
{
    return s_game_dir;
}

bool mdr_guest_init(const void *xbe_data, size_t xbe_size,
                    const char *game_dir, const char *save_dir)
{
    /* Have the kernel heap split and merge freed blocks, as the Xbox's does.
     * xboxrecomp's default hands a freed block back only whole, and only to a
     * request no bigger than it, so a title that frees and reallocates in
     * varying sizes fills the heap with free blocks it cannot reuse. JSRF ran
     * its 50 MB heap dry loading the first stage after the logos.
     * Set before the first allocation (the kernel reads it once) and through
     * the C runtime, whose environment the kernel's getenv reads; on Windows
     * SDL_setenv would set the Win32 one instead. */
    if (!getenv("RECOMP_HEAP_RECLAIM")) {
#ifdef _WIN32
        _putenv_s("RECOMP_HEAP_RECLAIM", "1");
#else
        setenv("RECOMP_HEAP_RECLAIM", "1", 1);
#endif
    }

    /* One guest thread at a time, as on the console's single CPU, switching
     * at kernel calls. Without it Orta's threads run truly in parallel: the
     * sound request processor (0x000692C0) walked a list another thread was
     * reallocating and called through a stale entry. Set RECOMP_GUEST_LOCK=0
     * to compare. */
    if (!getenv("RECOMP_GUEST_LOCK")) {
#ifdef _WIN32
        _putenv_s("RECOMP_GUEST_LOCK", "1");
#else
        setenv("RECOMP_GUEST_LOCK", "1", 1);
#endif
    }

    /* Asynchronous file handles answer STATUS_PENDING, as on the console;
     * the data is still read at once. Mad Dash copies a race's AI lines
     * (.dem) from the disc to the cache partition with overlapped reads,
     * and with every read finishing synchronously its copy came out in the
     * wrong order: the race then parsed garbage and printed warnings for
     * good. (xboxrecomp only asks whether the variable is set.) */
    if (!getenv("RECOMP_ASYNC_IO")) {
#ifdef _WIN32
        _putenv_s("RECOMP_ASYNC_IO", "1");
#else
        setenv("RECOMP_ASYNC_IO", "1", 1);
#endif
    }

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        SDL_Log("guest: could not map the Xbox address space");
        return false;
    }
    mapped = true;
    s_xbe = xbe_data;
    s_xbe_size = xbe_size;
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    SDL_Log("guest: address space mapped, host offset 0x%llx",
            (unsigned long long)g_xbox_mem_offset);

    mdr_crash_report_install();
    xbox_kernel_init();
    {
        /* The console's E: (TDATA, UDATA) holds saves; keep it out of the
         * game folder, which a re-import replaces wholesale. A folder per
         * save file, the one picked in the launcher (config.c). */
        char hdd[MDR_PATH_MAX];
        mdr_save_dir(mdr_settings_get()->save_slot, hdd, sizeof hdd);
        SDL_Log("guest: save file %d (%s)", mdr_settings_get()->save_slot, hdd);
        SDL_CreateDirectory(hdd);
        xbox_path_set_hdd_dir(hdd);
    }
    xbox_path_init(game_dir, save_dir);
    SDL_strlcpy(s_game_dir, game_dir, sizeof s_game_dir);
    xbox_kernel_bridge_init();

    if (!recomp_dispatch_init())
        SDL_Log("guest: dispatch table not built; falling back to binary search");
    return true;
}

void mdr_guest_run(uint32_t entry)
{
    recomp_func_t fn = recomp_lookup(entry);

    if (!fn) {
        SDL_Log("guest: entry point 0x%08x has no recompiled function", entry);
        return;
    }
    SDL_Log("guest: entering title at 0x%08x", entry);
    fn();
    SDL_Log("guest: title returned");
}

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
void guest_cpu_join(void);      /* xboxrecomp: the guest CPU lock (patch 0023) */
void guest_cpu_part(void);

/* The kernel layer's setup hands the guest CPU lock to the thread that set
 * it up, as the one that runs the title: it goes with the registers. */
void mdr_guest_cpu_save(mdr_guest_cpu *c)
{
    c->eax = g_eax; c->ecx = g_ecx; c->edx = g_edx; c->ebx = g_ebx;
    c->esp = g_esp; c->ebp = g_ebp; c->esi = g_esi; c->edi = g_edi;
    c->fs_base = g_fs_base;
    guest_cpu_part();
}

void mdr_guest_cpu_load(const mdr_guest_cpu *c)
{
    g_eax = c->eax; g_ecx = c->ecx; g_edx = c->edx; g_ebx = c->ebx;
    g_esp = c->esp; g_ebp = c->ebp; g_esi = c->esi; g_edi = c->edi;
    g_fs_base = c->fs_base;
    guest_cpu_join();
}

void mdr_guest_shutdown(void)
{
    if (!mapped)
        return;
    mdr_mixer_shutdown();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    mapped = false;
    if (open_log) {
        SDL_CloseIO(open_log);
        open_log = NULL;
    }
}

/* ── Warm reboot ──────────────────────────────────────────────────────────
 * The title boots again in this process (relaunch.c): the thread running it
 * unwinds to mdr_guest_run_boots and enters the title again, with the
 * registers it started with the first time. */

static jmp_buf s_boot_jmp;
static SDL_ThreadID s_boot_thread;
static mdr_guest_cpu s_boot_cpu;
static uint32_t s_boot_entry;

static void boot_registers(void)
{
    g_eax = s_boot_cpu.eax; g_ecx = s_boot_cpu.ecx; g_edx = s_boot_cpu.edx; g_ebx = s_boot_cpu.ebx;
    g_esp = s_boot_cpu.esp; g_ebp = s_boot_cpu.ebp; g_esi = s_boot_cpu.esi; g_edi = s_boot_cpu.edi;
    g_fs_base = s_boot_cpu.fs_base;
}

void mdr_guest_run_boots(const mdr_guest_cpu *cpu, uint32_t entry)
{
    s_boot_cpu = *cpu;
    s_boot_entry = entry;
    s_boot_thread = SDL_GetCurrentThreadID();
    mdr_guest_cpu_load(cpu);
    if (setjmp(s_boot_jmp))
        boot_registers();               /* the guest CPU is held already */
    mdr_guest_run(s_boot_entry);
}

extern void mdr_d3d_reboot(void);
extern void mdr_dsound_reboot(void);
extern void mdr_input_reboot(void);
extern int  xbox_kernel_reboot_begin(void);
extern void xbox_kernel_reboot_end(void);

bool mdr_guest_reboot(const uint8_t *launch_page, size_t launch_bytes)
{
    uint32_t page;

    if (!s_xbe || !xbox_kernel_reboot_begin())
        return false;
    /* Every other guest thread is out of guest code from here on. */
    mdr_dsound_reboot();
    mdr_d3d_reboot();
    mdr_input_reboot();
    mdr_gfx_forget_textures();
    if (!xbox_MemoryLayoutReload(s_xbe, s_xbe_size)) {
        SDL_Log("guest: the image could not be loaded again");
        exit(1);
    }
    xbox_kernel_bridge_init();
    page = xbox_HeapAlloc(4096, 4096);
    if (page && launch_page) {
        memcpy((void *)((uintptr_t)page + g_xbox_mem_offset), launch_page,
               launch_bytes < 4096 ? launch_bytes : 4096);
        memcpy((void *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) + g_xbox_mem_offset),
               &page, 4);
    }
    xbox_kernel_reboot_end();
    if (s_boot_thread && SDL_GetCurrentThreadID() == s_boot_thread)
        longjmp(s_boot_jmp, 1);
    /* Asked from another of the title's threads: this one runs the new boot
     * (the first thread parks with the rest). */
    boot_registers();
    mdr_guest_run(s_boot_entry);
    mdr_gfx_game_finished();
    for (;;)
        SDL_Delay(60000);
}
