#include "nso_album_sync/sync.hpp"
#include "nso_album_sync/path.hpp"

#include "nso_album_sync/game_aliases.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/util.hpp"

#include "import_windows.hpp"
#include "nso_album_sync/windows_compat.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nso {
namespace {

constexpr std::int64_t kMaxMediaDownloadBytes = 256LL * 1024LL * 1024LL;

struct ExistingAlbumIndex {
    std::set<std::string> filenames_and_prefixes;
    std::unordered_map<std::string, std::string> folder_by_timestamp_prefix;
    std::unordered_map<std::string, std::filesystem::path> device_capture_by_key;
};

bool is_workspace(const std::filesystem::path& path) {
    const auto name = path_to_utf8(path.filename());
    return name.starts_with(".switch-album-sync-import-") || name.starts_with(".switch-album-sync-share-");
}

bool should_cancel(const std::function<bool()>& cancelled) {
    return cancelled && cancelled();
}

void throw_if_cancelled(const std::function<bool()>& cancelled) {
    if (should_cancel(cancelled)) throw std::runtime_error("Sync cancelled");
}

bool ends_with_ascii(const std::string& value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool safe_media_url(const std::string& url) {
    constexpr std::string_view kHttpsPrefix = "https://";
    if (url.size() <= kHttpsPrefix.size() ||
        url.compare(0, kHttpsPrefix.size(), kHttpsPrefix) != 0) {
        return false;
    }

    const auto authority_start = kHttpsPrefix.size();
    const auto authority_end = url.find_first_of("/?#", authority_start);
    auto authority = url.substr(
        authority_start,
        authority_end == std::string::npos
            ? std::string::npos
            : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;

    // Nintendo media should be addressed by a public HTTPS hostname. Reject
    // literal IPs and non-standard ports so a corrupted upstream response
    // cannot turn the downloader into a convenient localhost/LAN fetcher.
    if (authority.front() == '[') return false;
    if (const auto port = authority.rfind(':'); port != std::string::npos) {
        if (authority.substr(port + 1) != "443") return false;
        authority.resize(port);
    }
    if (authority.empty()) return false;

    auto host = lower(std::move(authority));
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty() || host.find('.') == std::string::npos) return false;

    bool could_be_ipv4 = true;
    for (const unsigned char character : host) {
        if (!std::isdigit(character) && character != '.') {
            could_be_ipv4 = false;
            break;
        }
    }
    if (could_be_ipv4) return false;

    for (const unsigned char character : host) {
        if (!std::isalnum(character) && character != '-' && character != '.') {
            return false;
        }
    }

    return host != "localhost" &&
           !ends_with_ascii(host, ".localhost") &&
           !ends_with_ascii(host, ".local") &&
           !ends_with_ascii(host, ".localdomain") &&
           !ends_with_ascii(host, ".internal") &&
           !ends_with_ascii(host, ".lan") &&
           !ends_with_ascii(host, ".home");
}

void validate_media_item_for_download(const MediaItem& item) {
    if (!safe_media_url(item.content_uri)) {
        throw std::runtime_error(
            "Nintendo media download URL was rejected because it is not a safe public HTTPS URL");
    }
    if (item.content_length <= 0 || item.content_length > kMaxMediaDownloadBytes) {
        throw std::runtime_error(
            "Nintendo media download size is missing or exceeds the 256 MiB safety limit");
    }
}

std::int64_t media_timestamp(const MediaItem& item) {
    return item.captured_at != 0 ? item.captured_at : item.uploaded_at;
}

std::string media_retry_id(const MediaItem& item) {
    if (!item.id.empty()) return "id:" + item.id;
    const auto timestamp = media_timestamp(item);
    if (timestamp <= 0) return {};
    return "capture:" + std::to_string(timestamp) + ":" + item.type + ":" +
           (item.title_id.empty() ? item.app_name : item.title_id);
}

std::vector<unsigned char> download_capture(
    HttpClient& http, const MediaItem& item, const std::string& user_agent) {
    const auto size = static_cast<std::size_t>(item.content_length);
    std::vector<std::string> headers;
    if (!user_agent.empty()) headers.push_back("User-Agent: " + user_agent);
    auto response = http.get(item.content_uri, headers, 60);
    // Only network failures, where no response arrived (HttpClient raises
    // TransientHttpError), are retried automatically. Once Nintendo answers
    // with an error, the app waits for the user to retry instead of
    // re-requesting on its own.
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Media download failed (HTTP " + std::to_string(response.status) + ")");
    }
    if (response.body.size() != size) {
        throw std::runtime_error("Media download size did not match Nintendo's content length");
    }
    return std::move(response.body);
}

void write_capture(
    const std::filesystem::path& destination,
    const std::vector<unsigned char>& body,
    const std::function<bool()>& cancelled) {
    auto temporary = destination;
    // A user can deliberately point both apps at the same album.
    temporary += ".switch-album-sync.part";
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (cleanup_error) throw std::runtime_error("Could not remove a partial media file");

    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Could not create media file");
        file.write(reinterpret_cast<const char*>(body.data()),
                   static_cast<std::streamsize>(body.size()));
        file.close();
        if (!file) throw std::runtime_error("Could not write media file");

        throw_if_cancelled(cancelled);
        std::filesystem::rename(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::pair<std::uint32_t, std::size_t> decode_utf8(
    const std::string& text,
    std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};

    auto continuation = [&](std::size_t index) -> int {
        if (index >= text.size()) return -1;
        const auto byte = static_cast<unsigned char>(text[index]);
        return (byte & 0xc0) == 0x80 ? (byte & 0x3f) : -1;
    };

    if ((first & 0xe0) == 0xc0) {
        const int b1 = continuation(offset + 1);
        if (b1 >= 0) return {((first & 0x1fU) << 6U) | static_cast<unsigned>(b1), 2};
    } else if ((first & 0xf0) == 0xe0) {
        const int b1 = continuation(offset + 1);
        const int b2 = continuation(offset + 2);
        if (b1 >= 0 && b2 >= 0) {
            return {((first & 0x0fU) << 12U) |
                        (static_cast<unsigned>(b1) << 6U) |
                        static_cast<unsigned>(b2),
                    3};
        }
    } else if ((first & 0xf8) == 0xf0) {
        const int b1 = continuation(offset + 1);
        const int b2 = continuation(offset + 2);
        const int b3 = continuation(offset + 3);
        if (b1 >= 0 && b2 >= 0 && b3 >= 0) {
            return {((first & 0x07U) << 18U) |
                        (static_cast<unsigned>(b1) << 12U) |
                        (static_cast<unsigned>(b2) << 6U) |
                        static_cast<unsigned>(b3),
                    4};
        }
    }
    return {first, 1};
}

std::string normalize_for_matching_v1(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (std::size_t offset = 0; offset < text.size();) {
        const auto [code_point, width] = decode_utf8(text, offset);
        offset += width;

        if (code_point >= 0x0300 && code_point <= 0x036f) continue;

        if (code_point < 0x80) {
            const auto character = static_cast<unsigned char>(code_point);
            if (std::isalnum(character)) {
                normalized.push_back(static_cast<char>(std::tolower(character)));
            }
            continue;
        }

        char latin = '\0';
        switch (code_point) {
            case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x00c4: case 0x00c5:
            case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x00e4: case 0x00e5:
                latin = 'a'; break;
            case 0x00c7: case 0x00e7: latin = 'c'; break;
            case 0x00c8: case 0x00c9: case 0x00ca: case 0x00cb:
            case 0x00e8: case 0x00e9: case 0x00ea: case 0x00eb:
                latin = 'e'; break;
            case 0x00cc: case 0x00cd: case 0x00ce: case 0x00cf:
            case 0x00ec: case 0x00ed: case 0x00ee: case 0x00ef:
                latin = 'i'; break;
            case 0x00d1: case 0x00f1: latin = 'n'; break;
            case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x00d6: case 0x00d8:
            case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x00f6: case 0x00f8:
                latin = 'o'; break;
            case 0x00d9: case 0x00da: case 0x00db: case 0x00dc:
            case 0x00f9: case 0x00fa: case 0x00fb: case 0x00fc:
                latin = 'u'; break;
            case 0x00dd: case 0x0178: case 0x00fd: case 0x00ff:
                latin = 'y'; break;
            default: break;
        }
        if (latin != '\0') {
            normalized.push_back(latin);
            continue;
        }

        // .NET v1 kept non-Latin letters/digits after Unicode decomposition.
        // Preserve their UTF-8 representation so exact Japanese and other
        // localized aliases continue to compare instead of collapsing to empty.
        append_utf8(normalized, code_point);
    }
    return normalized;
}

bool is_unicode_dash_or_hyphen(std::uint32_t cp) {
    switch (cp) {
        case 0x2010:  // Hyphen
        case 0x2011:  // Non-breaking hyphen
        case 0x2012:  // Figure dash
        case 0x2013:  // En dash
        case 0x2014:  // Em dash
        case 0x2015:  // Horizontal bar
        case 0x2212:  // Minus sign
        case 0xFE58:  // Small em dash
        case 0xFE63:  // Small hyphen-minus
        case 0xFF0D:  // Fullwidth hyphen-minus
            return true;
        default:
            return false;
    }
}

std::string sanitize_folder_v1(const std::string& text) {
    if (trim(text).empty()) return "Other";
    std::string clean;
    clean.reserve(text.size());

    for (std::size_t offset = 0; offset < text.size();) {
        const auto [code_point, width] = decode_utf8(text, offset);
        offset += width;

        if (code_point < 0x20) continue;

        if (is_unicode_dash_or_hyphen(code_point)) {
            clean.push_back('-');
            continue;
        }

        switch (code_point) {
            case '<': case '>': case ':': case '"': case '/':
            case '\\': case '|': case '?': case '*':
            case 0xFF1C: case 0xFF1E: case 0xFF1A: case 0xFF02: case 0xFF0F:
            case 0xFF3C: case 0xFF5C: case 0xFF1F: case 0xFF0A:
                continue;
            default:
                append_utf8(clean, code_point);
                break;
        }
    }

    std::string result;
    result.reserve(clean.size());
    bool last_was_space = false;
    for (char c : clean) {
        if (c == ' ') {
            if (!last_was_space && !result.empty()) {
                result.push_back(' ');
                last_was_space = true;
            }
        } else {
            result.push_back(c);
            last_was_space = false;
        }
    }

    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    result = trim(std::move(result));
    return result.empty() ? "Other" : result;
}

bool equal_v1_name(const std::string& left, const std::string& right) {
    return lower(left) == lower(right);
}

std::string capture_timestamp_prefix(std::int64_t timestamp) {
    if (timestamp <= 0) timestamp = unix_now();
    const auto seconds = timestamp > 10'000'000'000LL ? timestamp / 1000 : timestamp;
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local_time{};
    localtime_s(&local_time, &time);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &local_time);
    return std::string(buffer) + "00";
}

std::string prefix_from_existing_filename(const std::filesystem::path& path) {
    std::string prefix = path_to_utf8(path.stem());
    if (prefix.size() == 49 && prefix[16] == '-' &&
        std::all_of(prefix.begin(), prefix.begin() + 16, [](char ch) { return ch >= '0' && ch <= '9'; }) &&
        std::all_of(prefix.begin() + 17, prefix.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        prefix.resize(16);
    } else if (prefix.size() > 2 && prefix.ends_with("_c")) {
        prefix.resize(prefix.size() - 2);
    } else if (prefix.size() > 3 && prefix.ends_with("-00")) {
        prefix.resize(prefix.size() - 3);
    }
    return prefix;
}

ExistingAlbumIndex index_existing_album(
    const std::filesystem::path& root,
    const std::function<bool()>& cancelled) {
    ExistingAlbumIndex index;
    const bool flat_default = is_default_album_folder(root);
    for (auto iterator = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied);
         iterator != std::filesystem::recursive_directory_iterator();
         ++iterator) {
        throw_if_cancelled(cancelled);
        if (iterator->is_directory() && is_workspace(iterator->path())) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file()) continue;

        const auto filename_lower = lower(path_to_utf8(iterator->path().filename()));
        if (filename_lower.ends_with(".part") || filename_lower.ends_with(".tmp")) {
            continue;
        }

        std::error_code error;
        if (iterator->file_size(error) == 0) continue;
        const auto& path = iterator->path();
        const auto filename = path_to_utf8(path.filename());
        const auto prefix = prefix_from_existing_filename(path);
        if (prefix.size() == 16 && filename.size() >= 53 && filename[16] == '-') {
            auto extension = lower(path_to_utf8(path.extension()));
            if (extension == ".jpeg") extension = ".jpg";
            index.device_capture_by_key.try_emplace(prefix.substr(0, 14) + extension, path);
        }
        index.filenames_and_prefixes.insert(lower(filename));
        index.filenames_and_prefixes.insert(lower(prefix));
        const auto parent = path.parent_path();
        if (!flat_default || (parent != root && parent != root / "Album")) {
            index.folder_by_timestamp_prefix.emplace(
                lower(prefix), path_to_utf8(parent.filename()));
        }
    }
    return index;
}

