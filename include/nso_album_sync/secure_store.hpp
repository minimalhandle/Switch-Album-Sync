#pragma once

#include <optional>
#include <string>

namespace nso {

class SecureStore {
public:
    static bool available();
    static bool put(const std::string& account, const std::string& secret);
    static std::optional<std::string> get(const std::string& account);
    static void erase(const std::string& account);
    // Tests only: keep credentials under "<name>/..." instead of the app's
    // "Switch Album Sync/..." so test runs never touch a real sign-in.
    static void use_namespace(const std::string& name);
};

}  // namespace nso
