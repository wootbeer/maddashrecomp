/*
 * The game's dispatch entry points under their own names. CMake
 * force-includes this into the generated code and src/hle/, src/guest/;
 * the unprefixed names, which the kernel layer calls, are
 * src/guest/program.c's. Kept from Orta Recomp's two-program layout, so a
 * second program could link beside it.
 */
#pragma once
#define recomp_call_all maddash_recomp_call_all
#define recomp_dispatch_flat_bytes maddash_recomp_dispatch_flat_bytes
#define recomp_dispatch_init maddash_recomp_dispatch_init
#define recomp_get_count maddash_recomp_get_count
#define recomp_lookup maddash_recomp_lookup
#define recomp_lookup_manual maddash_recomp_lookup_manual
