#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/version.hpp"
#include "nso_album_sync/windows_compat.hpp"
#include <winhttp.h>
#include <cwchar>

namespace nso {

struct ServerSentEvent {
    std::string event;
    std::string data;
    std::string id;
    std::int64_t retry_milliseconds = -1;
};

class ServerSentEventParser {
public:
    using EventCallback = std::function<bool(const ServerSentEvent&)>;

    explicit ServerSentEventParser(std::size_t max_event_bytes = 1024 * 1024)
        : max_event_bytes_(max_event_bytes) {
        if (max_event_bytes_ == 0) {
            throw std::invalid_argument("SSE event size limit must be greater than zero");
        }
    }

    bool feed(const unsigned char* data, std::size_t size, const EventCallback& on_event) {
        if (size != 0 && data == nullptr) {
            throw std::invalid_argument("SSE input buffer is null");
        }
        if (!on_event) {
            throw std::invalid_argument("SSE event callback is empty");
        }

        for (std::size_t index = 0; index < size; ++index) {
            const char character = static_cast<char>(data[index]);
            if (pending_cr_) {
                pending_cr_ = false;
                if (!process_line(on_event)) return false;
                if (character == '\n') continue;
            }

            if (character == '\r') {
                pending_cr_ = true;
            } else if (character == '\n') {
                if (!process_line(on_event)) return false;
            } else {
                line_.push_back(character);
                enforce_limit(line_.size());
            }
        }
        return true;
    }

    bool feed(const std::string& data, const EventCallback& on_event) {
        return feed(
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(), on_event);
    }

    bool finish(const EventCallback& on_event) {
        if (!on_event) {
            throw std::invalid_argument("SSE event callback is empty");
        }
        if (pending_cr_) {
            pending_cr_ = false;
            if (!process_line(on_event)) return false;
        } else if (!line_.empty()) {
            if (!process_line(on_event)) return false;
        }
        line_.clear();
        reset_event_fields();
        return true;
    }

    void reset() {
        line_.clear();
        data_.clear();
        event_.clear();
        last_event_id_.clear();
        retry_milliseconds_ = -1;
        pending_cr_ = false;
    }

private:
    void enforce_limit(std::size_t pending_size) const {
        if (pending_size > max_event_bytes_ ||
            data_.size() > max_event_bytes_ - pending_size) {
            throw std::runtime_error("SSE event exceeded configured size limit");
        }
    }

    static bool valid_retry(const std::string& value) {
        if (value.empty()) return false;
        for (const unsigned char character : value) {
            if (!std::isdigit(character)) return false;
        }
        return true;
    }

    bool process_line(const EventCallback& on_event) {
        std::string line;
        line.swap(line_);
        if (line.empty()) {
            if (data_.empty()) {
                reset_event_fields();
                return true;
            }
            if (data_.back() == '\n') data_.pop_back();
            ServerSentEvent parsed;
            parsed.event = event_.empty() ? "message" : event_;
            parsed.data = std::move(data_);
            parsed.id = last_event_id_;
            parsed.retry_milliseconds = retry_milliseconds_;
            reset_event_fields();
            return on_event(parsed);
        }

        if (line.front() == ':') return true;
        const auto separator = line.find(':');
        const auto field = separator == std::string::npos
            ? line : line.substr(0, separator);
        auto value = separator == std::string::npos
            ? std::string{} : line.substr(separator + 1);
        if (!value.empty() && value.front() == ' ') value.erase(value.begin());

        if (field == "data") {
            enforce_limit(value.size() + 1);
            data_ += value;
            data_.push_back('\n');
        } else if (field == "event") {
            enforce_limit(value.size());
            event_ = std::move(value);
        } else if (field == "id") {
            if (value.find('\0') == std::string::npos) {
                enforce_limit(value.size());
                last_event_id_ = std::move(value);
            }
        } else if (field == "retry" && valid_retry(value)) {
            try {
                retry_milliseconds_ = std::stoll(value);
            } catch (...) {
            }
        }
        return true;
    }

