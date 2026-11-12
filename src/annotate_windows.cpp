#include "annotate_windows.hpp"

#include "album_tools_windows.hpp"
#include "theme_windows.hpp"

#include <gdiplus.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr wchar_t kEditorClass[] = L"SwitchAlbumSyncEditor";
constexpr std::size_t kUndoLimit = 15;
enum Command { CropButton = 4001, BlurButton, TextButton, UndoButton, SaveButton, CopyButton };

using Bitmap = Gdiplus::Bitmap;

std::unique_ptr<Bitmap> copy_of(Bitmap& image, const Gdiplus::Rect& area) {
    return std::unique_ptr<Bitmap>(image.Clone(area, PixelFormat32bppARGB));
}

bool png_encoder(CLSID& clsid) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return false;
    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (std::wstring(encoders[i].MimeType) == L"image/png") {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

struct Editor {
    HWND window = nullptr;
    std::filesystem::path source;
    std::function<void(const std::filesystem::path&)> saved;
    std::unique_ptr<Bitmap> image;
    std::vector<std::unique_ptr<Bitmap>> undo;
    std::vector<HWND> buttons;
    HWND hint = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool dragging = false;
    POINT drag_from{}, drag_to{};
    Gdiplus::Rect selection;  // In image pixels; empty when nothing is selected
    // Where the image is drawn in the window.
    double scale = 1;
    int image_x = 0, image_y = 0;

    ~Editor() {
        if (font) DeleteObject(font);
    }
    int px(int units) const { return MulDiv(units, static_cast<int>(dpi), 96); }
    int toolbar_height() const { return px(52); }

    void say(const std::wstring& text) {
        SetWindowTextW(hint, text.c_str());
    }

    void create_controls() {
        const auto add = [&](int id, const wchar_t* label) {
            buttons.push_back(CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr));
        };
        add(CropButton, L"Crop");
        add(BlurButton, L"Blur area");
        add(TextButton, L"Add text…");
        add(UndoButton, L"Undo");
        add(SaveButton, L"Save copy…");
        add(CopyButton, L"Copy image");
        hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS,
            0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        fonts();
        theme::apply(window);
        theme::set_primary(buttons[4]);
        say(L"Drag over the picture to select an area. The original file is never changed.");
        refresh_buttons();
    }

    void fonts() {
        if (font) DeleteObject(font);
        font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (const auto button : buttons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    void layout() {
        RECT client{};
        GetClientRect(window, &client);
        const int widths[] = {70, 92, 98, 70, 108, 108};
        int x = px(14);
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            MoveWindow(buttons[i], x, px(11), px(widths[i]), px(30), TRUE);
            x += px(widths[i] + 8);
        }
        MoveWindow(hint, x + px(6), px(17), std::max(0, static_cast<int>(client.right) - x - px(20)), px(22), TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }

    void refresh_buttons() {
        const bool selected = !selection.IsEmptyArea();
        EnableWindow(buttons[0], selected);
        EnableWindow(buttons[1], selected);
        EnableWindow(buttons[3], !undo.empty());
    }

    RECT canvas() const {
        RECT client{};
        GetClientRect(window, &client);
        client.top = toolbar_height();
        return client;
    }

    void fit() {
        const auto area = canvas();
        const int margin = px(14);
        const double width = std::max(1L, area.right - area.left - 2 * margin);
        const double height = std::max(1L, area.bottom - area.top - 2 * margin);
        scale = std::min(width / image->GetWidth(), height / image->GetHeight());
        image_x = area.left + margin + static_cast<int>((width - image->GetWidth() * scale) / 2);
        image_y = area.top + margin + static_cast<int>((height - image->GetHeight() * scale) / 2);
    }

    POINT to_image(POINT point) const {
        const auto clamp = [](double value, UINT limit) {
            return static_cast<LONG>(std::clamp(value, 0.0, static_cast<double>(limit)));
        };
        return {clamp((point.x - image_x) / scale, image->GetWidth()), clamp((point.y - image_y) / scale, image->GetHeight())};
    }

    void paint() {
        PAINTSTRUCT paint{};
        const HDC target = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP surface = CreateCompatibleBitmap(target, client.right, client.bottom);
        const auto previous = SelectObject(dc, surface);
        FillRect(dc, &client, theme::brush(theme::palette().background));
        auto area = canvas();
        FillRect(dc, &area, theme::brush(theme::kPreview));
        fit();
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(image.get(), image_x, image_y,
                static_cast<INT>(image->GetWidth() * scale), static_cast<INT>(image->GetHeight() * scale));
            Gdiplus::Rect shown;
            if (dragging) {
                shown = Gdiplus::Rect(std::min(drag_from.x, drag_to.x), std::min(drag_from.y, drag_to.y),
                    std::abs(drag_to.x - drag_from.x), std::abs(drag_to.y - drag_from.y));
            } else if (!selection.IsEmptyArea()) {
                shown = Gdiplus::Rect(image_x + static_cast<INT>(selection.X * scale), image_y + static_cast<INT>(selection.Y * scale),
                    static_cast<INT>(selection.Width * scale), static_cast<INT>(selection.Height * scale));
            }
            if (!shown.IsEmptyArea()) {
                Gdiplus::Pen outline(Gdiplus::Color(255, 230, 0, 18), static_cast<Gdiplus::REAL>(px(2)));
                outline.SetDashStyle(Gdiplus::DashStyleDash);
                graphics.DrawRectangle(&outline, shown);
            }
        }
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(surface);
        DeleteDC(dc);
        EndPaint(window, &paint);
    }

    void remember() {
        undo.push_back(copy_of(*image, Gdiplus::Rect(0, 0, image->GetWidth(), image->GetHeight())));
        if (undo.size() > kUndoLimit) undo.erase(undo.begin());
    }

    void changed(const std::wstring& message) {
        selection = {};
        refresh_buttons();
        say(message);
        InvalidateRect(window, nullptr, FALSE);
    }

    void crop() {
        if (selection.IsEmptyArea()) return;
        remember();
        image = copy_of(*image, selection);
        changed(L"Cropped. Save a copy when you're done.");
    }

    void blur() {
        if (selection.IsEmptyArea()) return;
        remember();
        // Large blocks make text such as usernames unreadable, and unlike a
        // soft blur they can't be reversed.
        const int block = std::max(8, std::max(selection.Width, selection.Height) / 12);
        Gdiplus::BitmapData data{};
        if (image->LockBits(&selection, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                            PixelFormat32bppARGB, &data) != Gdiplus::Ok) return;
        for (int top = 0; top < selection.Height; top += block) {
            for (int left = 0; left < selection.Width; left += block) {
                const int bottom = std::min(selection.Height, top + block), right = std::min(selection.Width, left + block);
                unsigned long long sum[4]{};
                for (int y = top; y < bottom; ++y) {
                    const auto* row = static_cast<BYTE*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
                    for (int x = left; x < right; ++x) for (int c = 0; c < 4; ++c) sum[c] += row[x * 4 + c];
                }
                const auto count = static_cast<unsigned long long>((bottom - top) * (right - left));
                for (int y = top; y < bottom; ++y) {
                    auto* row = static_cast<BYTE*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
                    for (int x = left; x < right; ++x) for (int c = 0; c < 4; ++c) row[x * 4 + c] = static_cast<BYTE>(sum[c] / count);
                }
            }
        }
        image->UnlockBits(&data);
        changed(L"Area blurred.");
    }

    void add_text() {
        theme::Dialog dialog;
        dialog.title = L"Add text";
        dialog.heading = L"Add text";
        dialog.text = selection.IsEmptyArea() ? L"The text goes in the top-left corner." :
                                                L"The text goes at the top-left of the selected area.";
        dialog.field = true;
        dialog.field_cue = L"Text";
        dialog.buttons = {{IDOK, L"Add", true}, {IDCANCEL, L"Cancel"}};
        if (theme::run_dialog(window, dialog) != IDOK || dialog.field_value.empty()) return;
        remember();
        const auto size = static_cast<Gdiplus::REAL>(std::max(16U, image->GetHeight() / 16));
        const Gdiplus::PointF origin = selection.IsEmptyArea()
            ? Gdiplus::PointF(image->GetWidth() * 0.03f, image->GetHeight() * 0.03f)
            : Gdiplus::PointF(static_cast<Gdiplus::REAL>(selection.X), static_cast<Gdiplus::REAL>(selection.Y));
        Gdiplus::Graphics graphics(image.get());
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::GraphicsPath path;
        path.AddString(dialog.field_value.c_str(), -1, &family, Gdiplus::FontStyleBold, size, origin, nullptr);
        Gdiplus::Pen outline(Gdiplus::Color(255, 0, 0, 0), size / 7);
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawPath(&outline, &path);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
        graphics.FillPath(&fill, &path);
        changed(L"Text added.");
    }

    void step_back() {
        if (undo.empty()) return;
        image = std::move(undo.back());
        undo.pop_back();
        changed(L"Undone.");
    }

    void save_copy() {
        const auto suggested = source.parent_path() / (source.stem().wstring() + L"_edited.png");
        const auto target = pick_save_file(window, L"Save a copy", suggested, L"PNG image", L"*.png");
        if (target.empty()) return;
        std::error_code error;
        if (std::filesystem::equivalent(target, source, error)) {
            say(L"Choose a different name; the original is never replaced.");
            return;
        }
        CLSID png{};
        if (!png_encoder(png) || image->Save(target.c_str(), &png, nullptr) != Gdiplus::Ok) {
            say(L"Could not save the copy.");
            return;
        }
        say(L"Saved " + target.filename().wstring());
        if (saved) saved(target);
    }

    void copy_image() {
        HBITMAP bitmap = nullptr;
        if (image->GetHBITMAP(Gdiplus::Color(255, 255, 255), &bitmap) != Gdiplus::Ok) {
            say(L"Could not copy the image.");
            return;
        }
        bool copied = false;
        if (OpenClipboard(window)) {
            copied = EmptyClipboard() && SetClipboardData(CF_BITMAP, bitmap);
            CloseClipboard();
        }
        if (!copied) DeleteObject(bitmap);
        say(copied ? L"Image copied. Paste it into a chat or document." : L"Could not copy the image.");
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                dpi = GetDpiForWindow(window);
                create_controls();
                return 0;
            case WM_SIZE: layout(); return 0;
            case WM_DPICHANGED: {
                dpi = HIWORD(wp);
                const auto& bounds = *reinterpret_cast<const RECT*>(lp);
                SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                             bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
                fonts();
                layout();
                return 0;
            }
            case WM_GETMINMAXINFO:
                reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(760), px(420)};
                return 0;
            case WM_ERASEBKGND: return TRUE;
            case WM_PAINT: paint(); return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
                if (const auto brush = theme::control_color(msg, wp, lp, theme::palette().background)) {
                    if (reinterpret_cast<HWND>(lp) == hint) SetTextColor(reinterpret_cast<HDC>(wp), theme::palette().muted);
                    return brush;
                }
                break;
            case WM_LBUTTONDOWN: {
                const POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                if (point.y < toolbar_height()) break;
                dragging = true;
                drag_from = drag_to = point;
                SetCapture(window);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (dragging) {
                    drag_to = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP:
                if (dragging) {
                    dragging = false;
                    ReleaseCapture();
                    const auto a = to_image(drag_from), b = to_image(drag_to);
                    selection = Gdiplus::Rect(std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y));
                    if (selection.Width < 4 || selection.Height < 4) selection = {};
                    refresh_buttons();
                    say(selection.IsEmptyArea() ? L"Drag over the picture to select an area."
                        : L"Area selected: " + std::to_wstring(selection.Width) + L" × " + std::to_wstring(selection.Height));
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) changed(L"Selection cleared.");
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case CropButton: crop(); break;
                    case BlurButton: blur(); break;
                    case TextButton: add_text(); break;
                    case UndoButton: step_back(); break;
                    case SaveButton: save_copy(); break;
                    case CopyButton: copy_image(); break;
                }
                return 0;
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Editor*>(reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == WM_NCDESTROY && self) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete self;
            return DefWindowProcW(window, msg, wp, lp);
        }
        return self ? self->message(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
    }
};

}  // namespace

