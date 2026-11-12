#include "nso_album_sync/app.hpp"
#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/util.hpp"

#include "nso_album_sync/windows_compat.hpp"
#include "theme_windows.hpp"
#include <shellapi.h>

#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class SingleInstanceGuard {
public:
    SingleInstanceGuard() {
        handle_ = CreateMutexW(nullptr, FALSE,
            L"Local\\SwitchAlbumSync_SingleInstance_Mutex");
        acquired_ = handle_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    }

    ~SingleInstanceGuard() {
        if (handle_ != nullptr) CloseHandle(handle_);
    }

    bool acquired() const { return acquired_; }

private:
    bool acquired_ = false;
    HANDLE handle_ = nullptr;
};

int run_self_test() {
    try {
        if (nso::application_runtime_directory().empty()) {
            std::cerr << "Self-test failed: private runtime directory is unavailable\n";
            return 2;
        }

        if constexpr (nso::discord_available) {
            nso::DiscordPresence discord(nso::kDiscordApplicationId);
            if (!discord.self_test_runtime()) {
                std::cerr << "Self-test failed: Discord Social SDK runtime did not load\n";
                return 3;
            }
            discord.clear();
        }

        std::cout << "Switch Album Sync self-test passed\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Self-test failed during initialization\n";
        return 4;
    }
}

void log_debug_startup(const char* message) {
    if (!nso::debug_logging_enabled()) return;
    try {
        std::ofstream log(std::filesystem::temp_directory_path() / "switch-album-sync-rpc.log", std::ios::app);
        if (log) log << "[Switch Album Sync] " << message << '\n';
    } catch (const std::exception&) {
    }
}

int run_application(const std::string& auth_callback, [[maybe_unused]] bool start_in_tray = false) {
    log_debug_startup("Debug launch requested.");
    try {
        SingleInstanceGuard single_instance;
        if (!single_instance.acquired()) {
            log_debug_startup("Another Switch Album Sync instance is running. Exit it from the tray before relaunching in debug mode.");
            if (!auth_callback.empty()) {
                // Protocol callbacks launch a second copy of the executable.
                // Forward that URL to the already-running tray process instead
                // of treating the launch as an ordinary second app instance.
                for (int attempt = 0; attempt < 5; ++attempt) {
                    if (nso::publish_nintendo_auth_callback(auth_callback)) {
                        nso::show_running_dashboard();
                        return 0;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            if (auth_callback.empty() && start_in_tray) return 0;
            if (auth_callback.empty() && nso::show_running_dashboard()) return 0;
            nso::theme::message_box(nullptr,
                L"Open it from its icon in the notification area.",
                L"Switch Album Sync is already running", MB_OK);
            return 0;
        }

        if (!auth_callback.empty()) {
            nso::publish_nintendo_auth_callback(auth_callback);
        }

        log_debug_startup("Starting Switch Album Sync in debug mode.");
        nso::App app;
        return app.run();
    } catch (const std::exception&) {
        log_debug_startup("Switch Album Sync could not start.");
        std::cerr << "Fatal: Switch Album Sync could not start\n";
        return 1;
    }
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

bool command_line_has_argument(const std::wstring& argument) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && argument == argv[i]) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

std::size_t find_case_insensitive(
    const std::string& text,
    const std::string& needle) {
    if (needle.empty() || text.size() < needle.size()) {
        return std::string::npos;
    }
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        bool matches = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            const auto left = static_cast<unsigned char>(text[start + i]);
            const auto right = static_cast<unsigned char>(needle[i]);
            if (std::tolower(left) != std::tolower(right)) {
                matches = false;
                break;
            }
        }
        if (matches) return start;
    }
    return std::string::npos;
}

std::string extract_auth_callback(const std::string& text) {
    const std::string prefix = nso::kNintendoAuthCallbackPrefix;
    const auto start = find_case_insensitive(text, prefix);
    if (start == std::string::npos) return {};

    auto end = text.find_first_of("\"' \t\r\n", start);
    if (end == std::string::npos) end = text.size();
    while (end > start && (text[end - 1] == ',' || text[end - 1] == ';')) {
        --end;
    }

    const auto callback = text.substr(start, end - start);
    return nso::is_nintendo_auth_callback(callback) ? callback : std::string{};
}

std::string auth_callback_from_command_line() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            if (const auto callback = extract_auth_callback(wide_to_utf8(argv[i]));
                !callback.empty()) {
                LocalFree(argv);
                return callback;
            }
        }
        LocalFree(argv);
    }

    // Some browser/shell protocol launches do not survive argv tokenisation in
    // exactly the form we registered. Scan the raw command line as a fallback.
    return extract_auth_callback(wide_to_utf8(GetCommandLineW()));
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    nso::set_debug_logging_enabled(
        command_line_has_argument(L"--debug") || command_line_has_argument(L"-debug"));
    if (command_line_has_argument(L"--self-test")) return run_self_test();
    return run_application(auth_callback_from_command_line(), command_line_has_argument(L"--start-in-tray"));
}