    void reset_event_fields() {
        data_.clear();
        event_.clear();
        retry_milliseconds_ = -1;
    }

    std::size_t max_event_bytes_;
    std::string line_;
    std::string data_;
    std::string event_;
    std::string last_event_id_;
    std::int64_t retry_milliseconds_ = -1;
    bool pending_cr_ = false;
};

struct SseResponse {
    long status = 0;
    std::map<std::string, std::string> headers;
};

class SseClient {
public:
    using EventCallback = ServerSentEventParser::EventCallback;
    using CancelCallback = std::function<bool()>;

    SseClient() = default;
    explicit SseClient(std::string proxy_url) : proxy_url_(std::move(proxy_url)) {}

    void set_proxy(std::string proxy_url) {
        std::lock_guard lock(proxy_mutex_);
        proxy_url_ = std::move(proxy_url);
    }

    SseResponse stream(
        const std::string& url,
        const std::vector<std::string>& headers,
        const EventCallback& on_event,
        const CancelCallback& should_cancel = {},
        long connect_timeout_seconds = 30,
        std::size_t max_event_bytes = 1024 * 1024) const;

private:
    std::string proxy() const {
        std::lock_guard lock(proxy_mutex_);
        return proxy_url_;
    }

    mutable std::mutex proxy_mutex_;
    std::string proxy_url_;
};

namespace sse_detail {

constexpr std::size_t kReadBufferBytes = 16 * 1024;
constexpr long kReadPollSeconds = 1;

inline bool cancelled(const SseClient::CancelCallback& should_cancel) {
    return should_cancel && should_cancel();
}

inline std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return text;
}

inline std::string trim_left(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    return text;
}

inline long safe_timeout(long timeout_seconds) {
    return std::clamp(timeout_seconds, 1L, 24L * 60L * 60L);
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

inline std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("SSE string is too large");
    }
    const int length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (required <= 0) throw std::runtime_error("Invalid UTF-8 string");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
            result.data(), required) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

inline std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("SSE string is too large");
    }
    const int length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

[[noreturn]] inline void throw_winhttp_error(const char* operation) {
    const auto code = GetLastError();
    const auto message = std::string(operation) + " failed (WinHTTP error " +
        std::to_string(code) + ")";
    // Same classification as HttpClient: only a connection that failed or
    // dropped without a reply may be retried automatically.
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            throw TransientHttpError(message);
        default:
            throw std::runtime_error(message);
    }
}

inline ParsedUrl parse_url(const std::string& url) {
    const auto wide_url = utf8_to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            wide_url.c_str(), static_cast<DWORD>(wide_url.size()),
            ICU_REJECT_USERPWD, &parts)) {
        throw_winhttp_error("WinHttpCrackUrl");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Only HTTP(S) SSE URLs are supported");
    }
    ParsedUrl parsed;
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    parsed.port = parts.nPort;
    parsed.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    parsed.path = parts.dwUrlPathLength == 0
        ? L"/" : std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength != 0) {
        parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    return parsed;
}

inline std::wstring proxy_server_from_url(const std::string& proxy_url) {
    const auto parsed = parse_url(proxy_url);
    if (parsed.secure) throw std::runtime_error("Only http:// proxies are supported");
    std::wstring proxy = parsed.host;
    if (parsed.port != INTERNET_DEFAULT_HTTP_PORT) {
        proxy += L":" + std::to_wstring(parsed.port);
    }
    return proxy;
}

inline std::wstring build_headers(const std::vector<std::string>& headers) {
    std::wstring result =
        L"Connection: keep-alive\r\nAccept-Encoding: identity\r\n";
    for (const auto& header : headers) {
        result += utf8_to_wide(header);
        result += L"\r\n";
    }
    return result;
}

