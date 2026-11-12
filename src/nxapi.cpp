#include "nso_album_sync/nxapi.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/util.hpp"
#include "nso_album_sync/version.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

#include "nso_album_sync/windows_compat.hpp"

namespace nso {
namespace {

constexpr char kZncaBaseUrl[] =
    "https://nxapi-znca-api.fancy.org.uk/api/znca";
constexpr char kNxapiAuthUrl[] =
    "https://nxapi-auth.fancy.org.uk/api/oauth/token";
// The public API terms require a User-Agent with the project name, version and
// contact details, in the preferred `project/version (+url)` form.
constexpr char kUserAgent[] =
    "switch-album-sync/" NSO_APP_VERSION " (+" NSO_PROJECT_URL ")";
// Must be sent on every request and must never be set dynamically.
constexpr char kZncaClientVersion[] = NSO_ZNCA_CLIENT_VERSION;

constexpr auto kVersionCacheLifetime = std::chrono::hours(6);
constexpr auto kVersionRetryLifetime = std::chrono::minutes(15);
constexpr auto kDefaultRateLimitBackoff = std::chrono::minutes(15);

std::vector<std::string> znca_headers(const std::string& nso_version) {
    return {
        "X-znca-Platform: Android",
        "X-znca-Version: " + nso_version,
    };
}

std::int64_t epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point time_from_epoch(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

void replace_cache_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
    if (!MoveFileExW(
            temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
}

// nxapi explains failures with an error code such as incompatible_client.
// Only the code is kept: it identifies the problem and contains no user data.
std::string error_code(const HttpResponse& response) {
    try {
        const auto code = Json::parse(response.text()).string("error");
        if (!code.empty() && code.size() <= 64 &&
            code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos) return ", " + code;
    } catch (...) {
    }
    return {};
}

std::chrono::seconds retry_after_delay(const std::string& value) {
    try {
        const auto seconds = std::stoll(value);
        return std::chrono::seconds(std::max<std::int64_t>(1, seconds));
    } catch (...) {
    }

    std::tm parsed{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultRateLimitBackoff);
    }

    const std::time_t retry_at = _mkgmtime(&parsed);
    if (retry_at <= 0) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultRateLimitBackoff);
    }

    const auto now = std::time(nullptr);
    return std::chrono::seconds(std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(retry_at - now)));
}

}  // namespace

NxapiClient::NxapiClient(
    HttpClient& http,
    std::string client_id,
    std::filesystem::path cache_file)
    : http_(http),
      client_id_(std::move(client_id)),
      cache_file_(std::move(cache_file)) {
    load_cache();
}

void NxapiClient::load_cache() {
    if (cache_file_.empty() || !std::filesystem::exists(cache_file_)) return;
    try {
        std::ifstream file(cache_file_, std::ios::binary);
        const std::string contents{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        const auto json = Json::parse(contents);
        version_ = json.string("nsoVersion");
        version_expiry_ = time_from_epoch(json.integer("nsoVersionExpiresAt"));
        rate_limit_until_ = time_from_epoch(json.integer("rateLimitUntil"));
    } catch (...) {
        version_.clear();
        version_expiry_ = {};
        rate_limit_until_ = {};
    }
}

void NxapiClient::save_cache() const {
    if (cache_file_.empty()) return;
    try {
        std::filesystem::create_directories(cache_file_.parent_path());
        const Json json(Json::object{
            {"nsoVersion", version_},
            {"nsoVersionExpiresAt", epoch_seconds(version_expiry_)},
            {"rateLimitUntil", epoch_seconds(rate_limit_until_)},
        });
        auto temporary = cache_file_;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) return;
            file << json.dump();
            file.flush();
            if (!file) return;
        }
        replace_cache_file(temporary, cache_file_);
    } catch (...) {
        // Cache persistence is an optimisation; never break login because disk
        // cache storage is unavailable.
    }
}

void NxapiClient::clear_user_auth() {
    auth_generation_.fetch_add(1);
    std::lock_guard lock(auth_mutex_);
    auth_token_.clear();
    refresh_token_.clear();
    auth_expiry_ = {};
}

void NxapiClient::throw_if_rate_limited() const {
    if (Clock::now() < rate_limit_until_) {
        throw std::runtime_error(
            "nxapi-znca-api Retry-After backoff is active");
    }
}

