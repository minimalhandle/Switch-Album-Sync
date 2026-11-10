#pragma once

#include "nso_album_sync/json.hpp"
#include <filesystem>

namespace nso {
Json read_local_data(const std::filesystem::path& path);
bool write_local_data(const std::filesystem::path& path, const Json& data);
}
