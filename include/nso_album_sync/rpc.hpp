#pragma once
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/rpc_settings.hpp"
#include "nso_album_sync/zeldanotes.hpp"
#include <chrono>
#include <initializer_list>

namespace nso {
enum class RpcGameService {
    None,
    Splatoon3,
    ZeldaNotes,
    AnimalCrossing,
    Splatoon2,
};

// Rich Presence polls Nintendo for your status. Keep a steady pace while a game
// is running, and slow down once a few checks in a row find you offline (or
// fail), so an idle PC makes far fewer requests: every minute while playing,
// every 5 minutes once you're offline, every 15 minutes after about an hour
// offline. Explicit refreshes still run immediately.
inline constexpr auto kPresencePlayingInterval = std::chrono::seconds(60);
inline constexpr auto kPresenceOfflineInterval = std::chrono::seconds(300);
inline constexpr auto kPresenceLongOfflineInterval = std::chrono::seconds(900);
inline constexpr int kPresenceOfflineChecksBeforeBackoff = 3;
// 3 one-minute checks, then 12 five-minute checks: about an hour offline.
inline constexpr int kPresenceOfflineChecksBeforeLongBackoff = kPresenceOfflineChecksBeforeBackoff + 12;
// After this long offline, checks stop until someone uses the PC again, so a
// PC left on (with Discord open) doesn't contact Nintendo all night.
inline constexpr auto kPresenceIdlePauseAfter = std::chrono::hours(8);
inline constexpr auto kPresenceUserActiveWithin = std::chrono::minutes(15);

inline std::chrono::seconds presence_poll_interval(bool playing, int consecutive_offline_polls) {
    if (playing || consecutive_offline_polls < kPresenceOfflineChecksBeforeBackoff) {
        return kPresencePlayingInterval;
    }
    return consecutive_offline_polls < kPresenceOfflineChecksBeforeLongBackoff
        ? kPresenceOfflineInterval
        : kPresenceLongOfflineInterval;
}

// True when polling should pause: offline for kPresenceIdlePauseAfter and
// nobody has used this PC within kPresenceUserActiveWithin.
inline bool presence_idle_paused(std::chrono::steady_clock::duration offline_for,
                                 std::chrono::steady_clock::duration pc_idle_for) {
    return offline_for >= kPresenceIdlePauseAfter && pc_idle_for >= kPresenceUserActiveWithin;
}

inline bool rpc_contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const auto* needle : needles) {
        if (needle != nullptr && text.find(needle) != std::string::npos) return true;
    }
    return false;
}

inline RpcGameService rpc_game_service_for(const NintendoPresence& presence) {
    // Prefer Nintendo's stable application/title ID. Coral names are localized,
    // so routing only by English/Japanese substrings silently disables RPC
    // enrichment for other Nintendo Account languages.
    if (presence.title_id == "0100c2500fc20000") return RpcGameService::Splatoon3;
    if (presence.title_id == "01003bc0000a0000") return RpcGameService::Splatoon2;
    if (presence.title_id == "01006f8002326000") return RpcGameService::AnimalCrossing;
    if (zelda_notes_game_for_title_id(presence.title_id) != ZeldaNotesGame::Unknown) {
        return RpcGameService::ZeldaNotes;
    }

    // Name fallbacks cover Coral payloads that omit an application ID and
    // Nintendo Switch 2 Edition display names. Keep Zelda deliberately narrow:
    // Zelda Notes supports BOTW/TOTK, not every Zelda-family title.
    if (rpc_contains_any(presence.game_name, {"Splatoon 3", "スプラトゥーン3"})) {
        return RpcGameService::Splatoon3;
    }
    if (rpc_contains_any(presence.game_name, {
            "Breath of the Wild", "ブレス オブ ザ ワイルド",
            "Tears of the Kingdom", "ティアーズ オブ ザ キングダム"})) {
        return RpcGameService::ZeldaNotes;
    }
    if (rpc_contains_any(presence.game_name, {
            "Animal Crossing", "New Horizons", "どうぶつの森", "あつ森"})) {
        return RpcGameService::AnimalCrossing;
    }
    if (rpc_contains_any(presence.game_name, {"Splatoon 2", "スプラトゥーン2"})) {
        return RpcGameService::Splatoon2;
    }
    return RpcGameService::None;
}

