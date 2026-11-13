#include "nso_album_sync/app.hpp"
#include "import_windows.hpp"
#include "nso_album_sync/auto_sync_policy.hpp"
#include "nso_album_sync/backup.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/rpc.hpp"

#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/util.hpp"
#include "nso_album_sync/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <random>
#include <string_view>
#include <thread>

namespace nso {
namespace {

constexpr auto kAuthCallbackPollInterval = std::chrono::milliseconds(150);
constexpr auto kExitWatchdogDelay = std::chrono::seconds(5);
// While Rich Presence is on but no Discord client is running, check for one
// locally at this cadence instead of asking Nintendo for presence.
constexpr auto kDiscordClientCheckInterval = std::chrono::seconds(30);
constexpr std::int64_t kPollingJitterDivisor = 50;  // +/- 2%

// platform_windows.cpp shows this title with the nxapi project links.
constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kManualSignInInstructions[] =
    "Right-click ‘Select this account’ on Nintendo’s page, copy its link address, then paste it below.";
constexpr char kNxapiDisclosureBody[] =
    "Switch Album Sync uses nxapi-znca-api, a third-party service run by the nxapi "
    "project (nxapi-znca-api.fancy.org.uk, with nxapi-auth.fancy.org.uk), to sign, "
    "encrypt and decrypt Nintendo Switch Online requests.\n\n"
    "While you use Nintendo Switch Online features, your Nintendo Account id_token "
    "and the account details it carries (Nintendo Account ID, birthday, country and "
    "language), your Coral access token, and every Coral API request and response "
    "this app makes are sent to and processed by that service. These tokens can "
    "access Nintendo services while they remain valid. Your long-term Nintendo "
    "sign-in stays on this PC.\n\n"
    "Discord Rich Presence is optional and off until you turn it on. It shares the "
    "game you're playing with Discord and asks before it starts.\n\n"
    "Local imports, folder watching and the capture library don't use these services.";
constexpr char kNxapiSignInQuestion[] = "\n\nContinue with Nintendo Account sign-in?";
constexpr char kNxapiSignedInQuestion[] =
    "\n\nYou're already signed in. Continue to keep using Nintendo Switch Online "
    "features, or cancel to sign out.";

constexpr char kDiscordDisclosureTitle[] = "Turn on Discord Rich Presence?";
constexpr char kDiscordDisclosureEnabledTitle[] = "Discord Rich Presence";
constexpr char kDiscordDisclosureBody[] =
    "Rich Presence shows what you're playing on your Discord profile. While it's on, "
    "Switch Album Sync checks your Nintendo Switch Online status and sends the "
    "Discord app on this PC:\n\n"
    "• the game you're playing and which console\n"
    "• your Nintendo Switch username, profile picture and play time, unless you turn "
    "them off in Rich Presence settings\n"
    "• details for games you've enabled: Splatoon rank and weapon, Animal Crossing "
    "island and resident, Zelda Notes location\n\n"
    "Discord shows this to people who can see your activity and loads the images "
    "from Nintendo, splatoon3.ink and Zelda Wiki. Game details come straight from "
    "Nintendo's game services. Only the sign-in for each game service goes through "
    "nxapi, like other Nintendo Switch Online requests.";
constexpr char kDiscordTurnOnQuestion[] = "\n\nTurn on Rich Presence?";
constexpr char kDiscordKeepOnQuestion[] = "\n\nRich Presence is already on. Keep it on?";

std::chrono::milliseconds jittered_interval(std::chrono::milliseconds nominal) {
    const auto nominal_ms = std::max<std::int64_t>(1, nominal.count());
    const auto jitter_ms = std::max<std::int64_t>(1, nominal_ms / kPollingJitterDivisor);

    static thread_local std::mt19937_64 generator([] {
        std::random_device random;
        std::seed_seq seed{
            random(), random(), random(), random(),
            random(), random(), random(), random(),
        };
        return std::mt19937_64(seed);
    }());

    std::uniform_int_distribution<std::int64_t> distribution(-jitter_ms, jitter_ms);
    return std::chrono::milliseconds(
        std::max<std::int64_t>(1, nominal_ms + distribution(generator)));
}

std::string local_time_text(std::chrono::sys_seconds time) {
    const auto now = static_cast<std::time_t>(time.time_since_epoch().count());
    std::tm local_time{};
    if (localtime_s(&local_time, &now) != 0) return "Unavailable";
    char buffer[48];
    std::strftime(buffer, sizeof(buffer), "%H:%M (%Y-%m-%d)", &local_time);
    return buffer;
}

const char* failure_code(SyncFailure failure) {
    switch (failure) {
        case SyncFailure::sign_in: return "sign_in_required";
        case SyncFailure::service: return "album_service_unavailable";
        case SyncFailure::download: return "capture_download_failed";
        case SyncFailure::storage: return "album_storage_failed";
        case SyncFailure::none: return "none";
    }
    return "sync_failed";
}

const char* failure_message(SyncFailure failure) {
    switch (failure) {
        case SyncFailure::sign_in: return "Sign in to your Nintendo Account to sync.";
        case SyncFailure::service: return "Could not check Nintendo's album. Try syncing again shortly.";
        case SyncFailure::download: return "Some captures could not be downloaded.";
        case SyncFailure::storage: return "Could not save captures. Check folder access and free space.";
        case SyncFailure::none: return "";
    }
    return "Sync failed. Try again.";
}

// Only what changed: captures already on this PC aren't news, so they're left
// out, and a sync with nothing new reports nothing.
std::string sync_counts(int downloaded, int failed) {
    std::string result;
    if (downloaded > 0) result = std::to_string(downloaded) + " new";
    if (failed > 0) result += (result.empty() ? "" : ", ") + std::to_string(failed) + " failed";
    return result;
}

bool presence_enabled(const AppConfig& config) {
    return discord_available && config.discord_presence;
}

std::string history_scope(const AppConfig& config) {
    if (config.session_token.empty()) return {};
    return base64url(sha256(config.session_token + "\n" + config.destination_folder));
}

const char* credential_status(SessionStorage storage) {
    switch (storage) {
        case SessionStorage::secure: return "Sign-in saved in your system credential store.";
        case SessionStorage::session_only: return "Sign-in is available until you quit. Your system credential store did not save it.";
        case SessionStorage::legacy: return "An older credential is still in the settings file. You can clear the saved copy.";
        case SessionStorage::unavailable: return "The saved credential could not be read. Sign in again to reconnect.";
        case SessionStorage::none: return "";
    }
    return "";
}

}  // namespace

App::App()
    : auth_(http_),
      nxapi_(
          http_,
          NSO_NXAPI_AUTH_CLIENT_ID,
          config_.directory() / "nxapi-cache.json"),
      coral_(http_, auth_, nxapi_),
      splatnet_(http_),
      zeldanotes_(http_),
      game_services_(http_),
      rpc_enricher_(coral_, auth_, splatnet_, zeldanotes_, game_services_),
      sync_(config_, coral_, http_),
      discord_(config_.snapshot().discord_application_id) {
    const auto config = config_.snapshot();
    http_.set_proxy(config.proxy_url);
    discord_.set_rpc_settings(config.rpc);
    last_sync_ = config.last_sync.empty() ? "Never" : config.last_sync;
    const auto history = read_local_data(config_.directory() / "sync-history.json");
    if (history.is_array()) sync_history_ = std::make_shared<const Json::array>(history.as_array());
    if (debug_logging_enabled()) {
        std::cerr << "[auth] Saved session available: "
                  << (!config.session_token.empty() ? "yes" : "no") << '\n';
        if (!config.session_token.empty()) {
            try {
                const auto tokens = auth_.exchange_session_token(config.session_token);
                std::cerr << "[auth] Token exchange succeeded\n";
                const auto profile = auth_.fetch_profile(tokens.access_token);
                std::cerr << "[auth] Profile request succeeded; nickname present: "
                          << (!profile.nickname.empty() ? "yes" : "no") << '\n';
                if (!profile.nickname.empty()) {
                    config_.update([&](AppConfig& value) {
                        value.user_nickname = profile.nickname;
                    });
                }
            } catch (const std::exception&) {
                std::cerr << "[auth] Startup profile check failed\n";
            }
        }
    }
}

App::~App() {
    stop_workers();
}

void App::wake_workers() {
    reschedule_auto_sync();
    {
        std::lock_guard lock(presence_sleep_mutex_);
        presence_cv_.notify_all();
    }
    sync_queue_cv_.notify_all();
}

void App::reschedule_auto_sync() {
    {
        std::lock_guard lock(sleep_mutex_);
        ++auto_sync_revision_;
    }
    sleep_cv_.notify_all();
}

void App::update_menu() {
    std::lock_guard menu_lock(menu_mutex_);
    const auto config = config_.snapshot();
    MenuState menu;
    menu.settings_directory = config_.directory();
    menu.retry_scope = history_scope(config);
    menu.nickname = config.user_nickname;
    menu.signed_in = !config.session_token.empty();
    menu.sign_in_pending = auth_pending_;
    menu.manual_sign_in = menu.sign_in_pending && manual_sign_in_;
    menu.destination = album_directory(path_from_utf8(config.destination_folder));
    menu.watching_folder = !config.watch_folder.empty();
    menu.backup_folder = path_from_utf8(config.backup_folder);
    std::error_code path_error;
    const auto absolute_destination = std::filesystem::absolute(menu.destination, path_error);
    if (!path_error) menu.destination = absolute_destination.lexically_normal().make_preferred();
    menu.credential_status = credential_status(config.session_storage);
    menu.legacy_credential = config.session_storage == SessionStorage::legacy;
    menu.credential_warning = menu.legacy_credential ||
        config.session_storage == SessionStorage::session_only ||
        config.session_storage == SessionStorage::unavailable;
    menu.next_sync = !config.auto_sync ? "Off" :
        (!menu.signed_in ? "Sign in to schedule" : "Scheduling…");
    {
        std::lock_guard lock(state_mutex_);
        menu.last_sync = last_sync_;
        menu.status = status_;
        menu.sync_detail = sync_detail_;
        menu.sync_completed = sync_progress_.new_downloads + sync_progress_.skipped + sync_progress_.failed;
        menu.sync_total = sync_progress_.total_found;
        menu.retry_available = menu.signed_in && !failed_ids_.empty();
        menu.recent_captures = recent_captures_;
        menu.capture_library = capture_library_;
        menu.sync_history = sync_history_;
        menu.recent_loading = recent_loading_;
        if (config.auto_sync && menu.signed_in && next_sync_ != std::chrono::sys_seconds{}) {
            menu.next_sync = local_time_text(next_sync_);
        }
        // Only application-generated codes, flags, and counts belong here.
        menu.diagnostics = "Switch Album Sync " NSO_APP_VERSION
            "\nnxapi client: " NSO_NXAPI_AUTH_CLIENT_ID
            "\nSigned in: ";
        menu.diagnostics += menu.signed_in ? "yes\n" : "no\n";
        menu.diagnostics += config.auto_sync ? "Auto-sync: on\n" : "Auto-sync: off\n";
        menu.diagnostics += discord_available ? "Discord support: yes\n" : "Discord support: no\n";
        menu.diagnostics += "Latest event: " + diagnostic_code_ + "\n";
        const auto requests = requests_today();
        menu.diagnostics += "Requests today: " + std::to_string(requests.nintendo) + " to Nintendo, " +
            std::to_string(requests.nxapi) + " to nxapi\n";
        if (!last_error_.empty()) menu.diagnostics += "Latest error: " + last_error_ + "\n";
        // Diagnostics keep every count, including captures already present.
        menu.diagnostics += "Latest sync: " + std::to_string(sync_progress_.new_downloads) + " new, " +
            std::to_string(sync_progress_.skipped) + " skipped, " +
            std::to_string(sync_progress_.failed) + " failed\n";
    }
    menu.auto_sync = config.auto_sync;
    menu.notifications = config.notifications;
    menu.discord_available = discord_available;
    menu.discord = presence_enabled(config);
    menu.rpc = config.rpc;
    menu.start_on_boot = start_on_boot_enabled();
    menu.appearance = config.appearance;
    {
        std::lock_guard lock(sync_queue_mutex_);
        menu.sync_busy = sync_request_ != SyncRequest::none || sync_running_;
        const auto retry = next_retry_locked();
        if (retry != std::chrono::steady_clock::time_point::max()) {
            menu.next_retry = local_time_text(std::chrono::floor<std::chrono::seconds>(
                std::chrono::system_clock::now() + (retry - std::chrono::steady_clock::now())));
        }
    }
    menu.sync_interval_minutes = std::max(1, config.sync_interval_minutes);
    ui_.update(menu);
}

std::chrono::steady_clock::time_point App::next_retry_locked() const {
    auto next = std::chrono::steady_clock::time_point::max();
    for (const auto& [id, retry] : pending_retries_) next = std::min(next, retry.due);
    return next;
}

void App::clear_pending_retries() {
    std::lock_guard queue_lock(sync_queue_mutex_);
    pending_retries_.clear();
    sync_queue_cv_.notify_one();
}

bool App::invalidate_session(std::uint64_t generation, bool expired) {
    {
        std::lock_guard context_lock(sync_context_mutex_);
        if (account_generation_.load() != generation) return false;
        account_generation_.fetch_add(1);
        initial_sync_deferred_.store(false, std::memory_order_release);
        coral_.clear_cached_session();
        splatnet_.clear_cache();
        zeldanotes_.clear_cache();
        game_services_.clear_cache();
        nxapi_.clear_user_auth();
        auth_.clear_cached_tokens();
        bool saved = true;
        try {
            config_.clear_session();
        } catch (const std::exception&) {
            saved = false;
        }
        discord_.clear();
        std::lock_guard queue_lock(sync_queue_mutex_);
        if (sync_request_ != SyncRequest::local_import) sync_request_ = SyncRequest::none;
        pending_retries_.clear();
        std::lock_guard state_lock(state_mutex_);
        failed_ids_.clear();
        sync_progress_ = {};
        sync_detail_.clear();
        next_sync_ = {};
        status_ = expired
            ? "Nintendo Account session expired. Sign in again to continue."
            : "Signed out";
        diagnostic_code_ = expired ? "nintendo_session_expired" : "signed_out";
        if (!saved) {
            status_ += " The settings file could not be updated.";
            diagnostic_code_ = "settings_write_failed";
        }
    }
    update_menu();
    wake_workers();
    return true;
}

void App::sync_now(SyncRequest request, const std::string& capture, const std::string& scope) {
    std::uint64_t generation;
    std::uint64_t destination;
    std::vector<std::string> retry_ids;
    std::string run_scope;
    {
        std::lock_guard context_lock(sync_context_mutex_);
        const auto config = config_.snapshot();
        if (config.session_token.empty()) return;
        run_scope = history_scope(config);
        generation = account_generation_.load();
        destination = destination_generation_.load();
        if (request == SyncRequest::automatic_retry) {
            std::lock_guard queue_lock(sync_queue_mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [id, retry] : pending_retries_) {
                if (retry.due > now) continue;
                retry_ids.push_back(id);
                retry.due = std::chrono::steady_clock::time_point::max();
                ++retry.attempts;
            }
            if (retry_ids.empty()) return;
        }
        std::lock_guard state_lock(state_mutex_);
        if (request == SyncRequest::retry) {
            if (!capture.empty() && scope != run_scope) {
                status_ = "This capture belongs to a different album or sign-in.";
                return;
            }
            retry_ids = capture.empty() ? failed_ids_ : std::vector<std::string>{capture};
            if (retry_ids.empty()) return;
        }
        sync_progress_ = {};
        sync_detail_.clear();
        diagnostic_code_ = "checking_album";
        status_ = "Checking Nintendo’s album…";
    }
    update_menu();

    const auto cancelled = [this, generation, destination] {
        return stopping_.load() || account_generation_.load() != generation ||
            destination_generation_.load() != destination;
    };
    SyncResult result;
    try {
        result = sync_.sync(cancelled, [this, &cancelled](const SyncProgress& progress) {
            {
                std::lock_guard state_lock(state_mutex_);
                if (cancelled()) return;
                sync_progress_ = progress;
                if (progress.stage == SyncStage::downloading) {
                    status_ = "Downloading captures…";
                    diagnostic_code_ = "downloading_captures";
                    sync_detail_ = sync_counts(progress.new_downloads, progress.failed);
                }
            }
            update_menu();
        }, retry_ids);
    } catch (const std::exception& error) {
        result.failure = SyncFailure::service;
        result.fatal = true;
        result.error_detail = error.what();
        result.transient = dynamic_cast<const TransientHttpError*>(&error) != nullptr;
    }
    if (!result.error_detail.empty()) {
        std::lock_guard state_lock(state_mutex_);
        last_error_ = "Sync: " + result.error_detail.substr(0, 200);
    }
    if (cancelled()) return;
    {
        std::lock_guard context_lock(sync_context_mutex_);
        if (cancelled()) return;
        Json::array items;
        for (const auto& item : result.items) {
            items.emplace_back(Json::object{{"name", item.label}, {"outcome", item.outcome}, {"id", item.retry_id}});
        }
        auto history = sync_history_ ? *sync_history_ : Json::array{};
        history.insert(history.begin(), Json::object{
            {"time", local_time_text(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))},
            {"scope", run_scope}, {"automatic_retry", request == SyncRequest::automatic_retry}, {"downloaded", result.new_downloads}, {"skipped", result.skipped},
            {"failed", result.failed}, {"unavailable", result.unavailable}, {"error", failure_message(result.failure)},
            {"items", std::move(items)}});
        if (history.size() > 100) history.resize(100);
        if (!write_local_data(config_.directory() / "sync-history.json", Json(history))) {
            ui_.notify("Switch Album Sync", "Could not save sync history to disk.");
        }
        std::lock_guard state_lock(state_mutex_);
        sync_history_ = std::make_shared<const Json::array>(std::move(history));
    }
    if (result.failure == SyncFailure::sign_in) {
        invalidate_session(generation, true);
        return;
    }

