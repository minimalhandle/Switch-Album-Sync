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

constexpr wchar_t kPrimaryProp[] = L"Switch Album Sync.Primary";
constexpr wchar_t kHotProp[] = L"Switch Album Sync.Hot";
constexpr UINT_PTR kButtonSubclass = 0xCD01;

// Windows 11 caption attributes. Older SDKs may not define them.
constexpr DWORD kBorderColorAttribute = 34;
constexpr DWORD kCaptionColorAttribute = 35;
constexpr DWORD kTextColorAttribute = 36;

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
        const COLORREF fill = !enabled ? kControlPressed :
            on ? (hot ? kRedHot : kRed) : (hot ? kMuted : kDisabled);
        fill_round(dc, track, track_h / 2, fill);
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
        const COLORREF fill = !enabled ? kControlPressed :
            pressed ? (primary ? kRedPressed : kControlPressed) :
            hot ? (primary ? kRedHot : kControlHot) : (primary ? kRed : kControl);
        RECT shape = bounds;
        if (focused) {
            fill_round(dc, shape, height / 2, primary ? kText : kRed);
            InflateRect(&shape, -scale(button, 2), -scale(button, 2));
        }
        fill_round(dc, shape, (shape.bottom - shape.top) / 2, fill,
                   primary || !enabled || focused ? CLR_INVALID : kBorder);
        SetTextColor(dc, !enabled ? kDisabled : primary ? RGB(255, 255, 255) : kText);
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
        if (high_contrast()) return;
        ListView_SetBkColor(child, kCard);
        ListView_SetTextBkColor(child, kCard);
        ListView_SetTextColor(child, kText);
    } else if (!lstrcmpiW(name, PROGRESS_CLASSW)) {
        if (high_contrast()) return;
        SetWindowTheme(child, L"", L"");
        SendMessageW(child, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(kRed));
        SendMessageW(child, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kControlPressed));
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
        const COLORREF title = caption == Caption::red ? kRed : kBackground;
        const COLORREF border = caption == Caption::red ? kRed : kBorder;
        const COLORREF text = caption == Caption::red ? RGB(255, 255, 255) : kText;
        DwmSetWindowAttribute(window, kCaptionColorAttribute, &title, sizeof(title));
        DwmSetWindowAttribute(window, kBorderColorAttribute, &border, sizeof(border));
        DwmSetWindowAttribute(window, kTextColorAttribute, &text, sizeof(text));
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
            place(choices, margin, y, inner, px(210));
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
                FillRect(dc, &bounds, brush(kBackground));
                bounds.bottom = px(4);
                FillRect(dc, &bounds, brush(kRed));
                return TRUE;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLORLISTBOX:
                if (const auto color = control_color(msg, wp, lp, kBackground)) {
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
            LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT, kChoicesId, WS_EX_CLIENTEDGE);
        for (const auto& choice : spec.choices) {
            if (SendMessageW(state.choices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.c_str())) < 0) {
                DestroyWindow(window);
                return spec.cancel_id;
            }
        }
        for (int index : spec.selected) SendMessageW(state.choices, LB_SETSEL, TRUE, index);
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
        SendMessageW(state.choices, LB_SETHORIZONTALEXTENT, extent + state.px(12), 0);
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
        if (state.choices) {
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
