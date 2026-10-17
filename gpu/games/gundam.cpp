// SPDX-License-Identifier: GPL-2.0-or-later
// GUNDAM VERSUS (PS4, 2017; update 1.13): what the translator knows about it. Bandai Namco's own
// engine ("vdk") with Havok and Silicon Studio's YEBIS 3 post-processing; the frame is drawn by the
// graphics queue with up to twelve compute queues beside it, which wait (WAIT_REG_MEM) on fences of
// each other's work every frame. Its frame is not mapped for the temporal upscalers yet.
#include "game_profile.h"

namespace Game {
namespace {
/// The retail releases (scripts/game_check.py SUPPORTED_TITLES).
constexpr const char* Serials[] = {"CUSA08790"};
} // namespace

extern const Profile GundamProfile = {
    .name = "GUNDAM VERSUS",
    .serials = Serials,
};

} // namespace Game
