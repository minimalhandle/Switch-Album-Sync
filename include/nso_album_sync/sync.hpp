#pragma once

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/platform.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nso {

enum class SyncStage { checking, downloading };
enum class SyncFailure { none, sign_in, service, download, storage };

struct SyncProgress {
    SyncStage stage = SyncStage::checking;
    int total_found = 0;
    int new_downloads = 0;
    int skipped = 0;
    int failed = 0;
};

struct SyncResult {
    struct Item {
        std::string label;
        std::string outcome;
        std::string retry_id;
        bool transient = false;
    };
    int total_found = 0;
    int new_downloads = 0;
    int skipped = 0;
    int failed = 0;
    int unavailable = 0;
    int review = 0;
    SyncFailure failure = SyncFailure::none;
    bool fatal = false;
    std::vector<std::string> failed_ids;
    std::vector<Item> items;
    std::vector<std::filesystem::path> downloaded_paths;
};

std::filesystem::path album_directory(const std::filesystem::path& destination);
bool is_default_album_folder(const std::filesystem::path& folder);
std::vector<LocalCapture> scan_capture_library(const std::filesystem::path& destination,
    const std::function<bool()>& cancelled = {});
std::vector<std::filesystem::path> recent_captures(
    const std::filesystem::path& destination,
    std::size_t limit = 8,
    const std::function<bool()>& cancelled = {});

class SyncEngine {
public:
    SyncEngine(ConfigManager& config, CoralClient& coral, HttpClient& http)
        : config_(config), coral_(coral), http_(http) {}

    SyncResult sync(
        const std::function<bool()>& cancelled = {},
        const std::function<void(const SyncProgress&)>& progress = {},
        const std::vector<std::string>& retry_ids = {});
    std::filesystem::path fetch_latest_capture(
        const std::function<void()>& video_download_started = {});

private:
    ConfigManager& config_;
    CoralClient& coral_;
    HttpClient& http_;

    std::string resolve_game_folder(
        const std::filesystem::path& album_directory,
        const std::string& app_name) const;
};

}  // namespace nso
