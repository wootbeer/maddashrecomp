# Current state

v0.0.1, bring-up. The repository was set up from Orta Recomp (v0.1.15):
launcher, game-file picker (ISO or folder), F1 menu, settings, renderer,
mixer, crash reports, Android project and the 43 xboxrecomp patches, renamed
for Mad Dash (`MadDashRecomp`, `maddashrecomp.ini`, `MDR_*`, `mdr_*`). The
game recompiles, builds and boots into the real XDK Direct3D setup, where it
waits on the GPU (no hooks yet). Facts about the game and the first boot:
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
- `src/hle/orta/`: Orta's Direct3D, DirectSound, input and wait hooks, at
  **Orta's addresses**: not built, not scanned. `docs/hle-port.md` lists
  them and how to re-address them. Until then the game would run the real
  XDK code, which talks to the NV2A/APU hardware.
- Not carried over: Orta's pd1 (second program), CRI/Sofdec and AIX (Mad
  Dash uses Bink), Orta's game hooks (widescreen, unlocks, difficulty, intro
  skip, launch guard, video hooks), `CHANGELOG.md`.

## Next

1. XDK symbols for Mad Dash (XbSymbolDatabase CLI on the owner's XBE;
   could not be run in the cloud session), then re-address
   `src/hle/orta/` file by file (`docs/hle-port.md`), D3D first.
2. Boot with the whole disc (`media/`), keep collecting `[ICALL] Failed`
   lines into `tools/mdr_seeds.json` (`"observed": true`). The 64 MB heap
   request at boot (docs/maddash-facts.md).
3. Bink: decide between hooking Bink's own API (BinkOpen/BinkDoFrame/
   BinkCopyToBuffer) and letting the recompiled decoder run, with its
   output surfaces drawn by our D3D.
4. Mad Dash's data files: what the disc holds (for the folder check in
   `src/assets.c`), save format, its own widescreen if any.
5. The launcher-only build (no `gen/`) does not link: `overlay.c` is
   built only with the game code (also so in Orta Recomp).
6. Rename leftovers: comments that tell Orta's history were kept where they
   explain why code is as it is; `tools/xbe_match.py` still describes Orta
   vs JSRF; the Android project (`android/`) is renamed but untested.

## Testing

- Cloud: `MDR_NO_DRAW=1 MDR_FAST=4`, `MDR_SCREENSHOT=dir`,
  `MDR_CAPTURE=frame`, `MDR_INPUT_SCRIPT` (from Orta; they need the D3D
  and input hooks).
- Windows: build from a Developer PowerShell; `--log run.log`, not
  PowerShell redirection. F12 captures a frame, F11 dumps threads.