    std::string notification;
    bool notifications = false;
    {
        std::lock_guard context_lock(sync_context_mutex_);
        if (cancelled()) return;
        notifications = config_.snapshot().notifications;
        {
            using namespace std::chrono;
            std::lock_guard queue_lock(sync_queue_mutex_);
            if (!result.fatal || request == SyncRequest::automatic_retry) {
                std::map<std::string, PendingRetry> next;
                const auto schedule = [&](const std::string& id) {
                    const auto existing = pending_retries_.find(id);
                    if (existing != pending_retries_.end() && request != SyncRequest::automatic_retry &&
                        existing->second.due != steady_clock::time_point::max()) {
                        next.emplace(id, existing->second);
                        return;
                    }
                    const int attempts = existing == pending_retries_.end() ? 0 : existing->second.attempts;
                    const auto due = attempts < 3 ? steady_clock::now() + minutes(1 << attempts) :
                        steady_clock::time_point::max();
                    next.emplace(id, PendingRetry{due, attempts});
                };
                for (const auto& item : result.items) {
                    if (item.transient && !item.retry_id.empty()) schedule(item.retry_id);
                }
                // Reschedule a retry only when the album request itself never got
                // a reply. After an error response, wait for the user instead.
                if (request == SyncRequest::automatic_retry && result.failure == SyncFailure::service &&
                    result.transient) {
                    for (const auto& id : retry_ids) schedule(id);
                }
                if (retry_ids.empty()) pending_retries_.swap(next);
                else {
                    for (const auto& id : retry_ids) pending_retries_.erase(id);
                    pending_retries_.merge(next);
                }
            }
        }
        const bool complete = !result.fatal && result.failed == 0 && result.unavailable == 0;
        const auto sync_time = complete ? local_time_text(
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) : std::string{};
        bool saved = true;
        if (complete && request != SyncRequest::automatic_retry && capture.empty()) {
            try {
                config_.update([&](AppConfig& value) { value.last_sync = sync_time; });
            } catch (const std::exception&) {
                saved = false;
            }
        }

        std::lock_guard state_lock(state_mutex_);
        if (!result.fatal) {
            if (retry_ids.empty()) {
                failed_ids_ = std::move(result.failed_ids);
            } else {
                for (const auto& id : retry_ids) std::erase(failed_ids_, id);
                failed_ids_.insert(failed_ids_.end(), result.failed_ids.begin(), result.failed_ids.end());
            }
        }
        sync_progress_.total_found = result.total_found;
        sync_progress_.new_downloads = result.new_downloads;
        sync_progress_.skipped = result.skipped;
        sync_progress_.failed = result.failed;
        sync_detail_ = result.fatal ? std::string{} :
            sync_counts(result.new_downloads, result.failed);
        if (result.unavailable > 0) {
            sync_detail_ += (sync_detail_.empty() ? "" : ", ") + std::to_string(result.unavailable) + " no longer available";
        }
        if (result.review > 0) {
            sync_detail_ += (sync_detail_.empty() ? "" : ", ") + std::to_string(result.review) + " ready in Duplicate review";
        }
        if (complete) {
            if (request != SyncRequest::automatic_retry && capture.empty()) last_sync_ = sync_time;
            status_ = request == SyncRequest::automatic_retry ? "Automatic retry complete" :
                !capture.empty() ? "Capture is available in the album" :
                saved ? "Album is up to date" : "Album synced. Could not save the sync timestamp.";
            diagnostic_code_ = saved ? "sync_complete" : "settings_write_failed";
            if (result.new_downloads > 0 || result.review > 0 || request != SyncRequest::automatic) {
                notification = status_ + (sync_detail_.empty() ? "." : ". " + sync_detail_ + ".");
            }
        } else if (result.failure != SyncFailure::none) {
            diagnostic_code_ = failure_code(result.failure);
            status_ = failure_message(result.failure);
            if (result.failure == SyncFailure::download) {
                status_ += failed_ids_.empty() ? " Try syncing again." : " Retry the failed items.";
            }
            if (result.expiring_soon > 0) {
                // Nintendo's album only keeps captures for a limited time.
                status_ += " " + std::to_string(result.expiring_soon) +
                    (result.expiring_soon == 1 ? " of them leaves" : " of them leave") +
                    " Nintendo's album within 3 days.";
            }
            notification = status_;
        } else {
            diagnostic_code_ = "captures_no_longer_available";
            status_ = "Some captures are no longer available from Nintendo.";
            notification = status_;
        }
    }

