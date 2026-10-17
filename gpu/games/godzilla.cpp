// SPDX-License-Identifier: GPL-2.0-or-later
// GODZILLA (PS4, 2015; update 1.01): what the translator knows about its code. Addresses are those
// of the 1.01 executable (scripts/game_check.py makes sure that is the one running). Empty for now:
// the game runs on the core's generic paths; hooks and known shaders are added here as they are
// found.
#include "game_profile.h"

namespace Game {
namespace {
/// The retail releases (scripts/game_check.py SUPPORTED_TITLES).
constexpr const char* Serials[] = {"CUSA02046"};
} // namespace

extern const Profile GodzillaProfile = {
    .name = "GODZILLA",
    .serials = Serials,
};

} // namespace Game
