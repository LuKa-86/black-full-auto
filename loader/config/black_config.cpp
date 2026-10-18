// black-config.exe: the controller layout of the BLACK PC port, which does not fit the in-game menus, written to
// black.ini next to it (the folder black.exe keeps its files in): pad_<action> lines with up to three pad buttons per
// action, each pressed, tapped or held (input/action_list.h has the actions and the defaults; kPresets below adds
// layouts modelled on other shooters), the layout switch
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
    {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Left Stick (L3)", L"Right Stick (R3)", L"Start", L"Back",
     L"D-Pad Up", L"D-Pad Down", L"D-Pad Left", L"D-Pad Right"},
    {L"Cross", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"Left Stick (L3)", L"Right Stick (R3)",
     L"Options", L"Share / Create", L"D-Pad Up", L"D-Pad Down", L"D-Pad Left", L"D-Pad Right"},
    {L"B", L"A", L"Y", L"X", L"L", L"R", L"ZL", L"ZR", L"Left Stick Press", L"Right Stick Press", L"Plus (+)",
     L"Minus (−)", L"D-Pad Up", L"D-Pad Down", L"D-Pad Left", L"D-Pad Right"},
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

// ---- layout presets ----
// Familiar layouts from other shooters, mapped onto BLACK's actions by where those games put the same thing (BLACK
// has no jump, so A is mostly free). Actions not listed get no button. "BLACK (original)" switches to the game's own
// controller layouts instead of filling the grid.
struct Preset {
    const wchar_t *name;
    std::vector<std::pair<const char *, const char *>> buttons; // empty: the game's own layouts
};
const std::vector<std::pair<const char *, const char *>> kCommon = {
    {"fire", "RT"}, {"aim", "LT"}, {"sprint", "L3"}, {"reload", "X.tap"}, {"use", "X.hold"}, {"next_weapon", "Y"},
    {"prev_weapon", "DRIGHT"}, {"firing_mode", "DUP"}, {"suppressor", "DLEFT"}, {"pause", "START"}, {"objectives", "BACK"},
};
const Preset kPresets[] = {
    {L"Call of Duty (default)", {{"grenade", "RB"}, {"melee", "R3"}, {"crouch", "B"}, {"health_pack", "LB,A,DDOWN"}}},
    {L"Battlefield", {{"grenade", "RB"}, {"melee", "R3"}, {"crouch", "B"}, {"health_pack", "LB,DDOWN"}}},
    {L"Halo Infinite", {{"grenade", "LB"}, {"melee", "B"}, {"crouch", "R3"}, {"health_pack", "RB,DDOWN"}}},
    {L"Destiny 2", {{"grenade", "LB"}, {"melee", "RB"}, {"crouch", "B"}, {"health_pack", "DDOWN"}}},
    {L"BLACK (original game layout)", {}},
};

// ---- window ----

enum : int { ID_SAVE = 101, ID_PLAY, ID_CLOSE, ID_DEFAULTS, ID_CLEAR, ID_LAYOUT, ID_STICKS, ID_RUMBLE, ID_NAMES, ID_PRESET,
             ID_PAD0 = 1000 };
constexpr int kRumble[] = {0, 25, 50, 75, 100};
constexpr UINT_PTR kDetectTimer = 1;
const wchar_t kTitle[] = L"BLACK: Full Auto – Controller Layout";
const wchar_t kNone[] = L"—";

HWND g_wnd, g_status, g_note, g_detected, g_hint, g_title, g_subtitle;
HWND g_group_settings, g_group_mapping;
HFONT g_font, g_bold, g_big;
float g_dpi = 1.0f;
HWND g_pad[kActionCount][kPadSlots], g_action[kActionCount];
HWND g_layout, g_sticks, g_rumble, g_names, g_preset, g_preset_label;
HWND g_head[8], g_top_label[4], g_buttons[5]; // column headers; the settings' labels; the push buttons
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

HWND label(const wchar_t *text, HFONT font = nullptr, DWORD align = SS_LEFT)
{
    HWND l = add(L"STATIC", text, align);
    if (font) SendMessageW(l, WM_SETFONT, (WPARAM)font, TRUE);
    return l;
}

void place(HWND c, int x, int y, int w, int h) { MoveWindow(c, S(x), S(y), S(w), S(h), FALSE); }

int pad_index(const PadBind &b) { return b.button < 0 ? 0 : 1 + b.button * 3 + b.mode; }

