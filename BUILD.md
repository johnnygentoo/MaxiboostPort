# Building mbport

This builds the port from source: the runtime (`out/bb-probe`), the GPU library
(`out/gpu/libbbgpu.so`), the online module (`out/gpu/libbbnet.so`) and the matching server. The
game is **not** part of it. You need your own dump to play (see [LEGAL.md](LEGAL.md) and the
[README](README.md#1-the-game-folder)).

## What you need

- Linux on x86-64. A GPU with Vulkan 1.3: AMD or Intel with Mesa, or NVIDIA with NVIDIA's driver.
- GCC (C11 and C++20), CMake, Ninja, pkg-config, Python 3, and git (the build fetches submodules).
- The libraries below. With [Nix](https://nixos.org) you can skip this list: `build.sh` re-enters
  `shell.nix` by itself.

### Gentoo

Used for mbport's own builds and tests: three PCs, AMD RX 7800 XT, AMD RX 580 and NVIDIA RTX 3050.

```bash
emerge --ask --noreplace dev-build/cmake app-alternatives/ninja dev-util/vulkan-headers \
    media-libs/vulkan-loader media-libs/libsdl3 media-libs/freetype media-video/ffmpeg \
    dev-libs/boost dev-libs/libfmt dev-cpp/magic_enum dev-cpp/robin-map dev-libs/xxhash \
    media-libs/VulkanMemoryAllocator dev-util/glslang dev-util/spirv-headers dev-libs/xbyak \
    dev-libs/zydis dev-libs/miniz dev-libs/protobuf dev-cpp/nlohmann_json dev-libs/openssl \
    net-libs/miniupnpc dev-libs/wayland x11-libs/libX11 \
    gui-libs/gtk gui-libs/libadwaita dev-python/pygobject dev-python/pillow \
    media-fonts/dejavu media-fonts/noto media-fonts/noto-cjk
```

### Arch Linux (CachyOS, Manjaro, ...)

This list comes from bloodborne_linux. It has not been re-checked on Arch for mbport.

```bash
sudo pacman -S --needed base-devel cmake ninja python python-gobject gtk4 libadwaita sdl3 \
    vulkan-headers vulkan-icd-loader glslang spirv-headers spirv-cross ffmpeg boost fmt \
    robin-map xxhash zydis miniz protobuf nlohmann-json openssl miniupnpc python-pillow ttf-dejavu \
    freetype2 noto-fonts noto-fonts-cjk
paru -S magic_enum vulkan-memory-allocator xbyak   # from the AUR
```

### Optional parts

| Part | Needs | Without it |
|---|---|---|
| Online play (`libbbnet.so`) | protobuf, nlohmann_json, OpenSSL, zlib, miniupnpc | the port builds and plays offline only |
| *Install from PKG* in the launcher | `cargo` (Rust). The build fetches [ps4-pkg-extractor](https://github.com/paulomanrique/ps4-pkg-extractor) at a pinned revision | the launcher asks for a game folder instead |
| DLSS (NVIDIA RTX) | `DLSS_SDK_ROOT=<a checkout of github.com/NVIDIA/DLSS>` | DLSS is listed as unavailable; FSR 3, FSR 4 and TAA still work. DLSS has not been tested with MAXIBOOST ON yet |
| FSR 4.1.1 | AMD's DLLs, which you supply yourself (the launcher explains how) | FSR 4 uses its built-in model. FSR 4.1.1 has not been tested with MAXIBOOST ON yet |

## Get the source

```bash
git clone --recursive https://github.com/johnnygentoo/MaxiboostPort.git
cd MaxiboostPort
```

If you cloned without `--recursive`, `build.sh` runs `git submodule update --init --recursive`
itself. The submodules are FSR-Vulkan, Dear ImGui and LibAtrac9.

## Build

```bash
bash build.sh
```

This writes everything to `out/`. By default it builds with link-time optimization (LTO), which is
what the launcher and releases use. Options, as environment variables:

- `BB_LTO=OFF`: no link-time optimization. Faster to build, for development.
- `BB_PGO=generate | use | off`: profile-guided optimization. `generate` builds an instrumented
  library that writes `pgo/` while you play. `use` builds with that profile. The default uses
  `pgo/` when it exists.
- `DLSS_SDK_ROOT=...`: also build the DLSS bridge (see above).

## Run

```bash
launcher/bb-launcher.sh                  # the launcher; launcher/install-desktop.sh adds a menu entry
BB_GAME_DIR=/path/to/CUSA18758-full bash run.sh   # without the launcher
```

From a checkout, saves, the shader cache and `bbport.ini` go to `user/` and the checkout itself.
The launcher keeps its settings in `~/.config/mbport-launcher`. All of these are in `.gitignore`.

## Tests

```bash
python3 -m unittest discover -s tests    # launcher, scripts, server: no game needed
bash build.sh --test                     # also builds and runs the C tests
```

## AppImage

```bash
bash build.sh && bash packaging/appimage-sharun.sh
```

This writes `dist/MAXIBOOST-ON-mbport-x86_64.AppImage`. Saves and settings of the AppImage go to
`~/.local/share/mbport`. The AppImage holds the port only, never game files.

## The matching server alone

For an always-on PC that only hosts online games (no GPU and no game needed):

```bash
bash tools/gvnet-server/build.sh          # needs g++ and protobuf (protoc + pkg-config)
```

This writes `tools/gvnet-server/out/gvnet-server`. How to install and start it is in the README's
[Over the network](README.md#over-the-network) section. It listens on TCP 31333 and UDP 31334.

## When the build fails

- **The GPU library fails:** the build stops there on purpose, so an older `libbbgpu.so` is never
  used by mistake. The CMake/Ninja error above that line names the missing package.
- **"patch does not apply" in FSR-Vulkan:** run `bash build.sh` again. It resets the submodule
  and applies the patches again.
- **pkgextract fails** (`out/pkgextract-build.log`): only *Install from PKG* is affected. Install
  the game from a folder instead.
