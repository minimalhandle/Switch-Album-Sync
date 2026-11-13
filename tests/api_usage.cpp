// API usage tests: runs the real Nintendo/nxapi client code against
// MockNintendoApi and checks it stays within the nxapi-znca-api public API
// terms and uses Nintendo's services no more than needed.
//
//   ApiUsageTests                                fast compliance tests (ctest)
//   ApiUsageTests --soak <seconds> --report <dir> long run that follows the
//       app's real polling schedule and writes api-usage-report.{html,json}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mock_nintendo_api.hpp"

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/rpc.hpp"
#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/sync.hpp"

#include <windows.h>

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace {

using Path = std::filesystem::path;
Path g_root;
int g_harness = 0;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string describe(const std::vector<mock::Violation>& violations) {
    std::string text;
    for (const auto& violation : violations) text += "\n  [" + violation.rule + "] " + violation.detail;
    return text;
}

std::vector<mock::Violation> violations_of(const mock::MockNintendoApi& api, const std::string& rule) {
    std::vector<mock::Violation> matching;
    for (const auto& violation : api.violations()) {
        if (violation.rule == rule) matching.push_back(violation);
    }
    return matching;
}

void require_compliant(const mock::MockNintendoApi& api, const std::string& context) {
    const auto violations = api.violations();
    require(violations.empty(), context + ": terms violations" + describe(violations));
}

// The client stack the app builds, pointed at the mock.
struct Harness {
    explicit Harness(mock::Options options = {})
        : api(std::move(options)),
          directory(g_root / ("harness-" + std::to_string(++g_harness))),
          session_token("session-" + std::to_string(g_harness) + "-" + api.options().na_id) {
        std::filesystem::create_directories(directory);
        http.redirect_to_loopback(api.port());
    }
    ~Harness() { coral.clear_cached_session(); }

    mock::MockNintendoApi api;
    Path directory;
    std::string session_token;
    nso::HttpClient http;
    nso::NintendoAuthManager auth{http};
    nso::NxapiClient nxapi{http, NSO_NXAPI_AUTH_CLIENT_ID, directory / "nxapi-cache.json"};
    nso::CoralClient coral{http, auth, nxapi};
};

std::string form_value(const std::string& body, const std::string& name) {
    const auto start = body.find(name + "=");
    if (start == std::string::npos) return {};
    const auto end = body.find('&', start);
    return body.substr(start + name.size() + 1, end == std::string::npos ? std::string::npos : end - start - name.size() - 1);
}

constexpr char kCoralLoginUrl[] = "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login";
constexpr char kWebServiceTokenUrl[] = "https://api-lp1.znc.srv.nintendo.net/v4/Game/GetWebServiceToken";

std::vector<nso::Json> bodies_of(const mock::MockNintendoApi& api, const std::string& label) {
    std::vector<nso::Json> bodies;
    for (const auto& request : api.requests()) {
        if (request.label == label) bodies.push_back(nso::Json::parse(request.body));
    }
    return bodies;
}

// /encrypt-request calls for one Coral endpoint.
std::size_t separate_encryptions_of(const mock::MockNintendoApi& api, const std::string& url) {
    std::size_t count = 0;
    for (const auto& body : bodies_of(api, "nxapi:encrypt-request")) {
        if (body.string("url") == url) ++count;
    }
    return count;
}

// ---------------------------------------------------------------- fast tests

void https_requests_reach_the_mock() {
    mock::MockNintendoApi api;
    nso::HttpClient http;
    http.redirect_to_loopback(api.port());
    const auto response = http.get(std::string("https://") + mock::kNintendoProfile + "/2.0.0/users/me");
    require(response.status == 401, "Redirected request is answered by the mock");
    const auto requests = api.requests();
    require(requests.size() == 1 && requests[0].host == mock::kNintendoProfile &&
        requests[0].path == "/2.0.0/users/me", "Mock sees the original host and path");
}

void test_credentials_stay_out_of_the_real_store() {
    // main() switches to a test namespace before anything else runs.
    require(nso::SecureStore::put("ApiUsageProbe", "probe"), "Store a test credential");
    require(nso::SecureStore::get("ApiUsageProbe") == std::optional<std::string>("probe"), "Read it back");
    nso::SecureStore::use_namespace("Switch Album Sync");
    const auto real = nso::SecureStore::get("ApiUsageProbe");
    nso::SecureStore::use_namespace("Switch Album Sync API Tests");
    nso::SecureStore::erase("ApiUsageProbe");
    require(!real.has_value(), "Test credentials are not written to the app's real credential store");
}

void presence_polls_reuse_every_token() {
    Harness h;
    for (int i = 0; i < 3; ++i) h.coral.self_presence(h.session_token);
    require_compliant(h.api, "Three presence polls");
    require(h.api.count("nintendo:token") == 1, "One Nintendo token exchange for three polls");
    require(h.api.count("nintendo:profile") == 1, "One profile fetch for three polls");
    require(h.api.count("nxapi-auth:token") == 1, "One nxapi-auth token for three polls");
    require(h.api.count("nxapi:config") == 1, "nxapi /config is cached");
    require(h.api.count("nxapi:f(method 1)") == 1, "One Coral login attestation");
    require(h.api.count("coral:Account/Login") == 1, "One Coral login for three polls");
    require(h.api.count("coral:User/ShowSelf") == 3, "One ShowSelf per poll");
    require(h.api.count("nxapi:encrypt-request") == 3 && h.api.count("nxapi:decrypt-response") == 4,
        "The login is encrypted by its /f request, so only the polls use /encrypt-request (saw " +
        std::to_string(h.api.count("nxapi:encrypt-request")) + ")");
}

void coral_login_is_encrypted_by_its_f_request() {
    mock::Options options;
    options.na_id = "na-one-call-login";
    Harness h(options);
    h.coral.self_presence(h.session_token);
    require_compliant(h.api, "One-call login");
    const auto attestations = bodies_of(h.api, "nxapi:f(method 1)");
    require(attestations.size() == 1, "One Coral login attestation");
    const auto* encrypt = attestations[0].find("encrypt_token_request");
    require(encrypt != nullptr && encrypt->is_object(), "The login's /f request asks nxapi to encrypt the login");
    require(encrypt->string("url") == kCoralLoginUrl, "It names Account/Login");
    const auto* parameter = encrypt->find("parameter");
    require(parameter != nullptr && parameter->is_object(), "It carries the login parameter");
    require(parameter->string("naIdToken") == attestations[0].string("token"),
        "The login carries the id_token that /f signs");
    require(parameter->string("language") == "en-US", "The login carries the account's language");
    require(separate_encryptions_of(h.api, kCoralLoginUrl) == 0, "The login isn't encrypted again by /encrypt-request");
    require(h.api.count("coral:Account/Login") == 1, "One Coral login");
}

void web_service_token_is_encrypted_by_its_f_request() {
    mock::Options options;
    options.na_id = "na-one-call-web-service";
    Harness h(options);
    require(!h.coral.get_web_service_token(h.session_token, 4834290508791808ULL).empty(), "Web service token");
    require_compliant(h.api, "One-call web service token");
    const auto attestations = bodies_of(h.api, "nxapi:f(method 2)");
    require(attestations.size() == 1, "One web service attestation");
    const auto* encrypt = attestations[0].find("encrypt_token_request");
    require(encrypt != nullptr && encrypt->is_object(), "The /f request asks nxapi to encrypt GetWebServiceToken");
    require(encrypt->string("url") == kWebServiceTokenUrl, "It names Game/GetWebServiceToken");
    const auto* parameter = encrypt->find("parameter");
    require(parameter != nullptr && parameter->is_object() && parameter->integer("id") == 4834290508791808LL,
        "It asks for the requested game service");
    require(parameter->find("registrationToken") != nullptr && parameter->string("registrationToken").empty(),
        "It sends an empty registration token, like the app");
    require(separate_encryptions_of(h.api, kWebServiceTokenUrl) == 0,
        "GetWebServiceToken isn't encrypted again by /encrypt-request");
}

void sign_in_falls_back_when_f_does_not_encrypt() {
    mock::Options options;
    options.na_id = "na-two-call";
    options.one_call_encryption = false;
    Harness h(options);
    h.coral.self_presence(h.session_token);
    require(!h.coral.get_web_service_token(h.session_token, 4834290508791808ULL).empty(), "Web service token");
    require_compliant(h.api, "nxapi without one-call encryption");
    require(separate_encryptions_of(h.api, kCoralLoginUrl) == 1 &&
        separate_encryptions_of(h.api, kWebServiceTokenUrl) == 1,
        "The login and token request fall back to /encrypt-request");
}

void nxapi_requests_carry_required_headers() {
    Harness h;
    h.coral.self_presence(h.session_token);
    h.coral.get_web_service_token(h.session_token, 4834290508791808ULL);
    require_compliant(h.api, "nxapi headers");
    for (const auto& request : h.api.requests()) {
        if (request.host != mock::kNxapiZnca) continue;
        const auto& headers = request.headers;
        require(headers.contains("user-agent") && headers.at("user-agent").rfind("switch-album-sync/", 0) == 0,
            request.label + " identifies the project in User-Agent");
        require(headers.contains("x-znca-client-version") &&
            headers.at("x-znca-client-version") == NSO_ZNCA_CLIENT_VERSION,
            request.label + " sends the fixed X-znca-Client-Version");
        require(headers.contains("authorization"), request.label + " authenticates with nxapi-auth");
    }
}

void nxapi_never_runs_two_requests_at_once() {
    mock::Options options;
    options.na_id = "na-concurrency";
    options.nxapi_latency_ms = 40;
    Harness h(options);
    h.api.add_media({"m1"});
    std::exception_ptr failure;
    std::thread presence([&] {
        try {
            for (int i = 0; i < 4; ++i) h.coral.self_presence(h.session_token);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    for (int i = 0; i < 4; ++i) h.coral.media_list(h.session_token);
    presence.join();
    if (failure) std::rethrow_exception(failure);
    require(h.api.max_nxapi_in_flight() == 1, "At most one nxapi request in flight (saw " +
        std::to_string(h.api.max_nxapi_in_flight()) + ")");
    require_compliant(h.api, "Concurrent presence and sync");
}

void nxapi_retry_after_is_honoured() {
    mock::Options options;
    options.na_id = "na-retry-after";
    Harness h(options);
    h.api.inject({mock::kNxapiZnca, "/api/znca/encrypt-request", 429, "2"});
    bool failed = false;
    try {
        h.coral.self_presence(h.session_token);
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "A 429 from nxapi fails the poll");
    const auto after_429 = h.api.count_host(mock::kNxapiZnca) + h.api.count_host(mock::kNxapiAuth);
    for (int i = 0; i < 3; ++i) {
        try {
            h.coral.self_presence(h.session_token);
        } catch (const std::exception&) {
        }
    }
    require(h.api.count_host(mock::kNxapiZnca) + h.api.count_host(mock::kNxapiAuth) == after_429,
        "No nxapi requests while Retry-After is active");
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    h.coral.self_presence(h.session_token);
    require(h.api.count("coral:User/ShowSelf") == 1, "Polling resumes after Retry-After");
    require(violations_of(h.api, "nxapi: honour Retry-After").empty() &&
        violations_of(h.api, "No automatic retries").empty(), "Retry-After respected" + describe(h.api.violations()));
}

void nxapi_auth_token_is_refreshed_not_reissued() {
    mock::Options options;
    options.na_id = "na-refresh";
    options.nxapi_token_ttl = 1;
    Harness h(options);
    h.coral.self_presence(h.session_token);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    h.coral.self_presence(h.session_token);
    require_compliant(h.api, "nxapi-auth refresh");
    std::vector<std::string> grants;
    for (const auto& request : h.api.requests()) {
        if (request.host == mock::kNxapiAuth) grants.push_back(form_value(request.body, "grant_type"));
    }
    require(grants == std::vector<std::string>{"client_credentials", "refresh_token"},
        "An expired nxapi-auth token is renewed with its refresh token");
}

void coral_errors_are_not_retried() {
    mock::Options options;
    options.na_id = "na-no-retry";
    Harness h(options);
    h.api.inject({mock::kCoral, "/v4/User/ShowSelf", 500});
    try {
        h.coral.self_presence(h.session_token);
    } catch (const std::exception&) {
    }
    require(h.api.count("coral:User/ShowSelf") == 1, "A Coral error response is not retried");
    require_compliant(h.api, "Coral error");
}

void web_service_tokens_are_cached() {
    mock::Options options;
    options.na_id = "na-web-service";
    Harness h(options);
    for (int i = 0; i < 3; ++i) {
        require(!h.coral.get_web_service_token(h.session_token, 4834290508791808ULL).empty(), "Web service token");
    }
    require(h.api.count("nxapi:f(method 2)") == 1, "One web service attestation for three lookups");
    require(h.api.count("coral:Game/GetWebServiceToken") == 1, "One GetWebServiceToken for three lookups");
    require_compliant(h.api, "Web service tokens");
}

void coral_login_attestations_stay_under_limit() {
    mock::Options options;
    options.na_id = "na-f-limit";
    Harness h(options);
    // Eleven sign-ins in a row (each dropping every cached token) must not
    // exceed nxapi's 10 method-1 requests per hour.
    for (int i = 0; i < 11; ++i) {
        h.coral.clear_cached_session();
        h.auth.clear_cached_tokens();
        try {
            h.coral.self_presence(h.session_token);
        } catch (const std::exception&) {
        }
    }
    require(h.api.count("nxapi:f(method 1)") == 10, "The eleventh login is stopped locally");
    require(violations_of(h.api, "nxapi: /f rate limit").empty(), "Method-1 limit respected");
}

void sync_downloads_each_capture_once() {
    mock::Options options;
    options.na_id = "na-sync";
    Harness h(options);
    for (int i = 0; i < 3; ++i) h.api.add_media({"capture-" + std::to_string(i)});

    const auto app_data = h.directory / "AppData";
    std::filesystem::create_directories(app_data);
    _wputenv_s(L"APPDATA", app_data.c_str());
    nso::ConfigManager config;
    config.update([&](nso::AppConfig& value) {
        value.session_token = h.session_token;
        value.destination_folder = nso::path_to_utf8(h.directory / "Captures");
    });
    nso::SyncEngine engine(config, h.coral, h.http);
    const auto first = engine.sync();
    const auto second = engine.sync();
    require(first.new_downloads == 3 && first.failure == nso::SyncFailure::none,
        "First sync downloads every capture (" + first.error_detail + ")");
    require(second.new_downloads == 0, "Second sync downloads nothing new");
    require(h.api.count("media:download") == 3, "Each capture is downloaded once");
    require(h.api.count("coral:Media/List") == 2, "One album listing per sync");
    require_compliant(h.api, "Sync");
}

// ---------------------------------------------------------------- soak run

// Same +/-2% spread as App's jittered_interval (src/app.cpp).
std::chrono::milliseconds jittered(std::chrono::milliseconds nominal) {
    static thread_local std::mt19937_64 generator(std::random_device{}());
    const auto jitter = std::max<std::int64_t>(1, nominal.count() / 50);
    std::uniform_int_distribution<std::int64_t> spread(-jitter, jitter);
    return std::chrono::milliseconds(nominal.count() + spread(generator));
}

struct SoakEvent {
    double at;
    std::string text;
};

struct Check {
    std::string name;
    std::string rule;
    std::string observed;
    bool pass;
};

std::string json_escape(const std::string& text) {
    return nso::Json(text).dump();
}

std::string html_escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string fixed(double value, int digits = 1) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

std::string redact(std::string text) {
    // Reports never contain tokens, even mock ones.
    static const std::regex token(R"((Bearer )\S+)");
    return std::regex_replace(text, token, "$1<redacted>");
}

void write_report(const Path& directory, double duration, const mock::MockNintendoApi& api,
    const std::vector<SoakEvent>& events, const std::vector<Check>& checks, const std::string& started) {
    std::filesystem::create_directories(directory);
    const auto requests = api.requests();
    const auto violations = api.violations();

    struct Stats {
        std::string host;
        std::size_t count = 0;
        std::size_t errors = 0;
        double first = -1, last = 0, min_gap = 1e9;
    };
    std::map<std::string, Stats> by_label;
    std::vector<std::string> order;
    for (const auto& request : requests) {
        auto& stats = by_label[request.label];
        if (stats.count == 0) {
            order.push_back(request.label);
            stats.host = request.host;
            stats.first = request.at;
        } else {
            stats.min_gap = std::min(stats.min_gap, request.at - stats.last);
        }
        stats.last = request.at;
        ++stats.count;
        if (request.status / 100 != 2) ++stats.errors;
    }
    std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        return by_label[a].host != by_label[b].host ? by_label[a].host < by_label[b].host : a < b;
    });
    const std::size_t nintendo = std::count_if(requests.begin(), requests.end(), [](const auto& r) {
        return r.host != mock::kNxapiAuth && r.host != mock::kNxapiZnca;
    });
    const std::size_t nxapi = requests.size() - nintendo;
    const bool all_pass = std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.pass; });

    // JSON (machine-readable, every request).
    {
        std::ofstream json(directory / "api-usage-report.json", std::ios::binary);
        json << "{\n  \"started\": " << json_escape(started) << ",\n  \"durationSeconds\": " << fixed(duration, 1)
             << ",\n  \"totalRequests\": " << requests.size() << ",\n  \"compliant\": " << (all_pass ? "true" : "false")
             << ",\n  \"checks\": [\n";
        for (std::size_t i = 0; i < checks.size(); ++i) {
            json << "    {\"name\": " << json_escape(checks[i].name) << ", \"rule\": " << json_escape(checks[i].rule)
                 << ", \"observed\": " << json_escape(checks[i].observed) << ", \"pass\": "
                 << (checks[i].pass ? "true" : "false") << "}" << (i + 1 < checks.size() ? "," : "") << "\n";
        }
        json << "  ],\n  \"violations\": [\n";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            json << "    {\"at\": " << fixed(violations[i].at, 2) << ", \"rule\": " << json_escape(violations[i].rule)
                 << ", \"detail\": " << json_escape(violations[i].detail) << "}" << (i + 1 < violations.size() ? "," : "") << "\n";
        }
        json << "  ],\n  \"events\": [\n";
        for (std::size_t i = 0; i < events.size(); ++i) {
            json << "    {\"at\": " << fixed(events[i].at, 2) << ", \"event\": " << json_escape(events[i].text) << "}"
                 << (i + 1 < events.size() ? "," : "") << "\n";
        }
        json << "  ],\n  \"requests\": [\n";
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& r = requests[i];
            json << "    {\"at\": " << fixed(r.at, 3) << ", \"host\": " << json_escape(r.host) << ", \"method\": "
                 << json_escape(r.method) << ", \"path\": " << json_escape(r.path) << ", \"endpoint\": "
                 << json_escape(r.label) << ", \"status\": " << r.status << ", \"ms\": " << fixed(r.duration_ms, 1)
                 << "}" << (i + 1 < requests.size() ? "," : "") << "\n";
        }
        json << "  ]\n}\n";
    }

    // HTML (for reading).
    std::ostringstream html;
    html << R"(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>API Usage Report</title>
