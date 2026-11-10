#include "nso_album_sync/config.hpp"
#include "nso_album_sync/path.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "nso_album_sync/windows_compat.hpp"
#include <shlobj.h>
#include <wincrypt.h>

namespace nso {
namespace {

constexpr char kSecureStoreAccount[] = "NintendoAccount";
constexpr char kSecureMarker[] = "secure:v1";
constexpr char kVolatileMarker[] = "volatile:v1";

std::string environment_variable(const char* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    if (_wdupenv_s(&value, &length, wide_name.c_str()) != 0 || value == nullptr) {
        return {};
    }
    const auto result = path_to_utf8(std::filesystem::path(value));
    std::free(value);
    return result;
}

std::filesystem::path config_directory() {
    const auto app_data = environment_variable("APPDATA");
    return path_from_utf8(app_data.empty() ? "." : app_data) / "Switch Album Sync";
}
}  // namespace

std::string default_album_folder() {
    wchar_t pictures[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(
            nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures)) &&
        pictures[0] != L'\0') {
        return path_to_utf8(std::filesystem::path(pictures) / L"Switch Album Sync");
    }
    const auto home = environment_variable("USERPROFILE");
    const auto base = path_from_utf8(home.empty() ? "." : home);
    return path_to_utf8(base / "Pictures" / "Switch Album Sync");
}

namespace {

std::string string_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    const std::string& fallback = {}) {
    const auto current = json.string(current_key);
    return current.empty() ? json.string(legacy_key, fallback) : current;
}

bool bool_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    bool fallback) {
    if (const auto* current = json.find(current_key);
        current != nullptr && current->is_bool()) {
        return current->as_bool();
    }
    return json.boolean(legacy_key, fallback);
}

std::int64_t integer_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    std::int64_t fallback) {
    if (const auto* current = json.find(current_key);
        current != nullptr && current->is_number()) {
        return current->as_i64();
    }
    return json.integer(legacy_key, fallback);
}

std::string display_last_sync(const std::string& stored) {
    if (stored.empty()) return "Never";
    if (stored.size() >= 16 && stored[4] == '-' && stored[7] == '-' &&
        (stored[10] == 'T' || stored[10] == ' ')) {
        return stored.substr(11, 5) + " (" + stored.substr(0, 10) + ")";
    }
    return stored;
}

bool is_secure_store_marker(const std::string& value) {
    // Recognize old credential markers so a copied settings file cannot send
    // a storage marker to Nintendo as though it were an authentication token.
    return value == kSecureMarker || value == "keychain:v1" || value == "secret-service:v1";
}

std::string decrypt_legacy_dpapi_token(const std::string& value) {
    constexpr char kPrefix[] = "dpapi:";
    if (value.rfind(kPrefix, 0) != 0) return {};

    const auto cipher = base64_decode(value.substr(sizeof(kPrefix) - 1));
    if (cipher.empty()) return {};

    static constexpr char kEntropy[] = "NSO_Album_Sync_Salt_9981";
    DATA_BLOB input{
        static_cast<DWORD>(cipher.size()),
        const_cast<BYTE*>(cipher.data()),
    };
    DATA_BLOB entropy{
        static_cast<DWORD>(sizeof(kEntropy) - 1),
        reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)),
    };
    DATA_BLOB output{};

    if (!CryptUnprotectData(
            &input, nullptr, &entropy, nullptr, nullptr, 0, &output)) {
        return {};
    }

    std::string plain(
        reinterpret_cast<char*>(output.pbData),
        reinterpret_cast<char*>(output.pbData) + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
}

void replace_config_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
    if (!MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not replace config.json");
    }
}

}  // namespace

ConfigManager::ConfigManager()
    : directory_(config_directory()),
      config_file_(directory_ / "config.json") {
    load();
}

AppConfig ConfigManager::snapshot() const {
    std::lock_guard lock(mutex_);
    return config_;
}

AppConfig ConfigManager::update(const std::function<void(AppConfig&)>& updater) {
    std::lock_guard lock(mutex_);
    const auto previous = config_;
    updater(config_);
    const bool changed_session = config_.session_token != previous.session_token;
    bool stored_securely = false;
    if (changed_session) {
        stored_securely = !config_.session_token.empty() && SecureStore::available() &&
            SecureStore::put(kSecureStoreAccount, config_.session_token);
        config_.session_storage = config_.session_token.empty()
            ? SessionStorage::none
            : stored_securely ? SessionStorage::secure : SessionStorage::session_only;
    }
    try {
        save_locked();
    } catch (...) {
        if (!changed_session) {
            config_ = previous;
        } else if (!stored_securely || !secure_marker_on_disk_) {
            if (stored_securely) SecureStore::erase(kSecureStoreAccount);
            config_.session_storage = legacy_session_marker_.empty()
                ? SessionStorage::session_only : SessionStorage::legacy;
        }
        throw;
    }
    return config_;
}

