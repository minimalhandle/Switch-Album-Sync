#include "nso_album_sync/platform.hpp"

#include "dashboard_windows.hpp"
#include "import_windows.hpp"
#include "library_windows.hpp"
#include "theme_windows.hpp"
#include "nso_album_sync/windows_compat.hpp"
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nso {
namespace {
constexpr UINT kTrayMessage = WM_APP + 42;
constexpr UINT kStateChanged = WM_APP + 43;
constexpr UINT kNotificationReady = WM_APP + 44;
constexpr UINT_PTR kNotificationTimer = 1;
constexpr int kIconId = 101;
constexpr UINT kRecentCommand = 2000;
constexpr std::size_t kRecentLimit = 8;
constexpr int kDisclosureSourceButton = 1101;
constexpr int kDisclosureHelpButton = 1102;
constexpr wchar_t kAppTitle[] = L"Switch Album Sync";
constexpr wchar_t kTrayClass[] = L"SwitchAlbumSyncTray";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Switch Album Sync";
constexpr wchar_t kStartInTray[] = L"--start-in-tray";
constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
// The nxapi terms ask clients that sign in with a user's account to link the
// project, and allow linking its end-user help.
constexpr char kNxapiSourceUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api";
constexpr char kNxapiHelpUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api/blob/docs/docs/end-user-help.md";

enum Command : UINT {
    CmdSync = 1001, CmdCopy, CmdAuto, CmdNotifications, CmdDiscord, CmdFolder,
    CmdOpen, CmdStartup, CmdProxy, CmdAccount, CmdExit,
    CmdRpcUsername,
    CmdRpcProfilePicture,
    CmdRpcPlayTime,
    CmdRpcZelda, CmdRpcAnimalCrossing, CmdRpcSplatoon3, CmdRpcSplatoon2, CmdRpcRefresh,
    CmdDashboard, CmdRetry, CmdClearCredential, CmdPasteSignIn, CmdCancelSignIn, CmdImportCaptures,
};

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

HICON app_icon() {
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconId));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

std::wstring executable_path_wide() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring auto_label(int minutes) {
    if (minutes == 60) return L"Auto-Sync (Hourly)";
    return L"Auto-Sync (Every " + std::to_wstring(std::max(1, minutes)) + L" min)";
}

bool start_in_tray_requested() {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool requested = false;
    for (int i = 1; i < count; ++i) {
        if (std::wstring_view(arguments[i]) == kStartInTray) {
            requested = true;
            break;
        }
    }
    LocalFree(arguments);
    return requested;
}

void write_startup_command(HKEY key, const std::wstring& quoted_executable) {
    const std::wstring value = quoted_executable + L" " + kStartInTray;
    RegSetValueExW(key, kRunValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::string prompt_box(
    const std::string& title,
    const std::string& text,
    const std::string& initial,
    HWND owner) {
    theme::Dialog dialog;
    dialog.title = wide(title);
    dialog.heading = wide(title);
    dialog.text = wide(text);
    dialog.field = true;
    dialog.field_value = wide(initial);
    dialog.buttons = {{IDOK, L"Save", true}, {IDCANCEL, L"Cancel"}};
    if (theme::run_dialog(owner, dialog) != IDOK) return initial;
    return utf8(dialog.field_value);
}

bool disclosure_box(const std::string& title, const std::string& text, HWND owner) {
    theme::Dialog dialog;
    dialog.title = wide(title);
    dialog.heading = wide(title);
    dialog.text = wide(text);
    dialog.width = 600;
    dialog.buttons = {{kDisclosureSourceButton, L"View source", false, false},
                      {kDisclosureHelpButton, L"nxapi help", false, false},
                      {IDOK, L"Continue", true}, {IDCANCEL, L"Cancel"}};
    dialog.on_command = [](int id) {
        const char* link = id == kDisclosureSourceButton ? kNxapiSourceUrl :
            id == kDisclosureHelpButton ? kNxapiHelpUrl : nullptr;
        if (!link) return;
        const auto url = wide(link);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    return theme::run_dialog(owner, dialog) == IDOK;
}
}  // namespace

struct PlatformUi::Impl {
    std::atomic<HWND> window{nullptr};
    NOTIFYICONDATAW tray{};
    PlatformCallbacks callbacks;
    MenuState state;
    std::mutex mutex;
    bool state_posted = false;
    bool tray_available = false;
    struct Notification {
        std::string title, message;
        std::vector<std::filesystem::path> captures;
    };
    std::deque<Notification> notifications;
    bool balloon_active = false;
    std::vector<std::filesystem::path> notification_captures;
    WindowsDashboard dashboard;
};

namespace {
PlatformUi::Impl* g_ui = nullptr;

void show_next_notification(PlatformUi::Impl* ui, HWND window) {
    std::lock_guard lock(ui->mutex);
    if (ui->balloon_active || !ui->tray_available) return;
    while (!ui->notifications.empty()) {
        auto next = std::move(ui->notifications.front());
        ui->notifications.pop_front();
        auto balloon = ui->tray;
        balloon.uFlags = NIF_INFO;
        wcsncpy_s(balloon.szInfo, wide(next.message).c_str(), _TRUNCATE);
        wcsncpy_s(balloon.szInfoTitle, wide(next.title).c_str(), _TRUNCATE);
        balloon.dwInfoFlags = NIIF_INFO;
        if (!Shell_NotifyIconW(NIM_MODIFY, &balloon)) continue;
        ui->notification_captures = std::move(next.captures);
        ui->balloon_active = true;
        SetTimer(window, kNotificationTimer, 60000, nullptr);
        break;
    }
}

void invoke(PlatformUi::Impl* ui, UINT command) {
    if (!ui) return;
    const auto& c = ui->callbacks;
    switch (command) {
        case CmdDashboard: ui->dashboard.show(); break;
        case CmdImportCaptures: if (c.import_captures) c.import_captures(); break;
        case CmdSync: c.sync_now(); break;
        case CmdRetry: if (c.retry_failed) c.retry_failed(); break;
        case CmdCopy: ui->dashboard.copy_latest(); break;
        case CmdAuto: c.toggle_auto(); break;
        case CmdNotifications: c.toggle_notifications(); break;
        case CmdDiscord: c.toggle_discord(); break;
        case CmdRpcUsername: c.toggle_rpc_username(); break;
        case CmdRpcProfilePicture: c.toggle_rpc_profile_picture(); break;
        case CmdRpcPlayTime: c.toggle_rpc_play_time(); break;
        case CmdRpcZelda: c.toggle_rpc_zelda(); break;
        case CmdRpcAnimalCrossing: c.toggle_rpc_animal_crossing(); break;
        case CmdRpcSplatoon3: c.toggle_rpc_splatoon3(); break;
        case CmdRpcSplatoon2: c.toggle_rpc_splatoon2(); break;
        case CmdRpcRefresh: c.refresh_rpc(); break;
        case CmdFolder: c.select_folder(); break;
        case CmdOpen: c.open_folder(); break;
        case CmdStartup: c.toggle_start(); break;
        case CmdProxy: c.proxy(); break;
        case CmdAccount: c.sign_in_out(); break;
        case CmdPasteSignIn: if (c.paste_sign_in_link) c.paste_sign_in_link(); break;
        case CmdCancelSignIn: if (c.cancel_sign_in) c.cancel_sign_in(); break;
        case CmdClearCredential: if (c.clear_legacy_credential) c.clear_legacy_credential(); break;
        case CmdExit: c.exit(); break;
        default: break;
    }
}

void update_dashboard(PlatformUi::Impl* ui) {
    if (!ui) return;
    MenuState state;
    {
        std::lock_guard lock(ui->mutex);
        ui->state_posted = false;
        state = ui->state;
    }
    theme::set_appearance(state.appearance);
    ui->dashboard.update(state);
}

void tray_menu(PlatformUi::Impl* ui) {
    if (!ui) return;
    MenuState s;
    { std::lock_guard lock(ui->mutex); s = ui->state; }

    HMENU menu = CreatePopupMenu();
    const auto add = [&](UINT id, const std::wstring& label, UINT flags = MF_STRING) {
        AppendMenuW(menu, flags, id, label.c_str());
    };

    add(CmdDashboard, L"Open Dashboard");
    SetMenuDefaultItem(menu, CmdDashboard, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(
        0,
        s.signed_in ? L"Connected as " + wide(s.nickname) : L"Not signed in",
        MF_GRAYED);
    add(0, L"Last sync: " + wide(s.last_sync), MF_GRAYED);
    if (s.sync_busy) add(0, wide(s.status), MF_GRAYED);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdSync, L"Sync Now", s.signed_in && !s.sync_busy ? MF_STRING : MF_GRAYED);
    add(CmdImportCaptures, L"Import USB / SD…", s.sync_busy ? MF_GRAYED : MF_STRING);
    if (s.retry_available) {
        add(CmdRetry, L"Retry Failed Captures", s.signed_in && !s.sync_busy ? MF_STRING : MF_GRAYED);
    }
    add(
        CmdAuto,
        auto_label(s.sync_interval_minutes),
        MF_STRING | (s.auto_sync ? MF_CHECKED : 0));
    add(CmdCopy, L"Copy Last Capture", s.recent_captures.empty() ? MF_GRAYED : MF_STRING);
    const auto recent_count = std::min(kRecentLimit, s.recent_captures.size());
    std::vector<HBITMAP> menu_thumbnails;
    if (recent_count) {
        const auto recent = CreatePopupMenu();
        for (std::size_t i = 0; i < recent_count; ++i) {
            const auto& path = s.recent_captures[i];
            const bool video = _wcsicmp(path.extension().c_str(), L".mp4") == 0;
            auto description = (path.parent_path() == s.destination ? L"Album" : path.parent_path().filename().wstring()) +
                (video ? L"  ·  Video" : L"  ·  Screenshot");
            if (s.capture_library && i < s.capture_library->size() && (*s.capture_library)[i].path == path) {
                const auto modified = static_cast<std::time_t>((*s.capture_library)[i].modified);
                std::tm local{};
                wchar_t date[48]{};
                if (localtime_s(&local, &modified) == 0 &&
                    std::wcsftime(date, std::size(date), L"%d %b, %H:%M:%S", &local)) {
                    description += L"  ·  ";
                    description += date;
                }
            } else {
                description += L"  ·  Recent #" + std::to_wstring(i + 1);
            }
            std::wstring label;
            for (const auto ch : description) {
                label.push_back(ch);
                if (ch == L'&') label.push_back(ch);
            }
            label += L"  ·  Copy as file";
            AppendMenuW(recent, MF_STRING, kRecentCommand + static_cast<UINT>(i), label.c_str());
            if (const auto preview = ui->dashboard.recent_menu_thumbnail(path)) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_BITMAP;
                item.hbmpItem = preview;
                SetMenuItemInfoW(recent, static_cast<UINT>(i), TRUE, &item);
                menu_thumbnails.push_back(preview);
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), L"Recent captures");
    } else {
        add(0, L"Recent captures", MF_GRAYED);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdNotifications, L"Notifications",
        MF_STRING | (s.notifications ? MF_CHECKED : 0));
    if (s.discord_available) {
        HMENU rpc = CreatePopupMenu();
        const auto rpc_check = [&](UINT id, const wchar_t* label, bool checked, bool enabled = true) {
            AppendMenuW(rpc, MF_STRING | (checked ? MF_CHECKED : 0) |
                (enabled ? 0 : MF_GRAYED), id, label);
        };
        rpc_check(CmdDiscord, L"Enabled", s.discord);
        AppendMenuW(rpc, MF_SEPARATOR, 0, nullptr);
        rpc_check(CmdRpcUsername, L"Show Switch username",
            s.rpc.show_profile_picture && s.rpc.show_username, s.rpc.show_profile_picture);
        rpc_check(CmdRpcProfilePicture, L"Show profile picture", s.rpc.show_profile_picture);
        rpc_check(CmdRpcPlayTime, L"Show total play time", s.rpc.show_play_time);
        AppendMenuW(rpc, MF_SEPARATOR, 0, nullptr);
        rpc_check(CmdRpcZelda, L"Zelda Notes live location", s.rpc.zelda);
        rpc_check(CmdRpcAnimalCrossing, L"Animal Crossing details", s.rpc.animal_crossing);
        rpc_check(CmdRpcSplatoon3, L"Splatoon 3 details", s.rpc.splatoon3);
        rpc_check(CmdRpcSplatoon2, L"Splatoon 2 details", s.rpc.splatoon2);
        AppendMenuW(rpc, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(rpc, MF_STRING | (s.discord && s.signed_in ? 0 : MF_GRAYED),
            CmdRpcRefresh, L"Refresh Now");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rpc), L"Discord Rich Presence");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdFolder, L"Choose Album Folder…");
    add(CmdOpen, L"Open Album Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(
        CmdStartup,
        L"Start on Boot",
        MF_STRING | (s.start_on_boot ? MF_CHECKED : 0));
    add(CmdProxy, L"HTTP Proxy…");
    if (s.sign_in_pending) {
        if (s.manual_sign_in) add(CmdPasteSignIn, L"Paste Sign-In Link…");
        add(CmdCancelSignIn, L"Cancel Sign-In");
    } else {
        add(CmdAccount, s.signed_in ? L"Sign Out" : L"Sign In to Nintendo Account…");
    }
    if (s.legacy_credential) add(CmdClearCredential, L"Clear Saved Credential…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdExit, L"Exit");

    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(ui->window);
    const UINT selected = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY,
        p.x,
        p.y,
        0,
        ui->window,
        nullptr);
    DestroyMenu(menu);
    for (const auto preview : menu_thumbnails) DeleteObject(preview);
    if (selected >= kRecentCommand && selected < kRecentCommand + recent_count) {
        const auto& path = s.recent_captures[selected - kRecentCommand];
        const bool copied = ui->dashboard.copy_capture(path);
        if (!copied) theme::message_box(ui->dashboard.visible_window(), L"The file may have moved or be open in another app.",
                                        L"Could not copy this capture", MB_OK);
        return;
    }
    invoke(ui, selected);
}

LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayMessage) {
        const UINT event = LOWORD(lp);
        if (event == NIN_BALLOONUSERCLICK && g_ui) {
            std::vector<std::filesystem::path> captures;
            {
                std::lock_guard lock(g_ui->mutex);
                captures.swap(g_ui->notification_captures);
                g_ui->balloon_active = false;
            }
            KillTimer(hwnd, kNotificationTimer);
            PostMessageW(hwnd, kNotificationReady, 0, 0);
            if (!captures.empty()) {
                update_dashboard(g_ui);
                g_ui->dashboard.show_arrivals(captures);
            }
            return 0;
        }
        if ((event == NIN_BALLOONHIDE || event == NIN_BALLOONTIMEOUT) && g_ui) {
            std::lock_guard lock(g_ui->mutex);
            g_ui->notification_captures.clear();
            g_ui->balloon_active = false;
            KillTimer(hwnd, kNotificationTimer);
            PostMessageW(hwnd, kNotificationReady, 0, 0);
            return 0;
        }
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            tray_menu(g_ui);
            return 0;
        }
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP ||
            event == WM_LBUTTONDBLCLK) {
            invoke(g_ui, CmdDashboard);
            return 0;
        }
    }
    if (msg == kStateChanged) {
        update_dashboard(g_ui);
        return 0;
    }
    if (msg == kNotificationReady && g_ui) {
        show_next_notification(g_ui, hwnd);
        return 0;
    }
    if (msg == WM_TIMER && wp == kNotificationTimer && g_ui) {
        KillTimer(hwnd, kNotificationTimer);
        {
            std::lock_guard lock(g_ui->mutex);
            g_ui->balloon_active = false;
            g_ui->notification_captures.clear();
        }
        show_next_notification(g_ui, hwnd);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (g_ui) g_ui->window = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

PlatformUi::PlatformUi() : impl_(new Impl) {}
PlatformUi::~PlatformUi() { delete impl_; }

void PlatformUi::run(const PlatformCallbacks& callbacks) {
    impl_->callbacks = callbacks;
    g_ui = impl_;
    SetCurrentProcessExplicitAppUserModelID(L"minimalhandle.SwitchAlbumSync");
    theme::init_process();
    update_dashboard(impl_);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = instance;
    wc.hIcon = app_icon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTrayClass;
    RegisterClassW(&wc);

    impl_->window = CreateWindowExW(
        0,
        kTrayClass,
        kAppTitle,
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        nullptr);

    impl_->tray.cbSize = sizeof(impl_->tray);
    impl_->tray.hWnd = impl_->window;
    impl_->tray.uID = 1;
    impl_->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    impl_->tray.uCallbackMessage = kTrayMessage;
    impl_->tray.hIcon = app_icon();
    wcscpy_s(impl_->tray.szTip, kAppTitle);

    if (!impl_->window) {
        MessageBoxW(
            nullptr,
            L"Could not start the Switch Album Sync window.",
            kAppTitle,
            MB_OK | MB_ICONERROR);
        g_ui = nullptr;
        return;
    }
    impl_->tray_available = Shell_NotifyIconW(NIM_ADD, &impl_->tray) != FALSE;
    if (impl_->tray_available) {
        impl_->tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &impl_->tray);
    }

    const auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool dashboard_created = impl_->dashboard.create(app_icon(), impl_->callbacks,
        [this](const auto& path) { return copy_file_to_clipboard(path); }, impl_->tray_available);
    if (previous_dpi) SetThreadDpiAwarenessContext(previous_dpi);
    if (!dashboard_created && !impl_->tray_available) {
        DestroyWindow(impl_->window);
        g_ui = nullptr;
        MessageBoxW(nullptr, L"Could not open the dashboard or tray icon.", kAppTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    update_dashboard(impl_);
    if (!impl_->tray_available || !start_in_tray_requested()) impl_->dashboard.show();

    if (callbacks.ready) callbacks.ready();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!impl_->dashboard.route_key(msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->tray_available) Shell_NotifyIconW(NIM_DELETE, &impl_->tray);
        impl_->tray_available = false;
    }
    g_ui = nullptr;
}

void PlatformUi::stop() {
    if (const HWND window = impl_->window) PostMessageW(window, WM_CLOSE, 0, 0);
}

void PlatformUi::update(const MenuState& state) {
    std::lock_guard lock(impl_->mutex);
    impl_->state = state;
    const HWND window = impl_->window;
    if (window && !impl_->state_posted) {
        impl_->state_posted = PostMessageW(window, kStateChanged, 0, 0) != FALSE;
    }
}

void PlatformUi::notify(
    const std::string& title,
    const std::string& message) {
    notify_captures(title, message, {});
}

void PlatformUi::notify_captures(const std::string& title, const std::string& message,
                                 std::vector<std::filesystem::path> captures) {
    const HWND window = impl_->window;
    if (!window) return;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->tray_available) return;
    if (impl_->notifications.size() >= 16) impl_->notifications.pop_front();
    impl_->notifications.push_back({title, message, std::move(captures)});
    PostMessageW(window, kNotificationReady, 0, 0);
}

