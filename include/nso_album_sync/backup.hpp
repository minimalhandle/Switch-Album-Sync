#pragma once

#include <filesystem>
#include <functional>

namespace nso {

struct BackupResult {
    int copied = 0;
    int unchanged = 0;
    int failed = 0;
};

// Mirrors the album's screenshots and videos into `backup`, keeping the same
// folders, file names and timestamps. New and changed files are copied;
// nothing in the backup is ever deleted, so removing a capture from the album
// leaves its backup copy in place. Local only: no network access.
BackupResult backup_album(
    const std::filesystem::path& album,
    const std::filesystem::path& backup,
    const std::function<bool()>& cancelled = {});

// A backup folder must exist and must not contain, or sit inside, the album.
bool valid_backup_folder(const std::filesystem::path& backup, const std::filesystem::path& album);

}  // namespace nso
