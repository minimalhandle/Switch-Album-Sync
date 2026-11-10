#include "library_windows.hpp"
#include "import_windows.hpp"
#include "theme_windows.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/util.hpp"

#include <commctrl.h>
#include <gdiplus.h>
#include <mfplay.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

namespace nso {
namespace {
constexpr wchar_t kClass[] = L"CaptureDockAlbumTools";
constexpr UINT kApplySelection = WM_APP + 72;
enum Control { Library = 3000, History, Storage, Duplicates, Title, Search, NewCaptures, Game, Date, Type, Favorites,
               Collections, CollectionName, SaveCollection, DeleteCollection, Runs, Rows, Preview,
               ExistingLabel, IncomingLabel, IncomingPreview, OpenExisting, OpenIncoming, KeepExisting,
               Tags, Save, Favorite, Play, Stop, Action, Refresh, CopyFiles, Status, VideoSurface, End };

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring text_of(HWND window) {
    const int size = GetWindowTextLengthW(window);
    std::wstring result(size + 1, L'\0');
    GetWindowTextW(window, result.data(), size + 1);
    result.resize(size);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                  result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring folded(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring date_text(std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
    wchar_t value[32]{};
    if (localtime_s(&local, &time) == 0) std::wcsftime(value, std::size(value), L"%Y-%m-%d", &local);
    return value;
}

std::wstring size_text(std::uintmax_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    constexpr const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    double amount = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (amount >= 1024 && unit + 1 < std::size(units)) {
        amount /= 1024;
        ++unit;
    }
    wchar_t value[48]{};
    swprintf_s(value, L"%.1f %ls", amount, units[unit]);
    return value;
}

struct Imaging {
    ULONG_PTR token = 0;
    Imaging() {
        Gdiplus::GdiplusStartupInput options;
        Gdiplus::GdiplusStartup(&token, &options, nullptr);
    }
    ~Imaging() { if (token) Gdiplus::GdiplusShutdown(token); }
};

bool high_contrast() {
    HIGHCONTRASTW info{sizeof(info)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(info), &info, 0) &&
        (info.dwFlags & HCF_HIGHCONTRASTON);
}

using CapturePaths = std::vector<std::filesystem::path>;

HGLOBAL file_drop_data(const CapturePaths& paths) {
    if (paths.empty()) return nullptr;
    std::vector<wchar_t> names;
    for (const auto& path : paths) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error || !std::filesystem::is_regular_file(absolute, error)) return nullptr;
        const auto& native = absolute.native();
        names.insert(names.end(), native.begin(), native.end());
        names.push_back(L'\0');
    }
    names.push_back(L'\0');
    const auto bytes = names.size() * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + bytes);
    if (!memory) return nullptr;
    auto* data = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        return nullptr;
    }
    data->pFiles = sizeof(DROPFILES);
    data->fWide = TRUE;
    std::memcpy(data + 1, names.data(), bytes);
    GlobalUnlock(memory);
    return memory;
}

bool drag_capture_files(HWND owner, const CapturePaths& paths) {
    const auto memory = file_drop_data(paths);
    if (!memory) return false;
    const auto initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        GlobalFree(memory);
        return false;
    }
    bool available = false;
    {
        Microsoft::WRL::ComPtr<IDataObject> data;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = memory;
        available = SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data))) &&
            SUCCEEDED(data->SetData(&format, &medium, TRUE));
        if (available) {
            DWORD effect = DROPEFFECT_NONE;
            available = SUCCEEDED(SHDoDragDrop(owner, data.Get(), nullptr, DROPEFFECT_COPY, &effect));
        } else GlobalFree(memory);
    }
    OleUninitialize();
    return available;
}
}

bool copy_capture_files(HWND owner, const CapturePaths& paths) {
    const auto memory = file_drop_data(paths);
    if (!memory) return false;
    bool copied = false;
    if (OpenClipboard(owner)) {
        copied = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
        CloseClipboard();
    }
    if (!copied) GlobalFree(memory);
    return copied;
}

bool copy_capture_image(HWND owner, const std::filesystem::path& path) {
    Imaging imaging;
    if (!imaging.token) return false;
    Gdiplus::Bitmap image(path.c_str());
    HBITMAP bitmap = nullptr;
    if (image.GetHBITMAP(Gdiplus::Color(255, 255, 255), &bitmap) != Gdiplus::Ok) return false;
    bool copied = false;
    if (OpenClipboard(owner)) {
        copied = EmptyClipboard() && SetClipboardData(CF_BITMAP, bitmap);
        CloseClipboard();
    }
    if (!copied) DeleteObject(bitmap);
    return copied;
}

struct WindowsLibrary::Impl {
    HWND window = nullptr;
    std::array<HWND, End - Library> controls{};
    LibraryPage page = LibraryPage::captures;
    const PlatformCallbacks* callbacks = nullptr;
    MenuState state;
    std::vector<std::filesystem::path> games;
    std::vector<std::filesystem::path> folders;
    std::vector<std::size_t> rows;
    Json::object labels;
    std::filesystem::path labels_path;
    std::filesystem::path preferences_path;
    Json::object preferences;
    Json::object collections;
    std::vector<std::string> collection_names;
    std::filesystem::path game_filter;
    std::filesystem::path visited_album;
    std::int64_t previous_visit = 0;
    std::set<std::filesystem::path> arrivals;
    std::vector<ImportMatch> matches;
    bool updating = false;
    bool selection_pending = false;
    UINT dpi = 96;
    HFONT body = nullptr;
    HFONT title = nullptr;
    HBRUSH paper = theme::brush(theme::kBackground);
    HBRUSH ink = theme::brush(theme::kPreview);
    HBRUSH red = theme::brush(theme::kRed);
    Imaging imaging;
    std::unique_ptr<Gdiplus::Image> image;
    std::unique_ptr<Gdiplus::Image> incoming_image;
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player;
    bool com = false;

