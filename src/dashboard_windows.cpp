#include "dashboard_windows.hpp"
#include "library_windows.hpp"
#include <gdiplus.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace nso {
namespace {

constexpr wchar_t kDashboardClass[] = L"CaptureDockDashboard";
constexpr wchar_t kTitle[] = L"CaptureDock";
constexpr UINT kShowDashboard = WM_APP + 60;
constexpr UINT kThumbnailsReady = WM_APP + 61;
constexpr UINT_PTR kFeedbackTimer = 1;
constexpr int kWidth = 704;
constexpr int kMinimumWidth = 610;
constexpr int kHeight = 684;
constexpr int kDiscordSettingsHeight = 184;
constexpr int kMargin = 24;
constexpr int kThumbnailWidth = 96;
constexpr int kThumbnailHeight = 64;
constexpr std::size_t kThumbnailCount = 5;

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

enum Control {
    Title, Subtitle, RuleAccount, AccountHeading, AccountStatus, AccountAction,
    CredentialNote, ClearCredential, RuleAlbum, AlbumHeading, Destination,
    ChooseFolder, OpenFolder, LastSync, NextSync, SyncStatus, SyncDetail,
    Progress, SyncNow, RetryFailed, AutoSync, RuleCaptures, CapturesHeading,
    CopyLatest, ShowInFolder, Captures, EmptyCaptures, RuleSettings,
    DiscordHeading, DiscordToggle, DiscordUnavailable, CopyDiagnostics,
    Feedback, PasteSignIn, OpenLibrary, OpenHistory, OpenStorage, ImportCaptures, OpenDuplicates,
    DiscordUsername, DiscordPicture, DiscordPlayTime, DiscordZelda, DiscordAnimalCrossing,
    DiscordSplatoon3, DiscordSplatoon2, DiscordRefresh, DiscordHint,
    DiscordProfileHeading, DiscordGamesHeading, DiscordZeldaGames, ControlCount,
};

constexpr struct DiscordOption {
    Control control;
    const wchar_t* label;
    bool RpcSettings::*setting;
    std::function<void()> PlatformCallbacks::*callback;
} kDiscordOptions[] = {
    {DiscordPicture, L"Show profile picture", &RpcSettings::show_profile_picture, &PlatformCallbacks::toggle_rpc_profile_picture},
    {DiscordUsername, L"Show Switch username", &RpcSettings::show_username, &PlatformCallbacks::toggle_rpc_username},
    {DiscordPlayTime, L"Show total play time", &RpcSettings::show_play_time, &PlatformCallbacks::toggle_rpc_play_time},
    {DiscordZelda, L"Zelda Notes live location", &RpcSettings::zelda, &PlatformCallbacks::toggle_rpc_zelda},
    {DiscordAnimalCrossing, L"Animal Crossing details", &RpcSettings::animal_crossing, &PlatformCallbacks::toggle_rpc_animal_crossing},
    {DiscordSplatoon3, L"Splatoon 3 details", &RpcSettings::splatoon3, &PlatformCallbacks::toggle_rpc_splatoon3},
    {DiscordSplatoon2, L"Splatoon 2 details", &RpcSettings::splatoon2, &PlatformCallbacks::toggle_rpc_splatoon2},
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), length);
    return result;
}

bool clipboard_text(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    if (void* buffer = GlobalLock(memory)) {
        std::memcpy(buffer, text.c_str(), bytes);
        GlobalUnlock(memory);
        if (OpenClipboard(owner)) {
            const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
            CloseClipboard();
            if (copied) return true;
        }
    }
    GlobalFree(memory);
    return false;
}

bool reveal_capture(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return false;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(absolute.c_str());
    const bool opened = item && SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    CoTaskMemFree(item);
    if (SUCCEEDED(init)) CoUninitialize();
    return opened;
}

struct DeleteBitmap {
    void operator()(HBITMAP image) const { DeleteObject(image); }
};
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteBitmap>;

Bitmap thumbnail(const std::filesystem::path& path, SIZE size) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return {};
    }
    HBITMAP image = nullptr;
    factory->GetImage(size, SIIGBF_RESIZETOFIT, &image);
    factory->Release();
    return Bitmap(image);
}

