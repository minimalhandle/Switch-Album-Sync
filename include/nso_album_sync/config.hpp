#pragma once

#include "nso_album_sync/appearance.hpp"
#include "nso_album_sync/rpc_settings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kDiscordApplicationId = 1569740622355632200ULL;

// Bump these when the wording of a disclosure changes materially, so everyone
// sees and acknowledges the new text once.
inline constexpr int kNxapiDisclosureVersion = 1;
inline constexpr int kDiscordDisclosureVersion = 1;

// Automatic syncs never run more often than this, even if config.json asks for it.
inline constexpr int kMinimumSyncIntervalMinutes = 15;

enum class SessionStorage { none, secure, session_only, legacy, unavailable };

struct AppConfig {
    std::string session_token;
    SessionStorage session_storage = SessionStorage::none;
    std::string user_nickname = "Nintendo Switch Player";
    std::string destination_folder;
    std::string watch_folder;
    // Optional second copy of the album. Empty means no backups.
    std::string backup_folder;

    bool auto_sync = false;
    // Automatic syncs are skipped (never deferred into a burst) when these apply.
    bool pause_on_battery_saver = true;
    bool pause_on_metered = true;
    bool quiet_hours = false;
    int quiet_start_hour = 23;
    int quiet_end_hour = 7;
    int auto_sync_setting_version = 1;
    bool notifications = false;
    bool discord_presence = false;
    RpcSettings rpc;
    int discord_presence_setting_version = 1;
    bool start_on_boot = false;
    Appearance appearance = Appearance::system;

    // Highest disclosure version the user explicitly acknowledged (0 = never).
    int nxapi_disclosure_acknowledged = 0;
    int discord_disclosure_acknowledged = 0;

    int sync_interval_minutes = 60;
    std::string last_sync = "Never";

    std::string proxy_url;
    std::uint64_t discord_application_id = kDiscordApplicationId;
};

// True when a saved sign-in exists but the user hasn't acknowledged the current
// third-party service disclosure, so nxapi must not be contacted yet.
inline bool needs_nxapi_disclosure(const AppConfig& config) {
    return !config.session_token.empty() &&
        config.nxapi_disclosure_acknowledged < kNxapiDisclosureVersion;
}

std::string default_album_folder();

class ConfigManager {
public:
    ConfigManager();

    // Thread-safe snapshot/update APIs. The application has UI, presence and
    // sync threads, so callers should not retain references to the live config.
    AppConfig snapshot() const;
    AppConfig update(const std::function<void(AppConfig&)>& updater);

    void load();
    void clear_session();
    void clear_legacy_credential();

    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path config_file_;
    AppConfig config_;
    std::string legacy_session_marker_;
    bool secure_marker_on_disk_ = false;
    mutable std::mutex mutex_;

    void save_locked();
};

}  // namespace nso
