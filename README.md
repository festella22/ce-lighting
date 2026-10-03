# ce-lighting

**Better lighting for Halo: Combat Evolved on the Master Chief Collection.** A
[Spark](https://github.com/KodyJKing/spark) mod that adds lighting Halo CE never had, driven by
each level's own sun.

> **Work in progress.** The first effect, god rays, is being brought up now.

## What it does

- **Each level's real sun.** Halo CE keeps every level's sun (direction, color, power) in the
  level's sky. ce-lighting reads it, so the lighting matches every level with no hand tuning.
- **God rays.** Light shafts streak out from the sun wherever there's open sky around it, so
  cliffs, Forerunner arches and the ring's edge cut beams out of the light. Screen-space, from
  Halo's own depth buffer (GPU Gems 3, ch. 13).
- **F7** turns it on and off, to compare.

Planned: ray-traced sun shadows and ambient occlusion from the level's real geometry (not just
what's on screen), and proper shadows for characters and vehicles.

## Install

You need Halo: The Master Chief Collection on Steam with Halo: CE. **Offline only, with
anti-cheat disabled. Never use mods online.**

1. Build it (below), or grab a release when there is one.
2. Put `ce-lighting.dll` in `<MCC>\MCC\Binaries\Win64\mods\` and load Spark into MCC (started
   with anti-cheat disabled).
3. Load any Halo CE level.

Works alongside [HaloCraft](https://github.com/festella22/halocraft) (Minecraft inside Halo CE).

## Building

Visual Studio 2022 Build Tools (C++) and [Premake 5](https://premake.github.io/).

```powershell
git clone --recursive https://github.com/festella22/ce-lighting
cd ce-lighting
premake5 vs2022
MSBuild.exe ce-lighting.sln /t:ce-lighting /p:Configuration=Release /p:Platform=Win64
```

`tools\reload.ps1` builds it and (re)loads it into a running MCC.

## Credits

- [Spark](https://github.com/KodyJKing/spark) by KodyJKing: the Halo CE MCC mod loader and SDK.
- The light scattering method: Kenny Mitchell, "Volumetric Light Scattering as a Post-Process",
  GPU Gems 3, ch. 13.

MIT, see [LICENSE](LICENSE). Fan project, not affiliated with Microsoft, 343 Industries, Halo
Studios or Bungie.
