#pragma once

#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/windows_compat.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nso {

// Everything here works on files on this PC. None of it contacts Nintendo,
// nxapi or any other online service.

enum class AlbumTool { duplicates, cleanup, highlights, export_settings, import_settings };

// Runs a tool from the dashboard's Album tools menu. Returns true when files
// or saved data changed, so the caller can refresh the album and library.
bool run_album_tool(HWND owner, AlbumTool tool, const MenuState& state, const PlatformCallbacks& callbacks);

// Shows a cancellable progress dialog while `work` runs on another thread.
// Returns false if the user cancelled; rethrows anything `work` threw.
bool run_with_progress(HWND owner, const std::wstring& title, const std::wstring& text,
                       const std::function<void(const std::atomic<bool>& cancelled)>& work);

// Moves files to the Recycle Bin, so they can be restored. Returns how many moved.
int recycle_files(HWND owner, const std::vector<std::filesystem::path>& files);

std::filesystem::path pick_save_file(HWND owner, const std::wstring& title, const std::filesystem::path& suggested,
                                     const std::wstring& filter_name, const std::wstring& pattern);
std::filesystem::path pick_open_file(HWND owner, const std::wstring& title,
                                     const std::wstring& filter_name, const std::wstring& pattern);

struct DuplicateGroup {
    // files[0] is the copy to keep; the rest are the duplicates.
    std::vector<std::filesystem::path> files;
    bool exact = true;
};

// Finds byte-for-byte identical captures and, if `similar` is set,
// screenshots that look the same and were taken within a minute of each other.
std::vector<DuplicateGroup> find_duplicate_captures(const std::vector<LocalCapture>& captures,
    const std::filesystem::path& album, bool similar, const std::function<bool()>& cancelled);

// A 64-bit difference hash of an image (0 if it can't be read). Images that
// look alike differ in only a few bits.
std::uint64_t image_fingerprint(const std::filesystem::path& image);

// Up to `limit` screenshots from a period for a highlights grid: favourites
// first, then the rest spread evenly across the period.
std::vector<std::filesystem::path> pick_highlights(const std::vector<LocalCapture>& period,
    const std::vector<bool>& favourite, std::size_t limit);

}  // namespace nso
