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

## Direct3D (D3D8 3925)

- XDK symbols come from XbSymbolDatabase on the player's XBE (289 found;
  kept in `build/xdk_symbols.txt`, never committed).
- The device is a static struct at 0x00248E90; `D3D_g_pDevice`
  (0x0024BA20) points at it. Its first word is the push-buffer write
  cursor; +0x2304 the GPU channel registers (DMA PUT +0x40); +0x24EC the
  vblank callback, +0x24E8 the swap callback, +0x24F0 the vblank event.
- `D3D_g_RenderState` 0x0024BE88; deferred render states from 0x0024BFD0
  (FogEnable, index 82), complex ones from 0x0024C058 (PSTextureModes, 116;
  CullMode is 0x0024C084, 127); deferred texture states 0x0024BC88.
- 3925 has `Present(4 args, ret 0x10)` and no `Swap`;
  `SetVertexShaderConstant` (ret 0xC, ecx in use) instead of 4928's
  Constant1/4/NotInline. No SetStreamSource, SetIndices, DrawVerticesUP,
  SetMaterial, LightEnable, GetBackBuffer, CopyRects or SetGammaRamp were
  found as library functions.
- **The game writes NV2A methods itself.** Its renderer sits at about
  0x001E7000-0x001F1000. 0x001E7B10/0x001E7C00 end, run and begin a push
  buffer each frame (the frame is recorded, then run), then Present. The
  display-list player 0x001ECBA0 (opcodes 0-0x11, jump table 0x001ED290),
  with 0x001ECAD0 and 0x001ECB30, writes through the device's cursor
  (0x00248E90): vertex arrays (0x1720 offsets, 0x1760 formats), texture
  stages (0x1B00-0x1BCC), 0x0300, 0x033C, 0x1840. These are the only game
  functions that write it. They call D3D for `DrawIndexedVertices` (2 call
  sites), `DrawVertices` (1), `Begin`/`End`, `SetVertexShaderConstant`
  (11), `SetTexture` (8), `SetRenderState_Simple` (12, from 0x001F0720
  and 0x001F0E50). Vertex streams therefore never pass through a D3D call.

## Boot with the D3D frame hooks (default.xbe only)

- CreateDevice: back buffer 640x480, format 0x7, presentation interval 1;
  two frames are presented.