    if (result.new_downloads > 0) backup_pending_ = true;
    if (notifications && !notification.empty() && !cancelled()) {
        if (!result.downloaded_paths.empty()) {
            ui_.notify_captures("Switch Album Sync", notification + " Click to view new captures.",
                std::move(result.downloaded_paths));
        } else
        ui_.notify("Switch Album Sync", notification);
    }
    // Rescan the library only when this sync added files to it.
    if (result.new_downloads > 0 || result.review > 0) queue_recent_refresh();
}

bool App::queue_sync(SyncRequest request, const std::string& capture, const std::string& scope,
                     std::shared_ptr<const ImportPlan> import) {
    // Quieter auto-sync: skip this scheduled check entirely (it is not saved
    // up for later), so these options only ever reduce Nintendo requests.
    if (request == SyncRequest::automatic) {
        const auto reason = auto_sync_pause_reason(
            config_.snapshot(), battery_saver_on(), metered_connection(), local_hour_now());
        if (!reason.empty()) {
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Auto-sync skipped because " + reason + ".";
                diagnostic_code_ = "auto_sync_paused";
            }
            update_menu();
            return false;
        }
    }
    const bool local_only = request == SyncRequest::local_import || request == SyncRequest::backup;
    {
        std::lock_guard context_lock(sync_context_mutex_);
        std::lock_guard queue_lock(sync_queue_mutex_);
        if (stopping_ || sync_request_ != SyncRequest::none || sync_running_ ||
            (!local_only && config_.snapshot().session_token.empty())) {
            return false;
        }
        std::lock_guard state_lock(state_mutex_);
        if (request == SyncRequest::retry && capture.empty() && failed_ids_.empty()) return false;
        if (request == SyncRequest::retry && !capture.empty() &&
            scope != history_scope(config_.snapshot())) return false;
        requested_capture_ = capture;
        requested_scope_ = scope;
        requested_import_ = std::move(import);
        sync_request_ = request;
        status_ = request == SyncRequest::local_import ? "Import queued…" :
            request == SyncRequest::backup ? "Backup queued…" : "Sync queued…";
        sync_progress_ = {};
        sync_detail_.clear();
        diagnostic_code_ = "sync_queued";
    }
    update_menu();
    sync_queue_cv_.notify_one();
    return true;
}

void App::queue_recent_refresh() {
    {
        std::lock_guard queue_lock(sync_queue_mutex_);
        if (stopping_) return;
        std::lock_guard state_lock(state_mutex_);
        recent_refresh_requested_ = true;
        recent_loading_ = true;
    }
    update_menu();
    sync_queue_cv_.notify_one();
}

void App::refresh_recent_captures() {
    std::uint64_t generation;
    std::filesystem::path destination;
    {
        std::lock_guard context_lock(sync_context_mutex_);
        generation = destination_generation_.load();
        destination = path_from_utf8(config_.snapshot().destination_folder);
    }
    const auto cancelled = [this, generation] {
        return stopping_.load() || destination_generation_.load() != generation;
    };
    std::vector<LocalCapture> library;
    try {
        library = scan_capture_library(destination, cancelled);
    } catch (const std::exception&) {
        // Album sync and its result remain usable if local browsing fails.
    }
    {
        std::lock_guard state_lock(state_mutex_);
        if (cancelled()) return;
        recent_captures_.clear();
        for (std::size_t i = 0; i < std::min<std::size_t>(8, library.size()); ++i) {
            recent_captures_.push_back(library[i].path);
        }
        capture_library_ = std::make_shared<const std::vector<LocalCapture>>(std::move(library));
        recent_loading_ = false;
    }
    update_menu();
}

void App::request_presence_refresh() {
    std::lock_guard lock(presence_sleep_mutex_);
    if (stopping_) return;
    presence_refresh_requested_.store(true, std::memory_order_release);
    presence_cv_.notify_one();
}

