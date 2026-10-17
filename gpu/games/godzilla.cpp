// SPDX-License-Identifier: GPL-2.0-or-later
// GODZILLA (PS4, 2015; update 1.01): what the translator knows about its frame. The game is a
// PhyreEngine forward renderer: one pass draws the lit scene into an RGBA8 target with a D32
// depth (after a 2048x2048 shadow map), effects go over it, a bloom chain and a combine pass
// write the post-processed scene into a second RGBA8 target, the HUD is drawn over that one, and
// a last pass copies it out (frame capture, docs/upscaler.md).
#include "game_profile.h"

#include <cmath>

namespace Game {
namespace {
/// The retail releases (scripts/game_check.py SUPPORTED_TITLES).
constexpr const char* Serials[] = {"CUSA02046"};

/// PhyreEngine's scene constants (336 bytes, bound to the scene's vertex and pixel shaders):
/// [0] view-projection, [16] view, [32] projection, [48] inverse view (4x4 each, row vectors:
/// translation in the last row), [64] camera position, [72] near, far, render width, height,
/// 1 / width, 1 / height. The projection is OpenGL's: view space looks down -z, ndc z in -1..1.
bool Camera(const float* d, SceneCamera& out) {
    const float near_plane = d[72], far_plane = d[73], width = d[74], height = d[75];
    if (!(near_plane > 0.0f) || !(far_plane > near_plane) || width < 64.0f || height < 64.0f ||
        std::abs(d[76] * width - 1.0f) > 1e-3f || std::abs(d[77] * height - 1.0f) > 1e-3f ||
        d[32 + 11] != -1.0f || d[32 + 15] != 0.0f) {
        return false;
    }
    const float* m = d + 16; // view: m[r * 4 + c], v' = v * m
    // Rows [R | t] of the view for column vectors, z negated (forward positive).
    for (int r = 0; r < 3; ++r) {
        const float sign = r == 2 ? -1.0f : 1.0f;
        for (int c = 0; c < 4; ++c) {
            out.view[r * 4 + c] = sign * m[c * 4 + r];
        }
    }
    const float* p = d + 32;
    out.proj_x = p[0];
    out.proj_y = p[5];
    // clip z = view z * p22 + p32, clip w = -view z: ndc z = -p22 + p32 / z with z = -view z.
    out.ndc_z = -p[10];
    out.ndc_z_over_z = p[14];
    out.near_plane = near_plane;
    out.far_plane = far_plane;
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    return true;
}

constexpr UpscalerHooks Hooks = {
    .camera_constants_size = 336,
    .camera = Camera,
    .scene_pass_targets = 1,
    // The HUD pass's first vertex shader when the G-Force operator panel is up; without it the
    // HUD starts with another one (0x1c3cc5dd), hence scene_done_vertex_shader.
    .hud_vertex_shader = 0xb9725368,
    .hud_vertex_shaders = {0x1c3cc5dd},
    // The combine pass (bloom and the scene into the second RGBA8 target), then the copy of
    // the finished frame that the next frame's first pass puts into the display buffer.
    .scene_done_vertex_shader = 0x6fbcebea,
    .frame_copy_vertex_shader = 0x2ccea2b7,
};
} // namespace

extern const Profile GodzillaProfile = {
    .name = "GODZILLA",
    .serials = Serials,
    .upscaler = &Hooks,
};

} // namespace Game
