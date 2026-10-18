#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# BB_PREBUILT=1 (packaged builds, the AppImage): out/bb-probe and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# BB_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default this directory.
data=${BB_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export BB_CONFIG=${BB_CONFIG:-$data/bbport.ini}
# GPU paths are initialized when libbbgpu is loaded, before bbgpu_init receives --user.
# Select the same user directory now, including for an AppImage or a separate save profile.
export BB_GPU_USER_DIR=${BB_GPU_USER_DIR:-${BB_USER_DIR:-$data/user}}
# The drivers' own shader caches keep compiled pipelines between sessions (the port's cache gives
# them the shaders again at startup): NVIDIA's without its cleanup and with room for the game's
# pipelines (as Steam sets it), Mesa's with more room than its default.
export __GL_SHADER_DISK_CACHE=${__GL_SHADER_DISK_CACHE:-1}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
mkdir -p "$BB_GPU_USER_DIR"
# FSR 4.1.1 assets (tools/fsr4cap/build_assets.sh): next to run.sh or in the data directory.
if [[ -z ${BB_FSR411_DIR:-} && ! -d fsr4_411 && -d $data/fsr4_411 ]]; then
    export BB_FSR411_DIR=$data/fsr4_411
fi
if [[ -z ${BB_PREBUILT:-} && -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
# BB_SAVE_LOG=1 (launcher: "Save the log and statistics to a file"): this run's output and its
# per-frame statistics also go to $data/logs/<time>.log, .frames.csv and .readbacks.csv.
if [[ ${BB_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    mkdir -p "$logs"
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
    export BB_FRAME_STATS=1 BB_FRAME_LOG=${BB_FRAME_LOG:-$logs/$stamp.frames.csv}
    export BB_READBACK_LOG=${BB_READBACK_LOG:-$logs/$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    exec 3>"$logs/$stamp.log"
    # Bash builtins only (the AppImage's PATH has no tee).
    exec > >(while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; printf '%s\n' "$line" >&3; done) 2>&1
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# BB_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default the merged dump (base game
# with the 1.13 update) in this directory.
game=${BB_GAME_DIR:-CUSA08790-full}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2; exit 1; fi
original_game=$game
# Bloodborne's button prompts and gestures menu (scripts/menu_prompts.py) are edits of its
# dvdroot_ps4/menu files; GUNDAM VERSUS has none of them, so nothing is laid under the mods.
ui_mods=
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${BB_MODS_DIR:-$data/mods}" --config "${BB_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${BB_MODS_ENABLED:-1}" --ui-mods "$ui_mods")
# A private merged view lasts for this launch, including restarts. Cleanup only our own view.
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    trap '"$PYTHON" -c '\''import shutil,sys; shutil.rmtree(sys.argv[1])'\'' "$mod_game"' EXIT
fi
"$PYTHON" scripts/prepare.py "$game" --out "$out"
# Couch versus (BB_COOP=1, launcher: "Two players"): two games, one per player and controller,
# play each other through Player Match on a server on this PC (GUNDAM VERSUS has no local
# multiplayer: its versus modes are online only). See the end of this script.
if [[ ${BB_COOP:-0} == 1 ]]; then export BB_ONLINE=1; fi
# Play over the local network (BB_LAN, launcher: "Over the network"): host, this PC's matching server
# takes the other PCs' games too (TCP 31313, UDP 31314; each game's own UDP 3658); join, the games
# of this PC play through the host PC's server at BB_LAN_SERVER (its address, port 31313 unless
# given). Each PC needs its own copy of the game.
case ${BB_LAN:-} in
host)
    # The address the other PCs reach this one at: the server tells each game how the others see it.
    lan_ip=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
    [[ -n $lan_ip ]] || lan_ip=$(hostname -i 2>/dev/null | awk '{print $1}')
    [[ -n $lan_ip ]] || { echo 'STOP: this PC has no network address for the other PCs to reach' >&2; exit 1; }
    export BB_ONLINE=1 BB_SHADNET_SERVER=${BB_SHADNET_SERVER:-$lan_ip:31313}
    export SHADPS4_P2P_PORT=${SHADPS4_P2P_PORT:-3658}
    echo "Network play: other PCs join this one at $lan_ip"
    ;;
join)
    [[ -n ${BB_LAN_SERVER:-} ]] || { echo "STOP: network play needs the host PC's address (BB_LAN_SERVER)" >&2; exit 1; }
    server=$BB_LAN_SERVER; [[ $server == *:* ]] || server+=:31313
    export BB_ONLINE=1 BB_SHADNET_SERVER=${BB_SHADNET_SERVER:-$server}
    export SHADPS4_P2P_PORT=${SHADPS4_P2P_PORT:-3658}
    ;;
esac
# Online play (BB_ONLINE=1) also links the game's own libSceNpToolkit2.prx: GUNDAM VERSUS leaves
# PSN to it (sign-in state, callbacks, sessions); offline the runtime answers for it.
modules=(libc.prx libSceFios2.prx)
if [[ ${BB_ONLINE:-0} == 1 && -f $game/sce_module/libSceNpToolkit2.prx ]]; then modules+=(libSceNpToolkit2.prx); fi
"$PYTHON" scripts/link_modules.py "$game" --out "$out" --modules "${modules[@]}"
# BB_DLC_DIR (the launcher's DLC folder): the dumped add-ons of this title, one folder each (e.g.
# SPEXPANSIONDLC03). The runtime reports <user>/addcont/<title id>/<label> as installed (shadPS4's
# layout), so the folder is linked there; a real folder already in its place is left alone.
if [[ -n ${BB_DLC_DIR:-} && -d $BB_DLC_DIR ]]; then
    title=$("$PYTHON" -c 'import sys; sys.path.insert(0, "scripts"); from prepare import sfo
print(sfo(open(sys.argv[1] + "/sce_sys/param.sfo", "rb").read()).get("TITLE_ID", ""))' "$game" 2>/dev/null) || title=
    addcont=${BB_USER_DIR:-$data/user}/addcont
    if [[ $title =~ ^[A-Z]{4}[0-9]{5}$ && ( ! -e $addcont/$title || -L $addcont/$title ) ]]; then
        mkdir -p "$addcont"
        ln -sfn "$(realpath "$BB_DLC_DIR")" "$addcont/$title"
    fi
fi
# BB_REGION=eu|na (the launcher's Region): the game's region for this launch, whatever the dump's
# param.sfo says; it picks the regional text (Spanish from Spain or Latin American).
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${BB_CONTENT_SKU:-full}" --region "${BB_REGION:-}"
# Sizes chosen below for the previous launch are recomputed after an in-game restart.
if [[ ${BB_AUTO_RENDER_RES:-} == 1 ]]; then
    unset BB_RENDER_RES BB_OUTPUT_RES BB_AUTO_RENDER_RES
fi
# BB_RENDER_RES=WxH explicitly sets the game's render resolution (a patch at start).
# Frame rate: BB_FPS=60 (default: the game's own rate, unpatched) or uncap (a frame-rate patch
# from patches/Gundam.xml, once written; scripts/patches.py FPS_PRESETS). BB_PATCHES adds patch
# names ("a;b").
fps=${BB_FPS:-60}
# bbport.ini output_res other than 1080p (720p for the Steam Deck, 1440p, 2160p), or a temporal
# upscaler: the whole game renders at the output size divided by the preset's scale (a patch:
# scripts/patches.py resolution_writes); the upscaler fills the output, the HUD is
# drawn at the output size. Preset and output changes need a restart.
# Live resolution changes keep the guest at 1920x1080 and scale host targets at run time instead
# (output and presets change in the menu without a restart, but post-processing stays at 1080p
# and scene targets are copied back: much slower on the Steam Deck and older GPUs). Chosen by
# BB_LIVE_RES=0/1, else bbport.ini live_resolution=0/1/auto (auto: the GPU check, strong
# discrete GPUs get them); off when unset. 1080p output and TAA always use the live path.
# The GPU tools (bb-gpu-capabilities: the game's GPU, its live resolution support).
if [[ -n ${BB_PROBE:-} ]]; then caps=$(dirname -- "$BB_PROBE")/bb-gpu-capabilities
elif [[ -n ${BB_PREBUILT:-} ]]; then caps=bin/bb-gpu-capabilities
else caps=out/bb-gpu-capabilities; fi
if [[ -z ${BB_RENDER_RES:-} ]]; then
    read -r scaled_render scaled_output < <("$PYTHON" scripts/patches.py --print-scaled --settings "$BB_CONFIG") || true
fi
live=0
if [[ -n ${scaled_output:-} ]]; then
    live=${BB_LIVE_RES:-}
    # Bash builtins only: the AppImage's PATH has no sed/grep (a missing one ended run.sh silently).
    if [[ -z $live && -f $BB_CONFIG ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ $line =~ ^live_resolution=([01]|auto)$ ]] && live=${BASH_REMATCH[1]}
        done < "$BB_CONFIG"
    fi
    if [[ $live == auto ]]; then
        live=$("$caps" --live-resolution 2> >(while IFS= read -r line; do
            [[ $line == *MANGOHUD* ]] || printf '%s\n' "$line"; done >&2)) || live=0
    fi
    [[ $live == 1 ]] || live=0
fi
if [[ $live == 1 ]]; then
    echo "Output ${scaled_output}: live resolution changes (live_resolution=0: startup patch)"
elif [[ -n ${scaled_output:-} ]]; then
    export BB_RENDER_RES=$scaled_render BB_OUTPUT_RES=$scaled_output BB_AUTO_RENDER_RES=1
    export BB_DMEM_MB=${BB_DMEM_MB:-9152}
    echo "Output ${scaled_output}: scene ${scaled_render}, direct memory ${BB_DMEM_MB} MiB (live_resolution=1: live changes)"
fi
# An explicit render size above 1080p skips the scaled path above, but patches.py still grows the
# graphics heap: the direct memory has to follow, or the game stops at start (exit 139).
if [[ -z ${BB_DMEM_MB:-} && ${BB_RENDER_RES:-} =~ ^[[:space:]]*([0-9]+)[[:space:]]*[xX][[:space:]]*([0-9]+)[[:space:]]*$ ]] &&
        (( 10#${BASH_REMATCH[1]} * 10#${BASH_REMATCH[2]} > 1920 * 1080 )); then
    export BB_DMEM_MB=9152
    echo "Render ${BB_RENDER_RES}: direct memory ${BB_DMEM_MB} MiB"
fi
"$PYTHON" scripts/patches.py --out "$out" --fps "$fps" --extra "${BB_PATCHES:-}" --settings "$BB_CONFIG" --game-dir "$game" --render-res "${BB_RENDER_RES:-}" --output-res "${BB_OUTPUT_RES:-}" \
    --patches-dir "${BB_PATCHES_DIR:-$data/patches}" --patches-config "${BB_PATCHES_CONFIG:-$data/patches.json}"
# Background upload of the game's GPU memory into VRAM ahead of use (BufferCache::Preupload):
# 1 = only memory already in VRAM that the game rewrote (no new VRAM), 2 = all of it (~3 GB more
# VRAM), 0 = off. BB_GUEST_GPU_MEMORY=1 puts guest direct memory in GPU-visible dma-buf chunks
# (BB_GUEST_IN_PLACE below implies it: it commits all allocated memory up front and rules out BB_UFFD).
export BB_PREUPLOAD=${BB_PREUPLOAD:-1}
# Memory model and translation. BB_PC_MODEL=1 (the launcher's "Memory model": "Auto" gives every
# GPU the 0.3 model for GUNDAM VERSUS, below; BB_PC_MODEL=1/0 by hand) selects how a PC release would
# work: the GPU uses the game's memory where it is (GPU-visible system memory) and keeps the data
# it reads often in VRAM, the command processor's work is translated rather than emulated
# (BB_GUEST_IN_PLACE=1 and what depends on it). AMD keeps the game's memory in a sparse arena;
# other GPUs get the layer's memory module, which binds it in place and keeps VRAM copies without
# sparse binding (NVIDIA's sparse binding stalls the GPU for seconds); BB_LAYER_MEMORY=1/0 by hand.
# 0 (the default off AMD): the model of 0.3 (VRAM copies of the game's memory, write tracking),
# with the fixes made since. BB_GUEST_IN_PLACE set by hand overrides it.
# BB_AS_0_3=1 (the launcher's developer switch "Synchronisation as in 0.3"): what changed since the
# 0.3 release is reverted for comparisons: the 0.3 memory model, WRITE_DATA/DMA waiting for every
# host copy, the scheduler's concurrent recording check.
# The game's GPU (vendor ID and name), for the choices below that depend on it.
gpu_vendor= gpu_name=
if [[ -z ${BB_PC_MODEL:-} || ( ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} ) ]]; then
    IFS=$'\t' read -r gpu_vendor gpu_name < <("$caps" --device 2> /dev/null) || true
fi
# Memory model left to the port: the 0.3 one on every GPU. GUNDAM VERSUS' compute queues wait
# (WAIT_REG_MEM) for fences of the frame's earlier work; the new model writes those once the GPU
# has really finished it, so each wait is a GPU round trip: a battle ran at ~30 FPS on an
# RX 7800 XT with it, 60 with the 0.3 model (which renders it the same).
if [[ -z ${BB_PC_MODEL:-} ]]; then
    export BB_PC_MODEL=0
    echo "Memory model: the 0.3 one (${gpu_name:-GPU unknown}; BB_PC_MODEL=1: the new one, slower in this game)"
fi
# GCN 3-5 under RADV (Polaris, Vega and their APUs): with HiZ, depth compression showed black
# halos and missing geometry (issues #54, #99); RADV_DEBUG=nohiz fixed it at some cost. Set
# unless RADV_DEBUG is the user's, BB_AUTO_NOHIZ=0: off.
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && $gpu_name == *"(RADV "* ]]; then
    for chip in TONGA ICELAND CARRIZO FIJI STONEY POLARIS VEGA RAVEN RENOIR; do
        if [[ $gpu_name == *"(RADV $chip"* ]]; then
            export RADV_DEBUG=nohiz
            echo "RADV: $gpu_name: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; BB_AUTO_NOHIZ=0: off)"
            break
        fi
    done
fi
if [[ ${BB_AS_0_3:-0} == 1 ]]; then
    export BB_GUEST_IN_PLACE=0 BB_HOST_COPY_WAITS=all BB_PRODUCER_CHECK=1
fi
export BB_GUEST_IN_PLACE=${BB_GUEST_IN_PLACE:-${BB_PC_MODEL:-0}}
# PS4 Pro mode (sceKernelIsNeoMode): GUNDAM VERSUS is "PS4 Pro Enhanced" (param.sfo); on a Pro it
# renders its scene at 1920x1080 instead of the base PS4's 1440x1080 stretched to the screen.
# Holds 60 FPS on an RX 7800 XT. BB_NEO=0: the base PS4.
export BB_NEO=${BB_NEO:-1}
# MangoHud (launcher switch: MANGOHUD=1) must be drawn once. Two overlays on top of each other
# showed doubled, offset text: the Steam Deck's performance overlay (mangoapp, game mode) plus the
# in-game layer, or the AppImage's bundled layer plus a system MangoHud (the layer names differ,
# so the Vulkan loader loads both).
if [[ ${MANGOHUD:-0} == 1 ]]; then
    if pgrep -x mangoapp > /dev/null 2>&1; then
        echo "MangoHud: Steam's performance overlay is on; the in-game MangoHud stays off"
        unset MANGOHUD
        export DISABLE_MANGOHUD=1
    elif grep -qs '"VK_LAYER_MANGOHUD' /usr/share/vulkan/implicit_layer.d/*.json \
            /etc/vulkan/implicit_layer.d/*.json \
            "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json; then
        # The system's own MangoHud (and its config) is used; the bundled one is skipped.
        export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:+$VK_LOADER_LAYERS_DISABLE,}VK_LAYER_MANGOHUD_overlay_64_x86_64
    fi
fi
# Write tracking with userfaultfd write-protection instead of mprotect (no address-space write lock:
# a streaming burst re-protected thousands of pages, ~10-20 ms); falls back to mprotect when the
# kernel lacks it. Off by default for now: the Steam Deck crashed 4 times in 9 minutes with it
# (2026-10-04, PM4 type 0 = zeroed command buffers, heap corruption); 1 turns it on.
export BB_UFFD=${BB_UFFD:-0}
# The command buffers of each submission are copied when the game submits them and decoded from
# the copy: while an area loads the game reused that memory before the GPU thread got to it
# ("Unimplemented PM4 type 0" a few seconds after loading a save). 0 decodes guest memory.
export BB_COPY_GPU_BUFFERS=${BB_COPY_GPU_BUFFERS:-1}
# BB_GPU_WRITE_TWINS=1: a guest write next to small GPU outputs the GPU is still writing (counters,
# compute results, up to BB_GPU_WRITE_TWINS_MAX bytes per page) does not wait for the GPU
# (BufferCache twins). Off for GUNDAM VERSUS: it builds command buffers in pages beside its GPU
# counters, and twins tell the game's writes from the GPU's byte by byte, so a header whose low
# bytes equalled the old ones came back half old (0xc0034802 as 0xbf034802): bad packets, then a
# crash in a battle after 1-3 minutes. Without them it holds 60 FPS all the same.
export BB_GPU_WRITE_TWINS=${BB_GPU_WRITE_TWINS:-0} BB_GPU_WRITE_TWINS_MAX=${BB_GPU_WRITE_TWINS_MAX:-65536}
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=480 ;; 90) export BB_VBLANK_HZ=90 ;; *) export BB_VBLANK_HZ=60 ;; esac
fi
# FSR 4: faster post passes next to the downloaded ones (incremental; tools/fsr4_optimize.sh).
if [[ -z ${BB_PREBUILT:-} && -d fsr4_shaders ]] && command -v spirv-cross >/dev/null; then
    bash tools/fsr4_optimize.sh || echo 'FSR 4: optimized post passes not built' >&2
fi
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/bb-probe}
else
    bash build.sh
    probe=${BB_PROBE:-out/bb-probe}  # BB_PROBE: a wrapper (gdb) around it
fi
# Online play (BB_ONLINE=1; the online module gpu/bbnet): GUNDAM VERSUS' PSN matching goes to a
# shadNet server, gvnet-server (gpu/bbnet/server): BB_SHADNET_SERVER host:port, default one on
# this PC, started for this launch when none listens there. BB_SHADNET_NPID: the player's name.
if [[ ${BB_ONLINE:-0} == 1 ]]; then
    export BB_SHADNET_SERVER=${BB_SHADNET_SERVER:-127.0.0.1:31313}
    export BB_SHADNET_NPID=${BB_SHADNET_NPID:-${USER:-Player}}
    export BB_SHADNET_PASSWORD=${BB_SHADNET_PASSWORD:-local} BB_UPNP=${BB_UPNP:-0}
    [[ -n ${BB_SHADNET_WEBAPI:-} ]] || export BB_SHADNET_WEBAPI=http://${BB_SHADNET_SERVER%:*}:31315
    server_host=${BB_SHADNET_SERVER%:*} server_port=${BB_SHADNET_SERVER##*:}
    server_up() { (exec 9<>"/dev/tcp/$server_host/$server_port") 2>/dev/null; }
    if [[ $server_host == 127.0.0.1 || $server_host == localhost || ${BB_LAN:-} == host ]] && ! server_up; then
        if [[ -n ${BB_PREBUILT:-} ]]; then gvnet=bin/gvnet-server; else gvnet=out/gpu/gvnet-server; fi
        if [[ ! -x $gvnet ]]; then echo "STOP: online play needs $gvnet (the online module is not in this build)" >&2; exit 1; fi
        bind=127.0.0.1; [[ ${BB_LAN:-} == host ]] && bind=0.0.0.0  # the other PCs' games too
        "$gvnet" --bind "$bind" --port "$server_port" </dev/null > >(while IFS= read -r line; do printf '%s\n' "$line"; done) 2>&1 &
        server_pid=$!
        for _ in $(seq 50); do server_up && break; sleep 0.1; done
        game_pid=$$
        ( while kill -0 "$game_pid" 2>/dev/null; do sleep 2; done; kill -TERM "$server_pid" 2>/dev/null ) </dev/null >/dev/null 2>&1 &
        echo "Online: gvnet-server on $bind:$server_port for this launch"
    fi
fi
# Trophies (scripts/trophies.py): the dump's list and icons in the user directory, for the
# runtime's notices and the launcher; BB_TROPHY_KEY (the PS4's trophy key) decrypts the names.
if trophies=$("$PYTHON" scripts/trophies.py prepare --game "$original_game" \
        --user "${BB_USER_DIR:-$data/user}" --language "${BB_LANGUAGE:-1}") && [[ -n $trophies ]]; then
    export BB_TROPHY_DIR=$trophies
fi
# Discord Rich Presence, off by default: BB_DISCORD=1 with BB_DISCORD_CLIENT_ID, the id of a
# Discord application of your own (its name is what Discord shows as played), shows "GUNDAM VERSUS" and
# the time played (scripts/discord_presence.py). $$ becomes the game below (exec), and the helper
# ends with it.
if [[ ${BB_DISCORD:-0} == 1 && -n ${BB_DISCORD_CLIENT_ID:-} ]]; then
    "$PYTHON" scripts/discord_presence.py --pid $$ </dev/null &
fi
probe_args=("$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game" --user "${BB_USER_DIR:-$data/user}" --timeout "${BB_TIMEOUT:-0}" "$@")
# Couch versus: player 1 plays from the usual saves, player 2 from $data/user-p2 (BB_COOP_USER2),
# a copy of player 1's saves the first time. Each game takes its own gamepad (BB_GAMEPAD_SLOT: the
# launcher's choice first, then the others; a missing one leaves the keyboard of a focused window)
# and keeps reading it without the focus. Layout (BB_COOP_LAYOUT): split, the two windows side by
# side on one monitor (BB_DISPLAY); displays, each game full screen on its own monitor
# (BB_DISPLAY and BB_COOP_DISPLAY2, default 1 and 2). Player 2's sound is off unless
# BB_COOP_AUDIO2=1 (the same music twice otherwise). Names: BB_COOP_NAME1/2. Closing either game
# ends both.
if [[ ${BB_COOP:-0} == 1 ]]; then
    user1=${BB_USER_DIR:-$data/user} user2=${BB_COOP_USER2:-$data/user-p2}
    if [[ ! -d $user2/savedata && -d $user1/savedata ]]; then
        mkdir -p "$user2" && cp -a "$user1/savedata" "$user2/"
        echo "Couch versus: player 2's saves start as a copy of player 1's ($user2)"
    fi
    mkdir -p "$user2"
    layout=${BB_COOP_LAYOUT:-split}
    common=(BB_PLAYERS=1 BB_PAD_BACKGROUND=1)
    one=("${common[@]}" BB_GAMEPAD_SLOT=1 SHADPS4_P2P_PORT=${BB_COOP_PORT1:-3658} BB_SHADNET_NPID=${BB_COOP_NAME1:-Player1})
    two=("${common[@]}" BB_GAMEPAD_SLOT=2 SHADPS4_P2P_PORT=${BB_COOP_PORT2:-3659} BB_SHADNET_NPID=${BB_COOP_NAME2:-Player2}
         BB_USER_DIR=$user2 BB_GPU_USER_DIR=$user2)
    if [[ $layout == displays ]]; then
        one+=(BB_DISPLAY=${BB_DISPLAY:-1} BB_FULLSCREEN=1); two+=(BB_DISPLAY=${BB_COOP_DISPLAY2:-2} BB_FULLSCREEN=1)
    else
        one+=(BB_WINDOW_SPLIT=left BB_FULLSCREEN=0); two+=(BB_WINDOW_SPLIT=right BB_FULLSCREEN=0)
    fi
    [[ ${BB_COOP_AUDIO2:-0} == 1 ]] || two+=(BB_AUDIO=none)
    # Player 2's own copies of the files a test or the log names (frame log, pad file, dumps).
    [[ -n ${BB_FRAME_LOG:-} ]] && two+=(BB_FRAME_LOG=${BB_FRAME_LOG%.frames.csv}.p2.frames.csv)
    [[ -n ${BB_READBACK_LOG:-} ]] && two+=(BB_READBACK_LOG=${BB_READBACK_LOG%.readbacks.csv}.p2.readbacks.csv)
    [[ -n ${BB_PAD_FILE:-} ]] && two+=(BB_PAD_FILE=${BB_COOP_PAD_FILE2:-$BB_PAD_FILE.p2})
    [[ -n ${BB_PRESENT_DUMP_TRIGGER:-} ]] && two+=(BB_PRESENT_DUMP_TRIGGER=$BB_PRESENT_DUMP_TRIGGER.p2)
    [[ -n ${BB_DUMP_DIR:-} ]] && two+=(BB_DUMP_DIR=$BB_DUMP_DIR/p2)
    [[ -n ${BB_PEEK_FILE:-} ]] && two+=(BB_PEEK_FILE=$BB_PEEK_FILE.p2)
    args2=("$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game" --user "$user2" --timeout "${BB_TIMEOUT:-0}" "$@")
    echo "Couch versus: ${BB_COOP_NAME1:-Player1} (gamepad 1) and ${BB_COOP_NAME2:-Player2} (gamepad 2), layout $layout"
    echo "  In both games: PLAY > PLAYER MATCH. One creates a room (Room type: 1 on 1), the other finds it with Search for Room (same Room type)."
    env "${one[@]}" "$probe" "${probe_args[@]}" &
    pid1=$!
    env "${two[@]}" "$probe" "${args2[@]}" > >(while IFS= read -r line; do printf '[P2] %s\n' "$line"; done) 2>&1 &
    pid2=$!
    trap 'kill -TERM "$pid1" "$pid2" 2>/dev/null || true' TERM INT
    status=0
    wait -n "$pid1" "$pid2" || status=$?
    kill -TERM "$pid1" "$pid2" 2>/dev/null || true
    wait "$pid1" "$pid2" 2>/dev/null || true
    exit "$status"
fi
if [[ -n ${mod_game:-} ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    # An interrupted wait must finish the child before removing its mounted view.
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    exit "$mod_status"
fi
exec "$probe" "${probe_args[@]}"
