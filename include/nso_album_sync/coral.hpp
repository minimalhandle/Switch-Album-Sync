#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/rpc_settings.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nso {

struct MediaItem {
    std::string id;
    std::string title_id;
    std::string app_name;
    std::string type;
    std::string content_uri;
    std::string thumbnail_uri;
    std::int64_t content_length = 0;
    std::int64_t captured_at = 0;
    std::int64_t uploaded_at = 0;
    std::int64_t expires_at = 0;
};

struct NintendoPresence {
    std::string state = "OFFLINE";
    std::string platform;
    std::string user_name;
    std::string game_name;
    std::string title_id;
    std::string image_uri;
    std::string shop_uri;
    std::string sys_description;
    std::int64_t updated_at = 0;
    // Local start of the continuously observed game, in Unix milliseconds.
    std::int64_t elapsed_started_at_ms = 0;
    std::int64_t total_play_time = 0;

    RpcSettings rpc;
    bool zelda_notes_enabled = true;
    std::string profile_image_uri;
    std::string custom_details_without_name;
    std::string custom_state;
    std::string custom_details;
    std::string custom_image_uri;
    std::string custom_large_image_uri;
    std::string custom_large_text;

    bool is_playing() const { return state == "ONLINE" || state == "PLAYING"; }
    std::string console_name() const;
    std::string discord_state() const;
};

class CoralClient {
public:
    CoralClient(
        HttpClient& http,
        NintendoAuthManager& auth,
        NxapiClient& nxapi)
        : http_(http),
          auth_(auth),
          nxapi_(nxapi) {}

    std::vector<MediaItem> media_list(const std::string& session_token);
    NintendoPresence self_presence(const std::string& session_token);
    std::string get_web_service_token(
        const std::string& session_token,
        std::uint64_t game_service_id = 0);
    void clear_cached_session();
    // The User-Agent the app's Coral requests use, for other requests to
    // Nintendo services (imitating the official app, as the nxapi terms advise).
    std::string app_user_agent();
    // The Nintendo Switch Online app version these requests identify as
    // (nxapi's supported version, cached).
    std::string app_version();
    // The Nintendo Account language and country recorded at the last Coral
    // sign-in (empty if unknown). Game web services use them, as nxapi does,
    // without asking Nintendo for the account profile again.
    std::pair<std::string, std::string> account_locale();

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    NintendoAuthManager& auth_;
    NxapiClient& nxapi_;

    std::mutex session_mutex_;
    std::mutex login_mutex_;
    std::mutex request_mutex_;
    std::atomic<std::uint64_t> session_generation_{0};
    std::string coral_access_token_;
    std::string cached_session_token_;
    std::string user_id_;
    std::string na_id_;
    std::string na_language_;
    std::string na_country_;
    Clock::time_point coral_token_expiry_{};

    struct CachedWebServiceToken {
        std::string token;
        Clock::time_point expires_at{};
    };
    std::unordered_map<std::uint64_t, CachedWebServiceToken> web_service_tokens_;

    std::string ensure_session(const std::string& session_token);
    Json coral_call(
        const std::string& url,
        const std::string& access_token,
        const std::string& request_body);

    bool restore_persistent_session(
        const std::string& session_token,
        Clock::time_point now);
    void persist_session(const std::string& session_token);
};

}  // namespace nso
