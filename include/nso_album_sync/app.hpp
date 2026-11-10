#pragma once

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/discord.hpp"
#include "nso_album_sync/game_services.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"
#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/splatnet.hpp"
#include "nso_album_sync/sync.hpp"
#include "nso_album_sync/zeldanotes.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace nso {

class App {
public:
    App();
    ~App();

    int run();

private:
    ConfigManager config_;
    HttpClient http_;
    NintendoAuthManager auth_;
    NxapiClient nxapi_;
    CoralClient coral_;
    SplatNetClient splatnet_;
    ZeldaNotesClient zeldanotes_;
    GameServicesClient game_services_;
    SyncEngine sync_;
    DiscordPresence discord_;
    PlatformUi ui_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> workers_joined_{false};
    std::atomic<bool> explicit_exit_{false};
    std::atomic<bool> auth_pending_{false};
    std::atomic<bool> manual_sign_in_{false};
    std::atomic<bool> operational_workers_started_{false};
    std::atomic<bool> presence_refresh_requested_{false};
    std::atomic<std::uint64_t> rpc_revision_{0};
    std::atomic<bool> initial_sync_deferred_{false};
    std::atomic<std::uint64_t> account_generation_{0};

    std::thread sync_thread_;
    std::thread auto_sync_thread_;
    std::thread presence_thread_;
    std::thread auth_callback_thread_;

    enum class SyncRequest { none, automatic, manual, retry, local_import };

    std::mutex sync_queue_mutex_;
    std::condition_variable sync_queue_cv_;
    SyncRequest sync_request_ = SyncRequest::none;
    std::string requested_capture_;
    std::string requested_scope_;
    bool sync_running_ = false;
    bool recent_refresh_requested_ = false;

    std::mutex auth_flow_mutex_;
    std::string pasted_auth_link_;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
    std::uint64_t auto_sync_revision_ = 0;
    std::mutex presence_sleep_mutex_;
    std::condition_variable presence_cv_;

    std::mutex menu_mutex_;
    std::mutex sync_context_mutex_;
    std::atomic<std::uint64_t> destination_generation_{0};
    std::mutex state_mutex_;
    std::string last_sync_ = "Never";
    std::string status_ = "Ready";
    std::string sync_detail_;
    std::string diagnostic_code_ = "none";
    SyncProgress sync_progress_;
    std::chrono::sys_seconds next_sync_{};
    std::vector<std::string> failed_ids_;
    std::shared_ptr<const Json::array> sync_history_;
    std::vector<std::filesystem::path> recent_captures_;
    std::shared_ptr<const std::vector<LocalCapture>> capture_library_;
    bool recent_loading_ = false;

    void update_menu();
    void sync_now(SyncRequest request, const std::string& capture, const std::string& scope);
    bool queue_sync(SyncRequest request, const std::string& capture = {}, const std::string& scope = {});
    void queue_recent_refresh();
    void refresh_recent_captures();
#ifdef _WIN32
    void import_captures(const std::string& source);
#endif
    void reschedule_auto_sync();
    void request_presence_refresh();
    void sign_in_or_out();
    void paste_sign_in_link();
    void complete_pending_login(const std::string& redirect_url_or_code);
    bool invalidate_session(std::uint64_t generation, bool expired);

    void start_workers();
    void start_operational_workers();
    void request_stop();
    void request_exit();
    void stop_workers();
    void sync_loop();
    void presence_loop();
    void auto_sync_loop();
    void auth_callback_loop();
    void wake_workers();
};

}  // namespace nso