// The button lists in the current brand's names, keeping each list's choice.
void fill_pad_lists()
{
    std::vector<std::wstring> items = {kNone};
    for (int b = 0; b < kPadButtons; b++) {
        items.push_back(kNames[g_brand][b]);
        items.push_back(std::wstring(kNames[g_brand][b]) + L" (Tap)");
        items.push_back(std::wstring(kNames[g_brand][b]) + L" (Hold)");
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
    std::wstring s = g_pad_found ? std::wstring(L"Detected: ") + kBrandName[g_detected_brand] + L" controller"
                                 : std::wstring(L"No controller detected (showing ") + kBrandName[g_brand] + L" names)";
    SetWindowTextW(g_detected, s.c_str());
}

// "Automatic" follows the detected brand; checked again every 2 s so plugging a controller in updates the names.
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
        MessageBoxW(g_wnd, L"BLACK is currently running.\n\nPlease close the game first. It saves its settings when it "
                           L"exits, which would overwrite these changes.", kTitle, MB_OK | MB_ICONWARNING);
        return false;
    }
    ini_load(); // keep whatever the game wrote since this window opened
    ini_set("pad_layout", SendMessageW(g_layout, CB_GETCURSEL, 0, 0) == 1 ? "game" : "cod");
    ini_set("pad_sticks", SendMessageW(g_sticks, CB_GETCURSEL, 0, 0) == 1 ? "southpaw" : "normal");
    ini_set("rumble", std::to_string(kRumble[std::clamp((int)SendMessageW(g_rumble, CB_GETCURSEL, 0, 0), 0, 4)]));
    for (int a = 0; a < kActionCount; a++)
        if (g_pad[a][0]) ini_set(std::string("pad_") + kActions[a].ini, pad_row(a));
    if (!ini_save()) {
        MessageBoxW(g_wnd, L"The settings could not be saved. Check that black.ini in this folder is not read-only.",
                    kTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    status(L"Settings saved.");
    return true;
}

int rows() { return (kActionCount - 1 + 1) / 2; } // two columns; the cheat menu stays keyboard-only

void build()
{
    g_group_settings = add(L"BUTTON", L"Controller Settings", BS_GROUPBOX);
    g_group_mapping = add(L"BUTTON", L"Button Mapping", BS_GROUPBOX);
    g_title = label(L"Controller Layout", g_big);
    g_subtitle = label(L"Choose which controller buttons perform each action in BLACK: Full Auto.");
    g_top_label[0] = label(L"Layout:");
    g_layout = combo(ID_LAYOUT, {L"Custom (set below)", L"Original (in-game Controls menu)"});
    g_top_label[1] = label(L"Stick layout:");
    g_sticks = combo(ID_STICKS, {L"Standard (move left, look right)", L"Southpaw (sticks swapped)"});
    g_top_label[2] = label(L"Vibration:");
    g_rumble = combo(ID_RUMBLE, {L"Off", L"25%", L"50%", L"75%", L"100%"});
    g_top_label[3] = label(L"Button names:");
    g_names = combo(ID_NAMES, {L"Automatic", L"Xbox", L"PlayStation", L"Nintendo"});
    SendMessageW(g_names, CB_SETCURSEL, 0, 0);
    g_detected = label(L"");
    g_hint = label(L"Assign up to three buttons to each action. Tap: press and release within 0.3 seconds. Hold: keep it "
                   L"pressed for 0.3 seconds or longer. A button can be used for more than one action.");
    const wchar_t *heads[4] = {L"Action", L"Button 1", L"Button 2", L"Button 3"};
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 4; i++) g_head[c * 4 + i] = label(heads[i], g_bold);
    for (int a = 0; a < kActionCount; a++) {
        if (strcmp(kActions[a].ini, "cheat_menu") == 0) continue;
        g_action[a] = label(kActions[a].name);
        for (int s = 0; s < kPadSlots; s++)
            g_pad[a][s] = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PAD0 + a * kPadSlots + s);
    }
    g_preset_label = label(L"Preset:");
    g_preset = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PRESET);
    for (const Preset &p : kPresets) SendMessageW(g_preset, CB_ADDSTRING, 0, (LPARAM)p.name);
    SendMessageW(g_preset, CB_SETCURSEL, 0, 0);
    g_buttons[0] = add(L"BUTTON", L"Apply Preset", BS_PUSHBUTTON | WS_TABSTOP, ID_DEFAULTS);
    g_buttons[1] = add(L"BUTTON", L"Clear All", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR);
    g_note = label(L"PlayStation and Nintendo controllers need Steam Input, DS4Windows or a similar XInput driver. The "
                   L"layout, stick and vibration settings can also be changed in the game under PC Settings > Controls.");
    g_buttons[2] = add(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE);
    g_buttons[3] = add(L"BUTTON", L"Save && Play", BS_PUSHBUTTON | WS_TABSTOP, ID_PLAY);
    g_buttons[4] = add(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    g_status = label(L"Settings are saved to black.ini in this folder.");
    fill_pad_lists();
}

constexpr int kMinW = 1040, kRowH = 28, kSettingsY = 68, kSettingsH = 92, kMappingY = 172;
int mapping_height() { return 94 + rows() * kRowH + 8 + 28 + 14; } // hint, headers, rows, buttons, margin
int content_height() { return kMappingY + mapping_height() + 12 + 36 + 10 + 30 + 16; }

