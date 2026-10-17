// SPDX-License-Identifier: GPL-2.0-or-later
// Game profiles: what the translator knows about one game, kept out of its core. The core (the
// command processor, the caches, the shader recompiler) runs any game on its generic paths and
// asks the active profile only for the rest: hooks in the game's own code that announce its
// writes earlier than a write trap would, guest code known to write per-frame data, shaders with
// a known purpose. A game without a profile runs on the generic paths alone. One file per game
// (games/<game>.cpp) defines its profile; game_profile.cpp lists them.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace Game {

/// Guest addresses [begin, end).
struct CodeRange {
    std::uint64_t begin = 0, end = 0;
};

/// What the core offers a profile's hooks.
struct CoreServices {
    /// The game handed out GPU memory [address, address + size): what the GPU had there is dead.
    void (*fresh_range)(std::uint64_t address, std::uint64_t size) = nullptr;
};

/// The scene camera of a frame, in the motion shaders' convention: view space x right, y up,
/// z forward (positive); world to view as 3x4 rows [R | t].
struct SceneCamera {
    std::array<float, 12> view{};
    float proj_x = 0.0f, proj_y = 0.0f; ///< ndc.xy = proj * view.xy / view.z (ndc +y up)
    float ndc_z = 0.0f, ndc_z_over_z = 0.0f; ///< ndc z = ndc_z + ndc_z_over_z / view.z
    float near_plane = 0.0f, far_plane = 0.0f;
    std::uint32_t width = 0, height = 0; ///< the render size the constants name
};

/// How a game's frame meets the temporal upscalers (docs/upscaler.md): where its scene camera
/// is, which pass holds the scene depth, and which draw starts the HUD.
struct UpscalerHooks {
    /// The size of the bound constant buffer holding the scene camera.
    std::uint32_t camera_constants_size = 0;
    /// The scene camera from such a buffer, or false when it is not the scene's.
    bool (*camera)(const float* data, SceneCamera& out) = nullptr;
    /// The pass whose depth is the scene depth has at least this many color targets.
    std::uint32_t scene_pass_targets = 1;
    /// The vertex shader of the first HUD draw: the scene is upscaled before it and the HUD drawn
    /// at the output size.
    std::uint64_t hud_vertex_shader = 0;
    /// Other vertex shaders that draw the HUD (any of them starts it, when it starts the frame's
    /// HUD in place of hud_vertex_shader).
    std::array<std::uint64_t, 4> hud_vertex_shaders{};
    /// The vertex shader of the pass that finishes the scene (post-processing combined): every
    /// later draw into its target is the HUD, whichever shader starts it.
    std::uint64_t scene_done_vertex_shader = 0;
    /// The vertex shader of a pass copying the finished frame (scene and HUD) into another image
    /// the display pass reads later: that image is kept at the output size (a relay).
    std::uint64_t frame_copy_vertex_shader = 0;
};

struct Profile {
    const char* name = nullptr;
    /// The param.sfo TITLE_IDs it is for.
    std::span<const char* const> serials;
    /// Hooks in the game's code (each checks the bytes it replaces: another build of the game is
    /// left alone). They make the core learn of writes sooner; it does not depend on them for
    /// what it shows (write traps catch what nobody announces).
    void (*install_hooks)(const CoreServices& services) = nullptr;
    /// Guest code known to write per-frame data into GPU memory: the blocks it writes stay in
    /// the game's memory from its first write instead of after a few frames.
    std::span<const CodeRange> dynamic_writers;
    /// A compute shader (GCN program hash) that only copies buffers, into memory the translator
    /// reads on the CPU: its stores go through to the game's memory; 0 = none known.
    std::uint64_t buffer_copy_shader = 0;
    /// Edits to the game's code made once, before it runs (the image is still writable): hooks
    /// at fixed addresses of one executable, such as Bloodborne's settings menu. nullptr = none.
    void (*patch_image)(unsigned char* image, std::uint64_t size) = nullptr;
    /// The game's malloc table in its libc.prx is where the heap watch expects it
    /// (BbHeapSites: Bloodborne's link layout); off for other games.
    bool heap_watch = false;
    /// The temporal upscalers' view of the frame; nullptr: Bloodborne's built-in one.
    const UpscalerHooks* upscaler = nullptr;
};

/// Chooses the profile for the game by its TITLE_ID (bbgpu_init). BB_GAME_PROFILE=none runs the
/// game as an unknown one (no profile), for checking the generic paths.
void Select(const char* serial);

/// The active profile, or nullptr (an unknown game, or BB_GAME_PROFILE=none).
const Profile* Active();

/// The active profile's buffer copy shader, or 0.
inline std::uint64_t BufferCopyShader() {
    const Profile* profile = Active();
    return profile ? profile->buffer_copy_shader : 0;
}

/// Whether the active profile has the heap watch (BbHeapSites).
inline bool HeapWatch() {
    const Profile* profile = Active();
    return profile && profile->heap_watch;
}

/// The active profile's upscaler hooks, or nullptr (Bloodborne's built-in ones, or none).
inline const UpscalerHooks* Upscaler() {
    const Profile* profile = Active();
    return profile ? profile->upscaler : nullptr;
}

/// Whether guest_rip is in code the active profile knows to write per-frame data.
bool IsDynamicWriter(std::uint64_t guest_rip);

} // namespace Game