std::unordered_map<std::string, std::string> learn_title_folders(
    const std::vector<MediaItem>& media,
    const ExistingAlbumIndex& existing) {
    std::unordered_map<std::string, std::string> folders;
    for (const auto& item : media) {
        if (item.title_id.empty()) continue;
        const auto timestamp = item.captured_at != 0 ? item.captured_at : item.uploaded_at;
        const auto prefix = capture_timestamp_prefix(timestamp);
        const auto existing_folder =
            existing.folder_by_timestamp_prefix.find(lower(prefix));
        if (existing_folder != existing.folder_by_timestamp_prefix.end()) {
            folders[lower(item.title_id)] = existing_folder->second;
        }
    }
    return folders;
}

void preserve_capture_timestamp(
    const std::filesystem::path& path,
    std::int64_t timestamp) {
    if (timestamp <= 0) return;
    try {
        const auto seconds_value =
            timestamp > 10'000'000'000LL ? timestamp / 1000 : timestamp;
        const auto seconds = std::chrono::seconds(seconds_value);
        const auto capture_time = std::chrono::system_clock::time_point(seconds);
        constexpr std::uint64_t kUnixToWindowsEpochSeconds = 11'644'473'600ULL;
        if (seconds_value >= 0) {
            ULARGE_INTEGER raw_time{};
            raw_time.QuadPart =
                (static_cast<std::uint64_t>(seconds_value) +
                 kUnixToWindowsEpochSeconds) * 10'000'000ULL;
            FILETIME file_time{};
            file_time.dwLowDateTime = raw_time.LowPart;
            file_time.dwHighDateTime = raw_time.HighPart;
            HANDLE file = CreateFileW(
                path.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                SetFileTime(file, &file_time, nullptr, &file_time);
                CloseHandle(file);
            }
        }
        const auto file_time =
            std::filesystem::file_time_type::clock::now() +
            (capture_time - std::chrono::system_clock::now());
        std::filesystem::last_write_time(path, file_time);
    } catch (...) {
    }
}

