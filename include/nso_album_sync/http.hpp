#pragma once

#include <atomic>
#include <cstddef>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nso {

inline constexpr std::size_t kDefaultGetResponseLimit =
    256ULL * 1024ULL * 1024ULL;

class TransientHttpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Requests sent today (local time), by destination, for Copy diagnostics, so
// users can see how lightly the app uses Nintendo's and nxapi's services.
// Counted in memory only; nothing about the requests themselves is kept.
struct RequestCounts {
    int nintendo = 0;
    int nxapi = 0;
};

namespace detail {
inline std::mutex& request_count_mutex() {
    static std::mutex mutex;
    return mutex;
}
struct RequestCountState {
    int day = -1;  // tm_yday + 1000 * tm_year
    RequestCounts counts;
};
inline RequestCountState& request_count_state() {
    static RequestCountState state;
    return state;
}
inline int local_day_key() {
    const auto now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) != 0) return 0;
    return local.tm_yday + 1000 * local.tm_year;
}
inline bool host_ends_with(const std::string& host, const char* suffix) {
    const std::string tail(suffix);
    return host == tail.substr(1) ||
        (host.size() > tail.size() && host.compare(host.size() - tail.size(), tail.size(), tail) == 0);
}
}  // namespace detail

inline void count_request(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return;
    const auto start = scheme + 3;
    const auto end = url.find_first_of(":/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (auto& character : host) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    const bool nintendo = detail::host_ends_with(host, ".nintendo.net") ||
        detail::host_ends_with(host, ".nintendo.com");
    const bool nxapi = detail::host_ends_with(host, ".fancy.org.uk");
    if (!nintendo && !nxapi) return;
    std::lock_guard lock(detail::request_count_mutex());
    auto& state = detail::request_count_state();
    const auto day = detail::local_day_key();
    if (state.day != day) {
        state.day = day;
        state.counts = {};
    }
    ++(nintendo ? state.counts.nintendo : state.counts.nxapi);
}

inline RequestCounts requests_today() {
    std::lock_guard lock(detail::request_count_mutex());
    auto& state = detail::request_count_state();
    if (state.day != detail::local_day_key()) return {};
    return state.counts;
}

struct HttpResponse {
    long status = 0;
    std::vector<unsigned char> body;
    std::map<std::string, std::string> headers;

    std::string text() const {
        return std::string(body.begin(), body.end());
    }
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    void set_proxy(std::string proxy) { proxy_ = std::move(proxy); }
    std::string proxy_url() const { return static_cast<std::string>(proxy_); }

    // Tests only: send every https://<host>/<path> request to
    // http://127.0.0.1:<port>/<host>/<path> instead, so a local mock can stand
    // in for Nintendo and nxapi. 0 (the default) turns it off.
    void redirect_to_loopback(unsigned short port) { loopback_port_ = port; }

    HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers = {},
        const std::vector<unsigned char>& body = {},
        const std::string& content_type = "",
        long timeout_seconds = 30,
        std::size_t max_response_bytes = 0) const;

    HttpResponse get(
        const std::string& url,
        const std::vector<std::string>& headers = {},
        long timeout_seconds = 30,
        std::size_t max_response_bytes = kDefaultGetResponseLimit) const;

    HttpResponse post(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers = {},
        const std::string& content_type = "application/json",
        long timeout_seconds = 30) const;

    HttpResponse post_bytes(
        const std::string& url,
        const std::vector<unsigned char>& body,
        const std::vector<std::string>& headers = {},
        const std::string& content_type = "application/octet-stream",
        long timeout_seconds = 30) const;

    static std::string form_encode(
        const std::map<std::string, std::string>& values);

private:
    class SynchronizedString {
    public:
        SynchronizedString() = default;
        SynchronizedString(const SynchronizedString&) = delete;
        SynchronizedString& operator=(const SynchronizedString&) = delete;

        SynchronizedString& operator=(std::string value) {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            return *this;
        }

        bool empty() const {
            std::lock_guard lock(mutex_);
            return value_.empty();
        }

        operator std::string() const {
            std::lock_guard lock(mutex_);
            return value_;
        }

    private:
        mutable std::mutex mutex_;
        std::string value_;
    };

    SynchronizedString proxy_;
    std::atomic<unsigned short> loopback_port_{0};

    // One WinHTTP session keeps TLS connections alive between requests. It is
    // rebuilt when the proxy changes; requests already running keep their own
    // reference to the previous session until they finish.
    std::shared_ptr<void> session() const;
    mutable std::mutex session_mutex_;
    mutable std::shared_ptr<void> session_;
    mutable std::string session_proxy_;
};

}  // namespace nso
