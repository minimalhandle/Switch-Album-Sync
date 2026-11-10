#include "import_windows.hpp"
#include "theme_windows.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/sync.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <array>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace nso {
namespace {
using Microsoft::WRL::ComPtr;
using Path = std::filesystem::path;
constexpr wchar_t kReviewDirectory[] = L".switch-album-sync-import-review";
constexpr wchar_t kMatchFile[] = L"match.json";

bool plain_directory(const Path& path) {
    const auto flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) &&
        !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool plain_file(const Path& path) {
    const auto flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && !(flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::string watch_stamp(const Path& path, std::uintmax_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    const auto number = [](DWORD high, DWORD low) {
        return (static_cast<std::uint64_t>(high) << 32) | low;
    };
    size = number(data.nFileSizeHigh, data.nFileSizeLow);
    if (!size) return {};
    return std::to_string(size) + ":" +
        std::to_string(number(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime)) + ":" +
        std::to_string(number(data.ftCreationTime.dwHighDateTime, data.ftCreationTime.dwLowDateTime));
}

void mark_arrival(const Path& path) {
    const auto file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(file, &now, nullptr, nullptr);
    CloseHandle(file);
}

struct ComApartment {
    const HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
};

struct Pidl {
    PIDLIST_ABSOLUTE value = nullptr;
    ~Pidl() { CoTaskMemFree(value); }
};

struct ShellMemory {
    void operator()(void* pointer) const { CoTaskMemFree(pointer); }
};

std::wstring display_name(IShellItem* item, SIGDN mode) {
    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(mode, &text))) return {};
    const std::unique_ptr<wchar_t, ShellMemory> value(text);
    return value.get() ? value.get() : L"";
}

std::wstring folded(std::wstring name) {
    if (!name.empty()) CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    return name;
}

std::wstring media_type(const Path& file) {
    auto extension = folded(file.extension().wstring());
    if (extension == L".jpeg") extension = L".jpg";
    if (extension == L".jpg" || extension == L".png" || extension == L".mp4") return extension;
    return {};
}

bool safe_name(const std::wstring& name) {
    return !name.empty() && name.back() != L'.' && name.back() != L' ' &&
        name.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos &&
        std::none_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; });
}

bool within_album(const Path& album, const Path& file) {
    const auto relative = file.lexically_relative(album);
    if (relative.empty() || relative.has_root_path()) return false;
    auto current = album;
    for (const auto& component : relative.parent_path()) {
        if (!safe_name(component.wstring()) || component == kReviewDirectory) return false;
        current /= component;
        if (!plain_directory(current)) return false;
    }
    return safe_name(relative.filename().wstring());
}

bool same_bytes(const Path& first, const Path& second) {
    std::error_code error;
    const auto size = std::filesystem::file_size(first, error);
    if (error || size != std::filesystem::file_size(second, error) || error) return false;
    std::ifstream left(first, std::ios::binary), right(second, std::ios::binary);
    std::array<char, 16384> a{}, b{};
    auto remaining = size;
    while (remaining) {
        const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(remaining, a.size()));
        if (!left.read(a.data(), count) || !right.read(b.data(), count) ||
            !std::equal(a.begin(), a.begin() + count, b.begin())) return false;
        remaining -= count;
    }
    return true;
}

bool record_match(const Path& album, const Path& existing, const Path& incoming) {
    return write_local_data(incoming.parent_path() / kMatchFile, Json::object{
        {"existing", path_to_utf8(existing.lexically_relative(album))},
        {"incoming", path_to_utf8(incoming.filename())}});
}

Path online_slot(const Path& album, const Path& filename) {
    return album / kReviewDirectory / (L"online-" + filename.wstring());
}

