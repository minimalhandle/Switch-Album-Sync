#include "theme_windows.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace nso::theme {
namespace {

constexpr wchar_t kPrimaryProp[] = L"Switch Album Sync.Primary";
constexpr wchar_t kHotProp[] = L"Switch Album Sync.Hot";
constexpr UINT_PTR kButtonSubclass = 0xCD01;
constexpr UINT_PTR kWindowSubclass = 0xCD02;
constexpr UINT_PTR kListViewSubclass = 0xCD03;

// Windows 11 caption attributes. Older SDKs may not define them.
constexpr DWORD kBorderColorAttribute = 34;
constexpr DWORD kCaptionColorAttribute = 35;
constexpr DWORD kTextColorAttribute = 36;
constexpr DWORD kImmersiveDarkModeAttribute = 20;
constexpr COLORREF kDefaultCaptionColor = 0xFFFFFFFF;

constexpr Palette kLightPalette{
    // background, card, field, border, card_border
    RGB(241, 242, 244), RGB(255, 255, 255), RGB(246, 247, 249), RGB(212, 215, 220), RGB(228, 230, 234),
    // control, control_hot, control_pressed, text, muted, disabled
    RGB(246, 247, 249), RGB(236, 238, 241), RGB(225, 228, 232), RGB(29, 31, 35), RGB(96, 102, 110), RGB(150, 155, 162),
    // track, accent, accent_hot, accent_pressed
    RGB(186, 190, 196), RGB(230, 0, 18), RGB(206, 0, 16), RGB(176, 0, 14),
};
constexpr Palette kDarkPalette{
    RGB(22, 23, 26), RGB(31, 33, 37), RGB(25, 26, 30), RGB(66, 70, 78), RGB(42, 45, 50),
    RGB(41, 44, 49), RGB(50, 53, 59), RGB(58, 62, 69), RGB(230, 232, 235), RGB(158, 164, 173), RGB(108, 114, 122),
    RGB(74, 79, 88), RGB(222, 44, 56), RGB(236, 64, 75), RGB(190, 32, 44),
};

Appearance g_appearance = Appearance::system;
std::atomic<bool> g_dark = false;

bool windows_uses_dark_apps() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light == 0;
}

// Dark popup menus and scrollbars need uxtheme's undocumented dark-mode
// exports, looked up by ordinal as other Win32 apps do. They exist from
// Windows 10 1809; when they're missing, menus and scrollbars stay light.
struct DarkModeApi {
    using AppMode = int(WINAPI*)(int);  // 1809: AllowDarkModeForApp(BOOL); 1903+: SetPreferredAppMode
    using AllowForWindow = BOOL(WINAPI*)(HWND, BOOL);
    using FlushMenus = void(WINAPI*)();
    AppMode app_mode = nullptr;
    AllowForWindow allow_for_window = nullptr;
    FlushMenus flush_menus = nullptr;
    bool preferred_app_mode = false;  // Ordinal 135 takes a PreferredAppMode
};

DWORD windows_build() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get_version = ntdll
        ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return get_version && get_version(&info) == 0 ? info.dwBuildNumber : 0;
}

const DarkModeApi& dark_mode_api() {
    static const DarkModeApi api = [] {
        DarkModeApi result;
        const auto build = windows_build();
        if (build < 17763) return result;
        const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme) return result;
        result.allow_for_window = reinterpret_cast<DarkModeApi::AllowForWindow>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        result.app_mode = reinterpret_cast<DarkModeApi::AppMode>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        result.flush_menus = reinterpret_cast<DarkModeApi::FlushMenus>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        result.preferred_app_mode = build >= 18362;
        return result;
    }();
    return api;
}

void apply_app_mode(bool dark) {
    const auto& api = dark_mode_api();
    if (api.app_mode) {
        constexpr int kForceDark = 2;
        constexpr int kForceLight = 3;
        api.app_mode(api.preferred_app_mode ? (dark ? kForceDark : kForceLight) : (dark ? TRUE : FALSE));
    }
    if (api.flush_menus) api.flush_menus();
}

// Scrollbars and other system-drawn parts follow the window's visual style.
void apply_control_theme(HWND window, const wchar_t* dark_theme) {
    const bool dark = g_dark.load(std::memory_order_relaxed);
    if (const auto allow = dark_mode_api().allow_for_window) allow(window, dark ? TRUE : FALSE);
    SetWindowTheme(window, dark ? dark_theme : nullptr, nullptr);
}

