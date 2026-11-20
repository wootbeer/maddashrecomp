# Re-addressing Orta Recomp's XDK hooks for Mad Dash

`src/hle/orta/` holds Orta Recomp's Direct3D, DirectSound, input and wait
hooks as Orta had them: Orta's addresses, XDK 4928. They are not compiled
and not scanned for overrides (`tools/recomp.py` skips the folder). Each
file goes back to `src/hle/` once every address in it is Mad Dash's.

## Done

`src/hle/d3d_hle.c`: BlockUntilVerticalBlank 0x0023B240, Direct3D_CreateDevice
0x0023BFB0, Clear 0x002418B0, Present 0x002473A0 (in place of 4928's Swap),
CDevice::KickOff 0x002405D0, D3D_BlockOnTime 0x00240720. The draw path is
the next part, with a reader for the NV2A methods the game writes itself
(`docs/STATE.md`).

## Steps

1. Get Mad Dash's XDK symbols: build Cxbx-Reloaded's XbSymbolDatabase from a
   local clone and run its CLI on your own `default.xbe`, output to a file
   outside the repository (`build/` is ignored):

   ```
   XbSymbolDatabaseCLI path\to\default.xbe > build\xdk_symbols.txt
   ```

   Lines look like `D3DDevice_SetTexture = 0x...` (prefixed by library).
2. For each hook below, find the Mad Dash address of the same API function,
   check its `ret N` and how Mad Dash's own code passes its arguments
   (calling conventions and the `SetRenderState_*`/`SetTextureState_*`
   set changed between XDK builds), then change the address
   in the hook, its `_gen` extern and its comment.
3. Data addresses: `D3D__RenderState`, `D3D__TextureState`, `D3D__pDevice`,
   the back buffer, `RS_ENCODE`, `PROG_VA(...)` (drop the macro: there is
   one program). Find each in the matching Mad Dash function.
4. Move the file back to `src/hle/`, rerun `tools/recomp.py`, build.
5. `tools/gen_d3d_calltrace.py --symbols build/xdk_symbols.txt` makes the
   tracing hooks (`src/hle/d3d_calltrace.c`) once `d3d_hle.c` is back.

Order, as for JSRF and Orta: the frame clock (BlockUntilVerticalBlank) and
Swap, CreateDevice/Clear/draws, textures, DirectSound, XInput, vertex
programs, pixel shaders, render targets, lighting, stencil, fog.

## The hooks (Orta address -> API)

Direct3D (`d3d_hle.c`): Clear 0x002B53F0, Swap 0x002BAFC0, BlockOnTime
0x002B30B0, GetBackBuffer2 0x002ADF50, SetRenderTarget 0x002AD9C0,
CopyRects 0x002AE0C0, SetMaterial 0x002ADBE0, LightEnable 0x002AE6F0,
SetTexture 0x002AE810, SetStreamSource 0x002B19A0,
SetVertexShaderConstant1 0x002B1770, SetVertexShaderConstant4 0x002B17D0,
SetVertexShaderConstantNotInline 0x002B1960, SetShaderConstantMode
0x002B1B10, CreateVertexShader 0x002B1630, SetPixelShader 0x002BB5D0,
SetIndices 0x002AE9C0, SetVertexShader 0x002B1D70, SetTransform 0x002ADAA0,
SetViewport 0x002AE3B0, DrawVertices 0x002AF2B0, DrawIndexedVertices
0x002AF350, DrawVerticesUP 0x002AF010, DrawIndexedVerticesUP 0x002AF170,
SetGammaRamp 0x002AD9E0, Begin 0x002AF660, SetVertexData2f 0x002AF5C0,
SetVertexData4f 0x002AF600, End 0x002AF6A0; plus Orta-only ones (the
device setup 0x002B3510, 0x002B2F10, 0x002ADCB0, 0x002BB7D0, 0x002AF710,
Orta's game code 0x00279D30/0x00279E50): read the comments above each.

DirectSound (`dsound_hle.c`, 82 hooks): DirectSoundCreate,
DirectSoundCreateStream, DirectSoundDoWork, DirectSoundUseFullHRTF, the
IDirectSound, IDirectSoundBuffer and IDirectSoundStream methods (the
comments name each). DSOUND 3936 is far from 4928's: expect other method
layouts.

Input (`input_hle.c`): XInitDevices, XGetDeviceChanges, XInputOpen,
XInputClose, XInputGetCapabilities, XInputGetState, XInputSetState,
XInputPoll.

Waits (`wait_trace.c`): XAPI's wait functions (WaitForSingleObject and
friends), for F11's thread list.