void open_screenshot_editor(HWND owner, const std::filesystem::path& screenshot,
                            std::function<void(const std::filesystem::path&)> saved) {
    theme::init_process();
    auto editor = std::make_unique<Editor>();
    editor->source = screenshot;
    editor->saved = std::move(saved);
    {
        // Load, then copy, so the screenshot file isn't kept open.
        Bitmap original(screenshot.c_str());
        if (original.GetLastStatus() != Gdiplus::Ok || !original.GetWidth() || !original.GetHeight()) {
            theme::message_box(owner, L"This screenshot couldn't be opened.", L"Edit screenshot", MB_OK);
            return;
        }
        editor->image = copy_of(original, Gdiplus::Rect(0, 0, original.GetWidth(), original.GetHeight()));
    }
    if (!editor->image) return;

    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = Editor::procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    type.lpszClassName = kEditorClass;
    RegisterClassW(&type);

    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& work = monitor.rcWork;
    const int width = (work.right - work.left) * 4 / 5, height = (work.bottom - work.top) * 4 / 5;
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto* raw = editor.release();
    const HWND window = CreateWindowExW(0, kEditorClass, (L"Edit screenshot · " + screenshot.filename().wstring()).c_str(),
        WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, raw);
    if (previous) SetThreadDpiAwarenessContext(previous);
    // If creation fails part-way, WM_NCDESTROY may already have freed the
    // editor, so it isn't deleted here; a failed window leaks one small object.
    if (!window) return;
    ShowWindow(window, SW_SHOW);
}

}  // namespace nso