void NxapiClient::apply_rate_limit_response(const HttpResponse& response) {
    if (response.status == 401) {
        // The access token was rejected (invalid_token). Do not retry this
        // request. Forget only the access token: nxapi-auth's docs say to
        // renew with the refresh token, which the next request will do.
        std::lock_guard lock(auth_mutex_);
        auth_token_.clear();
        auth_expiry_ = {};
    }

    if (response.status == 406) {
        // The cached Nintendo Switch Online version is explicitly unsupported.
        // Do not retry this request; force the next independent action to fetch
        // /config instead of repeatedly sending a known-bad version.
        version_.clear();
        version_expiry_ = {};
        save_cache();
    }

    if (response.status != 429) return;

    auto delay = std::chrono::duration_cast<std::chrono::seconds>(
        kDefaultRateLimitBackoff);
    if (const auto header = response.headers.find("retry-after");
        header != response.headers.end()) {
        delay = retry_after_delay(header->second);
    }

    rate_limit_until_ = Clock::now() + delay;
    save_cache();
}

std::string NxapiClient::auth_token() {
    // Serialize token acquisition/refresh, but never hold the small state mutex
    // across network I/O. Signing out must be able to invalidate credentials
    // immediately even if nxapi-auth is slow or unavailable.
    std::lock_guard request_lock(auth_request_mutex_);
    const auto generation = auth_generation_.load();
    const auto now = Clock::now();

    std::string refresh_token;
    {
        std::lock_guard state_lock(auth_mutex_);
        if (!auth_token_.empty() && now < auth_expiry_) return auth_token_;
        refresh_token = refresh_token_;
    }

    const auto request_token = [&](const std::string& grant_type,
                                   const std::string& token_to_refresh) {
        std::map<std::string, std::string> fields{
            {"grant_type", grant_type},
            {"client_id", client_id_},
            {"scope", "ca:gf ca:er ca:dr"},
        };
        if (!token_to_refresh.empty()) fields["refresh_token"] = token_to_refresh;
        auto response = http_.post(
            kNxapiAuthUrl,
            HttpClient::form_encode(fields),
            {
                "Accept: application/json",
                std::string("User-Agent: ") + kUserAgent,
            },
            "application/x-www-form-urlencoded");
        // The auth service can rate-limit too. Never retry it automatically;
        // persist any Retry-After so a restart cannot bypass the backoff.
        apply_rate_limit_response(response);
        return response;
    };

    HttpResponse response;
    if (!refresh_token.empty()) {
        response = request_token("refresh_token", refresh_token);
        if (response.status / 100 != 2) {
            std::string error_code;
            try {
                error_code = Json::parse(response.text()).string("error");
            } catch (...) {
            }
            if (error_code != "invalid_grant") {
                throw std::runtime_error(
                    "nxapi-auth refresh failed (HTTP " + std::to_string(response.status) + ")");
            }
            refresh_token.clear();
            std::lock_guard state_lock(auth_mutex_);
            if (auth_generation_.load() == generation) refresh_token_.clear();
        }
    }

    if (refresh_token.empty() || response.status / 100 != 2) {
        response = request_token("client_credentials", "");
    }
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi-auth token request failed (HTTP " + std::to_string(response.status) + ")");
    }

    const auto json = Json::parse(response.text());
    const auto access_token = json.string("access_token");
    const auto new_refresh_token = json.string("refresh_token", refresh_token);
    if (access_token.empty()) {
        throw std::runtime_error("nxapi-auth response missing access_token");
    }
    const auto expires_in = json.integer("expires_in", 300);

    if (auth_generation_.load() != generation) {
        throw std::runtime_error("nxapi authentication cancelled");
    }
    {
        std::lock_guard state_lock(auth_mutex_);
        if (auth_generation_.load() != generation) {
            throw std::runtime_error("nxapi authentication cancelled");
        }
        auth_token_ = access_token;
        refresh_token_ = new_refresh_token;
        // Public API terms require caching this token for its full validity
        // period. Measure from receipt rather than request start.
        auth_expiry_ = Clock::now() + std::chrono::seconds(
            std::max<std::int64_t>(1, expires_in));
    }
    return access_token;
}