constexpr std::int64_t kReceiptRetentionSeconds = 7LL * 24 * 60 * 60;
// Used when Nintendo doesn't report when a capture expires from its album.
constexpr std::int64_t kReceiptFallbackLifetimeSeconds = 90LL * 24 * 60 * 60;

std::int64_t unix_seconds(std::int64_t value) {
    return value > 10'000'000'000LL ? value / 1000 : value;
}

std::string receipt_album_key(const std::filesystem::path& album) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(album, error);
    if (error) absolute = album;
    return lower(path_to_utf8(absolute.lexically_normal().make_preferred()));
}

}  // namespace

bool is_safe_media_url(const std::string& url) {
    return safe_media_url(url);
}

std::string capture_filename_prefix(std::int64_t timestamp) {
    return capture_timestamp_prefix(timestamp);
}

SyncReceipts::SyncReceipts(std::filesystem::path file, const std::filesystem::path& album)
    : file_(std::move(file)), album_key_(receipt_album_key(album)) {
    const auto saved = read_local_data(file_);
    if (saved.is_object()) {
        if (const auto* albums = saved.find("albums"); albums && albums->is_object()) {
            albums_ = albums->as_object();
        }
    }
    // Drop receipts a week after Nintendo's copy expired; the capture can't
    // come back, so the entry is no longer needed.
    const auto now = unix_now();
    for (auto album_entry = albums_.begin(); album_entry != albums_.end();) {
        if (!album_entry->second.is_object()) {
            album_entry = albums_.erase(album_entry);
            dirty_ = true;
            continue;
        }
        auto entries = album_entry->second.as_object();
        const auto before = entries.size();
        std::erase_if(entries, [now](const auto& entry) {
            return !entry.second.is_number() || entry.second.as_i64() + kReceiptRetentionSeconds < now;
        });
        if (entries.size() != before) dirty_ = true;
        if (entries.empty()) {
            album_entry = albums_.erase(album_entry);
            continue;
        }
        album_entry->second = Json(std::move(entries));
        ++album_entry;
    }
    if (const auto found = albums_.find(album_key_); found != albums_.end()) {
        entries_ = found->second.as_object();
    }
}