void App::sync_loop() {
    using Clock = std::chrono::steady_clock;
    auto next_watch = Clock::now();
    WatchedFolder watcher;
    for (;;) {
        SyncRequest request;
        std::string capture;
        std::string scope;
        std::shared_ptr<const ImportPlan> import;
        bool watch_due = false;
        {
            std::unique_lock lock(sync_queue_mutex_);
            while (!stopping_ && sync_request_ == SyncRequest::none && !recent_refresh_requested_) {
                const auto retry = next_retry_locked();
                const auto now = Clock::now();
                if (retry <= now) break;
                const bool watching = !config_.snapshot().watch_folder.empty();
                watch_due = watching && next_watch <= now;
                if (watch_due) break;
                const auto next = watching ? std::min(retry, next_watch) : retry;
                if (next == Clock::time_point::max()) sync_queue_cv_.wait(lock);
                else sync_queue_cv_.wait_until(lock, next);
            }
            if (stopping_) break;
            request = sync_request_;
            if (request == SyncRequest::none && next_retry_locked() <= std::chrono::steady_clock::now()) {
                request = SyncRequest::automatic_retry;
            }
            if (request == SyncRequest::none) {
                recent_refresh_requested_ = false;
            } else {
                sync_running_ = true;
                capture.swap(requested_capture_);
                scope.swap(requested_scope_);
                import.swap(requested_import_);
                sync_request_ = SyncRequest::none;
            }
        }
        if (watch_due && request == SyncRequest::none) {
            next_watch = Clock::now() + std::chrono::seconds(5);
            const auto config = config_.snapshot();
            if (config.watch_folder.empty()) continue;
            try {
                auto plan = watcher.poll(path_from_utf8(config.watch_folder),
                    album_directory(path_from_utf8(config.destination_folder)),
                    config_.directory() / "watch-receipts.json", [this] { return stopping_.load(); });
                if (plan.captures.empty()) continue;
                {
                    std::lock_guard lock(sync_queue_mutex_);
                    const auto current = config_.snapshot();
                    if (stopping_ || sync_request_ != SyncRequest::none ||
                        current.watch_folder != config.watch_folder ||
                        current.destination_folder != config.destination_folder) continue;
                    sync_running_ = true;
                }
                const auto completed = import_captures(plan);
                watcher.complete(completed);
                if (completed.size() != plan.captures.size()) {
                    next_watch = Clock::now() + std::chrono::seconds(30);
                }
                if (backup_pending_) run_backup(false);
            } catch (const std::exception&) {
                next_watch = Clock::now() + std::chrono::seconds(30);
            }
            {
                std::lock_guard lock(sync_queue_mutex_);
                sync_running_ = false;
            }
            update_menu();
            continue;
        }
        if (request == SyncRequest::none) {
            refresh_recent_captures();
            continue;
        }
        if (request == SyncRequest::local_import && import) import_captures(*import);
        else if (request == SyncRequest::backup) run_backup(true);
        else
            sync_now(request, capture, scope);
        if (backup_pending_) run_backup(false);
        {
            std::lock_guard lock(sync_queue_mutex_);
            sync_running_ = false;
        }
        update_menu();
    }
}

std::vector<std::filesystem::path> App::import_captures(const ImportPlan& plan) {
    const auto generation = destination_generation_.load();
    const bool watched = !plan.watch_source.empty();
    {
        std::lock_guard lock(state_mutex_);
        status_ = watched ? "Importing captures from the watched folder…" : "Importing captures from USB or microSD…";
        diagnostic_code_ = "importing_captures";
    }
    update_menu();
    const auto cancelled = [this, generation, &plan, watched] {
        return stopping_.load() || generation != destination_generation_.load() ||
            (watched && path_from_utf8(config_.snapshot().watch_folder) != plan.watch_source);
    };
    std::string status;
    std::string detail;
    std::vector<std::filesystem::path> copied_paths;
    std::vector<std::filesystem::path> completed_sources;
    try {
        const auto destination = album_directory(path_from_utf8(config_.snapshot().destination_folder));
        auto result = import_windows_captures(plan, destination, cancelled);
        status = result.failed ? "Import finished with errors" :
            result.copied || result.skipped || result.review ? "Import complete" : "No captures found in the selected folder";
        detail = std::to_string(result.copied) + " imported, " + std::to_string(result.skipped) +
            " skipped, " + std::to_string(result.failed) + " failed";
        if (result.review) detail += ", " + std::to_string(result.review) + " ready in Duplicate review";
        if (result.copied > 0) backup_pending_ = true;
        copied_paths = std::move(result.copied_paths);
        completed_sources = std::move(result.completed_sources);
    } catch (const std::exception&) {
        status = "Could not import captures. Check the source and destination folders.";
    }
    {
        std::lock_guard lock(state_mutex_);
        if (cancelled()) return completed_sources;
        status_ = std::move(status);
        sync_detail_ = std::move(detail);
        diagnostic_code_ = "import_finished";
    }
    if (!copied_paths.empty() && config_.snapshot().notifications && !cancelled()) {
        ui_.notify_captures("Switch Album Sync", "Import complete. Click to view imported captures.", std::move(copied_paths));
    }
    queue_recent_refresh();
    return completed_sources;
}

void App::run_backup(bool announce) {
    backup_pending_ = false;
    const auto config = config_.snapshot();
    if (config.backup_folder.empty()) return;
    const auto generation = destination_generation_.load();
    const auto album = album_directory(path_from_utf8(config.destination_folder));
    if (announce) {
        {
            std::lock_guard lock(state_mutex_);
            status_ = "Backing up captures…";
            sync_detail_.clear();
            diagnostic_code_ = "backing_up";
        }
        update_menu();
    }
    BackupResult result;
    bool failed = false;
    try {
        result = backup_album(album, path_from_utf8(config.backup_folder), [this, generation] {
            return stopping_.load() || destination_generation_.load() != generation;
        });
    } catch (const std::exception&) {
        failed = true;
    }
    std::string message;
    {
        std::lock_guard lock(state_mutex_);
        if (failed) {
            message = "Could not back up. Check that the backup folder is available.";
        } else if (result.failed > 0) {
            message = "Backup finished, but " + std::to_string(result.failed) + " captures couldn't be copied.";
        } else if (announce) {
            message = result.copied > 0
                ? "Backed up " + std::to_string(result.copied) + " new or changed captures."
                : "Backup is up to date.";
        }
        if (announce || failed || result.failed > 0) {
            status_ = message;
            diagnostic_code_ = failed || result.failed ? "backup_failed" : "backup_complete";
        } else if (result.copied > 0) {
            // After a sync or import, keep its result and note the backup.
            if (!sync_detail_.empty()) sync_detail_ += ", ";
            sync_detail_ += std::to_string(result.copied) + " backed up";
        }
    }
    update_menu();
    if (announce && !message.empty() && config.notifications) ui_.notify("Switch Album Sync", message);
}

void App::complete_pending_login(const std::string& redirect_url_or_code) {
    std::lock_guard auth_lock(auth_flow_mutex_);
    if (!auth_pending_) return;

    try {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Completing Nintendo Account sign-in…";
            diagnostic_code_ = "completing_sign_in";
        }
        update_menu();

        const auto auth_result = auth_.complete_login(redirect_url_or_code);
        AppConfig config;
        bool saved = true;
        {
            std::lock_guard context_lock(sync_context_mutex_);
            if (stopping_) return;
            account_generation_.fetch_add(1);
            clear_pending_retries();
            coral_.clear_cached_session();
            splatnet_.clear_cache();
            zeldanotes_.clear_cache();
            game_services_.clear_cache();
            nxapi_.clear_user_auth();
            try {
                config = config_.update([&](AppConfig& value) {
                    value.session_token = auth_result.session_token;
                    value.user_nickname = auth_result.user_nickname;
                });
            } catch (const std::exception&) {
                saved = false;
                config = config_.snapshot();
            }
            auth_pending_ = false;
            manual_sign_in_ = false;
            std::lock_guard state_lock(state_mutex_);
            failed_ids_.clear();
            sync_progress_ = {};
            sync_detail_.clear();
            status_ = saved ? "Nintendo Account connected"
                : "Nintendo Account connected for this session. Could not save settings.";
            diagnostic_code_ = saved ? "sign_in_complete" : "settings_write_failed";
        }
        unregister_nintendo_auth_protocol();
        clear_nintendo_auth_callback();
        update_menu();
        const bool use_presence = presence_enabled(config);
        initial_sync_deferred_.store(use_presence && config.auto_sync, std::memory_order_release);
        start_operational_workers();
        wake_workers();
        if (use_presence) {
            request_presence_refresh();
        } else if (config.auto_sync) {
            queue_sync(SyncRequest::automatic);
        }
    } catch (const std::exception& error) {
        const std::string_view detail = error.what();
        const bool wrong_state = detail == "Nintendo sign-in callback had an invalid OAuth state.";
        const bool cancelled = detail == "Nintendo Account sign-in was cancelled.";
        if (!wrong_state) {
            // A session-token code is one-time material. A failed login needs
            // a fresh PKCE verifier and code on the next attempt.
            auth_pending_ = false;
            manual_sign_in_ = false;
            unregister_nintendo_auth_protocol();
            clear_nintendo_auth_callback();
        }
        const std::string message = wrong_state
            ? (manual_sign_in_ ? "Copy the link from the Nintendo tab Switch Album Sync opened, then paste it again."
                : "Ignored a sign-in response that did not match this request. Finish signing in in your browser.")
            : cancelled ? "Nintendo Account sign-in was cancelled."
            : "Nintendo Account sign-in failed. Open the sign-in page and try again.";
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = message;
            diagnostic_code_ = wrong_state ? "sign_in_state_mismatch"
                : cancelled ? "sign_in_cancelled" : "sign_in_failed";
        }
        update_menu();
        if (config_.snapshot().notifications) {
            ui_.notify("Nintendo Sign-In", message);
        }
    }
}