inline bool rpc_service_enabled(RpcGameService service, const RpcSettings& settings) {
    switch (service) {
        case RpcGameService::ZeldaNotes: return settings.zelda;
        case RpcGameService::AnimalCrossing: return settings.animal_crossing;
        case RpcGameService::Splatoon3: return settings.splatoon3;
        case RpcGameService::Splatoon2: return settings.splatoon2;
        case RpcGameService::None: return false;
    }
    return false;
}

// One game-details probe per game session. A failed probe is not repeated
// automatically (nxapi's terms forbid automatic retries after an error
// response); Refresh presence, a settings change or the next game starts over.
struct RpcEnrichmentCache {
    using Clock = std::chrono::steady_clock;
    std::string game_key;
    std::uint64_t account = 0;
    std::uint64_t revision = 0;
    bool success = false;
    Clock::time_point retry_at{};
    std::string details, details_without_name, state, image, profile_image;

    bool begin(const std::string& key, std::uint64_t generation, std::uint64_t settings_revision) {
        if (game_key == key && account == generation && revision == settings_revision) return false;
        *this = {};
        game_key = key;
        account = generation;
        revision = settings_revision;
        return true;
    }
    bool should_probe(Clock::time_point now) const { return !success && now >= retry_at; }
    void complete(const NintendoPresence& presence, bool ready, Clock::time_point now) {
        success = ready || !presence.custom_details.empty() ||
            !presence.custom_state.empty() || !presence.custom_image_uri.empty();
        details = presence.custom_details;
        details_without_name = presence.custom_details_without_name;
        profile_image = presence.profile_image_uri;
        state = presence.custom_state;
        image = presence.custom_image_uri;
        (void)now;
        retry_at = Clock::time_point::max();
    }
    void apply(NintendoPresence& presence) const {
        presence.custom_details = details;
        presence.custom_details_without_name = details_without_name;
        if (!profile_image.empty()) presence.profile_image_uri = profile_image;
        presence.custom_state = state;
        presence.custom_image_uri = image;
    }
};
inline NintendoPresence rpc_display_presence(NintendoPresence presence) {
    if (!rpc_service_enabled(rpc_game_service_for(presence), presence.rpc)) {
        presence.custom_details.clear();
        presence.custom_details_without_name.clear();
        presence.custom_state.clear();
        presence.custom_image_uri.clear();
        presence.custom_large_image_uri.clear();
        presence.custom_large_text.clear();
    }
    presence.rpc.show_username = presence.rpc.show_username && presence.rpc.show_profile_picture;
    if (!presence.rpc.show_username) {
        presence.user_name.clear();
        presence.custom_details = presence.custom_details_without_name;
    }
    if (!presence.rpc.show_profile_picture) presence.profile_image_uri.clear();
    if (!presence.rpc.show_play_time) {
        presence.sys_description.clear();
        presence.total_play_time = 0;
    }
    return presence;
}

inline std::string rpc_store_url(const NintendoPresence& presence) {
    // Build a public link from the title only. Never publish Nintendo's
    // account-localized shopUri or its query parameters.
    auto title_id = presence.title_id;
    constexpr char prefix[] = "https://ec.nintendo.com/apps/";
    if (title_id.empty() && presence.shop_uri.rfind(prefix, 0) == 0) {
        title_id = presence.shop_uri.substr(sizeof(prefix) - 1, 16);
    }
    if (title_id.size() != 16) return {};
    for (auto& character : title_id) {
        if (character >= 'A' && character <= 'F') character += 'a' - 'A';
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) return {};
    }
    return std::string(prefix) + title_id + "/US";
}
}  // namespace nso
