#include "import_windows.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/sync.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace nso {
namespace {
using Microsoft::WRL::ComPtr;
using Path = std::filesystem::path;
constexpr wchar_t kReviewDirectory[] = L".capturedock-import-review";
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

    Importer(const Path& target, const Path& temporary, const std::function<bool()>& stop)
        : album(target), staging(temporary), cancelled(stop), pending(pending_import_matches(album)) {
        for (const auto& capture : scan_capture_library(album, cancelled)) remember(capture.path);
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

    void visit(IShellItem* folder, const std::wstring& game, int depth = 0) {
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
                if (folded(name).starts_with(L".capturedock-import-")) continue;
                const bool dated = name.size() == 4 && std::all_of(name.begin(), name.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
                const bool album_wrapper = depth == 0 && folded(name) == L"album";
                const auto next_game = depth == 0 && !dated && !album_wrapper ? name : game;
                visit(child.Get(), next_game, album_wrapper ? depth : depth + 1);
            } else {
                copy(child.Get(), local, item_filename(child.Get(), local), game);
            }
        }
    }
};
}

std::vector<ImportMatch> pending_import_matches(const Path& album) {
    std::vector<ImportMatch> matches;
    const auto review = album / kReviewDirectory;
    if (!plain_directory(review)) return matches;
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
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.incoming < b.incoming; });
    return matches;
}

bool resolve_import_match(const Path& album, const ImportMatch& match, bool keep_both) {
    const auto slot = match.incoming.parent_path();
    const auto review = album / kReviewDirectory;
    if (slot.parent_path() != review || !plain_directory(review) || !plain_directory(slot) ||
        !plain_file(match.incoming) || !within_album(album, match.existing)) return false;
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
    const auto slot = online_slot(album, filename);
    return plain_directory(slot.parent_path()) && plain_directory(slot) && plain_file(slot / kMatchFile);
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

std::string pick_import_source(HWND owner) {
    const ComApartment com;
    ComPtr<IFileOpenDialog> picker;
    if (FAILED(com.status) || FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    picker->GetOptions(&options);
    picker->SetOptions((options & ~FOS_FORCEFILESYSTEM) | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Import from Nintendo Switch USB or microSD");
    picker->SetOkButtonLabel(L"Import captures");
    ComPtr<IShellItem> item;
    struct Pidl {
        PIDLIST_ABSOLUTE value = nullptr;
        ~Pidl() { CoTaskMemFree(value); }
    } id;
    if (FAILED(picker->Show(owner)) || FAILED(picker->GetResult(&item)) ||
        FAILED(SHGetIDListFromObject(item.Get(), &id.value)) || !id.value) return {};
    return {reinterpret_cast<const char*>(id.value), ILGetSize(id.value)};
}

ImportResult import_windows_captures(const std::string& source, const Path& target,
                                     const std::function<bool()>& cancelled) {
    auto album = std::filesystem::absolute(target).lexically_normal();
    album.make_preferred();
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
    GUID id{};
    wchar_t unique[48]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, static_cast<int>(std::size(unique)))) {
        throw std::runtime_error("Could not create an import workspace");
    }
    const auto staging = album / (std::wstring(L".capturedock-import-") + unique);
    std::filesystem::create_directories(staging);
    struct Cleanup {
        const Path& path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{staging};
    Importer importer(album, staging, cancelled);
    auto source_name = display_name(root.Get(), SIGDN_NORMALDISPLAY);
    const auto name = folded(source_name);
    if (!local.empty() || name == L"album" || name.starts_with(L"nintendo switch")) {
        source_name = L"Imported captures";
    }
    importer.visit(root.Get(), source_name);
    return importer.result;
}
}