Bitmap thumbnail_tile(SIZE size, HBITMAP source = nullptr) {
    HDC screen = GetDC(nullptr);
    if (!screen) return {};
    Bitmap result(CreateCompatibleBitmap(screen, size.cx, size.cy));
    HDC tile = CreateCompatibleDC(screen);
    if (result && tile) {
        HGDIOBJ previous = SelectObject(tile, result.get());
        RECT bounds{0, 0, size.cx, size.cy};
        FillRect(tile, &bounds, GetSysColorBrush(COLOR_WINDOW));
        BITMAP image{};
        if (source && GetObjectW(source, sizeof(image), &image) &&
            image.bmWidth > 0 && image.bmHeight > 0) {
            HDC input = CreateCompatibleDC(screen);
            if (input) {
                HGDIOBJ old_input = SelectObject(input, source);
                const double scale = std::min({1.0, static_cast<double>(size.cx) / image.bmWidth,
                                              static_cast<double>(size.cy) / image.bmHeight});
                const auto width = std::max(1, static_cast<int>(image.bmWidth * scale));
                const auto height = std::max(1, static_cast<int>(image.bmHeight * scale));
                SetStretchBltMode(tile, HALFTONE);
                StretchBlt(tile, (size.cx - width) / 2, (size.cy - height) / 2, width, height,
                           input, 0, 0, image.bmWidth, image.bmHeight, SRCCOPY);
                SelectObject(input, old_input);
                DeleteDC(input);
            }
        } else {
            SetBkMode(tile, TRANSPARENT);
            SetTextColor(tile, GetSysColor(COLOR_GRAYTEXT));
            SelectObject(tile, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(tile, L"Capture", -1, &bounds, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        SelectObject(tile, previous);
    }
    if (tile) DeleteDC(tile);
    ReleaseDC(nullptr, screen);
    return result;
}

// Shell thumbnail providers can perform disk IO and video decoding. The window
// only receives images for its current album and monitor scale.
class ThumbnailLoader {
public:
    void stop() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    struct Batch {
        std::size_t generation = 0;
        std::vector<Bitmap> images;
    };

    void request(HWND window, std::size_t generation, SIZE size,
                 const std::vector<std::filesystem::path>& paths) {
        {
            std::lock_guard lock(mutex_);
            generation_ = generation;
            pending_ = {window, generation, size, paths};
            ready_ = {};
        }
        if (!worker_.joinable() && !paths.empty()) {
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        }
        wake_.notify_one();
    }

    Batch take() {
        std::lock_guard lock(mutex_);
        return std::exchange(ready_, {});
    }

private:
    struct Request {
        HWND window = nullptr;
        std::size_t generation = 0;
        SIZE size{};
        std::vector<std::filesystem::path> paths;
    };

    void run(std::stop_token stop) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init)) return;
        while (!stop.stop_requested()) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this] { return !pending_.paths.empty(); })) break;
                request = std::exchange(pending_, {});
            }
            Batch batch{request.generation, {}};
            for (const auto& path : request.paths) {
                if (stop.stop_requested()) break;
                {
                    std::lock_guard lock(mutex_);
                    if (generation_ != request.generation) break;
                }
                const auto image = thumbnail(path, request.size);
                batch.images.push_back(thumbnail_tile(request.size, image.get()));
            }
            std::lock_guard lock(mutex_);
            if (!stop.stop_requested() && generation_ == request.generation) {
                ready_ = std::move(batch);
                PostMessageW(request.window, kThumbnailsReady, 0, 0);
            }
        }
        CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::size_t generation_ = 0;
    Request pending_;
    Batch ready_;
    std::jthread worker_;
};

}  // namespace

struct WindowsDashboard::Impl {
    ULONG_PTR imaging = 0;
    Impl() {
        Gdiplus::GdiplusStartupInput options;
        Gdiplus::GdiplusStartup(&imaging, &options, nullptr);
    }
    WindowsLibrary library;
    HBRUSH dashboard_paper = CreateSolidBrush(RGB(250, 243, 244));
    HBRUSH dashboard_red = CreateSolidBrush(RGB(196, 9, 43));
    HBRUSH dashboard_deep_red = CreateSolidBrush(RGB(129, 6, 31));
    ~Impl() {
        thumbnails.stop();
        if (window) DestroyWindow(window);
        if (images) ImageList_Destroy(images);
        if (body) DeleteObject(body);
        if (heading) DeleteObject(heading);
        if (title) DeleteObject(title);
        DeleteObject(dashboard_paper);
        DeleteObject(dashboard_red);
        DeleteObject(dashboard_deep_red);
        if (imaging) Gdiplus::GdiplusShutdown(imaging);
    }

    HWND window = nullptr;
    HWND controls[ControlCount]{};
    const PlatformCallbacks* callbacks = nullptr;
    std::function<bool(const std::filesystem::path&)> copy_capture;
    MenuState state;
    UINT dpi = 96;
    int scroll = 0;
    bool discord_expanded() const { return state.discord_available && state.discord; }
    int content_height() const { return px(kHeight + (discord_expanded() ? kDiscordSettingsHeight : 0)); }

