#include "dashboard_windows.hpp"
#include "album_tools_windows.hpp"
#include "library_windows.hpp"
#include "theme_windows.hpp"
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

constexpr wchar_t kDashboardClass[] = L"SwitchAlbumSyncDashboard";
constexpr wchar_t kTitle[] = L"Switch Album Sync";
constexpr UINT kShowDashboard = WM_APP + 60;
constexpr UINT kThumbnailsReady = WM_APP + 61;
constexpr UINT_PTR kFeedbackTimer = 1;
constexpr int kWidth = 704;
constexpr int kMinimumWidth = 610;
constexpr int kBannerHeight = 82;
constexpr int kCardPadding = 12;
constexpr int kCardGap = 8;
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
    Title, Subtitle, AccountHeading, AccountStatus, AccountAction,
    CredentialNote, ClearCredential, AlbumHeading,
    ChooseFolder, OpenFolder, LastSync, SyncStatus,
    Progress, SyncNow, RetryFailed, AutoSync, CapturesHeading,
    CopyLatest, ShowInFolder, Captures, EmptyCaptures, AlbumActions, CaptureActions, DiscordSettings,
    DiscordHeading, DiscordToggle, DiscordUnavailable, CopyDiagnostics,
    Feedback, PasteSignIn, OpenLibrary, OpenHistory, OpenStorage, ImportCaptures, OpenDuplicates,
    DiscordUsername, DiscordPicture, DiscordPlayTime, DiscordZelda, DiscordAnimalCrossing,
    DiscordSplatoon3, DiscordSplatoon2, DiscordRefresh, DiscordHint,
    DiscordProfileHeading, DiscordGamesHeading, DiscordZeldaGames, ControlCount,
    ChooseWatchFolder, StopWatchingFolder, FollowWindows, LightMode, DarkMode,
    FindDuplicates, StorageCleanup, Highlights, ChooseBackup, BackupNow, StopBackup,
    AutoSyncOptions, ExportSettings, ImportSettings,
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

