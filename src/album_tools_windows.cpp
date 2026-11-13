#include "album_tools_windows.hpp"

#include "share_windows.hpp"
#include "theme_windows.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/util.hpp"

#include <bcrypt.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace nso {
namespace {

using Path = std::filesystem::path;
using Microsoft::WRL::ComPtr;

constexpr std::int64_t kSimilarWindowSeconds = 60;
constexpr int kSimilarMaxBitDifference = 6;
constexpr std::size_t kCleanupCandidates = 40;
constexpr std::size_t kHighlightLimit = 48;

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring size_text(std::uintmax_t bytes) {
    wchar_t text[32]{};
    if (bytes >= 1024ULL * 1024 * 1024) swprintf_s(text, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024) swprintf_s(text, L"%.1f MB", bytes / (1024.0 * 1024));
    else swprintf_s(text, L"%.0f KB", bytes / 1024.0);
    return text;
}

std::wstring date_text(std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
    wchar_t text[32]{};
    if (localtime_s(&local, &time) == 0) std::wcsftime(text, std::size(text), L"%Y-%m-%d", &local);
    return text;
}

std::wstring display_path(const Path& file, const Path& album) {
    const auto relative = file.lexically_relative(album);
    return relative.empty() || *relative.begin() == L".." ? file.wstring() : relative.wstring();
}

void message(HWND owner, const std::wstring& title, const std::wstring& text) {
    theme::message_box(owner, text, title, MB_OK);
}

// SHA-256 of a file, read in chunks so large videos don't need to fit in memory.
std::string file_digest(const Path& file, const std::function<bool()>& cancelled) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::string digest;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::ifstream input(file, std::ios::binary);
        std::vector<char> buffer(1 << 20);
        bool ok = static_cast<bool>(input);
        while (ok && input) {
            if (cancelled && cancelled()) { ok = false; break; }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = input.gcount();
            if (read > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                           static_cast<ULONG>(read), 0) < 0) ok = false;
        }
        unsigned char value[32]{};
        if (ok && BCryptFinishHash(hash, value, sizeof(value), 0) >= 0) {
            digest.assign(reinterpret_cast<const char*>(value), sizeof(value));
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

// Which copy of a duplicate to keep: one already sorted into a game folder,
// then the largest file (the best quality of look-alikes), then the shortest
// path, then alphabetical order.
bool better_keeper(const LocalCapture& left, const LocalCapture& right, const Path& album) {
    const auto rank = [&](const LocalCapture& capture) {
        const bool unsorted = capture.game_folder.lexically_normal() == album.lexically_normal();
        const bool imported = lower(path_to_utf8(capture.game_folder.filename())) == "imported captures";
        return std::make_tuple(unsorted, imported, ~capture.bytes, capture.path.native().size(), capture.path.native());
    };
    return rank(left) < rank(right);
}

Json::object read_object(const Path& file) {
    const auto data = read_local_data(file);
    return data.is_object() ? data.as_object() : Json::object{};
}

// Labels are keyed relative to the album; older entries may be absolute.
bool is_favourite(const Json::object& labels, const Path& file, const Path& album) {
    const auto relative = file.lexically_normal().lexically_relative(album.lexically_normal());
    for (const auto& key : {path_to_utf8(relative), path_to_utf8(file)}) {
        const auto found = labels.find(key);
        if (found != labels.end() && found->second.boolean("favorite")) return true;
    }
    return false;
}

std::vector<int> ask_which(HWND owner, const std::wstring& title, const std::wstring& heading, const std::wstring& text,
                           const std::vector<std::wstring>& choices, const std::vector<int>& selected,
                           const std::wstring& action) {
    theme::Dialog dialog;
    dialog.title = title;
    dialog.heading = heading;
    dialog.text = text;
    dialog.width = 720;
    dialog.choices = choices;
    dialog.selected = selected;
    dialog.buttons = {{IDCANCEL, L"Cancel"}, {IDOK, action, true}};
    if (theme::run_dialog(owner, dialog) != IDOK) return {};
    return dialog.selected;
}

bool remove_selected(HWND owner, const std::wstring& title, const std::vector<Path>& files) {
    if (files.empty()) return false;
    if (theme::message_box(owner, L"Move " + std::to_wstring(files.size()) +
            L" captures to the Recycle Bin? You can restore them from there.", title, MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return false;
    }
    const int moved = recycle_files(owner, files);
    message(owner, title, moved == static_cast<int>(files.size())
        ? L"Moved " + std::to_wstring(moved) + L" captures to the Recycle Bin."
        : L"Moved " + std::to_wstring(moved) + L" of " + std::to_wstring(files.size()) +
          L" captures. The rest may be open in another app.");
    return moved > 0;
}

bool find_duplicates_tool(HWND owner, const MenuState& state) {
    if (!state.capture_library || state.capture_library->empty()) {
        message(owner, L"Find duplicates", L"There are no captures in this album yet.");
        return false;
    }
    theme::Dialog intro;
    intro.title = L"Find duplicates";
    intro.heading = L"Find duplicate captures";
    intro.text = L"Looks for identical copies of the same capture anywhere in your album. "
                 L"Nothing is removed until you choose.";
    intro.checkbox_label = L"Also find screenshots that look the same (slower)";
    intro.checked = true;
    intro.buttons = {{IDCANCEL, L"Cancel"}, {IDOK, L"Find duplicates", true}};
    if (theme::run_dialog(owner, intro) != IDOK) return false;

    std::vector<DuplicateGroup> groups;
    const auto captures = state.capture_library;
    const auto album = state.destination;
    const bool similar = intro.checked;
    try {
        if (!run_with_progress(owner, L"Find duplicates", L"Comparing captures in your album…",
                [&](const std::atomic<bool>& cancelled) {
                    groups = find_duplicate_captures(*captures, album, similar, [&] { return cancelled.load(); });
                })) return false;
    } catch (const std::exception&) {
        message(owner, L"Find duplicates", L"Could not compare the album's captures.");
        return false;
    }
    std::vector<std::wstring> choices;
    std::vector<Path> candidates;
    std::vector<int> selected;
    for (const auto& group : groups) {
        for (std::size_t i = 1; i < group.files.size(); ++i) {
            if (group.exact) selected.push_back(static_cast<int>(choices.size()));
            choices.push_back((group.exact ? L"Identical: " : L"Looks the same: ") + display_path(group.files[i], album) +
                L"   ·   keeping " + display_path(group.files[0], album));
            candidates.push_back(group.files[i]);
        }
    }
    if (choices.empty()) {
        message(owner, L"Find duplicates", L"No duplicate captures found.");
        return false;
    }
    const auto chosen = ask_which(owner, L"Find duplicates", L"Choose duplicates to remove",
        std::to_wstring(choices.size()) + L" possible duplicates. Identical copies are selected; check captures that only "
        L"look the same before removing them. Click a row to toggle it.", choices, selected, L"Move to Recycle Bin");
    std::vector<Path> files;
    for (const int index : chosen) files.push_back(candidates.at(static_cast<std::size_t>(index)));
    return remove_selected(owner, L"Find duplicates", files);
}

bool cleanup_tool(HWND owner, const MenuState& state) {
    if (!state.capture_library || state.capture_library->empty()) {
        message(owner, L"Storage cleanup", L"There are no captures in this album yet.");
        return false;
    }
    std::vector<const LocalCapture*> largest;
    std::uintmax_t total = 0;
    for (const auto& capture : *state.capture_library) {
        largest.push_back(&capture);
        total += capture.bytes;
    }
    std::sort(largest.begin(), largest.end(), [](const auto* a, const auto* b) { return a->bytes > b->bytes; });
    if (largest.size() > kCleanupCandidates) largest.resize(kCleanupCandidates);
    std::vector<std::wstring> choices;
    for (const auto* capture : largest) {
        choices.push_back(size_text(capture->bytes) + L"   ·   " + display_path(capture->path, state.destination) +
            L"   ·   " + date_text(capture->modified) + (capture->video ? L"   ·   video" : L""));
    }
    const auto chosen = ask_which(owner, L"Storage cleanup", L"Free up space",
        L"Your album uses " + size_text(total) + L". These are its largest captures. Select any you no longer need; "
        L"they'll go to the Recycle Bin. More → Find duplicates finds extra copies.",
        choices, {}, L"Move to Recycle Bin");
    std::vector<Path> files;
    for (const int index : chosen) files.push_back(largest.at(static_cast<std::size_t>(index))->path);
    return remove_selected(owner, L"Storage cleanup", files);
}

bool highlights_tool(HWND owner, const MenuState& state) {
    struct Period {
        std::wstring label;
        std::vector<LocalCapture> screenshots;
    };
    std::map<std::string, Period, std::greater<>> periods;
    if (state.capture_library) for (const auto& capture : *state.capture_library) {
        if (capture.video || !is_share_screenshot(capture.path)) continue;
        const auto time = static_cast<std::time_t>(capture.modified);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) continue;
        wchar_t month[32]{};
        std::wcsftime(month, std::size(month), L"%B %Y", &local);
        char year_key[8], month_key[16];
        std::snprintf(year_key, sizeof(year_key), "%04d", local.tm_year + 1900);
        std::snprintf(month_key, sizeof(month_key), "%04d-%02d", local.tm_year + 1900, local.tm_mon + 1);
        // "2026-~" sorts after every month of 2026 in descending order, so
        // each year comes before its months.
        auto& year = periods[std::string(year_key) + "-~"];
        year.label = wide(year_key) + L", whole year";
        year.screenshots.push_back(capture);
        auto& entry = periods[month_key];
        entry.label = month;
        entry.screenshots.push_back(capture);
    }
    if (periods.empty()) {
        message(owner, L"Highlights", L"There are no screenshots in this album yet.");
        return false;
    }
    theme::Dialog dialog;
    dialog.title = L"Highlights";
    dialog.heading = L"Make a highlights grid";
    dialog.text = L"Pick a month or year. Up to 48 screenshots go into one PNG grid: favourites first, "
                  L"then a spread across the period.";
    dialog.width = 520;
    dialog.single_choice = true;
    dialog.selected = {0};
    std::vector<const Period*> order;
    for (const auto& [key, period] : periods) {
        dialog.choices.push_back(period.label + L"   ·   " + std::to_wstring(period.screenshots.size()) + L" screenshots");
        order.push_back(&period);
    }
    dialog.buttons = {{IDCANCEL, L"Cancel"}, {IDOK, L"Choose folder…", true}};
    if (theme::run_dialog(owner, dialog) != IDOK || dialog.selected.empty()) return false;
    const auto& period = *order.at(static_cast<std::size_t>(dialog.selected.front()));

    const auto labels = read_object(state.settings_directory / L"capture-labels.json");
    std::vector<bool> favourites;
    for (const auto& capture : period.screenshots) favourites.push_back(is_favourite(labels, capture.path, state.destination));
    const auto picks = pick_highlights(period.screenshots, favourites, kHighlightLimit);
    const auto folder = pick_share_folder(owner);
    if (folder.empty()) return false;
    Path saved;
    try {
        if (!run_with_progress(owner, L"Highlights", L"Making your highlights grid…",
                [&](const std::atomic<bool>&) { saved = export_captures(picks, folder, ShareFormat::contact_sheet, false); })) {
            return false;
        }
    } catch (const std::exception& error) {
        message(owner, L"Highlights", L"Could not make the grid. " + wide(error.what()));
        return false;
    }
    if (theme::message_box(owner, L"Saved " + std::to_wstring(picks.size()) + L" screenshots to " + saved.wstring() +
            L". Open it now?", L"Highlights", MB_YESNO) == IDYES) open_path(saved);
    return false;
}

constexpr char kBundleApp[] = "Switch Album Sync";
constexpr char kBundleKind[] = "settings";

bool export_settings_tool(HWND owner, const MenuState& state, const PlatformCallbacks& callbacks) {
    if (!callbacks.export_settings || state.settings_directory.empty()) return false;
    // Only album-relative labels travel: they're the ones that make sense on another PC.
    Json::object labels;
    for (const auto& [key, value] : read_object(state.settings_directory / L"capture-labels.json")) {
        if (!path_from_utf8(key).is_absolute()) labels.emplace(key, value);
    }
    Json::object collections;
    const auto preferences = Json(read_object(state.settings_directory / L"capture-library.json"));
    if (const auto* saved = preferences.find("collections"); saved && saved->is_object()) {
        for (const auto& [name, filters] : saved->as_object()) {
            if (!filters.is_object()) continue;
            auto copy = filters.as_object();
            const auto game = path_from_utf8(filters.string("game"));
            copy.erase("game");
            if (!game.empty()) copy["gameFolder"] = path_to_utf8(game.filename());
            collections.emplace(name, Json(std::move(copy)));
        }
    }
    const auto file = pick_save_file(owner, L"Export settings", L"Switch Album Sync settings.json",
                                     L"Switch Album Sync settings", L"*.json");
    if (file.empty()) return false;
    const Json bundle(Json::object{
        {"app", kBundleApp},
        {"kind", kBundleKind},
        {"version", 1},
        {"settings", callbacks.export_settings()},
        {"labels", Json(std::move(labels))},
        {"collections", Json(std::move(collections))},
    });
    message(owner, L"Export settings", write_local_data(file, bundle)
        ? L"Saved your preferences, tags, favourites, assigned games and smart collections to " + file.wstring() +
          L". Sign-in details aren't included."
        : L"Could not save the settings file.");
    return false;
}

bool import_settings_tool(HWND owner, const MenuState& state, const PlatformCallbacks& callbacks) {
    if (!callbacks.import_settings || state.settings_directory.empty()) return false;
    const auto file = pick_open_file(owner, L"Import settings", L"Switch Album Sync settings", L"*.json");
    if (file.empty()) return false;
    const auto bundle = read_local_data(file);
    if (!bundle.is_object() || bundle.string("app") != kBundleApp || bundle.string("kind") != kBundleKind) {
        message(owner, L"Import settings", L"That file isn't a Switch Album Sync settings export.");
        return false;
    }
    if (theme::message_box(owner,
            L"Replace your preferences, tags, favourites, assigned games and smart collections with the ones in this "
            L"file? Your sign-in, folders and whether Rich Presence is on stay as they are.",
            L"Import settings", MB_YESNO | MB_DEFBUTTON2) != IDYES) return false;

    const auto* settings = bundle.find("settings");
    const bool applied = settings && callbacks.import_settings(*settings);

    const auto labels_file = state.settings_directory / L"capture-labels.json";
    auto labels = read_object(labels_file);
    if (const auto* imported = bundle.find("labels"); imported && imported->is_object()) {
        for (const auto& [key, value] : imported->as_object()) {
            if (!path_from_utf8(key).is_absolute() && value.is_object()) labels[key] = value;
        }
    }
    const auto preferences_file = state.settings_directory / L"capture-library.json";
    auto preferences = read_object(preferences_file);
    if (const auto* imported = bundle.find("collections"); imported && imported->is_object()) {
        Json::object collections;
        for (const auto& [name, filters] : imported->as_object()) {
            if (!filters.is_object()) continue;
            auto copy = filters.as_object();
            const auto folder = filters.string("gameFolder");
            copy.erase("gameFolder");
            copy["game"] = folder.empty() ? std::string{} : path_to_utf8(state.destination / path_from_utf8(folder).filename());
            collections.emplace(name, Json(std::move(copy)));
        }
        preferences["collections"] = Json(std::move(collections));
    }
    const bool saved = write_local_data(labels_file, Json(labels)) && write_local_data(preferences_file, Json(preferences));
    message(owner, L"Import settings", applied && saved ? L"Settings imported." :
        L"Some settings couldn't be imported. Check access to the Switch Album Sync settings folder.");
    return true;
}

}  // namespace

