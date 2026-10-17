# GUNDAM VERSUS for Linux (gvport)

The PlayStation 4 version of *GUNDAM VERSUS* (2017, Bandai Namco Entertainment) running natively
on Linux. The game's own x86-64 code runs directly on the CPU and its graphics are translated to
Vulkan. It is not an emulator.

gvport is built on [bloodborne_linux](https://github.com/ayozetr/bloodborne_linux), ayozetr's
Linux fork of [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28, by way of gzport, the GODZILLA port made from it. The runtime, the GPU
translation, the upscalers and the launcher come from there; gvport points them at GUNDAM VERSUS.

> **No game files are included.** You need your own decrypted dump of GUNDAM VERSUS, the North
> American release `CUSA08790`, with update 1.13. This project is not affiliated with Bandai Namco
> Entertainment, SOTSU, SUNRISE or Sony Interactive Entertainment. GUNDAM VERSUS © SOTSU ·
> SUNRISE.

## Status

**Playable offline.** The game boots through its intro movie, agreements and title screen to the
main menu (offline), and Free Battle matches play at 60 FPS with sound, the HUD and the CPU
opponents. Tested on Linux with an AMD Radeon RX 7800 XT (Mesa/RADV) and a DualSense: ten minutes
of back-to-back battles without an error. The other offline modes, saving over a long session
and a full play-through are not verified yet.

- **PS4 Pro mode** (default; `BB_NEO=0`: a base PS4): the game renders its scene at 1920x1080 as
  on a PS4 Pro, instead of the base console's 1440x1080 stretched to the screen.
- **Other output sizes** (1440p, 4K) are scaled from the game's 1080p frame by the GPU library;
  the game does not render natively at them yet (its frame size is not mapped).
- **Memory model:** "Auto" is the model of 0.3 on every GPU: with the new one each of the
  frame's compute-queue waits is a GPU round trip, and battles ran at ~30 FPS.
- **Offline only.** GUNDAM VERSUS is an online-focused fighting game and ships PSN's toolkit
  (libSceNpToolkit2); the port answers as a console that is not signed in to PSN.
- **No local multiplayer in the game.** GUNDAM VERSUS has one local player; its 2v2 and 3v3
  modes are online only. The port logs further gamepads in as users 2-4 (the groundwork for
  playing two instances against each other), but no mode of the game takes a second local player.
- **Upscalers (FSR 3.1/4, DLSS, TAA) are off**: they need the game's scene color, depth and
  motion, and the frame of GUNDAM VERSUS is not mapped yet.

## What gvport inherits

From gzport, the runtime already implements PS4 libraries that Bloodborne did not use: the
libSceNgs2 audio renderer with ATRAC9, libSceFont through FreeType, kernel event flags, the common
dialogs of an offline console and imports resolved by NID for any executable's library layout.

- **Launcher** (GTK 4 / libadwaita) with every setting in one window, in English, Spanish,
  Italian, French, German, Portuguese, Russian and Simplified Chinese. It shows your dump's own
  icon and checks the game folder.
- **Controls:** keyboard and mouse or any SDL gamepad, every button remappable.
- **Trophies** from your own dump (`sce_sys/trophy`); their names need the PS4's trophy key
  (shadPS4's TrophyKey), entered in the launcher.
- **Install from PKG**, **save import**, **Add to Steam**, third-party patches in shadPS4's XML
  format for version 01.13, and an **AppImage** for any distribution.

## Requirements

- Linux x86-64 and a GPU with Vulkan 1.3: AMD or Intel with Mesa, or NVIDIA with NVIDIA's driver.
- Python 3 with PyGObject, GTK 4 and libadwaita for the launcher.

## Getting started

### 1. The game folder

Dump the game (`CUSA08790`, version 1.00) and its 1.13 update from your PS4. The game's files are
at the folder's root: `eboot.bin`, `sce_sys/`, `sce_module/`, `archives/`, the `.nus3audio` sound
banks and the `.mp4` movies. Then merge the update into the game. On one filesystem, hardlinks
keep the 45 GB from being copied:

```bash
cp -al CUSA08790 CUSA08790-full
cp -al --remove-destination CUSA08790-patch/. CUSA08790-full/
python3 scripts/game_check.py CUSA08790-full
```

### 2. Build and run

On Arch Linux (or CachyOS, Manjaro…):

```bash
sudo pacman -S --needed base-devel cmake ninja python python-gobject gtk4 libadwaita sdl3 \
    vulkan-headers vulkan-icd-loader glslang spirv-headers spirv-cross ffmpeg boost fmt \
    robin-map xxhash zydis miniz protobuf nlohmann-json openssl miniupnpc python-pillow ttf-dejavu \
    freetype2 noto-fonts noto-fonts-cjk
paru -S magic_enum vulkan-memory-allocator xbyak   # from the AUR

bash build.sh
launcher/bb-launcher.sh          # or launcher/install-desktop.sh for a menu entry
```

With [Nix](https://nixos.org), `bash build.sh` gets every dependency from `shell.nix` by itself.

In the launcher, choose your game folder (the one with `eboot.bin`) and press **Play**. Without the
launcher: `BB_GAME_DIR=/path/to/CUSA08790-full bash run.sh`.

The launcher keeps its settings in `~/.config/gvport-launcher`. From a checkout, saves, the shader
cache and `bbport.ini` go to `user/` and the checkout itself; nothing of a Bloodborne or GODZILLA
install (`~/.config/bbport-launcher`, `~/.config/gzport-launcher`, `~/.local/share/bbport`,
`~/.local/share/gzport`) is read or written.

### AppImage

Build it with `bash build.sh && bash packaging/appimage-sharun.sh`, then run
`dist/GUNDAM-VERSUS-gvport-x86_64.AppImage`. Saves and settings go to `~/.local/share/gvport`.

### Saves

Saves go to `user/savedata/1/CUSA08790/`. GUNDAM VERSUS' save folder names are not known yet
(the game has not saved under the port); **Import saves** in the launcher still expects
GODZILLA's layout until they are.

## Credits

- [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc): **bbport**, the
  native port everything here is built on, and almost all of the work behind it. Its README, with
  the technical details, is in [BBPORT.md](BBPORT.md).
- [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux): the Linux fork of bbport,
  with its launcher, upscalers, memory models, trophies, PKG install, save import and AppImage, and
  everyone thanked there.
- **gzport**, the GODZILLA port gvport starts from: imports by NID, game profiles, Ngs2,
  libSceFont and the dialogs of an offline console.
- [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc): the Windows port, whose
  DLSS support and Add to Steam came to Linux in bloodborne_linux.
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the renderer and shader recompiler.
- [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR):
  the DLSS bridge, and the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS).
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn and the AMD FidelityFX SDK.
- [KytyPS5](https://github.com/KytyPS5/KytyPS5) and [AnyPS5](https://github.com/boykopovar/AnyPS5)
  (GPL-2.0): the NGS2 parameter IDs and rendering semantics the runtime's Ngs2 follows.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) by Alex Barney (MIT) and
  [FreeType](https://freetype.org).
- [quick-sharun](https://github.com/pkgforge-dev/Anylinux-AppImages) for the AppImage.
- [ps4-pkg-extractor](https://github.com/paulomanrique/ps4-pkg-extractor) by paulomanrique
  (Unlicense): the launcher's *Install from PKG*.

Full list of components and licenses: [BBPORT.md](BBPORT.md#credits-and-licenses).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses. NVIDIA
and DLSS are trademarks of NVIDIA Corporation. GUNDAM VERSUS © SOTSU · SUNRISE.
