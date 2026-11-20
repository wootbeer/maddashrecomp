# Current state

v0.0.1, bring-up. The repository was set up from Orta Recomp (v0.1.15):
launcher, game-file picker (ISO or folder), F1 menu, settings, renderer,
mixer, crash reports, Android project and the 43 xboxrecomp patches, renamed
for Mad Dash (`MadDashRecomp`, `maddashrecomp.ini`, `MDR_*`, `mdr_*`). The
game recompiles, builds, opens its window, loads its front-end world, plays
the logo movies with sound and runs its title at 59.94 fps (no picture
yet: no draw hooks); left idle it relaunches itself into the attract demo
(`relaunch.c`). Facts about the game and the boots so far:
`docs/maddash-facts.md`.

## How it fits together

- `tools/recomp.py`: the USA `default.xbe` (or the ISO) -> `gen/` through
  xboxrecomp and patches 0001-0043. Data sections `.data` and `BINKDATA` are
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
  Clear, Present (3925's end of frame; no Swap), KickOff, BlockOnTime.
- `src/hle/dsound_hle.c`: DirectSound on our mixer, re-addressed from
  Orta's for DSOUND 3936 (mix-bin masks, light HRTF).
- `src/guest/stream_wait.c`: the file stream's consumer yields the guest
  CPU when its poll makes no progress (the console's preemption).
- xboxrecomp patches 0044 (IO status block order) and 0045 (the title's
  main thread holds the guest CPU lock): the stream reader races, found in
  this game (`docs/maddash-facts.md`). Worth sending upstream, and to
  Orta Recomp, whose main thread also ran unlocked.
- `src/hle/orta/`: the rest of Orta's hooks (Direct3D draws and state,
  input, waits; its DirectSound is ported), at **Orta's addresses**: not built, not
  scanned. `docs/hle-port.md` lists them.
- Not carried over: Orta's pd1 (second program), CRI/Sofdec and AIX (Mad
  Dash uses Bink), Orta's game hooks (widescreen, unlocks, difficulty, intro
  skip, launch guard), `CHANGELOG.md`.

## Next

1. Bink's CPU check: cpuid and pushfd/popfd in the lifter (a patch), so
   Bink picks its decoder routines (0x0031541C is left null now).
2. Draws. Mad Dash's display-list player writes NV2A methods itself
   (vertex arrays, texture stages: `docs/maddash-facts.md`), so Orta's
   API-level capture alone sees no vertex streams. Plan: at each D3D draw
   hook, read the methods the game wrote since the last D3D call (from
   the last hook's cursor to the device's cursor, 0x00248E90) for those
   few methods only, into the draw state; then port Orta's draw path
   (SetTexture, SetVertexShaderConstant, CreateVertexShader,
   SetPixelShader, Draw(Indexed)Vertices, Begin/End, render states from
   0x0024BE88). A narrow reader of the game's own writes, not an NV2A
   emulator.
3. Input (XInputGetState 0x0028F244 and friends), from `src/hle/orta/`.
4. Boot further (the cloud has a front-end subset of `media/`, uploaded by
   the owner; music `.str` files are not on the owner's listing), keep
   collecting `[ICALL] Failed`
   lines into `tools/mdr_seeds.json` (`"observed": true`). The 64 MB heap
   request at boot (docs/maddash-facts.md).
5. Bink: decide between hooking Bink's own API (BinkOpen/BinkDoFrame/
   BinkCopyToBuffer) and letting the recompiled decoder run, with its
   output surfaces drawn by our D3D.
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
- Windows: build from a Developer PowerShell; `--log run.log`, not
  PowerShell redirection. F12 captures a frame, F11 dumps threads.