    ~Impl() {
        stop();
        if (window) DestroyWindow(window);
        if (body) DeleteObject(body);
        if (title) DeleteObject(title);
        if (com) CoUninitialize();
    }
    HWND get(Control id) const { return controls[id - Library]; }
    int px(int units) const { return MulDiv(units, dpi, 96); }
    void set(Control id, const std::wstring& value) { SetWindowTextW(get(id), value.c_str()); }
    void show(Control id, bool visible) { ShowWindow(get(id), visible ? SW_SHOWNA : SW_HIDE); }
    void enable(Control id, bool enabled) { EnableWindow(get(id), enabled); }
    int choice(Control id) const { return static_cast<int>(SendMessageW(get(id), CB_GETCURSEL, 0, 0)); }
    void choice(Control id, int index) { SendMessageW(get(id), CB_SETCURSEL, index, 0); }
    void option(Control id, const std::wstring& label) {
        SendMessageW(get(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    bool library() const { return page == LibraryPage::captures; }
    bool history() const { return page == LibraryPage::history; }
    bool duplicates() const { return page == LibraryPage::duplicates; }
    bool checked(Control id) const { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void check(Control id, bool value) { SendMessageW(get(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
    std::wstring game_name(const std::filesystem::path& path) const {
        return path.lexically_normal() == state.destination.lexically_normal() ? L"Unsorted" : path.filename().wstring();
    }

    void make(Control id, const wchar_t* type, const wchar_t* label, DWORD style = 0, DWORD ex = 0) {
        controls[id - Library] = CreateWindowExW(ex, type, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }
    void button(Control id, const wchar_t* label, DWORD style = BS_PUSHBUTTON) {
        make(id, L"BUTTON", label, WS_TABSTOP | style);
    }
    void create_controls() {
        button(Library, L"Capture library");
        button(History, L"Sync history");
        button(Storage, L"Storage");
        button(Duplicates, L"Duplicate review");
        make(Title, L"STATIC", L"", SS_NOPREFIX);
        for (const auto id : {Search, Tags}) {
            make(id, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        }
        SendMessageW(get(Search), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search captures, games, or tags"));
        SendMessageW(get(Tags), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Tags, separated by commas"));
        SendMessageW(get(Tags), EM_LIMITTEXT, 512, 0);
        button(NewCaptures, L"New since last visit", BS_AUTOCHECKBOX);
        make(CollectionName, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        SendMessageW(get(CollectionName), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Collection name"));
        SendMessageW(get(CollectionName), EM_LIMITTEXT, 80, 0);
        button(SaveCollection, L"Save filters");
        button(DeleteCollection, L"Delete");
        for (const auto id : {Game, Date, Type, Runs, Collections}) {
            make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL);
        }
        for (auto label : {L"Any date", L"Today", L"Past 7 days", L"Past 30 days"}) option(Date, label);
        for (auto label : {L"All media", L"Screenshots", L"Videos"}) option(Type, label);
        choice(Date, 0);
        choice(Type, 0);
        button(Favorites, L"Favorites", BS_AUTOCHECKBOX);
        make(Rows, WC_LISTVIEWW, L"Results", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(get(Rows), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        make(Preview, L"STATIC", L"", SS_OWNERDRAW);
        make(ExistingLabel, L"STATIC", L"Album copy", SS_NOPREFIX);
        make(IncomingLabel, L"STATIC", L"Incoming copy", SS_NOPREFIX);
        make(IncomingPreview, L"STATIC", L"", SS_OWNERDRAW);
        button(OpenExisting, L"Open album copy");
        button(OpenIncoming, L"Open incoming copy");
        button(KeepExisting, L"Keep album copy");
        make(VideoSurface, L"STATIC", L"", SS_BLACKRECT);
        SetWindowSubclass(get(VideoSurface), video_paint, 1, reinterpret_cast<DWORD_PTR>(this));
        show(VideoSurface, false);
        button(Save, L"Save tags");
        button(Favorite, L"Add favorite");
        button(Play, L"Play");
        button(Stop, L"Stop");
        button(Action, L"Open file");
        button(Refresh, L"Refresh");
        button(CopyFiles, L"Copy selected files");
        make(Status, L"STATIC", L"", SS_NOPREFIX);
        fonts();
        theme::apply(window, theme::Caption::red);
        theme::set_primary(get(Action));
    }
    void fonts() {
        const auto old_body = body;
        const auto old_title = title;
        body = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        title = CreateFontW(-px(29), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (auto control : controls) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
        SendMessageW(get(Title), WM_SETFONT, reinterpret_cast<WPARAM>(title), TRUE);
        if (old_body) DeleteObject(old_body);
        if (old_title) DeleteObject(old_title);
    }
    void layout() {
        RECT area{};
        GetClientRect(window, &area);
        const int width = MulDiv(area.right, 96, dpi);
        const int height = MulDiv(area.bottom, 96, dpi);
        const int panel = std::max(205, width / 3);
        const int panel_x = width - panel - 20;
        auto place = [&](Control id, int x, int y, int w, int h) {
            MoveWindow(get(id), px(x), px(y), px(std::max(1, w)), px(std::max(1, h)), TRUE);
        };
        place(Title, 20, 13, width - 40, 42);
        place(Library, 20, 67, 126, 30);
        place(History, 156, 67, 108, 30);
        place(Storage, 274, 67, 96, 30);
        place(Duplicates, 380, 67, 150, 30);
        place(Search, 20, 109, width - 241, 26);
        place(NewCaptures, width - 211, 109, 196, 26);
        place(Game, 20, 145, width - 404, 230);
        place(Date, width - 374, 145, 116, 175);
        place(Type, width - 248, 145, 105, 170);
        place(Favorites, width - 133, 143, 118, 27);
        place(Collections, 20, 181, width - 379, 220);
        place(CollectionName, width - 349, 181, 150, 26);
        place(SaveCollection, width - 189, 180, 96, 27);
        place(DeleteCollection, width - 83, 180, 63, 27);
        place(Runs, 20, 110, width - 40, 235);
        const int top = library() ? 219 : duplicates() ? 109 : 148;
        const int list_height = duplicates() ? std::max(86, height / 4) : height - top - 78;
        place(Rows, 20, top, library() ? panel_x - 34 : width - 40, list_height);
        const int preview_height = std::max(50, height - 356);
        const int review_top = top + list_height + 10;
        const int half = (width - 52) / 2;
        const int review_height = height - review_top - 131;
        place(ExistingLabel, 20, review_top, half, 22);
        place(IncomingLabel, 32 + half, review_top, half, 22);
        if (duplicates()) place(Preview, 20, review_top + 24, half, review_height);
        else place(Preview, panel_x, 219, panel, preview_height);
        place(IncomingPreview, 32 + half, review_top + 24, half, review_height);
        place(OpenExisting, 20, height - 98, 151, 26);
        place(OpenIncoming, 32 + half, height - 98, 151, 26);
        place(VideoSurface, panel_x, 219, panel, preview_height);
        place(Play, panel_x, 226 + preview_height, 67, 27);
        place(Stop, panel_x + 76, 226 + preview_height, 67, 27);
        place(Tags, panel_x, 256 + preview_height, panel, 27);
        place(Save, panel_x, 286 + preview_height, 96, 27);
        place(Favorite, panel_x + 104, 286 + preview_height, panel - 104, 27);
        place(Status, 20, height - 69, library() ? panel_x - 34 : width - 40, 21);
        place(Action, 20, height - 39, 177, 29);
        place(KeepExisting, 207, height - 39, 151, 29);
        place(Refresh, duplicates() ? 368 : 207, height - 39, 86, 29);
        place(CopyFiles, 303, height - 39, 164, 29);
        if (player) player->UpdateVideo();
    }
    void stop() {
        if (player) {
            auto previous = std::move(player);
            previous->Shutdown();
        }
        show(VideoSurface, false);
        show(Preview, library() || duplicates());
    }
    static LRESULT CALLBACK video_paint(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR data) {
        const auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_PAINT && self->player) {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            self->player->UpdateVideo();
            EndPaint(hwnd, &paint);
            return 0;
        }
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    void choices() {
        SendMessageW(get(Game), CB_RESETCONTENT, 0, 0);
        option(Game, L"All games");
        std::map<std::filesystem::path, bool> distinct;
        if (state.capture_library) for (const auto& item : *state.capture_library) distinct[item.game_folder] = true;
        games.clear();
        int index = 0;
        for (const auto& [folder, unused] : distinct) {
            games.push_back(folder);
            option(Game, game_name(folder));
            if (folder == game_filter) index = static_cast<int>(games.size());
        }
        if (!index && !game_filter.empty()) {
            games.push_back(game_filter);
            option(Game, game_name(game_filter));
            index = static_cast<int>(games.size());
        }
        choice(Game, index);
        SendMessageW(get(Runs), CB_RESETCONTENT, 0, 0);
        if (state.sync_history) for (const auto& run : *state.sync_history) {
            option(Runs, wide(run.string("time")) + L"  ·  " + std::to_wstring(run.integer("downloaded")) +
                L" downloaded  ·  " + std::to_wstring(run.integer("skipped")) + L" skipped  ·  " +
                std::to_wstring(run.integer("failed")) + L" failed");
        }
        choice(Runs, 0);
    }

    void collection_choices(const std::string& selected = {}) {
        SendMessageW(get(Collections), CB_RESETCONTENT, 0, 0);
        option(Collections, L"Smart collections");
        collection_names.clear();
        int index = 0;
        for (const auto& [name, filters] : collections) {
            if (!filters.is_object()) continue;
            collection_names.push_back(name);
            option(Collections, wide(name));
            if (name == selected) index = static_cast<int>(collection_names.size());
        }
        choice(Collections, index);
        enable(DeleteCollection, index > 0);
    }

    std::string selected_collection() const {
        const auto index = choice(Collections);
        return index > 0 && static_cast<std::size_t>(index) <= collection_names.size()
            ? collection_names[index - 1] : std::string{};
    }

    void load_collection() {
        const auto name = selected_collection();
        enable(DeleteCollection, !name.empty());
        if (name.empty()) return;
        const auto& filters = collections.at(name);
        updating = true;
        set(Search, wide(filters.string("query")));
        set(CollectionName, wide(name));
        game_filter = path_from_utf8(filters.string("game"));
        choices();
        choice(Date, static_cast<int>(std::clamp<std::int64_t>(filters.integer("date"), 0, 3)));
        choice(Type, static_cast<int>(std::clamp<std::int64_t>(filters.integer("type"), 0, 2)));
        check(Favorites, filters.boolean("favorites"));
        clear_arrivals();
        updating = false;
        populate();
    }

    void edit_collection(bool remove) {
        const auto name = remove ? selected_collection() : trim(utf8(text_of(get(CollectionName))));
        if (name.empty()) {
            if (!remove) {
                set(Status, L"Enter a name for this smart collection.");
                SetFocus(get(CollectionName));
            }
            return;
        }
        auto next = collections;
        if (remove) next.erase(name);
        else next[name] = Json::object{
            {"query", utf8(text_of(get(Search)))},
            {"game", path_to_utf8(game_filter)},
            {"date", choice(Date)}, {"type", choice(Type)}, {"favorites", checked(Favorites)}};
        auto data = preferences;
        data["collections"] = next;
        if (preferences_path.empty() || !write_local_data(preferences_path, data)) {
            set(Status, L"Could not save smart collections.");
            return;
        }
        preferences.swap(data);
        collections.swap(next);
        collection_choices(remove ? "" : name);
        if (remove) set(CollectionName, L"");
        set(Status, remove ? L"Smart collection deleted." : L"Smart collection saved.");
    }

    void begin_visit() {
        if (state.destination.empty() || visited_album == state.destination || preferences_path.empty()) return;
        visited_album = state.destination;
        const auto key = path_to_utf8(visited_album);
        const Json prefs(preferences);
        const auto* stored = prefs.find("visits");
        auto visits = stored && stored->is_object() ? stored->as_object() : Json::object{};
        const auto now = std::chrono::floor<std::chrono::milliseconds>(
            std::chrono::system_clock::now()).time_since_epoch().count();
        const auto old = visits.find(key);
        previous_visit = old == visits.end() ? now : old->second.as_i64(now);
        visits[key] = now;
        auto data = preferences;
        data["visits"] = std::move(visits);
        if (write_local_data(preferences_path, data)) preferences.swap(data);
    }

    void clear_arrivals() {
        arrivals.clear();
        set(NewCaptures, L"New since last visit");
        check(NewCaptures, false);
    }
    void columns(std::initializer_list<std::pair<const wchar_t*, int>> headers) {
        while (ListView_DeleteColumn(get(Rows), 0)) {}
        int index = 0;
        for (const auto& [text, width] : headers) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(text);
            column.cx = px(width);
            ListView_InsertColumn(get(Rows), index++, &column);
        }
    }
    void add_row(std::initializer_list<std::wstring> fields) {
        const int index = ListView_GetItemCount(get(Rows));
        int column = 0;
        for (const auto& field : fields) {
            auto* value = const_cast<wchar_t*>(field.c_str());
            if (!column) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = index;
                item.pszText = value;
                ListView_InsertItem(get(Rows), &item);
            } else ListView_SetItemText(get(Rows), index, column, value);
            ++column;
        }
    }
    const LocalCapture* selected_capture() const {
        const int index = ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED);
        if (!library() || !state.capture_library || index < 0 || static_cast<std::size_t>(index) >= rows.size()) return nullptr;
        const auto row = rows[index];
        return row < state.capture_library->size() ? &(*state.capture_library)[row] : nullptr;
    }
    CapturePaths selected_paths() const {
        CapturePaths paths;
        if (!library() || !state.capture_library) return paths;
        for (int index = ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED); index >= 0;
             index = ListView_GetNextItem(get(Rows), index, LVNI_SELECTED)) {
            if (static_cast<std::size_t>(index) < rows.size()) paths.push_back((*state.capture_library)[rows[index]].path);
        }
        return paths;
    }

    void copy_selected() {
        const auto paths = selected_paths();
        if (paths.empty()) return;
        set(Status, copy_capture_files(window, paths)
            ? std::to_wstring(paths.size()) + L" files copied. Paste them into a chat or folder."
            : L"Could not copy the selected files. Refresh if a file has moved.");
    }

    const ImportMatch* selected_match() const {
        const int index = ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED);
        return index >= 0 && static_cast<std::size_t>(index) < matches.size() ? &matches[index] : nullptr;
    }
    const Json* selected_run() const {
        const int index = choice(Runs);
        if (!state.sync_history || index < 0 || static_cast<std::size_t>(index) >= state.sync_history->size()) return nullptr;
        return &(*state.sync_history)[index];
    }
    const Json* selected_result() const {
        const auto* run = selected_run();
        const auto* items = run ? run->find("items") : nullptr;
        const int index = ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED);
        return items && items->is_array() && index >= 0 && static_cast<std::size_t>(index) < items->as_array().size()
            ? &items->as_array()[index] : nullptr;
    }
    const Json& annotation(const LocalCapture& capture) const {
        static const Json empty;
        const auto found = labels.find(path_to_utf8(capture.path));
        return found == labels.end() ? empty : found->second;
    }
    void library_rows() {
        auto query = folded(text_of(get(Search)));
        const bool tag_only = query.starts_with(L"tag:");
        if (tag_only) query.erase(0, query.find_first_not_of(L" \t", 4));
        const int date = choice(Date), type = choice(Type);
        const bool favorites = checked(Favorites);
        const bool new_only = checked(NewCaptures);
        const auto now = unix_now();
        if (state.capture_library) for (std::size_t i = 0; i < state.capture_library->size(); ++i) {
            const auto& capture = (*state.capture_library)[i];
            if (!arrivals.empty()) {
                if (!arrivals.contains(capture.path.lexically_normal())) continue;
            } else if (new_only && capture.added_ms <= previous_visit) continue;
            const auto& label = annotation(capture);
            const bool favorite = label.boolean("favorite");
            if (favorites && !favorite) continue;
            if (!game_filter.empty() && capture.game_folder != game_filter) continue;
            if ((type == 1 && capture.video) || (type == 2 && !capture.video)) continue;
            if (date == 1 && date_text(now) != date_text(capture.modified)) continue;
            if (date >= 2 && capture.modified < now - (date == 2 ? 7 : 30) * 86400LL) continue;
            const auto name = capture.path.filename().wstring();
            const auto game_label = game_name(capture.game_folder);
            const auto tags = wide(label.string("tags"));
            const auto searchable = tag_only ? tags : name + L" " + game_label + L" " + tags;
            if (!query.empty() && folded(searchable).find(query) == std::wstring::npos) continue;
            add_row({favorite ? L"★ " + name : name, game_label, date_text(capture.modified), capture.video ? L"Video" : L"Screenshot"});
            rows.push_back(i);
        }
        set(Status, state.recent_loading ? L"Scanning the album…" : rows.empty() ?
            (new_only ? L"No new captures match this view." : L"No matching captures. Clear filters or refresh the album.") :
            std::to_wstring(rows.size()) + L" captures · Ctrl/Shift to select several, then drag or copy.");
    }

    void duplicate_rows() {
        matches = pending_import_matches(state.destination);
        for (const auto& match : matches) {
            add_row({match.existing.filename().wstring(), match.incoming.filename().wstring()});
        }
        set(Status, matches.empty() ? L"No matches to review. Overlapping Nintendo sync, USB and microSD captures appear here." :
            std::to_wstring(matches.size()) + L" possible matches. Review both copies before choosing.");
    }
    void history_rows() {
        const auto* run = selected_run();
        const auto* items = run ? run->find("items") : nullptr;
        if (items && items->is_array()) for (const auto& item : items->as_array()) {
            add_row({wide(item.string("name")), wide(item.string("outcome"))});
        }
        if (!run) set(Status, L"No sync runs yet. Start a sync from the dashboard.");
        else if (run->integer("unavailable") > 0) set(Status, std::to_wstring(run->integer("unavailable")) + L" captures no longer available from Nintendo.");
        else if (!run->string("error").empty()) set(Status, wide(run->string("error")));
        else set(Status, items && items->is_array() && !items->as_array().empty() ?
            L"Downloaded and skipped captures are shown below." : L"Nintendo returned no captures for this run.");
    }
    void storage_rows() {
        struct Usage { std::uintmax_t images = 0, videos = 0; };
        std::map<std::filesystem::path, Usage> games_usage;
        Usage total;
        if (state.capture_library) for (const auto& capture : *state.capture_library) {
            auto& usage = games_usage[capture.game_folder];
            if (capture.video) {
                usage.videos += capture.bytes;
                total.videos += capture.bytes;
            } else {
                usage.images += capture.bytes;
                total.images += capture.bytes;
            }
        }
        for (const auto& [folder, usage] : games_usage) {
            add_row({game_name(folder), size_text(usage.images), size_text(usage.videos), size_text(usage.images + usage.videos)});
            folders.push_back(folder);
        }
        set(Status, folders.empty() ? L"No local captures to measure." :
            size_text(total.images + total.videos) + L" total  ·  " + size_text(total.images) + L" screenshots  ·  " +
            size_text(total.videos) + L" videos");
    }
    void populate(const CapturePaths& preserve = {}) {
        updating = true;
        SendMessageW(get(Rows), WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(get(Rows));
        rows.clear();
        folders.clear();
        if (library()) library_rows();
        else if (history()) history_rows();
        else if (duplicates()) duplicate_rows();
        else storage_rows();
        int focus = -1;
        if (!preserve.empty() && state.capture_library) {
            const std::set<std::filesystem::path> selected(preserve.begin(), preserve.end());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (selected.contains((*state.capture_library)[rows[i]].path)) {
                    ListView_SetItemState(get(Rows), static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
                    if (focus == -1) focus = static_cast<int>(i);
                }
            }
        }
        if (ListView_GetItemCount(get(Rows))) {
            if (focus == -1) focus = 0;
            ListView_SetItemState(get(Rows), focus, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(get(Rows), focus, FALSE);
        }
        SendMessageW(get(Rows), WM_SETREDRAW, TRUE, 0);
        InvalidateRect(get(Rows), nullptr, TRUE);
        updating = false;
        selection();
    }
    void selection() {
        selection_pending = false;
        if (history()) {
            const auto* run = selected_run();
            const auto* item = selected_result();
            enable(Action, !state.sync_busy && !state.retry_scope.empty() && run && item &&
                run->string("scope") == state.retry_scope && !item->string("id").empty());
            return;
        }
        if (duplicates()) {
            const auto* match = selected_match();
            image = match ? comparison_image(match->existing) : nullptr;
            incoming_image = match ? comparison_image(match->incoming) : nullptr;
            std::error_code error;
            const bool existing = match && std::filesystem::is_regular_file(match->existing, error);
            enable(OpenExisting, existing);
            enable(OpenIncoming, match != nullptr);
            enable(Action, match && !state.sync_busy);
            enable(KeepExisting, existing && !state.sync_busy);
            set(Action, existing ? L"Keep both copies" : L"Keep incoming copy");
            InvalidateRect(get(Preview), nullptr, TRUE);
            InvalidateRect(get(IncomingPreview), nullptr, TRUE);
            return;
        }
        if (!library()) {
            enable(Action, ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED) >= 0);
            return;
        }
        stop();
        image.reset();
        const auto* capture = selected_capture();
        const auto selected = ListView_GetSelectedCount(get(Rows));
        enable(CopyFiles, selected > 0);
        if (selected > 1) capture = nullptr;
        for (const auto id : {Action, Tags, Save, Favorite}) enable(id, capture != nullptr);
        enable(Play, capture && capture->video);
        enable(Stop, false);
        set(Tags, capture ? wide(annotation(*capture).string("tags")) : L"");
        set(Favorite, capture && annotation(*capture).boolean("favorite") ? L"Remove favorite" : L"Add favorite");
        if (capture && !capture->video && imaging.token) {
            image = std::make_unique<Gdiplus::Image>(capture->path.c_str());
            if (image->GetLastStatus() != Gdiplus::Ok || !image->GetWidth() || !image->GetHeight()) image.reset();
        }
        RedrawWindow(get(Preview), nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    void label_capture(bool toggle) {
        if (ListView_GetSelectedCount(get(Rows)) != 1) return;
        const auto* capture = selected_capture();
        if (!capture || labels_path.empty()) return;
        const auto key = path_to_utf8(capture->path);
        const auto& previous = annotation(*capture);
        const bool favorite = previous.boolean("favorite") != toggle;
        const auto tags = toggle ? previous.string("tags") : utf8(text_of(get(Tags)));
        auto next = labels;
        if (favorite || !tags.empty()) next[key] = Json::object{{"favorite", favorite}, {"tags", tags}};
        else next.erase(key);
        if (!write_local_data(labels_path, Json(next))) {
            set(Status, L"Could not save labels. Check access to CaptureDock settings.");
            return;
        }
        labels.swap(next);
        populate({capture->path});
        set(Status, toggle ? L"Favorites updated." : L"Tags saved.");
    }
    void play() {
        if (selection_pending) selection();
        if (ListView_GetSelectedCount(get(Rows)) != 1) return;
        const auto* capture = selected_capture();
        if (!capture || !capture->video) return;
        const auto path = capture->path;
        stop();
        if (!com) com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        show(VideoSurface, true);
        show(Preview, false);
        if (FAILED(MFPCreateMediaPlayer(path.c_str(), TRUE, 0, nullptr, get(VideoSurface), &player))) {
            stop();
            set(Status, L"Preview unavailable. Open the video in your default player.");
            return;
        }
        enable(Stop, true);
    }
    std::unique_ptr<Gdiplus::Image> comparison_image(const std::filesystem::path& path) {
        if (!imaging.token) return {};
        if (folded(path.extension().wstring()) != L".mp4") {
            auto still = std::make_unique<Gdiplus::Image>(path.c_str());
            if (still->GetLastStatus() == Gdiplus::Ok && still->GetWidth() && still->GetHeight()) return still;
            return {};
        }
        if (!com) com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
        HBITMAP bitmap = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->GetImage({480, 270}, SIIGBF_THUMBNAILONLY, &bitmap))) return {};
        std::unique_ptr<Gdiplus::Image> still;
        {
            std::unique_ptr<Gdiplus::Bitmap> original(Gdiplus::Bitmap::FromHBITMAP(bitmap, nullptr));
            if (original && original->GetLastStatus() == Gdiplus::Ok) {
                still.reset(static_cast<Gdiplus::Image&>(*original).Clone());
            }
        }
        DeleteObject(bitmap);
        if (still && still->GetLastStatus() == Gdiplus::Ok && still->GetWidth() && still->GetHeight()) return still;
        return {};
    }

    void resolve_match(bool keep_both) {
        const auto* match = selected_match();
        if (!match || state.sync_busy || (!keep_both && !IsWindowEnabled(get(KeepExisting)))) return;
        const auto selected = *match;
        const auto album = state.destination;
        if (!keep_both && MessageBoxW(window, L"Discard CaptureDock’s staged incoming copy and keep the album copy?",
            L"Duplicate review", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        image.reset();
        incoming_image.reset();
        if (album != state.destination || state.sync_busy || !resolve_import_match(album, selected, keep_both)) {
            selection();
            set(Status, L"Could not resolve this match. Check that both files are closed and the album is writable.");
            return;
        }
        populate();
        set(Status, keep_both ? L"Incoming copy saved in the album." : L"Album copy kept.");
        if (keep_both && callbacks->refresh_captures) callbacks->refresh_captures();
    }

    void draw_preview(const DRAWITEMSTRUCT& draw) {
        RECT bounds = draw.rcItem;
        FillRect(draw.hDC, &bounds, ink);
        const auto& still = draw.CtlID == IncomingPreview ? incoming_image : image;
        if (!still) {
            const auto* capture = selected_capture();
            SetBkMode(draw.hDC, TRANSPARENT);
            SetTextColor(draw.hDC, theme::kMuted);
            const auto* caption = duplicates() ? (selected_match() ? L"Open file to view" : L"Select a match") :
                ListView_GetSelectedCount(get(Rows)) > 1 ? L"Multiple captures selected" :
                !capture ? L"Select a capture" : capture->video ? L"Press Play to preview" : L"Preview unavailable";
            DrawTextW(draw.hDC, caption,
                -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return;
        }
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        const double scale = std::min(static_cast<double>(width) / still->GetWidth(),
                                      static_cast<double>(height) / still->GetHeight());
        const int w = static_cast<int>(still->GetWidth() * scale), h = static_cast<int>(still->GetHeight() * scale);
        Gdiplus::Graphics graphics(draw.hDC);
        graphics.DrawImage(still.get(), bounds.left + (width - w) / 2, bounds.top + (height - h) / 2, w, h);
    }
    void switch_page(LibraryPage next) {
        stop();
        image.reset();
        incoming_image.reset();
        page = next;
        if (library()) begin_visit();
        const auto row_style = GetWindowLongPtrW(get(Rows), GWL_STYLE);
        SetWindowLongPtrW(get(Rows), GWL_STYLE, library() ? row_style & ~LVS_SINGLESEL : row_style | LVS_SINGLESEL);
        for (auto id : {Search, NewCaptures, Game, Date, Type, Favorites, Collections, CollectionName,
                       SaveCollection, DeleteCollection, Tags, Save, Favorite, Play, Stop, CopyFiles}) show(id, library());
        for (auto id : {ExistingLabel, IncomingLabel, IncomingPreview, OpenExisting, OpenIncoming, KeepExisting}) show(id, duplicates());
        show(Preview, library() || duplicates());
        show(Runs, history());
        show(Refresh, !history());
        set(Title, library() ? L"Capture library" : history() ? L"Sync history" :
            duplicates() ? L"Duplicate review" : L"Album storage");
        set(Action, library() ? L"Open file" : history() ? L"Retry selected capture" : L"Open game folder");
        // The current page shows as the red tab rather than a disabled button.
        theme::set_primary(get(Library), library());
        theme::set_primary(get(History), history());
        theme::set_primary(get(Storage), page == LibraryPage::storage);
        theme::set_primary(get(Duplicates), duplicates());
        if (library()) columns({{L"Capture", 213}, {L"Game", 125}, {L"Date", 95}, {L"Type", 94}});
        else if (history()) columns({{L"Capture", 485}, {L"Result", 150}});
        else if (duplicates()) columns({{L"Album copy", 318}, {L"Incoming copy", 318}});
        else columns({{L"Game", 245}, {L"Screenshots", 130}, {L"Videos", 130}, {L"Total", 130}});
        layout();
        populate();
        SetFocus(get(library() ? Search : history() ? Runs : Rows));
    }
    void action() {
        if (history()) {
            const auto* run = selected_run();
            const auto* item = selected_result();
            if (run && item && IsWindowEnabled(get(Action)) && callbacks->retry_capture) {
                callbacks->retry_capture(item->string("id"), run->string("scope"));
            }
        } else if (duplicates()) {
            resolve_match(true);
        } else if (library() && ListView_GetSelectedCount(get(Rows)) == 1) {
            if (const auto* capture = selected_capture()) open_path(capture->path);
        } else if (!library()) {
            const int index = ListView_GetNextItem(get(Rows), -1, LVNI_SELECTED);
            if (index >= 0 && static_cast<std::size_t>(index) < folders.size()) open_path(folders[index]);
        }
    }
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                dpi = GetDpiForWindow(window);
                updating = true;
                create_controls();
                choices();
                collection_choices();
                updating = false;
                return 0;
            case WM_SIZE: layout(); return 0;
            case WM_DPICHANGED: {
                dpi = HIWORD(wp);
                const auto& bounds = *reinterpret_cast<const RECT*>(lp);
                SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                    bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
                fonts();
                switch_page(page);
                return 0;
            }
            case WM_GETMINMAXINFO:
                reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(700), px(530)};
                return 0;
            case WM_ERASEBKGND: {
                RECT bounds{};
                GetClientRect(window, &bounds);
                const auto dc = reinterpret_cast<HDC>(wp);
                FillRect(dc, &bounds, high_contrast() ? GetSysColorBrush(COLOR_WINDOW) : paper);
                bounds.bottom = px(56);
                if (!high_contrast()) FillRect(dc, &bounds, red);
                return TRUE;
            }
            case WM_CTLCOLORSTATIC: {
                const auto dc = reinterpret_cast<HDC>(wp);
                const auto child = reinterpret_cast<HWND>(lp);
                if (high_contrast()) {
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                }
                if (child == get(Title)) {
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, RGB(255, 255, 255));
                    return reinterpret_cast<LRESULT>(red);
                }
                const auto result = theme::control_color(msg, wp, lp, theme::kBackground);
                if (child == get(Status)) SetTextColor(dc, theme::kMuted);
                return result;
            }
            case WM_CTLCOLORBTN:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                if (const auto brush = theme::control_color(msg, wp, lp, theme::kBackground)) return brush;
                break;
            case WM_DRAWITEM:
                if (wp == Preview || wp == IncomingPreview) {
                    draw_preview(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
                    return TRUE;
                }
                break;
            case WM_CLOSE:
                stop();
                visited_album.clear();
                clear_arrivals();
                image.reset();
                incoming_image.reset();
                ShowWindow(window, SW_HIDE);
                return 0;
            case kApplySelection:
                if (selection_pending) selection();
                return 0;
            case WM_TIMER:
                if (wp == 1) {
                    KillTimer(window, 1);
                    populate(selected_paths());
                    return 0;
                }
                break;
            case WM_COMMAND:
                if (updating) return 0;
                switch (LOWORD(wp)) {
                    case Library: if (!library()) switch_page(LibraryPage::captures); break;
                    case History: if (!history()) switch_page(LibraryPage::history); break;
                    case Storage: if (page != LibraryPage::storage) switch_page(LibraryPage::storage); break;
                    case Duplicates: if (!duplicates()) switch_page(LibraryPage::duplicates); break;
                    case Search: if (HIWORD(wp) == EN_CHANGE) SetTimer(window, 1, 150, nullptr); break;
                    case Game:
                        if (HIWORD(wp) == CBN_SELCHANGE) {
                            const auto index = choice(Game);
                            game_filter = index > 0 && static_cast<std::size_t>(index) <= games.size()
                                ? games[index - 1] : std::filesystem::path{};
                            populate();
                        }
                        break;
                    case Date: case Type: case Runs:
                        if (HIWORD(wp) == CBN_SELCHANGE) populate();
                        break;
                    case Favorites: populate(); break;
                    case NewCaptures:
                        if (!arrivals.empty()) clear_arrivals();
                        populate();
                        break;
                    case Collections: if (HIWORD(wp) == CBN_SELCHANGE) load_collection(); break;
                    case SaveCollection: edit_collection(false); break;
                    case DeleteCollection: edit_collection(true); break;
                    case CopyFiles: copy_selected(); break;
                    case OpenExisting: case OpenIncoming:
                        if (const auto* match = selected_match()) {
                            open_path(LOWORD(wp) == OpenExisting ? match->existing : match->incoming);
                        }
                        break;
                    case KeepExisting: resolve_match(false); break;
                    case Favorite: label_capture(true); break;
                    case Save: label_capture(false); break;
                    case Play: play(); break;
                    case Stop: stop(); enable(Stop, false); InvalidateRect(get(Preview), nullptr, TRUE); break;
                    case Action: action(); break;
                    case Refresh:
                        if (duplicates()) populate();
                        else if (callbacks->refresh_captures) callbacks->refresh_captures();
                        break;
                }
                return 0;
            case WM_NOTIFY: {
                const auto* notice = reinterpret_cast<NMHDR*>(lp);
                if (notice->idFrom == Rows && !updating) {
                    if (notice->code == LVN_ITEMCHANGED) {
                        const auto* change = reinterpret_cast<const NMLISTVIEW*>(lp);
                        // A single move can first deselect the old row, then select
                        // the new one. Read the final selection after both notices.
                        if (((change->uOldState ^ change->uNewState) & LVIS_SELECTED) && !selection_pending) {
                            selection_pending = PostMessageW(window, kApplySelection, 0, 0) != FALSE;
                        }
                    } else if (notice->code == LVN_BEGINDRAG && library()) {
                        const auto paths = selected_paths();
                        if (!paths.empty() && !drag_capture_files(window, paths)) {
                            set(Status, L"Could not drag the files. Try Copy selected files.");
                        }
                    } else if (notice->code == NM_DBLCLK || notice->code == NM_RETURN) {
                        if (duplicates()) {
                            if (const auto* match = selected_match()) open_path(match->incoming);
                        } else if (const auto* capture = selected_capture(); capture && capture->video) play();
                        else action();
                    }
                }
                return 0;
            }
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == WM_NCDESTROY && self) {
            self->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, msg, wp, lp);
        }
        return self ? self->message(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
    }
};

WindowsLibrary::WindowsLibrary() : impl_(std::make_unique<Impl>()) {}
WindowsLibrary::~WindowsLibrary() = default;

void WindowsLibrary::show(HWND owner, LibraryPage page, const PlatformCallbacks& callbacks) {
    impl_->callbacks = &callbacks;
    if (impl_->labels_path.empty() && !impl_->state.settings_directory.empty()) {
        impl_->labels_path = impl_->state.settings_directory / L"capture-labels.json";
        const auto data = read_local_data(impl_->labels_path);
        if (data.is_object()) impl_->labels = data.as_object();
        impl_->preferences_path = impl_->state.settings_directory / L"capture-library.json";
        const auto preferences = read_local_data(impl_->preferences_path);
        if (preferences.is_object()) impl_->preferences = preferences.as_object();
        if (const auto* collections = preferences.find("collections"); collections && collections->is_object()) {
            impl_->collections = collections->as_object();
        }
    }
    if (!impl_->window) {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW type{};
        type.lpfnWndProc = Impl::procedure;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        type.lpszClassName = kClass;
        RegisterClassW(&type);
        const auto old_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto dpi = GetDpiForWindow(owner);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto& work = monitor.rcWork;
        const int width = std::min(MulDiv(1000, dpi, 96), static_cast<int>(work.right - work.left));
        const int height = std::min(MulDiv(680, dpi, 96), static_cast<int>(work.bottom - work.top));
        CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"CaptureDock · Album tools", WS_OVERLAPPEDWINDOW,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, owner, nullptr, instance, impl_.get());
        if (old_dpi) SetThreadDpiAwarenessContext(old_dpi);
        if (!impl_->window) return;
    }
    ShowWindow(impl_->window, SW_SHOW);
    impl_->switch_page(page);
    SetForegroundWindow(impl_->window);
}

void WindowsLibrary::show_arrivals(HWND owner, const PlatformCallbacks& callbacks, const CapturePaths& captures) {
    show(owner, LibraryPage::captures, callbacks);
    if (!impl_->window || captures.empty()) return;
    impl_->updating = true;
    impl_->set(Search, L"");
    impl_->set(CollectionName, L"");
    impl_->collection_choices();
    impl_->game_filter.clear();
    impl_->choices();
    impl_->choice(Date, 0);
    impl_->choice(Type, 0);
    impl_->check(Favorites, false);
    impl_->arrivals.clear();
    for (const auto& path : captures) {
        std::error_code error;
        auto absolute = std::filesystem::absolute(path, error);
        if (!error) impl_->arrivals.insert(absolute.lexically_normal().make_preferred());
    }
    impl_->set(NewCaptures, L"From notification");
    impl_->check(NewCaptures, true);
    impl_->updating = false;
    impl_->populate();
}

void WindowsLibrary::update(const MenuState& state) {
    const bool album = state.capture_library != impl_->state.capture_library;
    const bool history = state.sync_history != impl_->state.sync_history;
    const bool loading = state.recent_loading != impl_->state.recent_loading;
    const bool destination = state.destination != impl_->state.destination;
    const bool busy = state.sync_busy != impl_->state.sync_busy;
    const auto previous = impl_->window && album ? impl_->selected_paths() : CapturePaths{};
    impl_->state = state;
    if (!impl_->window) return;
    if (destination) {
        impl_->visited_album.clear();
        impl_->clear_arrivals();
        impl_->game_filter.clear();
        if (impl_->library() && IsWindowVisible(impl_->window)) impl_->begin_visit();
    }
    if (album || history || destination) impl_->choices();
    if (destination || (impl_->history() && history) || (!impl_->history() && album)) impl_->populate(previous);
    else if (!impl_->history() && loading && state.recent_loading) impl_->set(Status, L"Scanning the album…");
    else if (impl_->history()) impl_->selection();
    else if (impl_->duplicates() && busy) impl_->selection();
}

bool WindowsLibrary::route_key(MSG& message) const {
    const HWND window = impl_->window;
    if (!window || !IsWindowVisible(window) || (message.hwnd != window && !IsChild(window, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(window, WM_CLOSE, 0, 0);
        return true;
    }
    if (impl_->library() && message.hwnd == impl_->get(Rows) && message.message == WM_KEYDOWN &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (message.wParam == 'C') {
            impl_->copy_selected();
            return true;
        }
        if (message.wParam == 'A') {
            ListView_SetItemState(impl_->get(Rows), -1, LVIS_SELECTED, LVIS_SELECTED);
            return true;
        }
    }
    return IsDialogMessageW(window, &message) != FALSE;
}
}
