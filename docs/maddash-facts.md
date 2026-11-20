# Mad Dash Racing: facts

What is known about the game, for the port. Addresses are the USA
default.xbe's (the only one recognised).

## The executable

- Title: Mad Dash Racing, title ID 45530001 (ES-001), North America,
  disc 0, version 1. Retail; entry 0x000ADEA6, kernel thunk 0x00294B20.
- Built with XDK 3936 (a launch-window XDK, older than JSRF's 4134 and
  Orta's 4928). Libraries: XAPILIB, XBOXKRNL, LIBC, XGRAPHC, D3DX8 3911;
  D3D8 3925; DSOUND 3936 (all QFE 1).
- Sections (`tools/xbe_info.py`):

  | Section | Address | Notes |
  |---|---|---|
  | .text | 0x00011000 | game code, 2.27 MB, to 0x0023B0B0 |
  | D3D | 0x0023B0C0 | Direct3D 8 |
  | BINK, BINK32, BINK32A, BINK16, BINK4444, BINK5551, BINK16MX, BINK16X2, BINK16M, BINK32MX, BINK32X2, BINK32M | 0x0024C100-0x0026498C | Bink video decoder and its blitters |
  | XGRPH | 0x002649A0 | XGraphics |
  | D3DX | 0x00266AA0 | D3DX8 |
  | DSOUND | 0x00282EC0 | DirectSound |
  | XPP | 0x0028D7E0 | XAPI peripherals (input) |
  | .rdata | 0x00294B20 | |
  | .data | 0x002F8FE0 | marked executable, is data |
  | BINKDATA | 0x003C0C20 | marked executable, is data |
  | DOLBY | 0x003C4E80 | the APU's Dolby DSP code, not x86 |
  | $$XTIMAGE, $$XSIMAGE | 0x003CBC20, 0x003CE420 | title/save images |

- Movies are **Bink**, not CRI Sofdec (Orta, JSRF). Audio is DirectSound
  with its own music files (strings mention `bldallmusic`).
- Engine strings: built as `Xbox_GoldMaster`; a "bigfile" of cooked models,
  AnimReelResource animations, XBFont fonts, libjpeg, libpng and zlib.

## Recompiling

- `tools/recomp.py` (xboxrecomp + patches 0001-0043): 23,878 functions, 0
  failed, about 2.06M lines in 96 files, 60 unresolved call targets stubbed.
  The missed-target pass seeded 574, then 67 more.
- The sweep misses functions reached only through tables: 22 in the D3D
  section (device setup, 0x0023C220-0x0023D200) and 2 in XPP (0x0028D9AC,
  0x0028E272), seen as `[ICALL] Failed` at boot, now in
  `tools/mdr_seeds.json` (23,902 functions).

## First boot (no XDK hooks, default.xbe only)

- Starts a worker thread (0x000ACF4B), makes `TDATA\45530001` and
  `UDATA\45530001` (TitleMeta.xbx, TitleImage.xbx, SaveImage.xbx), formats
  the cache partition.
- Asks the heap for 64 MB once (`xbox_HeapAlloc: out of memory`, requested
  67108864): to look into; the game goes on.
- Game data is under `D:\media`: first `media\game.erg`, then
  `media\gogames\gac\english.dir` and fonts in
  `media\gogames\gac\xdata\fonts\` (`courier_new_14.tga`, XBFont). The
  game folder check (`src/assets.c`) asks for `media` beside default.xbe.
- Then the real XDK Direct3D runs (`out dx, al` at 0x00243891, `wbinvd` at
  0x002415A3, interrupt vector 3 -> 0x00242950) and waits on the GPU: the
  end of the road until the D3D hooks are re-addressed.
