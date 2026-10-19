// SPDX-License-Identifier: GPL-2.0-or-later
// Game profile cameras (gpu/games): a scene camera block captured from the game parses into the
// motion shaders' convention. GUNDAM VERSUS: the 272-byte block of a Free Battle frame (frame
// capture, 1920x1080), camera at (467.473, 20.4028, -293.757).
//   cmake target game-camera-test, or: g++ -std=c++20 -Igpu/games tests/test_game_cameras.cpp gpu/games/gundam.cpp gpu/games/maxiboost.cpp
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_profile.h"

namespace Game {
extern const Profile GundamProfile;
extern const Profile MaxiboostProfile;
}

namespace {
bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

// out.view * (p, 1): view space, z the distance in front.
void ToView(const Game::SceneCamera& camera, const float p[3], float v[3]) {
    for (int r = 0; r < 3; ++r) {
        v[r] = camera.view[r * 4 + 0] * p[0] + camera.view[r * 4 + 1] * p[1] +
               camera.view[r * 4 + 2] * p[2] + camera.view[r * 4 + 3];
    }
}

void TestGundam() {
    const float block[68] = {
        -0.649218f, -0.147138f, 0.746235f, 0, 0, 0.98111f, 0.193449f, 0,
        -0.760602f, 0.12559f, -0.636955f, 0, 80.0603f, 85.6585f, -539.901f, 1,
        -0.649218f, 1.49012e-08f, -0.760602f, 0, -0.147138f, 0.98111f, 0.12559f, -0.0f,
        0.746235f, 0.193449f, -0.636955f, -0.0f, 467.473f, 20.4028f, -293.757f, 1,
        1.32517f, 0, 0, 0, 0, 2.35585f, 0, 0,
        0, 0, -1.0001f, -1, 0, 0, -1.0001f, 0,
        0.754622f, -0.0f, -0.0f, -0.0f, -0.0f, 0.424475f, -0.0f, -0.0f,
        -0.0f, 0, -0.0f, -0.9999f, 0, -0.0f, -1, 1,
        1920, 1080, 0, 0};
    float data[68];
    std::memcpy(data, block, sizeof(data));
    const std::uint32_t one = 1;
    std::memcpy(&data[66], &one, sizeof(one));
    const Game::UpscalerHooks* hooks = Game::GundamProfile.upscaler;
    assert(hooks && hooks->camera && hooks->camera_constants_size == sizeof(data));
    assert(hooks->scene_scaled_into_display && hooks->scene_pass_targets == 4);
    Game::SceneCamera camera;
    assert(hooks->camera(data, camera));
    assert(camera.width == 1920 && camera.height == 1080);
    assert(Near(camera.proj_x, 1.32517f, 1e-6f) && Near(camera.proj_y, 2.35585f, 1e-6f));
    // ndc z 0 at the near plane (1), 1 at the far plane (10001; 1 - p22 in floats is ~0.1% off).
    assert(Near(camera.near_plane, 1.0f, 1e-3f) && Near(camera.far_plane, 10001.0f, 50.0f));
    assert(Near(camera.ndc_z + camera.ndc_z_over_z / camera.near_plane, 0.0f, 1e-5f));
    // The camera's position is the view's origin; 10 units ahead (its -z axis) is z = 10.
    const float position[3] = {467.473f, 20.4028f, -293.757f};
    float v[3];
    ToView(camera, position, v);
    assert(Near(v[0], 0.0f, 2e-3f) && Near(v[1], 0.0f, 2e-3f) && Near(v[2], 0.0f, 2e-3f));
    const float ahead[3] = {position[0] - 10 * 0.746235f, position[1] - 10 * 0.193449f,
                            position[2] + 10 * 0.636955f};
    ToView(camera, ahead, v);
    assert(Near(v[0], 0.0f, 2e-3f) && Near(v[1], 0.0f, 2e-3f) && Near(v[2], 10.0f, 2e-3f));
    // Not a scene camera: an orthographic block (shadows) or a size of 0.
    data[32 + 11] = 0.0f;
    assert(!hooks->camera(data, camera));
    data[32 + 11] = -1.0f;
    data[64] = 0.0f;
    assert(!hooks->camera(data, camera));
}
// MAXIBOOST ON: the view-projection of a Free Battle frame's terrain (frame capture, 1920x1080)
// and its view matrix from the 544-byte scene block ([68]..[83], row vectors).
void TestMaxiboost() {
    const float vp[16] = {-0.670502f, 0.598049f, 0.846835f, 0.846751f, 0, 2.41291f, -0.271213f,
                          -0.271185f, 1.24049f, 0.323254f, 0.457727f, 0.457681f, 156.585f,
                          134.742f, 375.277f, 379.239f};
    const float view[16] = {-0.475499f, 0.238566f, -0.846751f, 0, 0, 0.962527f, 0.271185f, 0,
                            0.879716f, 0.128948f, -0.457681f, 0, 111.045f, 53.7498f, -379.239f, 1};
    const Game::UpscalerHooks* hooks = Game::MaxiboostProfile.upscaler;
    assert(hooks && hooks->camera && hooks->camera_constants_size == sizeof(vp));
    Game::SceneCamera camera;
    assert(hooks->camera(vp, camera));
    assert(Near(camera.proj_y / camera.proj_x, 16.0f / 9.0f, 1e-3f));
    // The derived view is the game's (column-vector rows, z negated).
    for (int r = 0; r < 3; ++r) {
        const float sign = r == 2 ? -1.0f : 1.0f;
        for (int c = 0; c < 4; ++c) {
            assert(Near(camera.view[r * 4 + c], sign * view[c * 4 + r], 2e-3f));
        }
    }
    assert(camera.near_plane > 0.0f && camera.far_plane > camera.near_plane);
    // A shadow pass's orthographic matrix is no scene camera.
    float ortho[16] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 0.001f, 0, 1, 2, 0.5f, 1};
    assert(!hooks->camera(ortho, camera));
}
} // namespace

int main() {
    TestGundam();
    TestMaxiboost();
    std::printf("PASS: MAXIBOOST ON scene camera\n");
    std::printf("PASS: GUNDAM VERSUS scene camera\n");
    return 0;
}