std::wstring capture_key(const Path& file) {
    const auto stem = folded(file.stem().wstring());
    if (stem.size() <= 16 || (stem[16] != L'-' && stem[16] != L'_')) return {};
    if (!std::all_of(stem.begin(), stem.begin() + 16, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return {};
    return stem.substr(0, 14) + media_type(file);
}

std::wstring game_signature(const Path& file) {
    const auto stem = folded(file.stem().wstring());
    if (stem.size() < 49 || stem[16] != L'-' || !stem.substr(0, 16).starts_with(L"20") ||
        capture_key(file).empty() || (stem.size() > 49 && stem[49] != L'-')) return {};
    const auto signature = stem.substr(17, 32);
    if (signature.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) return {};
    return signature;
}

void remember_game(std::map<std::wstring, std::wstring>& games, const std::wstring& key,
                   const std::wstring& game) {
    if (key.empty() || !safe_name(game) || folded(game) == L"imported captures") return;
    const auto [entry, inserted] = games.try_emplace(key, game);
    if (!inserted && folded(entry->second) != folded(game)) entry->second.clear();
}

bool synced_name(const Path& path) {
    return folded(path.stem().wstring()).ends_with(L"_c");
}

std::wstring item_filename(IShellItem* item, const Path& filesystem_path) {
    if (!filesystem_path.empty()) return filesystem_path.filename().wstring();
    ComPtr<IShellItem2> properties;
    PWSTR name = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&properties))) &&
        SUCCEEDED(properties->GetString(PKEY_FileName, &name))) {
        std::unique_ptr<wchar_t, ShellMemory> value(name);
        if (value && !media_type(Path(value.get())).empty()) return value.get();
    }
    auto display = display_name(item, SIGDN_NORMALDISPLAY);
    if (!media_type(Path(display)).empty()) return display;
    const auto parsing = display_name(item, SIGDN_PARENTRELATIVEPARSING);
    return media_type(Path(parsing)).empty() ? display : parsing;
}

bool skip_link(IShellItem* item, const Path& path) {
    SFGAOF flags = 0;
    if (item->GetAttributes(SFGAO_LINK, &flags) == S_OK && (flags & SFGAO_LINK)) return true;
    if (path.empty()) return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool shell_copy(IShellItem* item, const Path& staging) {
    ComPtr<IFileOperation> operation;
    ComPtr<IShellItem> folder;
    BOOL aborted = FALSE;
    return SUCCEEDED(SHCreateItemFromParsingName(staging.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation))) &&
        SUCCEEDED(operation->SetOperationFlags(FOF_NOERRORUI | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_SILENT)) &&
        SUCCEEDED(operation->CopyItem(item, folder.Get(), nullptr, nullptr)) &&
        SUCCEEDED(operation->PerformOperations()) &&
        SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && !aborted;
}

void set_capture_time(IShellItem* item, const Path& source, const Path& target) {
    if (!source.empty()) {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(source, error);
        if (!error) std::filesystem::last_write_time(target, modified, error);
        return;
    }
    ComPtr<IShellItem2> properties;
    FILETIME modified{};
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&properties))) ||
        FAILED(properties->GetFileTime(PKEY_DateModified, &modified))) return;
    const HANDLE file = CreateFileW(target.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SetFileTime(file, nullptr, nullptr, &modified);
        CloseHandle(file);
    }
}

struct Importer {
    const Path& album;
    const Path& staging;
    const std::function<bool()>& cancelled;
    ImportResult result;
    std::map<std::wstring, Path> names;
    std::map<std::wstring, Path> timestamps;
    std::map<std::wstring, Path> synced_timestamps;
    std::vector<ImportMatch> pending;

    Importer(const Path& target, const Path& temporary, const std::function<bool()>& stop,
             const std::vector<LocalCapture>& library)
        : album(target), staging(temporary), cancelled(stop) {
        if (!staging.empty()) pending = pending_import_matches(album);
        for (const auto& capture : library) remember(capture.path);
    }

    void remember(const Path& path) {
        names.try_emplace(folded(path.filename().wstring()), path);
        const auto key = capture_key(path);
        if (key.empty()) return;
        timestamps.try_emplace(key, path);
        if (synced_name(path)) synced_timestamps.try_emplace(key, path);
    }

    Path match_for(const Path& file) const {
        if (const auto name = names.find(folded(file.filename().wstring())); name != names.end()) return name->second;
        const auto key = capture_key(file);
        if (key.empty()) return {};
        const auto& index = synced_name(file) ? timestamps : synced_timestamps;
        const auto found = index.find(key);
        return found == index.end() ? Path{} : found->second;
    }

