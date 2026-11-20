/*
 * Which recompiled program runs. Mad Dash Racing has one (default.xbe).
 *
 * The generated code and our sources are compiled with
 * src/guest/maddash_names.h force-included, so the dispatch is
 * maddash_recomp_lookup and friends. The kernel layer and the host's shared
 * code call the unprefixed names; they are defined here.
 */

#include "guest/program.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t maddash_recomp_lookup(uint32_t xbox_va);
extern recomp_func_t maddash_recomp_lookup_manual(uint32_t xbox_va);
extern int maddash_recomp_dispatch_init(void);
extern size_t maddash_recomp_dispatch_flat_bytes(void);
extern const char *maddash_built_from(void);

static mdr_program active = MDR_PROGRAM_MAIN;

static const char *const names[MDR_PROGRAM_COUNT] = { "maddash" };
static const char *const xbes[MDR_PROGRAM_COUNT] = { "default.xbe" };

const char *mdr_program_name(mdr_program p) { return names[p]; }
const char *mdr_program_xbe(mdr_program p) { return xbes[p]; }

bool mdr_program_from_name(const char *name, mdr_program *out)
{
    int i;

    for (i = 0; i < MDR_PROGRAM_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            *out = (mdr_program)i;
            return true;
        }
    }
    return false;
}

const char *mdr_program_built_from(mdr_program p)
{
    return p == MDR_PROGRAM_MAIN ? maddash_built_from() : NULL;
}

void mdr_program_select(mdr_program p) { active = p; }
mdr_program mdr_program_active(void) { return active; }

recomp_func_t recomp_lookup(uint32_t xbox_va) { return maddash_recomp_lookup(xbox_va); }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { return maddash_recomp_lookup_manual(xbox_va); }
int recomp_dispatch_init(void) { return maddash_recomp_dispatch_init(); }
size_t recomp_dispatch_flat_bytes(void) { return maddash_recomp_dispatch_flat_bytes(); }

/* F11 and hangs: the guest threads' waits. Orta's src/hle/wait_trace.c
 * (now src/hle/orta/) hooked XAPI's waits at Orta's addresses; until it is
 * re-addressed for Mad Dash there is nothing to list. */
void mdr_dump_guest_waits(void) {}