void App::sign_in_or_out() {
    const auto generation = account_generation_.load();
    const auto current = config_.snapshot();

    if (!current.session_token.empty()) {
        if (invalidate_session(generation, false) && current.notifications) {
            ui_.notify("Switch Album Sync", "Signed out of your Nintendo Account.");
        }
        return;
    }

    if (auth_pending_) {
        ui_.notify(
            "Nintendo Sign-In",
            "A Nintendo Account sign-in is already waiting in your browser.");
        return;
    }

    if (!ui_.confirm(kNxapiDisclosureTitle,
            std::string(kNxapiDisclosureBody) + kNxapiSignInQuestion)) {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Sign-in cancelled";
            diagnostic_code_ = "sign_in_cancelled";
        }
        update_menu();
        return;
    }
    // Record the explicit acknowledgement the nxapi terms require. If the
    // settings file can't be written, the user is simply asked again next launch.
    try {
        config_.update([](AppConfig& value) {
            value.nxapi_disclosure_acknowledged = kNxapiDisclosureVersion;
        });
    } catch (const std::exception&) {
    }

    try {
        clear_nintendo_auth_callback();
        const bool automatic_return = register_nintendo_auth_protocol();
        const auto authorize_url = auth_.authorize_url();
        manual_sign_in_ = !automatic_return;
        auth_pending_ = true;
        {
            // Taking the lock orders this with the callback loop's check, so
            // the wake-up can't slip in between its check and its wait.
            std::lock_guard sleep_lock(sleep_mutex_);
        }
        sleep_cv_.notify_all();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = automatic_return ? "Waiting for Nintendo Account sign-in in your browser…" :
                "Finish signing in on Nintendo’s website, then use Paste sign-in link in Switch Album Sync.";
            diagnostic_code_ = automatic_return ? "waiting_for_browser" : "waiting_for_pasted_link";
        }
        update_menu();
        open_url(authorize_url);
    } catch (const std::exception&) {
        auth_pending_ = false;
        manual_sign_in_ = false;
        unregister_nintendo_auth_protocol();
        clear_nintendo_auth_callback();
        const std::string message = "Could not start Nintendo sign-in. Try again.";
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = message;
            diagnostic_code_ = "sign_in_start_failed";
        }
        update_menu();
        ui_.notify("Nintendo Sign-In", message);
    }
}

void App::paste_sign_in_link() {
    if (!auth_pending_ || !manual_sign_in_) return;
    auto link = trim(ui_.prompt("Paste Nintendo sign-in link", kManualSignInInstructions));
    if (link.empty()) return;
    if (link.size() > 16 * 1024 || !is_nintendo_auth_callback(link)) {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "That link did not come from the Nintendo account selection button. Copy it and try again.";
            diagnostic_code_ = "invalid_pasted_link";
        }
        update_menu();
        return;
    }
    {
        std::unique_lock auth_lock(auth_flow_mutex_, std::try_to_lock);
        if (!auth_lock.owns_lock() || !auth_pending_ || !manual_sign_in_) return;
        pasted_auth_link_ = std::move(link);
    }
    sleep_cv_.notify_all();
}

void App::presence_loop() {
    RpcEnrichmentCache enrichment;
    const auto reset_enrichment = [&] { enrichment = {}; };

    const auto release_deferred_sync = [&](const std::string& session_token) {
        const auto state = config_.snapshot();
        if (state.session_token != session_token) return;
        const bool was_deferred = initial_sync_deferred_.exchange(
            false, std::memory_order_acq_rel);
        if (was_deferred && state.auto_sync) {
            queue_sync(SyncRequest::automatic);
        }
    };

    const auto presence_paused = [this] {
        if (stopping_) return true;
        if (presence_refresh_requested_.load(std::memory_order_acquire)) return true;
        const auto state = config_.snapshot();
        return !state.discord_presence || state.session_token.empty();
    };

    // Consecutive polls that found you offline (or failed). After a few, the
    // poll interval stretches so an idle PC sends far fewer Nintendo requests.
    int offline_polls = 0;
    // When checks started finding you offline (or failing); unset while playing.
    std::chrono::steady_clock::time_point offline_since{};
    while (!stopping_) {
        AppConfig config;
        std::uint64_t generation;
        {
            std::lock_guard context_lock(sync_context_mutex_);
            config = config_.snapshot();
            generation = account_generation_.load();
        }
        if (!config.discord_presence || config.session_token.empty()) {
            offline_polls = 0;
            reset_enrichment();
            discord_.clear();
            std::unique_lock wait_lock(presence_sleep_mutex_);
            presence_cv_.wait(wait_lock, [this] {
                if (stopping_) return true;
                const auto state = config_.snapshot();
                return state.discord_presence && !state.session_token.empty();
            });
            continue;
        }

        // Only a running Discord client can show Rich Presence. Without one,
        // check again locally rather than asking Nintendo for presence.
        if (!discord_.client_running()) {
            offline_polls = 0;
            offline_since = {};
            reset_enrichment();
            discord_.clear();
            release_deferred_sync(config.session_token);
            presence_refresh_requested_.store(false, std::memory_order_release);
            std::unique_lock wait_lock(presence_sleep_mutex_);
            presence_cv_.wait_for(wait_lock, kDiscordClientCheckInterval, presence_paused);
            continue;
        }

        // Reaching this point means one of three things made a presence poll
        // eligible: Rich Presence became active, the recurring timer elapsed,
        // or an explicit refresh was queued by Sync Now. Consume any queued
        // refresh before polling; a new request arriving during the network
        // call remains set and causes another immediate pass afterwards.
        if (presence_refresh_requested_.exchange(false, std::memory_order_acq_rel)) {
            offline_polls = 0;
            offline_since = {};
        }

        // Offline for hours and nobody at the PC: stop asking Nintendo until
        // someone uses the PC again (checked locally once a minute).
        if (offline_since != std::chrono::steady_clock::time_point{} &&
            presence_idle_paused(std::chrono::steady_clock::now() - offline_since, user_idle_time())) {
            std::unique_lock wait_lock(presence_sleep_mutex_);
            presence_cv_.wait_for(wait_lock, std::chrono::minutes(1), presence_paused);
            continue;
        }

        const auto session_token = config.session_token;
        bool playing = false;
        bool failed = false;
        try {
            auto presence = coral_.self_presence(session_token);
            config = config_.snapshot();
            if (!stopping_ && config.discord_presence &&
                config.session_token == session_token &&
                account_generation_.load() == generation) {
                if (presence.is_playing()) {
                    playing = true;
                    const auto revision = rpc_revision_.load();
                    presence.rpc = config.rpc;
                    const auto service = rpc_game_service_for(presence);
                    auto game_key = !presence.title_id.empty()
                        ? presence.title_id : presence.game_name;
                    if (service == RpcGameService::ZeldaNotes) {
                        const auto zelda_game = zelda_notes_game_for_presence(
                            presence.title_id, presence.game_name);
                        if (zelda_game != ZeldaNotesGame::Unknown) {
                            game_key = std::string("zelda:") +
                                zelda_notes_short_name(zelda_game);
                        }
                    }
                    const bool changed = enrichment.begin(game_key, generation, revision);
                    const bool enabled = rpc_service_enabled(service, config.rpc);
                    if (changed) zeldanotes_.clear_cache();
                    const bool probing = enabled && enrichment.should_probe(RpcEnrichmentCache::Clock::now());

                    presence.zelda_notes_enabled = enabled && service == RpcGameService::ZeldaNotes &&
                        (enrichment.success || !probing);
                    enrichment.apply(presence);

                    if (!probing) {
                        release_deferred_sync(session_token);
                    }

                    if (probing) {
                        const bool service_ready = rpc_enricher_.enrich(
                            service, session_token,
                            [&] {
                                // Signing out or switching accounts can happen
                                // while a Nintendo request is in flight.
                                return !stopping_ && account_generation_.load() == generation &&
                                    config_.snapshot().session_token == session_token;
                            },
                            presence);

                        enrichment.complete(presence, service_ready, RpcEnrichmentCache::Clock::now());
                        presence.zelda_notes_enabled = enabled &&
                            service == RpcGameService::ZeldaNotes && enrichment.success;
                        release_deferred_sync(session_token);
                    }

                    // A sign-out clears Discord immediately. Do not allow a slow
                    // request from the old account to republish stale activity.
                    const auto latest = config_.snapshot();
                    if (!stopping_ && latest.discord_presence &&
                        latest.session_token == session_token &&
                        rpc_revision_.load() == revision &&
                        account_generation_.load() == generation) {
                        discord_.update(presence);
                    }
                } else {
                    reset_enrichment();
                    discord_.clear();
                    release_deferred_sync(session_token);
                }
            }
        } catch (const NintendoSessionExpired&) {
            invalidate_session(generation, true);
            continue;
        } catch (const std::exception& error) {
            failed = true;
            {
                std::lock_guard state_lock(state_mutex_);
                last_error_ = std::string("Rich Presence: ") + std::string(error.what()).substr(0, 200);
            }
            // Presence failure must not strand auto-sync behind RPC priority.
            release_deferred_sync(session_token);
            if (!stopping_) {
                std::cerr << "Presence: Nintendo service request failed\n";
            }
        }

        // Re-randomize every cycle so timer-driven Nintendo requests do not
        // land on an exact cadence. Explicit user-triggered refreshes still
        // bypass this wait and run immediately.
        if (playing) {
            offline_polls = 0;
            offline_since = {};
        } else {
            if (offline_since == std::chrono::steady_clock::time_point{}) {
                offline_since = std::chrono::steady_clock::now();
            }
            offline_polls = std::min(offline_polls + 1, kPresenceOfflineChecksBeforeLongBackoff);
            // After a failed check, go straight to the slower pace instead of
            // asking again a minute later.
            if (failed) offline_polls = std::max(offline_polls, kPresenceOfflineChecksBeforeBackoff);
        }
        const auto poll_interval = jittered_interval(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                presence_poll_interval(playing, offline_polls)));
        std::unique_lock wait_lock(presence_sleep_mutex_);
        presence_cv_.wait_for(wait_lock, poll_interval, presence_paused);
    }
}