    bool queue_match(const Path& existing, const Path& temporary) {
        if (existing.filename() == temporary.filename() && same_bytes(existing, temporary)) {
            ++result.skipped;
            return true;
        }
        for (const auto& pair : pending) {
            if (pair.existing == existing && pair.incoming.filename() == temporary.filename() &&
                same_bytes(pair.incoming, temporary)) {
                ++result.skipped;
                return true;
            }
        }
        const auto review = album / kReviewDirectory;
        const auto slot = review / (staging.filename().wstring() + L"-" + std::to_wstring(result.review));
        std::error_code error;
        std::filesystem::create_directory(review, error);
        if (error || !plain_directory(review) || !std::filesystem::create_directory(slot, error)) return false;
        const auto incoming = slot / temporary.filename();
        const bool saved = MoveFileExW(temporary.c_str(), incoming.c_str(), MOVEFILE_WRITE_THROUGH) &&
            record_match(album, existing, incoming);
        if (!saved) {
            std::filesystem::remove(incoming, error);
            std::filesystem::remove(slot, error);
            return false;
        }
        pending.push_back({existing, incoming});
        ++result.review;
        return true;
    }

    void copy(IShellItem* item, const Path& source, const std::wstring& name, const std::wstring& game) {
        const Path file(name);
        if (media_type(file).empty()) return;
        if (!safe_name(name)) { ++result.failed; return; }
        const auto existing = match_for(file);
        const auto destination = album / (safe_name(game) ? game : L"Imported captures") / file;
        std::error_code error;
        if (existing.empty()) std::filesystem::create_directories(destination.parent_path(), error);
        if (existing.empty() && (error || !plain_directory(destination.parent_path()))) {
            ++result.failed;
            return;
        }
        const auto temporary = staging / name;
        const bool copied = source.empty() ? shell_copy(item, temporary) :
            std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::none, error);
        const auto size = std::filesystem::file_size(temporary, error);
        bool valid = copied && !error && size > 0;
        if (valid && !source.empty()) {
            const auto expected = std::filesystem::file_size(source, error);
            valid = !error && expected == size;
        } else if (valid) {
            ComPtr<IShellItem2> properties;
            ULONGLONG expected = 0;
            if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&properties))) &&
                SUCCEEDED(properties->GetUInt64(PKEY_Size, &expected)) && expected != 0) valid = expected == size;
        }
        if (valid && !cancelled()) {
            set_capture_time(item, source, temporary);
            mark_arrival(temporary);
            if (!existing.empty()) {
                if (!queue_match(existing, temporary)) ++result.failed;
            } else if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                remember(destination);
                ++result.copied;
                result.copied_paths.push_back(destination);
                return;
            } else {
                const auto failure = GetLastError();
                if (failure == ERROR_FILE_EXISTS || failure == ERROR_ALREADY_EXISTS) {
                    if (!queue_match(destination, temporary)) ++result.failed;
                } else ++result.failed;
            }
        } else if (!cancelled()) ++result.failed;
        std::filesystem::remove(temporary, error);
    }

    void visit(IShellItem* folder, const std::wstring& game, ImportPlan& plan, int depth = 0) {
        if (cancelled()) return;
        if (depth > 32) { ++result.failed; return; }
        ComPtr<IEnumShellItems> children;
        if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&children)))) {
            ++result.failed;
            return;
        }
        for (;;) {
            if (cancelled()) return;
            ComPtr<IShellItem> child;
            const HRESULT next = children->Next(1, &child, nullptr);
            if (next != S_OK) {
                if (FAILED(next)) ++result.failed;
                break;
            }
            const Path local(display_name(child.Get(), SIGDN_FILESYSPATH));
            if (skip_link(child.Get(), local)) continue;
            SFGAOF flags = 0;
            if (FAILED(child->GetAttributes(SFGAO_FOLDER, &flags))) { ++result.failed; continue; }
            if (flags & SFGAO_FOLDER) {
                if (!local.empty() && (folded(local.lexically_normal().wstring()) == folded(album.lexically_normal().wstring()) ||
                    folded(local.lexically_normal().wstring()) == folded(staging.lexically_normal().wstring()))) continue;
                const auto name = local.empty() ? display_name(child.Get(), SIGDN_NORMALDISPLAY) : local.filename().wstring();
                const auto normalized_name = folded(name);
                if (normalized_name.starts_with(L".switch-album-sync-import-") ||
                    normalized_name.starts_with(L".switch-album-sync-share-")) continue;
                const bool dated = name.size() == 4 && std::all_of(name.begin(), name.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
                const bool album_wrapper = depth == 0 && folded(name) == L"album";
                const auto next_game = depth == 0 && !dated && !album_wrapper ? name : game;
                visit(child.Get(), next_game, plan, album_wrapper ? depth : depth + 1);
            } else {
                auto name = item_filename(child.Get(), local);
                if (media_type(Path(name)).empty()) continue;
                if (!safe_name(name)) { ++result.failed; continue; }
                ImportCandidate candidate{local, {}, std::move(name), game, {}, false, {}, {}};
                if (local.empty()) {
                    Pidl id;
                    if (FAILED(SHGetIDListFromObject(child.Get(), &id.value)) || !id.value) {
                        ++result.failed;
                        continue;
                    }
                    candidate.shell_id.assign(reinterpret_cast<const char*>(id.value), ILGetSize(id.value));
                    ComPtr<IShellItem2> properties;
                    ULONGLONG size = 0;
                    if (SUCCEEDED(child->QueryInterface(IID_PPV_ARGS(&properties))) &&
                        SUCCEEDED(properties->GetUInt64(PKEY_Size, &size))) candidate.bytes = size;
                } else {
                    std::error_code error;
                    const auto size = std::filesystem::file_size(local, error);
                    if (!error) candidate.bytes = size;
                }
                candidate.possible_duplicate = !match_for(Path(candidate.name)).empty();
                plan.captures.push_back(std::move(candidate));
            }
        }
    }
};