bool run_with_progress(HWND owner, const std::wstring& title, const std::wstring& text,
                       const std::function<void(const std::atomic<bool>& cancelled)>& work) {
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto task = std::async(std::launch::async, [work, cancelled] { work(*cancelled); });
    theme::Dialog progress;
    progress.title = title;
    progress.heading = title;
    progress.text = text;
    progress.buttons = {{IDCANCEL, L"Cancel", true}};
    progress.completed = [&task] { return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    if (theme::run_dialog(owner, progress) != IDOK) {
        cancelled->store(true);
        task.wait();
        return false;
    }
    task.get();
    return true;
}

int recycle_files(HWND owner, const std::vector<Path>& files) {
    std::wstring list;
    for (const auto& file : files) {
        list += file.wstring();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.hwnd = owner;
    operation.wFunc = FO_DELETE;
    operation.pFrom = list.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&operation);
    int moved = 0;
    for (const auto& file : files) {
        std::error_code error;
        if (!std::filesystem::exists(file, error)) ++moved;
    }
    return moved;
}

namespace {
Path show_file_dialog(HWND owner, bool save, const std::wstring& title, const Path& suggested,
                      const std::wstring& filter_name, const std::wstring& pattern) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Path chosen;
    {
        ComPtr<IFileDialog> dialog;
        const HRESULT created = save
            ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
            : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(created)) {
            const COMDLG_FILTERSPEC filter{filter_name.c_str(), pattern.c_str()};
            dialog->SetFileTypes(1, &filter);
            dialog->SetTitle(title.c_str());
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
            const auto extension = Path(pattern).extension().wstring();
            if (extension.size() > 1) dialog->SetDefaultExtension(extension.c_str() + 1);
            if (!suggested.empty()) {
                dialog->SetFileName(suggested.filename().c_str());
                if (suggested.has_parent_path()) {
                    ComPtr<IShellItem> folder;
                    if (SUCCEEDED(SHCreateItemFromParsingName(suggested.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                        dialog->SetFolder(folder.Get());
                    }
                }
            }
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                chosen = path;
                CoTaskMemFree(path);
            }
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return chosen;
}
}  // namespace

Path pick_save_file(HWND owner, const std::wstring& title, const Path& suggested,
                    const std::wstring& filter_name, const std::wstring& pattern) {
    return show_file_dialog(owner, true, title, suggested, filter_name, pattern);
}

Path pick_open_file(HWND owner, const std::wstring& title, const std::wstring& filter_name, const std::wstring& pattern) {
    return show_file_dialog(owner, false, title, {}, filter_name, pattern);
}

std::uint64_t image_fingerprint(const Path& image) {
    Gdiplus::Bitmap source(image.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok || !source.GetWidth() || !source.GetHeight()) return 0;
    Gdiplus::Bitmap reduced(9, 8, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&reduced);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        if (graphics.DrawImage(&source, 0, 0, 9, 8) != Gdiplus::Ok) return 0;
    }
    Gdiplus::Rect area(0, 0, 9, 8);
    Gdiplus::BitmapData data{};
    if (reduced.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return 0;
    double gray[8][9]{};
    for (int y = 0; y < 8; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < 9; ++x) {
            const auto* pixel = row + x * 4;
            gray[y][x] = 0.114 * pixel[0] + 0.587 * pixel[1] + 0.299 * pixel[2];
        }
    }
    reduced.UnlockBits(&data);
    std::uint64_t bits = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) bits = (bits << 1) | (gray[y][x] > gray[y][x + 1] ? 1ULL : 0ULL);
    }
    // A flat image hashes to 0; keep it distinguishable from "couldn't read".
    return bits ? bits : 1;
}