void App::auto_sync_loop() {
    std::unique_lock sleep_lock(sleep_mutex_);
    while (!stopping_) {
        const auto config = config_.snapshot();
        const auto revision = auto_sync_revision_;
        const auto rescheduled = [this, revision] {
            return stopping_.load() || auto_sync_revision_ != revision;
        };
        if (!config.auto_sync || config.session_token.empty()) {
            sleep_cv_.wait(sleep_lock, rescheduled);
            continue;
        }

        using namespace std::chrono;
        const auto interval = jittered_interval(duration_cast<milliseconds>(
            minutes(std::max(1, config.sync_interval_minutes))));
        // Millisecond precision can represent the full range of saved intervals.
        const auto deadline = time_point_cast<milliseconds>(steady_clock::now()) + interval;
        {
            std::lock_guard state_lock(state_mutex_);
            next_sync_ = time_point_cast<seconds>(system_clock::now()) +
                duration_cast<seconds>(interval);
        }
        sleep_lock.unlock();
        update_menu();
        sleep_lock.lock();

        auto remaining = deadline - time_point_cast<milliseconds>(steady_clock::now());
        while (remaining > milliseconds::zero() && !rescheduled()) {
            sleep_cv_.wait_for(sleep_lock,
                std::min(remaining, duration_cast<milliseconds>(hours(24))), rescheduled);
            remaining = deadline - time_point_cast<milliseconds>(steady_clock::now());
        }
        if (rescheduled()) continue;
        sleep_lock.unlock();
        if (config_.snapshot().auto_sync) queue_sync(SyncRequest::automatic);
        sleep_lock.lock();
    }
}

void App::auth_callback_loop() {
    while (!stopping_) {
        if (manual_sign_in_) {
            std::string link;
            {
                std::lock_guard auth_lock(auth_flow_mutex_);
                link.swap(pasted_auth_link_);
            }
            if (!link.empty()) complete_pending_login(link);
        } else if (const auto callback = take_nintendo_auth_callback()) {
            if (auth_pending_ && !manual_sign_in_) complete_pending_login(*callback);
        }
        std::unique_lock sleep_lock(sleep_mutex_);
        if (auth_pending_ || manual_sign_in_) {
            sleep_cv_.wait_for(sleep_lock, kAuthCallbackPollInterval);
        } else {
            // Nothing to look for until a sign-in starts, so sleep instead of
            // waking every 150 ms for the life of the app.
            sleep_cv_.wait(sleep_lock, [this] {
                return stopping_ || auth_pending_ || manual_sign_in_;
            });
        }
    }
}

void App::start_operational_workers() {
    bool expected = false;
    if (!operational_workers_started_.compare_exchange_strong(expected, true)) return;
    if constexpr (discord_available) presence_thread_ = std::thread([this] { presence_loop(); });
    auto_sync_thread_ = std::thread([this] { auto_sync_loop(); });
}

void App::start_workers() {
    sync_thread_ = std::thread([this] { sync_loop(); });
    auth_callback_thread_ = std::thread([this] { auth_callback_loop(); });
    if (!config_.snapshot().session_token.empty()) start_operational_workers();
}

void App::request_stop() {
    {
        std::lock_guard queue_lock(sync_queue_mutex_);
        if (stopping_.exchange(true)) return;
    }
    auth_pending_ = false;
    manual_sign_in_ = false;
    initial_sync_deferred_.store(false, std::memory_order_release);
    unregister_nintendo_auth_protocol();
    clear_nintendo_auth_callback();
    wake_workers();
}

void App::request_exit() {
    bool expected = false;
    if (!explicit_exit_.compare_exchange_strong(expected, true)) return;

    // Start the bound before doing any cleanup: even local Discord IPC or OS
    // unregister calls can block unexpectedly. This guarantees the explicit
    // Exit command cannot leave a hidden process behind.
    std::thread([] {
        std::this_thread::sleep_for(kExitWatchdogDelay);
        std::_Exit(0);
    }).detach();

    request_stop();
    ui_.stop();
    discord_.clear();
}

void App::stop_workers() {
    request_stop();
    if (workers_joined_.exchange(true)) return;

    // A successful login can start the operational workers from the auth thread.
    if (auth_callback_thread_.joinable()) auth_callback_thread_.join();
    if (sync_thread_.joinable()) sync_thread_.join();
    if (auto_sync_thread_.joinable()) auto_sync_thread_.join();
    if (presence_thread_.joinable()) presence_thread_.join();
    discord_.clear();
}

