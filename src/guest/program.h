#ifndef MDR_PROGRAM_H
#define MDR_PROGRAM_H

#include <stdbool.h>

/* The programs this build can run. Mad Dash Racing has one, default.xbe;
 * the table is kept (from Orta Recomp, which had two) so a second program
 * would slot in the same way. Its generated code links in under
 * maddash_recomp_lookup... (src/guest/maddash_names.h). */
typedef enum mdr_program {
    MDR_PROGRAM_MAIN,   /* default.xbe: Mad Dash Racing */
    MDR_PROGRAM_COUNT
} mdr_program;

/* "maddash": --program and the logs. */
const char *mdr_program_name(mdr_program p);
/* Parse a name; false if it is none of them. */
bool mdr_program_from_name(const char *name, mdr_program *out);
/* Its XBE in the game files: "default.xbe". */
const char *mdr_program_xbe(mdr_program p);
/* Image hash of the XBE it was generated from, or NULL when this build does
 * not have it. */
const char *mdr_program_built_from(mdr_program p);

/* The program the guest runs: recomp_lookup and recomp_lookup_manual, which
 * the kernel layer calls (thread starts, APCs, callbacks), go to its tables.
 * Set before mdr_guest_init. */
void mdr_program_select(mdr_program p);
mdr_program mdr_program_active(void);

#endif
