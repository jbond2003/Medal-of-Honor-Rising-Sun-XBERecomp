# Medal of Honor: Rising Sun - XBE Recomp

An unofficial native PC port of the original Xbox version of Medal of Honor: Rising Sun (USA), made with
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp). The game's x86 code is recompiled to C++ ahead of
time and the Xbox libraries run on Windows: Direct3D 12 for graphics, XAudio2 for sound.

> [!NOTE]
> This project does not include any game assets or game code. You must provide the files from your own legally
> acquired copy of the game. The XBE, the game data and the recompiled code (`generated/`) never go in this repository.

## Status

Playable on Windows from start to finish of a mission, with a controller or keyboard and mouse. Single player is
the goal; the co-op image, `MOH3RDC.xbe`, builds too (as `MOH3RDC.exe`) but is not a focus.

- The XDK 5455 libraries the game uses were added to the SDK (`sdk/xdk/5455`, in
  [the SDK fork](https://github.com/jbond2003/XboxXBERecomp)).
- Known issue: a rare crash while a level loads. If it happens, a crash dump and a copy of the log are written to
  `CrashDumps` beside the exe; please attach both to an issue.

## PC features

The original PC Options pages, the menu pointer, the software cursor, come from [neonoxd's MOHRS-XBERecomp](https://github.com/neonoxd/MOHRS-XBERecomp); see Credits.

- **PC Options inside the game's menus**: under Options on the main menu, and under Preferences on the pause
  screen. Display mode, render scale, frame rate limit (30, the game's own pacing, or 60), field
  of view, anisotropic filtering, FPS counter, mouse sensitivity, vertical sensitivity, invert,
  and Key Bindings, which opens the bindings over the screen.
- **Menu pointer**: the mouse moves a pointer over the game's menus; a left click picks, a right click backs out,
  the wheel scrolls or changes the row under the pointer. A click on the selected settings row changes its value.
  On the mission map a click picks a mission and a click on the picked one goes on. The pointer hides once you
  move a stick or press a key.
- **F10** still opens the overlay panel, which also has the texture tools and the key bindings. Settings are kept
  in `settings.toml` beside the exe, the bindings under `[keys]`.
- **Keyboard and mouse**: raw mouse input turns the view directly (not through the game's coarse stick), also
  while zoomed or on a mounted gun, slowed while zoomed in. The gameplay keys press the game's own actions, so they
  do the same whichever controller layout is picked in the game's options; a controller and the keyboard work
  together, and the controller does not rumble while the keyboard and mouse are in use. Defaults: WASD move, mouse
  look, left click fire, right click aim mode, F action, mouse wheel change weapon (sniper zoom while a scope is
  up), G grenade, Space jump, R reload, C or Ctrl crouch, Z center view, V
  or middle click melee, Alt walk, Escape pause, Tab back. WASD also moves through menus. A settings file from an
  older build has its old default keys (E/F action, Q/Mouse4 change weapon) moved to the new defaults once; keys
  you bound yourself stay.
- **Keyboard and mouse prompts**: while the keyboard and mouse are in use the in-game hints name the bound keys
  ("Press F to Open Door", "Press Escape to View Objectives") and the menus' bottom bar the keys in place of the
  A, B and dpad pictures ("ENTER CONFIRM", "BACKSPACE BACK"); the controller's come back when a controller is used.
- **Skippable movies**: a left click, Enter, Space, Escape or a controller face button skips any movie, the EA
  logo and the opening cinematic included. A skipped movie holds its last picture until what comes next is drawn.
- **Fast loads**: the game's loader no longer sleeps between its load steps, so levels and the menus come up in a
  moment.
- **Shader cache**: compiled shaders are kept in `shader_cache` beside the exe, so an effect only hitches the first
  time it is ever seen.
- **Performance logging**: put an empty `xbe_perf` file beside the exe, or set `XBE_PERF=1`, for once a second frame time breakdowns and present interval
  percentiles in the log, in release builds too.

## A note from the author

This port is a passion project of mine. It wouldn't have been possible without the help of
[Claude](https://claude.ai) and the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

I hope you enjoy playing it. I've only tested the first mission and part of the second, and I need people to test
it on Linux (through Proton or Wine). Please report any problems on the [issues page](../../issues), and feel free to
fork it and change anything you like.

## Playing a release

1. Download `MOHRS.zip` from the releases page and extract it anywhere.
2. Put your game files in its `assets` folder: `default.xbe` (the USA retail XBE) with the `DATA` folder beside it,
   and `MOH3RDC.xbe` for co-op.
3. Run `MOHRS.exe`. The first start recompiles the game's code from your XBE, which takes a few minutes; later
   starts are immediate. Windows 10 or 11, 64 bit, and a Direct3D 12 GPU are needed.

## Building

1. Clone with submodules: `git clone --recursive <this repo>`.
2. Put your game files in `assets`: `default.xbe` (the USA retail XBE, SHA-256
   `5e63112e14e91fa8ae5b3c9364e64ca027cc33d10c71f8acb7a50346e200f23f`) and `MOH3RDC.xbe`, with the `DATA` folder
   beside them.
3. Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and
   **C++ CMake tools for Windows** (`build.bat` offers to add the last two).
4. Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to
   `out\build\release\MOHRS.exe`, with `MOH3RDC.exe` beside it.

`config/game.toml` and `config/coop.toml` are already in the repository, so `build.bat` skips the symbol scan.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [Zydis](https://github.com/zyantific/zydis),
  [toml++](https://github.com/marzer/tomlplusplus), [Dear ImGui](https://github.com/ocornut/imgui) and
  [SDL](https://github.com/libsdl-org/SDL), through the SDK.
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase), for the library names the first config
  was checked against.
- **[MOHRS-XBERecomp](https://github.com/neonoxd/MOHRS-XBERecomp) by [neonoxd](https://github.com/neonoxd)**
  (MIT), another Rising Sun port on the same SDK. Several of this port's PC features come from it: the menu
  pointer (mouse hover, click and wheel in the game's menus), the PC Options pages inside the game's Options and
  pause menus, the software cursor and its image, the keyboard and mouse layer, and the field of view setting.
  Their code in `src/common` keeps neonoxd's copyright; the license is in `THIRD_PARTY_NOTICES.md`, which ships
  in the release zip.