void ConfigManager::load() {
    std::lock_guard lock(mutex_);
    config_ = {};
    legacy_session_marker_.clear();
    secure_marker_on_disk_ = false;
    std::filesystem::create_directories(directory_);

    bool needs_config_rewrite = false;

    if (std::filesystem::exists(config_file_)) {
        try {
            std::ifstream file(config_file_);
            const std::string contents{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()};
            const auto json = Json::parse(contents);

            config_.user_nickname = string_with_legacy_key(
                json, "userNickname", "UserNickname", config_.user_nickname);
            config_.destination_folder = string_with_legacy_key(
                json, "destinationFolder", "DestinationFolder", config_.destination_folder);
            config_.watch_folder = json.string("watchFolder");
            // v1.0.0 defaulted auto-sync to enabled and respected an
            // explicitly saved AutoSyncEnabled/autoSync value. Preserve that
            // behavior across the C++ config migration.
            config_.auto_sync = bool_with_legacy_key(
                json, "autoSync", "AutoSyncEnabled", false);
            config_.auto_sync_setting_version = 1;
            config_.notifications = bool_with_legacy_key(
                json, "notifications", "NotificationsEnabled", false);
            const auto discord_setting_version =
                json.integer("discordPresenceSettingVersion", 0);
            if (discord_setting_version >= 1) {
                config_.discord_presence = bool_with_legacy_key(
                    json, "discordPresence", "DiscordPresenceEnabled", false);
            } else {
                // Older v2 builds enabled RPC by default. Treat those saved
                // defaults as not having explicit consent and require one
                // deliberate opt-in after upgrading.
                config_.discord_presence = false;
                needs_config_rewrite = true;
            }
            config_.discord_presence_setting_version = 1;
            config_.rpc.show_username = json.boolean("rpcShowUsername", true);
            config_.rpc.show_profile_picture = json.boolean("rpcShowProfilePicture", true);
            config_.rpc.show_play_time = json.boolean("rpcShowPlayTime", true);
            config_.rpc.zelda = json.boolean("rpcZelda", true);
            config_.rpc.animal_crossing = json.boolean("rpcAnimalCrossing", true);
            config_.rpc.splatoon3 = json.boolean("rpcSplatoon3", true);
            config_.rpc.splatoon2 = json.boolean("rpcSplatoon2", true);
            config_.start_on_boot = bool_with_legacy_key(
                json, "startOnBoot", "StartOnBoot", false);
            config_.sync_interval_minutes = static_cast<int>(std::clamp<std::int64_t>(
                integer_with_legacy_key(
                    json, "syncIntervalMinutes", "SyncIntervalMinutes", 60),
                1,
                (std::numeric_limits<int>::max)()));
            config_.last_sync = display_last_sync(string_with_legacy_key(
                json, "lastSync", "LastSyncTime", "Never"));
            config_.proxy_url = string_with_legacy_key(
                json, "proxyUrl", "ProxyUrl");
            config_.nxapi_auth_client_id = string_with_legacy_key(
                json, "nxapiAuthClientId", "NxapiAuthClientId",
                config_.nxapi_auth_client_id);

            // Rich Presence uses Switch Album Sync's public application identity.
            // Older builds persisted this 64-bit snowflake as a
            // JSON number, which can lose precision because Json stores numbers
            // as doubles. Ignore and remove any legacy override instead.
            config_.discord_application_id = kDiscordApplicationId;
            if (json.find("discordApplicationId") != nullptr ||
                json.find("DiscordApplicationId") != nullptr) {
                needs_config_rewrite = true;
            }

            const auto stored_session = string_with_legacy_key(
                json, "sessionToken", "SessionToken");

            if (is_secure_store_marker(stored_session)) {
                secure_marker_on_disk_ = true;
                config_.session_storage = SessionStorage::unavailable;
                if (const auto token = SecureStore::get(kSecureStoreAccount); token && !token->empty()) {
                    config_.session_token = *token;
                    config_.session_storage = SessionStorage::secure;
                }
            } else if (stored_session.empty() || stored_session == kVolatileMarker) {
                config_.session_token.clear();
            } else if (stored_session.rfind("dpapi:", 0) == 0) {
                config_.session_token = decrypt_legacy_dpapi_token(stored_session);
                config_.session_storage = SessionStorage::legacy;
                legacy_session_marker_ = stored_session;
            } else {
                config_.session_token = stored_session;
                config_.session_storage = SessionStorage::legacy;
                legacy_session_marker_ = stored_session;
            }
        } catch (...) {
            // Keep defaults if the config is corrupt or from an unknown version.
        }
    }

    // Keep a corrupt/missing config from silently falling back to the process
    // working directory, which can otherwise save captures beside the exe.
    if (config_.destination_folder.empty()) {
        config_.destination_folder = default_album_folder();
    }

    try {
        if (config_.session_storage == SessionStorage::legacy) {
            // Keep the original credential visible until migration succeeds or
            // the user removes it. New credentials always use the secure store.
            if (!config_.session_token.empty() && SecureStore::available() &&
                SecureStore::put(kSecureStoreAccount, config_.session_token)) {
                config_.session_storage = SessionStorage::secure;
                try {
                    save_locked();
                } catch (...) {
                    SecureStore::erase(kSecureStoreAccount);
                    config_.session_storage = SessionStorage::legacy;
                }
            }
        } else if (needs_config_rewrite) {
            save_locked();
        }
    } catch (const std::exception&) {
        // An unwritable config must not hide the credential recovery controls.
    }
}

