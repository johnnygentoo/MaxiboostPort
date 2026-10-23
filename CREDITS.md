# Credits

mbport exists because of bbport, the native Bloodborne port, and the people who built and improved
it. Almost everything that makes a PS4 game run here was written for Bloodborne first: the native
runtime, the Vulkan renderer, the upscalers, the launcher, the online module. mbport points that
work at MAXIBOOST ON. Thank you all.

Components and their licenses: [below](#components-and-licenses), and the license files in each
third-party folder.

## The line of ports

1. **[bbport](https://github.com/deadinside28/bloodborne_pc)** by
   **[deadinside28](https://github.com/deadinside28)**: the native port of Bloodborne that
   everything here is built on. The game's own code runs on the CPU, a runtime replaces the PS4's
   libraries, and a renderer derived from shadPS4 turns its graphics into Vulkan. Its README, with
   the technical details, is in its own repository.
2. **[bloodborne_linux](https://github.com/ayozetr/bloodborne_linux)** by
   **[ayozetr](https://github.com/ayozetr)**: the Linux fork of bbport that the later ports start
   from. It adds improvements of its own and from the community: online play, trophies, any
   controller with its own prompts, the in-game menu, Add to Steam, the AppImage, and the work of
   everyone listed below.
3. **gzport**, the GODZILLA port: imports by NID, game profiles, Ngs2 audio, libSceFont and the
   dialogs of an offline console.
4. **gvport**, the GUNDAM VERSUS port (the same Bandai Namco engine): fibers, the offline NP
   toolkit, save data memory, the matching server for network play.
5. **mbport**, this port, by **[johnnygentoo](https://github.com/johnnygentoo)**, the author of
   gzport and gvport: MAXIBOOST ON's boot, native resolutions, upscaler profile, DLC, and online
   Player Match through a server of your own.

## Projects

| Project | What mbport takes from it |
|---|---|
| [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) | **bbport**: the whole foundation. |
| [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux) | The Linux fork the later ports start from, with its own improvements and the community's. |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | The renderer, the shader recompiler and the network and PSN code (GPL-2.0-or-later). |
| [Wozzardman/shadp2p](https://github.com/Wozzardman/shadp2p) | The network and PSN libraries of the online module (`gpu/bbnet`). |
| [Wozzardman/shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p) | The shadNet server. mbport's matching server follows its room logic (`gpu/bbnet/server`). |
| [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) | The Windows port: online play, DLSS, Add to Steam and many fixes that came to Linux. |
| [Awakenchan/LanternEngine](https://github.com/Awakenchan/LanternEngine) | Video memory on 4 GB cards, crash fixes, fewer stutters while loading, stick options, save import. |
| [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | The groundwork for DLSS (`gpu/dlss_bridge`), and the FSR 4 model without trails. |
| [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) | FSR 3.1 and the FSR 4 provider on Vulkan, with the AMD FidelityFX SDK (MIT). |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | DLSS (not distributed: you supply it). |
| [KytyPS5](https://github.com/KytyPS5/KytyPS5), [AnyPS5](https://github.com/boykopovar/AnyPS5) | The NGS2 parameter IDs and rendering semantics the runtime's Ngs2 follows (GPL-2.0). |
| [Thealexbarney/LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | ATRAC9 audio decoding (MIT). |
| [Dear ImGui](https://github.com/ocornut/imgui), [sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/), [cpp-httplib](https://github.com/yhirose/cpp-httplib), [FreeType](https://freetype.org), DejaVu fonts | The overlay, SPIR-V generation, half floats, HTTP, fonts. |
| [pkgforge-dev/Anylinux-AppImages](https://github.com/pkgforge-dev/Anylinux-AppImages) | quick-sharun, which builds the AppImage. |
| [paulomanrique/ps4-pkg-extractor](https://github.com/paulomanrique/ps4-pkg-extractor) | The launcher's *Install from PKG* (Unlicense), fetched at build time. |

## Components and licenses

mbport is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)), as bbport is: it
contains code from shadPS4 (GPL-2.0-or-later). The third-party components keep their own licenses:

- [shadPS4](https://github.com/shadps4-emu/shadPS4)'s video core and shader recompiler, and the
  network and PSN code of the online module (`gpu/bbnet`, from Wozzardman's shadp2p):
  GPL-2.0-or-later.
- [sirit](https://github.com/shadps4-emu/sirit) and [half](https://half.sourceforge.net/).
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT; FSR 3.1 on Vulkan and the
  FSR 4 v07 provider) and the AMD FidelityFX SDK (MIT).
- [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT), used to build the FSR 4.1.1
  assets.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui)
  (MIT), [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT), [FreeType](https://freetype.org)
  and the DejaVu fonts.
- The DLSS bridge (`gpu/dlss_bridge`, MIT) and loader, from
  [Supermedo's Windows port](https://github.com/Supermedo/bloodborne_pc), adapted from
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
- [ps4-pkg-extractor](https://github.com/paulomanrique/ps4-pkg-extractor) (Unlicense), fetched by
  the build, not stored here.

- The FSR 4 v07 model assets, built by [Q2RTX](https://github.com/FireBurn/Q2RTX) from AMD's
  FidelityFX SDK source (MIT): downloaded by `tools/fetch_fsr4_assets.sh` and included in the
  AppImage with their license, not stored here.

AMD's FSR 4.1.1 DLLs and NVIDIA's DLSS SDK and libraries are not distributed here.

## People of bbport and bloodborne_linux

As credited by bloodborne_linux. Their work is in the code mbport is built on.

- **[deadinside28](https://github.com/deadinside28)**: bbport, the port this whole project is built on.
- **[ayozetr](https://github.com/ayozetr)**: bloodborne_linux, the Linux fork, and gathering everyone's work into it.
- **[Supermedo](https://github.com/Supermedo)**: online play, DLSS, the cheats page, Add to Steam,
  the icon, and many fixes from the Windows port.
- **[0xCydral](https://github.com/0xCydral)**: gathered and adapted dozens of fixes (crashes while
  loading, black images, the upscaler, the performance line in the log).
- **[Awakenchan](https://github.com/Awakenchan)**: video memory on 4 GB cards and crash fixes,
  fewer stutters while areas load, stick options, the controller in use, save import, the
  version selector.
- **[Classmg22](https://github.com/Classmg22)** (CLS_AMG): BBSC, Seamless Co-op.
- **[MaskZeus](https://github.com/MaskZeus)**: bbbridge.
- **[Wozzardman](https://github.com/Wozzardman)**: the online libraries and the co-op server.
- **[droogie](https://github.com/droogie)**: The Hunter's Dream server, and fast loading (bbhost).
- **[fhody125-web](https://github.com/fhody125-web)**: NoDreamForHunter, the base of BBSC's game server.
- **[MikeTat1988](https://github.com/MikeTat1988)**: Hunter's Atlas, the fullscreen in-game map.
- **[IFreemz](https://github.com/IFreemz)**: the groundwork for DLSS, and FSR 4 without trails.
- **[FireBurn](https://github.com/FireBurn)**: FSR 3.1.
- **[Jhony-Jhansen](https://github.com/Jhony-Jhansen)**: keyboard movement and the direct mouse camera.
- **[mcrib884](https://github.com/mcrib884)**: the bbmouse mouse camera, the basis of the direct camera.
- **[HatsyRei](https://github.com/HatsyRei)**: the keyboard walk key.
- **[msisovic](https://github.com/msisovic)**: why loading hung after a lamp travel (guest thread handles above 40 bits).
- **[axpico](https://github.com/axpico)**: with VRAM full, the game waits for evicted textures to be freed instead of closing.
- **[pmwork-personal](https://github.com/pmwork-personal)**: ultrawide monitors without a stretched picture.
- **[emurphy04](https://github.com/emurphy04)**: ultrawide output and the HUD position, Quit to
  Desktop in the game's menus, and a video player crash when it stops.
- **[cassioKenji](https://github.com/cassioKenji)**: the keyboard sprint measurements.
- **[jpramosjp](https://github.com/jpramosjp)**: summons through the public shadNet server.
- **[yumlevi](https://github.com/yumlevi)**: the black character preview, the loading screen's
  picture and the texture cache's memory.
- **[devilismyfriend](https://github.com/devilismyfriend)**: the texture cache frees video memory in time.
- **[GoncaloLobo0](https://github.com/GoncaloLobo0)**: crashes while loading areas.
- **[bmy](https://github.com/bmy)**: the interface outside 1080p, motion vectors for the large
  beasts, and a memory fault.
- **[Mrsuss60](https://github.com/Mrsuss60)**: crashes when loading some maps, and the gestures
  menu from the keyboard.
- **[SrShadowy](https://github.com/SrShadowy)**: the on-screen keyboard for the character's name,
  and the shader compiling notice.
- **[Obrikash](https://github.com/Obrikash)**: the game starts when DLSS cannot be used.
- **[Qiming-Liu](https://github.com/Qiming-Liu)**: the Simplified Chinese translation.
- **[andrecabralgit](https://github.com/andrecabralgit)**: the game's audio at full level.
- **[Jgamrot](https://github.com/Jgamrot)**: the in-game menu's checkboxes and sliders.
- **[GerasimosKan](https://github.com/GerasimosKan)**: trophy names without the key, and the
  sound when one is earned.
- **[artemkaVG](https://github.com/artemkaVG)**: a stencil buffer fix.
- **[Vorshim92](https://github.com/Vorshim92)**: staying signed in on the public shadNet server.
- **[parsifal295](https://github.com/parsifal295)**: quitting from the menu with a controller,
  the menu within the screen, and the touchpad halves on Xbox pads.
- **[AyrtonSanabio](https://github.com/AyrtonSanabio)**: the in-game menu's language without the launcher.
- **[Aryxa](https://github.com/Aryxa)**: DLSS in the AppImage on NVIDIA.
- **[AndiMajore](https://github.com/AndiMajore)**: crash debugging (`BB_FAULT_WAIT`).
- **[AwesomeObserver](https://github.com/AwesomeObserver)**: large recorded command groups, and
  the swapchain after display changes.

### Help and testing

- **[willthehuman](https://github.com/willthehuman)**: detailed reports with logs, and testing the fixes.
- **[nr1971](https://github.com/nr1971)**: the game not starting on some graphics cards, the
  camera speeding up on diagonals, and a crash on quitting.
- **[Joomzie](https://github.com/Joomzie)**: swapping A and B on Nintendo-style controllers.
- **[Kaka556Kevin](https://github.com/Kaka556Kevin)**: testing the video memory fix.
- **[segasonicye](https://github.com/segasonicye)**: why the game did not start in the Steam Deck's Game Mode.
- **[Monochrome0907](https://github.com/Monochrome0907)**: which effects freeze loading screens on NVIDIA.
- **[bruxis](https://github.com/bruxis)**: the log that showed why Steam Input did not reach the game.
- **[shauryasagar](https://github.com/shauryasagar)** and **[ZeroSTF](https://github.com/ZeroSTF)**:
  the Hunter's Dream going black after long sessions.
- **[popcar2](https://github.com/popcar2)**: Quit to Desktop with online play on, and ideas for the launcher.
- **[shaneo88](https://github.com/shaneo88)**: shadPS4's patch file in the patches folder.
- **[n3vrs0ft](https://github.com/n3vrs0ft)** and **[cappp](https://github.com/cappp)**: the
  "Transaction failed" crash on Seamless Hunt.

If your work is in here and you're missing, or you'd rather not be listed, please open an issue.
