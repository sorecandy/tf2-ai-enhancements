# TF2 AI Enhancements

Source code for the **TF2_AI_Enhancements** Northstar mod for Titanfall 2:

- **NVIDIA DLSS** upscaling (DLAA / Quality / Balanced / Performance / Ultra Performance) in place of the game's TSAA pass.
- **AMD FSR 3 frame generation** (hybrid-graphics laptops).
- Optional **NVIDIA DLSS 5 Neural Rendering** through the RenoDX DLSS 5 add-on (the files are user-supplied).
- An **AI Enhancements** page under Settings > Video.

A small Northstar plugin loads the bundled ReShade and these add-ons from the mod's own package folder, so a mod manager can install everything.

**Players:** install **TF2_AI_Enhancements** by **sorecandy** from Thunderstore with r2modman, Thunderstore Mod Manager or Viper. The package README ([packaging/package_files/README.md](packaging/package_files/README.md)) covers:
- requirements;
- frame-generation limits;
- DLSS 5 setup;
- conflicts with an existing ReShade;
- known issues.

## What is in this repository

| Folder | What |
|---|---|
| `plugin/` | `TF2AIEnhancements.dll`, the Northstar plugin. It loads ReShade from the package as a DXGI proxy (hook mode is the fallback). It stays idle when a ReShade already sits in the game folder, and it never faults. |
| `addons/tf2-dlss-sr/` | `tf2-dlss-sr.addon64` (ReShade add-on). It replaces TSAA with DLSS through NGX, pins the render size, prepares DLSS inputs (`prep_cs.hlsl`) and exports frame-generation inputs. It also runs DLSS 5 when renodx-dlss5 is loaded. |
| `addons/bridge/` | `tf2-nr-bridge.addon64` plus shared headers: menu convar access, DLSS 5 tuning, and the interop between the upscaler and frame generation. |
| `addons/fg/` | `tf2-fg.addon64`: FSR 3 frame interpolation, with its own presenter (a DirectComposition layer) for hybrid laptops. |
| `fsr/` | The static FSR library the frame-generation add-on links. It has our wrapper (`lib/`), a patched copy of the FidelityFX SDK DX11 backend (`sdk_patched/`, MIT, from optiscaler/FidelityFX-SDK-DX11 @ 9b04fa49), the patch itself (`patches/`) and the pre-compiled shader headers (`shaders/generated/`). |
| `menu/` | The Northstar mod `TF2NR.GraphicsToggles` (menus + script) and the generators that write its `.menu` files. |
| `packaging/` | `build_package.py`, `validate_package.py`, `set_team.py`, `install_package.py`, and the files that go into the package: README, notices, licence texts, the default `ReShade.ini` and the icon. |

## Building

Needs **Visual Studio 2022 Build Tools** (x64 C++ and the Windows SDK, which provides `fxc.exe`). Set `VCVARS` if `vcvars64.bat` is not in the default Build Tools location.

External SDKs, **not included here**:
- **ReShade** source ([crosire/reshade](https://github.com/crosire/reshade), 6.x). Set `RESHADE_SDK` to the checkout; the build uses its `include\` folder (the add-on API).
- **NVIDIA DLSS SDK** ([NVIDIA/DLSS](https://github.com/NVIDIA/DLSS), tested with v310.9.1), for `tf2-dlss-sr` only. Set `NGX_SDK` to the checkout, and `NGX_LIB` if `nvsdk_ngx_s.lib` is not in `lib\Windows_x86_64\x64\`.
  - It is under the NVIDIA RTX SDKs License, which does not allow putting it under an open-source licence, so it stays out of this repo.

Build order:

```bat
fsr\lib\build_lib.bat
addons\bridge\build.bat
addons\tf2-dlss-sr\build.bat
addons\fg\build.bat
plugin\build.bat
```

`packaging\build_package.py` then builds the Thunderstore zip:
- Pass every input explicitly: `--menu-dir`, `--sr-addon`, `--bridge-addon`, `--fg-addon`, `--plugin-dll` and `--nvngx-dlss`. Its defaults point at the author's working folders.
- It also needs `packaging\vendor\ReShade64.dll` (the ReShade 6.8 add-on build) and `packaging\vendor\nvngx_dlss.dll` (the NVIDIA DLSS runtime from the official NVIDIA/DLSS release). Neither is in the repo; `vendor\VENDOR.json` pins their SHA-256.

`menu\make_video_menu.py` rebuilds `video.menu` from the game's own `resource/ui/menus/video.menu`. That is Respawn's file: extract it from your game install (`englishclient_frontend.bsp.pak000_dir.vpk`) into `menu\vanilla\` first.

## Known issues

- FSR 3 *upscaling* is not in 1.0.0 (only FSR 3 frame generation). An FSR 3 upscaler backend exists but is held back until a crash is understood: with the DLSS 5 add-on loaded, typing `tf2nr_upscaler` into the console has crashed the game.
- With the DLSS 5 files installed, the mod toggles DLSS 5 by pressing F6 for a few frames. If the game closes during that press, F6 can stay held. Press F6 once to clear it.

## Licence

Our code is under the **MIT License** ([LICENSE](LICENSE)). Third-party parts keep their own terms:

- FidelityFX SDK files in `fsr/sdk_patched/` and `fsr/shaders/generated/`: MIT, Advanced Micro Devices ([fsr/LICENSE_FidelityFX.txt](fsr/LICENSE_FidelityFX.txt)).
- The released package also contains ReShade (BSD 3-Clause) and the NVIDIA DLSS runtime `nvngx_dlss.dll` (NVIDIA RTX SDKs License). Their terms are in [packaging/package_files/THIRD_PARTY_NOTICES.txt](packaging/package_files/THIRD_PARTY_NOTICES.txt) and [packaging/package_files/licenses/](packaging/package_files/licenses/). Neither binary is in this repository.
- `menu/TF2NR.GraphicsToggles/mod/resource/ui/menus/video.menu` is Titanfall 2's video menu with one added row, as Northstar menu mods do. Titanfall 2 is © Respawn Entertainment / Electronic Arts.

This mod uses **NVIDIA DLSS**. NVIDIA, GeForce RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation. AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices. Not affiliated with or endorsed by NVIDIA, AMD, Respawn Entertainment, Electronic Arts or the Northstar team.
