/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>  // blur-nx 1.0.2: SameFolder
#include <array>
#include <cstdio>     // blur-nx 1.0.2: SameFiles
#include <cstring>
#include <queue>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xfile.h>
#include <rex/system/xobject.h>

/*
 * Readable copy of the saves and a trace of what happens to them.
 *
 * All of the game's saves go through here: XamContentCreate opens a container, the game writes inside it
 * as if it were a drive (SAV:, for example) and XamContentClose closes it. Where they end up is opaque
 * (content_root/xuid/title/type/name), so the player cannot see the saves and, when one comes out
 * "damaged", there is no way to tell whether the failure was in writing or in reading.
 *
 * Two additions:
 *   - A "[save]" trace in the log: every create, open and close, with the real path and the files
 *     inside with their sizes. A 0-byte save, or one that never gets written, shows at a glance.
 *   - content_backup_root: if it is not empty, when a container is closed it is copied whole to
 *     <content_backup_root>/<profile>/current, and whatever was there before moves to <profile>/previous.
 *     Besides giving readable copies of the saves, sorted by profile, it always keeps a previous
 *     version to recover a profile that got corrupted.
 */
// deadpool-nx (v1.0.1, 25a2e309). Off: it changes what a game is told about a package (the licence it reads).
REXCVAR_DEFINE_BOOL(deadpool_dlc_license_nonzero, false, "Kernel",
                    "deadpool-nx: a DLC package whose header gives no licence bits is opened and installed with licence "
                    "mask 1 (licensed). false: mask 0, as before (Deadpool's Merc with a Map Pack then stays locked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(content_backup_root, "", "Kernel",
                      "Folder where a copy of each save is left: <profile id>/<save>/current, and the copy before it "
                      "in previous (only a save that changed and has data moves them on; blur_content_backup_per_profile "
                      "off: <save>/...). Empty = nothing is copied");
