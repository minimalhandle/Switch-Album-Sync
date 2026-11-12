#pragma once

#include "nso_album_sync/appearance.hpp"
#include "nso_album_sync/windows_compat.hpp"

#include <functional>
#include <string>
#include <vector>

namespace nso::theme {

// Neutral surfaces with Nintendo red used only as an accent: primary actions,
// active toggles, progress and small brand marks. Light mode uses the brand
// red (#E60012); dark mode uses a slightly softer red that doesn't glow on
// charcoal. High-contrast mode always falls back to system colours.
constexpr COLORREF kPreview = RGB(20, 21, 24);

struct Palette {
    COLORREF background, card, field, border, card_border;
    COLORREF control, control_hot, control_pressed, text, muted, disabled;
    COLORREF track, accent, accent_hot, accent_pressed;
};

const Palette& palette();
void set_appearance(Appearance appearance);
bool high_contrast();

// Initialize the graphics used by custom controls.
void init_process();

HBRUSH brush(COLORREF color);

// Colours the caption to match the window and themes existing child controls.
void apply(HWND window);

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
    bool single_choice = false;
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
