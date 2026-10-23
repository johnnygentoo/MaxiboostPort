# MAXIBOOST ON for Linux (mbport)

The PlayStation 4 version of *MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON* (2020, Bandai Namco
Entertainment) running natively on Linux. The game's own x86-64 code runs directly on the CPU and
its graphics are translated to Vulkan. It is not an emulator.

mbport is built on [bloodborne_linux](https://github.com/ayozetr/bloodborne_linux), ayozetr's
Linux fork of [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28, by way of gzport (GODZILLA) and gvport (GUNDAM VERSUS, the same Bandai Namco
engine). The runtime, the GPU translation, the upscalers and the launcher come from there; mbport
points them at MAXIBOOST ON.

> **No game files are included, and none will ever be.** You need your own dump of MAXIBOOST ON,
> the North American release `CUSA18758`, with update 1.15, made from a copy you own. Don't ask for
> game files, saves or keys, and don't share them: see [LEGAL.md](LEGAL.md). This project is not
> affiliated with Bandai Namco Entertainment, SOTSU, SUNRISE or Sony Interactive Entertainment.
> MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON © SOTSU · SUNRISE.

[Build instructions](BUILD.md) · [Legal notice](LEGAL.md) · [Credits](CREDITS.md)

## Status

**Early, playable offline.** Tested on Linux with an AMD Radeon RX 7800 XT (Mesa/RADV):

- The game boots through its intro movie, the agreements and an offline "Server communication
  failed" notice to the main menu.
- **Free Battle** plays at 60 FPS, with the beams and explosions drawn.
- **Two players on one PC**: the game's own split screen. In Free Battle a player slot set to 2P
  plays with the second gamepad and the game splits the screen (verified with a virtual second
  pad). See [Two players](#two-players).
- **Saves** persist across restarts (see [Saves](#saves)).
- **Online Player Match**: two games signed in to our matching server played a 1-on-1 room:
  created, found, joined, both Ready, the battle at 60 FPS with hits landing both ways, and back
  to the room together when the timer ran out. Played twice: two instances on one PC, and two PCs
  on Wi-Fi (an RX 580 and an RTX 3050) joined to a server on a third PC. See [Over the network](#over-the-network).
- **DLC**: Gundam Barbatos Lupus Rex (its `.pkg` in the launcher's DLC folder) is offered in
  Machine Select and plays, offline and online (see [DLC](#dlc)).
- **Other output sizes** (720p, 1440p, 4K): the game renders natively at them, its 3D scene, its
  menus and its HUD (sizes patched at start, `scripts/patches.py`; above 1080p its GPU memory pools
  grow). Native 4K holds 60 FPS on an RX 7800 XT.
- **Upscalers (FSR 3.1, FSR 4, TAA)**: the scene renders at the output size divided by the preset's
  scale and the upscaler fills the output where the game scales its scene into the display buffer;
  the HUD stays the game's own, at the output size (`gpu/games/maxiboost.cpp`). Measured in Free
  Battle on an RX 7800 XT: FSR 3 Quality to 4K 60 FPS (FSR 2.2 ms), FSR 4 Performance to 4K 58-60
  FPS (its model ~8 ms), TAA at 1080p 60 FPS. Preset and output changes need a restart.

Not verified or not done yet:

- **Sound** has not been checked by ear (the game's mix comes out; nothing was listened to).
- **Trophies**: the list is read from your dump, but none has been seen unlocking.
- **Online and LAN play**: signed in to a matching server of our own the game reaches its online
  main menu (the ONLINE badge, the Online Battle list); its configuration is answered by the port
  (`data/maxiboost_env.json`, also as its PSN title storage; the publisher's server is never
  contacted). A 1-on-1 Player Match was played on one PC and between two PCs; the other room
  types (Free, Shuffle), more than two players, Ranked Match and Casual Match are not tried yet,
  nor the game's own **LAN Battle** (Options).
- **Upscalers in motion** (ghosting of the player's mech, effects), DLSS and FSR 4.1.1, and the
  upscalers or other sizes in split screen.
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

The full instructions, with the packages for Gentoo, Arch and Nix and the optional parts, are in
[BUILD.md](BUILD.md). In short, on Arch Linux (or CachyOS, Manjaro…):

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

**Partly verified with MAXIBOOST ON**: a 1-on-1 Player Match was played to its end, between two
games on one PC and between two PCs joined to a server on a third (one picks **Create Room** with
**Room type: 1-on-1**, the other **Search for Room** with the same Room type, both take a side of
a line and press Ready); other room types and more players are not tried yet (see
[Status](#status)). Each PC needs its own copy of the
game and of the port, and one matching server serves them all.

- **One of the players' PCs:** in the launcher, **Over the network: host** on that PC (its address
  is shown), **join** with that address on the others.
- **An always-on PC** (a home server, no GPU or game needed): build the server alone with
  `bash tools/gvnet-server/build.sh` (it needs g++ and protobuf), copy
  `tools/gvnet-server/out/gvnet-server` there as `~/mbnet-server/mbnet-server` (its own name, so a
  GUNDAM VERSUS port's `gvserver` does not take it for its own and stop it) and start it by hand with
  `tools/gvnet-server/mbserver toggle | start | stop | status | log`, or run it as a user service
  (`tools/gvnet-server/mbnet-server.service`, how to install it is in the file). It listens on
  port 31333 (TCP, and UDP 31334), so a GODZILLA shadNet server (31313-31315) and a GUNDAM VERSUS
  port's server (31323-31324) can run on the same PC. Every PC then plays with **join** and the
  address `<server>:31333`.

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
choose it as the launcher's DLC folder; the port reports them to the game as installed.
Verified with Barbatos Lupus Rex, offline and online: it is in Machine Select (Iron-Blooded
Orphans) and plays in Free Battle. The game checks an add-on's entitlement key against its own
data, a key a PS4 reads from the add-on's license; the built-in patch "Installed add-ons" makes an
installed add-on owned whatever its key. Without the add-on installed the unit is not offered.

## Credits

Everyone whose work is in mbport, the people of bbport and bloodborne_linux among them, is listed
in [CREDITS.md](CREDITS.md). In short:

- [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc): **bbport**, the
  native port everything here is built on, and almost all of the work behind it. Its README, with
  the technical details, is in [BBPORT.md](BBPORT.md).
- [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux): the Linux fork of bbport
  the later ports start from, with its own improvements and those of everyone thanked there.
- [Wozzardman](https://github.com/Wozzardman): the network and PSN libraries of the online module
  ([shadp2p](https://github.com/Wozzardman/shadp2p)) and the shadNet server whose room logic the
  matching server follows ([shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p)).
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

GNU GPL v2 or later ([LICENSE](LICENSE)). Read the [legal notice](LEGAL.md) too. Third-party components keep their own licenses. NVIDIA
and DLSS are trademarks of NVIDIA Corporation. MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON ©
SOTSU · SUNRISE.
