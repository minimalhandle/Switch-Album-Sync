#pragma once

#include "nso_album_sync/rpc_settings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kDiscordApplicationId = 1569740622355632200ULL;

enum class SessionStorage { none, secure, session_only, legacy, unavailable };

struct AppConfig {
    std::string session_token;
    SessionStorage session_storage = SessionStorage::none;
    std::string user_nickname = "Nintendo Switch Player";
    std::string destination_folder;

    bool auto_sync = false;
    int auto_sync_setting_version = 1;
    bool notifications = false;
    bool discord_presence = false;
    RpcSettings rpc;
    int discord_presence_setting_version = 1;
    bool start_on_boot = false;

    int sync_interval_minutes = 60;
    std::string last_sync = "Never";

    std::string proxy_url;
    std::string nxapi_auth_client_id = "eJ8TDme0c-Z4czx5SvZabA";
    std::uint64_t discord_application_id = kDiscordApplicationId;
};

std::string default_album_folder();

class ConfigManager {
public:
    ConfigManager();

    // Thread-safe snapshot/update APIs. The application has UI, presence and
    // sync threads, so callers should not retain references to the live config.
    AppConfig snapshot() const;
    AppConfig update(const std::function<void(AppConfig&)>& updater);

    void load();
    void save();
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