void append_import_matches(const Path& album, std::vector<ImportMatch>& matches) {
    const auto review = album / kReviewDirectory;
    if (!plain_directory(review)) return;
    std::error_code error;
    for (auto slot = std::filesystem::directory_iterator(review, error);
         !error && slot != std::filesystem::directory_iterator(); slot.increment(error)) {
        if (!plain_directory(slot->path()) || !plain_file(slot->path() / kMatchFile)) continue;
        const auto metadata = read_local_data(slot->path() / kMatchFile);
        const auto incoming_name = path_from_utf8(metadata.string("incoming"));
        const auto existing_name = path_from_utf8(metadata.string("existing"));
        if (!safe_name(incoming_name.wstring()) || existing_name.has_root_path()) continue;
        const auto incoming = slot->path() / incoming_name;
        const auto existing = album / existing_name;
        if (media_type(incoming).empty() || !plain_file(incoming) || !within_album(album, existing)) continue;
        matches.push_back({existing, incoming});
    }
}
}

void suggest_import_games(ImportPlan& plan, const std::vector<LocalCapture>& library) {
    const auto timestamp = [](const Path& file) {
        if (capture_key(file).empty()) return std::wstring{};
        return file.stem().wstring().substr(0, 16) + media_type(file);
    };
    std::map<std::wstring, std::wstring> signatures;
    std::map<std::wstring, std::wstring> timestamps;
    for (const auto& capture : library) {
        if (capture.game_folder == plan.album) continue;
        const auto game = capture.game_folder.filename().wstring();
        remember_game(signatures, game_signature(capture.path), game);
        remember_game(timestamps, timestamp(capture.path), game);
    }
    const auto lookup = [](const auto& games, const std::wstring& key) {
        const auto match = games.find(key);
        return match == games.end() ? std::wstring{} : match->second;
    };
    // A timestamp match can teach the game signature for the rest of a batch.
    for (const auto& capture : plan.captures) {
        const Path file(capture.name);
        const auto game = folded(capture.game) == L"imported captures"
            ? lookup(timestamps, timestamp(file)) : capture.game;
        remember_game(signatures, game_signature(file), game);
    }
    for (auto& capture : plan.captures) {
        capture.suggested_game.clear();
        if (folded(capture.game) != L"imported captures") continue;
        const Path file(capture.name);
        capture.suggested_game = lookup(timestamps, timestamp(file));
        if (capture.suggested_game.empty()) {
            capture.suggested_game = lookup(signatures, game_signature(file));
        }
    }
}

