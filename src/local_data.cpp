#include "nso_album_sync/local_data.hpp"

#include <fstream>
#include <iterator>

#include "nso_album_sync/windows_compat.hpp"

namespace nso {
Json read_local_data(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return Json::parse({std::istreambuf_iterator<char>(input), {}});
    } catch (const std::exception&) {
        return {};
    }
}

bool write_local_data(const std::filesystem::path& path, const Json& data) {
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << data.dump();
        output.close();
        if (output) {
            if (MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        }
    } catch (const std::exception&) {
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}
}
