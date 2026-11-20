#ifndef MDR_VSH_H
#define MDR_VSH_H

/*
 * NV2A vertex programs, run on the CPU.
 *
 * JSRF's vertex shaders are NV2A microcode (up to 136 instructions of 128
 * bits). The renderer takes vertices already in clip space, so the program
 * runs here, per vertex, and its outputs go down the same path as
 * fixed-function vertices. Deterministic and easy to check; moving it to the
 * GPU later is an optimisation with this as the reference.
 */

#include <stdbool.h>
#include <stdint.h>

#define MDR_VSH_MAX_INSNS  136
#define MDR_VSH_CONSTANTS  192
#define MDR_VSH_INPUTS     16

/* One vertex input as the declaration describes it. */
typedef struct mdr_vsh_input {
    uint8_t  stream;     /* vertex stream */
    uint8_t  type;       /* D3DVSDT_* (Xbox) */
    uint16_t offset;     /* byte offset in the stream's vertex */
    bool     present;
} mdr_vsh_input;

/* One operand, decoded. */
typedef struct mdr_vsh_src {
    uint8_t mux, reg, neg, swz[4];
} mdr_vsh_src;

/* One instruction, decoded once when the program is created. */
typedef struct mdr_vsh_op {
    uint8_t mac, ilu;
    uint8_t mac_mask, ilu_mask, o_mask;
    uint8_t out_r, ilu_r, orb, out_mux, rel;
    uint8_t cidx, vidx, use;          /* use: bit 0 A, 1 B, 2 C */
    uint8_t addr;
    mdr_vsh_src src[3];
} mdr_vsh_op;

/* The fast path's form of an instruction (vsh.c, compile): every operand
 * a row of one per-vertex register file (r0-r11, oPos as r12, v0-v15, a
 * row of zeros) or a constant, its swizzle classified, every destination a
 * row, an output or a constant. */
typedef struct mdr_vsh_fsrc {
    uint8_t row;                      /* register-file row (unused for C) */
    uint8_t is_const;
    uint8_t kind;                     /* 0 any swizzle, 1 xyzw, 2 one component */
    uint8_t neg;
    uint8_t swz[4];
    uint8_t slot[4];                  /* row * 4 + swizzle: the four-lane file */
} mdr_vsh_fsrc;

typedef struct mdr_vsh_fop {
    uint8_t mac, ilu, use, rel, cidx;
    uint8_t mac_row, ilu_row;         /* 255: not written */
    uint8_t mac_mask, ilu_mask, o_mask, out_mux;
    uint8_t o_kind, o_index;          /* 0 none, 1 file row, 2 output, 3 constant */
    mdr_vsh_fsrc src[3];
} mdr_vsh_fop;

typedef struct mdr_vsh_program {
    uint32_t      count;                         /* instructions */
    uint32_t      insn[MDR_VSH_MAX_INSNS][4];
    mdr_vsh_op    op[MDR_VSH_MAX_INSNS];
    mdr_vsh_fop   fop[MDR_VSH_MAX_INSNS];
    mdr_vsh_input input[MDR_VSH_INPUTS];
    uint16_t      inputs_read;                   /* v registers the program reads */
    uint8_t       consts_read[MDR_VSH_CONSTANTS / 8]; /* c registers it reads (all: one through a0) */
    bool          writes_constants;              /* then vertices must run in order */
    int32_t       gpu_program;                   /* renderer's copy (mdr_gfx_vsh_program), -1: none yet */
} mdr_vsh_program;

/* Outputs by NV2A output address: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts,
 * 7 oB0, 8 oB1, 9-12 oT0-oT3. */
enum { VSH_OPOS = 0, VSH_OD0 = 3, VSH_OD1 = 4, VSH_OFOG = 5, VSH_OPTS = 6,
       VSH_OB0 = 7, VSH_OB1 = 8, VSH_OT0 = 9, VSH_OUTPUTS = 16 };

/* Parse an XDK vertex shader function (header (slots << 16) | type, then four
 * words per slot) and declaration (D3DVSD tokens). Returns false if the
 * function is not in a form we know. */
bool mdr_vsh_parse(mdr_vsh_program *p, const uint32_t *function, uint32_t function_words,
                   const uint32_t *declaration, uint32_t declaration_words);

/* Run the program on one vertex. c is the constant file, which a program may
 * also write. out[] is filled with the outputs, defaults first. */
void mdr_vsh_run(const mdr_vsh_program *p, const float in[MDR_VSH_INPUTS][4],
                 float c[MDR_VSH_CONSTANTS][4], float out[VSH_OUTPUTS][4]);

/* The same for four vertices at once (the fast path's best case). A
 * program that writes constants runs them one after another, in order. */
void mdr_vsh_run4(const mdr_vsh_program *p, const float in[4][MDR_VSH_INPUTS][4],
                  float c[MDR_VSH_CONSTANTS][4], float out[4][VSH_OUTPUTS][4]);

/* The program for shaders/vsh.vert: 8 words an instruction, into out
 * (8 * p->count words). A program that writes constants has no GPU form;
 * returns false for it. */
bool mdr_vsh_gpu_code(const mdr_vsh_program *p, uint32_t *out);

/* Run fn(ctx, first, end) over [0, count) split across worker threads (the
 * calling thread takes a share too). Returns when all parts are done. */
void mdr_vsh_parallel(uint32_t count, void (*fn)(void *ctx, uint32_t first, uint32_t end), void *ctx);
/* The same pool for any work: chunk items per task, and below min_count
 * items (or a single chunk) on the calling thread alone. Not reentrant:
 * one caller at a time (the game thread). */
void mdr_parallel(uint32_t count, uint32_t chunk, uint32_t min_count,
                  void (*fn)(void *ctx, uint32_t first, uint32_t end), void *ctx);

/* Decode one input attribute of D3DVSDT type 'type' at host pointer src. */
void mdr_vsh_fetch(uint8_t type, const uint8_t *src, float out[4]);

/* Size in bytes of one attribute of a D3DVSDT type (0 if unknown). */
uint32_t mdr_vsh_type_size(uint8_t type);

extern _Thread_local bool mdr_vsh_trace;

#endif