// The dark header theme keeps dark text, so colour the column titles through
// the header's custom draw, which it sends to its parent list view.
LRESULT CALLBACK list_view_proc(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(list, list_view_proc, kListViewSubclass);
    if (msg == WM_NOTIFY && g_dark.load(std::memory_order_relaxed)) {
        const auto* notice = reinterpret_cast<const NMHDR*>(lp);
        if (notice->code == NM_CUSTOMDRAW && notice->hwndFrom == ListView_GetHeader(list)) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(draw->hdc, kDarkPalette.text);
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(list, msg, wp, lp);
}

bool refresh_palette() {
    const bool dark = !high_contrast() &&
        (g_appearance == Appearance::dark || (g_appearance == Appearance::system && windows_uses_dark_apps()));
    const bool changed = g_dark.exchange(dark, std::memory_order_relaxed) != dark;
    if (changed) apply_app_mode(dark);
    return changed;
}

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, window_proc, kWindowSubclass);
    const auto result = DefSubclassProc(window, msg, wp, lp);
    if (msg == WM_SETTINGCHANGE || msg == WM_SYSCOLORCHANGE) {
        refresh_palette();
        apply(window);
    }
    return result;
}

ULONG_PTR g_gdiplus = 0;

Gdiplus::Color gdi(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void round_path(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    const float d = std::max(1.0f, std::min({radius * 2, r.Width, r.Height}));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

int scale(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

bool is_toggle(LONG_PTR style) {
    const auto type = style & BS_TYPEMASK;
    return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX;
}

bool is_push(LONG_PTR style) {
    const auto type = style & BS_TYPEMASK;
    return type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
}

void paint_button(HWND button, HDC target) {
    RECT bounds{};
    GetClientRect(button, &bounds);
    const int width = bounds.right, height = bounds.bottom;
    if (width <= 0 || height <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP canvas = CreateCompatibleBitmap(target, width, height);
    if (!dc || !canvas) {
        if (dc) DeleteDC(dc);
        if (canvas) DeleteObject(canvas);
        return;
    }
    const auto old_canvas = SelectObject(dc, canvas);

    auto surface = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(button), WM_CTLCOLORBTN,
        reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(button)));
    if (!surface) surface = brush(palette().background);
    FillRect(dc, &bounds, surface);
    LOGBRUSH surface_info{};
    const COLORREF surface_color = GetObjectW(surface, sizeof(surface_info), &surface_info) &&
        surface_info.lbStyle == BS_SOLID ? surface_info.lbColor : palette().background;
    // Keyboard focus is a ring in the text colour, separated from the control
    // by a gap in the surface colour; `shape` shrinks to fit inside it.
    const auto focus_ring = [&](RECT& shape, int radius) {
        fill_round(dc, shape, radius, palette().text);
        InflateRect(&shape, -scale(button, 2), -scale(button, 2));
        fill_round(dc, shape, std::max(0, radius - scale(button, 2)), surface_color);
        InflateRect(&shape, -scale(button, 1), -scale(button, 1));
    };

    const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    wchar_t label[128]{};
    GetWindowTextW(button, label, static_cast<int>(std::size(label)));

    const auto style = GetWindowLongPtrW(button, GWL_STYLE);
    const bool enabled = IsWindowEnabled(button) != FALSE;
    const bool pressed = (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    const bool hot = GetPropW(button, kHotProp) != nullptr;
    const auto ui = SendMessageW(button, WM_QUERYUISTATE, 0, 0);
    const bool focused = GetFocus() == button && !(ui & UISF_HIDEFOCUS);
    const UINT prefix = (ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
    SetBkMode(dc, TRANSPARENT);

    if (is_toggle(style)) {
        const bool on = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const int track_w = scale(button, 36), track_h = scale(button, 20);
        RECT track{scale(button, 3), (height - track_h) / 2, 0, 0};
        track.right = track.left + track_w;
        track.bottom = track.top + track_h;
        if (focused) {
            RECT ring = track;
            InflateRect(&ring, scale(button, 3), scale(button, 3));
            focus_ring(ring, (ring.bottom - ring.top) / 2);
        }
        const COLORREF fill = !enabled ? palette().control_pressed :
            on ? (hot ? palette().accent_hot : palette().accent) : (hot ? palette().muted : palette().track);
        fill_round(dc, track, track_h / 2, fill);
        const int knob = track_h - scale(button, 8);
        RECT dot{on ? track.right - scale(button, 4) - knob : track.left + scale(button, 4),
                 track.top + scale(button, 4), 0, 0};
        dot.right = dot.left + knob;
        dot.bottom = dot.top + knob;
        fill_round(dc, dot, knob / 2, enabled ? RGB(255, 255, 255) : palette().disabled);
        RECT text = bounds;
        text.left = track.right + scale(button, 10);
        SetTextColor(dc, enabled ? palette().text : palette().disabled);
        DrawTextW(dc, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | prefix);
    } else {
        const bool primary = GetPropW(button, kPrimaryProp) != nullptr;
        const bool dark = g_dark.load(std::memory_order_relaxed);
        const COLORREF fill = !enabled ? palette().control_pressed :
            pressed ? (primary ? palette().accent_pressed : palette().control_pressed) :
            hot ? (primary ? palette().accent_hot : palette().control_hot) : (primary ? palette().accent : palette().control);
        RECT shape = bounds;
        const int radius = std::min(scale(button, 8), height / 2);
        if (focused) focus_ring(shape, radius);
        // Dark mode separates secondary buttons by fill alone; light mode adds
        // a soft outline so they don't vanish against white cards.
        const bool outline = !primary && enabled && !dark;
        fill_round(dc, shape, std::max(0, radius - static_cast<int>(bounds.bottom - shape.bottom)), fill,
                   outline ? palette().border : CLR_INVALID);
        SetTextColor(dc, !enabled ? palette().disabled : primary ? RGB(255, 255, 255) : palette().text);
        RECT text = bounds;
        InflateRect(&text, -scale(button, 8), 0);
        DrawTextW(dc, label, -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | prefix);
    }

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_canvas);
    DeleteObject(canvas);
    DeleteDC(dc);
}

// Lets the native button update its state without drawing its own light
// visuals, then repaints with the theme.
LRESULT quietly(HWND button, UINT msg, WPARAM wp, LPARAM lp) {
    const bool visible = IsWindowVisible(button) != FALSE;
    if (visible) DefSubclassProc(button, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(button, msg, wp, lp);
    if (visible && IsWindow(button)) DefSubclassProc(button, WM_SETREDRAW, TRUE, 0);
    if (IsWindow(button)) InvalidateRect(button, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK button_proc(HWND button, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemovePropW(button, kHotProp);
        RemovePropW(button, kPrimaryProp);
        RemoveWindowSubclass(button, button_proc, kButtonSubclass);
        return DefSubclassProc(button, msg, wp, lp);
    }
    if (high_contrast()) return DefSubclassProc(button, msg, wp, lp);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(button, &paint);
            paint_button(button, dc);
            EndPaint(button, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_button(button, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (!GetPropW(button, kHotProp)) {
                SetPropW(button, kHotProp, reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
                TrackMouseEvent(&track);
                InvalidateRect(button, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(button, kHotProp);
            InvalidateRect(button, nullptr, FALSE);
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_KEYDOWN:
        case BM_SETSTATE: case BM_SETCHECK: case BM_SETSTYLE:
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
        case WM_SETTEXT: case WM_UPDATEUISTATE:
            return quietly(button, msg, wp, lp);
        case WM_LBUTTONUP: case WM_KEYUP: case WM_CAPTURECHANGED: {
            // These can run the click handler, which may hide or move the
            // button, so redraw is never suppressed around them.
            const LRESULT result = DefSubclassProc(button, msg, wp, lp);
            if (IsWindow(button)) InvalidateRect(button, nullptr, FALSE);
            return result;
        }
        default: break;
    }
    return DefSubclassProc(button, msg, wp, lp);
}

void theme_child(HWND child) {
    wchar_t name[64]{};
    GetClassNameW(child, name, static_cast<int>(std::size(name)));
    if (!lstrcmpiW(name, L"Button")) {
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        if (!is_push(style) && !is_toggle(style)) return;
        DWORD_PTR data = 0;
        if (GetWindowSubclass(child, button_proc, kButtonSubclass, &data)) return;
        if ((style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) set_primary(child);
        SetWindowSubclass(child, button_proc, kButtonSubclass, 0);
    } else if (!lstrcmpiW(name, WC_LISTVIEWW)) {
        const bool contrast = high_contrast();
        const auto background = contrast ? GetSysColor(COLOR_WINDOW) : palette().card;
        const auto text = contrast ? GetSysColor(COLOR_WINDOWTEXT) : palette().text;
        ListView_SetBkColor(child, background);
        ListView_SetTextBkColor(child, background);
        ListView_SetTextColor(child, text);
        apply_control_theme(child, L"DarkMode_Explorer");
        if (const HWND header = ListView_GetHeader(child)) {
            apply_control_theme(header, L"DarkMode_ItemsView");
            SetWindowSubclass(child, list_view_proc, kListViewSubclass, 0);
        }
    } else if (!lstrcmpiW(name, L"ListBox")) {
        apply_control_theme(child, L"DarkMode_Explorer");
    } else if (!lstrcmpiW(name, WC_COMBOBOXW)) {
        apply_control_theme(child, L"DarkMode_CFD");
    } else if (!lstrcmpiW(name, PROGRESS_CLASSW)) {
        const bool contrast = high_contrast();
        SetWindowTheme(child, contrast ? nullptr : L"", contrast ? nullptr : L"");
        SendMessageW(child, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(contrast ? CLR_DEFAULT : palette().accent));
        SendMessageW(child, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(contrast ? CLR_DEFAULT : palette().control_pressed));
    }
}

}  // namespace

const Palette& palette() {
    return g_dark.load(std::memory_order_relaxed) ? kDarkPalette : kLightPalette;
}

void set_appearance(Appearance appearance) {
    g_appearance = appearance;
    if (!refresh_palette()) return;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        DWORD_PTR data = 0;
        if (GetWindowSubclass(window, window_proc, kWindowSubclass, &data)) apply(window);
        return TRUE;
    }, 0);
}

bool high_contrast() {
    HIGHCONTRASTW info{sizeof(info)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(info), &info, 0) &&
        (info.dwFlags & HCF_HIGHCONTRASTON);
}

void init_process() {
    static std::once_flag once;
    std::call_once(once, [] {
        Gdiplus::GdiplusStartupInput options;
        Gdiplus::GdiplusStartup(&g_gdiplus, &options, nullptr);
    });
}

HBRUSH brush(COLORREF color) {
    static std::mutex mutex;
    static std::map<COLORREF, HBRUSH> brushes;
    std::lock_guard lock(mutex);
    auto& cached = brushes[color];
    if (!cached) cached = CreateSolidBrush(color);
    return cached;
}

void apply(HWND window) {
    init_process();
    SetWindowSubclass(window, window_proc, kWindowSubclass, 0);
    const bool contrast = high_contrast();
    const BOOL dark = !contrast && g_dark.load(std::memory_order_relaxed);
    // The caption blends into the window so the title bar doesn't compete
    // with the content.
    const COLORREF title = contrast ? kDefaultCaptionColor : palette().background;
    const COLORREF border = contrast ? kDefaultCaptionColor : palette().border;
    const COLORREF text = contrast ? kDefaultCaptionColor : palette().text;
    DwmSetWindowAttribute(window, kImmersiveDarkModeAttribute, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, kCaptionColorAttribute, &title, sizeof(title));
    DwmSetWindowAttribute(window, kBorderColorAttribute, &border, sizeof(border));
    DwmSetWindowAttribute(window, kTextColorAttribute, &text, sizeof(text));
    // The window's own scrollbar (the dashboard scrolls) follows its theme.
    apply_control_theme(window, L"DarkMode_Explorer");
    EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
        theme_child(child);
        return TRUE;
    }, 0);
    RedrawWindow(window, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void set_primary(HWND button, bool primary) {
    if (!button) return;
    const bool current = GetPropW(button, kPrimaryProp) != nullptr;
    if (current == primary) return;
    if (primary) SetPropW(button, kPrimaryProp, reinterpret_cast<HANDLE>(1));
    else RemovePropW(button, kPrimaryProp);
    InvalidateRect(button, nullptr, FALSE);
}

LRESULT control_color(UINT msg, WPARAM wp, LPARAM lp, COLORREF surface) {
    if (high_contrast()) return 0;
    const auto dc = reinterpret_cast<HDC>(wp);
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            wchar_t name[16]{};
            GetClassNameW(reinterpret_cast<HWND>(lp), name, static_cast<int>(std::size(name)));
            const bool field = !lstrcmpiW(name, L"Edit");
            SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? palette().text : palette().disabled);
            SetBkColor(dc, field ? palette().field : surface);
            if (!field) SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(brush(field ? palette().field : surface));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(dc, palette().text);
            SetBkColor(dc, palette().field);
            return reinterpret_cast<LRESULT>(brush(palette().field));
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
            SetTextColor(dc, palette().text);
            SetBkColor(dc, surface);
            return reinterpret_cast<LRESULT>(brush(surface));
        default:
            return 0;
    }
}

void fill_round(HDC dc, const RECT& bounds, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const Gdiplus::RectF area(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top));
    Gdiplus::GraphicsPath path;
    round_path(path, area, static_cast<float>(radius));
    Gdiplus::SolidBrush paint(gdi(fill));
    graphics.FillPath(&paint, &path);
    if (border != CLR_INVALID) {
        Gdiplus::GraphicsPath edge;
        round_path(edge, Gdiplus::RectF(area.X + 0.5f, area.Y + 0.5f, area.Width - 1, area.Height - 1),
                   std::max(0.0f, radius - 0.5f));
        Gdiplus::Pen pen(gdi(border), 1.0f);
        graphics.DrawPath(&pen, &edge);
    }
}

namespace {

constexpr wchar_t kDialogClass[] = L"SwitchAlbumSyncDialog";
constexpr int kHeadingId = 900;
constexpr int kTextId = 901;
constexpr int kFieldId = 902;
constexpr int kCheckboxId = 903;
constexpr int kChoicesId = 904;

struct DialogWindow {
    Dialog* spec = nullptr;
    HWND window = nullptr;
    HWND owner = nullptr;
    HWND heading = nullptr;
    HWND text = nullptr;
    HWND field = nullptr;
    HWND checkbox = nullptr;
    HWND choices = nullptr;
    std::vector<HWND> buttons;
    RECT choices_frame{};
    HFONT body = nullptr;
    HFONT bold = nullptr;
    UINT dpi = 96;
    int result = 0;
    bool done = false;

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

    ~DialogWindow() {
        if (body) DeleteObject(body);
        if (bold) DeleteObject(bold);
    }

    void fonts() {
        const auto make = [this](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        const auto old_body = body, old_bold = bold;
        body = make(14, FW_NORMAL);
        bold = make(18, FW_SEMIBOLD);
        for (HWND control : buttons) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body), FALSE);
        SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(body), FALSE);
        if (field) {
            SendMessageW(field, WM_SETFONT, reinterpret_cast<WPARAM>(body), FALSE);
            SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(6), px(6)));
        }
        if (checkbox) SendMessageW(checkbox, WM_SETFONT, reinterpret_cast<WPARAM>(body), FALSE);
        if (choices) SendMessageW(choices, WM_SETFONT, reinterpret_cast<WPARAM>(body), FALSE);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(bold), FALSE);
        if (old_body) DeleteObject(old_body);
        if (old_bold) DeleteObject(old_bold);
    }

    SIZE measure(HFONT font, const std::wstring& value, int width, bool wrap) const {
        HDC dc = GetDC(window);
        const auto old = SelectObject(dc, font);
        RECT bounds{0, 0, width, 0};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds,
            DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE));
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        return {bounds.right - bounds.left, std::max(1L, bounds.bottom - bounds.top)};
    }

    // Lays out the controls and returns the client size they need.
    SIZE layout() {
        const int width = px(spec->width), margin = px(24), inner = width - margin * 2;
        int y = margin + px(4);
        const auto place = [](HWND control, int x, int top, int w, int h) {
            SetWindowPos(control, nullptr, x, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        const int heading_height = measure(bold, spec->heading, inner, true).cy;
        place(heading, margin, y, inner, heading_height);
        y += heading_height + px(8);
        if (!spec->text.empty()) {
            const int text_height = measure(body, spec->text, inner, true).cy;
            place(text, margin, y, inner, text_height + px(2));
            y += text_height + px(18);
        }
        if (field) {
            place(field, margin, y, inner, px(30));
            y += px(30) + px(20);
        }
        if (checkbox) {
            place(checkbox, margin, y, inner, px(26));
            y += px(38);
        }
        if (choices) {
            // The list is borderless inside a drawn rounded frame.
            choices_frame = RECT{margin, y, margin + inner, y + px(210)};
            const int inset = px(4);
            place(choices, margin + inset, y + inset, inner - inset * 2, px(210) - inset * 2);
            SendMessageW(choices, LB_SETITEMHEIGHT, 0, px(30));
            y += px(224);
        }
        const int height = px(32), gap = px(8);
        int right = width - margin;
        for (std::size_t i = buttons.size(); i-- > 0;) {
            const int w = std::max(px(96), static_cast<int>(measure(body, spec->buttons[i].label, 0, false).cx) + px(40));
            place(buttons[i], right - w, y, w, height);
            right -= w + gap;
        }
        return {width, y + height + margin};
    }

    void fit(const RECT* suggested) {
        const SIZE client = layout();
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        RECT frame{0, 0, client.cx, client.cy};
        AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, dpi);
        const int w = frame.right - frame.left, h = frame.bottom - frame.top;
        int x = 0, y = 0;
        if (suggested) {
            x = suggested->left;
            y = suggested->top;
        } else {
            RECT area{};
            if (owner && IsWindowVisible(owner)) GetWindowRect(owner, &area);
            else {
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                area = monitor.rcWork;
            }
            x = area.left + (area.right - area.left - w) / 2;
            y = area.top + (area.bottom - area.top - h) / 2;
        }
        SetWindowPos(window, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int primary_id() const {
        for (const auto& button : spec->buttons) if (button.primary) return button.id;
        return spec->buttons.empty() ? spec->cancel_id : spec->buttons.front().id;
    }

    void finish(int id) {
        result = id;
        done = true;
    }

    void command(int id) {
        for (const auto& button : spec->buttons) {
            if (button.id != id) continue;
            if (button.closes) finish(id);
            else if (spec->on_command) spec->on_command(id);
            return;
        }
        if (id == IDOK) finish(primary_id());
        else if (id == IDCANCEL) finish(spec->cancel_id);
    }

    // Draws one option row: a checkbox (or radio dot for single choice) and
    // the label, so a chosen option reads as ticked rather than highlighted.
    void draw_choice(const DRAWITEMSTRUCT& item) const {
        const HDC dc = item.hDC;
        RECT row = item.rcItem;
        const bool contrast = high_contrast();
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const bool focused = (item.itemState & ODS_FOCUS) != 0 && !(item.itemState & ODS_NOFOCUSRECT);
        const COLORREF background = contrast ? GetSysColor(COLOR_WINDOW) :
            focused ? palette().control_hot : palette().field;
        const COLORREF ink = contrast ? GetSysColor(COLOR_WINDOWTEXT) : palette().text;
        FillRect(dc, &row, contrast ? GetSysColorBrush(COLOR_WINDOW) : brush(palette().field));
        if (item.itemID == static_cast<UINT>(-1)) return;
        if (focused && !contrast) {
            RECT band = row;
            InflateRect(&band, 0, -px(1));
            fill_round(dc, band, px(4), background);
        }

        const int box = px(16);
        RECT mark{row.left + px(8), row.top + (row.bottom - row.top - box) / 2, 0, 0};
        mark.right = mark.left + box;
        mark.bottom = mark.top + box;
        const COLORREF on = contrast ? GetSysColor(COLOR_HIGHLIGHT) : palette().accent;
        const COLORREF edge = contrast ? GetSysColor(COLOR_WINDOWTEXT) : palette().muted;
        if (spec->single_choice) {
            fill_round(dc, mark, box / 2, selected ? on : background, selected ? CLR_INVALID : edge);
            if (selected) {
                RECT dot = mark;
                InflateRect(&dot, -px(5), -px(5));
                fill_round(dc, dot, (dot.right - dot.left) / 2,
                           contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255));
            }
        } else {
            fill_round(dc, mark, px(4), selected ? on : background, selected ? CLR_INVALID : edge);
            if (selected) {
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(gdi(contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255)),
                                 static_cast<float>(px(2)));
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                const float x = static_cast<float>(mark.left), y = static_cast<float>(mark.top);
                const float s = static_cast<float>(box);
                const Gdiplus::PointF points[] = {{x + s * 0.25f, y + s * 0.52f},
                                                  {x + s * 0.43f, y + s * 0.70f},
                                                  {x + s * 0.76f, y + s * 0.32f}};
                graphics.DrawLines(&pen, points, 3);
            }
        }

        const auto length = SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0);
        std::wstring label(length > 0 ? static_cast<std::size_t>(length) + 1 : 1, L'\0');
        if (length > 0) {
            SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<std::size_t>(length));
        } else {
            label.clear();
        }
        RECT label_bounds = row;
        label_bounds.left = mark.right + px(10);
        label_bounds.right -= px(8);
        const auto old_font = SelectObject(dc, body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &label_bounds,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: {
                RECT bounds{};
                GetClientRect(window, &bounds);
                const auto dc = reinterpret_cast<HDC>(wp);
                if (high_contrast()) {
                    FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));
                    return TRUE;
                }
                FillRect(dc, &bounds, brush(palette().background));
                if (choices) fill_round(dc, choices_frame, px(6), palette().field, palette().border);
                bounds.bottom = px(3);
                FillRect(dc, &bounds, brush(palette().accent));
                return TRUE;
            }
            case WM_MEASUREITEM: {
                auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
                if (item->CtlID != kChoicesId) break;
                item->itemHeight = px(30);
                return TRUE;
            }
            case WM_DRAWITEM: {
                const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
                if (item->CtlID != kChoicesId) break;
                draw_choice(*item);
                return TRUE;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLORLISTBOX:
                if (const auto color = control_color(msg, wp, lp, palette().background)) {
                    return color;
                }
                break;
            case DM_GETDEFID:
                return MAKELRESULT(primary_id(), DC_HASDEFID);
            case WM_COMMAND:
                if (HIWORD(wp) == BN_CLICKED || LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) command(LOWORD(wp));
                return 0;
            case WM_TIMER:
                if (wp == 1 && spec->completed && spec->completed()) finish(IDOK);
                return 0;
            case WM_CLOSE:
                finish(spec->cancel_id);
                return 0;
            case WM_DPICHANGED:
                dpi = HIWORD(wp);
                fonts();
                fit(reinterpret_cast<const RECT*>(lp));
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            default: break;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<DialogWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self && self->window ? self->message(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
    }
};

}  // namespace