inline std::map<std::string, std::string> read_headers(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
            raw.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders");
    }
    raw.resize(wcslen(raw.c_str()));
    std::istringstream lines(wide_to_utf8(raw));
    std::map<std::string, std::string> result;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        const auto name = lowercase(line.substr(0, separator));
        const auto value = trim_left(line.substr(separator + 1));
        const auto existing = result.find(name);
        if (name == "set-cookie" && existing != result.end()) {
            existing->second += "\n" + value;
        } else {
            result[name] = value;
        }
    }
    return result;
}

inline SseResponse stream_windows(
    const std::string& url,
    const std::string& proxy_url,
    const std::vector<std::string>& headers,
    const SseClient::EventCallback& on_event,
    const SseClient::CancelCallback& should_cancel,
    long connect_timeout_seconds,
    std::size_t max_event_bytes) {
    if (cancelled(should_cancel)) return {};
    const auto destination = parse_url(url);

    // With no proxy configured, follow the Windows proxy settings.
    std::wstring proxy;
    DWORD access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    LPCWSTR proxy_name = WINHTTP_NO_PROXY_NAME;
    if (!proxy_url.empty()) {
        proxy = proxy_server_from_url(proxy_url);
        access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy_name = proxy.c_str();
    }

    InternetHandle session(WinHttpOpen(
        L"Switch Album Sync/" NSO_APP_VERSION, access_type, proxy_name,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw_winhttp_error("WinHttpOpen");

    const long timeout = safe_timeout(connect_timeout_seconds);
    const long max_seconds = (std::numeric_limits<int>::max)() / 1000L;
    const int timeout_ms = static_cast<int>(std::min(timeout, max_seconds) * 1000L);
    if (!WinHttpSetTimeouts(
            session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms)) {
        throw_winhttp_error("WinHttpSetTimeouts");
    }

    InternetHandle connection(WinHttpConnect(
        session.get(), destination.host.c_str(), destination.port, 0));
    if (!connection) throw_winhttp_error("WinHttpConnect");

    const DWORD flags = destination.secure ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", destination.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) throw_winhttp_error("WinHttpOpenRequest");

    const auto additional_headers = build_headers(headers);
    if (!WinHttpSendRequest(
            request.get(), additional_headers.c_str(),
            static_cast<DWORD>(additional_headers.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        throw_winhttp_error("WinHttpSendRequest");
    }
    if (cancelled(should_cancel)) return {};
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders(status)");
    }

    SseResponse response;
    response.status = static_cast<long>(status);
    response.headers = read_headers(request.get());

    DWORD receive_timeout = static_cast<DWORD>(kReadPollSeconds * 1000L);
    if (!WinHttpSetOption(
            request.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT,
            &receive_timeout, sizeof(receive_timeout))) {
        throw_winhttp_error("WinHttpSetOption(receive timeout)");
    }

    ServerSentEventParser parser(max_event_bytes);
    std::array<unsigned char, kReadBufferBytes> buffer{};
    for (;;) {
        if (cancelled(should_cancel)) return response;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) continue;
            throw_winhttp_error("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            parser.finish(on_event);
            return response;
        }

        while (available > 0) {
            if (cancelled(should_cancel)) return response;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                available, buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), requested, &read)) {
                if (GetLastError() == ERROR_WINHTTP_TIMEOUT) break;
                throw_winhttp_error("WinHttpReadData");
            }
            if (read == 0) {
                parser.finish(on_event);
                return response;
            }
            if (!parser.feed(buffer.data(), read, on_event)) return response;
            available -= std::min(available, read);
        }
    }
}


}  // namespace sse_detail

inline SseResponse SseClient::stream(
    const std::string& url,
    const std::vector<std::string>& headers,
    const EventCallback& on_event,
    const CancelCallback& should_cancel,
    long connect_timeout_seconds,
    std::size_t max_event_bytes) const {
    if (!on_event) throw std::invalid_argument("SSE event callback is empty");
    count_request(url);
    return sse_detail::stream_windows(
        url, proxy(), headers, on_event, should_cancel,
        connect_timeout_seconds, max_event_bytes);
}

}  // namespace nso
