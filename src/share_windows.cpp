#include "share_windows.hpp"
#include "nso_album_sync/path.hpp"

#include <gdiplus.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace nso {
namespace {
using Path = std::filesystem::path;
constexpr std::uint32_t kZipLimit = 0xffffffff;
constexpr UINT kShareImageSize = 1280;

void zip_number(std::ostream& file, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        file.put(static_cast<char>(value & 255));
        value >>= 8;
    }
}

std::uint32_t zip_offset(std::ostream& file) {
    const auto position = static_cast<std::streamoff>(file.tellp());
    if (position < 0 || position >= kZipLimit) throw std::runtime_error("ZIP exceeds the 4 GiB limit");
    return static_cast<std::uint32_t>(position);
}

constexpr auto crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        auto value = index;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
        table[index] = value;
    }
    return table;
}

struct ZipEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint16_t date = 0x21;
    std::uint16_t time = 0;

    void header(std::ostream& file, bool central) const {
        zip_number(file, central ? 0x02014b50 : 0x04034b50, 4);
        if (central) zip_number(file, 20, 2);
        zip_number(file, 20, 2);     // ZIP version
        zip_number(file, 0x800, 2);  // UTF-8 names
        zip_number(file, 0, 2);      // Stored media
        zip_number(file, time, 2);
        zip_number(file, date, 2);
        zip_number(file, crc, 4);
        zip_number(file, size, 4);
        zip_number(file, size, 4);
        zip_number(file, static_cast<std::uint32_t>(name.size()), 2);
        zip_number(file, 0, 2);
        if (central) {
            zip_number(file, 0, 2);  // Comment length
            zip_number(file, 0, 2);  // Disk
            zip_number(file, 0, 2);  // Internal attributes
            zip_number(file, 0, 4);  // External attributes
            zip_number(file, offset, 4);
        }
        file.write(name.data(), static_cast<std::streamsize>(name.size()));
    }
};

ZipEntry append_zip_file(std::ostream& file, const Path& path, const Path& filename) {
    ZipEntry entry{path_to_utf8(filename), zip_offset(file)};
    const auto size = std::filesystem::file_size(path);
    if (size + entry.offset + entry.name.size() + 30 >= kZipLimit) {
        throw std::runtime_error("ZIP exceeds the 4 GiB limit");
    }
    entry.size = static_cast<std::uint32_t>(size);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    FILETIME local{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) &&
        FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local)) {
        FileTimeToDosDateTime(&local, &entry.date, &entry.time);
    }
    entry.header(file, false);
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit | std::ios::failbit);
    constexpr auto table = crc_table();
    std::array<char, 65536> buffer{};
    auto crc = 0xffffffffu;
    for (auto remaining = size; remaining;) {
        const auto count = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        file.write(buffer.data(), static_cast<std::streamsize>(count));
        for (std::size_t i = 0; i < count; ++i) {
            crc = table[(crc ^ static_cast<unsigned char>(buffer[i])) & 255] ^ (crc >> 8);
        }
        remaining -= count;
    }
    entry.crc = ~crc;
    const auto end = file.tellp();
    file.seekp(entry.offset + 14);
    zip_number(file, entry.crc, 4);
    file.seekp(end);
    return entry;
}

void finish_zip(std::ostream& file, const std::vector<ZipEntry>& entries) {
    const auto central = zip_offset(file);
    for (const auto& entry : entries) entry.header(file, true);
    const auto end = zip_offset(file);
    if (end > kZipLimit - 22) throw std::runtime_error("ZIP exceeds the 4 GiB limit");
    zip_number(file, 0x06054b50, 4);
    zip_number(file, 0, 4);  // Disk numbers
    const auto count = static_cast<std::uint32_t>(entries.size());
    zip_number(file, count, 2);
    zip_number(file, count, 2);
    zip_number(file, end - central, 4);
    zip_number(file, central, 4);
    zip_number(file, 0, 2);  // Comment length
}

std::wstring folded(std::wstring value) {
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

Path unique_name(const Path& filename, std::set<std::wstring>& names) {
    auto candidate = filename;
    for (int number = 2; !names.insert(folded(candidate.wstring())).second; ++number) {
        candidate = filename.stem().wstring() + L" (" + std::to_wstring(number) + L")" +
            filename.extension().wstring();
    }
    return candidate;
}

struct Imaging {
    ULONG_PTR token = 0;
    explicit Imaging(bool enabled) {
        if (!enabled) return;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("Could not initialize screenshot resizing");
        }
    }
    ~Imaging() { if (token) Gdiplus::GdiplusShutdown(token); }
};

struct PreparedCapture {
    Path path;
    Path filename;
};

PreparedCapture prepare_capture(const Path& source, const Path& staging, std::set<std::wstring>& names,
                     bool resize, bool zip) {
    const auto extension = folded(source.extension().wstring());
    if (resize && (extension == L".jpg" || extension == L".jpeg" || extension == L".png")) {
        Gdiplus::Bitmap original(source.c_str());
        const auto largest = std::max(original.GetWidth(), original.GetHeight());
        if (original.GetLastStatus() != Gdiplus::Ok || !largest) {
            throw std::runtime_error("Could not read a screenshot for resizing");
        }
        if (largest > kShareImageSize) {
            const auto width = std::max(1u, original.GetWidth() * kShareImageSize / largest);
            const auto height = std::max(1u, original.GetHeight() * kShareImageSize / largest);
            Gdiplus::Bitmap scaled(width, height, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&scaled);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            const Path filename = source.stem().wstring() + L".png";
            const auto target = staging / unique_name(filename, names);
            constexpr CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (graphics.DrawImage(&original, 0, 0, static_cast<int>(width), static_cast<int>(height)) != Gdiplus::Ok ||
                scaled.Save(target.c_str(), &png) != Gdiplus::Ok) {
                throw std::runtime_error("Could not resize a screenshot");
            }
            std::filesystem::last_write_time(target, std::filesystem::last_write_time(source));
            return {target, target.filename()};
        }
    }
    const auto filename = unique_name(source.filename(), names);
    if (zip) return {source, filename};
    const auto target = staging / filename;
    std::filesystem::copy_file(source, target);
    return {target, filename};
}

