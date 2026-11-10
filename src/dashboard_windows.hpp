#pragma once

#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/windows_compat.hpp"

#include <memory>

namespace nso {

class WindowsDashboard {
public:
    WindowsDashboard();
    ~WindowsDashboard();

    bool create(HICON icon, const PlatformCallbacks& callbacks,
                std::function<bool(const std::filesystem::path&)> copy_capture,
                bool hide_on_close);
    void show();
    void show_arrivals(const std::vector<std::filesystem::path>& captures);
    void copy_latest();
    bool copy_capture(const std::filesystem::path& path);
    // Caller owns the returned menu bitmap.
    HBITMAP recent_menu_thumbnail(const std::filesystem::path& path) const;
    void update(const MenuState& state);
    HWND visible_window() const;
    bool route_key(MSG& message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nso