std::vector<DuplicateGroup> find_duplicate_captures(const std::vector<LocalCapture>& captures,
    const Path& album, bool similar, const std::function<bool()>& cancelled) {
    const auto stop = [&] { return cancelled && cancelled(); };
    std::vector<DuplicateGroup> groups;
    std::set<std::size_t> grouped;

    // Identical: same size first (cheap), then the same SHA-256.
    std::map<std::uintmax_t, std::vector<std::size_t>> by_size;
    for (std::size_t i = 0; i < captures.size(); ++i) by_size[captures[i].bytes].push_back(i);
    for (const auto& [size, indexes] : by_size) {
        if (indexes.size() < 2) continue;
        std::map<std::string, std::vector<std::size_t>> by_digest;
        for (const auto index : indexes) {
            if (stop()) return {};
            const auto digest = file_digest(captures[index].path, cancelled);
            if (!digest.empty()) by_digest[digest].push_back(index);
        }
        for (auto& [digest, same] : by_digest) {
            if (same.size() < 2) continue;
            std::sort(same.begin(), same.end(), [&](std::size_t a, std::size_t b) {
                return better_keeper(captures[a], captures[b], album);
            });
            DuplicateGroup group;
            for (const auto index : same) {
                group.files.push_back(captures[index].path);
                // The kept copy still takes part in look-alike matching below.
                if (index != same.front()) grouped.insert(index);
            }
            groups.push_back(std::move(group));
        }
    }
    if (!similar) return groups;

    // Looks the same: screenshots taken within a minute of each other whose
    // fingerprints differ in only a few bits.
    std::vector<std::size_t> shots;
    for (std::size_t i = 0; i < captures.size(); ++i) {
        if (!captures[i].video && !grouped.contains(i)) shots.push_back(i);
    }
    std::sort(shots.begin(), shots.end(), [&](std::size_t a, std::size_t b) {
        return captures[a].modified < captures[b].modified;
    });
    std::unordered_map<std::size_t, std::uint64_t> prints;
    const auto print = [&](std::size_t index) {
        const auto found = prints.find(index);
        if (found != prints.end()) return found->second;
        return prints[index] = image_fingerprint(captures[index].path);
    };
    std::vector<std::size_t> parent(captures.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&](std::size_t index) {
        while (parent[index] != index) index = parent[index] = parent[parent[index]];
        return index;
    };
    for (std::size_t a = 0; a < shots.size(); ++a) {
        if (stop()) return {};
        for (std::size_t b = a + 1; b < shots.size() &&
             captures[shots[b]].modified - captures[shots[a]].modified <= kSimilarWindowSeconds; ++b) {
            const auto left = print(shots[a]), right = print(shots[b]);
            if (!left || !right || std::popcount(left ^ right) > kSimilarMaxBitDifference) continue;
            parent[root(shots[b])] = root(shots[a]);
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> clusters;
    for (const auto index : shots) clusters[root(index)].push_back(index);
    for (auto& [key, members] : clusters) {
        if (members.size() < 2) continue;
        std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
            return better_keeper(captures[a], captures[b], album);
        });
        DuplicateGroup group;
        group.exact = false;
        for (const auto index : members) group.files.push_back(captures[index].path);
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<Path> pick_highlights(const std::vector<LocalCapture>& period, const std::vector<bool>& favourite,
                                  std::size_t limit) {
    std::vector<std::size_t> order(period.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return period[a].modified < period[b].modified; });
    std::vector<std::size_t> chosen, rest;
    for (const auto index : order) {
        if (index < favourite.size() && favourite[index] && chosen.size() < limit) chosen.push_back(index);
        else rest.push_back(index);
    }
    const auto room = std::min(limit - chosen.size(), rest.size());
    for (std::size_t i = 0; i < room; ++i) chosen.push_back(rest[i * rest.size() / room]);
    std::sort(chosen.begin(), chosen.end(), [&](std::size_t a, std::size_t b) { return period[a].modified < period[b].modified; });
    std::vector<Path> paths;
    for (const auto index : chosen) paths.push_back(period[index].path);
    return paths;
}

bool run_album_tool(HWND owner, AlbumTool tool, const MenuState& state, const PlatformCallbacks& callbacks) {
    switch (tool) {
        case AlbumTool::duplicates: return find_duplicates_tool(owner, state);
        case AlbumTool::cleanup: return cleanup_tool(owner, state);
        case AlbumTool::highlights: return highlights_tool(owner, state);
        case AlbumTool::export_settings: return export_settings_tool(owner, state, callbacks);
        case AlbumTool::import_settings: return import_settings_tool(owner, state, callbacks);
    }
    return false;
}

}  // namespace nso
