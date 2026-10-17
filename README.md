# GODZILLA for Linux (gzport)

The PlayStation 4 version of *GODZILLA* (2015, Bandai Namco Entertainment) running natively on
Linux. The game's own x86-64 code runs directly on the CPU and its graphics are translated to
Vulkan. It is not an emulator.

gzport is built on [bloodborne_linux](https://github.com/ayozetr/bloodborne_linux), ayozetr's
Linux fork of [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28. The runtime, the GPU translation, the upscalers and the launcher come from there;
gzport points them at GODZILLA and drops what only Bloodborne had.

> **No game files are included.** You need your own decrypted dump of GODZILLA, the North
> American release `CUSA02046`, with update 1.01. This project is not affiliated with Toho Co.,
> Ltd., Bandai Namco Entertainment or Sony Interactive Entertainment. GODZILLA is a trademark of
> Toho Co., Ltd.

## Status

The game boots and plays. It runs at its own 30 FPS: 60 FPS, uncapped frame rate and other game
patches need patches for the 1.01 executable that are not written yet (`patches/Godzilla.xml`),
and the launcher offers them once they exist.

## Features

- **Launcher** (GTK 4 / libadwaita) with every setting in one window, in English, Spanish,
  Italian, French, German, Portuguese, Russian and Simplified Chinese. It shows your dump's own
  icon and checks the game folder (the 1.01 executable, the files a whole extraction has).
- **Upscaling:** AMD FSR 3.1, FSR 4 and FSR 4.1.1 (built from your own AMD DLL), **NVIDIA DLSS**
  on GeForce RTX, and native-resolution TAA. Output from 720p to 4K. Settings also in the
  in-game overlay (Insert or L3+R3).
- **Two memory models:** the new one, where the GPU works with the game's memory as in a PC game
  (the default on AMD), and the one of 0.3 (the default elsewhere); the launcher switches them.
- **Controls:** keyboard and mouse or any SDL gamepad, every button remappable, sticks swapped,
  camera inverted, A/B swapped for Nintendo-style pads.
- **Trophies:** a notice in the game and a Trophies page in the launcher, from your own dump
  (`sce_sys/trophy`). Their names and descriptions need the PS4's trophy key (shadPS4's
  TrophyKey), which you enter in the launcher; without it, only the icons.
- **Install from PKG:** the game, its 1.01 update and add-ons from your own `.pkg` files, with the
  update merged in.
- **Save import** from shadPS4, another port or a decrypted PS4 export.
- **Add to Steam** in one click (native or Flatpak Steam).
- **Third-party patches** in shadPS4's XML format for version 01.01, from a patches folder.
- **AppImage:** one file with everything inside, for any distribution and the Steam Deck.

Not carried over from bloodborne_linux: online play (The Hunter's Dream, bbbridge, shadNet), the
cheats page, Bloodborne's button prompts and gestures menu, loose-file mods, the self-updater and
Discord status with Yharnam's areas. Discord Rich Presence is still there, off: set `BB_DISCORD=1`
and `BB_DISCORD_CLIENT_ID` (a Discord application of your own named GODZILLA) to turn it on.

## Requirements

- Linux x86-64 and a GPU with Vulkan 1.3: AMD or Intel with Mesa, or NVIDIA with NVIDIA's driver
  (open kernel modules included). Mesa's NVK has not been tested.
- DLSS needs a GeForce RTX (20 series or newer) and NVIDIA's driver; it does not work with NVK.
- Python 3 with PyGObject, GTK 4 and libadwaita for the launcher.

## Getting started

### 1. The game folder

Dump the game (`CUSA02046`, version 1.00) and its 1.01 update from your PS4. The game's files are
at the folder's root: `eboot.bin`, `sce_sys/`, `sce_module/`, `sound/` and the `*.pac` archives.
Then merge the update into the game: copy everything from the update's dump (for example
`CUSA02046-patch`) into the game folder, replacing files (`eboot.bin` and `sce_sys` too). Check it:

```bash
python3 scripts/game_check.py /path/to/CUSA02046   # GODZILLA 1.01: supported
```

With `.pkg` files instead, the launcher's **Game → Install from PKG** extracts the game and the
update and merges them for you.

### 2. Build and run

On Arch Linux (or CachyOS, Manjaro…):

```bash
sudo pacman -S --needed base-devel cmake ninja python python-gobject gtk4 libadwaita sdl3 \
    vulkan-headers vulkan-icd-loader glslang spirv-headers spirv-cross ffmpeg boost fmt \
    robin-map xxhash zydis miniz protobuf nlohmann-json openssl miniupnpc python-pillow ttf-dejavu
paru -S magic_enum vulkan-memory-allocator xbyak   # from the AUR

bash build.sh
launcher/bb-launcher.sh          # or launcher/install-desktop.sh for a menu entry
```

With [Nix](https://nixos.org), `bash build.sh` gets every dependency from `shell.nix` by itself.

In the launcher, choose your game folder (the one with `eboot.bin`) and press **Play**. Without the
launcher: `BB_GAME_DIR=/path/to/CUSA02046 bash run.sh`.

The launcher keeps its settings in `~/.config/gzport-launcher`. From a checkout, saves, the shader
cache and `bbport.ini` go to `user/` and the checkout itself; nothing of a Bloodborne install
(`~/.config/bbport-launcher`, `~/.local/share/bbport`) is read or written.

### AppImage

Build it with `bash build.sh && bash packaging/appimage-sharun.sh`, then run
`dist/GODZILLA-gzport-x86_64.AppImage`. Saves and settings go to `~/.local/share/gzport`.

### Saves

The game keeps its progress in `user/savedata/1/CUSA02046/SAVEDATA00/DATA.BIN`. **Import saves**
next to the saves folder in the launcher copies one in from shadPS4
(`~/.local/share/shadPS4/savedata/1/CUSA02046/SAVEDATA00`), another port or a decrypted export
(Apollo Save Tool, Save Wizard); the saves already there are backed up first. A console's own USB
copy is encrypted with that console's keys and has to be exported decrypted on the PS4.

### DLSS

On a PC with a GeForce RTX, run `bash tools/build_dlss.sh` once after `build.sh`. It downloads
what it needs from the NVIDIA DLSS SDK and builds the bridge. Then choose **DLSS** as the
upscaler.

### DLC

Dump your add-ons too, each one a folder named by its entitlement label, put them in one folder
and pick it in the launcher (**Game → DLC folder**), or set `BB_DLC_DIR=/path/to/dlc` for
`run.sh`; the launcher shows how many it found.

### Add to Steam

**Game → Add to Steam** puts GODZILLA in your Steam library, with your dump's icon; it starts with
the launcher's settings. Steam has to be closed while the game is added, and the launcher offers
to do that. Flatpak Steam runs games inside its sandbox, which has neither the game nor your
drivers, so the shortcut starts the game outside it with `flatpak-spawn --host`. That needs the
`org.freedesktop.Flatpak` permission for Steam, which the launcher asks for once.

## Credits

- [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc): **bbport**, the
  native port everything here is built on, and almost all of the work behind it. Its README, with
  the technical details, is in [BBPORT.md](BBPORT.md).
- [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux): the Linux fork of bbport
  gzport starts from, with its launcher, upscalers, memory models, trophies, PKG install, save
  import and AppImage, and everyone thanked there.
- [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc): the Windows port, whose
  DLSS support and Add to Steam came to Linux in bloodborne_linux.
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the renderer and shader recompiler.
- [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR):
  the DLSS bridge, and the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS).
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn and the AMD FidelityFX SDK.
- [quick-sharun](https://github.com/pkgforge-dev/Anylinux-AppImages) for the AppImage.
- [ps4-pkg-extractor](https://github.com/paulomanrique/ps4-pkg-extractor) by paulomanrique
  (Unlicense), on [orbis-pkg-util](https://crates.io/crates/orbis-pkg-util): the launcher's
  *Install from PKG*.

Full list of components and licenses: [BBPORT.md](BBPORT.md#credits-and-licenses).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses. NVIDIA
and DLSS are trademarks of NVIDIA Corporation. GODZILLA is a trademark of Toho Co., Ltd.