// deadpool-nx (v1.0.1, 25a2e309; its toml's dlc_enabled): every DLC package off at once (saves untouched).
REXCVAR_DEFINE_BOOL(deadpool_dlc_enabled, true, "Kernel",
                    "deadpool-nx: DLC packages are registered from the DLC folder and listed to the game. false: none "
                    "is installed, registered or listed (they stay on the card; saves are untouched)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// katamari-nx (2026-11-01): DLC kept from the game without uninstalling it (the user: Beautiful Katamari's packs that
// only unlock what the game unlocks by itself take the fun out of unlocking it). The app's katamari_dlc_unlock_packs
// adds those packs' names; anything listed here is left out of the game's DLC list, and emptying it shows them again.
REXCVAR_DEFINE_STRING(dlc_hidden, "", "Kernel",
                      "DLC packages the game is not shown: display names or package file names, separated by ';' "
                      "(any case). They stay installed. Empty = every installed package is shown");
// deadpool-nx (v1.0.1, 25a2e309).
REXCVAR_DEFINE_BOOL(deadpool_content_robust_io, true, "Kernel",
                    "deadpool-nx: the content manager (saves, DLC headers, a DLC unpack) uses the error_code forms of "
                    "exists / file_size / create_directories (Horizon's file system throws on an I/O error), takes a "
                    "folder that is already there as made, and reports a failed header write or a failed unpack read "
                    "or write (they returned SUCCESS). false: the forms and results as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx 1.0.2 (2026-11-26, GitHub issue #9; renut-ultimate port 330, hardened with a key): a save written again is
// set aside, not deleted, until the game closes the new one, and one whose write never finished is put back; the
// readable copy moves on only for a changed save with data. Off: deleted first and copied at every close, as before.
REXCVAR_DEFINE_BOOL(blur_content_atomic_rewrite, true, "Kernel",
                    "A save the game writes again (XamContentCreate's CREATE_ALWAYS / TRUNCATE_EXISTING) is moved "
                    "aside (content_root/xuid/title/Rewriting/...) until the new one is closed, and put back at the "
                    "next look if the game closed while writing (a crash mid-save left it empty before: Blur's "
                    "GitHub issue #9). The content_backup_root copy moves on only for a changed save with data. "
                    "Off: deleted first, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx's advice (renut-ultimate port 333): content_backup_root copies saved games only.
REXCVAR_DEFINE_BOOL(blur_content_backup_saves_only, true, "Kernel",
                    "content_backup_root copies saved games only (content type 1), not DLC folders a game closes "
                    "(read whole at each close to compare, copied at the first). Off: every closed content, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx 01cad9d6 (1.0.2; renut-ultimate port 332): the content_backup_root copies one folder per profile.
REXCVAR_DEFINE_BOOL(blur_content_backup_per_profile, true, "Kernel",
                    "content_backup_root's copies go to <profile id>/<save>/current|previous (the profile id from the "
                    "save's own path). Off: <save>/current|previous, every profile's copies of one save name in one "
                    "folder, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
bool BackupSavesOnlyOn() {
  static const bool on = REXCVAR_GET(blur_content_backup_saves_only);
  return on;
}
bool BackupPerProfileOn() {
  static const bool on = REXCVAR_GET(blur_content_backup_per_profile);
  return on;
}
bool AtomicRewriteOn() {
  static const bool on = REXCVAR_GET(blur_content_atomic_rewrite);
  return on;
}

bool ContentRobustIo() {
  static const bool on = REXCVAR_GET(deadpool_content_robust_io);
  return on;
}
// std::filesystem::exists: the error_code form (an I/O error is "not there") or the throwing one.
bool ContentPathExists(const std::filesystem::path& path) {
  if (ContentRobustIo()) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
  }
  return std::filesystem::exists(path);
}
// std::filesystem::file_size: ok false on an error (the error_code form) or the throwing one (ok always true).
uintmax_t ContentFileSize(const std::filesystem::path& path, bool& ok) {
  if (ContentRobustIo()) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    ok = !ec;
    return size;
  }
  ok = true;
  return std::filesystem::file_size(path);
}
}  // namespace

namespace rex {
namespace system {
namespace xam {

static const char* kThumbnailFileName = "__thumbnail.png";

static const char* kGameUserContentDirName = "profile";

static const char* kGameContentHeaderDirName = "Headers";

static int content_device_id_ = 0;

namespace {

// A folder name the SD accepts, derived from the one the player sees.
std::string SafeName(const std::string_view original) {
  std::string clean;
  for (const char c : original) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      continue;
    }
    // 0x5C is the backslash.
    if (c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|' || u == 0x5C) {
      continue;
    }
    clean.push_back(c);
  }
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) {
    clean.pop_back();
  }
  return clean.empty() ? std::string("no_name") : clean;
}

// Whether dlc_hidden names this package, by its display name or its file name.
bool DlcHidden(const XCONTENT_DATA& data) {
  const std::string display = rex::string::to_utf8(data.display_name());
  const std::string file = data.file_name();
  std::string_view list = REXCVAR_GET(dlc_hidden);
  while (!list.empty()) {
    const size_t end = list.find(';');
    std::string_view name = list.substr(0, end);
    list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    if (!name.empty() && (rex::string::utf8_equal_case(name, display) || rex::string::utf8_equal_case(name, file))) {
      return true;
    }
  }
  return false;
}

// What is inside a container, for the log. It is the line that says whether the save has data or not.
std::string ThatHasInside(const std::filesystem::path& folder) {
  std::string text;
  uint64_t total = 0;
  uint32_t how_many = 0;
  for (const auto& f : rex::filesystem::ListFiles(folder)) {
    if (f.type != rex::filesystem::FileInfo::Type::kFile) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += fmt::format("{} ({} B)", rex::path_to_utf8(f.name), f.total_size);
    total += f.total_size;
    ++how_many;
  }
  if (how_many == 0) {
    return "EMPTY";
  }
  return fmt::format("{} file(s), {} B: {}", how_many, total, text);
}

bool CopyFile(const std::filesystem::path& source, const std::filesystem::path& target) {
  FILE* entry = rex::filesystem::OpenFile(source, "rb");
  if (!entry) {
    return false;
  }
  FILE* output = rex::filesystem::OpenFile(target, "wb");
  if (!output) {
    fclose(entry);
    return false;
  }
  // blur-nx 1.0.2 (2026-11-26, GitHub issue #9): the copy runs as a save closes, which is when a heap short of room
  // was seen to fail: a buffer on the stack (saves are a few KB), not a heap block that could throw.
  uint8_t buffer[16 * 1024];
  bool ok = true;
  while (true) {
    const size_t read = fread(buffer, 1, sizeof(buffer), entry);
    if (read == 0) {
      break;
    }
    if (fwrite(buffer, 1, read, output) != read) {
      ok = false;
      break;
    }
  }
  if (ferror(entry)) {
    ok = false;
  }
  fclose(entry);
  if (fclose(output) != 0) {
    ok = false;
  }
  return ok;
}

// Saves are small, so copying them by hand is cheaper than fighting std::filesystem on Horizon.
bool CopyFolder(const std::filesystem::path& source, const std::filesystem::path& target,
                   uint32_t& files) {
  std::error_code ec;
  std::filesystem::create_directories(target, ec);
  bool ok = true;
  for (const auto& f : rex::filesystem::ListFiles(source)) {
    if (f.type == rex::filesystem::FileInfo::Type::kDirectory) {
      ok = CopyFolder(source / f.name, target / f.name, files) && ok;
      continue;
    }
    if (CopyFile(source / f.name, target / f.name)) {
      ++files;
    } else {
      ok = false;
    }
  }
  return ok;
}

// blur-nx 1.0.2 (2026-11-26, GitHub issue #9): whether two folders hold the same files with the same bytes (saves are
// a few KB: read whole, through stack buffers), and whether a folder holds nothing or an empty file (a write that never
// happened).
bool SameFiles(const std::filesystem::path& a, const std::filesystem::path& b) {
  FILE* fa = rex::filesystem::OpenFile(a, "rb");
  FILE* fb = fa ? rex::filesystem::OpenFile(b, "rb") : nullptr;
  bool same = fa && fb;
  uint8_t ba[4096], bb[4096];
  while (same) {
    const size_t na = fread(ba, 1, sizeof(ba), fa), nb = fread(bb, 1, sizeof(bb), fb);
    if (na != nb || std::memcmp(ba, bb, na) != 0) same = false;
    if (na == 0 || !same) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b) {
  const auto la = rex::filesystem::ListFiles(a), lb = rex::filesystem::ListFiles(b);
  if (la.size() != lb.size()) return false;
  for (const auto& f : la) {
    const auto it = std::find_if(lb.begin(), lb.end(), [&f](const rex::filesystem::FileInfo& g) {
      return g.name == f.name && g.type == f.type;
    });
    if (it == lb.end()) return false;
    if (f.type == rex::filesystem::FileInfo::Type::kDirectory) {
      if (!SameFolder(a / f.name, b / f.name)) return false;
    } else if (f.total_size != it->total_size || !SameFiles(a / f.name, b / f.name)) {
      return false;
    }
  }
  return true;
}

bool EmptyOrHasEmptyFile(const std::filesystem::path& folder) {
  bool any = false;
  for (const auto& f : rex::filesystem::ListFiles(folder)) {
    if (f.type == rex::filesystem::FileInfo::Type::kDirectory) {
      if (EmptyOrHasEmptyFile(folder / f.name)) return true;
      any = true;
    } else if (f.total_size == 0) {
      return true;
    } else {
      any = true;
    }
  }
  return !any;
}

// The readable copy: <content_backup_root>/<profile id>/<save>/current, and the previous one to .../previous.
// blur-nx 1.0.2 (issue #9): only a save that changed and has data moves the copies on. A close that only read the save
// (the game's look at the start) left previous = current before, so after a save emptied by a crash the next start's
// read and the game's recreated save pushed the last good one out of both.
// blur-nx 01cad9d6 (1.0.2, 2026-11-26; renut-ultimate port 332): one folder per profile, as this said: the save's name
// alone (Blur's AMAXSAVE) put every profile's saves in one folder. The profile id is the save's own path's
// (content_root/<xuid>/<title>/<type>/<name>); with none, the save's name alone.
void CopyForTheUser(const std::string_view name, const std::filesystem::path& source) {
  const std::string root = REXCVAR_GET(content_backup_root);
  if (root.empty()) {
    return;
  }
  // renut-ultimate (port 332's key): blur_content_backup_per_profile off, one folder per save name, as before.
  const std::string xuid = !BackupPerProfileOn() ? std::string()
                           : SafeName(rex::path_to_utf8(source.parent_path().parent_path().parent_path().filename()));
  const std::string save = SafeName(name);
  const std::string profile = xuid.empty() ? save : xuid + "/" + save;  // for the log
  const std::filesystem::path base =
      xuid.empty() ? std::filesystem::path(root) / save : std::filesystem::path(root) / xuid / save;
  const std::filesystem::path actual = base / "current";
  const std::filesystem::path previous = base / "previous";

  std::error_code ec;
  if (AtomicRewriteOn() && EmptyOrHasEmptyFile(source)) {
    REXSYS_WARN("[save] '{}' has an empty file or none: not copied over {}", profile, rex::path_to_utf8(actual));
    return;
  }
  if (AtomicRewriteOn() && std::filesystem::exists(actual, ec) && SameFolder(source, actual)) {
    return;  // unchanged since the last copy
  }
  if (std::filesystem::exists(actual, ec)) {
    std::filesystem::remove_all(previous, ec);
    ec.clear();
    std::filesystem::rename(actual, previous, ec);
    if (ec) {
      // If renaming is not allowed, copy and delete: what matters is not losing the previous version.
      uint32_t n = 0;
      CopyFolder(actual, previous, n);
      std::filesystem::remove_all(actual, ec);
      ec.clear();
    }
  }

  uint32_t files = 0;
  const bool ok = CopyFolder(source, actual, files);
  if (ok) {
    REXSYS_INFO("[save] copy of '{}': {} file(s) in {}", profile, files,
                rex::path_to_utf8(actual));
  } else {
    REXSYS_WARN("[save] the copy of '{}' failed halfway ({} file(s) in {})", profile,
                files, rex::path_to_utf8(actual));
  }
}

}  // namespace

ContentPackage::ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path,
                               const std::filesystem::path& package_file)
    : kernel_state_(kernel_state), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
  content_data_ = data;

  auto fs = kernel_state_->file_system();
  // rr6-nx (2026-11-18): a DLC package read in place (ContentManager::RegisterPackageFile): its files as the package
  // holds them, read only, nothing unpacked. If it cannot be read, the folder as before.
  if (!package_file.empty()) {
    auto stfs = std::make_unique<rex::filesystem::StfsContainerDevice>(device_path_, package_file);
    if (stfs->Initialize()) {
      fs->RegisterDevice(std::move(stfs));
      fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
      from_package_file_ = true;
      return;
    }
    REXSYS_ERROR("[dlc] '{}': its package {} could not be read; the folder {} is used", data.file_name(),
                 rex::path_to_utf8(package_file), rex::path_to_utf8(package_path));
  }
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(device_path_, package_path, false,
                                                                  /*allow_share_delete=*/true);
  device->Initialize();
  fs->RegisterDevice(std::move(device));
  fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path header_path) {
  if (!ContentPathExists(header_path)) {  // deadpool_content_robust_io
    return;
  }

  auto file = rex::filesystem::OpenFile(header_path, "rb");
  if (!file) {
    return;
  }

  bool size_ok = true;
  auto file_size = ContentFileSize(header_path, size_ok);
  if (!size_ok || file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  fread(&license_, 1, sizeof(license_), file);
  fclose(file);
}

ContentManager::ContentManager(KernelState* kernel_state, const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(uint64_t xuid, XContentType content_type,
                                                         uint32_t title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));

  // Package root path:
  // content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(uint64_t xuid,
                                                         const XCONTENT_AGGREGATE_DATA& data) {
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  // DLCs are stored in common directory
  if (data.content_type == XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  // Content path:
  // content_root/xuid/title_id/content_type/data_file_name/
  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return package_root / rex::to_path(data.file_name());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               uint64_t xuid, uint32_t title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Header root path:
  // content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(uint32_t device_id, uint64_t xuid,
                                                                 XContentType content_type,
                                                                 uint32_t title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;
  if (content_type == XContentType::kMarketplaceContent && !REXCVAR_GET(deadpool_dlc_enabled)) {
    return result;
  }

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  // Search path:
  // content_root/xuid/title_id/type_name/*
  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  PutBackInterruptedUnder(package_root);  // blur-nx 1.0.2 (issue #9): saves whose last write never finished, back
  auto file_infos = rex::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {
    if (file_info.type != rex::filesystem::FileInfo::Type::kDirectory) {
      // Directories only.
      continue;
    }

    XCONTENT_AGGREGATE_DATA content_data;
    // mk9-nx (2026-10-28; port 56): == X_ERROR_SUCCESS, not XSUCCEEDED: its Win32 errors (X_ERROR_FILE_NOT_FOUND is 2)
    // pass XSUCCEEDED, so a package without a header file (DLC put in place by hand) was listed from an uninitialised
    // record (no name, type 5A000000) and the game could not open it.
    if (ReadContentHeaderFile(rex::path_to_utf8(file_info.name), xuid, title_id, content_type,
                              content_data) == X_ERROR_SUCCESS) {
      result.emplace_back(std::move(content_data));
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(rex::path_to_utf16(file_info.name));
      content_data.set_file_name(rex::path_to_utf8(file_info.name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(std::move(content_data));
    }
  }

  // rr6-nx (2026-11-18): DLC read in place has its header and no folder of unpacked files: listed from the header,
  // in the order registered (the DLC installer's), after any listed from folders. Only for the content with no user
  // (DLC is installed for xuid 0; the game also lists its profile's, which has none).
  if (content_type == XContentType::kMarketplaceContent && xuid == 0) {
    auto global_lock = global_critical_region_.Acquire();
    for (const auto& [name, file] : package_files_) {
      if (std::any_of(result.begin(), result.end(),
                      [&name](const XCONTENT_AGGREGATE_DATA& e) { return e.file_name() == name; })) {
        continue;
      }
      XCONTENT_AGGREGATE_DATA content_data;
      if (ReadContentHeaderFile(name, xuid, title_id, content_type, content_data) == X_ERROR_SUCCESS) {
        result.emplace_back(std::move(content_data));
      }
    }
  }

  if (content_type == XContentType::kMarketplaceContent && !REXCVAR_GET(dlc_hidden).empty()) {
    std::erase_if(result, [](const XCONTENT_AGGREGATE_DATA& entry) {
      if (!DlcHidden(entry)) return false;
      REXSYS_INFO("[dlc] '{}' ({}) hidden (dlc_hidden)", rex::string::to_utf8(entry.display_name()),
                  entry.file_name());
      return true;
    });
  }

  // What the game sees when it asks for the list of saves. If a profile that exists on disk does not show
  // up here, the problem is the enumeration; if it shows up and is then reported as damaged, the problem
  // is its contents.
  REXSYS_INFO("[save] list type {:08X} in {}: {} entry(ies)", uint32_t(content_type),
              rex::path_to_utf8(package_root), result.size());
  for (const auto& entry : result) {
    REXSYS_INFO("[save]   '{}' (displayed as '{}')", entry.file_name(),
                rex::string::to_utf8(entry.display_name()));
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  const auto package_file = PackageFileOf(data);  // rr6-nx (2026-11-18): DLC read in place
  if (package_file.empty() && !ContentPathExists(package_path)) {
    return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, package_path, package_file);
  return package;
}

bool ContentManager::ContentExists(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  if (!PackageFileOf(data).empty()) {  // rr6-nx (2026-11-18): DLC read in place
    return true;
  }
  auto path = ResolvePackagePath(xuid, data);
  PutBackInterrupted(path);  // blur-nx 1.0.2 (issue #9): a save whose last write never finished comes back first
  return ContentPathExists(path);
}

void ContentManager::RegisterPackageFile(const std::string& file_name,
                                         const std::filesystem::path& package_file) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& [name, file] : package_files_) {
    if (name == file_name) {
      file = package_file;
      return;
    }
  }
  package_files_.emplace_back(file_name, package_file);
}

std::filesystem::path ContentManager::PackageFileOf(const XCONTENT_AGGREGATE_DATA& data) {
  if (data.content_type != XContentType::kMarketplaceContent) {
    return {};
  }
  const std::string name = data.file_name();
  std::filesystem::path found;
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const auto& [n, file] : package_files_) {
      if (n == name) {
        found = file;
        break;
      }
    }
  }
  std::error_code ec;
  if (found.empty() || !std::filesystem::is_regular_file(found, ec)) {
    return {};
  }
  return found;
}

X_RESULT ContentManager::WriteContentHeaderFile(uint64_t xuid, XCONTENT_AGGREGATE_DATA data,
                                                uint32_t license_mask) {
  if (data.title_id == uint32_t(-1)) {
    data.title_id = kernel_state_->title_id();
  }
  if (data.xuid == uint64_t(-1)) {
    data.xuid = xuid;
  }
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto parent_path = header_path.parent_path();

  if (ContentRobustIo()) {
    // deadpool-nx: create_directories says false for a folder that is already there.
    std::error_code ec;
    if (!std::filesystem::is_directory(parent_path, ec)) {
      ec.clear();
      std::filesystem::create_directories(parent_path, ec);
      std::error_code check_ec;
      if (ec && !std::filesystem::is_directory(parent_path, check_ec)) {
        REXSYS_ERROR("[save] header of '{}': could NOT create {} ({})", data.file_name(),
                     rex::path_to_utf8(parent_path), ec.value());
        return X_ERROR_ACCESS_DENIED;
      }
    }
  } else if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return X_ERROR_ACCESS_DENIED;
    }
  }

  rex::filesystem::CreateEmptyFile(header_path);

  auto file = rex::filesystem::OpenFile(header_path, "wb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  if (ContentRobustIo()) {
    // deadpool-nx: a short write or a failed close is an error (it was SUCCESS).
    bool ok = fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file) == sizeof(XCONTENT_AGGREGATE_DATA);
    if (ok && license_mask != 0) {
      ok = fwrite(&license_mask, 1, sizeof(license_mask), file) == sizeof(license_mask);
    }
    if (fclose(file) != 0) {
      ok = false;
    }
    REXSYS_INFO("[save] header of '{}' {} in {}", data.file_name(), ok ? "written" : "FAILED",
                rex::path_to_utf8(header_path));
    return ok ? X_ERROR_SUCCESS : X_ERROR_ACCESS_DENIED;
  }
  fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file);
  if (license_mask != 0) {
    fwrite(&license_mask, 1, sizeof(license_mask), file);
  }
  const bool ok = fclose(file) == 0;
  REXSYS_INFO("[save] header of '{}' {} in {}", data.file_name(),
              ok ? "written" : "FAILED on close", rex::path_to_utf8(header_path));
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ReadContentHeaderFile(const std::string_view file_name, uint64_t xuid,
                                               uint32_t title_id, XContentType content_type,
                                               XCONTENT_AGGREGATE_DATA& data) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  if (!ContentPathExists(header_file_path)) {  // deadpool_content_robust_io
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file = rex::filesystem::OpenFile(header_file_path, "rb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  bool size_ok = true;
  auto file_size = ContentFileSize(header_file_path, size_ok);
  if (!size_ok || file_size < header_size) {
    fclose(file);
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::array<uint8_t, header_size> buffer;
  size_t result = fread(buffer.data(), 1, header_size, file);
  fclose(file);

  if (result != header_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::memcpy(&data, buffer.data(), buffer.size());
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name, uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  REXSYS_INFO("[save] create '{}' (root {}) in {}", data.file_name(), root_name,
              rex::path_to_utf8(package_path));
  if (ContentPathExists(package_path)) {  // deadpool_content_robust_io
    REXSYS_WARN("[save] create '{}': it already existed, returning ALREADY_EXISTS", data.file_name());
    return X_ERROR_ALREADY_EXISTS;
  }
  if (ContentRobustIo()) {
    // deadpool-nx: a folder made meanwhile (create_directories says false) is made.
    std::error_code ec;
    std::filesystem::create_directories(package_path, ec);
    std::error_code check_ec;
    if (ec && !std::filesystem::is_directory(package_path, check_ec)) {
      REXSYS_ERROR("[save] create '{}': could NOT create {} ({})", data.file_name(),
                   rex::path_to_utf8(package_path), ec.value());
      return X_ERROR_ACCESS_DENIED;
    }
  } else if (!std::filesystem::create_directories(package_path)) {
    REXSYS_ERROR("[save] create '{}': could NOT create {}", data.file_name(),
                 rex::path_to_utf8(package_path));
    return X_ERROR_ACCESS_DENIED;
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name, uint64_t xuid,
                                     const XCONTENT_AGGREGATE_DATA& data,
                                     uint32_t& content_license) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  const auto package_file = PackageFileOf(data);  // rr6-nx (2026-11-18): DLC read in place
  if (package_file.empty()) PutBackInterrupted(package_path);  // blur-nx 1.0.2 (issue #9)
  if (package_file.empty() && !ContentPathExists(package_path)) {  // deadpool_content_robust_io
    REXSYS_WARN("[save] open '{}': {} does not exist", data.file_name(),
                rex::path_to_utf8(package_path));
    return X_ERROR_FILE_NOT_FOUND;
  }
  // What the game will find inside. If this says EMPTY and the profile is then reported as damaged, the
  // failure was in saving; if it lists the files with their sizes, the failure is in reading them or in
  // the contents themselves.
  if (!package_file.empty()) {
    REXSYS_INFO("[save] open '{}' (root {}) from its package {} (read in place)", data.file_name(), root_name,
                rex::path_to_utf8(package_file));
  } else {
    REXSYS_INFO("[save] open '{}' (root {}) in {} -> {}", data.file_name(), root_name,
                rex::path_to_utf8(package_path), ThatHasInside(package_path));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);
  package->LoadPackageLicenseMask(ResolvePackageHeaderPath(
      data.file_name(), xuid, kernel_state_->title_id(), data.content_type));
  content_license = package->GetPackageLicense();
  // deadpool-nx (v1.0.1, 25a2e309): a DLC package whose header gives no licence bits is the player's own copy: licensed
  // (Deadpool's Merc with a Map Pack read 0 and the game kept it locked).
  static const bool license_nonzero = REXCVAR_GET(deadpool_dlc_license_nonzero);
  if (license_nonzero && content_license == 0 && data.content_type == XContentType::kMarketplaceContent) {
    content_license = 1;
  }

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    // Some games use different casing between Create and Close (e.g. "save" vs "SAVE")
    auto it = open_packages_.find(string::string_key_case(root_name));
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  // The path and name are recorded before destroying the package, which is what holds them.
  const std::filesystem::path path = package->package_path();
  const std::string name = package->GetPackageContentData().file_name();
  const bool from_package_file = package->from_package_file();
  const XContentType content_type = package->GetPackageContentData().content_type;  // port 333
  delete package;  // unmounts the guest drive: from here on the files are in place

  if (from_package_file) {  // rr6-nx (2026-11-18): DLC read in place, read only: nothing to look at or copy
    REXSYS_INFO("[save] close '{}' (root {}): its package", name, root_name);
    return X_ERROR_SUCCESS;
  }

  REXSYS_INFO("[save] close '{}' (root {}) in {} -> {}", name, root_name,
              rex::path_to_utf8(path), ThatHasInside(path));
  // blur-nx 1.0.2 (issue #9): a save written again is closed whole: the one set aside for it goes.
  {
    bool was_rewriting = false;
    {
      auto global_lock = global_critical_region_.Acquire();
      const auto it = std::find(rewriting_.begin(), rewriting_.end(), path);
      if (it != rewriting_.end()) {
        rewriting_.erase(it);
        was_rewriting = true;
      }
    }
    const std::filesystem::path aside = ResolveSetAsidePath(path);
    std::error_code ec;
    if (was_rewriting && std::filesystem::exists(aside, ec)) {
      std::filesystem::remove_all(aside, ec);
      REXSYS_INFO("[save] '{}' written again and closed: the save set aside for it is dropped", name);
    }
  }
  // blur-nx's advice (2026-11-26; renut-ultimate port 333): the readable copy for saved games only. Since port 330
  // a close compares the folder with its last copy byte by byte (SameFolder): nothing for a save of a few KB, a whole
  // extracted DLC folder read at each of its closes (and copied at the first). Off: every closed content, as before.
  if (content_type == XContentType::kSavedGame || !BackupSavesOnlyOn())
    CopyForTheUser(name, path);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / kThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = rex::filesystem::OpenFile(thumb_path, "rb");
    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::SetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    auto file = rex::filesystem::OpenFile(thumb_path, "wb");
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::DeleteContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
    return X_ERROR_ACCESS_DENIED;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

// blur-nx 1.0.2 (2026-11-26, GitHub issue #9; see SetAsideContent in content_manager.h).
std::filesystem::path ContentManager::ResolveSetAsidePath(const std::filesystem::path& package_path) {
  // content_root/xuid/title/type/name -> content_root/xuid/title/Rewriting/type/name (outside the type's folder, so
  // the game's list of saves never shows it)
  const std::filesystem::path type_dir = package_path.parent_path();
  return type_dir.parent_path() / "Rewriting" / type_dir.filename() / package_path.filename();
}

bool ContentManager::IsPackagePathOpen(const std::filesystem::path& package_path) {
  auto global_lock = global_critical_region_.Acquire();
  if (std::find(rewriting_.begin(), rewriting_.end(), package_path) != rewriting_.end()) return true;
  for (const auto& [root, package] : open_packages_) {
    if (package && package->package_path() == package_path) return true;
  }
  return false;
}

void ContentManager::PutBackInterrupted(const std::filesystem::path& package_path) {
  if (!AtomicRewriteOn()) return;  // renut-ultimate: blur_content_atomic_rewrite off, as before
  const std::filesystem::path aside = ResolveSetAsidePath(package_path);
  std::error_code ec;
  if (!std::filesystem::exists(aside, ec) || IsPackagePathOpen(package_path)) return;
  // The game set this save aside to write it again and never closed the new one: what is in its place is that
  // unfinished write. The save before it comes back.
  const std::string what = ThatHasInside(package_path);
  std::filesystem::remove_all(package_path, ec);
  ec.clear();
  std::filesystem::rename(aside, package_path, ec);
  if (ec) {
    REXSYS_ERROR("[save] '{}': its last write never finished, and the save before it could NOT be put back from {} "
                 "({})", rex::path_to_utf8(package_path.filename()), rex::path_to_utf8(aside), ec.message());
    return;
  }
  REXSYS_WARN("[save] '{}': its last write never finished (the game closed while saving; it held {}): the save "
              "before it is back -> {}", rex::path_to_utf8(package_path.filename()), what,
              ThatHasInside(package_path));
}

void ContentManager::PutBackInterruptedUnder(const std::filesystem::path& package_root) {
  if (!AtomicRewriteOn()) return;
  const std::filesystem::path aside_root = ResolveSetAsidePath(package_root / "x").parent_path();
  std::error_code ec;
  if (!std::filesystem::exists(aside_root, ec)) return;
  for (const auto& f : rex::filesystem::ListFiles(aside_root)) {
    if (f.type == rex::filesystem::FileInfo::Type::kDirectory) PutBackInterrupted(package_root / f.name);
  }
}

X_RESULT ContentManager::SetAsideContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  if (!AtomicRewriteOn()) return DeleteContent(xuid, data);  // renut-ultimate: the key off, as before
  const std::filesystem::path path = ResolvePackagePath(xuid, data);
  PutBackInterrupted(path);  // an older unfinished write first: the whole save it set aside is the one to keep
  const std::filesystem::path aside = ResolveSetAsidePath(path);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return X_ERROR_FILE_NOT_FOUND;
  std::filesystem::remove_all(aside, ec);
  ec.clear();
  std::filesystem::create_directories(aside.parent_path(), ec);
  ec.clear();
  std::filesystem::rename(path, aside, ec);
  if (ec) {
    REXSYS_WARN("[save] '{}' could not be set aside while it is written again ({}): deleted as before",
                data.file_name(), ec.message());
    return DeleteContent(xuid, data);
  }
  {
    auto global_lock = global_critical_region_.Acquire();
    if (std::find(rewriting_.begin(), rewriting_.end(), path) == rewriting_.end()) rewriting_.push_back(path);
  }
  REXSYS_INFO("[save] '{}' set aside while the game writes it again (in {} until it is closed)", data.file_name(),
              rex::path_to_utf8(aside));
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::UnmountContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::UnmountAndDeleteContent(uint64_t xuid,
                                                 const XCONTENT_AGGREGATE_DATA& data) {
  // Unmount phase: tolerant of not-mounted state
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it != open_packages_.end()) {
      package = DetachPackage(it);
    }
  }
  delete package;

  // Delete phase: remove package directory and .header file
  auto package_path = ResolvePackagePath(xuid, data);

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);

  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = fmt::format("{:08X}", kernel_state_->title_id());
  auto user_name = rex::to_path(kernel_state_->user_profile()->name());

  // Per-game per-profile data location:
  // content_root/title_id/profile/user_name
  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unordered_map<string::string_key_case, ContentPackage*,
                   string::string_key_case::Hash>::iterator
ContentManager::FindOpenPackageByData(const XCONTENT_AGGREGATE_DATA& data) {
  // Resolve kCurrentlyRunningTitleId so both sides compare actual title IDs.
  uint32_t query_title = data.title_id;
  if (query_title == kCurrentlyRunningTitleId) {
    query_title = kernel_state_->title_id();
  }

  for (auto it = open_packages_.begin(); it != open_packages_.end(); ++it) {
    const auto& pkg = it->second->GetPackageContentData();

    uint32_t pkg_title = pkg.title_id;
    if (pkg_title == kCurrentlyRunningTitleId) {
      pkg_title = kernel_state_->title_id();
    }

    // Match on content_type + file_name + resolved title_id.
    // device_id is a virtual storage selector, not a content identifier.
    if (data.content_type == pkg.content_type && data.file_name() == pkg.file_name() &&
        query_title == pkg_title) {
      return it;
    }
  }
  return open_packages_.end();
}

ContentPackage* ContentManager::DetachPackage(
    std::unordered_map<string::string_key_case, ContentPackage*,
                       string::string_key_case::Hash>::iterator it) {
  CloseOpenedFilesFromContent(it->first.view());
  ContentPackage* package = it->second;
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

std::filesystem::path ContentManager::GetOpenPackagePath(const std::string_view root_name) const {
  auto it = open_packages_.find(string::string_key_case(root_name));
  if (it == open_packages_.end()) {
    return {};
  }
  return it->second->package_path();
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(XObject::Type::File);

  std::string resolved_path = "";
  kernel_state_->file_system()->FindSymbolicLink(std::string(root_name) + ':', resolved_path);

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content = rex::string::utf8_starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

static X_RESULT ExtractEntryBase(rex::filesystem::Entry* entry,
                                 const std::filesystem::path& base_path) {
  auto dest_path = base_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));

  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    std::error_code ec;
    std::filesystem::create_directories(dest_path, ec);
    if (ec) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }

  // Ensure parent directory exists
  std::error_code ec;
  std::filesystem::create_directories(dest_path.parent_path(), ec);

  rex::filesystem::File* in_file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file);
  if (status != X_STATUS_SUCCESS) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto out_file = rex::filesystem::OpenFile(dest_path, "wb");
  if (!out_file) {
    in_file->Destroy();
    return X_ERROR_ACCESS_DENIED;
  }

  constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4 MiB
  auto buffer = std::make_unique<uint8_t[]>(kBufferSize);
  size_t remaining = entry->size();
  size_t offset = 0;

  while (remaining > 0) {
    size_t bytes_read = 0;
    size_t to_read = std::min(remaining, kBufferSize);
    in_file->ReadSync(std::span<uint8_t>(buffer.get(), to_read), offset, &bytes_read);
    if (bytes_read == 0) {
      break;
    }
    fwrite(buffer.get(), 1, bytes_read, out_file);
    offset += bytes_read;
    remaining -= bytes_read;
  }

  fclose(out_file);
  in_file->Destroy();
  return X_ERROR_SUCCESS;
}

