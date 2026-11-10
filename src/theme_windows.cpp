#include "theme_windows.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace nso::theme {
namespace {

constexpr wchar_t kPrimaryProp[] = L"CaptureDock.Primary";
constexpr wchar_t kHotProp[] = L"CaptureDock.Hot";
constexpr UINT_PTR kButtonSubclass = 0xCD01;

// Windows 10 1809+/Windows 11 attributes. Older SDKs may not define them.
constexpr DWORD kDarkModeAttribute = 20;
constexpr DWORD kDarkModeAttributeLegacy = 19;
constexpr DWORD kBorderColorAttribute = 34;
constexpr DWORD kCaptionColorAttribute = 35;
constexpr DWORD kTextColorAttribute = 36;

using AllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);
AllowDarkModeForWindow g_allow_dark_window = nullptr;
ULONG_PTR g_gdiplus = 0;

DWORD windows_build() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get = ntdll ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW info{sizeof(info)};
    return get && get(&info) == 0 ? info.dwBuildNumber : 0;
}

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
    FillRect(dc, &bounds, surface ? surface : brush(kBackground));

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
        const int track_w = scale(button, 34), track_h = scale(button, 18);
        RECT track{scale(button, 2), (height - track_h) / 2, 0, 0};
        track.right = track.left + track_w;
        track.bottom = track.top + track_h;
        if (focused) {
            RECT ring = track;
            InflateRect(&ring, scale(button, 2), scale(button, 2));
            fill_round(dc, ring, (ring.bottom - ring.top) / 2, kText);
        }
        const COLORREF fill = on ? (enabled ? (hot ? kRedHot : kRed) : RGB(96, 24, 30)) :
            enabled ? (hot ? kControlHot : kControl) : kControlPressed;
        fill_round(dc, track, track_h / 2, fill, on ? CLR_INVALID : kBorder);
        const int knob = track_h - scale(button, 6);
        RECT dot{on ? track.right - scale(button, 3) - knob : track.left + scale(button, 3),
                 track.top + scale(button, 3), 0, 0};
        dot.right = dot.left + knob;
        dot.bottom = dot.top + knob;
        fill_round(dc, dot, knob / 2, enabled ? RGB(255, 255, 255) : kDisabled);
        RECT text = bounds;
        text.left = track.right + scale(button, 10);
        SetTextColor(dc, enabled ? kText : kDisabled);
        DrawTextW(dc, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | prefix);
    } else {
        const bool primary = GetPropW(button, kPrimaryProp) != nullptr;
        const COLORREF fill = !enabled ? (primary ? RGB(96, 24, 30) : kControlPressed) :
            pressed ? (primary ? kRedPressed : kControlPressed) :
            hot ? (primary ? kRedHot : kControlHot) : (primary ? kRed : kControl);
        RECT shape = bounds;
        if (focused) {
            fill_round(dc, shape, height / 2, primary ? kText : kRed);
            InflateRect(&shape, -scale(button, 2), -scale(button, 2));
        }
        fill_round(dc, shape, (shape.bottom - shape.top) / 2, fill,
                   primary || !enabled || focused ? CLR_INVALID : kBorder);
        SetTextColor(dc, !enabled ? (primary ? RGB(200, 140, 145) : kDisabled) :
                         primary ? RGB(255, 255, 255) : kText);
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

// Header controls report custom draw to their list view, so the list view is
// subclassed to give column titles readable text on the dark header.
LRESULT CALLBACK list_proc(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(list, list_proc, id);
    } else if (msg == WM_NOTIFY && !high_contrast()) {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (draw->hdr.code == NM_CUSTOMDRAW && draw->hdr.hwndFrom == ListView_GetHeader(list)) {
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(draw->hdc, kMuted);
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(list, msg, wp, lp);
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
    } else if (!lstrcmpiW(name, L"Edit")) {
        SetWindowTheme(child, L"DarkMode_CFD", nullptr);
    } else if (!lstrcmpiW(name, L"ComboBox")) {
        SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(child, &info) && info.hwndList) {
            SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
        }
    } else if (!lstrcmpiW(name, WC_LISTVIEWW)) {
        SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        if (const auto header = ListView_GetHeader(child)) SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
        SetWindowSubclass(child, list_proc, kButtonSubclass + 1, 0);
        ListView_SetBkColor(child, kField);
        ListView_SetTextBkColor(child, kField);
        ListView_SetTextColor(child, kText);
    } else if (!lstrcmpiW(name, PROGRESS_CLASSW)) {
        SetWindowTheme(child, L"", L"");
        SendMessageW(child, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(kRed));
        SendMessageW(child, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kControl));
    }
}

}  // namespace

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
        const DWORD build = windows_build();
        if (build < 17763) return;
        const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme) return;
        // Undocumented but stable uxtheme exports used for dark menus and
        // scrollbars: 133 AllowDarkModeForWindow, 135 SetPreferredAppMode
        // (AllowDarkModeForApp before 1903), 136 FlushMenuThemes.
        g_allow_dark_window = reinterpret_cast<AllowDarkModeForWindow>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        using SetAppMode = int(WINAPI*)(int);
        if (const auto set_mode = reinterpret_cast<SetAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))) {
            set_mode(build >= 18362 ? 2 /* ForceDark */ : 1 /* allow */);
        }
        using Flush = void(WINAPI*)();
        if (const auto flush = reinterpret_cast<Flush>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)))) flush();
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

void apply(HWND window, Caption caption) {
    init_process();
    if (!high_contrast()) {
        const BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(window, kDarkModeAttribute, &dark, sizeof(dark)))) {
            DwmSetWindowAttribute(window, kDarkModeAttributeLegacy, &dark, sizeof(dark));
        }
        const COLORREF title = caption == Caption::red ? kRed : kBackground;
        const COLORREF border = caption == Caption::red ? kRed : kBorder;
        const COLORREF text = RGB(255, 255, 255);
        DwmSetWindowAttribute(window, kCaptionColorAttribute, &title, sizeof(title));
        DwmSetWindowAttribute(window, kBorderColorAttribute, &border, sizeof(border));
        DwmSetWindowAttribute(window, kTextColorAttribute, &text, sizeof(text));
        if (g_allow_dark_window) g_allow_dark_window(window, TRUE);
        SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
    }
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
            SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? kText : kDisabled);
            SetBkColor(dc, field ? kField : surface);
            if (!field) SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(brush(field ? kField : surface));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(dc, kText);
            SetBkColor(dc, kField);
            return reinterpret_cast<LRESULT>(brush(kField));
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
            SetTextColor(dc, kText);
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

}  // namespace nso::theme