int App::run() {
    update_menu();
    PlatformCallbacks callbacks;

    callbacks.ready = [this] {
        // A sign-in carried over from an older build (or from NSO Album Sync)
        // may never have seen the current disclosure. Nothing has contacted
        // nxapi yet: ask first, and sign out if the user declines.
        if (needs_nxapi_disclosure(config_.snapshot())) {
            if (ui_.confirm(kNxapiDisclosureTitle,
                    std::string(kNxapiDisclosureBody) + kNxapiSignedInQuestion)) {
                try {
                    config_.update([](AppConfig& value) {
                        value.nxapi_disclosure_acknowledged = kNxapiDisclosureVersion;
                    });
                } catch (const std::exception&) {
                    // Acknowledged for this session; asked again next launch.
                }
            } else {
                invalidate_session(account_generation_.load(), false);
            }
        }
        // Rich Presence turned on by an older build: explain what reaches
        // Discord once, and turn it off if the user says no.
        if constexpr (discord_available) {
            const auto current = config_.snapshot();
            if (current.discord_presence &&
                current.discord_disclosure_acknowledged < kDiscordDisclosureVersion) {
                const bool keep = ui_.confirm(kDiscordDisclosureEnabledTitle,
                    std::string(kDiscordDisclosureBody) + kDiscordKeepOnQuestion);
                try {
                    config_.update([keep](AppConfig& value) {
                        if (keep) value.discord_disclosure_acknowledged = kDiscordDisclosureVersion;
                        else value.discord_presence = false;
                    });
                } catch (const std::exception&) {
                    // Asked again next launch.
                }
            }
        }

        const auto config = config_.snapshot();
        const bool use_presence = presence_enabled(config);
        initial_sync_deferred_.store(
            use_presence && config.auto_sync && !config.session_token.empty(),
            std::memory_order_release);
        start_workers();
        queue_recent_refresh();
        if (use_presence && !config.session_token.empty()) {
            request_presence_refresh();
        } else if (config.auto_sync && !config.session_token.empty()) {
            queue_sync(SyncRequest::automatic);
        }
    };

    callbacks.sync_now = [this] {
        initial_sync_deferred_.store(false, std::memory_order_release);
        if (!queue_sync(SyncRequest::manual)) return;
        if constexpr (discord_available) {
            const auto config = config_.snapshot();
            if (config.discord_presence && !config.session_token.empty()) request_presence_refresh();
        }
    };
    callbacks.retry_failed = [this] { queue_sync(SyncRequest::retry); };
    callbacks.retry_capture = [this](const std::string& id, const std::string& scope) {
        if (!id.empty()) queue_sync(SyncRequest::retry, id, scope);
    };
    callbacks.refresh_captures = [this] { queue_recent_refresh(); };
    callbacks.import_captures = [this] {
        auto plan = ui_.choose_import_plan(album_directory(path_from_utf8(config_.snapshot().destination_folder)));
        if (plan && !queue_sync(SyncRequest::local_import, {}, {}, std::move(plan))) {
            ui_.notify("Switch Album Sync", "Wait for the current sync or import to finish, then try again.");
        }
    };

    const auto report_settings_failure = [this] {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Could not save settings. Check access to the app’s settings folder.";
            diagnostic_code_ = "settings_write_failed";
        }
        update_menu();
    };
    callbacks.set_appearance = [this, report_settings_failure](Appearance appearance) {
        try {
            config_.update([appearance](AppConfig& value) { value.appearance = appearance; });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        update_menu();
    };
    callbacks.clear_legacy_credential = [this, report_settings_failure] {
        if (!ui_.confirm("Clear saved credential",
                "Remove the older credential from the settings file? Your current session can continue until you quit.")) {
            return;
        }
        try {
            std::lock_guard context_lock(sync_context_mutex_);
            config_.clear_legacy_credential();
            std::lock_guard state_lock(state_mutex_);
            status_ = "Older credential cleared from settings";
            diagnostic_code_ = "legacy_credential_cleared";
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        update_menu();
    };

    callbacks.toggle_auto = [this, report_settings_failure] {
        AppConfig config;
        try {
            config = config_.update([](AppConfig& value) {
                value.auto_sync = !value.auto_sync;
                value.auto_sync_setting_version = 1;
            });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        initial_sync_deferred_.store(false, std::memory_order_release);
        {
            std::lock_guard state_lock(state_mutex_);
            const bool signed_in = !config.session_token.empty();
            status_ = !config.auto_sync ? "Auto-sync disabled" :
                signed_in ? "Auto-sync enabled" : "Auto-sync is on. It starts after you sign in.";
            next_sync_ = {};
        }
        update_menu();
        reschedule_auto_sync();
        if (config.auto_sync && !config.session_token.empty()) {
            queue_sync(SyncRequest::manual);
        }
        if (config.notifications) {
            ui_.notify(
                "Switch Album Sync",
                !config.auto_sync ? "Auto-sync disabled." :
                config.session_token.empty() ? "Auto-sync is on. It starts after you sign in." :
                    "Auto-sync enabled. Interval: " + std::to_string(config.sync_interval_minutes) + " min.");
        }
    };

    callbacks.toggle_notifications = [this, report_settings_failure] {
        bool enabled = false;
        try {
            enabled = config_.update([](AppConfig& value) {
                value.notifications = !value.notifications;
            }).notifications;
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        update_menu();
        if (enabled) ui_.notify("Switch Album Sync", "Notifications are enabled.");
    };

    callbacks.toggle_discord = [this, report_settings_failure] {
        if constexpr (!discord_available) return;
        // Rich Presence sends Nintendo account and game data to Discord, so
        // explain what is shared and get an explicit yes the first time.
        const auto current = config_.snapshot();
        const bool acknowledge = !current.discord_presence &&
            current.discord_disclosure_acknowledged < kDiscordDisclosureVersion;
        if (acknowledge && !ui_.confirm(kDiscordDisclosureTitle,
                std::string(kDiscordDisclosureBody) + kDiscordTurnOnQuestion)) {
            update_menu();  // Undo the checkbox the user just clicked.
            return;
        }
        rpc_revision_.fetch_add(1);
        AppConfig config;
        try {
            config = config_.update([acknowledge](AppConfig& value) {
                value.discord_presence = !value.discord_presence;
                value.discord_presence_setting_version = 1;
                if (acknowledge) value.discord_disclosure_acknowledged = kDiscordDisclosureVersion;
            });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        if (config.discord_presence && !config.session_token.empty()) {
            request_presence_refresh();
        } else {
            const bool was_deferred = initial_sync_deferred_.exchange(
                false, std::memory_order_acq_rel);
            if (was_deferred && config.auto_sync && !config.session_token.empty()) {
                queue_sync(SyncRequest::automatic);
            }
            discord_.clear();
            std::lock_guard lock(presence_sleep_mutex_);
            presence_refresh_requested_.store(false, std::memory_order_release);
            presence_cv_.notify_all();
        }
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.discord_presence
                ? "Discord presence enabled — visibility is controlled by Discord Activity Sharing"
                : "Discord presence disabled";
        }
        update_menu();
        if (config.discord_presence && config.notifications) {
            ui_.notify(
                "Discord Rich Presence",
                "Presence is enabled. In Discord, Activity Sharing must also "
                "be enabled for friends or server members to see it.");
        }
    };

    const auto toggle_rpc = [this, report_settings_failure](bool RpcSettings::*setting) {
        AppConfig config;
        try {
            config = config_.update([setting](AppConfig& value) {
                if (setting == &RpcSettings::show_username && !value.rpc.show_profile_picture) return;
                value.rpc.*setting = !(value.rpc.*setting);
            });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        discord_.set_rpc_settings(config.rpc);
        update_menu();
        if (setting == &RpcSettings::zelda || setting == &RpcSettings::animal_crossing ||
            setting == &RpcSettings::splatoon3 || setting == &RpcSettings::splatoon2) {
            rpc_revision_.fetch_add(1);
            request_presence_refresh();
        }
    };
    callbacks.toggle_rpc_zelda = [toggle_rpc] { toggle_rpc(&RpcSettings::zelda); };
    callbacks.toggle_rpc_animal_crossing = [toggle_rpc] { toggle_rpc(&RpcSettings::animal_crossing); };
    callbacks.toggle_rpc_splatoon3 = [toggle_rpc] { toggle_rpc(&RpcSettings::splatoon3); };
    callbacks.toggle_rpc_splatoon2 = [toggle_rpc] { toggle_rpc(&RpcSettings::splatoon2); };
    callbacks.toggle_rpc_username = [toggle_rpc] { toggle_rpc(&RpcSettings::show_username); };
    callbacks.toggle_rpc_profile_picture = [toggle_rpc] { toggle_rpc(&RpcSettings::show_profile_picture); };
    callbacks.toggle_rpc_play_time = [toggle_rpc] { toggle_rpc(&RpcSettings::show_play_time); };
    callbacks.refresh_rpc = [this] {
        rpc_revision_.fetch_add(1);
        request_presence_refresh();
    };

    const auto set_watch_folder = [this, report_settings_failure](const std::string& folder) {
        try {
            config_.update([&](AppConfig& config) { config.watch_folder = folder; });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        {
            std::lock_guard lock(state_mutex_);
            status_ = folder.empty() ? "Stopped watching the folder" : "Watching " + folder;
            sync_detail_.clear();
        }
        {
            std::lock_guard lock(sync_queue_mutex_);
            sync_queue_cv_.notify_one();
        }
        update_menu();
    };
    callbacks.choose_watch_folder = [this, set_watch_folder] {
        const auto config = config_.snapshot();
        const auto selected = ui_.choose_folder(config.watch_folder, L"Choose a capture folder to watch");
        if (selected.empty() || selected == config.watch_folder) return;
        const auto destination = album_directory(path_from_utf8(config.destination_folder));
        if (!valid_watch_folder(path_from_utf8(selected), destination)) {
            ui_.notify("Switch Album Sync", "Choose a watched folder separate from the album and its parent folders.");
            return;
        }
        if (ui_.confirm("Watch a capture folder",
                "Import existing and new captures from this folder while Switch Album Sync is running? "
                "Original files stay in the watched folder.")) {
            set_watch_folder(selected);
        }
    };
    callbacks.stop_watching_folder = [set_watch_folder] { set_watch_folder({}); };

    const auto report_status = [this](const std::string& message, const char* code) {
        {
            std::lock_guard lock(state_mutex_);
            status_ = message;
            sync_detail_.clear();
            diagnostic_code_ = code;
        }
        update_menu();
    };
    callbacks.choose_backup_folder = [this, report_settings_failure, report_status] {
        const auto config = config_.snapshot();
        const auto selected = ui_.choose_folder(config.backup_folder, L"Choose a backup folder");
        if (selected.empty()) return;
        const auto album = album_directory(path_from_utf8(config.destination_folder));
        if (!valid_backup_folder(path_from_utf8(selected), album)) {
            ui_.notify("Switch Album Sync", "Choose a backup folder separate from the album and its parent folders.");
            return;
        }
        if (!ui_.confirm("Back up your album",
                "Copy your captures to " + selected + "? New and changed captures are copied after each sync "
                "or import. Nothing in the backup folder is ever deleted.")) return;
        try {
            config_.update([&](AppConfig& value) { value.backup_folder = selected; });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        if (!queue_sync(SyncRequest::backup)) report_status("Backup folder set. It will update after the current task.", "backup_folder_set");
    };
    callbacks.backup_now = [this, report_status] {
        if (config_.snapshot().backup_folder.empty()) return;
        if (!queue_sync(SyncRequest::backup)) {
            ui_.notify("Switch Album Sync", "Wait for the current sync or import to finish, then try again.");
        }
    };
    callbacks.stop_backup = [this, report_settings_failure, report_status] {
        try {
            config_.update([](AppConfig& value) { value.backup_folder.clear(); });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        report_status("Stopped backing up. Existing backup copies were left in place.", "backup_stopped");
    };
    callbacks.auto_sync_options = [this, report_settings_failure, report_status] {
        const auto config = config_.snapshot();
        const auto hours = [](int hour) {
            char text[8];
            std::snprintf(text, sizeof(text), "%02d:00", hour);
            return std::string(text);
        };
        std::vector<int> selected;
        if (config.pause_on_battery_saver) selected.push_back(0);
        if (config.pause_on_metered) selected.push_back(1);
        if (config.quiet_hours) selected.push_back(2);
        const auto chosen = ui_.choose_options("Auto-sync options",
            "Choose when automatic syncs should be skipped. A skipped check simply waits for the next one, "
            "so these options only reduce how often the app contacts Nintendo. Sync now always works.",
            {"Skip while battery saver is on", "Skip on metered connections",
             "Quiet hours: skip between " + hours(config.quiet_start_hour) + " and " + hours(config.quiet_end_hour)},
            selected);
        if (!chosen) return;
        const auto has = [&](int index) { return std::find(chosen->begin(), chosen->end(), index) != chosen->end(); };
        try {
            config_.update([&](AppConfig& value) {
                value.pause_on_battery_saver = has(0);
                value.pause_on_metered = has(1);
                value.quiet_hours = has(2);
            });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        report_status("Auto-sync options saved.", "auto_sync_options_saved");
    };
    callbacks.export_settings = [this] {
        const auto config = config_.snapshot();
        return Json(Json::object{
            {"appearance", config.appearance == Appearance::dark ? "dark" :
                config.appearance == Appearance::light ? "light" : "system"},
            {"notifications", config.notifications},
            {"autoSync", config.auto_sync},
            {"syncIntervalMinutes", config.sync_interval_minutes},
            {"autoSyncPauseOnBatterySaver", config.pause_on_battery_saver},
            {"autoSyncPauseOnMetered", config.pause_on_metered},
            {"autoSyncQuietHours", config.quiet_hours},
            {"quietHoursStart", config.quiet_start_hour},
            {"quietHoursEnd", config.quiet_end_hour},
            {"rpcShowUsername", config.rpc.show_username},
            {"rpcShowProfilePicture", config.rpc.show_profile_picture},
            {"rpcShowPlayTime", config.rpc.show_play_time},
            {"rpcZelda", config.rpc.zelda},
            {"rpcAnimalCrossing", config.rpc.animal_crossing},
            {"rpcSplatoon3", config.rpc.splatoon3},
            {"rpcSplatoon2", config.rpc.splatoon2},
        });
    };
    callbacks.import_settings = [this, report_settings_failure](const Json& settings) {
        if (!settings.is_object()) return false;
        // Only preferences are imported. Rich Presence itself and anything that
        // needs a disclosure stays as it is on this PC.
        try {
            config_.update([&](AppConfig& value) {
                const auto appearance = settings.string("appearance", "system");
                value.appearance = appearance == "dark" ? Appearance::dark :
                    appearance == "light" ? Appearance::light : Appearance::system;
                value.notifications = settings.boolean("notifications", value.notifications);
                value.auto_sync = settings.boolean("autoSync", value.auto_sync);
                value.sync_interval_minutes = static_cast<int>(std::clamp<std::int64_t>(
                    settings.integer("syncIntervalMinutes", value.sync_interval_minutes),
                    kMinimumSyncIntervalMinutes, 7 * 24 * 60));
                value.pause_on_battery_saver = settings.boolean("autoSyncPauseOnBatterySaver", value.pause_on_battery_saver);
                value.pause_on_metered = settings.boolean("autoSyncPauseOnMetered", value.pause_on_metered);
                value.quiet_hours = settings.boolean("autoSyncQuietHours", value.quiet_hours);
                value.quiet_start_hour = static_cast<int>(std::clamp<std::int64_t>(
                    settings.integer("quietHoursStart", value.quiet_start_hour), 0, 23));
                value.quiet_end_hour = static_cast<int>(std::clamp<std::int64_t>(
                    settings.integer("quietHoursEnd", value.quiet_end_hour), 0, 23));
                value.rpc.show_username = settings.boolean("rpcShowUsername", value.rpc.show_username);
                value.rpc.show_profile_picture = settings.boolean("rpcShowProfilePicture", value.rpc.show_profile_picture);
                value.rpc.show_play_time = settings.boolean("rpcShowPlayTime", value.rpc.show_play_time);
                value.rpc.zelda = settings.boolean("rpcZelda", value.rpc.zelda);
                value.rpc.animal_crossing = settings.boolean("rpcAnimalCrossing", value.rpc.animal_crossing);
                value.rpc.splatoon3 = settings.boolean("rpcSplatoon3", value.rpc.splatoon3);
                value.rpc.splatoon2 = settings.boolean("rpcSplatoon2", value.rpc.splatoon2);
            });
        } catch (const std::exception&) {
            report_settings_failure();
            return false;
        }
        discord_.set_rpc_settings(config_.snapshot().rpc);
        reschedule_auto_sync();
        update_menu();
        return true;
    };

    callbacks.select_folder = [this, report_settings_failure] {
        const auto current = config_.snapshot();
        const auto selected = ui_.choose_folder(current.destination_folder);
        if (selected.empty() || selected == current.destination_folder) return;
        if (!current.watch_folder.empty() &&
            !valid_watch_folder(path_from_utf8(current.watch_folder), album_directory(path_from_utf8(selected)))) {
            ui_.notify("Switch Album Sync", "Stop watching the current folder before choosing this album destination.");
            return;
        }
        try {
            std::lock_guard context_lock(sync_context_mutex_);
            config_.update([&](AppConfig& value) {
                destination_generation_.fetch_add(1);
                value.destination_folder = selected;
                value.last_sync = "Never";
            });
            clear_pending_retries();
            std::lock_guard state_lock(state_mutex_);
            last_sync_ = "Never";
            failed_ids_.clear();
            sync_progress_ = {};
            sync_detail_.clear();
            recent_captures_.clear();
            capture_library_.reset();
            status_ = "Album folder updated";
            diagnostic_code_ = "album_folder_updated";
        } catch (const std::exception&) {
            report_settings_failure();
            queue_recent_refresh();
            return;
        }
        queue_recent_refresh();
        update_menu();
    };

    callbacks.open_folder = [this] {
        const auto folder = album_directory(path_from_utf8(config_.snapshot().destination_folder));
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (!error) {
            open_path(folder);
            return;
        }
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Could not open the album folder. Check folder access or choose another folder.";
            diagnostic_code_ = "album_storage_failed";
        }
        update_menu();
    };

    callbacks.toggle_start = [this, report_settings_failure] {
        set_start_on_boot(!start_on_boot_enabled());
        const bool enabled = start_on_boot_enabled();
        try {
            config_.update([enabled](AppConfig& value) { value.start_on_boot = enabled; });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        update_menu();
    };

    callbacks.proxy = [this, report_settings_failure] {
        const auto current = config_.snapshot();
        const auto proxy = ui_.prompt(
            "HTTP Proxy",
            "Enter an http:// proxy URL, or leave this empty to use the Windows proxy settings.",
            current.proxy_url);
        const auto normalized = trim(proxy);
        if (!normalized.empty() && lower(normalized).rfind("http://", 0) != 0) {
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Proxy not changed. Enter a URL that starts with http://.";
            }
            update_menu();
            return;
        }
        try {
            config_.update([&](AppConfig& value) { value.proxy_url = normalized; });
        } catch (const std::exception&) {
            report_settings_failure();
            return;
        }
        http_.set_proxy(normalized);
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = normalized.empty()
                ? "Using the Windows proxy settings"
                : "HTTP proxy updated";
        }
        update_menu();
    };

    callbacks.sign_in_out = [this] { sign_in_or_out(); };
    callbacks.paste_sign_in_link = [this] { paste_sign_in_link(); };
    callbacks.cancel_sign_in = [this] {
        std::unique_lock auth_lock(auth_flow_mutex_, std::try_to_lock);
        if (!auth_lock.owns_lock() || !auth_pending_.exchange(false)) return;
        manual_sign_in_ = false;
        pasted_auth_link_.clear();
        unregister_nintendo_auth_protocol();
        clear_nintendo_auth_callback();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Nintendo Account sign-in was cancelled.";
            diagnostic_code_ = "sign_in_cancelled";
        }
        update_menu();
    };
    callbacks.exit = [this] { request_exit(); };

    ui_.run(callbacks);
    stop_workers();
    return 0;
}

}  // namespace nso