<style>
:root{--bg:#fafaf9;--surface:#fff;--text:#1c1917;--muted:#57534e;--line:#e7e5e4;--pass:#15803d;--fail:#b91c1c;--accent:#e60012;--nx:#2563eb;--ev:#a16207}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#121212;--surface:#1c1c1c;--text:#f5f5f4;--muted:#a8a29e;--line:#333;--pass:#4ade80;--fail:#f87171;--accent:#ff4d5a;--nx:#60a5fa;--ev:#facc15}}
:root[data-theme="dark"]{--bg:#121212;--surface:#1c1c1c;--text:#f5f5f4;--muted:#a8a29e;--line:#333;--pass:#4ade80;--fail:#f87171;--accent:#ff4d5a;--nx:#60a5fa;--ev:#facc15}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1040px;margin:0 auto;padding:32px 16px 64px}
h1{font-size:26px;margin:0 0 4px}h2{font-size:18px;margin:36px 0 12px}
.muted{color:var(--muted)}.pass{color:var(--pass);font-weight:600}.fail{color:var(--fail);font-weight:600}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-top:20px}
.tile{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:14px}
.tile b{display:block;font-size:24px;font-variant-numeric:tabular-nums}
.scroll{overflow-x:auto;background:var(--surface);border:1px solid var(--line);border-radius:10px}
table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:8px 12px;border-bottom:1px solid var(--line);vertical-align:top}
th{font-weight:600;color:var(--muted)}td.n{text-align:right;font-variant-numeric:tabular-nums}
tr:last-child td{border-bottom:0}
svg text{fill:var(--muted);font-size:11px}
.banner{margin-top:16px;padding:12px 16px;border-radius:10px;border:1px solid var(--line);background:var(--surface)}
</style></head><body><main>
)";
    html << "<h1>Switch Album Sync API usage report</h1><p class=\"muted\">Soak run against the mock Nintendo / nxapi services, started "
         << html_escape(started) << ", " << fixed(duration / 60.0, 1) << " minutes. Build " NSO_APP_VERSION
         << ", X-znca-Client-Version " NSO_ZNCA_CLIENT_VERSION ".</p>";
    html << "<div class=\"banner\"><span class=\"" << (all_pass ? "pass\">All " : "fail\">Not all ") << checks.size()
         << " checks passed</span> · " << violations.size() << " terms violations recorded by the mock</div>";
    html << "<div class=\"tiles\"><div class=\"tile\"><b>" << requests.size() << "</b>requests in total</div>"
         << "<div class=\"tile\"><b>" << fixed(requests.size() * 3600.0 / duration, 0) << "</b>per hour at this rate</div>"
         << "<div class=\"tile\"><b>" << nxapi << "</b>to nxapi (" << fixed(nxapi * 60.0 / duration, 2) << "/min)</div>"
         << "<div class=\"tile\"><b>" << nintendo << "</b>to Nintendo (" << fixed(nintendo * 60.0 / duration, 2) << "/min)</div>"
         << "<div class=\"tile\"><b>" << api.max_nxapi_in_flight() << "</b>max concurrent nxapi requests</div></div>";

    html << "<h2>Compliance checks</h2><div class=\"scroll\"><table><tr><th>Check</th><th>Limit</th><th>Observed</th><th>Result</th></tr>";
    for (const auto& check : checks) {
        html << "<tr><td>" << html_escape(check.name) << "</td><td>" << html_escape(check.rule) << "</td><td>"
             << html_escape(check.observed) << "</td><td class=\"" << (check.pass ? "pass\">Pass" : "fail\">Fail") << "</td></tr>";
    }
    html << "</table></div>";

    // Timeline: one lane per endpoint, a tick per request.
    const int lane = 22, left = 210, width = 960, plot = width - left - 20;
    const int height = static_cast<int>(order.size()) * lane + 60;
    html << "<h2>Request timeline</h2><div class=\"scroll\"><svg viewBox=\"0 0 " << width << " " << height
         << "\" width=\"" << width << "\" height=\"" << height << "\" role=\"img\" aria-label=\"Requests over time per endpoint\">";
    for (int minute = 0; minute <= static_cast<int>(duration / 60); ++minute) {
        const double x = left + plot * (minute * 60.0 / duration);
        html << "<line x1=\"" << fixed(x) << "\" x2=\"" << fixed(x) << "\" y1=\"10\" y2=\"" << height - 30
             << "\" stroke=\"var(--line)\"/><text x=\"" << fixed(x) << "\" y=\"" << height - 12 << "\" text-anchor=\"middle\">"
             << minute << "m</text>";
    }
    for (const auto& event : events) {
        const double x = left + plot * (event.at / duration);
        html << "<line x1=\"" << fixed(x) << "\" x2=\"" << fixed(x) << "\" y1=\"10\" y2=\"" << height - 30
             << "\" stroke=\"var(--ev)\" stroke-dasharray=\"3 3\"><title>" << html_escape(event.text) << "</title></line>";
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int y = 24 + static_cast<int>(i) * lane;
        html << "<text x=\"" << left - 10 << "\" y=\"" << y + 4 << "\" text-anchor=\"end\">" << html_escape(order[i]) << "</text>";
        for (const auto& request : requests) {
            if (request.label != order[i]) continue;
            const double x = left + plot * (request.at / duration);
            const bool nx = request.host == mock::kNxapiAuth || request.host == mock::kNxapiZnca;
            html << "<rect x=\"" << fixed(x - 1.5) << "\" y=\"" << y - 7 << "\" width=\"3\" height=\"14\" rx=\"1\" fill=\""
                 << (request.status / 100 != 2 ? "var(--fail)" : nx ? "var(--nx)" : "var(--accent)") << "\"><title>"
                 << html_escape(request.label) << " at " << fixed(request.at, 1) << "s, HTTP " << request.status << "</title></rect>";
        }
    }
    html << "</svg></div><p class=\"muted\">Blue: nxapi. Red: Nintendo. Darker red: error responses. Dashed lines: scenario events (hover for details).</p>";

    html << "<h2>Requests by endpoint</h2><div class=\"scroll\"><table><tr><th>Endpoint</th><th>Host</th><th>Count</th>"
            "<th>Errors</th><th>Per hour</th><th>Shortest gap</th></tr>";
    for (const auto& label : order) {
        const auto& s = by_label[label];
        html << "<tr><td>" << html_escape(label) << "</td><td>" << html_escape(s.host) << "</td><td class=\"n\">" << s.count
             << "</td><td class=\"n\">" << s.errors << "</td><td class=\"n\">" << fixed(s.count * 3600.0 / duration, 1)
             << "</td><td class=\"n\">" << (s.count > 1 ? fixed(s.min_gap, 1) + " s" : "–") << "</td></tr>";
    }
    html << "</table></div>";

    html << "<h2>Scenario events</h2><div class=\"scroll\"><table><tr><th>Time</th><th>Event</th></tr>";
    for (const auto& event : events) {
        html << "<tr><td class=\"n\">" << fixed(event.at, 1) << " s</td><td>" << html_escape(event.text) << "</td></tr>";
    }
    html << "</table></div>";

    html << "<h2>Violations</h2>";
    if (violations.empty()) {
        html << "<p class=\"pass\">None. The mock checked every request against the rules above.</p>";
    } else {
        html << "<div class=\"scroll\"><table><tr><th>Time</th><th>Rule</th><th>Detail</th></tr>";
        for (const auto& v : violations) {
            html << "<tr><td class=\"n\">" << fixed(v.at, 1) << " s</td><td>" << html_escape(v.rule) << "</td><td>"
                 << html_escape(redact(v.detail)) << "</td></tr>";
        }
        html << "</table></div>";
    }

    html << "<h2>Method</h2><p>The real <code>HttpClient</code>, <code>NintendoAuthManager</code>, <code>NxapiClient</code>, "
            "<code>CoralClient</code> and <code>SyncEngine</code> ran unchanged, with HTTPS requests redirected to a loopback mock "
            "that answers like Nintendo Accounts, nxapi-auth, nxapi-znca-api, Coral and the media CDN. Two threads followed the "
            "app's own schedule: the Rich Presence loop (60 s while playing, stretching to 300 s after 3 offline polls, ±2% jitter, "
            "from <code>rpc.hpp</code>) and auto-sync (an initial sync, then every 15 minutes, the shortest interval the app allows). "
            "Tokens used the lifetimes of the real services where known (Nintendo 900 s, Coral 7200 s) and a short nxapi-auth "
            "lifetime so renewal happens during the run. Rules are from nxapi-znca-api <code>docs/public-api-terms.md</code> and "
            "<code>docs/api.md</code>.</p><p class=\"muted\">Not covered: the Win32 UI, Discord, and game-service enrichment "
            "(Splatoon, Animal Crossing, Zelda Notes), whose requests only run for those games.</p>";
    html << "</main></body></html>\n";
    std::ofstream(directory / "api-usage-report.html", std::ios::binary) << html.str();
}

