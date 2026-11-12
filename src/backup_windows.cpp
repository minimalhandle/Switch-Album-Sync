#include "nso_album_sync/backup.hpp"

#include "import_windows.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/util.hpp"
#include "nso_album_sync/windows_compat.hpp"

#include <string>
#include <system_error>

namespace nso {
namespace {

bool media_file(const std::filesystem::path& path) {
    const auto extension = lower(path_to_utf8(path.extension()));
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".mp4";
}

bool same_file(const std::filesystem::path& source, const std::filesystem::path& target) {
    WIN32_FILE_ATTRIBUTE_DATA from{}, to{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &from) ||
        !GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &to)) return false;
    return from.nFileSizeHigh == to.nFileSizeHigh && from.nFileSizeLow == to.nFileSizeLow &&
        CompareFileTime(&from.ftLastWriteTime, &to.ftLastWriteTime) == 0;
}

bool copy_into_place(const std::filesystem::path& source, const std::filesystem::path& target) {
    // CopyFileExW keeps the capture's timestamps. Copy beside the target and
    // swap it in, so an interrupted backup never leaves a half-written file.
    auto partial = target;
    partial += L".switch-album-sync.part";
    if (!CopyFileExW(source.c_str(), partial.c_str(), nullptr, nullptr, nullptr, 0)) return false;
    if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(partial.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool valid_backup_folder(const std::filesystem::path& backup, const std::filesystem::path& album) {
    // Same rules as a watched folder: it must exist and not overlap the album.
    return valid_watch_folder(backup, album);
}

BackupResult backup_album(
    const std::filesystem::path& album,
    const std::filesystem::path& backup,
    const std::function<bool()>& cancelled) {
    BackupResult result;
    if (!valid_backup_folder(backup, album)) {
        throw std::runtime_error("Choose a backup folder that exists and is separate from the album");
    }
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(album, options, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (cancelled && cancelled()) break;
        const auto& path = it->path();
        const auto name = lower(path_to_utf8(path.filename()));
        std::error_code entry_error;
        if (it->is_directory(entry_error)) {
            // Skip the app's own working folders (import staging, review slots).
            if (name.starts_with(".switch-album-sync-")) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entry_error) || !media_file(path)) continue;
        const auto target = backup / path.lexically_relative(album);
        if (same_file(path, target)) {
            ++result.unchanged;
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), entry_error);
        if (entry_error || !copy_into_place(path, target)) {
            ++result.failed;
            continue;
        }
        ++result.copied;
    }
    if (error) ++result.failed;
    return result;
}

}  // namespace nso
