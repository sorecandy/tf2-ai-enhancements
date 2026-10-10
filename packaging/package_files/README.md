# TF2 AI Enhancements

**NVIDIA DLSS** AI upscaling, **AMD FSR 3 Frame Generation** and optional **NVIDIA DLSS 5 Neural Rendering** for Titanfall 2 on Northstar. Everything is set on one page: **Settings > Video > AI Enhancements**.

A small Northstar plugin loads the bundled ReShade and the mod's add-ons from this package. You do not install ReShade yourself.

## Requirements

- Titanfall 2 on PC with **Northstar 1.31.9** (tested). The game runs on DirectX 11; so does this mod.
- **Anti-aliasing = TSAA** in Settings > Video. The upscaler replaces the TSAA pass.
- **An NVIDIA GeForce RTX GPU** and a current NVIDIA driver. The upscaler is DLSS, and frame generation needs the upscaler on. On other GPUs the mod does nothing and the game keeps its own anti-aliasing.
- **Frame generation** also needs a laptop with hybrid graphics: the screen driven by the integrated GPU while the RTX GPU renders the game. See [Frame generation limits](#frame-generation-limits).
- **DLSS 5 Neural Rendering**: a GeForce RTX 50 series GPU plus two extra files (see [DLSS 5](#dlss-5-optional)).
- Tested on one machine: RTX 5070 Laptop GPU (driver 617.14) with a Radeon 780M driving a 1920x1200 165 Hz screen.

AMD FSR 3 *upscaling* is not part of 1.0.0. The FSR 3 in this mod is frame generation only.

## Install

- **r2modman, Thunderstore Mod Manager or Viper**: install it like any other mod.
- **By hand**: extract the zip into `Titanfall2\R2Northstar\packages\<Author>-TF2_AI_Enhancements-1.0.0\`. Keep that folder name; Northstar skips package folders named differently.

If you have an older hand-installed copy of the menu mod (`R2Northstar\mods\TF2NR.GraphicsToggles`), delete it. Northstar disables a mod that is installed twice.

## The AI Enhancements page

Settings > Video > **AI Enhancements**. Changes apply at once.

| Row | Options | What it does |
|---|---|---|
| Upscaler | Off / DLSS | DLSS renders fewer pixels and rebuilds a sharp full-resolution image in place of TSAA. Off = the game's own TSAA. |
| Quality | DLAA / Quality / Balanced / Performance / Ultra Performance | Render resolution: 100, about 67, 58, 50 and 33 percent. Lower = more fps. DLAA = native resolution, best anti-aliasing, no speed-up. |
| DLSS 5 | Off / On | DLSS 5 Neural Rendering: relights and re-details the image inside DLSS. Needs the optional files. Costs a lot of fps. |
| DLSS 5 Adjustments | page of sliders | Strength, tone, structure, skin, style and look controls. Only change the image while DLSS 5 is On. |
| Frame Generation | Off / On | AMD FSR 3 frame generation: one generated frame between every two rendered frames. Needs Upscaler = DLSS. |

Everything starts Off on a fresh install. With Upscaler Off and DLSS 5 On (and the DLSS 5 files installed), DLSS keeps running at DLAA, because DLSS 5 runs inside DLSS.

## Frame generation limits

- It works only when the **screen is driven by a different GPU than the one rendering the game**: laptops with hybrid graphics (NVIDIA Optimus / Advanced Optimus in hybrid mode). It **stays off** on desktops, on laptops set to dGPU-only (MUX) mode, and on an external monitor wired to the RTX GPU.
- It **stays off while the Steam, Discord, RTSS or OBS overlay is hooked into the game**, and while NVIDIA Smooth Motion is on.
- Turn **V-Sync off**.
- It pauses by itself in menus and while the real frame rate is **under 40 fps** or **over half the screen's refresh rate** (82.5 fps at 165 Hz; never above 83 fps). Use `fps_max` to stay inside that window.
- While it runs, it shows its frames in its own layer on top of the game window, so overlays drawn into the game's image (for example the EA app overlay) are covered.
- It costs some real frames (about 25 % on the test laptop) and adds about **12 ms of input latency**.
- Alt+Tab pauses it until you return to the game.

`plugins\lib\tf2-fg.log` says why it is off (`no presenter: ...`).

## DLSS 5 (optional)

DLSS 5 Neural Rendering needs two files that this package **does not include** (neither comes with a licence that allows us to redistribute it):

| File | What it is | Tested version |
|---|---|---|
| `renodx-dlss5.addon64` | the RenoDX DLSS 5 add-on for ReShade (closed-source community build) | 7.0.0-rc8, SHA-256 `ff8b9738738265e09f01a1c470a0cb0a59eb021b2b23797c3df7cea722a724b6` |
| `nvngx_dlssnr.dll` | NVIDIA's DLSS 5 Neural Rendering runtime | 310.8.0.0, NVIDIA-signed, SHA-256 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |

Get them from the RenoDX DLSS 5 add-on's own release page; its instructions name the matching `nvngx_dlssnr.dll`. Use only an `nvngx_dlssnr.dll` whose digital signature (file Properties > Digital Signatures) says NVIDIA Corporation and is valid: modified copies circulate.

To install them:

1. Open the package's `plugins\lib\` folder: `...\R2Northstar\packages\<Author>-TF2_AI_Enhancements-1.0.0\plugins\lib\`. Your mod manager's "open profile folder" option leads to the profile that contains `R2Northstar`. The folder already holds `ReShade64.dll`, `ReShade.ini` and the `*.addon64` files.
2. Put **both** files into that folder, next to the other `.addon64` files. ReShade loads every add-on in this folder, and renodx-dlss5 looks for `nvngx_dlssnr.dll` next to itself.
   Keep the exact file name `renodx-dlss5.addon64` (rename a versioned download): the mod finds DLSS 5 by that name, and under any other name the DLSS 5 row does nothing.
3. Restart the game. Set Upscaler = DLSS, then DLSS 5 = On.

A mod-manager update replaces the package folder: copy the two files in again after updating.

### DLSS 5 on Linux (Proton)

The RenoDX DLSS 5 add-on does not run under Proton. There, use **tf2-nr-linux** instead
([github.com/Milofax/tf2-nr-linux](https://github.com/Milofax/tf2-nr-linux), GPL, DynamicShaderFrameGen's NR runtime):

1. Put `tf2-nr-linux.addon64` and the same `nvngx_dlssnr.dll` (310.8.0.0, NVIDIA-signed) into `plugins\lib\`.
2. Leave `renodx-dlss5.addon64` out.
3. Set Upscaler = DLSS and DLSS 5 = On, as above. The DLSS 5 Adjustments sliders apply live.

`tf2-dlss-sr.log` then says `tf2-nr-linux route: active`. Its own log is `tf2-nr-linux.log`.

## You already have ReShade in the game folder?

If a ReShade `dxgi.dll` (or `d3d11.dll` and similar) sits in `Titanfall2\bin\x64_retail\` or in `Titanfall2\` itself, the plugin loads **nothing**, because two ReShades cannot run in one game. `plugins\lib\tf2-ai-loader.log` then says `SKIP: ReShade is installed in the game folder` and names the file. Either:

- remove your own ReShade to use the bundled one, or
- keep yours (it must be a ReShade 6.x build **with add-on support**, in `bin\x64_retail`). Copy `tf2-dlss-sr.addon64`, `tf2-nr-bridge.addon64`, `tf2-fg.addon64` and `nvngx_dlss.dll` from the package's `plugins\lib\` next to your ReShade, and copy the `[TF2DLSSSR]`, `[TF2FG]`, `[TF2NRBridge]` and `[RenoDX.DLSS5]` sections of the package's `ReShade.ini` into yours.

A ReShade `dxgi.dll` only in `Titanfall2\` (not in `bin\x64_retail`) is usually left over from running the ReShade installer on the wrong exe. Northstar normally does not load it, but the plugin still steps aside to be safe: delete it to use the bundled ReShade.

Another (non-ReShade) `dxgi.dll` in those folders is left alone: the plugin then loads ReShade in hook mode instead, where frame generation may stay off.

## Launch options

- `-tf2ai_off`: the plugin loads nothing for that session (the menu stays).
- `-tf2ai_hookmode`: troubleshooting only. Loads ReShade in hook mode instead of as a DXGI proxy. Frame generation may stay off in this mode.

## Uninstall

**Remove** the mod in your mod manager, or delete the package folder. Disabling it may not be enough: Northstar loads a package's plugin even when the package's mod is disabled, so the plugin and ReShade may keep loading. To keep the mod installed but inactive, launch with `-tf2ai_off`.

The mod writes only inside its own package folder, plus its `tf2nr_*` settings in the game's `profile.cfg` (harmless without the mod).

## Logs

All in the package's `plugins\lib\` folder: `tf2-ai-loader.log` (what the plugin did and why), `ReShade.log`, `tf2-dlss-sr.log` (upscaler), `tf2-fg.log` (frame generation), `tf2-nr-bridge.log`. Include them when you report a problem.

## Known issues

- ReShade's input hook is disabled on purpose (it breaks Titanfall 2's mouse look), so the ReShade overlay cannot be used with the mouse. Use the in-game menu.
- DLSS 5 costs a lot of fps (about 44 fps at DLAA + DLSS 5 on an RTX 5070 Laptop GPU at 1920x1200).
- Settings > Mod Settings: the first click on a button there may only move the keyboard focus out of the search box (Northstar behaviour); click again. Settings > Video > AI Enhancements opens with one click.
- With the DLSS 5 files installed, typing `tf2nr_upscaler` into the developer console has crashed the game in our tests (not seen without them). Change the upscaler in the menu, not in the console.
- With the DLSS 5 files installed, the mod switches DLSS 5 by pressing F6 for a few frames. If the game closes or crashes in that moment, Windows can keep F6 held down; press and release F6 once to clear it.
- ReShade's add-on build is meant by its author for games without anti-cheat. Northstar has none. Use at your own risk.

## Credits and licences

- This mod uses **NVIDIA DLSS**. NVIDIA, GeForce RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation.
- **AMD FidelityFX Super Resolution 3** frame interpolation, from the DirectX 11 port of the FidelityFX SDK. MIT licence.
- **ReShade** by crosire (Patrick Mours). BSD 3-Clause licence.
- **Northstar** by the R2Northstar team (required, not included).

Full terms: `THIRD_PARTY_NOTICES.txt` and the `licenses\` folder.