    void fit_height() {
        if (IsZoomed(window) || IsIconic(window)) return;
        MONITORINFO monitor{sizeof(monitor)};
        RECT frame{}, current{};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
            !GetWindowRect(window, &current)) return;
        frame.bottom = content_height();
        AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
        const auto& work = monitor.rcWork;
        const auto height = std::min(frame.bottom - frame.top, work.bottom - work.top);
        const auto top = std::clamp(current.top, work.top, work.bottom - height);
        SetWindowPos(window, nullptr, current.left, top, current.right - current.left, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    bool marquee = false;
    bool hide_on_close = true;
    HFONT body = nullptr;
    HFONT heading = nullptr;
    HFONT title = nullptr;
    HIMAGELIST images = nullptr;
    std::vector<std::filesystem::path> shown_captures;
    std::size_t thumbnail_generation = 0;
    ThumbnailLoader thumbnails;

    int px(int units) const { return MulDiv(units, static_cast<int>(dpi), 96); }

    void control(Control id, const wchar_t* type, const wchar_t* text, DWORD style = 0,
                 DWORD ex_style = 0) {
        controls[id] = CreateWindowExW(ex_style, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2000 + id)),
            GetModuleHandleW(nullptr), nullptr);
    }

    void label(Control id, const wchar_t* text = L"", DWORD style = SS_LEFT) {
        control(id, L"STATIC", text, style | SS_NOPREFIX);
    }

    void button(Control id, const wchar_t* text, DWORD style = BS_PUSHBUTTON) {
        control(id, L"BUTTON", text, WS_TABSTOP | style);
    }

    void text(Control id, const std::wstring& value) {
        const int length = GetWindowTextLengthW(controls[id]);
        if (length == static_cast<int>(value.size())) {
            std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(controls[id], current.data(), length + 1);
            if (value == current.c_str()) return;
        }
        SetWindowTextW(controls[id], value.c_str());
    }

    void create_controls() {
        label(Title, kTitle);
        label(Subtitle, L"Keep your Nintendo Switch captures in a local album.");
        button(OpenLibrary, L"Capture library", BS_OWNERDRAW);
        button(OpenHistory, L"Sync history", BS_OWNERDRAW);
        button(OpenStorage, L"Storage", BS_OWNERDRAW);
        button(ImportCaptures, L"Import USB / SD…", BS_OWNERDRAW);
        button(OpenDuplicates, L"Duplicate review", BS_OWNERDRAW);
        label(RuleAccount, L"", SS_ETCHEDHORZ);
        label(AccountHeading, L"Nintendo Account");
        label(AccountStatus);
        button(PasteSignIn, L"Paste sign-in link…");
        button(AccountAction, L"Sign in…");
        label(CredentialNote);
        button(ClearCredential, L"Clear saved credential");
        label(RuleAlbum, L"", SS_ETCHEDHORZ);
        label(AlbumHeading, L"Album folder and sync");
        control(Destination, L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE);
        button(ChooseFolder, L"Choose folder…");
        button(OpenFolder, L"Open folder");
        label(LastSync);
        label(NextSync);
        label(SyncStatus);
        label(SyncDetail);
        control(Progress, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        button(SyncNow, L"Sync now", BS_DEFPUSHBUTTON);
        button(RetryFailed, L"Retry failed");
        button(AutoSync, L"Auto-sync", BS_AUTOCHECKBOX);
        label(RuleCaptures, L"", SS_ETCHEDHORZ);
        label(CapturesHeading, L"Recent captures");
        button(CopyLatest, L"Copy latest");
        button(ShowInFolder, L"Show in folder");
        control(Captures, WC_LISTVIEWW, L"Recent captures",
                WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE |
                LVS_SHAREIMAGELISTS | LVS_NOLABELWRAP);
        ListView_SetExtendedListViewStyle(controls[Captures], LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        label(EmptyCaptures);
        label(RuleSettings, L"", SS_ETCHEDHORZ);
        label(DiscordHeading, L"Discord Rich Presence");
        button(DiscordToggle, L"Enable Rich Presence", BS_AUTOCHECKBOX);
        for (const auto& option : kDiscordOptions) button(option.control, option.label, BS_AUTOCHECKBOX);
        button(DiscordRefresh, L"Refresh presence");
        label(DiscordHint, L"Discord desktop must be running. Visibility follows Discord Activity Sharing.");
        label(DiscordProfileHeading, L"Profile and play time");
        label(DiscordGamesHeading, L"Game details");
        label(DiscordZeldaGames, L"Zelda Notes live location in CaptureDock\nOcarina of Time (Switch 2)\nBreath of the Wild and Tears of the Kingdom (Switch 2 Editions)");
        label(DiscordUnavailable, L"Unavailable in this build.");
        button(CopyDiagnostics, L"Copy diagnostics");
        label(Feedback, L"", SS_RIGHT);
        for (const auto rule : {RuleAccount, RuleAlbum, RuleCaptures, RuleSettings}) {
            ShowWindow(controls[rule], SW_HIDE);
        }
        set_fonts();
        update(state);
    }

    void set_fonts() {
        const HFONT old_body = body;
        const HFONT old_heading = heading;
        const HFONT old_title = title;
        const auto font = [this](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        body = font(14, FW_NORMAL);
        heading = font(15, FW_SEMIBOLD);
        title = font(27, FW_SEMIBOLD);
        for (auto child : controls) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
        for (auto id : {AccountHeading, AlbumHeading, SyncStatus, CapturesHeading, DiscordHeading,
                        DiscordProfileHeading, DiscordGamesHeading}) {
            SendMessageW(controls[id], WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
        }
        SendMessageW(controls[Title], WM_SETFONT, reinterpret_cast<WPARAM>(title), TRUE);
        SendMessageW(controls[Destination], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(px(6), px(6)));
        if (old_body) DeleteObject(old_body);
        if (old_heading) DeleteObject(old_heading);
        if (old_title) DeleteObject(old_title);
    }

    void layout(int desired_scroll) {
        RECT client{};
        GetClientRect(window, &client);
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = content_height() - 1;
        info.nPage = static_cast<UINT>(std::max(0L, client.bottom));
        info.nPos = desired_scroll;
        SetScrollInfo(window, SB_VERT, &info, TRUE);
        scroll = GetScrollPos(window, SB_VERT);
        GetClientRect(window, &client);
        const int right = std::max(kMinimumWidth - kMargin,
                                  MulDiv(client.right, 96, static_cast<int>(dpi)) - kMargin);
        const int span = right - kMargin;
        const auto place = [&](Control id, int x, int y, int width, int height) {
            if (id != Title && id != Subtitle && id != OpenLibrary && id != OpenHistory &&
                id != OpenStorage && id != ImportCaptures && id != OpenDuplicates) y += 50;
            MoveWindow(controls[id], px(x), px(y) - scroll, px(width), px(height), TRUE);
        };
        place(Title, kMargin, 18, span, 36);
        place(Subtitle, kMargin, 56, span, 22);
        place(OpenLibrary, kMargin, 96, 116, 31);
        place(OpenHistory, kMargin + 124, 96, 90, 31);
        place(OpenStorage, kMargin + 222, 96, 68, 31);
        place(ImportCaptures, kMargin + 298, 96, 130, 31);
        place(OpenDuplicates, kMargin + 436, 96, 124, 31);
        place(RuleAccount, kMargin, 85, span, 2);
        place(AccountHeading, kMargin, 96, span - 130, 20);
        place(AccountStatus, kMargin, 120, span - (state.manual_sign_in ? 289 : 130), 31);
        place(PasteSignIn, right - 280, 106, 153, 29);
        place(AccountAction, right - 118, 106, 118, 29);
        place(CredentialNote, kMargin, 151, span - (state.legacy_credential ? 177 : 0), 34);
        place(ClearCredential, right - 166, 151, 166, 28);
        place(RuleAlbum, kMargin, 186, span, 2);
        place(AlbumHeading, kMargin, 196, span, 20);
        place(Destination, kMargin, 222, span - 221, 28);
        place(ChooseFolder, right - 212, 222, 112, 28);
        place(OpenFolder, right - 92, 222, 92, 28);
        place(LastSync, kMargin, 259, span / 2 - 8, 21);
        place(NextSync, kMargin + span / 2, 259, span / 2, 21);
        place(SyncStatus, kMargin, 284, span, 41);
        place(SyncDetail, kMargin, 327, span, 18);
        place(Progress, kMargin, 345, span, 8);
        place(SyncNow, kMargin, 363, 100, 29);
        place(RetryFailed, kMargin + 108, 363, 102, 29);
        place(AutoSync, right - 157, 363, 157, 29);
        place(RuleCaptures, kMargin, 407, span, 2);
        place(CapturesHeading, kMargin, 422, span - 220, 21);
        place(CopyLatest, right - 208, 416, 98, 28);
        place(ShowInFolder, right - 102, 416, 102, 28);
        place(Captures, kMargin, 451, span, 103);
        place(EmptyCaptures, kMargin, 457, span, 42);
        place(RuleSettings, kMargin, 561, span, 2);
        place(DiscordHeading, kMargin, 574, span - 270, 22);
        place(DiscordToggle, kMargin, 596, span - 270, 25);
        place(DiscordUnavailable, kMargin, 599, span - 270, 20);
        place(DiscordProfileHeading, kMargin, 626, span / 2 - 8, 21);
        place(DiscordGamesHeading, kMargin + span / 2, 626, span / 2 - 8, 21);
        for (std::size_t index = 0; index < std::size(kDiscordOptions); ++index) {
            const int column = index < 3 ? 0 : 1;
            const int row = static_cast<int>(index) - (column ? 3 : 0);
            place(kDiscordOptions[index].control, kMargin + column * (span / 2),
                  649 + row * 22, span / 2 - 8, 22);
        }
        place(DiscordRefresh, kMargin, 714, 142, 26);
        place(DiscordZeldaGames, kMargin, 747, span, 54);
        place(DiscordHint, kMargin, 805, span, 20);
        place(CopyDiagnostics, right - 128, 581, 128, 29);
        place(Feedback, right - 295, 585, 156, 24);
        InvalidateRect(window, nullptr, TRUE);
    }

    void refresh_captures(bool force = false) {
        const auto count = std::min(state.recent_captures.size(), kThumbnailCount);
        std::vector paths(state.recent_captures.begin(), state.recent_captures.begin() + count);
        if (!force && state.recent_captures == thumbnail_paths) return;
        thumbnail_paths = state.recent_captures;
        tray_images.clear();
        const auto selection = selected_capture();
        shown_captures = std::move(paths);
        ListView_DeleteAllItems(controls[Captures]);
        ListView_SetImageList(controls[Captures], nullptr, LVSIL_NORMAL);
        if (images) ImageList_Destroy(images);
        const SIZE size{px(kThumbnailWidth), px(kThumbnailHeight)};
        images = ImageList_Create(size.cx, size.cy, ILC_COLOR32, static_cast<int>(count), 1);
        ListView_SetImageList(controls[Captures], images, LVSIL_NORMAL);
        ListView_SetIconSpacing(controls[Captures], px(108), px(101));

        const auto blank = thumbnail_tile(size);
        int select_index = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (shown_captures[i] == selection) select_index = static_cast<int>(i);
            const int image = blank && images ? ImageList_Add(images, blank.get(), nullptr) : -1;
            const auto extension = shown_captures[i].extension().wstring();
            std::wstring caption = CompareStringOrdinal(extension.c_str(), -1, L".mp4", -1, TRUE) ==
                CSTR_EQUAL ? L"Video" : L"Screenshot";
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = static_cast<int>(i);
            item.iImage = image;
            item.pszText = caption.data();
            ListView_InsertItem(controls[Captures], &item);
        }
        if (count > 0) {
            ListView_SetItemState(controls[Captures], select_index,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        thumbnails.request(window, ++thumbnail_generation, size, thumbnail_paths);
    }

    std::vector<std::filesystem::path> thumbnail_paths;
    std::vector<Bitmap> tray_images;

    std::filesystem::path selected_capture() const {
        const int selected = ListView_GetNextItem(controls[Captures], -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= shown_captures.size()) return {};
        return shown_captures[static_cast<std::size_t>(selected)];
    }

    void update(const MenuState& next) {
        const bool resize = discord_expanded() != (next.discord_available && next.discord);
        const bool relayout = state.legacy_credential != next.legacy_credential ||
            state.manual_sign_in != next.manual_sign_in || state.discord_available != next.discord_available;
        state = next;
        library.update(state);
        if (!window) return;

        text(AccountStatus, state.manual_sign_in ? L"Copy the link from Nintendo, then paste it here." :
            state.sign_in_pending ? L"Waiting for Nintendo sign-in in your browser…" :
            state.signed_in ? (state.nickname.empty() ? L"Connected" : L"Connected as " + wide(state.nickname)) :
                              L"Not signed in");
        text(AccountAction, state.sign_in_pending ? L"Cancel sign-in" :
            state.signed_in ? L"Sign out" : L"Sign in…");
        ShowWindow(controls[PasteSignIn], state.manual_sign_in ? SW_SHOWNA : SW_HIDE);
        text(CredentialNote, !state.credential_status.empty() ? wide(state.credential_status) :
            state.signed_in ? L"" :
            L"Sign-in uses nxapi, a third-party service that processes Nintendo tokens, "
            L"account details and Coral API traffic. Details appear before you continue.");
        ShowWindow(controls[ClearCredential], state.legacy_credential ? SW_SHOWNA : SW_HIDE);

        text(Destination, state.destination.wstring());
        EnableWindow(controls[OpenFolder], !state.destination.empty());
        text(LastSync, L"Last successful sync  ·  " + wide(state.last_sync));
        text(NextSync, L"Next scheduled sync  ·  " + wide(state.next_sync));
        text(SyncStatus, wide(state.status));
        text(SyncDetail, wide(state.sync_detail));
        EnableWindow(controls[SyncNow], state.signed_in && !state.sync_busy);
        EnableWindow(controls[ImportCaptures], !state.sync_busy);
        EnableWindow(controls[RetryFailed], state.signed_in && !state.sync_busy && state.retry_available);
        SendMessageW(controls[AutoSync], BM_SETCHECK, state.auto_sync ? BST_CHECKED : BST_UNCHECKED, 0);
        text(AutoSync, L"Auto-sync (" + std::to_wstring(state.sync_interval_minutes) + L" min)");

        const bool animate = state.sync_busy && state.sync_total == 0;
        if (animate != marquee) {
            marquee = animate;
            LONG_PTR style = GetWindowLongPtrW(controls[Progress], GWL_STYLE);
            SetWindowLongPtrW(controls[Progress], GWL_STYLE,
                marquee ? style | PBS_MARQUEE : style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
            SendMessageW(controls[Progress], PBM_SETMARQUEE, marquee, 30);
        }
        if (!marquee) {
            const auto percent = state.sync_total ? static_cast<int>(100.0 *
                std::min(state.sync_completed, state.sync_total) / state.sync_total) : 0;
            SendMessageW(controls[Progress], PBM_SETPOS, percent, 0);
        }
        ShowWindow(controls[Progress], state.sync_busy ? SW_SHOWNA : SW_HIDE);

        refresh_captures();
        const bool has_captures = !shown_captures.empty();
        ShowWindow(controls[Captures], has_captures ? SW_SHOWNA : SW_HIDE);
        ShowWindow(controls[EmptyCaptures], has_captures ? SW_HIDE : SW_SHOWNA);
        text(EmptyCaptures, state.recent_loading ? L"Checking the album folder for captures…" :
            L"No captures in this album yet. New downloads will appear here.");
        EnableWindow(controls[CopyLatest], has_captures);
        EnableWindow(controls[ShowInFolder], !selected_capture().empty());

        ShowWindow(controls[DiscordToggle], state.discord_available ? SW_SHOWNA : SW_HIDE);
        ShowWindow(controls[DiscordUnavailable], state.discord_available ? SW_HIDE : SW_SHOWNA);
        SendMessageW(controls[DiscordToggle], BM_SETCHECK, state.discord ? BST_CHECKED : BST_UNCHECKED, 0);
        const int discord_visibility = discord_expanded() ? SW_SHOWNA : SW_HIDE;
        for (const auto& option : kDiscordOptions) {
            const bool enabled = option.control != DiscordUsername || state.rpc.show_profile_picture;
            ShowWindow(controls[option.control], discord_visibility);
            EnableWindow(controls[option.control], enabled);
            SendMessageW(controls[option.control], BM_SETCHECK,
                enabled && state.rpc.*(option.setting) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        ShowWindow(controls[DiscordRefresh], discord_visibility);
        ShowWindow(controls[DiscordHint], discord_visibility);
        for (const auto id : {DiscordProfileHeading, DiscordGamesHeading, DiscordZeldaGames}) {
            ShowWindow(controls[id], discord_visibility);
        }
        EnableWindow(controls[DiscordRefresh], state.discord && state.signed_in);
        EnableWindow(controls[CopyDiagnostics], !state.diagnostics.empty());
        if (resize) fit_height();
        if (relayout || resize) layout(scroll);
    }

    void feedback(const wchar_t* message) {
        text(Feedback, message);
        SetTimer(window, kFeedbackTimer, 3500, nullptr);
    }

    void command(Control id) {
        for (const auto& option : kDiscordOptions) {
            if (id != option.control) continue;
            const auto& toggle = callbacks->*(option.callback);
            if (state.discord_available && IsWindowEnabled(controls[id]) && toggle) toggle();
            return;
        }
        switch (id) {
            case OpenLibrary: library.show(window, LibraryPage::captures, *callbacks); break;
            case OpenHistory: library.show(window, LibraryPage::history, *callbacks); break;
            case OpenStorage: library.show(window, LibraryPage::storage, *callbacks); break;
            case OpenDuplicates: library.show(window, LibraryPage::duplicates, *callbacks); break;
            case ImportCaptures: if (callbacks->import_captures) callbacks->import_captures(); break;
            case AccountAction:
                if (state.sign_in_pending) callbacks->cancel_sign_in();
                else callbacks->sign_in_out();
                break;
            case PasteSignIn:
                if (state.manual_sign_in && callbacks->paste_sign_in_link) callbacks->paste_sign_in_link();
                break;
            case ClearCredential: callbacks->clear_legacy_credential(); break;
            case ChooseFolder: callbacks->select_folder(); break;
            case OpenFolder: callbacks->open_folder(); break;
            case SyncNow: callbacks->sync_now(); break;
            case RetryFailed: callbacks->retry_failed(); break;
            case AutoSync: callbacks->toggle_auto(); break;
            case DiscordToggle: callbacks->toggle_discord(); break;
            case DiscordRefresh:
                if (state.discord_available && state.discord && state.signed_in && callbacks->refresh_rpc) {
                    callbacks->refresh_rpc();
                    feedback(L"Refresh requested");
                }
                break;
            case CopyLatest:
                if (!shown_captures.empty()) {
                    feedback(copy_capture(shown_captures.front()) ? L"Latest capture copied" :
                                                                  L"Could not copy capture");
                }
                break;
            case ShowInFolder:
                if (!reveal_capture(selected_capture())) feedback(L"Capture could not be found");
                break;
            case CopyDiagnostics:
                feedback(clipboard_text(window, wide(state.diagnostics)) ? L"Diagnostics copied" :
                                                                          L"Could not copy diagnostics");
                break;
            default: break;
        }
    }

    void show() const {
        if (!IsWindowEnabled(window)) {
            SetForegroundWindow(GetLastActivePopup(window));
            return;
        }
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
        if (!GetFocus() || !IsChild(window, GetFocus())) {
            SetFocus(controls[state.manual_sign_in ? PasteSignIn :
                state.signed_in && !state.sync_busy ? SyncNow : AccountAction]);
        }
    }

    void close() const {
        if (hide_on_close) ShowWindow(window, SW_HIDE);
        else callbacks->exit();
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                dpi = GetDpiForWindow(window);
                create_controls();
                return 0;
            case WM_SIZE:
                if (wp != SIZE_MINIMIZED) layout(scroll);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                RECT frame{0, 0, px(kMinimumWidth), content_height()};
                AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW,
                    FALSE, WS_EX_CONTROLPARENT, dpi);
                limits->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
                const auto work_height = monitor.rcWork.bottom - monitor.rcWork.top;
                if (work_height > 0) limits->ptMinTrackSize.y = std::min(limits->ptMinTrackSize.y, work_height);
                return 0;
            }
            case WM_DPICHANGED: {
                dpi = HIWORD(wp);
                auto* frame = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(window, nullptr, frame->left, frame->top,
                    frame->right - frame->left, frame->bottom - frame->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                set_fonts();
                refresh_captures(true);
                layout(scroll);
                return 0;
            }
            case WM_VSCROLL: {
                int position = scroll;
                RECT client{};
                GetClientRect(window, &client);
                switch (LOWORD(wp)) {
                    case SB_LINEUP: position -= px(30); break;
                    case SB_LINEDOWN: position += px(30); break;
                    case SB_PAGEUP: position -= client.bottom; break;
                    case SB_PAGEDOWN: position += client.bottom; break;
                    case SB_THUMBTRACK: {
                        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
                        GetScrollInfo(window, SB_VERT, &info);
                        position = info.nTrackPos;
                        break;
                    }
                    default: break;
                }
                layout(position);
                return 0;
            }
            case WM_MOUSEWHEEL:
                layout(scroll - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), px(72), WHEEL_DELTA));
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == IDCANCEL) {
                    close();
                    return 0;
                }
                if (HIWORD(wp) == BN_CLICKED) command(static_cast<Control>(LOWORD(wp) - 2000));
                return 0;
            case WM_NOTIFY: {
                const auto* notice = reinterpret_cast<NMHDR*>(lp);
                if (notice->hwndFrom == controls[Captures]) {
                    if (notice->code == LVN_ITEMCHANGED) {
                        EnableWindow(controls[ShowInFolder], !selected_capture().empty());
                    }
                    if (notice->code == LVN_GETINFOTIPW) {
                        auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
                        if (tip->iItem >= 0 && static_cast<std::size_t>(tip->iItem) < shown_captures.size() &&
                            tip->cchTextMax > 0) {
                            const auto filename = shown_captures[static_cast<std::size_t>(tip->iItem)].filename();
                            wcsncpy_s(tip->pszText, tip->cchTextMax, filename.c_str(), _TRUNCATE);
                        }
                    }
                    if (notice->code == NM_DBLCLK || notice->code == NM_RETURN) {
                        if (!selected_capture().empty()) command(ShowInFolder);
                    }
                }
                return 0;
            }
            case WM_ERASEBKGND: {
                RECT bounds{};
                GetClientRect(window, &bounds);
                const auto dc = reinterpret_cast<HDC>(wp);
                if (high_contrast_enabled()) {
                    FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));
                } else {
                    FillRect(dc, &bounds, dashboard_paper);
                    const auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
                    const auto brush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
                    const int right = bounds.right - px(12);
                    const auto card = [&](int top, int bottom) {
                        RoundRect(dc, px(12), px(top) - scroll, right, px(bottom) - scroll, px(17), px(17));
                    };
                    card(135, 235);
                    card(242, 451);
                    card(458, 609);
                    card(616, 675 + (discord_expanded() ? kDiscordSettingsHeight : 0));
                    SelectObject(dc, brush);
                    SelectObject(dc, pen);
                    bounds.bottom = px(84) - scroll;
                    FillRect(dc, &bounds, dashboard_red);
                    bounds.top = bounds.bottom - px(4);
                    FillRect(dc, &bounds, dashboard_deep_red);
                }
                return TRUE;
            }
            case WM_DRAWITEM: {
                const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
                if (item.hwndItem != controls[OpenLibrary] && item.hwndItem != controls[OpenHistory] &&
                    item.hwndItem != controls[OpenStorage] && item.hwndItem != controls[ImportCaptures] &&
                    item.hwndItem != controls[OpenDuplicates]) {
                    return DefWindowProcW(window, msg, wp, lp);
                }
                auto bounds = item.rcItem;
                const bool primary = item.hwndItem == controls[OpenLibrary];
                const bool contrast = high_contrast_enabled();
                const bool pressed = (item.itemState & ODS_SELECTED) != 0;
                FillRect(item.hDC, &bounds, contrast ? GetSysColorBrush(COLOR_BTNFACE) : dashboard_paper);
                const HBRUSH fill = contrast ? GetSysColorBrush(pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE) :
                    (primary || pressed ? dashboard_red : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                if (imaging && !contrast) {
                    const auto color = primary || pressed ? RGB(196, 9, 43) : RGB(255, 255, 255);
                    Gdiplus::Graphics graphics(item.hDC);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
                    Gdiplus::GraphicsPath shape;
                    const int diameter = px(14);
                    shape.AddArc(bounds.left, bounds.top, diameter, diameter, 180, 90);
                    shape.AddArc(bounds.right - diameter, bounds.top, diameter, diameter, 270, 90);
                    shape.AddArc(bounds.right - diameter, bounds.bottom - diameter, diameter, diameter, 0, 90);
                    shape.AddArc(bounds.left, bounds.bottom - diameter, diameter, diameter, 90, 90);
                    shape.CloseFigure();
                    graphics.FillPath(&brush, &shape);
                } else {
                    FillRect(item.hDC, &bounds, fill);
                    FrameRect(item.hDC, &bounds, GetSysColorBrush(COLOR_BTNTEXT));
                }
                SetBkMode(item.hDC, TRANSPARENT);
                SetTextColor(item.hDC, (item.itemState & ODS_DISABLED) ? GetSysColor(COLOR_GRAYTEXT) :
                    contrast ? GetSysColor(pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) :
                    primary || pressed ? RGB(255, 255, 255) : RGB(34, 25, 31));
                wchar_t label[80]{};
                GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
                DrawTextW(item.hDC, label, -1, &bounds, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                if (item.itemState & ODS_FOCUS) {
                    InflateRect(&bounds, -px(4), -px(4));
                    DrawFocusRect(item.hDC, &bounds);
                }
                return TRUE;
            }
            case WM_CTLCOLORSTATIC: {
                auto dc = reinterpret_cast<HDC>(wp);
                SetBkMode(dc, TRANSPARENT);
                if (high_contrast_enabled()) {
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                }
                const auto child = reinterpret_cast<HWND>(lp);
                const bool banner = child == controls[Title] || child == controls[Subtitle];
                SetTextColor(dc, banner ? RGB(255, 255, 255) : RGB(34, 25, 31));
                return reinterpret_cast<LRESULT>(banner ? dashboard_red : GetStockObject(WHITE_BRUSH));
            }
            case WM_CLOSE:
                close();
                return 0;
            case WM_TIMER:
                if (wp == kFeedbackTimer) {
                    KillTimer(window, kFeedbackTimer);
                    text(Feedback, L"");
                }
                return 0;
            case kShowDashboard:
                show();
                return 0;
            case kThumbnailsReady: {
                auto batch = thumbnails.take();
                if (batch.generation == thumbnail_generation && images) {
                    for (std::size_t i = 0; i < std::min(batch.images.size(), shown_captures.size()); ++i) {
                        if (batch.images[i]) ImageList_Replace(images, static_cast<int>(i),
                                                             batch.images[i].get(), nullptr);
                    }
                    tray_images = std::move(batch.images);
                    InvalidateRect(controls[Captures], nullptr, FALSE);
                }
                return 0;
            }
            default:
                return DefWindowProcW(window, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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

WindowsDashboard::WindowsDashboard() : impl_(std::make_unique<Impl>()) {}
WindowsDashboard::~WindowsDashboard() = default;

bool WindowsDashboard::create(HICON icon, const PlatformCallbacks& callbacks,
    std::function<bool(const std::filesystem::path&)> copy_capture, bool hide_on_close) {
    impl_->callbacks = &callbacks;
    impl_->copy_capture = std::move(copy_capture);
    impl_->hide_on_close = hide_on_close;
    const INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = Impl::procedure;
    type.hInstance = instance;
    type.hIcon = icon;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    type.lpszClassName = kDashboardClass;
    RegisterClassW(&type);

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
    constexpr DWORD ex_style = WS_EX_CONTROLPARENT;
    HWND window = CreateWindowExW(ex_style, kDashboardClass, kTitle, style,
        work.left, work.top, 0, 0, nullptr, nullptr, instance, impl_.get());
    if (!window) return false;
    RECT bounds{0, 0, impl_->px(kWidth), impl_->px(kHeight)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, ex_style, impl_->dpi);
    const int width = std::min(bounds.right - bounds.left, work.right - work.left);
    const int height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
                 work.top + (work.bottom - work.top - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void WindowsDashboard::show() {
    if (impl_->window) impl_->show();
}

void WindowsDashboard::show_arrivals(const std::vector<std::filesystem::path>& captures) {
    if (!impl_->window) return;
    impl_->show();
    impl_->library.show_arrivals(impl_->window, *impl_->callbacks, captures);
}

void WindowsDashboard::copy_latest() {
    impl_->command(CopyLatest);
}

bool WindowsDashboard::copy_capture(const std::filesystem::path& path) {
    return impl_->copy_capture && impl_->copy_capture(path);
}

HBITMAP WindowsDashboard::recent_menu_thumbnail(const std::filesystem::path& path) const {
    const auto found = std::find(impl_->thumbnail_paths.begin(), impl_->thumbnail_paths.end(), path);
    if (found == impl_->thumbnail_paths.end()) return nullptr;
    const auto index = static_cast<std::size_t>(found - impl_->thumbnail_paths.begin());
    if (index >= impl_->tray_images.size() || !impl_->tray_images[index]) return nullptr;
    const int width = impl_->px(48), height = impl_->px(32);
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = width;
    format.bmiHeader.biHeight = -height;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    Bitmap bitmap(CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &pixels, nullptr, 0));
    HDC input = CreateCompatibleDC(nullptr), output = CreateCompatibleDC(nullptr);
    bool drawn = false;
    BITMAP source{};
    if (bitmap && pixels && input && output && GetObjectW(impl_->tray_images[index].get(), sizeof(source), &source)) {
        const auto old_input = SelectObject(input, impl_->tray_images[index].get());
        const auto old_output = SelectObject(output, bitmap.get());
        SetStretchBltMode(output, HALFTONE);
        drawn = StretchBlt(output, 0, 0, width, height, input, 0, 0,
                           source.bmWidth, source.bmHeight, SRCCOPY) != FALSE;
        SelectObject(input, old_input);
        SelectObject(output, old_output);
        GdiFlush();
        // Native menus composite 32-bit menu bitmaps using per-pixel alpha.
        auto* color = static_cast<DWORD*>(pixels);
        for (int i = 0; i < width * height; ++i) color[i] |= 0xff000000;
    }
    if (input) DeleteDC(input);
    if (output) DeleteDC(output);
    return drawn ? bitmap.release() : nullptr;
}

void WindowsDashboard::update(const MenuState& state) {
    impl_->update(state);
}

HWND WindowsDashboard::visible_window() const {
    return IsWindowVisible(impl_->window) ? impl_->window : nullptr;
}

bool WindowsDashboard::route_key(MSG& message) const {
    if (impl_->library.route_key(message)) return true;
    const HWND window = visible_window();
    if (!window || (message.hwnd != window && !IsChild(window, message.hwnd)) ||
        !IsDialogMessageW(window, &message)) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
        RECT child{};
        GetWindowRect(GetFocus(), &child);
        POINT top{child.left, child.top};
        ScreenToClient(window, &top);
        RECT client{};
        GetClientRect(window, &client);
        const int bottom = top.y + child.bottom - child.top;
        if (top.y < 0) impl_->layout(impl_->scroll + top.y - impl_->px(12));
        if (bottom > client.bottom) {
            impl_->layout(impl_->scroll + bottom - client.bottom + impl_->px(12));
        }
    }
    return true;
}

bool show_running_dashboard() {
    const HWND window = FindWindowW(kDashboardClass, kTitle);
    if (!window) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process) AllowSetForegroundWindow(process);
    return PostMessageW(window, kShowDashboard, 0, 0) != FALSE;
}

}  // namespace nso
