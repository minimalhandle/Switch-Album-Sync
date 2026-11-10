#pragma once

#include "nso_album_sync/windows_compat.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nso {
struct ImportResult {
    int copied = 0;
    int skipped = 0;
    int failed = 0;
    int review = 0;
    std::vector<std::filesystem::path> copied_paths;
};

struct ImportMatch {
    std::filesystem::path existing;
    std::filesystem::path incoming;
};

std::vector<ImportMatch> pending_import_matches(const std::filesystem::path& album);
bool resolve_import_match(const std::filesystem::path& album, const ImportMatch& match, bool keep_both);
bool recorded_online_match(const std::filesystem::path& album, const std::filesystem::path& filename);
void stage_online_match(const std::filesystem::path& album, const std::filesystem::path& filename,
    const std::filesystem::path& existing, const std::function<void(const std::filesystem::path&)>& save);

std::string pick_import_source(HWND owner);
ImportResult import_windows_captures(const std::string& source,
    const std::filesystem::path& album, const std::function<bool()>& cancelled);
}