Bitmap thumbnail_tile(SIZE size, HBITMAP source = nullptr, bool video = false) {
    HDC screen = GetDC(nullptr);
    if (!screen) return {};
    Bitmap result(CreateCompatibleBitmap(screen, size.cx, size.cy));
    HDC tile = CreateCompatibleDC(screen);
    if (result && tile) {
        HGDIOBJ previous = SelectObject(tile, result.get());
        RECT bounds{0, 0, size.cx, size.cy};
        FillRect(tile, &bounds, theme::high_contrast() ? GetSysColorBrush(COLOR_WINDOW) : theme::brush(theme::palette().field));
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
            if (video) {
                // A small play badge marks videos, since tiles have no labels.
                Gdiplus::Graphics graphics(tile);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float d = static_cast<float>(size.cy) * 0.36f;
                const float x = 5.0f, y = static_cast<float>(size.cy) - d - 5.0f;
                Gdiplus::SolidBrush shade(Gdiplus::Color(170, 0, 0, 0));
                graphics.FillEllipse(&shade, x, y, d, d);
                const Gdiplus::PointF play[] = {{x + d * 0.38f, y + d * 0.28f},
                                                {x + d * 0.38f, y + d * 0.72f},
                                                {x + d * 0.74f, y + d * 0.50f}};
                Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                graphics.FillPolygon(&white, play, 3);
            }
        } else {
            SetBkMode(tile, TRANSPARENT);
            SetTextColor(tile, theme::high_contrast() ? GetSysColor(COLOR_GRAYTEXT) : theme::palette().muted);
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
                const auto extension = path.extension().wstring();
                const bool video = CompareStringOrdinal(extension.c_str(), -1, L".mp4", -1, TRUE) == CSTR_EQUAL;
                batch.images.push_back(thumbnail_tile(request.size, image.get(), video));
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
    ~Impl() {
        thumbnails.stop();
        if (window) DestroyWindow(window);
        if (images) ImageList_Destroy(images);
        if (body) DeleteObject(body);
        if (heading) DeleteObject(heading);
        if (title) DeleteObject(title);
        if (imaging) Gdiplus::GdiplusShutdown(imaging);
    }

    HWND window = nullptr;
    HWND controls[ControlCount]{};
    const PlatformCallbacks* callbacks = nullptr;
    std::function<bool(const std::filesystem::path&)> copy_capture;
    MenuState state;
    UINT dpi = 96;
    int scroll = 0;
    bool discord_settings_open = false;
    bool discord_expanded() const { return state.discord_available && discord_settings_open; }
    int content_height() const { return px(arrange(kMinimumWidth - kMargin).height); }

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
        label(Subtitle, L"Sync your Nintendo Switch album to this PC.");
        button(OpenLibrary, L"Open library");
        button(OpenFolder, L"Open folder");
        button(ImportCaptures, L"Import USB / SD…");
        button(AlbumActions, L"More ▾");
        label(AlbumHeading, L"Album sync");
        label(LastSync, L"", SS_RIGHT);
        label(SyncStatus);
        control(Progress, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        button(SyncNow, L"Sync now", BS_DEFPUSHBUTTON);
        button(RetryFailed, L"Retry failed");
        button(AutoSync, L"Auto-sync", BS_AUTOCHECKBOX);
        label(CapturesHeading, L"Recent captures");
        button(CaptureActions, L"Capture actions ▾");
        control(Captures, WC_LISTVIEWW, L"Recent captures",
                WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE |
                LVS_SHAREIMAGELISTS | LVS_NOLABELWRAP | LVS_NOSCROLL);
        ListView_SetExtendedListViewStyle(controls[Captures], LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        label(EmptyCaptures);
        label(AccountHeading, L"Nintendo Account");
        label(AccountStatus);
        button(AccountAction, L"Sign in…");
        button(PasteSignIn, L"Paste sign-in link…");
        label(CredentialNote);
        button(ClearCredential, L"Clear saved credential");
        label(DiscordHeading, L"Discord Rich Presence");
        button(DiscordToggle, L"Show what I’m playing", BS_AUTOCHECKBOX);
        button(DiscordSettings, L"Settings ▾");
        for (const auto& option : kDiscordOptions) button(option.control, option.label, BS_AUTOCHECKBOX);
        button(DiscordRefresh, L"Refresh presence");
        label(DiscordHint, L"Discord desktop must be running. Visibility follows Discord Activity Sharing.");
        label(DiscordProfileHeading, L"Profile and play time");
        label(DiscordGamesHeading, L"Game details");
        label(DiscordZeldaGames, L"Zelda Notes live location works with:\n"
            L"•  Ocarina of Time on Nintendo Switch 2\n"
            L"•  Breath of the Wild and Tears of the Kingdom, Nintendo Switch 2 Editions");
        label(DiscordUnavailable, L"Unavailable in this build.");
        label(Feedback, L"", SS_RIGHT);
        set_fonts();
        theme::apply(window);
        theme::set_primary(controls[SyncNow]);
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
        title = font(24, FW_SEMIBOLD);
        for (auto child : controls) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
        for (auto id : {AccountHeading, AlbumHeading, CapturesHeading, DiscordHeading,
                        DiscordProfileHeading, DiscordGamesHeading}) {
            SendMessageW(controls[id], WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
        }
        SendMessageW(controls[Title], WM_SETFONT, reinterpret_cast<WPARAM>(title), TRUE);
        if (old_body) DeleteObject(old_body);
        if (old_heading) DeleteObject(old_heading);
        if (old_title) DeleteObject(old_title);
    }

    struct Placement {
        Control id;
        RECT bounds;  // 96 DPI units, before scrolling
    };
    struct Arrangement {
        std::vector<Placement> items;
        std::vector<std::pair<int, int>> cards;  // top and bottom of each section panel
        std::vector<int> dividers;  // hairlines between rows that share a card
        int height = 0;
    };
    std::vector<std::pair<int, int>> cards;
    std::vector<int> dividers;

    // "21:56 (2026-11-12)" reads as "21:56" when it is today.
    static std::wstring short_time(const std::string& value) {
        auto text = wide(value);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t today[16]{};
        swprintf_s(today, L"(%04u-%02u-%02u)", now.wYear, now.wMonth, now.wDay);
        if (text.size() == 18 && text.compare(6, 12, today) == 0) text.resize(5);
        return text;
    }

    // One muted line beside the auto-sync toggle: when it last ran and, while
    // auto-sync is on, when it runs next.
    std::wstring schedule_text() const {
        auto result = L"Last sync " + short_time(state.last_sync);
        if (!state.auto_sync || !state.signed_in) return result;
        const auto next = short_time(state.next_sync);
        const bool time = !next.empty() && iswdigit(next.front());
        return result + L"  ·  " + (time ? L"Next " + next : next);
    }

    // The status with any counts from the last run appended, e.g.
    // "Album is up to date  ·  17 new".
    std::wstring status_text() const {
        auto result = wide(state.status);
        const auto detail = wide(state.sync_detail);
        if (!detail.empty()) result += (result.empty() ? L"" : L"  ·  ") + detail;
        return result;
    }

    // Height in 96 DPI units for the status line, which wraps when long.
    int status_height(int width) const {
        const auto value = status_text();
        if (!window || value.empty()) return 20;
        HDC dc = GetDC(window);
        if (!dc) return 20;
        const auto old = SelectObject(dc, body);
        RECT bounds{0, 0, px(width), 0};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        return std::max(20, MulDiv(bounds.bottom - bounds.top, 96, static_cast<int>(dpi)) + 2);
    }

    // Builds the dashboard from top to bottom. Each section is a card and each
    // row advances the cursor, so resizing a row moves everything below it.
    Arrangement arrange(int right) const {
        Arrangement result;
        const int left = kMargin, span = right - left, half = span / 2;
        const auto put = [&](Control id, int x, int top, int width, int height) {
            result.items.push_back({id, RECT{x, top, x + width, top + height}});
        };
        // Places a control whose right edge sits `inset` units from the margin.
        const auto put_right = [&](Control id, int inset, int top, int width, int height) {
            put(id, right - inset, top, width, height);
        };
        int y = 18;
        put(Title, left, y, span, 34);
        put(Subtitle, left, y + 36, span, 22);

        y = kBannerHeight + 10;
        put(OpenLibrary, left, y, 112, 32);
        put(OpenFolder, left + 120, y, 108, 32);
        put(ImportCaptures, left + 236, y, 140, 32);
        put_right(AlbumActions, 88, y, 88, 32);
        y += 44;

        int card_top = 0;
        const auto open_card = [&] {
            card_top = y;
            y += kCardPadding;
        };
        const auto close_card = [&] {
            y += kCardPadding;
            result.cards.push_back({card_top, y});
            y += kCardGap;
        };

        open_card();
        put(AlbumHeading, left, y + 5, span - 222, 22);
        put_right(SyncNow, 106, y, 106, 32);
        put_right(RetryFailed, 218, y, 104, 32);
        y += 40;
        const int status = status_height(span);
        put(SyncStatus, left, y, span, status);
        y += status + 6;
        if (state.sync_busy) {
            put(Progress, left, y, span, 6);
            y += 14;
        }
        put(AutoSync, left, y, 184, 27);
        put(LastSync, left + 192, y + 4, span - 192, 21);
        y += 27;
        close_card();

        open_card();
        put(CapturesHeading, left, y + 3, span - 304, 21);
        put_right(CaptureActions, 145, y - 3, 145, 28);
        put_right(Feedback, 303, y + 2, 150, 24);
        y += 32;
        put(Captures, left, y, span, kThumbnailHeight + 6);
        put(EmptyCaptures, left, y + 6, span, 42);
        y += state.recent_captures.empty() ? 45 : kThumbnailHeight + 6;
        close_card();

        // Nintendo Account and Discord share one card: a row each, with the
        // longer explanations only while something needs attention.
        open_card();
        constexpr int kLabelWidth = 176;
        const bool status_below = state.manual_sign_in || state.sign_in_pending;
        put(AccountHeading, left, y + 5, kLabelWidth, 22);
        put_right(AccountAction, 118, y, 118, 32);
        put_right(PasteSignIn, 280, y, 153, 32);
        if (!status_below) put(AccountStatus, left + kLabelWidth, y + 6, span - kLabelWidth - 126, 21);
        y += 32;
        if (status_below) {
            put(AccountStatus, left, y + 4, span, 21);
            y += 26;
        }
        if (!state.signed_in || state.credential_warning) {
            put(CredentialNote, left, y + 4, span - (state.legacy_credential ? 177 : 0), 36);
            put_right(ClearCredential, 166, y + 4, 166, 28);
            y += 40;
        }
        result.dividers.push_back(y + 8);
        y += 16;
        put(DiscordHeading, left, y + 5, kLabelWidth, 22);
        put(DiscordToggle, left + kLabelWidth, y + 3, span - kLabelWidth - 126, 26);
        put(DiscordUnavailable, left + kLabelWidth, y + 6, span - kLabelWidth - 126, 21);
        put_right(DiscordSettings, 112, y, 112, 32);
        y += 32;
        if (discord_expanded()) {
            y += 5;
            put(DiscordProfileHeading, left, y, half - 8, 21);
            put(DiscordGamesHeading, left + half, y, half - 8, 21);
            y += 23;
            int column_bottom[2] = {y, y};
            for (std::size_t index = 0; index < std::size(kDiscordOptions); ++index) {
                const int column = index < 3 ? 0 : 1;
                put(kDiscordOptions[index].control, left + column * half, column_bottom[column], half - 8, 22);
                column_bottom[column] += 22;
            }
            put(DiscordRefresh, left, column_bottom[0], 142, 26);
            column_bottom[0] += 26;
            y = std::max(column_bottom[0], column_bottom[1]) + 8;
            put(DiscordZeldaGames, left, y, span, 54);
            y += 58;
            put(DiscordHint, left, y, span, 20);
            y += 20;
        }
        close_card();
        result.height = y - kCardGap + 12;
        return result;
    }

    // Right edge of the content in 96 DPI units for the current window width.
    int content_right() const {
        RECT client{};
        GetClientRect(window, &client);
        return std::max(kMinimumWidth - kMargin, MulDiv(client.right, 96, static_cast<int>(dpi)) - kMargin);
    }

    void layout(int desired_scroll, bool scrolling = false) {
        RECT client{};
        GetClientRect(window, &client);
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = content_height() - 1;
        info.nPage = static_cast<UINT>(std::max(0L, client.bottom));
        const int maximum = std::max(0, info.nMax - std::max(0, static_cast<int>(info.nPage) - 1));
        if (scrolling && std::clamp(desired_scroll, 0, maximum) == scroll) return;
        info.nPos = desired_scroll;
        SetScrollInfo(window, SB_VERT, &info, TRUE);
        scroll = GetScrollPos(window, SB_VERT);
        const auto plan = arrange(content_right());
        cards = plan.cards;
        dividers = plan.dividers;
        const auto place = [&](const Placement& item, HDWP batch) {
            const auto& r = item.bounds;
            constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
            const int x = px(r.left), y = px(r.top) - scroll;
            const int width = px(r.right - r.left), height = px(r.bottom - r.top);
            if (batch) return DeferWindowPos(batch, controls[item.id], nullptr, x, y, width, height, flags);
            SetWindowPos(controls[item.id], nullptr, x, y, width, height, flags);
            return HDWP{};
        };
        HDWP batch = BeginDeferWindowPos(static_cast<int>(plan.items.size()));
        for (const auto& item : plan.items) {
            if (!batch) break;
            batch = place(item, batch);
        }
        if (!batch || !EndDeferWindowPos(batch)) {
            for (const auto& item : plan.items) place(item, nullptr);
        }
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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
        // Thumbnails carry no labels: the tip names the game and file, and
        // videos get a play badge on the tile.
        ListView_SetIconSpacing(controls[Captures], px(kThumbnailWidth + 12), px(kThumbnailHeight + 12));

        const auto blank = thumbnail_tile(size);
        int select_index = -1;
        for (std::size_t i = 0; i < count; ++i) {
            if (!selection.empty() && shown_captures[i] == selection) select_index = static_cast<int>(i);
            const int image = blank && images ? ImageList_Add(images, blank.get(), nullptr) : -1;
            LVITEMW item{};
            item.mask = LVIF_IMAGE;
            item.iItem = static_cast<int>(i);
            item.iImage = image;
            ListView_InsertItem(controls[Captures], &item);
        }
        // Keep a selection the user made; otherwise nothing starts selected.
        if (select_index >= 0) {
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
        const bool resize = state.discord_available != next.discord_available ||
            state.signed_in != next.signed_in || state.credential_warning != next.credential_warning ||
            state.recent_captures.empty() != next.recent_captures.empty();
        const int old_status_height = window ? status_height(content_right() - kMargin) : 0;
        bool relayout = state.manual_sign_in != next.manual_sign_in ||
            state.sign_in_pending != next.sign_in_pending ||
            state.sync_busy != next.sync_busy || state.legacy_credential != next.legacy_credential;
        state = next;
        // A status that wraps onto another line (or stops wrapping) moves the rows below it.
        if (window && status_height(content_right() - kMargin) != old_status_height) relayout = true;
        library.update(state);
        if (!window) return;

        text(AccountStatus, state.manual_sign_in ? L"Copy the link from Nintendo, then paste it here." :
            state.sign_in_pending ? L"Waiting for Nintendo sign-in in your browser…" :
            state.signed_in ? (state.nickname.empty() ? L"Connected" : L"Connected as " + wide(state.nickname)) :
                              L"Not signed in");
        text(AccountAction, state.sign_in_pending ? L"Cancel sign-in" :
            state.signed_in ? L"Sign out" : L"Sign in…");
        theme::set_primary(controls[AccountAction], !state.signed_in && !state.sign_in_pending);
        ShowWindow(controls[PasteSignIn], state.manual_sign_in ? SW_SHOWNA : SW_HIDE);
        text(CredentialNote, !state.credential_status.empty() ? wide(state.credential_status) :
            state.signed_in ? L"" :
            L"Sign-in uses nxapi, a third-party service that processes Nintendo tokens, "
            L"account details and Coral API traffic. Details appear before you continue.");
        ShowWindow(controls[ClearCredential], state.legacy_credential ? SW_SHOWNA : SW_HIDE);
        ShowWindow(controls[CredentialNote], !state.signed_in || state.credential_warning ? SW_SHOWNA : SW_HIDE);

        text(LastSync, schedule_text());
        text(SyncStatus, status_text());
        EnableWindow(controls[OpenFolder], !state.destination.empty());
        EnableWindow(controls[SyncNow], state.signed_in && !state.sync_busy);
        EnableWindow(controls[ImportCaptures], !state.sync_busy);
        ShowWindow(controls[RetryFailed], state.retry_available ? SW_SHOWNA : SW_HIDE);
        EnableWindow(controls[RetryFailed], state.signed_in && !state.sync_busy);
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
        EnableWindow(controls[CaptureActions], has_captures);

        ShowWindow(controls[DiscordToggle], state.discord_available ? SW_SHOWNA : SW_HIDE);
        ShowWindow(controls[DiscordSettings], state.discord_available ? SW_SHOWNA : SW_HIDE);
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
        if (resize) fit_height();
        if (relayout || resize) layout(scroll);
    }

    void feedback(const wchar_t* message) {
        text(Feedback, message);
        SetTimer(window, kFeedbackTimer, 3500, nullptr);
    }

    void show_actions(Control anchor) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        const auto add = [menu](Control id, const wchar_t* label, bool enabled = true) {
            AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), 2000 + id, label);
        };
        if (anchor == AlbumActions) {
            add(OpenHistory, L"Sync history");
            add(OpenStorage, L"Storage");
            add(OpenDuplicates, L"Duplicate review");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            add(ChooseFolder, L"Change album folder…");
            add(ChooseWatchFolder, state.watching_folder ? L"Change watched folder…" : L"Watch a folder…");
            if (state.watching_folder) add(StopWatchingFolder, L"Stop watching folder");
            const bool backing_up = !state.backup_folder.empty();
            add(ChooseBackup, backing_up ? L"Change backup folder…" : L"Back up to a folder…");
            if (backing_up) {
                add(BackupNow, L"Back up now", !state.sync_busy);
                add(StopBackup, L"Stop backing up");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            const bool has_captures = state.capture_library && !state.capture_library->empty();
            add(FindDuplicates, L"Find duplicates…", has_captures);
            add(StorageCleanup, L"Storage cleanup…", has_captures);
            add(Highlights, L"Highlights grid…", has_captures);
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            add(AutoSyncOptions, L"Auto-sync options…");
            if (HMENU appearance = CreatePopupMenu()) {
                const auto add_mode = [&](Control id, const wchar_t* label, Appearance mode) {
                    AppendMenuW(appearance, MF_STRING | (state.appearance == mode ? MF_CHECKED : MF_UNCHECKED),
                                2000 + id, label);
                };
                add_mode(FollowWindows, L"Follow Windows", Appearance::system);
                add_mode(LightMode, L"Light", Appearance::light);
                add_mode(DarkMode, L"Dark", Appearance::dark);
                // The menu owns the submenu, so DestroyMenu below frees both.
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), L"Appearance");
            }
            add(ExportSettings, L"Export settings…");
            add(ImportSettings, L"Import settings…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            add(CopyDiagnostics, L"Copy diagnostics", !state.diagnostics.empty());
        } else {
            add(CopyLatest, L"Copy latest capture", !shown_captures.empty());
            add(ShowInFolder, L"Show selected capture in folder", !selected_capture().empty());
        }
        RECT bounds{};
        GetWindowRect(controls[anchor], &bounds);
        const auto selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTALIGN,
            bounds.right, bounds.bottom, window, nullptr);
        DestroyMenu(menu);
        if (selected) command(static_cast<Control>(selected - 2000));
    }

    void command(Control id) {
        for (const auto& option : kDiscordOptions) {
            if (id != option.control) continue;
            const auto& toggle = callbacks->*(option.callback);
            if (state.discord_available && IsWindowEnabled(controls[id]) && toggle) toggle();
            return;
        }
        switch (id) {
            case AlbumActions:
            case CaptureActions: show_actions(id); break;
            case FollowWindows: case LightMode: case DarkMode: {
                const auto mode = id == LightMode ? Appearance::light :
                    id == DarkMode ? Appearance::dark : Appearance::system;
                if (mode != state.appearance && callbacks->set_appearance) callbacks->set_appearance(mode);
                break;
            }
            case DiscordSettings:
                discord_settings_open = !discord_settings_open;
                text(DiscordSettings, discord_settings_open ? L"Settings ▴" : L"Settings ▾");
                update(state);
                fit_height();
                layout(scroll);
                break;
            case OpenLibrary: library.show(window, LibraryPage::captures, *callbacks); break;
            case OpenHistory: library.show(window, LibraryPage::history, *callbacks); break;
            case OpenStorage: library.show(window, LibraryPage::storage, *callbacks); break;
            case OpenDuplicates: library.show(window, LibraryPage::duplicates, *callbacks); break;
            case ImportCaptures: if (callbacks->import_captures) callbacks->import_captures(); break;
            case ChooseWatchFolder: if (callbacks->choose_watch_folder) callbacks->choose_watch_folder(); break;
            case ChooseBackup: if (callbacks->choose_backup_folder) callbacks->choose_backup_folder(); break;
            case BackupNow: if (callbacks->backup_now) callbacks->backup_now(); break;
            case StopBackup: if (callbacks->stop_backup) callbacks->stop_backup(); break;
            case AutoSyncOptions: if (callbacks->auto_sync_options) callbacks->auto_sync_options(); break;
            case FindDuplicates: case StorageCleanup: case Highlights: case ExportSettings: case ImportSettings: {
                const auto tool = id == FindDuplicates ? AlbumTool::duplicates : id == StorageCleanup ? AlbumTool::cleanup :
                    id == Highlights ? AlbumTool::highlights : id == ExportSettings ? AlbumTool::export_settings :
                    AlbumTool::import_settings;
                const auto snapshot = state;  // Dialogs pump messages; keep a stable copy.
                if (run_album_tool(window, tool, snapshot, *callbacks)) {
                    library.reload_saved_data();
                    if (callbacks->refresh_captures) callbacks->refresh_captures();
                }
                break;
            }
            case StopWatchingFolder: if (callbacks->stop_watching_folder) callbacks->stop_watching_folder(); break;
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
                layout(position, true);
                return 0;
            }
            case WM_MOUSEWHEEL:
                layout(scroll - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), px(72), WHEEL_DELTA), true);
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
                    if (notice->code == LVN_GETINFOTIPW) {
                        auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
                        if (tip->iItem >= 0 && static_cast<std::size_t>(tip->iItem) < shown_captures.size() &&
                            tip->cchTextMax > 0) {
                            // Long game names are cut short under the thumbnail, so
                            // the tip spells out the folder as well as the file.
                            const auto& capture = shown_captures[static_cast<std::size_t>(tip->iItem)];
                            const auto folder = capture.parent_path();
                            const std::wstring text = folder.lexically_normal() == state.destination.lexically_normal()
                                ? capture.filename().wstring()
                                : folder.filename().wstring() + L"\n" + capture.filename().wstring();
                            wcsncpy_s(tip->pszText, tip->cchTextMax, text.c_str(), _TRUNCATE);
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
                    FillRect(dc, &bounds, theme::brush(theme::palette().background));
                    const int right = bounds.right - px(12);
                    const auto card = [&](int top, int bottom) {
                        theme::fill_round(dc, RECT{px(12), px(top) - scroll, right, px(bottom) - scroll},
                                          px(14), theme::palette().card, theme::palette().card_border);
                    };
                    for (const auto& [top, bottom] : cards) card(top, bottom);
                    for (const int divider : dividers) {
                        const RECT line{px(kMargin), px(divider) - scroll, right - px(kMargin - 12),
                                        px(divider) - scroll + std::max(1, px(1))};
                        FillRect(dc, &line, theme::brush(theme::palette().card_border));
                    }
                    // A small red mark beside the title carries the Nintendo accent.
                    theme::fill_round(dc, RECT{px(12), px(24) - scroll, px(16), px(48) - scroll},
                                      px(2), theme::palette().accent);
                }
                return TRUE;
            }
            case WM_CTLCOLORBTN: {
                if (high_contrast_enabled()) return DefWindowProcW(window, msg, wp, lp);
                const auto child = reinterpret_cast<HWND>(lp);
                const bool header = child == controls[OpenLibrary] || child == controls[ImportCaptures] ||
                    child == controls[AlbumActions] || child == controls[OpenFolder];
                return theme::control_color(msg, wp, lp, header ? theme::palette().background : theme::palette().card);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                if (const auto brush = theme::control_color(msg, wp, lp, theme::palette().card)) return brush;
                return DefWindowProcW(window, msg, wp, lp);
            case WM_CTLCOLORSTATIC: {
                auto dc = reinterpret_cast<HDC>(wp);
                SetBkMode(dc, TRANSPARENT);
                if (high_contrast_enabled()) {
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                }
                const auto child = reinterpret_cast<HWND>(lp);
                if (child == controls[Title] || child == controls[Subtitle]) {
                    SetTextColor(dc, child == controls[Title] ? theme::palette().text : theme::palette().muted);
                    return reinterpret_cast<LRESULT>(theme::brush(theme::palette().background));
                }
                const auto result = theme::control_color(msg, wp, lp, theme::palette().card);
                for (const auto id : {CredentialNote, LastSync, AccountStatus, EmptyCaptures,
                                      DiscordHint, DiscordZeldaGames, DiscordUnavailable, Feedback}) {
                    if (child == controls[id]) SetTextColor(dc, theme::palette().muted);
                }
                return result;
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
    type.lpszClassName = kDashboardClass;
    RegisterClassW(&type);

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN;
    constexpr DWORD ex_style = WS_EX_CONTROLPARENT;
    HWND window = CreateWindowExW(ex_style, kDashboardClass, kTitle, style,
        work.left, work.top, 0, 0, nullptr, nullptr, instance, impl_.get());
    if (!window) return false;
    RECT bounds{0, 0, impl_->px(kWidth), impl_->content_height()};
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