HttpResponse NxapiClient::znca_request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& accept,
    const std::vector<std::string>& extra_headers) {
    // Serialize all requests. The public API requires at most one concurrent
    // automated request, and serialising interactive work as well avoids bursts.
    std::lock_guard lock(request_mutex_);
    throw_if_rate_limited();

    std::vector<std::string> headers{
        "Accept: " + accept,
        std::string("User-Agent: ") + kUserAgent,
        std::string("X-znca-Client-Version: ") + kZncaClientVersion,
    };
    const auto bearer_token = auth_token();
    if (!bearer_token.empty()) {
        headers.push_back("Authorization: Bearer " + bearer_token);
    }
    headers.insert(headers.end(), extra_headers.begin(), extra_headers.end());

    const auto response = http_.request(
        method,
        std::string(kZncaBaseUrl) + path,
        headers,
        std::vector<unsigned char>(body.begin(), body.end()),
        body.empty() ? "" : "application/json",
        30);
    apply_rate_limit_response(response);
    return response;
}

std::string NxapiClient::nso_version() {
    std::lock_guard lock(request_mutex_);
    const auto now = Clock::now();
    if (!version_.empty() && now < version_expiry_) return version_;

    const auto response = znca_request("GET", "/config", "", "application/json");
    if (response.status / 100 == 2) {
        const auto supported_version = Json::parse(response.text()).string("nso_version");
        if (!supported_version.empty()) {
            version_ = supported_version;
            version_expiry_ = now + kVersionCacheLifetime;
            save_cache();
            return version_;
        }
    }

    if (!version_.empty()) {
        version_expiry_ = now + kVersionRetryLifetime;
        save_cache();
        return version_;
    }
    throw std::runtime_error(
        "Could not retrieve supported NSO version from nxapi /config");
}

FAttestation NxapiClient::generate_f(
    int hash_method,
    const std::string& token,
    const std::string& na_id,
    const std::string& coral_user_id) {
    const auto version = nso_version();
    // The /f API accepts coral_user_id only for hash method 2.
    // Passing an empty coral_user_id for method 1 fails request validation.
    Json::object fields{
        {"hash_method", std::to_string(hash_method)},
        {"token", token},
        {"na_id", na_id},
    };
    if (hash_method == 2 && !coral_user_id.empty()) {
        fields.emplace("coral_user_id", coral_user_id);
    }
    const Json payload(std::move(fields));

    const auto response = znca_request(
        "POST", "/f", payload.dump(), "application/json", znca_headers(version));
    if (response.status / 100 != 2) {
        throw std::runtime_error("nxapi /f failed (HTTP " + std::to_string(response.status) + error_code(response) + ")");
    }
    const auto json = Json::parse(response.text());
    FAttestation attestation;
    attestation.f = json.string("f");
    attestation.request_id = json.string("request_id");
    attestation.timestamp = json.integer("timestamp");
    if (attestation.f.empty() || attestation.request_id.empty() || attestation.timestamp == 0) {
        throw std::runtime_error("nxapi /f returned incomplete attestation");
    }
    return attestation;
}

std::vector<unsigned char> NxapiClient::encrypt_request(
    const std::string& url,
    const std::string& coral_token,
    const std::string& json) {
    const auto version = nso_version();
    const Json payload(Json::object{
        {"url", url},
        {"token", coral_token.empty() ? Json(nullptr) : Json(coral_token)},
        {"data", json},
    });
    const auto response = znca_request(
        "POST", "/encrypt-request", payload.dump(), "application/json",
        znca_headers(version));
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi /encrypt-request failed (HTTP " + std::to_string(response.status) + error_code(response) + ")");
    }
    const auto encrypted = Json::parse(response.text()).string("data");
    if (encrypted.empty()) {
        throw std::runtime_error("nxapi /encrypt-request missing data");
    }
    return base64_decode(encrypted);
}

std::string NxapiClient::decrypt_response(
    const std::vector<unsigned char>& body) {
    const auto version = nso_version();
    const Json payload(Json::object{{"data", base64_encode(body)}});
    const auto response = znca_request(
        "POST", "/decrypt-response", payload.dump(), "text/plain",
        znca_headers(version));
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi /decrypt-response failed (HTTP " + std::to_string(response.status) + error_code(response) + ")");
    }
    return response.text();
}

}  // namespace nso
