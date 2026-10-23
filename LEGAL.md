# Legal notice

**Read this before you use mbport, and before you share anything made with it.**

## No game is included

mbport is a compatibility layer: source code that lets a PlayStation 4 game you own run on a
Linux PC. It contains **no part of MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON**, and no part of
any other game, of the PS4 system software or of Sony's network services. This repository, its
releases and its AppImages ship none of the following, and never will:

- the game's executable (`eboot.bin`), its modules (`sce_module/`, `.prx`, `.sprx`), its system
  folder (`sce_sys/`, `param.sfo`), its archives, movies or sound banks;
- updates or add-ons (DLC), as `.pkg` files or extracted;
- save data, PSN title storage or user data of any player;
- console keys of any kind, including the trophy key, and PS4 firmware or system libraries;
- AMD's FSR 4.1.1 DLLs and the assets made from them, and NVIDIA's DLSS SDK and libraries (their
  licenses don't allow it; you supply them yourself if you want them).

The AppImage does include the FSR 4 **v07** model (`fsr4_shaders/`, built by the
[Q2RTX](https://github.com/FireBurn/Q2RTX) project from AMD's FidelityFX SDK source, which AMD
published under the MIT license), with that license next to it (`LICENSE-FSR4-v07.txt`). The
repository itself doesn't contain it: `tools/fetch_fsr4_assets.sh` downloads it.

## You must supply your own legal dump

To play, you need **your own dump** of the game (`CUSA18758`, update 1.15), made from **a copy you
bought, on a PS4 you own**. The same goes for any add-on: only content you bought, dumped by you.
How you make the dump is up to you and to the laws of your country. Check that it is allowed
where you live. mbport doesn't help you make it: the launcher's *Install from PKG* only unpacks
package files you already have, using a third-party tool (ps4-pkg-extractor) that the build
downloads, and it ships no keys.

**Don't ask for game files, and don't share them.** Don't post them, link them or attach them to
issues, pull requests, discussions or chats about this project. Issues that ask for, link to or
contain game files, keys or other people's saves are closed and deleted. The same applies to
"pre-installed" packs, repacks or AppImages bundled with a game: they aren't from this project.
Please report them.

## Online play

mbport's online play uses **a matching server you run yourself** (`gpu/bbnet/server`). The port
never contacts Sony's PlayStation Network or Bandai Namco's game servers. The game configuration
that the official server used to send is answered locally from `data/maxiboost_env.json`.
mbport is no way to reach, imitate or get into official online services, and you must not use it
for that.

## Not affiliated

mbport is an independent, non-commercial fan project. It is not affiliated with, endorsed by or
sponsored by Bandai Namco Entertainment, SOTSU, SUNRISE, Sony Interactive Entertainment, AMD or
NVIDIA.

- MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON © SOTSU · SUNRISE. GUNDAM and related names are
  trademarks of their owners.
- PlayStation, PS4 and PlayStation Network are trademarks of Sony Interactive Entertainment Inc.
- AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. NVIDIA and DLSS are
  trademarks of NVIDIA Corporation.

These names are used only to say which game and hardware mbport works with.

## License and warranty

mbport's own code is licensed under the **GNU General Public License, version 2 or later**
([LICENSE](LICENSE)). The third-party components it contains keep their own licenses (see
[CREDITS.md](CREDITS.md#components-and-licenses)). The AppImage carries all of these licenses
in `share/bbport/licenses/`, and its source code is this repository at the release's tag.

As the GPL says, the software comes **without any warranty**. You use it at your own risk,
including the risk to your saves. Back them up.

This notice is not legal advice.
