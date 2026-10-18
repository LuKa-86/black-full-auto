// black-config.exe: the controller layout of the BLACK PC port, which does not fit the in-game menus, written to
// black.ini next to it (the folder black.exe keeps its files in): pad_<action> lines with up to three pad buttons per
// action, each pressed, tapped or held (input/action_list.h has the actions and the defaults), the layout switch
// (the port's or the game's own), southpaw sticks and the vibration strength. Every other setting is in the game's
// PC SETTINGS pages. Lines the tool does not know are kept as they are. The game reads black.ini when it starts and
// writes it when its settings change and when it closes, so the tool asks for the game to be closed first.
#include "input/action_list.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace bindings;

namespace {

// ---- black.ini ----

std::wstring g_dir; // this program's folder
std::vector<std::string> g_lines;

std::wstring ini_path() { return g_dir + L"\\black.ini"; }

void ini_load()
{
    g_lines.clear();
    FILE *f = nullptr;
    if (_wfopen_s(&f, ini_path().c_str(), L"r") || !f) return;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        std::string s = buf;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        g_lines.push_back(s);
    }
    fclose(f);
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

int find_line(const std::string &key)
{
    for (size_t i = 0; i < g_lines.size(); i++) {
        const std::string &l = g_lines[i];
        size_t eq = l.find('=');
        if (!l.empty() && l[0] != '#' && eq != std::string::npos && trim(l.substr(0, eq)) == key) return (int)i;
    }
    return -1;
}

std::string ini_get(const std::string &key, const std::string &def)
{
    int i = find_line(key);
    if (i < 0) return def;
    return trim(g_lines[i].substr(g_lines[i].find('=') + 1));
}

int ini_int(const std::string &key, int def)
{
    std::string v = ini_get(key, "");
    return v.empty() ? def : atoi(v.c_str());
}

void ini_set(const std::string &key, const std::string &value)
{
    std::string line = key + " = " + value;
    int i = find_line(key);
    if (i >= 0) g_lines[i] = line;
    else g_lines.push_back(line);
}

bool ini_save()
{
    if (g_lines.empty() || g_lines[0].rfind("#", 0) != 0)
        g_lines.insert(g_lines.begin(), "# BLACK PC port settings (written by PC SETTINGS and on exit; command-line flags override them)");
    FILE *f = nullptr;
    if (_wfopen_s(&f, ini_path().c_str(), L"w") || !f) return false;
    for (const std::string &l : g_lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    return true;
}

// ---- controller brand ----
// The game reads XInput pads, which do not say what they are, so the brand comes from the HID devices Windows knows
// (raw input device list, vendor id): Sony = PlayStation, Nintendo = Switch, anything else = Xbox. PlayStation and
// Nintendo pads reach the game through Steam Input, DS4Windows and similar, which present them as XInput pads with
// the buttons in the same places, so their names here follow the position (Cross = A, Nintendo's B = Xbox A, ...).
enum Brand { kXbox, kPlayStation, kNintendo };
const wchar_t *const kBrandName[] = {L"Xbox", L"PlayStation", L"Nintendo"};
const wchar_t *const kNames[3][kPadButtons] = {
    {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"L3 (left stick)", L"R3 (right stick)", L"START", L"BACK",
     L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right"},
    {L"Cross", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"L3 (left stick)", L"R3 (right stick)",
     L"OPTIONS", L"SHARE / CREATE", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right"},
    {L"B", L"A", L"Y", L"X", L"L", L"R", L"ZL", L"ZR", L"Left stick press", L"Right stick press", L"+ (plus)",
     L"- (minus)", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right"},
};

Brand detect_brand(bool *found)
{
    *found = false;
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return kXbox;
    std::vector<RAWINPUTDEVICELIST> list(n);
    n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    if (n == (UINT)-1) return kXbox;
    Brand best = kXbox;
    for (UINT i = 0; i < n; i++) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info = {sizeof info};
        UINT size = sizeof info;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) continue;
        const RID_DEVICE_INFO_HID &h = info.hid;
        if (h.usUsagePage != 1 || (h.usUsage != 4 && h.usUsage != 5)) continue; // joystick / gamepad
        *found = true;
        if (h.dwVendorId == 0x054C) return kPlayStation; // Sony
        if (h.dwVendorId == 0x057E) best = kNintendo;    // Nintendo
    }
    return best;
}

// ---- window ----

enum : int { ID_SAVE = 101, ID_PLAY, ID_CLOSE, ID_DEFAULTS, ID_CLEAR, ID_LAYOUT, ID_STICKS, ID_RUMBLE, ID_NAMES, ID_PAD0 = 1000 };
constexpr int kRumble[] = {0, 25, 50, 75, 100};
constexpr UINT_PTR kDetectTimer = 1;

HWND g_wnd, g_status, g_note, g_detected;
HFONT g_font, g_bold;
float g_dpi = 1.0f;
HWND g_pad[kActionCount][kPadSlots], g_action[kActionCount];
HWND g_layout, g_sticks, g_rumble, g_names;
HWND g_head[6], g_top_label[4], g_buttons[5]; // column headers; Layout / Sticks / Vibration / Button names labels
Brand g_brand = kXbox;      // whose names the lists show
Brand g_detected_brand = kXbox;
bool g_pad_found;

int S(int v) { return (int)(v * g_dpi + 0.5f); }

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int id = 0)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

HWND combo(int id, std::initializer_list<const wchar_t *> items)
{
    HWND c = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
    for (const wchar_t *s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
    return c;
}

HWND label(const wchar_t *text, bool bold = false)
{
    HWND l = add(L"STATIC", text, SS_LEFT);
    if (bold) SendMessageW(l, WM_SETFONT, (WPARAM)g_bold, TRUE);
    return l;
}

void place(HWND c, int x, int y, int w, int h) { MoveWindow(c, S(x), S(y), S(w), S(h), FALSE); }

int pad_index(const PadBind &b) { return b.button < 0 ? 0 : 1 + b.button * 3 + b.mode; }

// The pad lists' entries in the current brand's names, keeping each list's choice.
void fill_pad_lists()
{
    std::vector<std::wstring> items = {L"-"};
    for (int b = 0; b < kPadButtons; b++) {
        items.push_back(kNames[g_brand][b]);
        items.push_back(std::wstring(kNames[g_brand][b]) + L" (tap)");
        items.push_back(std::wstring(kNames[g_brand][b]) + L" (hold)");
    }
    for (int a = 0; a < kActionCount; a++)
        for (int s = 0; s < kPadSlots; s++) {
            HWND c = g_pad[a][s];
            if (!c) continue;
            int sel = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
            SendMessageW(c, WM_SETREDRAW, FALSE, 0);
            SendMessageW(c, CB_RESETCONTENT, 0, 0);
            for (const std::wstring &it : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)it.c_str());
            SendMessageW(c, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
            SendMessageW(c, CB_SETDROPPEDWIDTH, S(190), 0); // the open list is wider than the box
            SendMessageW(c, WM_SETREDRAW, TRUE, 0);
        }
}

void show_detected()
{
    std::wstring s = g_pad_found ? std::wstring(L"Controller found: ") + kBrandName[g_detected_brand]
                                 : std::wstring(L"No controller found (names: ") + kBrandName[g_brand] + L")";
    SetWindowTextW(g_detected, s.c_str());
}

// "Auto" follows the detected brand; checked again every 2 s so plugging a pad in updates the names.
void update_brand()
{
    g_detected_brand = detect_brand(&g_pad_found);
    int choice = (int)SendMessageW(g_names, CB_GETCURSEL, 0, 0);
    Brand want = choice <= 0 ? g_detected_brand : (Brand)(choice - 1);
    if (want != g_brand) {
        g_brand = want;
        fill_pad_lists();
        InvalidateRect(g_wnd, nullptr, TRUE);
    }
    show_detected();
}

void set_pad_row(int a, const std::string &value)
{
    PadBind b[kPadSlots];
    if (!parse_pad(value, b)) parse_pad(pad_default(a), b);
    for (int s = 0; s < kPadSlots; s++) SendMessageW(g_pad[a][s], CB_SETCURSEL, pad_index(b[s]), 0);
}

std::string pad_row(int a)
{
    PadBind b[kPadSlots];
    for (int s = 0; s < kPadSlots; s++) {
        int i = (int)SendMessageW(g_pad[a][s], CB_GETCURSEL, 0, 0);
        if (i > 0) b[s].button = (int8_t)((i - 1) / 3), b[s].mode = (PadMode)((i - 1) % 3);
    }
    return pad_text(b);
}

void load_values()
{
    ini_load();
    SendMessageW(g_layout, CB_SETCURSEL, ini_get("pad_layout", "cod") == "game" ? 1 : 0, 0);
    SendMessageW(g_sticks, CB_SETCURSEL, ini_get("pad_sticks", "normal") == "southpaw" ? 1 : 0, 0);
    int r = ini_int("rumble", 100), ri = 4;
    for (int i = 0; i < 5; i++)
        if (r >= kRumble[i]) ri = i;
    SendMessageW(g_rumble, CB_SETCURSEL, ri, 0);
    for (int a = 0; a < kActionCount; a++)
        if (g_pad[a][0]) set_pad_row(a, ini_get(std::string("pad_") + kActions[a].ini, pad_default(a)));
}

void status(const wchar_t *text) { SetWindowTextW(g_status, text); }

bool save_values()
{
    if (FindWindowW(L"BlackLoaderWindow", nullptr)) {
        MessageBoxW(g_wnd, L"BLACK is running. Close the game first: it writes black.ini when it closes, which would undo "
                           L"these changes.", L"BLACK controller", MB_OK | MB_ICONWARNING);
        return false;
    }
    ini_load(); // keep whatever the game wrote since this window opened
    ini_set("pad_layout", SendMessageW(g_layout, CB_GETCURSEL, 0, 0) == 1 ? "game" : "cod");
    ini_set("pad_sticks", SendMessageW(g_sticks, CB_GETCURSEL, 0, 0) == 1 ? "southpaw" : "normal");
    ini_set("rumble", std::to_string(kRumble[std::clamp((int)SendMessageW(g_rumble, CB_GETCURSEL, 0, 0), 0, 4)]));
    for (int a = 0; a < kActionCount; a++)
        if (g_pad[a][0]) ini_set(std::string("pad_") + kActions[a].ini, pad_row(a));
    if (!ini_save()) {
        MessageBoxW(g_wnd, (L"Could not write " + ini_path()).c_str(), L"BLACK controller", MB_OK | MB_ICONERROR);
        return false;
    }
    status(L"Saved.");
    return true;
}

int rows() { return (kActionCount - 1 + 1) / 2; } // two columns; the cheat menu stays keyboard-only

void build()
{
    g_top_label[0] = label(L"Layout");
    g_layout = combo(ID_LAYOUT, {L"The port's layout (set below)", L"The game's own layouts (its controls menu)"});
    g_top_label[1] = label(L"Sticks");
    g_sticks = combo(ID_STICKS, {L"Left moves, right looks", L"Southpaw (swapped)"});
    g_top_label[2] = label(L"Vibration");
    g_rumble = combo(ID_RUMBLE, {L"Off", L"25%", L"50%", L"75%", L"100%"});
    g_top_label[3] = label(L"Button names");
    g_names = combo(ID_NAMES, {L"Auto (the connected controller)", L"Xbox", L"PlayStation", L"Nintendo"});
    SendMessageW(g_names, CB_SETCURSEL, 0, 0);
    g_detected = label(L"");
    g_note = label(L"Up to three buttons per action. Tap: released within 0.3 s. Hold: held 0.3 s or longer. A button may "
                   L"be used by several actions. PlayStation and Nintendo controllers need Steam Input, DS4Windows or a "
                   L"similar XInput driver; the names follow the button positions. Layout, sticks and vibration are also "
                   L"in the game (PC SETTINGS > CONTROLS), as is everything else.");
    for (int c = 0; c < 2; c++) {
        g_head[c * 2] = label(L"Action", true);
        g_head[c * 2 + 1] = label(L"Buttons", true);
    }
    for (int a = 0; a < kActionCount; a++) {
        if (strcmp(kActions[a].ini, "cheat_menu") == 0) continue;
        g_action[a] = label(kActions[a].name);
        for (int s = 0; s < kPadSlots; s++)
            g_pad[a][s] = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PAD0 + a * kPadSlots + s);
    }
    g_buttons[0] = add(L"BUTTON", L"Call of Duty layout", BS_PUSHBUTTON | WS_TABSTOP, ID_DEFAULTS);
    g_buttons[1] = add(L"BUTTON", L"Clear all", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR);
    g_buttons[2] = add(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE);
    g_buttons[3] = add(L"BUTTON", L"Save and play", BS_PUSHBUTTON | WS_TABSTOP, ID_PLAY);
    g_buttons[4] = add(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    g_status = label((L"Settings file: " + ini_path()).c_str());
    SetWindowLongW(g_status, GWL_STYLE, GetWindowLongW(g_status, GWL_STYLE) | SS_PATHELLIPSIS);
    fill_pad_lists();
}

constexpr int kMinW = 1040, kRowH = 30;
int content_height() { return 16 + 36 + 34 + 52 + 24 + rows() * kRowH + 10 + 48 + 30 + 16; }

// Everything placed for a client area w (in 96-dpi units): two columns of actions sharing the width, each with the
// action's name and three lists of equal width.
void layout(int w)
{
    w = std::max(w, kMinW);
    int m = 24, y = 16;
    place(g_top_label[0], m, y + 3, 50, 20);
    place(g_layout, m + 52, y, 300, 320);
    place(g_top_label[1], m + 370, y + 3, 46, 20);
    place(g_sticks, m + 418, y, 200, 320);
    place(g_top_label[2], m + 636, y + 3, 66, 20);
    place(g_rumble, m + 704, y, 90, 320);
    y += 36;
    place(g_top_label[3], m, y + 3, 90, 20);
    place(g_names, m + 92, y, 260, 320);
    place(g_detected, m + 370, y + 3, w - m - 370 - m, 20);
    y += 34;
    place(g_note, m, y, w - 2 * m, 48);
    y += 52;
    int col_w = (w - 2 * m - 24) / 2, name_w = std::min(190, col_w / 3), list_w = (col_w - name_w - 3 * 6) / 3;
    for (int c = 0; c < 2; c++) {
        int x = m + c * (col_w + 24);
        place(g_head[c * 2], x, y + 3, name_w, 20);
        place(g_head[c * 2 + 1], x + name_w + 6, y + 3, list_w, 20);
    }
    y += 24;
    for (int a = 0, n = 0; a < kActionCount; a++) {
        if (!g_action[a]) continue;
        int col = n / rows(), row = n % rows();
        n++;
        int x = m + col * (col_w + 24), yy = y + row * kRowH;
        place(g_action[a], x, yy + 3, name_w, 20);
        for (int s = 0; s < kPadSlots; s++) place(g_pad[a][s], x + name_w + 6 + s * (list_w + 6), yy, list_w, 360);
    }
    int by = y + rows() * kRowH + 10;
    place(g_buttons[0], m, by, 170, 28);
    place(g_buttons[1], m + 180, by, 110, 28);
    by += 48;
    place(g_status, m, by + 6, w - 2 * m - 350, 22);
    place(g_buttons[2], w - m - 330, by, 100, 30);
    place(g_buttons[3], w - m - 220, by, 120, 30);
    place(g_buttons[4], w - m - 90, by, 90, 30);
    InvalidateRect(g_wnd, nullptr, TRUE);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_layout) layout((int)(LOWORD(lp) / g_dpi));
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r = {0, 0, S(kMinW), S(content_height())};
        AdjustWindowRect(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE);
        ((MINMAXINFO *)lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_TIMER:
        if (wp == kDetectTimer) update_brand();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SAVE: save_values(); return 0;
        case ID_PLAY:
            if (save_values()) {
                ShellExecuteW(h, L"open", (g_dir + L"\\black.exe").c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);
                DestroyWindow(h);
            }
            return 0;
        case ID_CLOSE: DestroyWindow(h); return 0;
        case ID_DEFAULTS:
            for (int a = 0; a < kActionCount; a++)
                if (g_pad[a][0]) set_pad_row(a, pad_default(a));
            status(L"Call of Duty layout set (not saved yet).");
            return 0;
        case ID_CLEAR:
            for (int a = 0; a < kActionCount; a++)
                for (int s = 0; s < kPadSlots && g_pad[a][0]; s++) SendMessageW(g_pad[a][s], CB_SETCURSEL, 0, 0);
            status(L"All buttons cleared (not saved yet).");
            return 0;
        case ID_NAMES:
            if (HIWORD(wp) == CBN_SELCHANGE) update_brand(); // only the names: nothing to save
            return 0;
        }
        if (HIWORD(wp) == CBN_SELCHANGE) status(L"Changed (not saved yet).");
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L"\\/"));
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"BlackConfigWindow";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN; // resizable; the lists widen with the window
    RECT r = {0, 0, S(1180), S(content_height())};
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"BLACK - controller layout", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    build();
    load_values();
    update_brand();
    RECT cr;
    GetClientRect(g_wnd, &cr);
    layout((int)(cr.right / g_dpi));
    SetTimer(g_wnd, kDetectTimer, 2000, nullptr);
    ShowWindow(g_wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
