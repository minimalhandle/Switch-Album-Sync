#pragma once

#include <atomic>
#include <cstddef>
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
