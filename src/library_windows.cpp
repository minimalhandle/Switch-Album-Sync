#include "library_windows.hpp"
#include "album_tools_windows.hpp"
#include "annotate_windows.hpp"
#include "import_windows.hpp"
#include "text_search_windows.hpp"
#include "video_windows.hpp"
#include "share_windows.hpp"
#include "theme_windows.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/sync.hpp"
#include "nso_album_sync/util.hpp"

#include <commctrl.h>
#include <gdiplus.h>
#include <mfplay.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <future>
#include <map>
#include <set>

namespace nso {
namespace {
constexpr wchar_t kClass[] = L"SwitchAlbumSyncAlbumTools";
constexpr UINT kApplySelection = WM_APP + 72;
constexpr UINT kTextIndexed = WM_APP + 73;
constexpr double kMaxGifSeconds = 15;
enum Control { Library = 3000, History, Storage, Duplicates, Title, Search, NewCaptures, Game, Date, Type, Favorites,
               Collections, CollectionName, SaveCollection, DeleteCollection, ClearFilters, Runs, NextRetry, Rows, Preview,
               ExistingLabel, IncomingLabel, IncomingPreview, OpenExisting, OpenIncoming, KeepExisting,
               Tags, Save, Favorite, Play, Stop, Action, Refresh, CopyFiles, QuickShare, Status, VideoSurface,
               EditCapture, TextSearch, End,
               AssignGame };
// Library list columns, for sorting.
enum Column { NameColumn, GameColumn, DateColumn, TypeColumn };

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
    std::future<std::filesystem::path> share_export;
    bool share_dialog_open = false;
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
    Imaging imaging;
    std::unique_ptr<Gdiplus::Image> image;
    std::unique_ptr<Gdiplus::Image> incoming_image;
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player;
    bool com = false;
    int sort_column = DateColumn;
    bool sort_descending = true;
    bool text_search = false;
    ScreenshotText screenshot_text;

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
        // TRUE keeps the hint visible while the box has focus, which it has
        // when the library opens.
        SendMessageW(get(Search), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search captures, games, or tags"));
        SendMessageW(get(Tags), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Tags, separated by commas"));
        SendMessageW(get(Tags), EM_LIMITTEXT, 512, 0);
        button(NewCaptures, L"New since last visit", BS_AUTOCHECKBOX);
        make(CollectionName, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        SendMessageW(get(CollectionName), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Collection name"));
        SendMessageW(get(CollectionName), EM_LIMITTEXT, 80, 0);
        button(SaveCollection, L"Save filters");
        button(DeleteCollection, L"Delete");
        button(ClearFilters, L"Clear filters");
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
        button(EditCapture, L"Edit…");
        button(TextSearch, L"Text in screenshots", BS_AUTOCHECKBOX);
        check(TextSearch, text_search);
        button(Refresh, L"Refresh");
        button(CopyFiles, L"Copy");
        button(QuickShare, L"Quick Share…");
        make(NextRetry, L"STATIC", L"", SS_NOPREFIX);
        make(Status, L"STATIC", L"", SS_NOPREFIX);
        fonts();
        theme::apply(window);
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
        place(Search, 20, 109, width - 431, 26);
        place(TextSearch, width - 401, 109, 182, 26);
        place(NewCaptures, width - 211, 109, 196, 26);
        place(Game, 20, 145, width - 404, 230);
        place(Date, width - 374, 145, 116, 175);
        place(Type, width - 248, 145, 105, 170);
        place(Favorites, width - 133, 143, 118, 27);
        place(Collections, 20, 181, width - 483, 220);
        place(CollectionName, width - 453, 181, 150, 26);
        place(SaveCollection, width - 293, 180, 96, 27);
        place(DeleteCollection, width - 187, 180, 63, 27);
        place(ClearFilters, width - 114, 180, 94, 27);
        place(Runs, 20, 110, width - 40, 235);
        place(NextRetry, 20, 146, width - 40, 22);
        const int top = library() ? 219 : duplicates() ? 109 : history() ? 174 : 148;
        const int list_height = duplicates() ? std::max(86, height / 4) : height - top - 78;
        place(Rows, 20, top, library() ? panel_x - 34 : width - 40, list_height);
        fit_columns();
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
        place(EditCapture, panel_x + 152, 226 + preview_height, std::min(80, panel - 152), 27);
        place(Tags, panel_x, 256 + preview_height, panel, 27);
        place(Save, panel_x, 286 + preview_height, 96, 27);
        place(Favorite, panel_x + 104, 286 + preview_height, panel - 104, 27);
        place(Status, 20, height - 69, library() ? panel_x - 34 : width - 40, 21);
        place(Action, 20, height - 39, 177, 29);
        place(KeepExisting, 207, height - 39, 151, 29);
        place(Refresh, duplicates() ? 368 : 207, height - 39, 86, 29);
        place(CopyFiles, 303, height - 39, 86, 29);
        place(QuickShare, 399, height - 39, 140, 29);
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
        if (state.capture_library) for (const auto& item : *state.capture_library) distinct[capture_game(item)] = true;
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
        const bool has_runs = state.sync_history && !state.sync_history->empty();
        if (has_runs) for (const auto& run : *state.sync_history) {
            option(Runs, wide(run.string("time")) + (run.boolean("automatic_retry") ? L"  ·  Automatic retry" : L"") +
                L"  ·  " + std::to_wstring(run.integer("downloaded")) +
                L" downloaded  ·  " + std::to_wstring(run.integer("skipped")) + L" skipped  ·  " +
                std::to_wstring(run.integer("failed")) + L" failed");
        } else {
            // An empty dropdown looks broken; say why there's nothing to pick.
            option(Runs, L"No sync runs yet");
        }
        EnableWindow(get(Runs), has_runs);
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
        if (name.empty()) {
            set(CollectionName, L"");
            return;
        }
        const auto& filters = collections.at(name);
        updating = true;
        set(Search, wide(filters.string("query")));
        set(CollectionName, wide(name));
        game_filter = path_from_utf8(filters.string("game"));
        const auto legacy = state.destination / L"Album";
        if (is_default_album_folder(state.destination)) {
            if (game_filter == legacy) game_filter = state.destination;
            else if (game_filter.parent_path() == legacy) game_filter = state.destination / game_filter.filename();
        }
        choices();
        choice(Date, static_cast<int>(std::clamp<std::int64_t>(filters.integer("date"), 0, 3)));
        choice(Type, static_cast<int>(std::clamp<std::int64_t>(filters.integer("type"), 0, 2)));
        check(Favorites, filters.boolean("favorites"));
        clear_arrivals();
        check(NewCaptures, filters.boolean("new_captures"));
        updating = false;
        populate();
    }

    void clear_filters() {
        updating = true;
        set(Search, L"");
        set(CollectionName, L"");
        collection_choices();
        game_filter.clear();
        choice(Game, 0);
        choice(Date, 0);
        choice(Type, 0);
        check(Favorites, false);
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
        if (!remove && collections.contains(name) &&
            theme::message_box(window, L"Replace the filters for “" + wide(name) + L"”?",
                L"Save smart collection", MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
        auto next = collections;
        if (remove) next.erase(name);
        else next[name] = Json::object{
            {"query", utf8(text_of(get(Search)))},
            {"game", path_to_utf8(game_filter)},
            {"date", choice(Date)}, {"type", choice(Type)}, {"favorites", checked(Favorites)},
            {"new_captures", arrivals.empty() && checked(NewCaptures)}};
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
        auto old = visits.find(key);
        if (old == visits.end() && is_default_album_folder(visited_album)) {
            old = visits.find(path_to_utf8(visited_album / L"Album"));
        }
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
            if (!index) first_column_width = width;
            ListView_InsertColumn(get(Rows), index++, &column);
        }
        fit_columns();
    }
    // The first column (capture or game name) takes the width the others
    // leave, so names aren't cut off next to an empty, untitled header area.
    int first_column_width = 0;
    void fit_columns() {
        const HWND list = get(Rows);
        const HWND header = list ? ListView_GetHeader(list) : nullptr;
        const int count = header ? Header_GetItemCount(header) : 0;
        if (count <= 0) return;
        RECT client{};
        GetClientRect(list, &client);
        int others = 0;
        for (int i = 1; i < count; ++i) others += ListView_GetColumnWidth(list, i);
        ListView_SetColumnWidth(list, 0, std::max<int>(px(first_column_width), client.right - others));
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
            ? (paths.size() == 1 ? L"File copied." : std::to_wstring(paths.size()) + L" files copied.")
            : L"Could not copy the selected files. Refresh if a file has moved.");
    }

    void capture_menu(LPARAM position) {
        const HWND list = get(Rows);
        POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        if (point.x == -1 && point.y == -1) {
            const int row = ListView_GetNextItem(list, -1, LVNI_FOCUSED | LVNI_SELECTED);
            RECT bounds{};
            if (row < 0 || !ListView_GetItemRect(list, row, &bounds, LVIR_LABEL)) return;
            point = {bounds.left, bounds.bottom};
            ClientToScreen(list, &point);
        } else {
            LVHITTESTINFO hit{};
            hit.pt = point;
            ScreenToClient(list, &hit.pt);
            const int row = ListView_SubItemHitTest(list, &hit);
            if (row < 0 || !(hit.flags & LVHT_ONITEM)) return;
            if (!(ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED)) {
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, CopyFiles, L"Copy\tCtrl+C");
        AppendMenuW(menu, ListView_GetSelectedCount(list) == 1 ? MF_STRING : MF_GRAYED, EditCapture, L"Edit…");
        AppendMenuW(menu, labels_path.empty() ? MF_GRAYED : MF_STRING, AssignGame, L"Assign game…");
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command) SendMessageW(window, WM_COMMAND, command, 0);
    }

    void quick_share() {
        if (share_export.valid() || share_dialog_open) return;
        const auto paths = selected_paths();
        if (paths.empty()) return;
        share_dialog_open = true;
        theme::Dialog dialog;
        dialog.title = L"Quick Share";
        dialog.heading = L"Export " + std::to_wstring(paths.size()) + L" selected captures";
        dialog.width = 580;
        dialog.text = L"Choose a folder of copies, a ZIP file, or a PNG grid of up to 48 screenshots. "
                      L"Grids include screenshots only. Resized copies are saved as PNG.";
        dialog.checkbox_label = L"Resize screenshots to fit within 1280 × 1280";
        dialog.buttons = {{IDCANCEL, L"Cancel"}, {IDNO, L"Export ZIP"}, {IDOK, L"Export folder", true}};
        const auto screenshots = std::count_if(paths.begin(), paths.end(), is_share_screenshot);
        if (screenshots > 0 && screenshots <= 48) {
            dialog.buttons.insert(dialog.buttons.begin() + 1, {IDYES, L"Screenshot grid"});
        }
        const int mode = theme::run_dialog(window, dialog);
        const auto folder = mode == IDCANCEL ? std::filesystem::path{} : pick_share_folder(window);
        share_dialog_open = false;
        if (folder.empty()) return;
        try {
            const auto format = mode == IDYES ? ShareFormat::contact_sheet :
                mode == IDNO ? ShareFormat::zip : ShareFormat::folder;
            share_export = std::async(std::launch::async, export_captures, paths, folder, format, dialog.checked);
            SetTimer(window, 2, 150, nullptr);
            enable(QuickShare, false);
            set(Status, L"Exporting captures…");
        } catch (const std::exception&) {
            set(Status, L"Could not start the export.");
        }
    }

    void finish_share() {
        if (!share_export.valid() || share_export.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        KillTimer(window, 2);
        std::wstring message;
        try {
            message = L"Export saved to " + share_export.get().wstring();
        } catch (const std::exception& error) {
            message = L"Export failed. " + wide(error.what());
        }
        enable(QuickShare, library() && ListView_GetSelectedCount(get(Rows)) > 0);
        if (IsWindowVisible(window)) theme::message_box(window, message, L"Quick Share", MB_OK);
        if (library()) set(Status, message);
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
    // Labels are keyed relative to the album so tags, favourites and assigned
    // games survive moving or renaming the album folder. Files outside the
    // album keep an absolute key.
    std::string label_key(const std::filesystem::path& path) const {
        if (state.destination.empty()) return path_to_utf8(path);
        const auto relative = path.lexically_normal().lexically_relative(state.destination.lexically_normal());
        if (relative.empty() || *relative.begin() == L"..") return path_to_utf8(path);
        return path_to_utf8(relative);
    }
    const Json& annotation(const std::filesystem::path& path) const {
        static const Json empty;
        auto found = labels.find(label_key(path));
        // Older builds used absolute keys; migrate_labels() rewrites them.
        if (found == labels.end()) found = labels.find(path_to_utf8(path));
        return found == labels.end() ? empty : found->second;
    }
    std::filesystem::path capture_game(const LocalCapture& capture) const {
        const auto game = path_from_utf8(annotation(capture.path).string("game"));
        if (game.empty()) return capture.game_folder;
        // Assigned games are stored as the game folder's name in this album.
        return state.destination / (game.is_absolute() ? game.filename() : game);
    }
    void migrate_labels() {
        if (labels_path.empty() || state.destination.empty()) return;
        auto next = labels;
        bool changed = false;
        for (const auto& [key, value] : labels) {
            const auto path = path_from_utf8(key);
            if (!path.is_absolute()) continue;
            const auto relative = label_key(path);
            if (relative == key) continue;  // Belongs to a different album.
            Json migrated = value;
            if (value.is_object()) {
                auto fields = value.as_object();
                if (const auto game = fields.find("game"); game != fields.end() && game->second.is_string()) {
                    const auto folder = path_from_utf8(game->second.as_string());
                    if (folder.is_absolute()) game->second = Json(path_to_utf8(folder.filename()));
                }
                migrated = Json(std::move(fields));
            }
            next.erase(key);
            next.emplace(relative, std::move(migrated));
            changed = true;
        }
        if (changed && write_local_data(labels_path, Json(next))) labels.swap(next);
    }

    void assign_game() {
        const auto paths = selected_paths();
        if (paths.empty() || labels_path.empty()) return;
        const auto destination = state.destination;
        std::set<std::filesystem::path> available;
        for (const auto& capture : *state.capture_library) {
            available.insert(capture.game_folder);
            available.insert(capture_game(capture));
        }
        available.erase(destination);
        const CapturePaths targets(available.begin(), available.end());
        theme::Dialog dialog;
        dialog.title = L"Assign game";
        dialog.heading = L"Assign a game to selected captures";
        dialog.text = L"Choose an existing game. This updates the library and storage totals without moving files.";
        dialog.choices = {L"Use detected game"};
        for (const auto& target : targets) dialog.choices.push_back(game_name(target));
        dialog.single_choice = true;
        dialog.buttons = {{IDOK, L"Apply", true}, {IDCANCEL, L"Cancel"}};
        if (theme::run_dialog(window, dialog) != IDOK || dialog.selected.empty() ||
            destination != state.destination) return;
        const auto index = dialog.selected.front();
        const auto game = index == 0 ? std::string{} : path_to_utf8(targets[index - 1].filename());
        auto next = labels;
        for (const auto& path : paths) {
            const auto& previous = annotation(path);
            const auto key = label_key(path);
            auto fields = previous.is_object() ? previous.as_object() : Json::object{};
            next.erase(path_to_utf8(path));
            if (game.empty()) fields.erase("game");
            else fields["game"] = game;
            if (fields.empty()) next.erase(key);
            else next[key] = Json(std::move(fields));
        }
        if (!write_local_data(labels_path, Json(next))) {
            set(Status, L"Could not save game assignments. Check access to Switch Album Sync settings.");
            return;
        }
        labels.swap(next);
        choices();
        populate(paths);
        set(Status, L"Game assignment updated for " + std::to_wstring(paths.size()) + L" capture(s).");
    }
    void library_rows() {
        auto query = folded(text_of(get(Search)));
        const bool tag_only = query.starts_with(L"tag:");
        if (tag_only) query.erase(0, query.find_first_not_of(L" \t", 4));
        const int date = choice(Date), type = choice(Type);
        const bool favorites = checked(Favorites);
        const bool new_only = checked(NewCaptures);
        const auto now = unix_now();
        struct Match {
            std::size_t index;
            std::wstring name, game;
            bool favorite;
        };
        std::vector<Match> matches_found;
        if (state.capture_library) for (std::size_t i = 0; i < state.capture_library->size(); ++i) {
            const auto& capture = (*state.capture_library)[i];
            if (!arrivals.empty()) {
                if (!arrivals.contains(capture.path.lexically_normal())) continue;
            } else if (new_only && capture.added_ms <= previous_visit) continue;
            const auto& label = annotation(capture.path);
            const auto game = capture_game(capture);
            const bool favorite = label.boolean("favorite");
            if (favorites && !favorite) continue;
            if (!game_filter.empty() && game != game_filter) continue;
            if ((type == 1 && capture.video) || (type == 2 && !capture.video)) continue;
            if (date == 1 && date_text(now) != date_text(capture.modified)) continue;
            if (date >= 2 && capture.modified < now - (date == 2 ? 7 : 30) * 86400LL) continue;
            const auto name = capture.path.filename().wstring();
            const auto game_label = game_name(game);
            const auto tags = wide(label.string("tags"));
            auto searchable = tag_only ? tags : name + L" " + game_label + L" " + tags;
            if (!query.empty() && folded(searchable).find(query) == std::wstring::npos) {
                // Text read from the screenshot, if text search is on.
                if (tag_only || !text_search || capture.video ||
                    screenshot_text.text(label_key(capture.path)).find(query) == std::wstring::npos) continue;
            }
            matches_found.push_back({i, name, game_label, favorite});
        }
        if (!state.capture_library) matches_found.clear();
        static const std::vector<LocalCapture> no_captures;
        const auto& library_items = state.capture_library ? *state.capture_library : no_captures;
        std::stable_sort(matches_found.begin(), matches_found.end(), [&](const Match& a, const Match& b) {
            const auto& left = library_items[a.index];
            const auto& right = library_items[b.index];
            int order = 0;
            switch (sort_column) {
                case NameColumn: order = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) - CSTR_EQUAL; break;
                case GameColumn: order = CompareStringOrdinal(a.game.c_str(), -1, b.game.c_str(), -1, TRUE) - CSTR_EQUAL; break;
                case TypeColumn: order = static_cast<int>(left.video) - static_cast<int>(right.video); break;
                default: break;
            }
            if (order == 0) order = left.modified < right.modified ? -1 : left.modified > right.modified ? 1 : 0;
            return sort_descending ? order > 0 : order < 0;
        });
        for (const auto& match : matches_found) {
            const auto& capture = library_items[match.index];
            add_row({match.favorite ? L"★ " + match.name : match.name, match.game, date_text(capture.modified),
                     capture.video ? L"Video" : L"Screenshot"});
            rows.push_back(match.index);
        }
        show_sort_arrow();
        std::wstring status = state.recent_loading ? L"Scanning the album…" : rows.empty() ?
            (new_only ? L"No new captures match this view." : L"No matching captures. Clear filters or refresh the album.") :
            std::to_wstring(rows.size()) + L" captures · Ctrl/Shift to select several, then drag or copy.";
        if (text_search && screenshot_text.running()) {
            status += L" Reading text: " + std::to_wstring(screenshot_text.done()) + L" of " +
                std::to_wstring(screenshot_text.total()) + L".";
        } else if (text_search && !screenshot_text.available()) {
            status += L" Text search needs a Windows text recognition language (Settings → Time & language).";
        }
        set(Status, status);
    }

    // Updates the reading-text progress without rebuilding the list.
    void library_status_only() {
        if (!text_search) return;
        std::wstring status = std::to_wstring(rows.size()) + L" captures.";
        if (screenshot_text.running()) {
            status += L" Reading text: " + std::to_wstring(screenshot_text.done()) + L" of " +
                std::to_wstring(screenshot_text.total()) + L".";
        } else if (screenshot_text.available()) {
            status += L" Text in screenshots is ready to search.";
        } else {
            status += L" Text search needs a Windows text recognition language (Settings → Time & language).";
        }
        set(Status, status);
    }

    void show_sort_arrow() {
        const HWND header = ListView_GetHeader(get(Rows));
        for (int column = 0; column < Header_GetItemCount(header); ++column) {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            Header_GetItem(header, column, &item);
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column == sort_column) item.fmt |= sort_descending ? HDF_SORTDOWN : HDF_SORTUP;
            Header_SetItem(header, column, &item);
        }
    }

    void sort_by(int column) {
        if (column == sort_column) sort_descending = !sort_descending;
        else {
            sort_column = column;
            sort_descending = column == DateColumn;  // Newest first; names A to Z
        }
        populate(selected_paths());
    }

    std::vector<ScreenshotText::Item> text_items() const {
        std::vector<ScreenshotText::Item> items;
        if (state.capture_library) for (const auto& capture : *state.capture_library) {
            if (!capture.video) items.push_back({label_key(capture.path), capture.path, capture.modified});
        }
        return items;
    }

    void start_text_search() {
        if (text_search && !state.settings_directory.empty() && state.capture_library) {
            screenshot_text.start(state.settings_directory / L"capture-text.json", text_items(), window, kTextIndexed);
        }
    }

    void toggle_text_search() {
        const bool enable = checked(TextSearch);
        if (enable && !preferences.contains("textSearch") &&
            theme::message_box(window, L"Read the text in your screenshots so search can find it? Windows' built-in text "
                L"recognition does this on this PC; nothing is uploaded. It runs in the background while the library is open.",
                L"Text in screenshots", MB_YESNO) != IDYES) {
            check(TextSearch, false);
            return;
        }
        text_search = enable;
        auto data = preferences;
        data["textSearch"] = enable;
        if (!preferences_path.empty() && write_local_data(preferences_path, data)) preferences.swap(data);
        if (enable) start_text_search();
        else screenshot_text.stop();
        populate(selected_paths());
    }

    void edit_capture() {
        if (ListView_GetSelectedCount(get(Rows)) != 1) return;
        const auto* capture = selected_capture();
        if (!capture) return;
        const auto path = capture->path;
        if (!capture->video) {
            const auto album = state.destination;
            const auto* callbacks_now = callbacks;
            open_screenshot_editor(window, path, [album, callbacks_now](const std::filesystem::path& saved) {
                // A copy saved into the album appears in the library after a refresh.
                const auto relative = saved.lexically_relative(album);
                if (!relative.empty() && *relative.begin() != L".." && callbacks_now && callbacks_now->refresh_captures) {
                    callbacks_now->refresh_captures();
                }
            });
            return;
        }
        stop();
        const auto info = probe_video(path);
        if (!info || info->seconds <= 0) {
            theme::message_box(window, L"This video couldn't be opened.", L"Edit video", MB_OK);
            return;
        }
        wchar_t length[32]{};
        swprintf_s(length, L"%.1f", info->seconds);
        theme::Dialog dialog;
        dialog.title = L"Edit video";
        dialog.heading = L"Save part of this video";
        dialog.text = std::wstring(L"The video is ") + length + L" seconds long. Enter where to start and end, in seconds "
            L"(for example 2.5-10). GIFs can be up to 15 seconds long and have no sound.";
        dialog.field = true;
        dialog.field_value = std::wstring(L"0-") + length;
        dialog.field_cue = L"Start-end in seconds";
        dialog.single_choice = true;
        dialog.choices = {L"MP4 clip with sound", L"GIF, 480 pixels wide, 15 frames per second",
                          L"GIF, 320 pixels wide, 10 frames per second"};
        dialog.selected = {0};
        dialog.width = 560;
        dialog.buttons = {{IDCANCEL, L"Cancel"}, {IDOK, L"Save…", true}};
        if (theme::run_dialog(window, dialog) != IDOK || dialog.selected.empty()) return;
        double start = 0, end = 0;
        auto range = dialog.field_value;
        std::replace_if(range.begin(), range.end(), [](wchar_t c) { return c == L'-' || c == L'–' || c == L','; }, L' ');
        if (swscanf_s(range.c_str(), L"%lf %lf", &start, &end) != 2 || start < 0 || end <= start) {
            theme::message_box(window, L"Enter a start and end time, such as 2.5-10.", L"Edit video", MB_OK);
            return;
        }
        end = std::min(end, info->seconds);
        const int format = dialog.selected.front();
        const bool gif = format != 0;
        if (gif && end - start > kMaxGifSeconds) {
            theme::message_box(window, L"GIFs can be up to 15 seconds long. Choose a shorter part.", L"Edit video", MB_OK);
            return;
        }
        const auto suggested = path.parent_path() / (path.stem().wstring() + (gif ? L"_clip.gif" : L"_clip.mp4"));
        const auto output = pick_save_file(window, L"Save the clip", suggested,
            gif ? L"GIF animation" : L"MP4 video", gif ? L"*.gif" : L"*.mp4");
        if (output.empty()) return;
        std::error_code error;
        if (std::filesystem::equivalent(output, path, error)) {
            theme::message_box(window, L"Choose a different name; the original is never replaced.", L"Edit video", MB_OK);
            return;
        }
        try {
            const bool finished = run_with_progress(window, L"Edit video", gif ? L"Making the GIF…" : L"Saving the clip…",
                [&](const std::atomic<bool>& cancelled) {
                    if (gif) export_video_gif(path, output, start, end, format == 1 ? 480 : 320, format == 1 ? 15 : 10, cancelled);
                    else export_video_clip(path, output, start, end, cancelled);
                });
            if (!finished) return;
        } catch (const std::exception& failure) {
            theme::message_box(window, L"Could not save the clip. " + wide(failure.what()), L"Edit video", MB_OK);
            return;
        }
        if (theme::message_box(window, L"Saved " + output.filename().wstring() + L". Open it now?", L"Edit video",
                MB_YESNO) == IDYES) open_path(output);
        if (!gif && callbacks && callbacks->refresh_captures) callbacks->refresh_captures();
    }

    void load_saved_data() {
        labels.clear();
        preferences.clear();
        collections.clear();
        if (state.settings_directory.empty()) return;
        labels_path = state.settings_directory / L"capture-labels.json";
        const auto data = read_local_data(labels_path);
        if (data.is_object()) labels = data.as_object();
        preferences_path = state.settings_directory / L"capture-library.json";
        const auto saved = read_local_data(preferences_path);
        if (saved.is_object()) preferences = saved.as_object();
        if (const auto* stored = saved.find("collections"); stored && stored->is_object()) collections = stored->as_object();
        text_search = saved.boolean("textSearch");
        if (window) check(TextSearch, text_search);
        migrate_labels();
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
            auto& usage = games_usage[capture_game(capture)];
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
            set(NextRetry, state.next_retry.empty() ? L"No automatic retry scheduled" :
                L"Next automatic retry: " + wide(state.next_retry));
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
        enable(QuickShare, selected > 0 && !share_export.valid());
        if (selected > 1) capture = nullptr;
        for (const auto id : {Action, Tags, Save, Favorite, EditCapture}) enable(id, capture != nullptr);
        enable(Play, capture && capture->video);
        enable(Stop, false);
        set(Tags, capture ? wide(annotation(capture->path).string("tags")) : L"");
        set(Favorite, capture && annotation(capture->path).boolean("favorite") ? L"Remove favorite" : L"Add favorite");
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
        const auto key = label_key(capture->path);
        const auto& previous = annotation(capture->path);
        const bool favorite = previous.boolean("favorite") != toggle;
        const auto tags = toggle ? previous.string("tags") : utf8(text_of(get(Tags)));
        auto next = labels;
        auto fields = previous.is_object() ? previous.as_object() : Json::object{};
        next.erase(path_to_utf8(capture->path));
        if (favorite) fields["favorite"] = true;
        else fields.erase("favorite");
        if (!tags.empty()) fields["tags"] = tags;
        else fields.erase("tags");
        if (fields.empty()) next.erase(key);
        else next[key] = Json(std::move(fields));
        if (!write_local_data(labels_path, Json(next))) {
            set(Status, L"Could not save labels. Check access to Switch Album Sync settings.");
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
        if (!keep_both && theme::message_box(window, L"Discard Switch Album Sync’s staged incoming copy and keep the album copy?",
            L"Duplicate review", MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
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
        const bool contrast = high_contrast();
        FillRect(draw.hDC, &bounds, contrast ? GetSysColorBrush(COLOR_WINDOW) : theme::brush(theme::kPreview));
        const auto& still = draw.CtlID == IncomingPreview ? incoming_image : image;
        if (!still) {
            const auto* capture = selected_capture();
            SetBkMode(draw.hDC, TRANSPARENT);
            SetTextColor(draw.hDC, contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(230, 231, 233));
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
                       SaveCollection, DeleteCollection, ClearFilters, Tags, Save, Favorite, Play, Stop, CopyFiles, QuickShare,
                       EditCapture, TextSearch}) show(id, library());
        for (auto id : {ExistingLabel, IncomingLabel, IncomingPreview, OpenExisting, OpenIncoming, KeepExisting}) show(id, duplicates());
        show(Preview, library() || duplicates());
        show(Runs, history());
        show(NextRetry, history());
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
                FillRect(dc, &bounds, high_contrast() ? GetSysColorBrush(COLOR_WINDOW) : theme::brush(theme::palette().background));
                if (!high_contrast()) {
                    bounds.bottom = px(60);
                    FillRect(dc, &bounds, theme::brush(theme::palette().card));
                    const RECT accent{px(20), px(56), px(52), px(59)};
                    theme::fill_round(dc, accent, px(1), theme::palette().accent);
                }
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
                    SetTextColor(dc, theme::palette().text);
                    return reinterpret_cast<LRESULT>(theme::brush(theme::palette().card));
                }
                const auto result = theme::control_color(msg, wp, lp, theme::palette().background);
                if (child == get(Status)) SetTextColor(dc, theme::palette().muted);
                return result;
            }
            case WM_CTLCOLORBTN:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                if (const auto brush = theme::control_color(msg, wp, lp, theme::palette().background)) return brush;
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
            case WM_CONTEXTMENU:
                if (library() && reinterpret_cast<HWND>(wp) == get(Rows)) {
                    capture_menu(lp);
                    return 0;
                }
                break;
            case kApplySelection:
                if (selection_pending) selection();
                return 0;
            case kTextIndexed:
                // New text is searchable now; refresh results if a search is active.
                if (library() && IsWindowVisible(window)) {
                    if (!text_of(get(Search)).empty()) populate(selected_paths());
                    else library_status_only();
                }
                return 0;
            case WM_TIMER:
                if (wp == 2) { finish_share(); return 0; }
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
                    case ClearFilters: clear_filters(); break;
                    case QuickShare: quick_share(); break;
                    case CopyFiles: copy_selected(); break;
                    case AssignGame: assign_game(); break;
                    case EditCapture: edit_capture(); break;
                    case TextSearch: toggle_text_search(); break;
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
                    } else if (notice->code == LVN_COLUMNCLICK && library()) {
                        sort_by(reinterpret_cast<const NMLISTVIEW*>(lp)->iSubItem);
                    } else if (notice->code == LVN_BEGINDRAG && library()) {
                        const auto paths = selected_paths();
                        if (!paths.empty() && !drag_capture_files(window, paths)) {
                            set(Status, L"Could not drag the files. Try Copy.");
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

void WindowsLibrary::reload_saved_data() {
    if (impl_->labels_path.empty()) return;  // Not loaded yet; the next show() reads fresh data.
    impl_->load_saved_data();
    if (!impl_->window) return;
    impl_->updating = true;
    impl_->collection_choices();
    impl_->choices();
    impl_->updating = false;
    impl_->populate(impl_->selected_paths());
}

void WindowsLibrary::show(HWND owner, LibraryPage page, const PlatformCallbacks& callbacks) {
    impl_->callbacks = &callbacks;
    if (impl_->labels_path.empty()) impl_->load_saved_data();
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
        CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Switch Album Sync · Album tools", WS_OVERLAPPEDWINDOW,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, owner, nullptr, instance, impl_.get());
        if (old_dpi) SetThreadDpiAwarenessContext(old_dpi);
        if (!impl_->window) return;
    }
    ShowWindow(impl_->window, SW_SHOW);
    impl_->switch_page(page);
    if (impl_->text_search && !impl_->screenshot_text.running()) impl_->start_text_search();
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
    // Read text in new or changed screenshots while the library is open.
    if (album && impl_->text_search && IsWindowVisible(impl_->window)) impl_->start_text_search();
    if (destination) {
        impl_->migrate_labels();
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