bool PlatformUi::copy_file_to_clipboard(const std::filesystem::path& path) {
    return copy_capture_files(impl_->window, {path});
}

std::string PlatformUi::prompt(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
    return prompt_box(title, message, initial, impl_->dashboard.visible_window());
}

bool PlatformUi::confirm(
    const std::string& title,
    const std::string& message) {
    if (title == kNxapiDisclosureTitle) {
        return disclosure_box(title, message, impl_->dashboard.visible_window());
    }
    return theme::message_box(impl_->dashboard.visible_window(), wide(message), wide(title),
                              MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

std::optional<std::vector<int>> PlatformUi::choose_options(
    const std::string& title,
    const std::string& message,
    const std::vector<std::string>& choices,
    const std::vector<int>& selected) {
    theme::Dialog dialog;
    dialog.title = wide(title);
    dialog.heading = wide(title);
    dialog.text = wide(message);
    dialog.width = 540;
    for (const auto& choice : choices) dialog.choices.push_back(wide(choice));
    dialog.selected = selected;
    dialog.buttons = {{IDOK, L"Save", true}, {IDCANCEL, L"Cancel"}};
    if (theme::run_dialog(impl_->dashboard.visible_window(), dialog) != IDOK) return std::nullopt;
    return dialog.selected;
}

std::shared_ptr<const ImportPlan> PlatformUi::choose_import_plan(const std::filesystem::path& album) {
    const auto owner = impl_->dashboard.visible_window();
    return pick_import_plan(owner ? owner : impl_->window.load(), album);
}

std::string PlatformUi::choose_folder(const std::string& initial, const wchar_t* title) {
    const HRESULT init = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileOpenDialog* dialog = nullptr;
    std::string selected;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))) &&
        dialog) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(
            options |
            FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);

        if (!initial.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    wide(initial).c_str(),
                    nullptr,
                    IID_PPV_ARGS(&folder))) &&
                folder) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(impl_->dashboard.visible_window()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                    path) {
                    selected = utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (SUCCEEDED(init)) CoUninitialize();
    return selected;
}

bool start_on_boot_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS) {
        return false;
    }
    const bool exists =
        RegQueryValueExW(
            key,
            kRunValue,
            nullptr,
            nullptr,
            nullptr,
            nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void set_start_on_boot(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (!enabled) {
        RegDeleteValueW(key, kRunValue);
        RegCloseKey(key);
        return;
    }

    const auto exe = executable_path_wide();
    if (exe.empty()) {
        RegCloseKey(key);
        return;
    }

    write_startup_command(key, L"\"" + exe + L"\"");
    RegCloseKey(key);
}
}  // namespace nso
