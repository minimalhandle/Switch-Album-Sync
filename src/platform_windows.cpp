#include "nso_album_sync/platform.hpp"

#ifdef _WIN32
#include "dashboard_windows.hpp"
#include "import_windows.hpp"
#include "library_windows.hpp"
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
constexpr wchar_t kAppTitle[] = L"CaptureDock";
constexpr wchar_t kTrayClass[] = L"CaptureDockTray";
constexpr wchar_t kPromptClass[] = L"CaptureDockPrompt";
constexpr wchar_t kDisclosureClass[] = L"CaptureDockDisclosure";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CaptureDock";
constexpr wchar_t kStartInTray[] = L"--start-in-tray";
constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiSourceUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api";

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

struct PromptState {
    HWND edit = nullptr;
    bool done = false;
    bool accepted = false;
};

struct DisclosureState {
    bool done = false;
    bool accepted = false;
};

class ScaledDialog {
public:
    ScaledDialog() : previous_(SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)) {}
    ~ScaledDialog() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }

private:
    DPI_AWARENESS_CONTEXT previous_;
};

LRESULT dialog_color(UINT msg, WPARAM wp) {
    if (msg == WM_CTLCOLORSTATIC) {
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (msg == WM_CTLCOLOREDIT) {
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    return 0;
}

HWND centered_dialog(
    DWORD ex_style,
    const wchar_t* class_name,
    const std::wstring& title,
    int client_width,
    int client_height,
    HINSTANCE instance,
    void* state,
    HWND owner) {
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT rect{0, 0, client_width, client_height};
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &area);
    const int x = area.left + (area.right - area.left - width) / 2;
    const int y = area.top + (area.bottom - area.top - height) / 2;
    return CreateWindowExW(
        ex_style, class_name, title.c_str(), style,
        x, y, width, height, owner, nullptr, instance, state);
}

HWND make_control(
    HINSTANCE instance,
    HWND parent,
    const wchar_t* cls,
    const std::wstring& caption,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id,
    DWORD ex_style = 0) {
    HWND control = CreateWindowExW(
        ex_style,
        cls,
        caption.c_str(),
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
        instance,
        nullptr);
    if (control != nullptr) {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
            TRUE);
    }
    return control;
}

int measure_text_height(const std::wstring& text, int width) {
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) return 180;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ previous = SelectObject(dc, font);
    RECT bounds{0, 0, width, 0};
    DrawTextW(
        dc,
        text.c_str(),
        static_cast<int>(text.size()),
        &bounds,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);
    return std::max(1L, bounds.bottom - bounds.top);
}

LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<PromptState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<PromptState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        return dialog_color(msg, wp);
    }

    if (state && msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            state->accepted = true;
            state->done = true;
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            state->done = true;
            return 0;
        }
    }
    if (state && msg == WM_CLOSE) {
        state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK disclosure_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DisclosureState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<DisclosureState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (msg == WM_CTLCOLORSTATIC) {
        return dialog_color(msg, wp);
    }

    if (state && msg == WM_COMMAND) {
        const auto command = LOWORD(wp);
        if (command == IDOK) {
            state->accepted = true;
            state->done = true;
            return 0;
        }
        if (command == IDCANCEL) {
            state->done = true;
            return 0;
        }
        if (command == kDisclosureSourceButton) {
            const auto url = wide(kNxapiSourceUrl);
            ShellExecuteW(
                nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }
    if (state && msg == WM_CLOSE) {
        state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string prompt_box(
    const std::string& title,
    const std::string& text,
    const std::string& initial,
    HWND owner) {
    const ScaledDialog dpi;
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = instance;
    wc.hIcon = app_icon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kPromptClass;
    RegisterClassW(&wc);

    PromptState state;
    HWND window = centered_dialog(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kPromptClass,
        wide(title),
        540,
        150,
        instance,
        &state,
        owner);
    if (!window) return initial;

    make_control(
        instance,
        window,
        L"STATIC",
        wide(text),
        SS_LEFT | SS_NOPREFIX,
        18,
        17,
        504,
        36,
        0);

    state.edit = make_control(
        instance,
        window,
        L"EDIT",
        wide(initial),
        WS_TABSTOP | ES_AUTOHSCROLL,
        18,
        63,
        504,
        27,
        10,
        WS_EX_CLIENTEDGE);
    if (state.edit != nullptr) {
        SendMessageW(
            state.edit,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(6, 6));
    }

    make_control(
        instance,
        window,
        L"BUTTON",
        L"Save",
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        354,
        105,
        80,
        28,
        IDOK);
    make_control(
        instance,
        window,
        L"BUTTON",
        L"Cancel",
        WS_TABSTOP,
        442,
        105,
        80,
        28,
        IDCANCEL);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    if (state.edit != nullptr) {
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, -1);
    }

    MSG msg{};
    BOOL received = TRUE;
    while (!state.done && (received = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (state.edit != nullptr &&
            msg.message == WM_KEYDOWN &&
            msg.hwnd == state.edit) {
            if (msg.wParam == VK_RETURN) {
                state.accepted = true;
                state.done = true;
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                state.done = true;
                continue;
            }
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!received) PostQuitMessage(static_cast<int>(msg.wParam));

    std::string result = initial;
    if (state.accepted && state.edit != nullptr) {
        const int n = GetWindowTextLengthW(state.edit);
        std::wstring value(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(state.edit, value.data(), n + 1);
        value.resize(static_cast<std::size_t>(n));
        result = utf8(value);
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(window);
    return result;
}

bool disclosure_box(const std::string& title, const std::string& text, HWND owner) {
    const ScaledDialog dpi;
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = disclosure_proc;
    wc.hInstance = instance;
    wc.hIcon = app_icon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kDisclosureClass;
    RegisterClassW(&wc);

    constexpr int client_width = 540;
    constexpr int margin = 16;
    constexpr int content_width = client_width - (margin * 2);
    constexpr int button_height = 28;
    constexpr int button_gap = 8;
    constexpr int bottom_margin = 16;
    constexpr int row_gap = 14;
    constexpr int source_width = 96;
    constexpr int button_width = 82;

    const auto message = wide(text);
    const int text_height = measure_text_height(message, content_width);
    const int button_y = margin + text_height + row_gap;
    const int client_height = button_y + button_height + bottom_margin;

    DisclosureState state;
    HWND window = centered_dialog(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kDisclosureClass,
        wide(title),
        client_width,
        client_height,
        instance,
        &state,
        owner);
    if (!window) return false;

    make_control(
        instance,
        window,
        L"STATIC",
        message,
        SS_LEFT | SS_NOPREFIX,
        margin,
        margin,
        content_width,
        text_height + 2,
        0);

    const int cancel_x = client_width - margin - button_width;
    const int continue_x = cancel_x - button_gap - button_width;
    const int source_x = continue_x - button_gap - source_width;

    make_control(
        instance,
        window,
        L"BUTTON",
        L"View Source",
        WS_TABSTOP,
        source_x,
        button_y,
        source_width,
        button_height,
        kDisclosureSourceButton);
    make_control(
        instance,
        window,
        L"BUTTON",
        L"Continue",
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        continue_x,
        button_y,
        button_width,
        button_height,
        IDOK);
    make_control(
        instance,
        window,
        L"BUTTON",
        L"Cancel",
        WS_TABSTOP,
        cancel_x,
        button_y,
        button_width,
        button_height,
        IDCANCEL);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);

    MSG msg{};
    BOOL received = TRUE;
    while (!state.done && (received = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!received) PostQuitMessage(static_cast<int>(msg.wParam));
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(window);
    return state.accepted;
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
        if (!copied) MessageBoxW(ui->dashboard.visible_window(), L"Could not copy this capture.", kAppTitle, MB_OK | MB_ICONINFORMATION);
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
    SetCurrentProcessExplicitAppUserModelID(L"minimalhandle.CaptureDock");

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
            L"Could not start the CaptureDock window.",
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
    return MessageBoxW(
        impl_->dashboard.visible_window(),
        wide(message).c_str(),
        wide(title).c_str(),
        MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES;
}

std::string PlatformUi::choose_import_source() {
    return pick_import_source(impl_->dashboard.visible_window());
}

std::string PlatformUi::choose_folder(const std::string& initial) {
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
        dialog->SetTitle(L"Choose Album Folder");

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
#endif