void ConfigManager::save_locked() {
    std::filesystem::create_directories(directory_);

    std::string session_marker;
    const auto storage = config_.session_storage;
    const bool secure_marker = storage == SessionStorage::secure || storage == SessionStorage::unavailable;
    if (secure_marker) {
        session_marker = kSecureMarker;
    } else if (storage == SessionStorage::session_only) {
        session_marker = kVolatileMarker;
    } else if (storage == SessionStorage::legacy) {
        // Preserve an existing unmigrated credential until it can be migrated
        // or the user chooses to remove it. New logins never use this branch.
        session_marker = legacy_session_marker_;
    }

    const Json json(Json::object{
        {"sessionToken", session_marker},
        {"userNickname", config_.user_nickname},
        {"destinationFolder", config_.destination_folder},
        {"watchFolder", config_.watch_folder},
        {"autoSync", config_.auto_sync},
        {"autoSyncSettingVersion",
         static_cast<std::int64_t>(config_.auto_sync_setting_version)},
        {"notifications", config_.notifications},
        {"discordPresence", config_.discord_presence},
        {"rpcShowUsername", config_.rpc.show_username},
        {"rpcShowProfilePicture", config_.rpc.show_profile_picture},
        {"rpcShowPlayTime", config_.rpc.show_play_time},
        {"rpcZelda", config_.rpc.zelda},
        {"rpcAnimalCrossing", config_.rpc.animal_crossing},
        {"rpcSplatoon3", config_.rpc.splatoon3},
        {"rpcSplatoon2", config_.rpc.splatoon2},
        {"discordPresenceSettingVersion",
         static_cast<std::int64_t>(config_.discord_presence_setting_version)},
        {"startOnBoot", config_.start_on_boot},
        {"syncIntervalMinutes", config_.sync_interval_minutes},
        {"lastSync", config_.last_sync},
        {"proxyUrl", config_.proxy_url},
        {"nxapiAuthClientId", config_.nxapi_auth_client_id},
    });

    auto temporary = config_file_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc | std::ios::binary);
        if (!file) throw std::runtime_error("Could not write config.json");
        file << json.dump();
        file.flush();
        if (!file) throw std::runtime_error("Could not write config.json");
    }
    replace_config_file(temporary, config_file_);
    secure_marker_on_disk_ = secure_marker;
    if (storage != SessionStorage::legacy) legacy_session_marker_.clear();
}

void ConfigManager::save() {
    std::lock_guard lock(mutex_);
    save_locked();
}

void ConfigManager::clear_session() {
    std::lock_guard lock(mutex_);
    config_.session_token.clear();
    config_.session_storage = SessionStorage::none;
    config_.user_nickname.clear();
    SecureStore::erase(kSecureStoreAccount);
    try {
        save_locked();
    } catch (...) {
        if (!legacy_session_marker_.empty()) config_.session_storage = SessionStorage::legacy;
        else if (secure_marker_on_disk_) config_.session_storage = SessionStorage::unavailable;
        throw;
    }
}

void ConfigManager::clear_legacy_credential() {
    std::lock_guard lock(mutex_);
    if (config_.session_storage != SessionStorage::legacy) return;
    SecureStore::erase(kSecureStoreAccount);
    config_.session_storage = config_.session_token.empty()
        ? SessionStorage::none : SessionStorage::session_only;
    try {
        save_locked();
    } catch (...) {
        config_.session_storage = SessionStorage::legacy;
        throw;
    }
}

}  // namespace nso
