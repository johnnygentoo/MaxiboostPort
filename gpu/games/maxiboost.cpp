// SPDX-License-Identifier: GPL-2.0-or-later
// MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON (PS4, 2020; update 1.15): what the translator knows
// about it. The same Bandai Namco engine as GUNDAM VERSUS, with a forward renderer: a depth
// pre-pass, two 2048x2048 shadow maps, one pass drawing the lit scene into an RGBA8 target with a
// D32S8 depth, the scene copied into an RGBA8 target where effects (half size) and transparent
// objects are drawn, a bloom chain, then one pass tonemapping that into the display buffer, where
// the HUD is drawn at the display size (frame capture, scripts/patches.py for the sizes).
#include "game_profile.h"

#include <cmath>

namespace Game {
namespace {
/// The retail releases (scripts/game_check.py SUPPORTED_TITLES).
constexpr const char* Serials[] = {"CUSA18758"};

/// The view-projection the scene's static geometry is drawn with (64 bytes, row vectors:
/// clip = (p, 1) * m). Its world is the identity, so view and projection separate: the
/// projection is OpenGL's (clip w = -view z), its columns 0 and 1 are the view's x and y axes
/// scaled by the projection's x and y scales (unit axes), column 3 the view's negated z axis.
/// Shadow passes bind an orthographic one of the same size (column 3 = 0, 0, 0, 1): refused.
bool Camera(const float* d, SceneCamera& out) {
    auto column = [&](int c, float v[4]) {
        for (int r = 0; r < 4; ++r) v[r] = d[r * 4 + c];
    };
    float c0[4], c1[4], c2[4], c3[4];
    column(0, c0), column(1, c1), column(2, c2), column(3, c3);
    auto length3 = [](const float v[4]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    const float zl = length3(c3), px = length3(c0), py = length3(c1);
    // The scene's camera is 16:9 (the game's other perspective views, such as the pilot
    // portraits, are not).
    if (std::fabs(zl - 1.0f) > 1e-3f || !(px > 0.1f) || !(py > 0.1f) ||
        std::fabs(py / px - 16.0f / 9.0f) > 0.01f) {
        return false;
    }
    float axis[3][4]; // the view's x, y and z axes (columns of the row-vector view matrix)
    for (int r = 0; r < 4; ++r) {
        axis[0][r] = c0[r] / px;
        axis[1][r] = c1[r] / py;
        axis[2][r] = -c3[r];
    }
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    if (std::fabs(dot3(axis[0], axis[1])) > 1e-3f || std::fabs(dot3(axis[0], axis[2])) > 1e-3f ||
        std::fabs(dot3(axis[1], axis[2])) > 1e-3f) {
        return false; // not a camera of a world at the identity
    }
    // Column 2 = p22 * view z axis + p32 * (0, 0, 0, 1).
    const float p22 = dot3(c2, axis[2]);
    const float p32 = c2[3] - p22 * axis[2][3];
    // Rows [R | t] of the view for column vectors, z negated (forward positive).
    for (int r = 0; r < 3; ++r) {
        const float sign = r == 2 ? -1.0f : 1.0f;
        for (int c = 0; c < 4; ++c) {
            out.view[r * 4 + c] = sign * axis[r][c];
        }
    }
    out.proj_x = px;
    out.proj_y = py;
    out.ndc_z = -p22;
    out.ndc_z_over_z = p32;
    const float a = -p22, b = p32;
    out.near_plane = a != 0.0f ? -b / a : 0.0f;
    out.far_plane = a != 1.0f ? b / (1.0f - a) : 0.0f;
    if (!(out.near_plane > 0.0f) || !(out.far_plane > out.near_plane)) {
        out.near_plane = 1.0f;
        out.far_plane = 10000.0f;
    }
    out.width = 0; // not in the block: the upscaler's render size
    out.height = 0;
    return true;
}

constexpr UpscalerHooks Hooks = {
    .camera_constants_size = 64,
    .camera = Camera,
    .scene_pass_targets = 1,
    .scene_scaled_into_display = true,
    .scene_depth_at_render_size = true,
};
} // namespace

extern const Profile MaxiboostProfile = {
    .name = "MAXIBOOST ON",
    .serials = Serials,
    .upscaler = &Hooks,
};

} // namespace Game
