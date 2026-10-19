# MAXIBOOST ON for Linux (mbport)

The PlayStation 4 version of *MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON* (2020, Bandai Namco
Entertainment) running natively on Linux. The game's own x86-64 code runs directly on the CPU and
its graphics are translated to Vulkan. It is not an emulator.

mbport is built on [bloodborne_linux](https://github.com/ayozetr/bloodborne_linux), ayozetr's
Linux fork of [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28, by way of gzport (GODZILLA) and gvport (GUNDAM VERSUS, the same Bandai Namco
engine). The runtime, the GPU translation, the upscalers and the launcher come from there; mbport
points them at MAXIBOOST ON.

> **No game files are included.** You need your own decrypted dump of MAXIBOOST ON, the North
> American release `CUSA18758`, with update 1.15. This project is not affiliated with Bandai Namco
> Entertainment, SOTSU, SUNRISE or Sony Interactive Entertainment. MOBILE SUIT GUNDAM EXTREME VS.
> MAXIBOOST ON © SOTSU · SUNRISE.

## Status

**Early, playable offline.** Tested on Linux with an AMD Radeon RX 7800 XT (Mesa/RADV):

- The game boots through its intro movie, the agreements and an offline "Server communication
  failed" notice to the main menu.
- **Free Battle** plays at 60 FPS, with the beams and explosions drawn.
- **Two players on one PC**: the game's own split screen. In Free Battle a player slot set to 2P
  plays with the second gamepad and the game splits the screen (verified with a virtual second
  pad). See [Two players](#two-players).
- **Saves** persist across restarts (see [Saves](#saves)).

Not verified or not done yet:

- **Sound** has not been checked by ear.
- **Trophies**: the list is read from your dump, but none has been seen unlocking.
- **DLC** (the add-ons' unlock-key `.pkg` files, see [DLC](#dlc)) has not been checked in the game.
- **Online and LAN play** through a matching server (`BB_ONLINE=1`, launcher: **Over the
  network**) are not tested with this game; without a server the port answers as a console not
  signed in to PSN.
- **Native resolutions other than the game's own and the upscalers** (FSR 3.1, FSR 4, DLSS, TAA)
  are pending: the game's frame is not mapped yet, so the launcher keeps the upscaler off.
- The other modes, long sessions and a full play-through.

## What mbport inherits

From gzport and gvport, the runtime already implements PS4 libraries that Bloodborne did not use:
the libSceNgs2 audio renderer with ATRAC9, libSceFont through FreeType, kernel event flags, fibers,
save data memory, the common dialogs of an offline console and imports resolved by NID for any
executable's library layout.

- **Launcher** (GTK 4 / libadwaita) with every setting in one window, in English, Spanish,
  Italian, French, German, Portuguese, Russian and Simplified Chinese. It checks the game folder.
- **Controls:** keyboard and mouse or any SDL gamepad, every button remappable.
- **Trophies** from your own dump (`sce_sys/trophy`); their names need the PS4's trophy key
  (shadPS4's TrophyKey), entered in the launcher.
- **Install from PKG**, **save import**, **Add to Steam**, third-party patches in shadPS4's XML
  format for version 01.15, and an **AppImage** for any distribution.

## Requirements

- Linux x86-64 and a GPU with Vulkan 1.3: AMD or Intel with Mesa, or NVIDIA with NVIDIA's driver.
- Python 3 with PyGObject, GTK 4 and libadwaita for the launcher.

## Getting started

### 1. The game folder

Dump the game (`CUSA18758`, version 1.00) and its 1.15 update from your PS4. The game's files are
at the folder's root: `eboot.bin`, `sce_sys/`, `sce_module/`, `archives/`, `raw/` (the movies and
sound banks) and `sce_discmap.plt`. Then merge the update into the game. On one filesystem,
hardlinks keep the 30 GB from being copied:

```bash
cp -al CUSA18758 CUSA18758-full
cp -al --remove-destination CUSA18758-patch/. CUSA18758-full/
python3 scripts/game_check.py CUSA18758-full
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
launcher: `BB_GAME_DIR=/path/to/CUSA18758-full bash run.sh`.

The launcher keeps its settings in `~/.config/mbport-launcher`. From a checkout, saves, the shader
cache and `bbport.ini` go to `user/` and the checkout itself; nothing of a Bloodborne, GODZILLA or
GUNDAM VERSUS install (`~/.config/bbport-launcher`, `~/.config/gzport-launcher`,
`~/.config/gvport-launcher`, `~/.local/share/bbport`, `~/.local/share/gzport`,
`~/.local/share/gvport`) is read or written.

### AppImage

Build it with `bash build.sh && bash packaging/appimage-sharun.sh`, then run
`dist/MAXIBOOST-ON-mbport-x86_64.AppImage`. Saves and settings go to `~/.local/share/mbport`.

### Two players

MAXIBOOST ON has local multiplayer of its own, so one game is enough. Connect a second gamepad,
go to **Free Battle**, and under **Mobile Suit Settings** set a player slot to **2P**: the second
gamepad plays that slot and the game splits the screen. Verified with a virtual second pad; a
second physical controller and the other modes' 2P are not tested yet.

### Over the network

Not tested with MAXIBOOST ON yet. The port keeps gvport's network play: each PC needs its own copy
of the game and of the port, and one matching server serves them all.

- **One of the players' PCs:** in the launcher, **Over the network: host** on that PC (its address
  is shown), **join** with that address on the others.
- **An always-on PC** (a home server, no GPU or game needed): build the server alone with
  `bash tools/gvnet-server/build.sh` (it needs g++ and protobuf), copy
  `tools/gvnet-server/out/gvnet-server` to `~/mbnet-server/` there and start it by hand with
  `tools/gvnet-server/mbserver toggle | start | stop | status | log`, or run it as a user service
  (`tools/gvnet-server/mbnet-server.service`, how to install it is in the file). It listens on
  port 31323 (TCP, and UDP 31324), so a GODZILLA shadNet server can run on the same PC (a GUNDAM
  VERSUS port's server uses 31323 too: give one of them another port). Every PC then plays with
  **join** and the address `<server>:31323`.

### Saves

The game keeps its progress in save data memory and its replays in a save folder:

- `user/savedata/1/CUSA18758.memory/memory.dat` (2 MiB, with `param.bin` beside it),
- `user/savedata/1/CUSA18758/REPLAY/`.

**Import saves** in the launcher (or `scripts/save_import.py`) copies in the newest `memory.dat`
and `REPLAY` folder it finds, after backing up the ones there to `user/savedata-backups/`. That
saves from shadPS4 or a decrypted PS4 export look the same and are accepted by the game is not
verified yet.

### DLC

The add-ons are unlock keys: their data is in the game and its update. Put their `.pkg` files
(for example `UP0700-CUSA18758_00-GUNDAMVSMBDLC001`, Gundam Barbatos Lupus Rex) in a folder and
choose it as the launcher's DLC folder; the port reports them to the game as installed. Not
checked in the game yet.

## Credits

- [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc): **bbport**, the
  native port everything here is built on, and almost all of the work behind it. Its README, with
  the technical details, is in [BBPORT.md](BBPORT.md).
- [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux): the Linux fork of bbport,
  with its launcher, upscalers, memory models, trophies, PKG install, save import and AppImage, and
  everyone thanked there.
- **gzport**, the GODZILLA port: imports by NID, game profiles, Ngs2, libSceFont and the dialogs
  of an offline console; and **gvport**, the GUNDAM VERSUS port mbport starts from: fibers, the
  offline NP toolkit, save data memory, the matching server for network play.
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
and DLSS are trademarks of NVIDIA Corporation. MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON ©
SOTSU · SUNRISE.
