/**
 * pokersmash-nx (2026-11-25, the user: "could you make the white icon the switch user profile icon (first user on
 * system by default) give option in in game config to switch it"): a host source for the gamer picture that
 * XamReadTileToTexture gives a game (sdk/src/kernel/xam/xam_user.cpp). Without one, or when it gives nothing, the
 * texture is white as before. The app sets it once at start (app/src/gamer_picture.cpp: a Switch user's icon by the key
 * gamer_picture_user, or an image file by gamer_picture_file; both off in the base).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace rex::system::xam {

// width x height pixels, row by row, each 0xAARRGGBB; true when it gave a picture of exactly width x height. Called on
// the thread that reads the tile, as often as the game reads it.
using GamerPictureProvider = bool (*)(uint32_t width, uint32_t height, std::vector<uint32_t>& argb);

void SetGamerPictureProvider(GamerPictureProvider provider);
GamerPictureProvider GetGamerPictureProvider();

}  // namespace rex::system::xam
