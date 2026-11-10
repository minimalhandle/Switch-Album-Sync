#pragma once

#include "nso_album_sync/windows_compat.hpp"

#include <filesystem>
#include <vector>

namespace nso {

enum class ShareFormat { folder, zip, contact_sheet };

bool is_share_screenshot(const std::filesystem::path& path);
std::filesystem::path pick_share_folder(HWND owner);
std::filesystem::path export_captures(const std::vector<std::filesystem::path>& captures,
    const std::filesystem::path& folder, ShareFormat format, bool resize);

}