// deadpool-nx (v1.0.1): the entry's path without leading or trailing separators (base_path / "/x" is "/x"), the
// folders that are already there accepted, and every read, write and close checked.
static X_RESULT ExtractEntryRobust(rex::filesystem::Entry* entry,
                                   const std::filesystem::path& base_path) {
  std::string rel = rex::string::utf8_fix_path_separators(entry->path());
  while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
    rel.erase(rel.begin());
  }
  while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) {
    rel.pop_back();
  }

  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    if (rel.empty()) {
      return X_ERROR_SUCCESS;
    }
    auto dest_path = base_path / rex::to_path(rel);
    std::error_code ec;
    if (std::filesystem::is_directory(dest_path, ec)) {
      return X_ERROR_SUCCESS;
    }
    ec.clear();
    std::filesystem::create_directories(dest_path, ec);
    std::error_code check_ec;
    if (ec && !std::filesystem::is_directory(dest_path, check_ec)) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }

  if (rel.empty()) {
    return X_ERROR_ACCESS_DENIED;
  }
  auto dest_path = base_path / rex::to_path(rel);

  // Ensure parent directory exists
  auto parent_path = dest_path.parent_path();
  if (!parent_path.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent_path, ec)) {
      ec.clear();
      std::filesystem::create_directories(parent_path, ec);
      std::error_code check_ec;
      if (ec && !std::filesystem::is_directory(parent_path, check_ec)) {
        return X_ERROR_ACCESS_DENIED;
      }
    }
  }

  rex::filesystem::File* in_file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file);
  if (status != X_STATUS_SUCCESS || !in_file) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto out_file = rex::filesystem::OpenFile(dest_path, "wb");
  if (!out_file) {
    in_file->Destroy();
    return X_ERROR_ACCESS_DENIED;
  }

  constexpr size_t kBufferSize = 1 * 1024 * 1024;  // 1 MiB (deadpool-nx: was 4, and for every file)
  size_t remaining = entry->size();
  std::unique_ptr<uint8_t[]> buffer;
  if (remaining > 0) {
    buffer = std::make_unique<uint8_t[]>(std::min<size_t>(remaining, kBufferSize));
  }
  size_t offset = 0;
  bool io_ok = true;

  while (remaining > 0) {
    size_t bytes_read = 0;
    size_t to_read = std::min(remaining, kBufferSize);
    X_STATUS read_status =
        in_file->ReadSync(std::span<uint8_t>(buffer.get(), to_read), offset, &bytes_read);
    if (read_status != X_STATUS_SUCCESS || bytes_read == 0) {
      REXSYS_ERROR("[dlc] unpack: read of '{}' failed at {} ({:08X})", rel, offset, uint32_t(read_status));
      io_ok = false;
      break;
    }
    if (fwrite(buffer.get(), 1, bytes_read, out_file) != bytes_read) {
      REXSYS_ERROR("[dlc] unpack: write of {} failed at {}", rex::path_to_utf8(dest_path), offset);
      io_ok = false;
      break;
    }
    offset += bytes_read;
    remaining -= bytes_read;
  }

  if (fclose(out_file) != 0) {
    io_ok = false;
  }
  in_file->Destroy();
  return io_ok ? X_ERROR_SUCCESS : X_ERROR_ACCESS_DENIED;
}