bool SyncReceipts::contains(const std::string& media_id) const {
    return !media_id.empty() && entries_.contains(media_id);
}

void SyncReceipts::add(const std::string& media_id, std::int64_t expires_at, std::int64_t captured_at) {
    if (media_id.empty()) return;
    std::int64_t expiry = unix_seconds(expires_at);
    if (expiry <= 0) {
        const auto captured = unix_seconds(captured_at);
        expiry = (captured > 0 ? captured : unix_now()) + kReceiptFallbackLifetimeSeconds;
    }
    const auto existing = entries_.find(media_id);
    if (existing != entries_.end() && existing->second.as_i64() == expiry) return;
    entries_[media_id] = Json(expiry);
    dirty_ = true;
}

bool SyncReceipts::save() {
    if (!dirty_) return true;
    if (entries_.empty()) albums_.erase(album_key_);
    else albums_[album_key_] = Json(entries_);
    const bool saved = write_local_data(file_, Json(Json::object{
        {"version", 1},
        {"albums", Json(albums_)},
    }));
    if (saved) dirty_ = false;
    return saved;
}

std::filesystem::path album_directory(const std::filesystem::path& destination) {
    if (destination.empty()) return path_from_utf8(default_album_folder());
    auto root = destination;
    if (!root.has_filename() && root.has_relative_path()) root = root.parent_path();
    return is_default_album_folder(root) || lower(path_to_utf8(root.filename())) == "album"
        ? root : root / "Album";
}

