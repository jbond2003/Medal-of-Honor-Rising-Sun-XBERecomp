# Medal of Honor: Rising Sun - XBE Recomp

> This port is a passion project of mine. It wouldn't have been possible without the help of
> [Claude](https://claude.ai) and the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.
>
> I hope you enjoy playing it. I've only tested the first mission and part of the second, and I need people to test
> it on Linux (through Proton or Wine). Please report any problems on the [issues page](../../issues), and feel free
> to fork it and change anything you like.

An unofficial native PC port of the original Xbox version of Medal of Honor: Rising Sun, with PC graphics options,
keyboard and mouse support, and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of
> the game to install or build.

## Installation

- Put your game files in the `assets` folder. This can be an ISO, or the extracted files, of the USA release. An ISO
  can be extracted with [extract-xiso](https://github.com/xboxdev/extract-xiso).
- Run `MOHRS.exe`. The first start compiles the game's code for your PC, this only happens once.
- PC settings can be adjusted in the in-game menus (Options on the main menu, Preferences on the pause menu), or
  through the Settings overlay, which is opened with F10.

## Features

### Display

- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Render Scale** - Renders the game at 50% to 200% of the output resolution, allowing supersampling or
  downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 70 being the
  game's default. Scopes still zoom as before.
- **Frame Rate Limit** - 30, the game's original pacing, or 60.
- **FPS Counter** - Displays the frame rate on screen.

### Graphics

- **Anisotropic Filtering** - Forces 2x, 4x, 8x or 16x anisotropic filtering on all textures.

### Textures

- **Texture Replacement** - Replaces the game's textures with the `.dds` or `.png` images in the
  `textures\replacement` folder beside the exe.
- **Texture Dumping** - Saves every texture the game uses into the `textures\dumps` folder beside the exe.

### Input

- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable horizontal and vertical
  sensitivity, inverted look and rebindable controls.
- **Seamless Switching** - Use a controller and the keyboard and mouse at any time. Button prompts and menu hints
  follow the device you're using.
- **Menu Mouse** - Hover and click in the game's menus, right click to go back, and use the wheel to scroll or
  change settings.

### General

- **Skippable Movies** - Skip any movie with a click, Enter, Space, Escape or a controller face button.
- **Fast Loads** - Levels and menus load without the original's built-in waits.

## System Requirements

### Minimum

- **OS**
  - Windows 10 64-bit
- **CPU** with support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12
- **Memory**
  - 4 GB

## Building

### 1. Clone the Repository

Clone this repository and its SDK submodule using [Git](https://git-scm.com/).

```
git clone --recursive https://github.com/jbond2003/Medal-of-Honor-Rising-Sun-XBERecomp.git
```

### 2. Add the Required Game Files

Place your extracted game files in the `assets` folder. `default.xbe` from the USA release must be there, with the
`DATA` folder and `MOH3RDC.xbe` beside it.

### 3. Install Dependencies

Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following workloads and
individual components:

- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

### 4. Build the Project

- Run `build.bat`. The first build recompiles the entire game and can take a while.
- When it finishes, run `out\build\release\MOHRS.exe`.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482, the recompiler and runtime this port is
  built on.
- [MOHRS-XBERecomp](https://github.com/neonoxd/MOHRS-XBERecomp) by [neonoxd](https://github.com/neonoxd) (MIT): the
  menu pointer, the original PC Options page before I made changes and fixes to it, and the software cursor come
  from it. Its license is in `THIRD_PARTY_NOTICES.md`.
