// SPDX-License-Identifier: GPL-2.0-or-later
// GUNDAM VERSUS (PS4, 2017; update 1.13): what the translator knows about it. Bandai Namco's own
// engine ("vdk") with Havok and Silicon Studio's YEBIS 3 post-processing; the frame is drawn by the
// graphics queue with up to twelve compute queues beside it, which wait (WAIT_REG_MEM) on fences of
// each other's work every frame.
//
// The frame (frame capture, scripts/patches.py): a depth pre-pass, a G-buffer pass of four color
// targets (three RGBA8 sRGB, one RGBA16F) with a D32S8 depth, the lighting in compute (two
// B10G11R11 targets), forward and effect passes (effects at half size), the YEBIS bloom chain and
// tonemap into an RGBA8 target of the scene size, then one pass scaling that into the display
// buffer, where the HUD is drawn at the display size. The scene and display sizes are the game's
// own (patched at start for other output sizes and the upscaler presets).
#include "game_profile.h"

namespace Game {
namespace {
/// The retail releases (scripts/game_check.py SUPPORTED_TITLES).
constexpr const char* Serials[] = {"CUSA08790"};

/// The scene camera (272 bytes, bound to the depth pre-pass and the G-buffer pass): [0] view,
/// [16] inverse view, [32] projection, [48] inverse projection (4x4 each, row vectors: translation
/// in the last row), [64] render width and height, then an integer 1 and 0. The projection is
/// OpenGL's (view space looks down -z): ndc z = -p22 + p32 / z with z the distance in front.
bool Camera(const float* d, SceneCamera& out) {
    const float width = d[64], height = d[65];
    const float* p = d + 32;
    if (width < 64.0f || height < 64.0f || width > 8192.0f || height > 8192.0f ||
        p[11] != -1.0f || p[15] != 0.0f || d[15] != 1.0f || !(p[0] > 0.0f) || !(p[5] > 0.0f)) {
        return false;
    }
    // Rows [R | t] of the view for column vectors, z negated (forward positive).
    for (int r = 0; r < 3; ++r) {
        const float sign = r == 2 ? -1.0f : 1.0f;
        for (int c = 0; c < 4; ++c) {
            out.view[r * 4 + c] = sign * d[c * 4 + r];
        }
    }
    out.proj_x = p[0];
    out.proj_y = p[5];
    out.ndc_z = -p[10];
    out.ndc_z_over_z = p[14];
    // ndc z 0 (near) and 1 (far): z = p32 / (ndc z + p22). The game's is 1 to 10001.
    const float a = -p[10], b = p[14];
    out.near_plane = a != 0.0f ? -b / a : 0.0f;
    out.far_plane = a != 1.0f ? b / (1.0f - a) : 0.0f;
    if (!(out.near_plane > 0.0f) || !(out.far_plane > out.near_plane)) {
        out.near_plane = 0.1f;
        out.far_plane = 10000.0f;
    }
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    return true;
}

constexpr UpscalerHooks Hooks = {
    .camera_constants_size = 272,
    .camera = Camera,
    .scene_pass_targets = 4,
    .scene_scaled_into_display = true,
};
} // namespace

extern const Profile GundamProfile = {
    .name = "GUNDAM VERSUS",
    .serials = Serials,
    .upscaler = &Hooks,
};

} // namespace Game
