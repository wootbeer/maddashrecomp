#ifndef MDR_GUEST_H
#define MDR_GUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Map the Xbox address space from the XBE, set up the kernel layer and the
 * function dispatch table. Does not run any game code. */
bool mdr_guest_init(const void *xbe_data, size_t xbe_size,
                    const char *game_dir, const char *save_dir);
/* The game files' folder given to mdr_guest_init. */
const char *mdr_guest_game_dir(void);

/* Run the title from its entry point. Returns when the title exits. */
void mdr_guest_run(uint32_t entry);

/* The guest CPU's registers live per host thread, and mdr_guest_init sets
 * them up on the thread that calls it. To run the title on another thread
 * (made after mdr_guest_init: the Xbox address space has to be mapped at
 * its fixed addresses before a big thread stack can land on them), save
 * them there and load them on the new thread before mdr_guest_run. The
 * guest CPU lock, which the setup gives the calling thread, goes with
 * them. */
typedef struct mdr_guest_cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, fs_base;
} mdr_guest_cpu;
void mdr_guest_cpu_save(mdr_guest_cpu *c);
void mdr_guest_cpu_load(const mdr_guest_cpu *c);

void mdr_guest_shutdown(void);

/* Switching between the disc's programs (src/guest/relaunch.c): when a title
 * boots one this build has, MadDashRecomp starts again on it with its launch
 * data. Install before running the title; load the data handed over (from
 * --launch-data) after mdr_guest_init. */
void mdr_relaunch_install(int argc, char **argv);
bool mdr_relaunch_load(const char *file);

/* Write every game file the title opens, once each, to list_path. */
void mdr_guest_log_opened_files(const char *list_path);

/* The game's movie setup opens a movie (src/hle/dsound_hle.c sees it):
 * its .sfd name, a guest address. d3d_hle.c reads the picture's size. */
void mdr_movie_opened(uint32_t sfd_name_va);

#endif
