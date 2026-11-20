# Mad Dash Recomp: notes for Claude sessions

Static recompilation of Mad Dash Racing (Xbox, NTSC-U, Eidos/Crystal
Dynamics, 2001) on xboxrecomp, SDL3 throughout so the same code later runs
on Android. Set up from Orta Recomp (wootbeer/ortarecomp): its launcher,
game-file picker, F1 menu, renderer, mixer and kernel patches, renamed. Orta
was built on Concept of Love (JSRF, wootbeer/conceptoflove): read its
`docs/porting-another-title.md` once for the plan and the lessons.

**Start each session by reading `docs/STATE.md` (one page).** Do not read
`gen/` wholesale: generated C is ~2M lines; grep it for one function by
address or name instead.

## Rules

- **Nothing from the game is ever committed**: no XBE, ISO, extracted files,
  generated C, screenshots, captures, listings, symbol lists, match reports
  or hashes beyond the dump hashes in `src/xbe.c`/`tools/recomp.py`. Players
  supply their own game files. Check `git status` before every commit. No
  personal names, paths or emails in anything committed.
- **Never edit generated C.** Fix the lifter (patch in `patches/xboxrecomp/`,
  to send upstream), the inputs to `tools/recomp.py`, or override a function
  in `src/guest/` or `src/hle/`. Regeneration must stay deterministic.
- **xboxrecomp patches must not overlap.** `recomp.py` and CMake decide a
  patch is applied with `git apply --reverse --check`, one patch at a time.
  To change lines an earlier patch added, edit that patch instead.
  There are 46 now: 0001-0043 as Orta Recomp's, 0044-0046 found here.
- **Third-party projects** (Cxbx-Reloaded and its XbSymbolDatabase, xemu,
  other ports): facts only, no code. Symbol databases and listings are read
  from a local clone at run time, never committed. Our own GPL code from
  Orta Recomp and Concept of Love may be copied.
- Graphics/audio/input are ours, on SDL3 (GPU API, audio, gamepad);
  xboxrecomp is used for the recompiler and the kernel layer only. No
  NV2A/APU emulation.
- Accuracy over speed; keep fixes minimal and explain them in commit messages.
- Keep `docs/STATE.md` to one page: update it at the end of a session and
  move finished detail into `docs/archive/`. Facts about the game go in
  `docs/maddash-facts.md`.

## Overriding game functions

`tools/recomp.py` passes every `.c` file under `src/` to the recompiler,
which reads these forms (literally, at the start of a line):

- `void sub_XXXXXXXX(void)` replaces the function for every caller, direct
  calls included. Declare `extern void sub_XXXXXXXX_gen(void);` to keep the
  original callable (wrapping).
- `void sub_XXXXXXXX_enter(void)` runs at the top of the generated function
  (no trailing comment on that line).

After adding or removing one, rerun `tools/recomp.py`. Guest registers are
`g_eax`... and memory is `MEM32(va)` from `gen/recomp_types.h`. Guest ABI:
stdcall, arguments at `g_esp+4+4n`. Name hooks by address; XDK API names in
comments are fine (public API, factual mapping). Put the API name in the
comment just above the function.

## Porting from Orta Recomp

`src/hle/d3d_hle.c` (growing) and `src/hle/dsound_hle.c` are Mad Dash's,
re-addressed. The rest of Orta's hooks wait in `src/hle/orta/` (`d3d_hle.c`,
`input_hle.c`, `wait_trace.c`; its `dsound_hle.c` for reference), **still at Orta's addresses (XDK 4928)**,
not built and not scanned for overrides. Mad Dash is XDK 3936 (D3D8 3925, DSOUND 3936, XAPI 3911):
every `sub_XXXXXXXX`, `_gen`, `_enter` and every data address there
(`D3D__RenderState`, `D3D__pDevice`, back-buffer surfaces, texture slots,
`RS_ENCODE`, `PROG_VA(...)`) must be re-found in Mad Dash and the
render-state indices re-derived from Mad Dash's own setters, never from
another XDK's table. Before replacing a function, check its `ret N` and
argument use in Mad Dash's own code. Sources of names, all read locally:
Cxbx-Reloaded's XbSymbolDatabase CLI run on the player's own XBE (XDK
symbols), `tools/xbe_match.py` against Orta's or JSRF's XBE, disassembly.
Mad Dash's symbol list is `build/xdk_symbols.txt` (the owner's
XbSymbolDatabase output; never committed). Mad Dash's renderer writes some
NV2A methods into the push buffer itself (`docs/maddash-facts.md`).

Mad Dash uses **Bink** for movies (BINK* sections), not CRI/Sofdec: Orta's
`cri_hle.c` and `aix.c` were not carried over.

## Layout

- `tools/xbe_info.py`: identify an XBE (title ID, XDK build, sections)
- `tools/xbe_match.py`: functions shared with another XBE (Orta, JSRF)
- `tools/recomp.py`: XBE/ISO -> `gen/` (xboxrecomp pipeline)
- `src/`: launcher, game-file picker, XISO reader, XBE identification
- `src/hle/`: Direct3D, DirectSound and input in place of the Xbox's
- `src/guest/`: bring-up of the recompiled title and Mad Dash's own hooks;
  `program.c` routes the dispatch
- `src/gfx/`, `src/audio/`: renderer, launcher/menu overlay, mixer
- `tools/package.py`: release zip; `tools/make_icon.py`: the app icon
- `tools/check_menu_text.py`: run after changing menu labels, values or help
- `external/xboxrecomp`, `external/SDL`: pinned submodules

The owner tests on Windows (`C:\dev\maddashrecomp`, PowerShell).
