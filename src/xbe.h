#ifndef MDR_XBE_H
#define MDR_XBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MDR_XBE_MAX_SECTIONS  64
#define MDR_XBE_MAX_LIBRARIES 64

typedef struct mdr_xbe_section {
    char     name[16];
    uint32_t vaddr, vsize;
    uint32_t raw_addr, raw_size;
} mdr_xbe_section;

typedef struct mdr_xbe_library {
    char     name[9];
    uint16_t build;             /* XDK build, e.g. 4134 */
} mdr_xbe_library;

typedef struct mdr_xbe_info {
    uint32_t base;
    uint32_t entry;             /* decoded (un-XORed) entry point VA */
    uint32_t kernel_thunk;      /* decoded kernel thunk table VA */
    uint32_t title_id;
    char     title[41];         /* ASCII rendering of the certificate title */
    uint32_t num_sections;
    mdr_xbe_section sections[MDR_XBE_MAX_SECTIONS];
    uint32_t num_libraries;
    mdr_xbe_library libraries[MDR_XBE_MAX_LIBRARIES];
    uint16_t xdk_build;         /* highest library build: the XDK version */
    uint8_t  file_sha256[32];   /* whole file */
    uint8_t  image_sha256[32];  /* section contents only; ignores the certificate */
} mdr_xbe_info;

typedef enum mdr_xbe_verdict {
    MDR_XBE_KNOWN,          /* a dump we have seen before */
    MDR_XBE_LAYOUT_MATCH,   /* Mad Dash (NTSC-U) section layout, not a known dump */
    MDR_XBE_WRONG_GAME,     /* a valid XBE, but not the game we recompile */
    MDR_XBE_INVALID,        /* not an XBE */
} mdr_xbe_verdict;

/* Parse an in-memory XBE. On failure writes a reason to err and returns false. */
bool mdr_xbe_parse(const uint8_t *data, size_t size, mdr_xbe_info *out,
                   char *err, size_t err_len);

mdr_xbe_verdict mdr_xbe_identify(const mdr_xbe_info *info);

/* Human-readable explanation of a verdict, for dialogs and logs. */
const char *mdr_xbe_verdict_text(mdr_xbe_verdict v);

#endif
