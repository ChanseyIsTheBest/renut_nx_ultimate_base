// pokersmash-nx (2026-11-25, the user: "could you make the white icon the switch user profile icon (first user on
// system by default) give option in in game config to switch it"): the gamer picture by the player's name in a game's
// HUD (the Xbox 360's gamer tile, which XamReadTileToTexture gives: white in the SDK) from a Switch user's icon or an
// image file. renut-ultimate: both keys off by default (it changes a picture); a game turns gamer_picture_user on in
// its own toml (Poker Smash: 0).
//   gamer_picture_user   -1 off (the white square, the base's default), 0 the console's first user, 1-7 the next ones:
//                        the order of the console's user list (libnx accountListAllUsers). Switch only.
//   gamer_picture_file   an image file (PNG or JPEG, at most 4096 x 4096) used instead of a Switch user's icon; on the
//                        PC the only source.
// The icon is the account service's JPEG (256 x 256), decoded by stb_image (a static JPEG / PNG-only copy here: the
// SDK's own is PNG-only) and scaled to the game's tile (64 x 64) by area averaging; made once (both keys are read at
// start). The SDK side is rex/system/xam/gamer_picture.h. Poker Smash's game-menu row (Settings: Gamer picture, the
// console's users by nickname) is not taken: game_menu.h's SetGameMenuRows is the one slot a game's own rows use, and
// the row's text has no translations.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xam/gamer_picture.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../sdk/thirdparty/stb/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_INT32(gamer_picture_user, -1, "Profile",
                     "The gamer picture by the player's name in a game (the Xbox 360's gamer tile): a Switch user's "
                     "icon. -1 off (the white square, the base's default), 0 the console's first user, 1-7 the next "
                     "ones (the console's user list). Read at start")
    .range(-1, 7)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(gamer_picture_file, "", "Profile",
                      "An image file (PNG or JPEG, at most 4096 x 4096) for the gamer picture in place of a Switch "
                      "user's icon (on the PC the only source); empty: none. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr int kMaxSide = 4096;  // a picture's header can name any size: nothing larger is decoded

struct Picture {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width x height x 4
};

bool Decode(const std::vector<uint8_t>& bytes, Picture& out) {
  if (bytes.empty() || bytes.size() > size_t(16) << 20)
    return false;
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n) || w <= 0 || h <= 0 || w > kMaxSide ||
      h > kMaxSide)
    return false;
  stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
  if (!pixels)
    return false;
  out.width = w;
  out.height = h;
  out.rgba.assign(pixels, pixels + size_t(w) * h * 4);
  stbi_image_free(pixels);
  return true;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  bool ok = std::fseek(f, 0, SEEK_END) == 0;
  const long size = ok ? std::ftell(f) : -1;
  ok = ok && size > 0 && size < (16 << 20) && std::fseek(f, 0, SEEK_SET) == 0;
  if (ok) {
    bytes.resize(size_t(size));
    ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
  }
  std::fclose(f);
  return ok;
}

#if REX_PLATFORM_SWITCH
// The icon of the console's index-th user (its JPEG) and the user's nickname, or false.
bool SwitchUserIcon(int index, std::vector<uint8_t>& jpeg, std::string& nickname) {
  if (R_FAILED(accountInitialize(AccountServiceType_Application)))
    return false;
  bool ok = false;
  AccountUid uids[ACC_USER_LIST_SIZE] = {};
  s32 total = 0;
  if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && index >= 0 && index < total &&
      index < ACC_USER_LIST_SIZE) {
    AccountProfile profile;
    if (R_SUCCEEDED(accountGetProfile(&profile, uids[index]))) {
      AccountProfileBase base = {};
      if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)))
        nickname.assign(base.nickname, strnlen(base.nickname, sizeof(base.nickname)));
      u32 size = 0;
      if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0 && size < (1u << 20)) {
        jpeg.resize(size);
        u32 got = 0;
        ok = R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), jpeg.size(), &got)) && got > 0 &&
             got <= size;
        jpeg.resize(ok ? got : 0);
      }
      accountProfileClose(&profile);
    }
  }
  accountExit();
  return ok;
}
#endif

// The picture the keys name, made once (the first time a game reads a gamer tile).
const Picture& CurrentPicture() {
  static const Picture picture = [] {
    Picture p;
    const std::string file = REXCVAR_GET(gamer_picture_file);
    const int user = REXCVAR_GET(gamer_picture_user);
    std::vector<uint8_t> bytes;
    std::string what;
    bool read = false;
    if (!file.empty()) {
      what = "file " + file;
      read = ReadFile(file, bytes);
    } else if (user >= 0) {
      what = "user " + std::to_string(user);
#if REX_PLATFORM_SWITCH
      std::string nickname;
      read = SwitchUserIcon(user, bytes, nickname);
      what = "the console's user " + std::to_string(user) + " (" + nickname + ")";
#endif
    } else {
      return p;  // both keys off: no picture, no log line
    }
    if (read && Decode(bytes, p))
      REXLOG_INFO("[gamer picture] {}: {}x{} (gamer_picture_user / gamer_picture_file)", what, p.width, p.height);
    else
      REXLOG_INFO("[gamer picture] {}: no picture (the white square)", what);
    return p;
  }();
  return picture;
}

// rex/system/xam/gamer_picture.h: the picture scaled to the tile by area averaging (each tile pixel the mean of the
// source pixels it covers), as 0xAARRGGBB.
bool Provide(uint32_t width, uint32_t height, std::vector<uint32_t>& argb) {
  const Picture& p = CurrentPicture();
  if (p.rgba.empty() || !width || !height || width > 4096 || height > 4096)
    return false;
  argb.assign(size_t(width) * height, 0xFFFFFFFFu);
  for (uint32_t y = 0; y < height; ++y) {
    const int y0 = int(uint64_t(y) * uint64_t(p.height) / height);
    const int y1 = std::max(y0 + 1, int(uint64_t(y + 1) * uint64_t(p.height) / height));
    for (uint32_t x = 0; x < width; ++x) {
      const int x0 = int(uint64_t(x) * uint64_t(p.width) / width);
      const int x1 = std::max(x0 + 1, int(uint64_t(x + 1) * uint64_t(p.width) / width));
      uint64_t sum[4] = {0, 0, 0, 0}, n = 0;
      for (int sy = y0; sy < y1 && sy < p.height; ++sy)
        for (int sx = x0; sx < x1 && sx < p.width; ++sx) {
          const uint8_t* s = &p.rgba[(size_t(sy) * p.width + sx) * 4];
          for (int c = 0; c < 4; ++c)
            sum[c] += s[c];
          ++n;
        }
      if (!n)
        continue;
      const uint32_t r = uint32_t(sum[0] / n), g = uint32_t(sum[1] / n), b = uint32_t(sum[2] / n),
                     a = uint32_t(sum[3] / n);
      argb[size_t(y) * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  return true;
}

struct Register {
  Register() { rex::system::xam::SetGamerPictureProvider(&Provide); }
} g_register;

}  // namespace
