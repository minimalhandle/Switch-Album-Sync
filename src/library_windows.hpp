#pragma once
#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/windows_compat.hpp"

namespace nso {
enum class LibraryPage { captures, history, storage, duplicates };

class WindowsLibrary {
public:
    WindowsLibrary();
    ~WindowsLibrary();
    void show(HWND owner, LibraryPage page, const PlatformCallbacks& callbacks);
    void show_arrivals(HWND owner, const PlatformCallbacks& callbacks,
                       const std::vector<std::filesystem::path>& captures);
    void update(const MenuState& state);
    // Re-reads tags, favourites and collections after they changed on disk
    // (for example, Import settings).
    void reload_saved_data();
    bool route_key(MSG& message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool copy_capture_files(HWND owner, const std::vector<std::filesystem::path>& paths);
}