std::vector<ImportMatch> pending_import_matches(const Path& album) {
    std::vector<ImportMatch> matches;
    append_import_matches(album, matches);
    const auto legacy = album / L"Album";
    if (is_default_album_folder(album) && plain_directory(legacy)) append_import_matches(legacy, matches);
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.incoming < b.incoming; });
    return matches;
}

bool resolve_import_match(const Path& album, const ImportMatch& match, bool keep_both) {
    const auto slot = match.incoming.parent_path();
    const auto legacy = album / L"Album";
    const auto& root = slot.parent_path() == legacy / kReviewDirectory &&
        is_default_album_folder(album) && plain_directory(legacy) ? legacy : album;
    const auto review = root / kReviewDirectory;
    if (slot.parent_path() != review || !plain_directory(review) || !plain_directory(slot) ||
        !plain_file(match.incoming) || !within_album(root, match.existing)) return false;
    std::error_code error;
    if (keep_both) {
        const auto folder = match.existing.parent_path();
        const auto stem = match.incoming.stem().wstring();
        const auto extension = match.incoming.extension().wstring();
        bool moved = false;
        for (int number = 1; number <= 1000; ++number) {
            const auto suffix = number == 1 ? L" (imported)" : L" (imported " + std::to_wstring(number) + L")";
            const auto target = folder / (stem + suffix + extension);
            if (MoveFileExW(match.incoming.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                mark_arrival(target);
                moved = true;
                break;
            }
            const auto failure = GetLastError();
            if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS) return false;
        }
        if (!moved) return false;
    } else if (!plain_file(match.existing) || !std::filesystem::remove(match.incoming, error)) {
        return false;
    }
    // Keep a receipt for Nintendo matches so future syncs don't fetch the same copy again.
    if (!slot.filename().wstring().starts_with(L"online-")) {
        std::filesystem::remove(slot / kMatchFile, error);
        std::filesystem::remove(slot, error);
    }
    return true;
}

bool recorded_online_match(const Path& album, const Path& filename) {
    const auto recorded = [&](const Path& root) {
        const auto slot = online_slot(root, filename);
        return plain_directory(slot.parent_path()) && plain_directory(slot) && plain_file(slot / kMatchFile);
    };
    const auto legacy = album / L"Album";
    return recorded(album) || (is_default_album_folder(album) && plain_directory(legacy) && recorded(legacy));
}

void stage_online_match(const Path& album, const Path& filename, const Path& existing,
                        const std::function<void(const Path&)>& save) {
    const auto slot = online_slot(album, filename);
    const auto root = slot.parent_path();
    const auto incoming = slot / filename;
    std::error_code error;
    std::filesystem::create_directory(root, error);
    if (error || !plain_directory(root) || !within_album(album, existing) || !plain_file(existing)) {
        throw std::runtime_error("Cannot open duplicate review folder");
    }
    std::filesystem::create_directory(slot, error);
    if (error || !plain_directory(slot)) throw std::runtime_error("Cannot stage a duplicate capture");
    try {
        save(incoming);
        if (!record_match(album, existing, incoming)) throw std::runtime_error("Cannot save duplicate review entry");
    } catch (...) {
        std::filesystem::remove(incoming, error);
        std::filesystem::remove(slot, error);
        throw;
    }
}

