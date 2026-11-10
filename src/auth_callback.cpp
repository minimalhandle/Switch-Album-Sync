#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/path.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "nso_album_sync/windows_compat.hpp"

namespace nso {
namespace {

constexpr std::size_t kMaxCallbackBytes = 16 * 1024;
std::atomic<std::uint64_t> g_callback_temp_counter{0};

std::string environment_variable(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
}

std::filesystem::path runtime_directory_candidate() {
    const auto local_app_data = environment_variable("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return std::filesystem::path(local_app_data) / "Switch Album Sync" / "Runtime";
    }
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : temporary / "Switch Album Sync" / "Runtime";
}

bool ensure_private_runtime_directory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    return std::filesystem::is_directory(directory, error) && !error;
}

std::filesystem::path callback_path() {
    const auto directory = application_runtime_directory();
    return directory.empty() ? std::filesystem::path{} : directory / "auth-callback.txt";
}

std::filesystem::path callback_temporary_path(
    const std::filesystem::path& target) {
    const auto sequence = g_callback_temp_counter.fetch_add(1, std::memory_order_relaxed);
    const auto process = static_cast<unsigned long>(GetCurrentProcessId());
    return target.parent_path() /
           ("auth-callback.tmp." + std::to_string(process) + "." +
            std::to_string(sequence));
}

std::string strip_space(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ascii_equal_case_insensitive(
    std::string_view left,
    std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto l = static_cast<unsigned char>(left[i]);
        const auto r = static_cast<unsigned char>(right[i]);
        if (std::tolower(l) != std::tolower(r)) return false;
    }
    return true;
}


}  // namespace

std::filesystem::path application_runtime_directory() {
    const auto directory = runtime_directory_candidate();
    return ensure_private_runtime_directory(directory)
        ? directory
        : std::filesystem::path{};
}

bool is_nintendo_auth_callback(const std::string& url) {
    const std::string_view prefix = kNintendoAuthCallbackPrefix;
    if (url.size() < prefix.size() ||
        !ascii_equal_case_insensitive(
            std::string_view(url).substr(0, prefix.size()), prefix)) {
        return false;
    }

    std::size_t position = prefix.size();
    if (position == url.size()) return true;

    // Chromium/Windows can canonicalize a custom URI whose authority is
    // "auth" by inserting the otherwise-empty path slash. Nintendo's
    // npf...://auth#... callback can therefore arrive as npf...://auth/#...
    // (or npf...://auth/?...). Treat those forms as the same callback.
    if (url[position] == '/') {
        ++position;
        if (position == url.size()) return true;
    }

    return url[position] == '#' || url[position] == '?';
}

bool publish_nintendo_auth_callback(const std::string& url) {
    if (!is_nintendo_auth_callback(url) || url.size() > kMaxCallbackBytes) {
        return false;
    }

    try {
        const auto target = callback_path();
        if (target.empty()) return false;

        const auto temporary = callback_temporary_path(target);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output << url;
            output.flush();
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        std::error_code error;
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> take_nintendo_auth_callback() {
    try {
        const auto target = callback_path();
        if (target.empty()) return std::nullopt;

        std::string value;
        std::ifstream input(target, std::ios::binary);
        if (!input) return std::nullopt;
        value.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        input.close();
        std::error_code error;
        std::filesystem::remove(target, error);
        if (value.size() > kMaxCallbackBytes) return std::nullopt;

        value = strip_space(std::move(value));
        if (!is_nintendo_auth_callback(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

void clear_nintendo_auth_callback() {
    try {
        const auto directory = application_runtime_directory();
        if (directory.empty()) return;

        std::error_code error;
        std::filesystem::remove(directory / "auth-callback.txt", error);

        // Clean only our own stale temporary callback files from the private
        // runtime directory. They contain no long-lived Nintendo credential,
        // but should not survive a crash longer than necessary.
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory,
                 std::filesystem::directory_options::skip_permission_denied,
                 error)) {
            if (error) break;
            const auto name = path_to_utf8(entry.path().filename());
            if (name.rfind("auth-callback.tmp.", 0) == 0) {
                std::filesystem::remove(entry.path(), error);
                error.clear();
            }
        }
    } catch (...) {
    }
}

}  // namespace nso
