# Current state

v0.0.1, bring-up. The repository was set up from Orta Recomp (v0.1.15):
launcher, game-file picker (ISO or folder), F1 menu, settings, renderer,
mixer, crash reports, Android project and the 43 xboxrecomp patches, renamed
for Mad Dash (`MadDashRecomp`, `maddashrecomp.ini`, `MDR_*`, `mdr_*`). The
game recompiles, builds, plays its logo movies with sound and shows its
title screen and its menus as on the console, and takes controller input
(START opens Adventure, A its sign-up screen: cloud screenshots with
`MDR_INPUT_SCRIPT`); left idle it relaunches itself into the attract demo.
Its level loads (it relaunches its own XBE) reboot in place, in the same
window (`relaunch.c`, `mdr_guest_reboot`, patch 0047). Facts about the game and the boots so far:
`docs/maddash-facts.md`.

## How it fits together

- `tools/recomp.py`: the USA `default.xbe` (or the ISO) -> `gen/` through
  xboxrecomp and patches 0001-0048. Data sections `.data` and `BINKDATA` are
  excluded from the code sweep. 24 seeds from the first boot
  (`tools/mdr_seeds.json`).
- `src/xbe.c`, `tools/recomp.py`: title ID 45530001, the one known dump
  hash and the section layout (keep the two in step).
- `src/guest/`: bring-up (`guest.c`), the dispatch (`program.c`, one
  program; `maddash_names.h`), ICALL/UNIMPL diagnostics
  (`guest_manual.c`, Orta's game hooks removed), crash reports, vertex
  programs on the CPU (`vsh.c`), a placeholder `widescreen.c`.
- `src/hle/d3d_hle.c`: Mad Dash's Direct3D hooks, re-addressed from
  XbSymbolDatabase names and checked in its code: frame clock
  (BlockUntilVerticalBlank, vblank callback), CreateDevice (our window),
  Clear, Present (3925's end of frame; no Swap), KickOff, BlockOnTime;
  per-call tracing (`MDR_D3D_TRACE`, hooks in `d3d_calltrace.c` from
  `tools/gen_d3d_calltrace.py --symbols build/xdk_symbols.txt`); the draw
  path (Orta's, fed from Mad Dash's sources): a push-buffer reader
  (`pb_catch_up`) for what the game writes as NV2A methods itself (vertex
  arrays, texture stages, simple render states, culling), vertex programs
  from Load/SelectVertexShader's program memory, Begin/End through the
  program, `MDR_PB_TRACE=first-last`.
- `src/hle/dsound_hle.c`: DirectSound on our mixer, re-addressed from
  Orta's for DSOUND 3936 (mix-bin masks, light HRTF).
- `src/guest/stream_wait.c`: the file stream's consumer yields the guest
  CPU when its poll makes no progress (the console's preemption).
- xboxrecomp patches 0044 (IO status block order), 0045 (the title's
  main thread holds the guest CPU lock) and 0046 (cpuid, pushfd, popfd
  lifted: Bink's CPU check): the stream reader races, found in
  this game (`docs/maddash-facts.md`); 0048 (movsx from bp/sp was a
  zero extension: Bink's IDCT, the movies' wrong blocks). Worth sending upstream, and to
  Orta Recomp, whose main thread also ran unlocked. 0047: a warm reboot
  (the title boots again in the same process; old threads park at the
  guest CPU lock, a boot generation added to 0023's lock).
- `src/hle/input_hle.c`: Orta's input on SDL3, re-addressed for XAPI 3911
  (XGetDevices, XInputOpen/Close/GetState; XGetDeviceChanges runs on the
  masks it keeps current); no rumble (the game calls no XInputSetState).
- `src/hle/orta/`: Orta's originals, kept for reference (its wait trace
  is not ported), at **Orta's addresses**: not built, not
  scanned. `docs/hle-port.md` lists them.
- Not carried over: Orta's pd1 (second program), CRI/Sofdec and AIX (Mad
  Dash uses Bink), Orta's game hooks (widescreen, unlocks, difficulty, intro
  skip, launch guard), `CHANGELOG.md`.

## Next

2. Races play on Windows (Tiki Village): in-place level loads, sound
   effects (3D voices on the crosstalk mixbins), characters (no more
   tethers), 60 fps. Open there: music cut-outs (recheck now that the
   shaders run on the GPU; `MDR_DSOUND_TRACE=1` logs Process), the menus'
   ambience against the music.
3. A boot hung once on Windows (frame 2, `interp 1`, 6x): the main thread
   polls a file request with Sleep(0) (0x0018C620 loop: request +4 <
   wanted; pump 0x0017B290), the stream worker (0x00180D10) waits on its
   request semaphore (queue at +0x4C, the reads ReadFile-with-OVERLAPPED
   via 0x000AD2EB), so the request's completion was never seen. Not seen
   in 8 cloud boots. Suspect: RECOMP_ASYNC_IO's STATUS_PENDING against
   XAPI's OVERLAPPED handling. Instrument if it repeats.
4. Frame gen. ghosting: fixed in the cloud, to confirm on Windows. Two
   causes: with `fs_wait` (on from the start on AMD's driver) a draw whose
   bone registers jumped was left unblended (running characters, every
   frame); and a frame never shown on its own was blended from without
   its upload or vertex pre-pass. Check: `--interp=1 MDR_CAPTURE=2600
   MDR_CAPTURE_INTERP=1` without `MDR_NO_DRAW`, `_t000` against `_prev`.
5. Boot further (the cloud has a front-end subset of `media/`, uploaded by
   the owner; music `.str` files are not on the owner's listing), keep
   collecting `[ICALL] Failed`
   lines into `tools/mdr_seeds.json` (`"observed": true`). The 64 MB heap
   request at boot (docs/maddash-facts.md).
6. Mad Dash's data files: what the disc holds (for the folder check in
   `src/assets.c`), save format, its own widescreen if any.
7. The launcher-only build (no `gen/`) does not link: `overlay.c` is
   built only with the game code (also so in Orta Recomp).
8. Rename leftovers: comments that tell Orta's history were kept where they
   explain why code is as it is; `tools/xbe_match.py` still describes Orta
   vs JSRF; the Android project (`android/`) is renamed but untested.

## Testing

- Cloud: gdb (`thread apply all bt`) on a stuck run names the game
  functions; lavapipe (`apt-get install mesa-vulkan-drivers`),
  `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy`; `MDR_FAST=4`, `MDR_SCREENSHOT=dir`,
  `MDR_CAPTURE=frame`, `MDR_INPUT_SCRIPT` (from Orta; they need the D3D
  and input hooks).
- A lifted function against the original x86: `src/guest/bink_diff.c`
  saves guest memory around one call (MDR_BINK_DIFF=n), and Unicorn runs
  the same call from the XBE's code (found Bink's movsx bug, patch 0048).
- Before pushing C changes, check that they build for Windows:
  `x86_64-w64-mingw32-gcc -fsyntax-only` on the changed files (package
  gcc-mingw-w64-x86-64, the include paths from `build/dev/build.ninja`).
  `windows.h` defines `far`, `near` and other words as macros.
- Windows: build from a Developer PowerShell; `--log run.log`, not
  PowerShell redirection. F12 captures a frame, F11 dumps threads.
