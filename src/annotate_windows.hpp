#pragma once

#include "nso_album_sync/windows_compat.hpp"

#include <filesystem>
#include <functional>

namespace nso {

// Opens a window for cropping, blurring (pixelating) and adding text to a
// copy of a screenshot. The original file is never changed. `saved` is called
// with the path of each copy the user saves.
void open_screenshot_editor(HWND owner, const std::filesystem::path& screenshot,
                            std::function<void(const std::filesystem::path&)> saved);

}  // namespace nso
