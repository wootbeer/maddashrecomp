# Mad Dash Recomp: Mad Dash Racing (2001 Xbox) Recompiled and Port

You will need the game files from a licensed copy in order to play it.

A native PC (and later Android) port of Mad Dash Racing, made by statically
recompiling the Xbox game with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp),
with graphics, audio and input of its own on [SDL 3](https://libsdl.org).
Built the same way as, and from the code of,
[Orta Recomp](https://github.com/wootbeer/ortarecomp).

Unofficial, and not affiliated with the rights holders. **No game data is
included.** You need your own copy of the game to play this.

**Early bring-up.** The game recompiles and builds; it does not play yet.

## Planned features

The same launcher and F1 menu as Orta Recomp:

- Game file picker: your disc image (ISO) or an extracted game folder,
  checked against the known USA release
- Higher resolutions, widescreen, MSAA/FXAA, anisotropic filtering
- Frame interpolation above 60 fps, Lossless Scaling frame generation
- Color correction, CRT look, bloom
- 3D sound, 5.1, reverb, master volume
- Controllers with vibration, keyboard and mouse, rebindable; touch controls
  on Android
- Save slots, shader cache

## Game files

You need your own copy of **Mad Dash Racing (USA)**: the disc image, or the
extracted game folder with its `default.xbe`.

## Building

See [BUILDING.md](BUILDING.md).

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and
  contributors: the recompiler and the Xbox kernel layer this port runs on
- [SDL](https://libsdl.org): windowing, graphics, audio and input
- [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) (MIT) and
  [DXVK](https://github.com/doitsujin/dxvk)'s shader compiler (zlib), through
  `lib/lsfg` from BattleTanx: Global Assault Recompiled: Lossless Scaling
  frame generation
- [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) (MIT), for
  building that
- [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause):
  custom GPU drivers on Android
- [glslang](https://github.com/KhronosGroup/glslang) (BSD and others, see
  its license): the game's vertex programs made into GPU shaders of their own
- Orta Recomp and Concept of Love: the code this port started from

## License

The project's own code is GPL-3.0; see [COPYING](COPYING). Third-party
licenses are in [licenses/](licenses). The game itself remains the property
of its rights holders.
