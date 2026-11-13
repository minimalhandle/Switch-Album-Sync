#pragma once

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/platform.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nso {

enum class SyncStage { checking, downloading };
inline constexpr std::int64_t kExpiryWarningSeconds = 3 * 24 * 60 * 60;
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
    // Failed captures that Nintendo's album removes within kExpiryWarning,
    // from the expiry dates the album listing already includes.
    int expiring_soon = 0;
    SyncFailure failure = SyncFailure::none;
    bool fatal = false;
    // The failure was a dropped connection with no reply (TransientHttpError).
    // Only these may be retried automatically; an error response never is.
    bool transient = false;
    // Technical reason for a failure (step and HTTP status). Never contains tokens.
    std::string error_detail;
    std::vector<std::string> failed_ids;
    std::vector<Item> items;
    std::vector<std::filesystem::path> downloaded_paths;
};

std::filesystem::path album_directory(const std::filesystem::path& destination);
bool is_default_album_folder(const std::filesystem::path& folder);
std::vector<LocalCapture> scan_capture_library(const std::filesystem::path& destination,
    const std::function<bool()>& cancelled = {});

// Remembers which Nintendo captures were already synced to an album, so a
// capture you delete, rename or move isn't downloaded again, and a time-zone
// change can't create duplicates. Kept per album in the settings folder;
// entries are dropped a week after Nintendo's copy expires.
class SyncReceipts {
public:
    SyncReceipts(std::filesystem::path file, const std::filesystem::path& album);

    bool contains(const std::string& media_id) const;
    // expires_at is Unix time in seconds or milliseconds; 0 uses captured_at.
    void add(const std::string& media_id, std::int64_t expires_at, std::int64_t captured_at);
    // Writes the file if anything changed. Returns false if it couldn't be written.
    bool save();

private:
    std::filesystem::path file_;
    std::string album_key_;
    Json::object albums_;
    Json::object entries_;
    bool dirty_ = false;
};

// Exposed for tests.
bool is_safe_media_url(const std::string& url);
// The 16-digit local-time prefix used in synced capture filenames.
std::string capture_filename_prefix(std::int64_t timestamp);
// The album subfolder for a game title, matching existing folders and known
// localized names before falling back to a sanitized title.
std::string resolve_game_folder(const std::filesystem::path& album_directory, const std::string& app_name);

class SyncEngine {
public:
    SyncEngine(ConfigManager& config, CoralClient& coral, HttpClient& http)
        : config_(config), coral_(coral), http_(http) {}

    SyncResult sync(
        const std::function<bool()>& cancelled = {},
        const std::function<void(const SyncProgress&)>& progress = {},
        const std::vector<std::string>& retry_ids = {});

private:
    ConfigManager& config_;
    CoralClient& coral_;
    HttpClient& http_;
};

}  // namespace nso
