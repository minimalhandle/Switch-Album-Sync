#pragma once

// A loopback stand-in for every remote service Switch Album Sync talks to:
// Nintendo Accounts, nxapi-auth, nxapi-znca-api, Coral and the media CDN.
//
// HttpClient::redirect_to_loopback(port) sends https://<host>/<path> to
// http://127.0.0.1:<port>/<host>/<path>, so the real client code runs
// unchanged. The mock answers like the real services and checks each request
// against the nxapi-znca-api public API terms
// (docs/public-api-terms.md, docs/api.md "Rate limits"):
//
//   * User-Agent with project name, version and contact URL
//   * X-znca-Client-Version on every request, never changing
//   * nxapi-auth bearer token on every znca request, and tokens cached for
//     their full validity (no new token while the current one is valid)
//   * at most one concurrent request to the API
//   * no automatic retries, except as Retry-After allows
//   * /f: 10 method-1 requests / 60 min per Nintendo Account,
//         20 method-2 requests / 30 min per Coral user
//   * /f without the deprecated timestamp / request_id fields
//
// and Nintendo's own services: Nintendo and Coral tokens cached for their
// lifetime, no retries after an HTTP response, each capture downloaded once.
// A breach is recorded as a violation; rate-limit breaches are also refused
// with 429, as the real API does.

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/util.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mock {

inline constexpr char kNintendoAccounts[] = "accounts.nintendo.com";
inline constexpr char kNintendoProfile[] = "api.accounts.nintendo.com";
inline constexpr char kNxapiAuth[] = "nxapi-auth.fancy.org.uk";
inline constexpr char kNxapiZnca[] = "nxapi-znca-api.fancy.org.uk";
inline constexpr char kCoral[] = "api-lp1.znc.srv.nintendo.net";
inline constexpr char kMediaCdn[] = "album-media.mock-cdn.nintendo.net";

struct Request {
    double at = 0;           // seconds since the mock started
    double duration_ms = 0;  // time the mock held the request
    std::string host;
    std::string method;
    std::string path;        // without query
    std::string label;       // short endpoint name for reports
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};

struct Violation {
    double at = 0;
    std::string rule;
    std::string detail;
};

struct Fault {
    std::string host;
    std::string path;
    int status = 500;
    std::string retry_after;  // empty: no header
    int remaining = 1;
};

struct MediaFixture {
    std::string id;
    std::string app_name = "Mock Kart";
    std::int64_t captured_at = 0;
    std::int64_t size = 2048;
};

struct Options {
    std::string na_id = "na-mock-0001";
    std::int64_t coral_user_id = 5550001;
    int nintendo_token_ttl = 900;  // seconds, like Nintendo
    int nxapi_token_ttl = 300;
    int coral_token_ttl = 7200;
    int nxapi_latency_ms = 0;      // widens the window for concurrency checks
    std::string expected_client_version = NSO_ZNCA_CLIENT_VERSION;
};

class MockNintendoApi {
public:
    explicit MockNintendoApi(Options options = {})
        : options_(std::move(options)), start_(Clock::now()) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Start Winsock");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("Create mock socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 16) != 0) {
            throw std::runtime_error("Listen on mock socket");
        }
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~MockNintendoApi() {
        closesocket(listener_);
        if (acceptor_.joinable()) acceptor_.join();
        {
            std::lock_guard lock(mutex_);
            for (const auto client : clients_) shutdown(client, SD_BOTH);
        }
        for (auto& connection : connections_) {
            if (connection.joinable()) connection.join();
        }
        WSACleanup();
    }

    MockNintendoApi(const MockNintendoApi&) = delete;
    MockNintendoApi& operator=(const MockNintendoApi&) = delete;

    unsigned short port() const { return port_; }
    double now() const { return seconds_since(start_); }
    const Options& options() const { return options_; }

    std::vector<Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::vector<Violation> violations() const {
        std::lock_guard lock(mutex_);
        return violations_;
    }
    std::size_t count(const std::string& label) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(),
            [&](const Request& request) { return request.label == label; }));
    }
    std::size_t count_host(const std::string& host) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(),
            [&](const Request& request) { return request.host == host; }));
    }
    int max_nxapi_in_flight() const { return max_nxapi_in_flight_.load(); }

    void inject(Fault fault) {
        std::lock_guard lock(mutex_);
        faults_.push_back(std::move(fault));
    }
    void set_playing(bool playing) {
        std::lock_guard lock(mutex_);
        playing_ = playing;
    }
    void add_media(MediaFixture media) {
        std::lock_guard lock(mutex_);
        if (media.captured_at == 0) media.captured_at = 1762944000 + static_cast<std::int64_t>(media_.size()) * 60;
        media_.push_back(std::move(media));
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Token {
        double expires_at = 0;
    };

    struct Response {
        int status = 200;
        std::string content_type = "application/json";
        std::string body;
        std::vector<std::string> headers;
    };

    static double seconds_since(Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    static std::string lower(std::string text) {
        for (auto& character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return text;
    }

    void accept_loop() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::lock_guard lock(mutex_);
            clients_.push_back(client);
            connections_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(SOCKET client) {
        std::string buffer;
        char chunk[16384];
        for (;;) {
            const auto header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                Request request;
                std::size_t content_length = 0;
                parse_head(buffer.substr(0, header_end), request, content_length);
                const auto total = header_end + 4 + content_length;
                if (buffer.size() >= total) {
                    request.body = buffer.substr(header_end + 4, content_length);
                    buffer.erase(0, total);
                    const auto reply = handle(std::move(request));
                    if (send(client, reply.data(), static_cast<int>(reply.size()), 0) <= 0) break;
                    continue;
                }
            }
            const int received = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (received <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(received));
        }
        closesocket(client);
    }

    static void parse_head(const std::string& head, Request& request, std::size_t& content_length) {
        const auto line_end = head.find("\r\n");
        const auto request_line = head.substr(0, line_end);
        const auto first_space = request_line.find(' ');
        const auto second_space = request_line.find(' ', first_space + 1);
        request.method = request_line.substr(0, first_space);
        auto target = request_line.substr(first_space + 1, second_space - first_space - 1);
        if (const auto query = target.find('?'); query != std::string::npos) target.resize(query);
        // /<host>/<path> as rewritten by HttpClient::redirect_to_loopback.
        const auto host_end = target.find('/', 1);
        request.host = target.substr(1, host_end == std::string::npos ? std::string::npos : host_end - 1);
        request.path = host_end == std::string::npos ? "/" : target.substr(host_end);

        std::size_t start = line_end;
        while (start != std::string::npos && start < head.size()) {
            start += 2;
            const auto end = head.find("\r\n", start);
            const auto line = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (const auto colon = line.find(':'); colon != std::string::npos) {
                auto value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                request.headers[lower(line.substr(0, colon))] = value;
            }
            start = end;
        }
        if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
            content_length = static_cast<std::size_t>(std::stoull(found->second));
        }
    }

    static std::string label_for(const Request& request) {
        const auto& h = request.host;
        if (h == kNintendoAccounts) return "nintendo:token";
        if (h == kNintendoProfile) return "nintendo:profile";
        if (h == kNxapiAuth) return "nxapi-auth:token";
        if (h == kNxapiZnca) {
            if (request.path == "/api/znca/f") {
                try {
                    const auto json = nso::Json::parse(request.body);
                    return "nxapi:f(method " + json.string("hash_method") + ")";
                } catch (...) {
                    return "nxapi:f";
                }
            }
            return "nxapi:" + request.path.substr(std::string("/api/znca/").size());
        }
        if (h == kCoral) return "coral:" + request.path.substr(std::string("/v4/").size());
        if (h == kMediaCdn) return "media:download";
        return h + request.path;
    }

    void violation(const std::string& rule, const std::string& detail) {
        violations_.push_back({now(), rule, detail});
    }

    bool valid_token(const std::map<std::string, Token>& tokens, const std::string& header) const {
        constexpr std::string_view prefix = "Bearer ";
        if (header.rfind(prefix, 0) != 0) return false;
        const auto found = tokens.find(header.substr(prefix.size()));
        return found != tokens.end() && now() < found->second.expires_at;
    }

    bool any_valid(const std::map<std::string, Token>& tokens, double margin) const {
        return std::any_of(tokens.begin(), tokens.end(),
            [&](const auto& token) { return now() < token.second.expires_at - margin; });
    }

    std::string header(const Request& request, const std::string& name) const {
        const auto found = request.headers.find(name);
        return found == request.headers.end() ? std::string{} : found->second;
    }

    std::string handle(Request request) {
        const auto received = Clock::now();
        request.at = now();
        request.label = label_for(request);
        const bool nxapi = request.host == kNxapiZnca || request.host == kNxapiAuth;
        if (nxapi) {
            const int in_flight = ++nxapi_in_flight_;
            int expected = max_nxapi_in_flight_.load();
            while (in_flight > expected && !max_nxapi_in_flight_.compare_exchange_weak(expected, in_flight)) {
            }
            if (options_.nxapi_latency_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(options_.nxapi_latency_ms));
            }
        }

        Response response;
        {
            std::lock_guard lock(mutex_);
            if (nxapi && nxapi_in_flight_.load() > 1) {
                violation("nxapi: 1 concurrent request", request.label + " overlapped another nxapi request");
            }
            check_retry(request);
            response = respond(request);
            request.status = response.status;
            request.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - received).count();
            if (response.status / 100 != 2) last_error_[request.host + request.path + request.body] = now();
            requests_.push_back(request);
        }
        if (nxapi) --nxapi_in_flight_;

        std::string reply = "HTTP/1.1 " + std::to_string(response.status) + " Mock\r\n";
        reply += "Content-Type: " + response.content_type + "\r\n";
        reply += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        for (const auto& line : response.headers) reply += line + "\r\n";
        reply += "\r\n" + response.body;
        return reply;
    }

    // Repeating an identical request soon after an error response is an
    // automatic retry, which neither nxapi nor Nintendo services allow.
    void check_retry(const Request& request) {
        const auto key = request.host + request.path + request.body;
        const auto found = last_error_.find(key);
        if (found != last_error_.end() && now() - found->second < 10.0) {
            violation("No automatic retries", request.label + " repeated " +
                std::to_string(now() - found->second).substr(0, 4) + "s after an error response");
        }
    }

    Response respond(const Request& request) {
        for (auto fault = faults_.begin(); fault != faults_.end(); ++fault) {
            if (fault->host == request.host && fault->path == request.path) {
                Response injected{fault->status, "application/json", R"({"error":"injected"})", {}};
                if (fault->status == 429) injected.body = R"({"error":"rate_limit"})";
                if (!fault->retry_after.empty()) {
                    injected.headers.push_back("Retry-After: " + fault->retry_after);
                    if (request.host == kNxapiZnca || request.host == kNxapiAuth) {
                        nxapi_retry_after_until_ = now() + std::stod(fault->retry_after);
                    }
                }
                if (--fault->remaining <= 0) faults_.erase(fault);
                return injected;
            }
        }
        if (request.host == kNxapiZnca || request.host == kNxapiAuth) {
            check_nxapi_common(request);
        }
        if (request.host == kNintendoAccounts) return nintendo_token(request);
        if (request.host == kNintendoProfile) return nintendo_profile(request);
        if (request.host == kNxapiAuth) return nxapi_token(request);
        if (request.host == kNxapiZnca) return znca(request);
        if (request.host == kCoral) return coral(request);
        if (request.host == kMediaCdn) return media(request);
        violation("Known hosts only", "Request to unexpected host " + request.host);
        return {404, "application/json", "{}", {}};
    }

    void check_nxapi_common(const Request& request) {
        static const std::regex user_agent(R"(^switch-album-sync/\d+\.\d+\.\d+ \(\+https?://\S+\)$)");
        if (!std::regex_match(header(request, "user-agent"), user_agent)) {
            violation("nxapi: User-Agent", request.label + " sent User-Agent '" + header(request, "user-agent") + "'");
        }
        if (now() < nxapi_retry_after_until_) {
            violation("nxapi: honour Retry-After", request.label + " sent " +
                std::to_string(nxapi_retry_after_until_ - now()).substr(0, 4) + "s before Retry-After expired");
        }
    }

    Response nintendo_token(const Request& request) {
        if (request.path != "/connect/1.0.0/api/token") return {404, "application/json", "{}", {}};
        // The client renews 10 s early; anything sooner skipped the cache.
        if (any_valid(nintendo_tokens_, 15.0)) {
            violation("Nintendo: cache id_token", "New Nintendo token requested while one was still valid");
        }
        const auto id = "natok-" + std::to_string(++serial_);
        nintendo_tokens_[id] = {now() + options_.nintendo_token_ttl};
        return {200, "application/json", nso::Json(nso::Json::object{
            {"id_token", "idtok-" + std::to_string(serial_)},
            {"access_token", id},
            {"expires_in", options_.nintendo_token_ttl},
        }).dump(), {}};
    }

    Response nintendo_profile(const Request& request) {
        if (!valid_token(nintendo_tokens_, header(request, "authorization"))) {
            violation("Nintendo: valid token", "Profile requested without a valid Nintendo access token");
            return {401, "application/json", R"({"error":"invalid_token"})", {}};
        }
        return {200, "application/json", nso::Json(nso::Json::object{
            {"id", options_.na_id}, {"nickname", "Mock Player"}, {"language", "en-US"},
            {"country", "US"}, {"birthday", "1990-01-01"},
        }).dump(), {}};
    }

    Response nxapi_token(const Request& request) {
        const auto grant = form_value(request.body, "grant_type");
        if (grant == "refresh_token") {
            const auto refresh = form_value(request.body, "refresh_token");
            if (!refresh_tokens_.contains(refresh)) {
                return {400, "application/json", R"({"error":"invalid_grant"})", {}};
            }
            refresh_tokens_.erase(refresh);
        } else if (grant != "client_credentials") {
            return {400, "application/json", R"({"error":"unsupported_grant_type"})", {}};
        }
        if (any_valid(nxapi_tokens_, 0.0)) {
            violation("nxapi-auth: cache token for full validity",
                "New nxapi-auth token (" + grant + ") requested while one was still valid");
        }
        ++nxapi_grants_[grant];
        const auto access = "nxat-" + std::to_string(++serial_);
        const auto refresh = "nxrt-" + std::to_string(serial_);
        nxapi_tokens_[access] = {now() + options_.nxapi_token_ttl};
        refresh_tokens_.insert(refresh);
        return {200, "application/json", nso::Json(nso::Json::object{
            {"access_token", access}, {"refresh_token", refresh},
            {"token_type", "Bearer"}, {"expires_in", options_.nxapi_token_ttl},
        }).dump(), {}};
    }

    Response znca(const Request& request) {
        if (header(request, "x-znca-client-version") != options_.expected_client_version) {
            violation("nxapi: X-znca-Client-Version", request.label + " sent '" +
                header(request, "x-znca-client-version") + "'");
        }
        if (!valid_token(nxapi_tokens_, header(request, "authorization"))) {
            violation("nxapi: authenticate with nxapi-auth", request.label + " without a valid nxapi-auth token");
            return {401, "application/json", R"({"error":"invalid_token"})", {}};
        }
        const auto& path = request.path;
        if (path == "/api/znca/config") {
            return {200, "application/json", R"({"nso_version":"3.1.0"})", {}};
        }
        if (header(request, "x-znca-platform").empty() || header(request, "x-znca-version").empty()) {
            violation("nxapi: X-znca-Platform/Version", request.label + " missing platform/version headers");
        }
        const auto json = nso::Json::parse(request.body);
        if (path == "/api/znca/f") return generate_f(request, json);
        if (path == "/api/znca/encrypt-request") {
            const auto data = json.string("data");
            return {200, "application/json", nso::Json(nso::Json::object{
                {"data", nso::base64_encode(std::vector<unsigned char>(data.begin(), data.end()))},
            }).dump(), {}};
        }
        if (path == "/api/znca/decrypt-response") {
            const auto bytes = nso::base64_decode(json.string("data"));
            return {200, "text/plain", std::string(bytes.begin(), bytes.end()), {}};
        }
        return {404, "application/json", "{}", {}};
    }

    Response generate_f(const Request&, const nso::Json& json) {
        if (json.find("timestamp") || json.find("request_id")) {
            violation("nxapi: no timestamp/request_id", "/f sent fields removed in nxapi 2.12.0");
        }
        const auto method = json.string("hash_method");
        const bool coral_auth = method == "1";
        const auto key = coral_auth ? json.string("na_id") : json.string("coral_user_id");
        const double window = coral_auth ? 3600 : 1800;
        const std::size_t limit = coral_auth ? 10 : 20;
        auto& history = f_history_[method + ":" + key];
        while (!history.empty() && now() - history.front() >= window) history.pop_front();
        if (history.size() >= limit) {
            violation("nxapi: /f rate limit", "method " + method + " exceeded " + std::to_string(limit) +
                " requests per " + std::to_string(static_cast<int>(window / 60)) + " min");
            nxapi_retry_after_until_ = now() + 600;
            return {429, "application/json", R"({"error":"rate_limit"})", {"Retry-After: 600"}};
        }
        history.push_back(now());
        return {200, "application/json", nso::Json(nso::Json::object{
            {"f", "mock-f-" + std::to_string(++serial_)},
            {"request_id", "req-" + std::to_string(serial_)},
            {"timestamp", static_cast<std::int64_t>(1762944000000LL + serial_)},
        }).dump(), {}};
    }

    Response coral(const Request& request) {
        const auto& path = request.path;
        if (path == "/v4/Account/Login") {
            if (any_valid(coral_tokens_, 60.0)) {
                violation("Coral: cache access token", "Account/Login while a Coral token was still valid");
            }
            const auto token = "coral-" + std::to_string(++serial_);
            coral_tokens_[token] = {now() + options_.coral_token_ttl};
            return {200, "application/octet-stream", nso::Json(nso::Json::object{
                {"status", 0},
                {"result", nso::Json(nso::Json::object{
                    {"user", nso::Json(nso::Json::object{{"id", options_.coral_user_id}, {"name", "Mock Player"}})},
                    {"webApiServerCredential", nso::Json(nso::Json::object{
                        {"accessToken", token}, {"expiresIn", options_.coral_token_ttl}})},
                })},
            }).dump(), {}};
        }
        if (!valid_token(coral_tokens_, header(request, "authorization"))) {
            violation("Coral: valid token", request.label + " without a valid Coral token");
            return {401, "application/json", R"({"status":9404})", {}};
        }
        if (path == "/v4/User/ShowSelf") {
            nso::Json::object presence{{"state", playing_ ? "PLAYING" : "OFFLINE"}, {"updatedAt", 1762944000}};
            if (playing_) {
                presence["game"] = nso::Json(nso::Json::object{
                    {"name", "Mock Kart"}, {"totalPlayTime", 120},
                    {"shopUri", "https://ec.nintendo.com/apps/0100000000010000/US"},
                });
            }
            return {200, "application/octet-stream", nso::Json(nso::Json::object{
                {"status", 0},
                {"result", nso::Json(nso::Json::object{
                    {"name", "Mock Player"}, {"presence", nso::Json(std::move(presence))}})},
            }).dump(), {}};
        }
        if (path == "/v4/Game/GetWebServiceToken") {
            return {200, "application/octet-stream", nso::Json(nso::Json::object{
                {"status", 0},
                {"result", nso::Json(nso::Json::object{
                    {"accessToken", "wst-" + std::to_string(++serial_)}, {"expiresIn", 7200}})},
            }).dump(), {}};
        }
        if (path == "/v4/Media/List") {
            nso::Json::array items;
            for (const auto& media : media_) {
                items.push_back(nso::Json(nso::Json::object{
                    {"id", media.id}, {"appName", media.app_name}, {"type", "image"},
                    {"capturedAt", media.captured_at}, {"uploadedAt", media.captured_at},
                    {"expiresAt", media.captured_at + 30LL * 86400},
                    {"contentUri", std::string("https://") + kMediaCdn + "/captures/" + media.id + ".jpg"},
                    {"contentLength", media.size},
                }));
            }
            return {200, "application/octet-stream", nso::Json(nso::Json::object{
                {"status", 0}, {"result", nso::Json(nso::Json::object{{"media", nso::Json(std::move(items))}})},
            }).dump(), {}};
        }
        return {404, "application/json", "{}", {}};
    }

    Response media(const Request& request) {
        if (!downloaded_.insert(request.path).second) {
            violation("Download each capture once", request.path + " downloaded again");
        }
        for (const auto& item : media_) {
            if (request.path == "/captures/" + item.id + ".jpg") {
                return {200, "image/jpeg", std::string(static_cast<std::size_t>(item.size), 'x'), {}};
            }
        }
        return {404, "text/plain", "missing", {}};
    }

    static std::string form_value(const std::string& body, const std::string& name) {
        std::size_t start = 0;
        while (start <= body.size()) {
            const auto end = body.find('&', start);
            const auto pair = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (pair.rfind(name + "=", 0) == 0) return pair.substr(name.size() + 1);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return {};
    }

    Options options_;
    Clock::time_point start_;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread acceptor_;
    mutable std::mutex mutex_;
    std::vector<SOCKET> clients_;
    std::vector<std::thread> connections_;

    std::vector<Request> requests_;
    std::vector<Violation> violations_;
    std::vector<Fault> faults_;
    std::map<std::string, double> last_error_;
    std::atomic<int> nxapi_in_flight_{0};
    std::atomic<int> max_nxapi_in_flight_{0};
    double nxapi_retry_after_until_ = 0;

    std::uint64_t serial_ = 0;
    std::map<std::string, Token> nintendo_tokens_;
    std::map<std::string, Token> nxapi_tokens_;
    std::set<std::string> refresh_tokens_;
    std::map<std::string, Token> coral_tokens_;
    std::map<std::string, int> nxapi_grants_;
    std::map<std::string, std::deque<double>> f_history_;
    std::set<std::string> downloaded_;
    std::vector<MediaFixture> media_;
    bool playing_ = true;
};

}  // namespace mock