// Everything placed for a client area w (in 96-dpi units): two columns of actions sharing the width, each with the
// action's name and three lists of equal width.
void layout(int w)
{
    w = std::max(w, kMinW);
    int m = 20, in = m + 16, gw = w - 2 * m;
    place(g_title, m, 10, gw, 30);
    place(g_subtitle, m, 42, gw, 20);

    place(g_group_settings, m, kSettingsY, gw, kSettingsH);
    int y = kSettingsY + 24;
    place(g_top_label[0], in, y + 3, 92, 20);
    place(g_layout, in + 96, y, 300, 320);
    place(g_top_label[1], in + 416, y + 3, 92, 20);
    place(g_sticks, in + 510, y, 250, 320);
    place(g_top_label[2], in + 780, y + 3, 70, 20);
    place(g_rumble, in + 852, y, 100, 320);
    y += 34;
    place(g_top_label[3], in, y + 3, 92, 20);
    place(g_names, in + 96, y, 300, 320);
    place(g_detected, in + 416, y + 3, gw - 416 - 32, 20);

    place(g_group_mapping, m, kMappingY, gw, mapping_height());
    y = kMappingY + 24;
    place(g_hint, in, y, gw - 32, 36);
    y += 44;
    int inner = gw - 32, col_w = (inner - 28) / 2, name_w = std::min(170, col_w / 3), list_w = (col_w - name_w - 3 * 6) / 3;
    for (int c = 0; c < 2; c++) {
        int x = in + c * (col_w + 28);
        place(g_head[c * 4], x, y, name_w, 20);
        for (int i = 1; i < 4; i++) place(g_head[c * 4 + i], x + name_w + 6 + (i - 1) * (list_w + 6) + 2, y, list_w, 20);
    }
    y += 26;
    for (int a = 0, n = 0; a < kActionCount; a++) {
        if (!g_action[a]) continue;
        int col = n / rows(), row = n % rows();
        n++;
        int x = in + col * (col_w + 28), yy = y + row * kRowH;
        place(g_action[a], x, yy + 3, name_w, 20);
        for (int s = 0; s < kPadSlots; s++) place(g_pad[a][s], x + name_w + 6 + s * (list_w + 6), yy, list_w, 360);
    }
    y += rows() * kRowH + 8;
    place(g_preset_label, in, y + 6, 52, 20);
    place(g_preset, in + 54, y + 2, 260, 320);
    place(g_buttons[0], in + 326, y, 130, 28);
    place(g_buttons[1], in + 466, y, 110, 28);

    y = kMappingY + mapping_height() + 12;
    place(g_note, m, y, gw, 36);
    y += 46;
    place(g_status, m, y + 7, gw - 360, 20);
    place(g_buttons[2], w - m - 340, y, 100, 30);
    place(g_buttons[3], w - m - 230, y, 120, 30);
    place(g_buttons[4], w - m - 100, y, 100, 30);
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
        case ID_DEFAULTS: { // Apply Preset
            int i = std::clamp((int)SendMessageW(g_preset, CB_GETCURSEL, 0, 0), 0, (int)std::size(kPresets) - 1);
            const Preset &p = kPresets[i];
            std::wstring msg = std::wstring(p.name) + L" layout applied. Click Save to keep it.";
            if (p.buttons.empty()) { // the game's own controller layouts
                SendMessageW(g_layout, CB_SETCURSEL, 1, 0);
                status(msg.c_str());
                return 0;
            }
            SendMessageW(g_layout, CB_SETCURSEL, 0, 0);
            for (int a = 0; a < kActionCount; a++) {
                if (!g_pad[a][0]) continue;
                std::string v = "none";
                for (const auto *list : {&kCommon, &p.buttons})
                    for (const auto &kv : *list)
                        if (!strcmp(kv.first, kActions[a].ini)) v = kv.second;
                set_pad_row(a, v);
            }
            status(msg.c_str());
            return 0;
        }
        case ID_CLEAR:
            for (int a = 0; a < kActionCount; a++)
                for (int s = 0; s < kPadSlots && g_pad[a][0]; s++) SendMessageW(g_pad[a][s], CB_SETCURSEL, 0, 0);
            status(L"All buttons cleared. Click Save to keep this.");
            return 0;
        case ID_NAMES:
            if (HIWORD(wp) == CBN_SELCHANGE) update_brand(); // only the names: nothing to save
            return 0;
        case ID_PRESET: return 0; // nothing changes until Apply Preset
        }
        if (HIWORD(wp) == CBN_SELCHANGE) status(L"You have unsaved changes.");
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (c == g_note || c == g_subtitle || c == g_detected || c == g_hint) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
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
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 8 / 5; // the title, 1.6 times the text
    g_big = CreateFontIndirectW(&lf);

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
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, inst, nullptr);
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