int run_soak(double duration, const Path& report_directory) {
    mock::Options options;
    options.na_id = "na-soak";
    options.nxapi_token_ttl = 300;
    Harness h(options);
    for (int i = 0; i < 3; ++i) h.api.add_media({"soak-capture-" + std::to_string(i)});

    const auto app_data = h.directory / "AppData";
    std::filesystem::create_directories(app_data);
    _wputenv_s(L"APPDATA", app_data.c_str());
    nso::ConfigManager config;
    config.update([&](nso::AppConfig& value) {
        value.session_token = h.session_token;
        value.auto_sync = true;
        value.discord_presence = true;
        value.sync_interval_minutes = nso::kMinimumSyncIntervalMinutes;
        value.destination_folder = nso::path_to_utf8(h.directory / "Captures");
    });
    nso::SyncEngine engine(config, h.coral, h.http);

    const auto started_at = std::time(nullptr);
    char started[64];
    std::tm local{};
    localtime_s(&local, &started_at);
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", &local);

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool first_presence_done = false;
    std::vector<SoakEvent> events;
    std::vector<double> presence_polls;
    int syncs = 0, downloads = 0;
    const auto log = [&](const std::string& text) {
        std::lock_guard lock(mutex);
        events.push_back({h.api.now(), text});
        std::cout << "[" << fixed(h.api.now(), 0) << "s] " << text << std::endl;
    };
    const auto wait = [&](std::chrono::milliseconds interval) {
        std::unique_lock lock(mutex);
        return !wake.wait_for(lock, interval, [&] { return stopping; });
    };

    // Rich Presence loop, as App::presence_loop schedules it.
    std::thread presence([&] {
        int offline_polls = 0;
        bool playing = false;
        do {
            playing = false;
            try {
                playing = h.coral.self_presence(h.session_token).is_playing();
                std::lock_guard lock(mutex);
                presence_polls.push_back(h.api.now());
            } catch (const std::exception& error) {
                log(std::string("Presence poll failed: ") + error.what());
            }
            {
                std::lock_guard lock(mutex);
                first_presence_done = true;
            }
            wake.notify_all();
            offline_polls = playing ? 0 : std::min(offline_polls + 1, nso::kPresenceOfflineChecksBeforeBackoff);
        } while (wait(jittered(std::chrono::duration_cast<std::chrono::milliseconds>(
            nso::presence_poll_interval(playing, offline_polls)))));
    });

    // Auto-sync loop: the deferred startup sync, then the configured interval.
    std::thread sync([&] {
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || first_presence_done; });
            if (stopping) return;
        }
        do {
            const auto result = engine.sync();
            {
                std::lock_guard lock(mutex);
                ++syncs;
                downloads += result.new_downloads;
            }
            log("Auto-sync: " + std::to_string(result.total_found) + " captures listed, " +
                std::to_string(result.new_downloads) + " downloaded" +
                (result.failure != nso::SyncFailure::none ? ", failed: " + result.error_detail : ""));
        } while (wait(jittered(std::chrono::minutes(config.snapshot().sync_interval_minutes))));
    });

    // Scenario, scaled to the run length.
    const auto at = [&](double fraction, const std::function<void()>& action) {
        const auto target = duration * fraction;
        while (h.api.now() < target) {
            if (!wait(std::chrono::milliseconds(static_cast<int>(std::min(1000.0, (target - h.api.now()) * 1000) + 1)))) return;
        }
        action();
    };
    log("Started: playing Mock Kart, 3 captures in the album");
    // On a 10-minute run: 429 at 2.5 min while polling every 60 s, a new
    // capture at 4 min, then offline from 5.5 min so polling backs off.
    at(0.25, [&] {
        h.api.inject({mock::kNxapiZnca, "/api/znca/encrypt-request", 429, "90"});
        log("nxapi will answer the next request with 429, Retry-After: 90");
    });
    at(0.4, [&] { h.api.add_media({"soak-capture-3"}); log("New capture uploaded to Nintendo (picked up by the next scheduled sync)"); });
    at(0.55, [&] { h.api.set_playing(false); log("Player stopped playing (console offline)"); });
    at(1.0, [] {});
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    presence.join();
    sync.join();
    log("Stopped");

    // Checks against the published limits.
    const auto ran = h.api.now();
    std::vector<Check> checks;
    const auto add = [&](std::string name, std::string rule, std::string observed, bool pass) {
        checks.push_back({std::move(name), std::move(rule), std::move(observed), pass});
    };
    const auto violations = h.api.violations();
    add("Mock found no terms violations", "0 violations", std::to_string(violations.size()), violations.empty());
    add("One nxapi request at a time", "≤ 1 concurrent", std::to_string(h.api.max_nxapi_in_flight()),
        h.api.max_nxapi_in_flight() <= 1);
    const auto f1 = h.api.count("nxapi:f(method 1)");
    add("Coral login attestations (/f method 1)", "≤ 10 per 60 min", std::to_string(f1), f1 <= 10);
    const auto f2 = h.api.count("nxapi:f(method 2)");
    add("Web service attestations (/f method 2)", "≤ 20 per 30 min", std::to_string(f2), f2 <= 20);
    const auto nx_tokens = h.api.count("nxapi-auth:token");
    const auto nx_allowed = static_cast<std::size_t>(std::ceil(ran / options.nxapi_token_ttl)) + 1;
    add("nxapi-auth tokens cached for full validity", "≤ " + std::to_string(nx_allowed) + " for " +
        std::to_string(options.nxapi_token_ttl) + " s tokens", std::to_string(nx_tokens), nx_tokens <= nx_allowed);
    const auto na_tokens = h.api.count("nintendo:token");
    const auto na_allowed = static_cast<std::size_t>(std::ceil(ran / (options.nintendo_token_ttl - 10))) + 1;
    add("Nintendo tokens cached", "≤ " + std::to_string(na_allowed) + " for 900 s tokens", std::to_string(na_tokens),
        na_tokens <= na_allowed);
    const auto logins = h.api.count("coral:Account/Login");
    add("Coral sign-ins", "1 per 7200 s token", std::to_string(logins), logins == 1);
    add("/config cached", "1 per 6 h", std::to_string(h.api.count("nxapi:config")), h.api.count("nxapi:config") == 1);
    double min_gap = 1e9;
    for (std::size_t i = 1; i < presence_polls.size(); ++i) min_gap = std::min(min_gap, presence_polls[i] - presence_polls[i - 1]);
    const auto show_self = h.api.requests();
    double min_request_gap = 1e9, last = -1;
    for (const auto& r : show_self) {
        if (r.label != "coral:User/ShowSelf") continue;
        if (last >= 0) min_request_gap = std::min(min_request_gap, r.at - last);
        last = r.at;
    }
    add("Presence poll spacing", "≥ 58.8 s (60 s − 2% jitter)", presence_polls.size() > 1 ? fixed(min_request_gap) + " s shortest" : "n/a",
        min_request_gap >= 58.8);
    add("Album listings", "1 per sync", std::to_string(h.api.count("coral:Media/List")) + " for " + std::to_string(syncs) + " syncs",
        h.api.count("coral:Media/List") == static_cast<std::size_t>(syncs));
    add("Capture downloads", "each capture once", std::to_string(h.api.count("media:download")) + " for " +
        std::to_string(downloads) + " new captures", h.api.count("media:download") == static_cast<std::size_t>(downloads));
    std::size_t during_backoff = 0;
    double backoff_start = -1;
    for (const auto& r : h.api.requests()) {
        if (r.status == 429) backoff_start = r.at;
        else if (backoff_start >= 0 && r.at < backoff_start + 90 &&
                 (r.host == mock::kNxapiZnca || r.host == mock::kNxapiAuth)) ++during_backoff;
    }
    add("Retry-After honoured", "0 nxapi requests for 90 s after 429",
        backoff_start < 0 ? "no 429 seen" : std::to_string(during_backoff) + " requests", backoff_start >= 0 && during_backoff == 0);
    add("Only known services contacted", "Nintendo, nxapi, media CDN", violations_of(h.api, "Known hosts only").empty() ? "yes" : "no",
        violations_of(h.api, "Known hosts only").empty());

    write_report(report_directory, ran, h.api, events, checks, started);
    const bool pass = std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.pass; });
    for (const auto& check : checks) {
        std::cout << (check.pass ? "PASS  " : "FAIL  ") << check.name << ": " << check.observed << " (" << check.rule << ")\n";
    }
    std::cout << "Report: " << (report_directory / "api-usage-report.html").string() << "\n";
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    nso::SecureStore::use_namespace("Switch Album Sync API Tests");
    g_root = std::filesystem::temp_directory_path() / ("switch-album-sync-api-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(g_root);
    const auto previous_app_data = [] {
        wchar_t* value = nullptr;
        std::size_t size = 0;
        _wdupenv_s(&value, &size, L"APPDATA");
        std::wstring text = value ? value : L"";
        free(value);
        return text;
    }();

    int result = 0;
    try {
        double soak = 0;
        Path report = std::filesystem::current_path() / "api-usage-report";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--soak" && i + 1 < argc) soak = std::stod(argv[++i]);
            else if (arg == "--report" && i + 1 < argc) report = argv[++i];
        }
        if (soak > 0) {
            result = run_soak(soak, report);
        } else {
            const std::vector<std::pair<const char*, void (*)()>> tests{
                {"https requests reach the mock", https_requests_reach_the_mock},
                {"test credentials stay out of the real store", test_credentials_stay_out_of_the_real_store},
                {"presence polls reuse every token", presence_polls_reuse_every_token},
                {"Coral login is encrypted by its /f request", coral_login_is_encrypted_by_its_f_request},
                {"web service token is encrypted by its /f request", web_service_token_is_encrypted_by_its_f_request},
                {"sign-in falls back when /f doesn't encrypt", sign_in_falls_back_when_f_does_not_encrypt},
                {"nxapi requests carry required headers", nxapi_requests_carry_required_headers},
                {"nxapi never runs two requests at once", nxapi_never_runs_two_requests_at_once},
                {"nxapi Retry-After is honoured", nxapi_retry_after_is_honoured},
                {"nxapi-auth token is refreshed, not reissued", nxapi_auth_token_is_refreshed_not_reissued},
                {"Coral errors are not retried", coral_errors_are_not_retried},
                {"web service tokens are cached", web_service_tokens_are_cached},
                {"Coral login attestations stay under the limit", coral_login_attestations_stay_under_limit},
                {"sync downloads each capture once", sync_downloads_each_capture_once},
            };
            for (const auto& [name, test] : tests) {
                try {
                    test();
                    std::cout << "PASS  " << name << "\n";
                } catch (const std::exception& error) {
                    std::cout << "FAIL  " << name << ": " << error.what() << "\n";
                    result = 1;
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        result = 1;
    }
    _wputenv_s(L"APPDATA", previous_app_data.c_str());
    std::error_code ignored;
    std::filesystem::remove_all(g_root, ignored);
    return result;
}