static X_RESULT ExtractEntry(rex::filesystem::Entry* entry,
                             const std::filesystem::path& base_path) {
  return ContentRobustIo() ? ExtractEntryRobust(entry, base_path) : ExtractEntryBase(entry, base_path);
}

X_RESULT ContentManager::InstallContent(const std::filesystem::path& package_path, bool extract_files) {
  if (!ContentPathExists(package_path)) {  // deadpool_content_robust_io
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Mount the STFS package as a virtual filesystem device
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package_path);
  if (!device->Initialize()) {
    return X_ERROR_ACCESS_DENIED;
  }

  // Derive install destination:
  // root_path_/0000000000000000/{title_id}/00000002/{filename}/
  auto file_name = rex::path_to_utf8(package_path.filename());

  XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
  content_data.content_type = XContentType::kMarketplaceContent;
  content_data.title_id = kernel_state_->title_id();
  content_data.xuid = 0;
  content_data.set_file_name(file_name);

  // Read display name from STFS metadata
  auto display_name = device->header().metadata.display_name(rex::system::XLanguage::kEnglish);
  if (!display_name.empty()) {
    content_data.set_display_name(display_name);
  } else {
    content_data.set_display_name(rex::path_to_utf16(package_path.filename()));
  }

  auto install_path = ResolvePackagePath(0, content_data);

  // rr6-nx (2026-11-18): read in place, the package's files stay in it: only the header below.
  std::queue<rex::filesystem::Entry*> queue;
  if (extract_files) {
    // Create destination directory
    std::error_code ec;
    std::filesystem::create_directories(install_path, ec);
    std::error_code check_ec;
    if (ec && (!ContentRobustIo() || !std::filesystem::is_directory(install_path, check_ec))) {
      return X_ERROR_ACCESS_DENIED;
    }

    // Extract all files breadth-first
    auto* root = device->ResolvePath("");
    if (!root) {
      return X_ERROR_ACCESS_DENIED;
    }
    queue.push(root);
  }

  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();

    for (auto& child : entry->children()) {
      queue.push(child.get());
    }

    auto result = ExtractEntry(entry, install_path);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
  }

  // Compute license mask from STFS header licenses
  uint32_t license_mask = 0;
  for (size_t i = 0; i < 0x10; i++) {
    if (device->header().header.licenses[i].license_flags) {
      license_mask |= device->header().header.licenses[i].license_bits;
    }
  }
  if (license_mask == 0 && REXCVAR_GET(deadpool_dlc_license_nonzero)) {
    license_mask = 1;  // deadpool-nx (v1.0.1): as OpenContent's
  }

  // Write .header file
  return WriteContentHeaderFile(0, content_data, license_mask);
}

}  // namespace xam
}  // namespace system
}  // namespace rex
