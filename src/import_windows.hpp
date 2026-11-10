#pragma once

#include "nso_album_sync/windows_compat.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nso {
struct LocalCapture;

struct ImportResult {
    int copied = 0;
    int skipped = 0;
    int failed = 0;
    int review = 0;
    std::vector<std::filesystem::path> copied_paths;
    std::vector<std::filesystem::path> completed_sources;
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

struct ImportCandidate {
    std::filesystem::path local;
    std::string shell_id;
    std::wstring name;
    std::wstring game;
    std::optional<std::uintmax_t> bytes;
    bool possible_duplicate = false;
    std::wstring suggested_game;
    std::string watch_stamp;
};

struct ImportPlan {
    std::filesystem::path album;
    std::vector<ImportCandidate> captures;
    int scan_errors = 0;
    std::filesystem::path watch_source;
};

void suggest_import_games(ImportPlan& plan, const std::vector<LocalCapture>& library);
bool valid_watch_folder(const std::filesystem::path& source, const std::filesystem::path& album);

class WatchedFolder {
public:
    ImportPlan poll(const std::filesystem::path& source, const std::filesystem::path& album,
                    const std::filesystem::path& receipts, const std::function<bool()>& cancelled);
    void complete(const std::vector<std::filesystem::path>& sources);

private:
    std::filesystem::path source_;
    std::filesystem::path album_;
    std::filesystem::path receipts_;
    std::map<std::filesystem::path, std::string> observed_;
    std::map<std::filesystem::path, std::string> completed_;
};

std::shared_ptr<const ImportPlan> pick_import_plan(HWND owner, const std::filesystem::path& album);
ImportResult import_windows_captures(const ImportPlan& plan,
    const std::filesystem::path& album, const std::function<bool()>& cancelled);
}