static std::string pick_import_source(HWND owner) {
    const ComApartment com;
    ComPtr<IFileOpenDialog> picker;
    if (FAILED(com.status) || FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    picker->GetOptions(&options);
    picker->SetOptions((options & ~FOS_FORCEFILESYSTEM) | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Import from Nintendo Switch USB or microSD");
    picker->SetOkButtonLabel(L"Preview captures");
    ComPtr<IShellItem> item;
    Pidl id;
    if (FAILED(picker->Show(owner)) || FAILED(picker->GetResult(&item)) ||
        FAILED(SHGetIDListFromObject(item.Get(), &id.value)) || !id.value) return {};
    return {reinterpret_cast<const char*>(id.value), ILGetSize(id.value)};
}

static ImportPlan scan_import_source(const std::string& source, const Path& target,
                                     const std::function<bool()>& cancelled) {
    ImportPlan plan;
    plan.album = std::filesystem::absolute(target).lexically_normal().make_preferred();
    const auto& album = plan.album;
    const ComApartment com;
    ComPtr<IShellItem> root;
    if (FAILED(com.status) || source.empty() ||
        FAILED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(source.data()), IID_PPV_ARGS(&root)))) {
        throw std::runtime_error("Could not open the import source");
    }
    Path local(display_name(root.Get(), SIGDN_FILESYSPATH));
    if (!local.empty()) {
        std::error_code error;
        for (const auto& candidate : {local / L"Nintendo" / L"Album", local / L"Album"}) {
            ComPtr<IShellItem> detected;
            if (std::filesystem::is_directory(candidate, error) &&
                SUCCEEDED(SHCreateItemFromParsingName(candidate.c_str(), nullptr, IID_PPV_ARGS(&detected)))) {
                root = std::move(detected);
                local = candidate;
                break;
            }
        }
        if (local == local.root_path()) throw std::runtime_error("Choose an album or capture folder");
        if (folded(local.lexically_normal().wstring()) == folded(album.lexically_normal().wstring())) {
            throw std::runtime_error("Choose a source outside the destination album");
        }
    }
    const Path no_staging;
    const auto library = scan_capture_library(album, cancelled);
    Importer importer(album, no_staging, cancelled, library);
    auto source_name = display_name(root.Get(), SIGDN_NORMALDISPLAY);
    const auto name = folded(source_name);
    if (!local.empty() || name == L"album" || name.starts_with(L"nintendo switch")) {
        source_name = L"Imported captures";
    }
    importer.visit(root.Get(), source_name, plan);
    if (!cancelled()) suggest_import_games(plan, library);
    plan.scan_errors = importer.result.failed;
    return plan;
}

