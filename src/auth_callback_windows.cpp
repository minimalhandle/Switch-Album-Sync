#include "nso_album_sync/auth_callback.hpp"


#include "nso_album_sync/windows_compat.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr wchar_t kSchemeKey[] = L"npf71b963c1b7b6d119";
constexpr wchar_t kUserSchemeKey[] =
    L"Software\\Classes\\npf71b963c1b7b6d119";
constexpr wchar_t kUserChoiceKey[] =
    L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\"
    L"npf71b963c1b7b6d119\\UserChoice";
constexpr wchar_t kCommandKey[] = L"shell\\open\\command";
constexpr wchar_t kHandlerDescription[] =
    L"URL:Switch Album Sync Nintendo Account callback";
std::mutex g_protocol_mutex;
std::wstring g_registered_command;

std::wstring desired_command() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size()) {
            return L"\"" + std::wstring(buffer.data(), length) + L"\" \"%1\"";
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool key_may_exist(HKEY root, const wchar_t* path) {
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}

std::wstring read_default_value(HKEY root, const std::wstring& path) {
    DWORD bytes = 0;
    if (RegGetValueW(root, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
            nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
            buffer.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return buffer.data();
}

bool command_matches(const std::wstring& actual, const std::wstring& expected) {
    return !actual.empty() && !expected.empty() &&
        CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool current_handler_is_ours(const std::wstring& command) {
    const auto suffix = std::wstring(L"\\") + kCommandKey;
    return read_default_value(HKEY_CURRENT_USER, kUserSchemeKey) == kHandlerDescription &&
        command_matches(read_default_value(HKEY_CURRENT_USER, kUserSchemeKey + suffix), command) &&
        command_matches(read_default_value(HKEY_CLASSES_ROOT, kSchemeKey + suffix), command);
}

bool write_string_value(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

bool register_nintendo_auth_protocol() {
    std::lock_guard lock(g_protocol_mutex);
    // Both apps use Nintendo's redirect URI. A running tray process is fine
    // as long as it hasn't claimed the browser callback for its own sign-in.
    if (key_may_exist(HKEY_CURRENT_USER, kUserChoiceKey)) {
        return false;
    }

    const auto command = desired_command();
    if (command.empty()) return false;
    if (current_handler_is_ours(command)) {
        // Clean up a registration left by this executable after a previous crash.
        g_registered_command = command;
        return true;
    }
    if (key_may_exist(HKEY_CLASSES_ROOT, kSchemeKey)) return false;

    HKEY scheme = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserSchemeKey, 0, nullptr, 0,
            KEY_WRITE, nullptr, &scheme, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    if (disposition != REG_CREATED_NEW_KEY) {
        // The handler changed between our read and the claim. Leave it alone.
        RegCloseKey(scheme);
        return false;
    }

    HKEY command_key = nullptr;
    const bool root_owned = write_string_value(scheme, nullptr, kHandlerDescription);
    const bool registered = root_owned &&
        write_string_value(scheme, L"URL Protocol", L"") &&
        RegCreateKeyExW(scheme, kCommandKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &command_key, nullptr) == ERROR_SUCCESS &&
        write_string_value(command_key, nullptr, command);
    if (command_key) RegCloseKey(command_key);
    RegCloseKey(scheme);
    if (!registered || !current_handler_is_ours(command)) {
        if (root_owned && read_default_value(HKEY_CURRENT_USER, kUserSchemeKey) == kHandlerDescription) {
            RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
        }
        return false;
    }

    g_registered_command = command;
    return true;
}

void unregister_nintendo_auth_protocol() {
    std::lock_guard lock(g_protocol_mutex);
    if (!g_registered_command.empty() && current_handler_is_ours(g_registered_command)) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
    }
    g_registered_command.clear();
}

}  // namespace nso