void write_contact_sheet(const std::vector<Path>& screenshots, const Path& target) {
    if (screenshots.empty() || screenshots.size() > 48) {
        throw std::runtime_error("Select between 1 and 48 screenshots for a grid");
    }
    constexpr int width = 400, height = 225, gap = 12;
    const int columns = std::min(3, static_cast<int>(screenshots.size()));
    const int rows = (static_cast<int>(screenshots.size()) + columns - 1) / columns;
    Gdiplus::Bitmap canvas(gap + columns * (width + gap), gap + rows * (height + gap), PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&canvas);
        if (graphics.Clear(Gdiplus::Color(255, 24, 26, 28)) != Gdiplus::Ok) {
            throw std::runtime_error("Could not create the screenshot grid");
        }
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        for (int index = 0; index < static_cast<int>(screenshots.size()); ++index) {
            Gdiplus::Bitmap image(screenshots[index].c_str());
            const auto source_width = image.GetWidth();
            const auto source_height = image.GetHeight();
            if (image.GetLastStatus() != Gdiplus::Ok || !source_width || !source_height) {
                throw std::runtime_error("Could not read a screenshot for the grid");
            }
            const double scale = std::min(static_cast<double>(width) / source_width,
                                          static_cast<double>(height) / source_height);
            const int w = std::max(1, static_cast<int>(source_width * scale));
            const int h = std::max(1, static_cast<int>(source_height * scale));
            const int x = gap + index % columns * (width + gap) + (width - w) / 2;
            const int y = gap + index / columns * (height + gap) + (height - h) / 2;
            if (graphics.DrawImage(&image, x, y, w, h) != Gdiplus::Ok) {
                throw std::runtime_error("Could not draw a screenshot in the grid");
            }
        }
    }
    constexpr CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (canvas.Save(target.c_str(), &png) != Gdiplus::Ok) {
        throw std::runtime_error("Could not save the screenshot grid");
    }
}

Path publish(const Path& staged, const Path& folder, ShareFormat format) {
    for (int number = 1; number <= 10000; ++number) {
        auto name = std::wstring(format == ShareFormat::contact_sheet
            ? L"Switch Album Sync Screenshot Grid" : L"Switch Album Sync Share");
        if (number > 1) name += L" (" + std::to_wstring(number) + L")";
        if (format == ShareFormat::zip) name += L".zip";
        if (format == ShareFormat::contact_sheet) name += L".png";
        const auto target = folder / name;
        if (MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return target;
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    throw std::runtime_error("Could not publish the export");
}
}

bool is_share_screenshot(const Path& path) {
    const auto extension = folded(path.extension().wstring());
    return extension == L".jpg" || extension == L".jpeg" || extension == L".png";
}

std::filesystem::path pick_share_folder(HWND owner) {
    const auto status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(status)) return {};
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) return {};
    picker->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Choose where to save the export");
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    if (FAILED(picker->Show(owner)) || FAILED(picker->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return {};
    const Path path(name);
    CoTaskMemFree(name);
    return path;
}

std::filesystem::path export_captures(const std::vector<Path>& captures, const Path& folder, ShareFormat format, bool resize) {
    const bool zip = format == ShareFormat::zip;
    if (captures.empty() || (zip && captures.size() >= 65535)) throw std::runtime_error("Invalid export size");
    GUID id{};
    wchar_t unique[48]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, static_cast<int>(std::size(unique)))) {
        throw std::runtime_error("Could not create an export workspace");
    }
    const auto workspace = folder / (std::wstring(L".switch-album-sync-share-") + unique);
    if (!std::filesystem::create_directory(workspace)) throw std::runtime_error("Could not open export workspace");
    SetFileAttributesW(workspace.c_str(), FILE_ATTRIBUTE_HIDDEN);
    struct Cleanup {
        const Path& workspace;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(workspace, ignored); }
    } cleanup{workspace};
    if (format == ShareFormat::contact_sheet) {
        const Imaging imaging(true);
        std::vector<Path> screenshots;
        std::copy_if(captures.begin(), captures.end(), std::back_inserter(screenshots), is_share_screenshot);
        const auto sheet = workspace / L"grid.png";
        write_contact_sheet(screenshots, sheet);
        return publish(sheet, folder, format);
    }
    const auto copies = workspace / L"Captures";
    std::filesystem::create_directory(copies);
    const Imaging imaging(resize);
    std::set<std::wstring> names;
    if (!zip) {
        for (const auto& capture : captures) prepare_capture(capture, copies, names, resize, false);
        return publish(copies, folder, format);
    }

    const auto archive = workspace / L"export.zip";
    std::ofstream file(archive, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    std::vector<ZipEntry> entries;
    entries.reserve(captures.size());
    for (const auto& capture : captures) {
        const auto prepared = prepare_capture(capture, copies, names, resize, true);
        entries.push_back(append_zip_file(file, prepared.path, prepared.filename));
    }
    finish_zip(file, entries);
    file.close();
    return publish(archive, folder, format);
}

}