std::shared_ptr<const ImportPlan> pick_import_plan(HWND owner, const Path& album) {
    const auto source = pick_import_source(owner);
    if (source.empty()) return {};
    ImportPlan plan;
    try {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto scan = std::async(std::launch::async, [source, album, cancelled] {
            return scan_import_source(source, album, [cancelled] { return cancelled->load(); });
        });
        theme::Dialog progress;
        progress.title = L"Import preview";
        progress.heading = L"Scanning for captures";
        progress.text = L"Switch Album Sync is reading filenames, sizes, and possible duplicates.";
        progress.buttons = {{IDCANCEL, L"Cancel", true}};
        progress.completed = [&scan] { return scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
        if (theme::run_dialog(owner, progress) != IDOK) {
            cancelled->store(true);
            return {};
        }
        plan = scan.get();
    } catch (const std::exception&) {
        theme::message_box(owner, L"Choose a readable capture folder outside your destination album.",
            L"Import preview", MB_OK);
        return {};
    }
    if (plan.captures.empty()) {
        theme::message_box(owner, plan.scan_errors ?
            L"No captures found. Some folders or files could not be read." : L"No captures found.",
            L"Import preview", MB_OK);
        return {};
    }

    theme::Dialog dialog;
    dialog.title = L"Import preview";
    dialog.heading = L"Choose captures to import";
    dialog.width = 660;
    dialog.buttons = {{IDCANCEL, L"Cancel"}, {IDOK, L"Import selected", true}};
    std::uintmax_t bytes = 0;
    std::size_t duplicates = 0, unknown = 0;
    std::set<std::wstring> filenames;
    for (const auto& capture : plan.captures) {
        const bool repeated = !filenames.insert(folded(capture.name)).second;
        const bool duplicate = capture.possible_duplicate || repeated;
        duplicates += duplicate;
        unknown += !capture.bytes.has_value();
        if (capture.bytes) bytes += *capture.bytes;
        auto size = capture.bytes ? std::to_wstring(*capture.bytes) + L" bytes" : L"size unavailable";
        dialog.selected.push_back(static_cast<int>(dialog.choices.size()));
        const auto game = capture.suggested_game.empty() ? capture.game : capture.suggested_game + L" (suggested)";
        dialog.choices.push_back(game + L" / " + capture.name + L"  ·  " + size +
            (duplicate ? L"  ·  Possible duplicate" : L""));
    }
    dialog.text = std::to_wstring(plan.captures.size()) + L" captures, " +
        std::to_wstring(bytes) + L" known bytes, " + std::to_wstring(duplicates) + L" possible duplicates.";
    if (unknown) dialog.text += L" Size unavailable for " + std::to_wstring(unknown) + L" captures.";
    if (plan.scan_errors) dialog.text += L" Some folders or files could not be read.";
    dialog.text += L" Click a row to toggle it. Only highlighted captures will be copied.";
    if (theme::run_dialog(owner, dialog) != IDOK || dialog.selected.empty()) return {};
    std::vector<ImportCandidate> selected;
    selected.reserve(dialog.selected.size());
    for (int index : dialog.selected) selected.push_back(std::move(plan.captures.at(index)));
    plan.captures = std::move(selected);
    std::map<std::wstring, int> counts;
    for (const auto& capture : plan.captures) {
        if (!capture.suggested_game.empty()) ++counts[capture.suggested_game];
    }
    if (!counts.empty()) {
        theme::Dialog review;
        review.title = L"Review game suggestions";
        review.heading = L"Sort captures by game";
        review.text = L"These game folders match filenames in your library or the import. "
                      L"Clear any suggestion you want to leave in Imported captures.";
        review.buttons = {{IDCANCEL, L"Cancel import"}, {IDOK, L"Import captures", true}};
        std::vector<std::wstring> games;
        for (const auto& [game, count] : counts) {
            review.selected.push_back(static_cast<int>(games.size()));
            games.push_back(game);
            review.choices.push_back(game + L"  ·  " + std::to_wstring(count) + L" captures");
        }
        if (theme::run_dialog(owner, review) != IDOK) return {};
        std::set<std::wstring> confirmed;
        for (const int index : review.selected) confirmed.insert(games.at(index));
        for (auto& capture : plan.captures) {
            if (confirmed.contains(capture.suggested_game)) capture.game = capture.suggested_game;
        }
    }
    return std::make_shared<const ImportPlan>(std::move(plan));
}

bool valid_watch_folder(const Path& source, const Path& album) {
    std::error_code error;
    if (source.empty() || !plain_directory(source)) return false;
    const auto from = std::filesystem::canonical(source, error).make_preferred();
    if (error || from == from.root_path()) return false;
    const auto to = std::filesystem::weakly_canonical(album, error).make_preferred();
    if (error || to.empty()) return false;
    const auto contains = [](const Path& parent, const Path& child) {
        auto part = child.begin();
        for (const auto& component : parent) {
            if (part == child.end() || folded(component.wstring()) != folded(part->wstring())) return false;
            ++part;
        }
        return true;
    };
    return !contains(from, to) && !contains(to, from);
}

ImportPlan WatchedFolder::poll(const Path& source, const Path& target, const Path& receipts,
                              const std::function<bool()>& cancelled) {
    ImportPlan plan;
    plan.album = std::filesystem::absolute(target).lexically_normal().make_preferred();
    plan.watch_source = source;
    if (source != source_ || plan.album != album_ || receipts != receipts_) {
        source_ = source;
        album_ = plan.album;
        receipts_ = receipts;
        observed_.clear();
        completed_.clear();
        const auto saved = read_local_data(receipts_);
        const auto* files = saved.find("files");
        if (saved.string("source") == path_to_utf8(source_) &&
            saved.string("album") == path_to_utf8(album_) && files && files->is_object()) {
            for (const auto& [file, stamp] : files->as_object()) {
                if (stamp.is_string()) completed_.emplace(path_from_utf8(file), stamp.as_string());
            }
        }
    }
    if (!valid_watch_folder(source_, album_)) {
        observed_.clear();
        return plan;
    }
    std::map<Path, std::string> current;
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(source_, options, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (cancelled()) return {};
        const auto& path = it->path();
        if (it->is_directory(error)) {
            const auto name = folded(path.filename().wstring());
            if (it.depth() >= 32 || !plain_directory(path) || name.starts_with(L".switch-album-sync-")) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const auto name = path.filename().wstring();
        if (!safe_name(name) || media_type(path).empty()) continue;
        std::uintmax_t size = 0;
        const auto stamp = watch_stamp(path, size);
        if (stamp.empty()) continue;
        current.emplace(path, stamp);
        const auto previous = observed_.find(path);
        const auto done = completed_.find(path);
        if (plan.captures.size() >= 128 || previous == observed_.end() || previous->second != stamp ||
            (done != completed_.end() && done->second == stamp)) continue;
        const auto relative = path.lexically_relative(source_);
        auto game = relative.has_parent_path() ? relative.begin()->wstring() : L"Imported captures";
        const bool year = game.size() == 4 && game.find_first_not_of(L"0123456789") == std::wstring::npos;
        if (year || folded(game) == L"album" || folded(game) == L"nintendo") game = L"Imported captures";
        plan.captures.push_back({path, {}, name, std::move(game), size, false, {}, stamp});
    }
    if (error) {
        observed_.clear();
        plan.captures.clear();
        return plan;
    }
    observed_.swap(current);
    if (std::erase_if(completed_, [this](const auto& entry) { return !observed_.contains(entry.first); })) {
        complete({});
    }
    return plan;
}

void WatchedFolder::complete(const std::vector<Path>& sources) {
    for (const auto& source : sources) {
        if (const auto found = observed_.find(source); found != observed_.end()) {
            completed_.insert_or_assign(source, found->second);
        }
    }
    Json::object files;
    for (const auto& [file, stamp] : completed_) files[path_to_utf8(file)] = stamp;
    write_local_data(receipts_, Json::object{
        {"source", path_to_utf8(source_)}, {"album", path_to_utf8(album_)}, {"files", std::move(files)}});
}

ImportResult import_windows_captures(const ImportPlan& plan, const Path& target,
                                     const std::function<bool()>& cancelled) {
    const auto album = std::filesystem::absolute(target).lexically_normal().make_preferred();
    if (album != plan.album) throw std::runtime_error("Destination changed after the import preview");
    if (!plan.watch_source.empty() && !valid_watch_folder(plan.watch_source, album)) {
        throw std::runtime_error("The watched folder overlaps the destination album");
    }
    const ComApartment com;
    if (FAILED(com.status)) throw std::runtime_error("Could not open the import source");
    GUID id{};
    wchar_t unique[48]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, static_cast<int>(std::size(unique)))) {
        throw std::runtime_error("Could not create an import workspace");
    }
    const auto staging = album / (std::wstring(L".switch-album-sync-import-") + unique);
    std::filesystem::create_directories(staging);
    struct Cleanup {
        const Path& path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{staging};
    Importer importer(album, staging, cancelled, scan_capture_library(album, cancelled));
    for (const auto& capture : plan.captures) {
        if (cancelled()) break;
        ComPtr<IShellItem> item;
        if (capture.local.empty()) {
            if (FAILED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(capture.shell_id.data()),
                                            IID_PPV_ARGS(&item)))) {
                ++importer.result.failed;
                continue;
            }
        } else if (!plain_file(capture.local)) {
            ++importer.result.failed;
            continue;
        }
        std::unique_ptr<void, decltype(&CloseHandle)> locked(nullptr, CloseHandle);
        if (!capture.watch_stamp.empty()) {
            const auto file = CreateFileW(capture.local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) continue;
            locked.reset(file);
            std::uintmax_t size = 0;
            if (watch_stamp(capture.local, size) != capture.watch_stamp) continue;
        }
        const auto failures = importer.result.failed;
        importer.copy(item.Get(), capture.local, capture.name, capture.game);
        if (locked && !cancelled() && importer.result.failed == failures) {
            importer.result.completed_sources.push_back(capture.local);
        }
    }
    return std::move(importer.result);
}
}
