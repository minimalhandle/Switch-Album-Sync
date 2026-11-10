#pragma once

#include "nso_album_sync/windows_compat.hpp"

#include <functional>
#include <string>
#include <vector>

namespace nso::theme {

// Nintendo brand red (#E60012) on a neutral light base. High-contrast mode
// always falls back to system colours.
constexpr COLORREF kRed = RGB(230, 0, 18);
constexpr COLORREF kRedHot = RGB(215, 0, 17);
constexpr COLORREF kRedPressed = RGB(184, 0, 14);
constexpr COLORREF kBackground = RGB(244, 245, 246);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kField = RGB(244, 245, 246);
constexpr COLORREF kBorder = RGB(203, 207, 211);
constexpr COLORREF kCardBorder = RGB(222, 225, 228);
constexpr COLORREF kControl = RGB(250, 250, 251);
constexpr COLORREF kControlHot = RGB(235, 237, 239);
constexpr COLORREF kControlPressed = RGB(222, 225, 228);
constexpr COLORREF kText = RGB(31, 34, 38);
constexpr COLORREF kMuted = RGB(83, 89, 96);
constexpr COLORREF kDisabled = RGB(109, 113, 117);
constexpr COLORREF kPreview = RGB(24, 26, 28);

bool high_contrast();

// Initialize the graphics used by custom controls.
void init_process();

HBRUSH brush(COLORREF color);

enum class Caption { neutral, red };

// Apply the caption and theme existing child controls.
void apply(HWND window, Caption caption = Caption::neutral);

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

// A modal, per-monitor DPI aware dialog in the Switch Album Sync theme: a heading,
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
    std::wstring checkbox_label;
    bool checked = false;
    std::vector<std::wstring> choices;
    std::vector<int> selected;
    int width = 460;
    std::function<void(int)> on_command;
    std::function<bool()> completed;
};

// Returns the id of the button that closed the dialog, or cancel_id. When
// `field` is set, field_value holds the edited text afterwards.
int run_dialog(HWND owner, Dialog& dialog);

// Themed replacement for MessageBoxW. Supports MB_OK, MB_OKCANCEL and
// MB_YESNO, plus MB_DEFBUTTON2 for the initially focused button.
int message_box(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags);

}  // namespace nso::theme
