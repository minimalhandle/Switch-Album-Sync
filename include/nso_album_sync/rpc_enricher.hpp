#pragma once

#include "nso_album_sync/rpc.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace nso {

class CoralClient;
class GameServicesClient;
class NintendoAuthManager;
class SplatNetClient;
class ZeldaNotesClient;

// Adds game-specific Rich Presence details (Splatoon 3, Splatoon 2, Animal
// Crossing, Zelda Notes) from Nintendo's game web services. App calls it once
// per game session and caches the result (RpcEnrichmentCache).
class RpcEnricher {
public:
    RpcEnricher(
        CoralClient& coral,
        NintendoAuthManager& auth,
        SplatNetClient& splatnet,
        ZeldaNotesClient& zeldanotes,
        GameServicesClient& game_services);

    // Fills presence.custom_* for `service`. `still_current` is checked before
    // any game service is contacted, so signing out mid-probe stops early.
    // Returns true when a Zelda Notes live session is ready.
    // Throws NintendoSessionExpired; any other failure leaves `presence` as
    // Coral reported it.
    bool enrich(
        RpcGameService service,
        const std::string& session_token,
        const std::function<bool()>& still_current,
        NintendoPresence& presence);

private:
    CoralClient& coral_;
    NintendoAuthManager& auth_;
    SplatNetClient& splatnet_;
    ZeldaNotesClient& zeldanotes_;
    GameServicesClient& game_services_;
    // The Zelda Notes service ID that last worked, tried first next time so a
    // probe normally needs one web-service token, not two.
    std::uint64_t zelda_service_id_ = 0;

    void use_account_locale(const std::string& session_token);
    void add_splatoon3(const std::string& session_token, NintendoPresence& presence);
    bool prepare_zelda_notes(const std::string& session_token);
    void add_animal_crossing(const std::string& session_token, NintendoPresence& presence);
    void add_splatoon2(const std::string& session_token, NintendoPresence& presence);
};

}  // namespace nso
