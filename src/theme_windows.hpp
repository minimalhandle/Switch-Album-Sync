#pragma once

#include "nso_album_sync/windows_compat.hpp"

#include <functional>
#include <string>
#include <vector>

namespace nso::theme {

// Nintendo brand red (#E60012) on a dark charcoal base. High-contrast mode
// always falls back to system colours.
constexpr COLORREF kRed = RGB(230, 0, 18);
constexpr COLORREF kRedHot = RGB(255, 38, 56);
constexpr COLORREF kRedPressed = RGB(184, 0, 14);
constexpr COLORREF kRedDeep = RGB(150, 0, 12);
constexpr COLORREF kBackground = RGB(24, 24, 27);
constexpr COLORREF kCard = RGB(36, 36, 40);
constexpr COLORREF kField = RGB(20, 20, 23);
constexpr COLORREF kBorder = RGB(62, 62, 68);
constexpr COLORREF kControl = RGB(56, 56, 62);
constexpr COLORREF kControlHot = RGB(70, 70, 77);
constexpr COLORREF kControlPressed = RGB(46, 46, 51);
constexpr COLORREF kText = RGB(242, 242, 245);
constexpr COLORREF kMuted = RGB(168, 168, 176);
constexpr COLORREF kDisabled = RGB(108, 108, 116);
constexpr COLORREF kPreview = RGB(14, 14, 16);

bool high_contrast();

// Call once per process before any window or menu is created: opts the app
// into dark popup menus and scrollbars where Windows supports it.
void init_process();

HBRUSH brush(COLORREF color);

enum class Caption { dark, red };

// Dark caption, dark scrollbars and themed child controls. Call after the
// window's controls exist; safe to call again for new controls.
void apply(HWND window, Caption caption = Caption::dark);

// Marks a push button as the red primary action.
void set_primary(HWND button, bool primary = true);

// Handles WM_CTLCOLOR* for a themed window. Returns 0 when the message is
// not a colour message. `surface` is the brush behind the control.
LRESULT control_color(UINT msg, WPARAM wp, LPARAM lp, COLORREF surface);

void fill_round(HDC dc, const RECT& bounds, int radius, COLORREF fill, COLORREF border = CLR_INVALID);

struct DialogButton {
    int id = 0;
    std::wstring label;
    bool primary = false;
    // A button that does not close the dialog reports through on_command.
    bool closes = true;
};

// A modal, per-monitor DPI aware dialog in the CaptureDock theme: a heading,
// body text, an optional single-line field and a row of buttons.
struct Dialog {
    std::wstring title;
    std::wstring heading;
    std::wstring text;
    std::vector<DialogButton> buttons;
    int cancel_id = IDCANCEL;
    int focus_id = 0;
    bool field = false;
    std::wstring field_value;
    std::wstring field_cue;
    int width = 460;
    std::function<void(int)> on_command;
};

// Returns the id of the button that closed the dialog, or cancel_id. When
// `field` is set, field_value holds the edited text afterwards.
int run_dialog(HWND owner, Dialog& dialog);

// Themed replacement for MessageBoxW. Supports MB_OK, MB_OKCANCEL and
// MB_YESNO, plus MB_DEFBUTTON2 for the initially focused button.
int message_box(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags);

}  // namespace nso::theme
