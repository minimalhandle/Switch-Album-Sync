#include "nso_album_sync/rpc_enricher.hpp"

#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/game_services.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/splatnet.hpp"
#include "nso_album_sync/zeldanotes.hpp"

#include <exception>

namespace nso {

RpcEnricher::RpcEnricher(
    CoralClient& coral,
    NintendoAuthManager& auth,
    SplatNetClient& splatnet,
    ZeldaNotesClient& zeldanotes,
    GameServicesClient& game_services)
    : coral_(coral),
      auth_(auth),
      splatnet_(splatnet),
      zeldanotes_(zeldanotes),
      game_services_(game_services) {}

bool RpcEnricher::enrich(
    RpcGameService service,
    const std::string& session_token,
    const std::function<bool()>& still_current,
    NintendoPresence& presence) {
    if (service == RpcGameService::None) return false;
    use_account_locale(session_token);
    if (still_current && !still_current()) return false;

    // Every game service is best effort: a failure keeps Coral's generic
    // presence. An expired Nintendo session is the exception, so App can sign
    // out instead of retrying with a dead account.
    try {
        switch (service) {
            case RpcGameService::Splatoon3:
                add_splatoon3(session_token, presence);
                return false;
            case RpcGameService::ZeldaNotes:
                return prepare_zelda_notes(session_token);
            case RpcGameService::AnimalCrossing:
                add_animal_crossing(session_token, presence);
                return false;
            case RpcGameService::Splatoon2:
                add_splatoon2(session_token, presence);
                return false;
            case RpcGameService::None:
                return false;
        }
    } catch (const NintendoSessionExpired&) {
        throw;
    } catch (...) {
    }
    return false;
}

void RpcEnricher::use_account_locale(const std::string& session_token) {
    // Game WebView bootstraps use the Nintendo Account locale, as in nxapi.
    // This profile data is only needed for the one enrichment probe.
    try {
        const auto tokens = auth_.exchange_session_token(session_token);
        const auto profile = auth_.fetch_profile(tokens.access_token);
        splatnet_.set_locale(profile.language, profile.country);
        zeldanotes_.set_locale(profile.language, profile.country);
        game_services_.set_locale(profile.language, profile.country);
    } catch (const NintendoSessionExpired&) {
        throw;
    } catch (...) {
        // The service clients keep safe en-GB/GB defaults.
    }
}

void RpcEnricher::add_splatoon3(const std::string& session_token, NintendoPresence& presence) {
    const auto web_token = coral_.get_web_service_token(session_token, kSplatoon3GameServiceId);
    if (web_token.empty()) return;
    const auto splat = splatnet_.fetch_presence(web_token);
    if (!splat.active) return;
    presence.custom_details = splat.format_details();
    presence.custom_details_without_name = splat.title;
    presence.custom_state = splat.format_state();
    if (!splat.stage_image_uri.empty()) presence.custom_image_uri = splat.stage_image_uri;
}

bool RpcEnricher::prepare_zelda_notes(const std::string& session_token) {
    for (const auto service_id : {kZeldaNotesGameServiceId, kZeldaNotesGameServiceIdAlt}) {
        try {
            const auto token = coral_.get_web_service_token(session_token, service_id);
            if (zeldanotes_.prepare_session(token)) return true;
        } catch (const NintendoSessionExpired&) {
            throw;
        } catch (const std::exception&) {
            // Try the alternate service if Nintendo rejects this one.
        }
    }
    return false;
}

void RpcEnricher::add_animal_crossing(const std::string& session_token, NintendoPresence& presence) {
    const auto web_token = coral_.get_web_service_token(session_token, kAnimalCrossingGameServiceId);
    if (web_token.empty()) return;
    const auto island = game_services_.fetch_animal_crossing_presence(web_token);
    if (!island.active) return;
    const auto state = island.format_state();
    const auto details = island.format_details();
    presence.custom_details_without_name = island.island_name;
    if (!state.empty()) presence.custom_state = state;
    if (!details.empty()) presence.custom_details = details;
    // Discord accepts image URLs up to 300 characters.
    if (!island.image_uri.empty() && island.image_uri.size() <= 300) {
        presence.profile_image_uri = island.image_uri;
    }
}

void RpcEnricher::add_splatoon2(const std::string& session_token, NintendoPresence& presence) {
    const auto web_token = coral_.get_web_service_token(session_token, kSplatoon2GameServiceId);
    if (web_token.empty()) return;
    const auto splat = game_services_.fetch_splatoon2_presence(web_token);
    if (!splat.active) return;
    const auto state = splat.format_state();
    const auto details = splat.format_details();
    presence.custom_details_without_name.clear();
    if (!state.empty()) presence.custom_state = state;
    if (!details.empty()) presence.custom_details = details;
    if (!splat.stage_image_uri.empty()) presence.custom_image_uri = splat.stage_image_uri;
}

}  // namespace nso