int run_dialog(HWND owner, Dialog& spec) {
    init_process();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = DialogWindow::procedure;
    type.hInstance = instance;
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = kDialogClass;
    RegisterClassW(&type);

    DialogWindow state;
    state.spec = &spec;
    state.owner = owner;
    state.result = spec.cancel_id;
    POINT anchor{};
    if (owner && IsWindowVisible(owner)) {
        RECT bounds{};
        GetWindowRect(owner, &bounds);
        anchor = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    } else {
        GetCursorPos(&anchor);
    }
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        spec.title.c_str(), style, anchor.x, anchor.y, 1, 1, owner, nullptr, instance, &state);
    if (previous) SetThreadDpiAwarenessContext(previous);
    if (!window) return spec.cancel_id;
    state.dpi = GetDpiForWindow(window);

    const auto child = [&](const wchar_t* type_name, const std::wstring& label, DWORD child_style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, type_name, label.c_str(), WS_CHILD | WS_VISIBLE | child_style, 0, 0, 0, 0,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    state.heading = child(L"STATIC", spec.heading.empty() ? spec.title : spec.heading, SS_LEFT | SS_NOPREFIX, kHeadingId);
    if (spec.heading.empty()) spec.heading = spec.title;
    state.text = child(L"STATIC", spec.text, SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kTextId);
    if (spec.field) {
        state.field = child(L"EDIT", spec.field_value, WS_TABSTOP | ES_AUTOHSCROLL, kFieldId, WS_EX_CLIENTEDGE);
        if (!spec.field_cue.empty()) {
            SendMessageW(state.field, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(spec.field_cue.c_str()));
        }
    }
    if (!spec.checkbox_label.empty()) {
        state.checkbox = child(L"BUTTON", spec.checkbox_label, WS_TABSTOP | BS_AUTOCHECKBOX, kCheckboxId);
        SendMessageW(state.checkbox, BM_SETCHECK, spec.checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (!spec.choices.empty()) {
        state.choices = child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
            (spec.single_choice ? 0 : LBS_MULTIPLESEL) | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, kChoicesId);
        for (const auto& choice : spec.choices) {
            if (SendMessageW(state.choices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.c_str())) < 0) {
                DestroyWindow(window);
                return spec.cancel_id;
            }
        }
        if (spec.single_choice) {
            if (!spec.selected.empty()) SendMessageW(state.choices, LB_SETCURSEL, spec.selected.front(), 0);
        } else {
            for (int index : spec.selected) SendMessageW(state.choices, LB_SETSEL, TRUE, index);
        }
    }
    const int primary = state.primary_id();
    for (const auto& button : spec.buttons) {
        const HWND control = child(L"BUTTON", button.label,
            WS_TABSTOP | (button.id == primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), button.id);
        state.buttons.push_back(control);
    }
    state.fonts();
    if (state.choices) {
        LONG extent = 0;
        for (const auto& choice : spec.choices) extent = std::max(extent, state.measure(state.body, choice, 0, false).cx);
        SendMessageW(state.choices, LB_SETHORIZONTALEXTENT, extent + state.px(46), 0);
    }
    state.fit(nullptr);
    apply(window);
    for (std::size_t i = 0; i < spec.buttons.size(); ++i) set_primary(state.buttons[i], spec.buttons[i].primary);

    if (owner) EnableWindow(owner, FALSE);
    if (spec.completed && !SetTimer(window, 1, 100, nullptr)) {
        if (owner) EnableWindow(owner, TRUE);
        DestroyWindow(window);
        return spec.cancel_id;
    }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    HWND focus = state.field ? state.field : state.choices;
    for (std::size_t i = 0; !focus && i < spec.buttons.size(); ++i) {
        if (spec.buttons[i].id == (spec.focus_id ? spec.focus_id : primary)) focus = state.buttons[i];
    }
    if (focus) SetFocus(focus);
    if (state.field) SendMessageW(state.field, EM_SETSEL, 0, -1);

    MSG msg{};
    BOOL received = TRUE;
    while (!state.done && (received = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == state.field) {
            if (msg.wParam == VK_RETURN) {
                state.finish(primary);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                state.finish(spec.cancel_id);
                continue;
            }
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!received) PostQuitMessage(static_cast<int>(msg.wParam));
    if (state.field && state.result != spec.cancel_id) {
        const int length = GetWindowTextLengthW(state.field);
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(state.field, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(length));
        spec.field_value = std::move(value);
    }
    if (state.result != spec.cancel_id) {
        if (state.checkbox) spec.checked = SendMessageW(state.checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (state.choices && spec.single_choice) {
            const auto index = SendMessageW(state.choices, LB_GETCURSEL, 0, 0);
            spec.selected.clear();
            if (index != LB_ERR) spec.selected.push_back(static_cast<int>(index));
        } else if (state.choices) {
            const auto count = SendMessageW(state.choices, LB_GETSELCOUNT, 0, 0);
            spec.selected.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
            if (count > 0) SendMessageW(state.choices, LB_GETSELITEMS, count,
                reinterpret_cast<LPARAM>(spec.selected.data()));
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(window);
    return state.result;
}

int message_box(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags) {
    Dialog dialog;
    dialog.title = title;
    dialog.heading = title;
    dialog.text = text;
    const UINT kind = flags & MB_TYPEMASK;
    if (kind == MB_YESNO) {
        dialog.buttons = {{IDYES, L"Yes", true}, {IDNO, L"No"}};
        dialog.cancel_id = IDNO;
    } else if (kind == MB_OKCANCEL) {
        dialog.buttons = {{IDOK, L"OK", true}, {IDCANCEL, L"Cancel"}};
    } else {
        dialog.buttons = {{IDOK, L"OK", true}};
        dialog.cancel_id = IDOK;
    }
    if ((flags & MB_DEFMASK) == MB_DEFBUTTON2 && dialog.buttons.size() > 1) dialog.focus_id = dialog.buttons[1].id;
    return run_dialog(owner, dialog);
}

}  // namespace nso::theme