- Then it reads `media\gogames\gac\fonts\{debug,font24,plain16}\*.fnt`, string
  tables under `objects\player\`, `levels\ui\`, `objects\glueworld\dialogs\`,
  looks for saves `TDATA\45530001\GOP4.dat`, `GRK3.dat`, and without the
  data returns to the dashboard (HalReturnToFirmware 2).

## Streaming and threads

- Files load through a stream reader (vtable 0x002B6980): a 256 KB ring
  of 8 overlapped 32 KB reads (state at +4: 2 reading, 3 done, 5 at end,
  6 failed; reads queued +0xC4, collected +0xC8, filled to +0x10). A worker
  thread (0x00180D10, object 0x0033A368) takes requests from a semaphore
  (`CreateSemaphore(0, 0x7FFFFFFF)`) and reads them at time-critical
  priority; the consumer (0x00180800) spins on the reader's slot 3
  (0x001814F0) until the filled position moves, with no kernel call while
  reads are in flight. Completion is judged from the OVERLAPPED alone
  (0x00180C00: queued mark at +0x10, then Status != STATUS_PENDING).
- Two kernel-layer faults broke it, fixed in xboxrecomp patches:
  0044 (the IO status block's Information written before its Status) and
  0045 (the title's main thread, run inline by the first
  PsCreateSystemThreadEx, now holds the guest CPU lock; before, it ran
  beside every other guest thread, and the reader's worker and consumer
  raced on the OVERLAPPED). With the lock real, the consumer's spin needs
  a yield (`src/guest/stream_wait.c`).

## Boot with the game's data (front-end subset, no sound archive)

- Loads `Code.bgx`, `objects\list.txt`, all of `levels\maddash\maddash.bgx`
  (the front-end world; its material builder logs "Material Build Error
  tCount 3 psCount 2 made 1" and "Pixelshader leaves texture 2 unused!"
  12 times each), `uiglue.bgx`, `levels\ui\gluehud.bgx`; asks for
  `sounds\music\xbox\18_rollitup.str` (not on the owner's listing of the
  disc's `media`); opens `movies\Eidos.bik` and `movies\CD_Logo.bik`, the
  boot logos (Bink's own CPU check, `cpuid`/`pushfd` at 0x0025AC75, runs).
  Works without `sounds\archive.rkv` ("Failed to open archive file").
- Then DirectSound's `CMcpxAPU_Commit3dSettings` (0x00284D94) calls through
  a junk pointer for ever (from 0x00284F2F, its voice list at +0x118 never
  set up): the real DSOUND without the APU. DirectSound is the next hook
  set.

## DirectSound (DSOUND 3936)

- `src/hle/dsound_hle.c`: Orta's, 36 entry points re-addressed (every
  `ret N` the same as 4928's). 3936's differences: mix bins are masks (bit
  n = mix bin n) in DSBUFFERDESC (+16, `dwMixBinMask`), DSSTREAMDESC (+20)
  and SetMixBins; a 3D voice always gets 0x7C0, mask 0 a default by channel
  count (table 0x0028BF68: mono 0x3F, stereo 0x3). The light HRTF
  (`DirectSoundUseLightHRTF`; pairs at 0x0028C118, azimuth only). The
  stream vtable is 0x002BE5A8.
- The game: one ADPCM buffer per sound (DSBUFFERDESC built at 0x00153B87),
  music streams (DirectSoundCreateStream at 0x001512BC, 3 packets), the DSP
  image `soundBins\dsstdfx.bin`, two I3DL2 listener presets.
- Bink's audio (0x00259CD0-0x0025A680) makes a 0-byte buffer, gives it its
  own memory with SetBufferData and fills it with Lock/Unlock.

## Self-relaunch to load a level

- Mad Dash loads a level by relaunching its own default.xbe
  (XLaunchNewImage, launch data "lev..lod" with the level path, e.g.
  `ruins/ruins`), as HalReturnToFirmware(2). `src/guest/relaunch.c`
  boots it again in place (patch 0047's warm reboot, `mdr_guest_reboot`):
  the window, GPU device and sound output stay; memory, the kernel layer
  and our D3D/DirectSound/input state start over; the old boot's threads
  park. The attract demo (after the title sits idle) goes this way, to
  `levels\oasis\oasis01.bgx` in the cloud run. `MDR_REBOOT_AT=frame,...`
  reboots in place without launch data, as a test.

## Bink

- Movies play (Eidos and Crystal Dynamics logos, ~30 fps). Bink's CPU
  check (pushfd/popfd/cpuid at 0x0025AC75) picks its decoder routines
  into 0x0031541C; with the three as lifter no-ops it was left null and
  called a million times. Patch 0046 lifts them (cpuid answers as the
  Xbox's Pentium III): no null calls since.

## A title-screen frame (frame 1400, `MDR_D3D_TRACE`, `MDR_PB_TRACE`)

- 459 D3D calls: 76 DrawIndexedVertices (triangle strips 6 and lists 5,
  16-bit indices in the model data), 3 Begin/End batches (56 vertices of
  SetVertexData2f/4f/Color each), 110 SetVertexShaderConstant, 9
  LoadVertexShaderProgram + SelectVertexShader (programs loaded into
  program memory by address, from tables in .text such as 0x001F38B0; not
  CreateVertexShader handles), 7 SetPixelShaderProgram (+SetPixelShader
  0x002492F4), one SetVertexShaderInput, render states through
  SetRenderState_Simple/Deferred/CullMode/Stencil*, SetScissors. No
  SetTexture and no SetStreamSource.
- What the display-list player writes, read at each draw: vertex arrays,
  e.g. attribute 0 format 0xE35 (stride 14, 3 x NV2A type 5, 16-bit
  integers), 1: 0xE40 (4 bytes, type 0, a D3DCOLOR), 2: 0xE25 (2 x type
  5), at physical offsets; texture stages 0-2, e.g. offset 0x00BD9480,
  format 0x07780C2A (2D, colour format 0x0C DXT, 8 levels, 128 x 128),
  address 0x00010101, filter 0x02063F01.

## Direct3D 3925 numbering and shadows (for the draw port)

- Render states (`D3D_g_RenderState` 0x0024BE88, `SetRenderStateNotInline`
  0x0023C2A0): simple 0-81, written as the NV2A method in the table at
  0x002A29A8 (full headers, 0x0004xxxx); deferred 82-115 (dirty bits from
  0x00247CB0 into the device's +8); complex 116 on, by setter (table
  0x00247BB8).
  - 57-81 as Orta's 4928 enum: ZFUNC 57 (0x354), ALPHAFUNC 58, ALPHABLEND
    59, ALPHATEST 60, ALPHAREF 61, SRCBLEND 62, DESTBLEND 63, ZWRITE 64,
    DITHER 65, SHADEMODE 66, COLORWRITE 67, STENCILZFAIL 68, STENCILPASS 69,
    STENCILFUNC 70, STENCILREF 71, STENCILMASK 72, STENCILWRITEMASK 73,
    BLENDOP 74, BLENDCOLOR 75, SWATHWIDTH 76, POLYOFFSET slope 77 / offset
    78, POINT/WIREFRAME/SOLID OFFSET ENABLE 79/80/81. 0-56: the pixel
    shader registers, as in Orta.
  - Deferred = Orta's minus 10 (checked: FOGENABLE 82; LazySetPointParams
    reads POINTSIZE..POINTSIZE_MAX at 106-113): LIGHTING 92, COLORVERTEX
    95, material sources 100-103, AMBIENT 105.
  - Complex, from the setters' stores: PSTEXTUREMODES 116, VERTEXBLEND 117,
    FOGCOLOR 118, FILLMODE 119, BACKFILLMODE 120, TWOSIDEDLIGHTING 121,
    NORMALIZENORMALS 122, ZENABLE 123, STENCILENABLE 124, STENCILFAIL 125,
    FRONTFACE 126, CULLMODE 127, TEXTUREFACTOR 128, ZBIAS 129, LOGICOP 130,
    EDGEANTIALIAS 131, MULTISAMPLEANTIALIAS 132, MULTISAMPLEMASK 133,
    MULTISAMPLETYPE 134, SHADOWFUNC 135, LINEWIDTH 136, DXT1NOISE 137, YUV
    138, OCCLUSIONCULL 139, STENCILCULL 140.
- SetVertexShaderConstant(Register, pData, Count), stdcall `ret 0xc`:
  NV2A slot = Register + 96; unless the device is pure (bit 0x10 of the
  device's +0xC, from CreateDevice's BehaviorFlags) the library copies the
  constants to the device at +0xCC0 + 16 * slot (0x00249B50 + 16 * slot),
  then writes them to the push buffer (method 0x0B80).
- Texture state read from the push buffer is the NV2A's: OFFSET (physical),
  FORMAT (the D3D resource's format word: Orta's xtex reads it), ADDRESS
  (u/v/p wrap modes, 1 = wrap), CONTROL0/1 (enable, pitch << 16 for
  linear), FILTER (min bits 16-23, mag 24-27: 1 nearest, 2 linear, 3-6
  with mips), IMAGE_RECT (w << 16 | h, linear textures).

## Drawing (what the port found)

- Besides vertex arrays and textures, the display list writes the simple
  render states itself (opcode 5: nine methods from 0x033C, blend and
  alpha; 0x0300/0x0304 enables) and culling (0x308/0x39C/0x3A0) is changed
  through methods too; the reader maps them back into D3D's table.
- Movies: an immediate-mode quad (Begin/End, QUADLIST, through a 4-
  instruction program loaded each frame at the next 4 slots of program
  memory) with the Bink frame as a linear X8R8G8B8 640 x 480 texture set
  through SetTexture.
- Title and menu: about 80 draws a frame; the logo is a swizzled
  A8R8G8B8 512 x 256 texture (format 0x0891062A).

## Characters (skinning)

- The characters' vertex program (27 instructions) blends two bones a
  vertex: v3 is PBYTE4 (bone A, bone B, weight, 0), each bone's first
  constant row v3 * c191.w (256) through ARL, its matrix c[a0..a0+2], the
  view-projection in c185-c188. It owns the whole 192-constant file:
  bones run through c[57-59], where the XDK's compiled programs keep the
  viewport transform. Writing the viewport there for every program sent
  bone 20's vertices millions of units off ("tethers"); it is now written
  only for programs that read c58/c59 directly (d3d_hle.c reads_viewport).
- The game records part of each frame into push buffers (Begin/
  EndPushBuffer 0x002466B0/0x00246960; the ring cursor saved at
  dev+0x400) and the ring wraps with a jump (MakeSpace 0x00240810):
  pb_catch_up reads across both.

## Sound mixing

- 3D voices (buffers and DSSTREAMCAPS_CTRL3D streams: the ambience and
  most effects) get mixbins 6-9 (XTLK_FL/FR/BL/BR) and 10 (I3DL2); the
  mixer takes 6-9 as the corner speakers. Mono 2D voices default to all
  six speakers (0x0028BF68); on stereo output a voice with both front bins
  plays from those only.
