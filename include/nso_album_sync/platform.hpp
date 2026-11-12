#pragma once

#include "nso_album_sync/appearance.hpp"
#include "nso_album_sync/rpc_settings.hpp"
#include "nso_album_sync/json.hpp"
#include <memory>
#include <cstdint>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>


namespace nso {

struct LocalCapture {
    std::filesystem::path path;
    std::filesystem::path game_folder;
    std::int64_t modified = 0;
    std::uintmax_t bytes = 0;
    bool video = false;
    std::int64_t added_ms = 0;
};

struct MenuState {
    std::string nickname;
    std::string last_sync;
    std::string status;
    std::filesystem::path destination;
    bool watching_folder = false;
    std::filesystem::path backup_folder;
    std::string next_sync;
    std::string sync_detail;
    std::string credential_status;
    std::string diagnostics;
    std::vector<std::filesystem::path> recent_captures;
    std::shared_ptr<const std::vector<LocalCapture>> capture_library;
    std::filesystem::path settings_directory;
    std::shared_ptr<const Json::array> sync_history;
    std::string retry_scope;
    std::string next_retry;
    std::size_t sync_completed = 0;
    std::size_t sync_total = 0;

    bool auto_sync = false;
    bool notifications = false;
    bool discord = false;
    bool discord_available = false;
    RpcSettings rpc;
    bool start_on_boot = false;
    Appearance appearance = Appearance::system;
    bool signed_in = false;
    bool sign_in_pending = false;
    bool manual_sign_in = false;
    bool sync_busy = false;
    bool retry_available = false;
    bool legacy_credential = false;
    bool credential_warning = false;
    bool recent_loading = false;
    int sync_interval_minutes = 60;
};

struct PlatformCallbacks {
    std::function<void()> import_captures;
    std::function<void()> choose_watch_folder;
    std::function<void()> stop_watching_folder;
    std::function<void()> choose_backup_folder;
    std::function<void()> backup_now;
    std::function<void()> stop_backup;
    std::function<void()> auto_sync_options;
    // Portable preferences for Export/Import settings. Never includes sign-in
    // details, tokens, disclosure acknowledgements or PC-specific folders.
    std::function<Json()> export_settings;
    std::function<bool(const Json&)> import_settings;
    std::function<void()> ready;
    std::function<void()> sync_now;
    std::function<void()> retry_failed;
    std::function<void(const std::string&, const std::string&)> retry_capture;
    std::function<void()> refresh_captures;
    std::function<void()> clear_legacy_credential;
    std::function<void()> toggle_auto;
    std::function<void()> toggle_notifications;
    std::function<void()> toggle_discord;
    std::function<void()> toggle_rpc_zelda;
    std::function<void()> toggle_rpc_animal_crossing;
    std::function<void()> toggle_rpc_splatoon3;
    std::function<void()> toggle_rpc_splatoon2;
    std::function<void()> toggle_rpc_username;
    std::function<void()> toggle_rpc_profile_picture;
    std::function<void()> toggle_rpc_play_time;
    std::function<void()> refresh_rpc;
    std::function<void()> select_folder;
    std::function<void()> open_folder;
    std::function<void()> toggle_start;
    std::function<void(Appearance)> set_appearance;
    std::function<void()> proxy;
    std::function<void()> sign_in_out;
    std::function<void()> cancel_sign_in;
    std::function<void()> paste_sign_in_link;
    std::function<void()> exit;
};

class PlatformUi {
public:
    PlatformUi();
    ~PlatformUi();

    void run(const PlatformCallbacks& callbacks);
    void stop();
    void update(const MenuState& state);
    void notify(const std::string& title, const std::string& message);
    void notify_captures(const std::string& title, const std::string& message,
                         std::vector<std::filesystem::path> captures);
    bool copy_file_to_clipboard(const std::filesystem::path& path);

    std::string prompt(
        const std::string& title,
        const std::string& message,
        const std::string& initial = "");

    bool confirm(const std::string& title, const std::string& message);
    // A list of options with checkboxes. Returns the selected indexes, or
    // nothing if the user cancelled.
    std::optional<std::vector<int>> choose_options(
        const std::string& title,
        const std::string& message,
        const std::vector<std::string>& choices,
        const std::vector<int>& selected);
    std::string choose_folder(const std::string& initial, const wchar_t* title = L"Choose Album Folder");
    std::shared_ptr<const struct ImportPlan> choose_import_plan(const std::filesystem::path& album);

    struct Impl;

private:
    Impl* impl_;
};

void set_start_on_boot(bool enabled);
bool start_on_boot_enabled();
bool show_running_dashboard();

}  // namespace nso