bool is_default_album_folder(const std::filesystem::path& folder) {
    const auto default_folder = path_from_utf8(default_album_folder());
    if (folder.lexically_normal() == default_folder.lexically_normal()) return true;
    std::error_code error;
    return std::filesystem::equivalent(folder, default_folder, error);
}

std::vector<LocalCapture> scan_capture_library(
    const std::filesystem::path& destination, const std::function<bool()>& cancelled) {
    std::error_code error;
    auto album = std::filesystem::absolute(album_directory(destination), error).lexically_normal().make_preferred();
    if (error) return {};
    const auto legacy = is_default_album_folder(album) ? album / "Album" : std::filesystem::path{};
    std::vector<LocalCapture> captures;
    if (!std::filesystem::is_directory(album, error) || error) return {};

    // One FindFirstFileEx pass returns each file's size, modified and created
    // times together, so large albums need no extra per-file system calls.
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    std::vector<std::filesystem::path> pending{album};
    while (!pending.empty()) {
        if (should_cancel(cancelled)) return {};
        const auto directory = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW((directory / L"*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;  // Unreadable folders are skipped.
        do {
            if (should_cancel(cancelled)) {
                FindClose(find);
                return {};
            }
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            // Symbolic links and junctions are skipped, as before. Other reparse
            // points (such as OneDrive files) are ordinary captures.
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
                continue;
            }
            auto path = directory / data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!is_workspace(path)) pending.push_back(std::move(path));
                continue;
            }
            const auto extension = lower(path_to_utf8(path.extension()));
            const bool video = extension == ".mp4";
            if (!video && extension != ".jpg" && extension != ".jpeg" && extension != ".png") continue;
            const auto bytes = (static_cast<std::uintmax_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (!bytes) continue;
            const auto written = ticks(data.ftLastWriteTime);
            const auto created = ticks(data.ftCreationTime);
            const auto timestamp = written >= kUnixEpochTicks
                ? static_cast<std::int64_t>((written - kUnixEpochTicks) / 10000000ULL) : 0;
            const auto added_ms = created >= kUnixEpochTicks
                ? static_cast<std::int64_t>((created - kUnixEpochTicks) / 10000ULL) : 0;
            auto relative = path.lexically_relative(album);
            if (!legacy.empty() && relative.has_parent_path() && *relative.begin() == legacy.filename()) {
                relative = path.lexically_relative(legacy);
            }
            const auto folder = relative.has_parent_path() ? album / *relative.begin() : album;
            captures.push_back({std::move(path), folder, timestamp, bytes, video, added_ms});
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::sort(captures.begin(), captures.end(), [](const LocalCapture& left, const LocalCapture& right) {
        if (left.modified != right.modified) return left.modified > right.modified;
        return left.path < right.path;
    });
    return captures;
}

std::string resolve_game_folder(
    const std::filesystem::path& album_directory,
    const std::string& app_name) {
    const auto default_clean = sanitize_folder_v1(app_name);
    if (!std::filesystem::exists(album_directory) || trim(app_name).empty()) {
        return default_clean;
    }

    try {
        struct DirectoryName {
            std::string original;
            std::string normalized;
        };
        std::vector<DirectoryName> directories;
        for (const auto& directory : std::filesystem::directory_iterator(album_directory)) {
            if (!directory.is_directory()) continue;
            const auto name = path_to_utf8(directory.path().filename());
            directories.push_back({name, normalize_for_matching_v1(name)});
        }

        const auto normalized_app_name = normalize_for_matching_v1(app_name);
        std::vector<std::string> synonyms{app_name};
        for (const auto& group : game_alias_groups()) {
            bool group_matches = false;
            for (const auto& alias : group) {
                if (normalize_for_matching_v1(alias) == normalized_app_name) {
                    group_matches = true;
                    break;
                }
            }
            if (group_matches) {
                synonyms = group;
                break;
            }
        }

        // Match v1.0.0 ordering: sanitized exact match first, then normalized
        // exact match for every known localization/synonym.
        for (const auto& synonym : synonyms) {
            const auto clean_synonym = sanitize_folder_v1(synonym);
            const auto normalized_synonym = normalize_for_matching_v1(synonym);

            for (const auto& directory : directories) {
                if (equal_v1_name(directory.original, clean_synonym)) {
                    return directory.original;
                }
            }
            for (const auto& directory : directories) {
                if (directory.normalized == normalized_synonym) {
                    return directory.original;
                }
            }
        }

        // v1's final fallback only fuzzed the API title itself, not every alias.
        if (!normalized_app_name.empty()) {
            for (const auto& directory : directories) {
                if (directory.normalized.size() >= 6 &&
                    normalized_app_name.size() >= 6 &&
                    (directory.normalized.find(normalized_app_name) != std::string::npos ||
                     normalized_app_name.find(directory.normalized) != std::string::npos)) {
                    return directory.original;
                }
            }
        }
    } catch (...) {
        // v1 treated local folder enumeration/matching failures as non-fatal and
        // simply fell back to the sanitized API title.
    }

    return default_clean;
}

SyncResult SyncEngine::sync(
    const std::function<bool()>& cancelled,
    const std::function<void(const SyncProgress&)>& progress,
    const std::vector<std::string>& retry_ids) {
    SyncResult result;
    const auto fatal = [&](SyncFailure failure) {
        result.fatal = true;
        result.failure = failure;
        return std::move(result);
    };
    const auto report = [&](SyncStage stage) {
        if (progress) {
            progress({stage, result.total_found, result.new_downloads, result.skipped, result.failed});
        }
    };

    const auto config = config_.snapshot();
    if (config.session_token.empty()) return fatal(SyncFailure::sign_in);

    throw_if_cancelled(cancelled);
    report(SyncStage::checking);

    std::vector<MediaItem> media;
    try {
        media = coral_.media_list(config.session_token);
    } catch (const NintendoSessionExpired&) {
        throw_if_cancelled(cancelled);
        return fatal(SyncFailure::sign_in);
    } catch (const std::exception& error) {
        throw_if_cancelled(cancelled);
        result.error_detail = error.what();
        result.transient = dynamic_cast<const TransientHttpError*>(&error) != nullptr;
        return fatal(SyncFailure::service);
    }
    throw_if_cancelled(cancelled);

    // Re-read the listing on every retry so signed download URLs are always fresh.
    std::set<std::string> remaining(retry_ids.begin(), retry_ids.end());
    std::vector<const MediaItem*> selected;
    for (const auto& item : media) {
        if (retry_ids.empty() || remaining.erase(media_retry_id(item))) selected.push_back(&item);
    }
    result.total_found = static_cast<int>(selected.size());
    result.unavailable = static_cast<int>(remaining.size());

    std::filesystem::path root;
    std::filesystem::path album;
    try {
        root = path_from_utf8(config.destination_folder.empty()
            ? default_album_folder()
            : config.destination_folder);
        album = album_directory(root);
        std::filesystem::create_directories(root);
    } catch (const std::runtime_error&) {
        throw_if_cancelled(cancelled);
        return fatal(SyncFailure::storage);
    }

    SyncReceipts receipts(config_.directory() / "synced-media.json", album);

    // Most automatic syncs find nothing new. Only walk the album folder (which
    // can hold thousands of files) when something might be downloaded.
    const bool all_synced = retry_ids.empty() &&
        std::all_of(selected.begin(), selected.end(), [&](const MediaItem* item) {
            return receipts.contains(item->id);
        });
    ExistingAlbumIndex existing;
    if (!all_synced) {
        try {
            existing = index_existing_album(root, cancelled);
        } catch (const std::runtime_error&) {
            throw_if_cancelled(cancelled);
            return fatal(SyncFailure::storage);
        }
    }
    const auto title_folders = learn_title_folders(media, existing);
    // Save whatever was recorded even if the sync is cancelled part way.
    struct SaveReceipts {
        SyncReceipts& receipts;
        ~SaveReceipts() {
            try {
                receipts.save();
            } catch (...) {
            }
        }
    } save_receipts{receipts};
    const auto remember = [&](const MediaItem& item) {
        receipts.add(item.id, item.expires_at, media_timestamp(item));
    };

    // Downloads from Nintendo's servers identify the same way as the app's
    // Coral requests. The version is already cached from the album request.
    std::string user_agent;
    try {
        user_agent = coral_.app_user_agent();
    } catch (const std::exception&) {
    }
    report(SyncStage::downloading);

    for (const auto* selected_item : selected) {
        throw_if_cancelled(cancelled);
        const auto& item = *selected_item;
        const auto timestamp = media_timestamp(item);
        const auto prefix = capture_timestamp_prefix(timestamp);
        const auto extension = lower(item.type) == "video" ? "mp4" : "jpg";
        const auto filename = prefix + "_c." + extension;
        const auto filename_lower = lower(filename);
        const auto prefix_lower = lower(prefix);
        const auto label = item.app_name + " / " + filename;

        // Synced before: don't download it again, even if the file was since
        // deleted, renamed or moved, or the PC's time zone changed. Retries of
        // failed items are never receipted, so they skip this check.
        if (retry_ids.empty() && receipts.contains(item.id)) {
            ++result.skipped;
            result.items.push_back({label, "Already synced", {}});
            report(SyncStage::downloading);
            continue;
        }

        const bool existing_filename = existing.filenames_and_prefixes.contains(filename_lower);
        const bool existing_timestamp = existing.filenames_and_prefixes.contains(prefix_lower);
        const auto device_match = existing.device_capture_by_key.find(prefix.substr(0, 14) + "." + extension);
        const bool can_review = !existing_filename && device_match != existing.device_capture_by_key.end();
        const bool reviewed = can_review && recorded_online_match(album, path_from_utf8(filename));
        const bool review = can_review && !reviewed;
        if (!review && (existing_filename || existing_timestamp || reviewed)) {
            remember(item);
            ++result.skipped;
            result.items.push_back({label, "Skipped", {}});
            report(SyncStage::downloading);
            continue;
        }

        auto failure = SyncFailure::download;
        try {
            validate_media_item_for_download(item);

            if (review) {
                const auto body = download_capture(http_, item, user_agent);
                throw_if_cancelled(cancelled);
                failure = SyncFailure::storage;
                stage_online_match(album, path_from_utf8(filename), device_match->second, [&](const auto& incoming) {
                    write_capture(incoming, body, cancelled);
                    preserve_capture_timestamp(incoming, timestamp);
                });
                remember(item);
                ++result.review;
                result.items.push_back({label, "Ready in Duplicate review", {}});
                report(SyncStage::downloading);
                continue;
            }
            failure = SyncFailure::storage;
            const auto known_folder = title_folders.find(lower(item.title_id));
            const auto game_folder = known_folder == title_folders.end()
                ? resolve_game_folder(album, item.app_name)
                : known_folder->second;
            const auto destination = album / path_from_utf8(game_folder) / path_from_utf8(filename);
            std::filesystem::create_directories(destination.parent_path());

            failure = SyncFailure::download;
            const auto body = download_capture(http_, item, user_agent);
            throw_if_cancelled(cancelled);

            failure = SyncFailure::storage;
            write_capture(destination, body, cancelled);
            preserve_capture_timestamp(destination, timestamp);
            remember(item);
            existing.filenames_and_prefixes.insert(filename_lower);
            existing.filenames_and_prefixes.insert(prefix_lower);
            ++result.new_downloads;
            result.downloaded_paths.push_back(destination);
            result.items.push_back({label, "Downloaded", {}});
        } catch (const std::runtime_error& error) {
            throw_if_cancelled(cancelled);
            ++result.failed;
            if (result.failure == SyncFailure::none || failure == SyncFailure::storage) {
                result.failure = failure;
            }
            if (item.expires_at > 0) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (unix_seconds(item.expires_at) - now <= kExpiryWarningSeconds) ++result.expiring_soon;
            }
            auto retry_id = media_retry_id(item);
            const bool transient = dynamic_cast<const TransientHttpError*>(&error) != nullptr;
            result.items.push_back({label, failure == SyncFailure::storage ? "Save failed" : "Download failed",
                                    retry_id, transient});
            if (!retry_id.empty()) result.failed_ids.push_back(std::move(retry_id));
        }
        report(SyncStage::downloading);
    }

    return result;
}

}  // namespace nso
