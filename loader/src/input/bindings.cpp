// Keyboard and mouse bound to the game's own actions.
//
// The game reads every player action through its binding table (*0x2F3BD4: 37 dwords, action -> controller
// channel; FUN_001F1520 fills it from one of the presets at 0x2A7AF0, 0x94 bytes each: default, assault, scout,
// southpaw, custom; the custom-controls screen edits single entries). The channels are made from the pad by the
// controller update FUN_000C5A80 (channel object: floats at +0x44, current/previous digital state at +0x25/+0x09):
//   0-3 D-pad up/down/left/right, 4/5 left stick up/down, 6/7 left/right, 8/9 right stick up/down, 10/11 left/right,
//   12 START, 13 BACK, 14/15 left/right stick click, 16-23 A B X Y BLACK WHITE, left trigger, right trigger,
//   24-27 the four stick axes signed; 28 = unbound.
// A bound key looks up its action's channel in the live table and presses the pad input that feeds that channel,
// so the keys mean the same action whatever controller layout is chosen in the game, and controllers keep working.
//
// Menus, pauses and cutscenes (no player update, see in_play) take no bound keys and nothing goes through the pad:
// arrows, Enter and Backspace are the menus' own inputs (up, down, left, right, select, back; held arrows repeat),
// which fe/calls.cpp gives the menus directly; Esc there only lets go of the mouse. Outside the menus the game itself
// waits for the pad (PRESS A TO CONTINUE after a hint, START to skip the mission's intro text): there Enter is A or
// START (see apply). A key held across a switch between play and menus is ignored until it is released (a press that
// starts play must not also act in it).
#include "input/bindings.h"

#include "common.h"
#include "input/action_list.h"
#include "d3d/host.h"
#include "input/cheats.h"
#include "input/mouselook.h"
#include "input/player_extras.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <cstring>

namespace bindings {
namespace {

constexpr int kCount = kActionCount;
constexpr int kCheat = kCount - 1;
constexpr int kSprint = 4;
constexpr int kAim = 6;
constexpr int kGrenade = 10;
constexpr int kPause = 22;
constexpr int kWeapon1 = 24;

// The port's controller layout (`pad_layout = cod`; buttons, modes and defaults in input/action_list.h). The config
// tool black-config.exe edits it in black.ini.
PadBind g_pad[kCount][kPadSlots];
bool g_pad_set;

void ensure_pad_defaults()
{
    if (g_pad_set) return;
    g_pad_set = true;
    for (int i = 0; i < kCount; i++) parse_pad(pad_default(i), g_pad[i]);
}

// Pad button b down in the Xbox pad state (read_pad: analog A B X Y BLACK=RB WHITE=LB LT RT, buttons D-pad 1/2/4/8,
// START 0x10, BACK 0x20, L3 0x40, R3 0x80).
bool pad_button(int b, uint16_t buttons, const uint8_t analog[8])
{
    static const int8_t kAnalog[8] = {0, 1, 2, 3, 5, 4, 6, 7};
    static const uint16_t kBits[8] = {0x40, 0x80, 0x10, 0x20, 0x01, 0x02, 0x04, 0x08};
    return b < 8 ? analog[kAnalog[b]] > 64 : (buttons & kBits[b - 8]) != 0;
}

uint16_t g_keys[kCount][kSlots];
bool g_keys_set; // defaults applied

void ensure_defaults()
{
    if (g_keys_set) return;
    g_keys_set = true;
    for (int i = 0; i < kCount; i++)
        for (int s = 0; s < kSlots; s++) g_keys[i][s] = kActions[i].defaults[s];
}

struct Named {
    uint16_t key;
    const char *name;
};
const Named kNames[] = {
    {VK_LBUTTON, "Mouse1"}, {VK_RBUTTON, "Mouse2"}, {VK_MBUTTON, "Mouse3"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {KEY_WHEEL_UP, "WheelUp"}, {KEY_WHEEL_DOWN, "WheelDown"}, {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"},
    {VK_TAB, "Tab"}, {VK_BACK, "Backspace"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"},
    {VK_LSHIFT, "LShift"}, {VK_RSHIFT, "RShift"}, {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"},
    {VK_RMENU, "RAlt"}, {VK_CAPITAL, "CapsLock"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"}, {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"}, {VK_DECIMAL, "Num."},
    {VK_DIVIDE, "Num/"}, {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_NUMLOCK, "NumLock"}, {VK_APPS, "Menu"},
};

// black.ini spelling: the names above, letters and digits as themselves, F1-F24, Num0-Num9, others as 0xNN.
std::string ini_name(uint16_t k)
{
    if (!k) return "none";
    for (const Named &n : kNames)
        if (n.key == k) return n.name;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, (char)k);
    if (k >= VK_F1 && k <= VK_F24) return "F" + std::to_string(k - VK_F1 + 1);
    if (k >= VK_NUMPAD0 && k <= VK_NUMPAD9) return "Num" + std::to_string(k - VK_NUMPAD0);
    char buf[8];
    snprintf(buf, sizeof buf, "0x%02X", k);
    return buf;
}

uint16_t parse_key(const std::string &s)
{
    if (s.empty() || _stricmp(s.c_str(), "none") == 0) return 0;
    for (const Named &n : kNames)
        if (_stricmp(n.name, s.c_str()) == 0) return n.key;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return (uint16_t)toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && s.size() <= 3 && isdigit((unsigned char)s[1])) {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return (uint16_t)(VK_F1 + n - 1);
    }
    if (_strnicmp(s.c_str(), "Num", 3) == 0 && s.size() == 4 && isdigit((unsigned char)s[3])) return (uint16_t)(VK_NUMPAD0 + s[3] - '0');
    if (_strnicmp(s.c_str(), "0x", 2) == 0) return (uint16_t)strtoul(s.c_str(), nullptr, 16);
    return 0;
}

// ---- per-poll state (game thread) ----

std::bitset<0x102> g_stale;      // held across a play/menu switch: ignored until released
std::bitset<0x102> g_script;     // --press k:Name, this poll only
bool g_was_play;
bool g_freed_by_esc; // the pause key (or Esc in a menu) let go of the mouse: take it again when play starts
bool g_release_was_down;
std::atomic<bool> g_sprint_down; // the sprint key at the last poll in play
std::atomic<uint64_t> g_forced;  // game actions (bit = binding index) to press at the next poll in play
std::atomic<bool> g_grenade_down; // the grenade key at the last poll in play (held back while cooking)

// The aim key. Player input FUN_001ec450 (ebx = player controller) keeps the aim state in ctrl +0x45. The aim action
// (binding +0x40) sets it while held when the profile's aim toggle ctrl +0x103 (copied from game +0x20166 at level
// start, 0x1D566E; set in a default profile) and the weapon's flag [[[ctrl+0x8C]+0x29C]+0xEC]+0xC0 are both clear;
// otherwise each press toggles it (edge via +0x104), and a press the weapon is not ready for (switching, firing,
// reloading) is lost. So when the game toggles, the port decides the state it wants and presses the aim input once
// at a time until the state matches:
// - hold to aim (PC SETTINGS > CONTROLS > AIM, `aim_hold`): aiming while the key is held;
// - toggle (the game's): each press of the key asks for the other state, retried for half a second, so a click while
//   the weapon is busy still takes;
// - starting to sprint (player_extras.cpp) drops the aim in both modes; aiming again while sprinting aims.
// The pad's LT in the Call of Duty layout always aims while held (pad_lt).
bool aim_input(bool key, bool sprint_key, bool pad_lt)
{
    uint8_t *ctrl = player_controller();
    if (!ctrl) return key || pad_lt;
    static int pad_release; // polls left to bring the aim back down after LT was let go
    if (pad_lt) pad_release = 60;
    else if (pad_release && !ctrl[0x45]) pad_release = 0; // not aiming any more: done
    else if (pad_release) pad_release--;
    bool pad_hold = pad_lt || pad_release > 0; // LT held, or let go and still aiming: hold rules
    if (pad_hold) key = g_cfg.aim_hold ? key || pad_lt : pad_lt;
    bool sprinting = sprint_key && g_cfg.sprint > 1.0f && *(const float *)(ctrl + 0xE4) > 0.1f && !ctrl[0x40];
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
    uint32_t stance = player ? *(const uint32_t *)(uintptr_t)(player + 0x29C) : 0;
    uint32_t weapon = stance ? *(const uint32_t *)(uintptr_t)(stance + 0xEC) : 0;
    bool toggles = ctrl[0x103] || (weapon && *(const uint8_t *)(uintptr_t)(weapon + 0xC0));
    bool aiming = ctrl[0x45] != 0;
    static bool key_was, sprint_was, sprint_owns;
    static int target = -1, expire, wait; // target: the wanted state (-1 none); wait: polls before comparing again
    bool edge = key && !key_was;
    key_was = key;
    // Whichever was pressed last wins, as in Call of Duty: starting to sprint drops the aim, pressing aim while
    // sprinting aims (and the sprint stops, player_extras.cpp does not sprint while aiming).
    bool sprint_start = sprinting && !sprint_was;
    sprint_was = sprinting;
    if (sprint_start) sprint_owns = true;
    if (edge) sprint_owns = false;
    bool blocked = sprinting && sprint_owns;
    if (!toggles) return key && !blocked; // the game aims while the input is held
    bool hold = g_cfg.aim_hold || pad_hold;
    if (hold) target = key && !blocked;
    else if (sprint_start) {
        if (aiming || target == 1) target = 0, expire = 30;
    } else if (edge) target = !aiming, expire = 30;
    if (!hold && target >= 0 && --expire <= 0) target = -1;
    if (target < 0) return false;
    if (target == (int)aiming) {
        if (!hold) target = -1;
        return false;
    }
    if (wait) {
        wait--;
        return false;
    }
    wait = 5; // pressed for this poll, then released: one edge
    return true;
}

// Menu keys -> the menus' input ids (FEInputId); held arrows repeat like the game's own pad input.
struct MenuKey {
    uint16_t vk;
    int input;
    bool repeats;
};
const MenuKey kMenuKeys[] = {{VK_UP, 1, true}, {VK_DOWN, 2, true}, {VK_LEFT, 3, true}, {VK_RIGHT, 4, true},
                             {VK_RETURN, 5, false}, {VK_BACK, 6, false}};
constexpr int kMenuKeyCount = sizeof kMenuKeys / sizeof kMenuKeys[0];
constexpr ULONGLONG kRepeatDelay = 400, kRepeatEvery = 120; // ms
bool g_menu_down[kMenuKeyCount];
int g_repeat = -1; // the held arrow that repeats
ULONGLONG g_repeat_at;
std::atomic<int> g_menu_input;            // pressed at the last poll, until the menus take it
std::atomic<uint32_t> g_menu_input_frame; // when
bool g_menu_select;                       // Enter held (movies: skip)

// A hint that stops the game (PRESS A TO CONTINUE: the HUD's hint code at 0x131C47 sets game object (*0x2D45F0) +
// 0x21098 = 3 for the hint ids at 0x28AA80) is closed by the game on A or START of the player's controller (its channel
// block: A pressed +0x35, START +0x31; checks at 0x129B10 and around), not through the menus' input.
bool hint_stopped()
{
    uint32_t game = *(const uint32_t *)(uintptr_t)0x002D45F0;
    return game && *(const int32_t *)(uintptr_t)(game + 0x21098) == 3;
}

// No menu page is up: the front end's current page name (FE object *0x2D199C + 0x3840, as FUN_00118A80 compares it with
// "PauseMenu") is empty. So it is while a mission's intro text types out (the location and time before play), which the
// game skips on START of the player's controller.
bool no_menu_page()
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)0x002D199C;
    return fe && *(const char *)(uintptr_t)(fe + 0x3840) == 0;
}

bool raw_held(uint16_t k, bool focus, int mouse)
{
    if (k < g_script.size() && g_script[k]) return true;
    switch (k) {
    case VK_LBUTTON: return mouse & 1;
    case VK_RBUTTON: return mouse & 2;
    case VK_MBUTTON: return mouse & 4;
    case VK_XBUTTON1: return mouse & 8;
    case VK_XBUTTON2: return mouse & 16;
    case KEY_WHEEL_UP:
    case KEY_WHEEL_DOWN: return false; // handled per notch
    }
    return focus && k < 0x100 && (GetAsyncKeyState(k) & 0x8000);
}

// Each wheel notch is one press of the key bound to that direction: held 2 polls, then 2 released so quick notches
// stay separate presses.
uint16_t wheel_key(int wheel)
{
    static int pending, hold, gap; // pending: notches, + = up, - = down
    static uint16_t held;
    pending = std::clamp(pending + wheel, -4, 4);
    if (hold) {
        hold--;
        if (!hold) gap = 2;
        return held;
    }
    if (gap) {
        gap--;
        return 0;
    }
    if (!pending) return 0;
    held = pending > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    pending += pending > 0 ? -1 : 1;
    hold = 1;
    return held;
}

const int32_t *game_table()
{
    if (!mouselook_installed()) return nullptr; // not the build these addresses are for
    const int32_t *t = *(const int32_t *const *)(uintptr_t)0x002F3BD4;
    return t ? t : (const int32_t *)(uintptr_t)0x002A7AF0; // before the game sets it up: its default preset
}

struct Pad {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int axis[4] = {}; // left x, left y, right x, right y: sum of +1 / -1 pushes
};

void press_channel(int ch, Pad &p)
{
    static const int8_t kAxis[8][2] = {{1, 1}, {1, -1}, {0, -1}, {0, 1}, {3, 1}, {3, -1}, {2, -1}, {2, 1}}; // channels 4-11
    if (ch < 0) return;
    if (ch < 4) p.buttons |= (uint16_t)(1 << ch);
    else if (ch < 12) p.axis[kAxis[ch - 4][0]] += kAxis[ch - 4][1];
    else if (ch < 16) p.buttons |= (uint16_t)(0x10 << (ch - 12));
    else if (ch < 24) p.analog[ch - 16] = 255;
}

// Weapon slot keys: the inventory (player +0x278: +0x20 slot array of weapon users, +0x42 slot count, +0x43 current
// slot, +0x38 swap state) has two slots, so going to the other one is one press of "next weapon" (action 14). Nothing
// when that slot is current or empty, or a swap is under way.
void select_slot(int slot)
{
    uint8_t *ctrl = player_controller();
    uint32_t player = ctrl ? *(const uint32_t *)(ctrl + 0x8C) : 0;
    if (!player) return;
    uint32_t inv = player + 0x278;
    int count = *(const uint8_t *)(uintptr_t)(inv + 0x42), cur = *(const uint8_t *)(uintptr_t)(inv + 0x43);
    uint32_t slots = *(const uint32_t *)(uintptr_t)(inv + 0x20);
    if (slot >= count || slot == cur || !slots || !*(const uint32_t *)(uintptr_t)(slots + 4 * slot) ||
        *(const uint32_t *)(uintptr_t)(inv + 0x38))
        return;
    g_forced |= 1ull << 14;
    logf("[input] weapon slot %d: next weapon (from slot %d)", slot + 1, cur + 1);
}

} // namespace

const Action *actions(int *count)
{
    *count = kCount;
    return kActions;
}

uint16_t key(int action, int slot)
{
    ensure_defaults();
    return g_keys[action][slot];
}

void set_key(int action, int slot, uint16_t k)
{
    ensure_defaults();
    if (!fixed(action, slot)) g_keys[action][slot] = k;
}

void reset_defaults()
{
    g_keys_set = false;
    ensure_defaults();
}

bool fixed(int action, int slot) { return action == kPause && slot == 0; }

std::wstring key_name(uint16_t k)
{
    if (!k) return L"";
    switch (k) {
    case VK_LBUTTON: return L"Left mouse button";
    case VK_RBUTTON: return L"Right mouse button";
    case VK_MBUTTON: return L"Middle mouse button";
    case VK_XBUTTON1: return L"Mouse button 4";
    case VK_XBUTTON2: return L"Mouse button 5";
    case KEY_WHEEL_UP: return L"Mouse wheel up";
    case KEY_WHEEL_DOWN: return L"Mouse wheel down";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RSHIFT: return L"Right Shift";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_LMENU: return L"Left Alt";
    case VK_RMENU: return L"Right Alt";
    }
    bool oem = (k >= 0xBA && k <= 0xC0) || (k >= 0xDB && k <= 0xDF) || k == 0xE2;
    if (oem) { // the character on the key in the current layout
        wchar_t buf[32];
        UINT sc = MapVirtualKeyW(k, MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 32) > 0) return buf;
    }
    return widen(ini_name(k));
}

bool load_ini(const char *k, const char *value)
{
    if (strncmp(k, "pad_", 4) == 0) {
        if (strcmp(k, "pad_sticks") == 0) {
            g_cfg.pad_southpaw = _stricmp(value, "southpaw") == 0;
            return true;
        }
        ensure_pad_defaults();
        int i = action_index(k + 4);
        PadBind b[kPadSlots];
        if (i >= 0 && i != kCheat && parse_pad(value, b)) memcpy(g_pad[i], b, sizeof b);
        else if (i >= 0) logf("[input] black.ini: %s = %s: not a pad button list (A B X Y LB RB LT RT L3 R3 START BACK "
                              "DUP DDOWN DLEFT DRIGHT, .tap / .hold)", k, value);
        return true;
    }
    if (strncmp(k, "key_", 4) != 0) return false;
    ensure_defaults();
    for (int i = 0; i < kCount; i++) {
        if (strcmp(k + 4, kActions[i].ini) != 0) continue;
        std::string v = value;
        size_t comma = v.find(',');
        for (int s = 0; s < kSlots; s++) {
            std::string part = s == 0 ? v.substr(0, comma) : comma == std::string::npos ? "" : v.substr(comma + 1);
            set_key(i, s, parse_key(part));
        }
        return true;
    }
    return true; // a key_ line for an action this build does not have
}

void save_ini(FILE *f)
{
    ensure_defaults();
    for (int i = 0; i < kCount; i++) {
        std::string v = ini_name(g_keys[i][0]);
        if (g_keys[i][1]) v += "," + ini_name(g_keys[i][1]);
        fprintf(f, "key_%s = %s\n", kActions[i].ini, v.c_str());
    }
    ensure_pad_defaults();
    fprintf(f, "pad_sticks = %s\n", g_cfg.pad_southpaw ? "southpaw" : "normal");
    for (int i = 0; i < kCount; i++)
        if (i != kCheat) fprintf(f, "pad_%s = %s\n", kActions[i].ini, pad_text(g_pad[i]).c_str());
}

void script_hold(const std::string &name)
{
    uint16_t k = parse_key(name);
    if (k && k < g_script.size()) g_script[k] = true;
}

int take_menu_input()
{
    int v = g_menu_input.exchange(0);
    return v && g_frame_count - g_menu_input_frame <= 2 ? v : 0;
}

bool menu_select_held() { return g_menu_select; }

bool sprint_held() { return g_sprint_down; }

void press_game_action(int game)
{
    if (game >= 0 && game < 64) g_forced |= 1ull << game;
}

bool grenade_held() { return g_grenade_down; }

bool in_play()
{
    uint32_t f = mouselook_play_frame();
    return f && g_frame_count - f <= 3;
}

void apply(uint16_t &buttons, uint8_t analog[8], int16_t &lx, int16_t &ly, int16_t &rx, int16_t &ry, bool focus,
           bool mouse_live, int wheel)
{
    ensure_defaults();
    bool play = in_play();
    int mouse = mouse_live ? host3d::mouse_buttons() : 0;
    uint16_t wkey = wheel_key(mouse_live ? wheel : 0);

    if (play != g_was_play) { // keys held now belong to the other mode: ignore them until released
        logf("[input] %s (frame %u)", play ? "in play: keys drive the game's actions" : "menu, pause or cutscene: "
             "arrows / Enter / Backspace drive the menus", g_frame_count.load());
        for (int i = 0; i < kCount; i++)
            for (int s = 0; s < kSlots; s++)
                if (raw_held(g_keys[i][s], focus, mouse)) g_stale[g_keys[i][s]] = true;
        for (uint16_t k : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_ESCAPE})
            if (raw_held(k, focus, 0)) g_stale[k] = true;
        if (play && g_freed_by_esc && focus) host3d::request_capture(true);
        if (play) g_freed_by_esc = false;
        g_was_play = play;
    }
    auto held = [&](uint16_t k) {
        if (!k) return false;
        if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) return k == wkey || g_script[k];
        if (!raw_held(k, focus, mouse)) {
            g_stale[k] = false;
            return false;
        }
        return !g_stale[k];
    };

    // The cheat menu (input/cheats.cpp): its key opens and closes it in play; while it is open, arrows / Enter / Esc
    // drive it and nothing reaches the game. Keys still down when it closes wait for a release.
    {
        bool cheat_key = false;
        for (int s = 0; s < kSlots; s++) cheat_key |= held(g_keys[kCheat][s]);
        static bool cheat_was;
        if (play && cheat_key && !cheat_was) cheats::toggle_menu();
        cheat_was = cheat_key;
        if (!play && cheats::menu_open()) cheats::toggle_menu();
        if (cheats::menu_open()) {
            static const uint16_t kKeys[6] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE};
            static bool was[6];
            for (int i = 0; i < 6; i++) {
                bool h = raw_held(kKeys[i], focus, 0);
                if (h && !was[i]) cheats::menu_key(i + 1);
                was[i] = h;
            }
            if (!cheats::menu_open()) // closed now: the keys still down must not act in the game
                for (int i = 0; i < kCount; i++)
                    for (int s = 0; s < kSlots; s++)
                        if (raw_held(g_keys[i][s], focus, mouse)) g_stale[g_keys[i][s]] = true;
            if (!cheats::menu_open())
                for (uint16_t k : kKeys)
                    if (raw_held(k, focus, 0)) g_stale[k] = true;
            buttons = 0;
            memset(analog, 0, 8);
            lx = ly = rx = ry = 0;
            g_script.reset();
            return;
        }
    }

    Pad p;
    const int32_t *table = game_table();
    bool pause_down = false, sprint_down = false, grenade_down = false;
    bool sprint_key = false;
    for (int s = 0; s < kSlots; s++) sprint_key |= held(g_keys[kSprint][s]);
    // The port's controller layout (`pad_layout = cod`, the default; g_pad, see above): in play the pad's buttons are
    // turned into actions like keys, through the game's binding table; the sticks stay as they are (left move, right
    // look; swapped with pad_sticks = southpaw). Menus, pauses and cutscenes get the pad unchanged.
    bool pad_down[kCount] = {};
    if (play && table && g_cfg.pad_cod) {
        ensure_pad_defaults();
        static ULONGLONG since[kPadButtons]; // when each button went down (0 = up)
        bool tapped[kPadButtons] = {}, long_held[kPadButtons] = {}, now_down[kPadButtons] = {};
        ULONGLONG t = GetTickCount64();
        for (int b = 0; b < kPadButtons; b++) {
            now_down[b] = pad_button(b, buttons, analog);
            if (now_down[b]) {
                if (!since[b]) since[b] = t;
                long_held[b] = t - since[b] >= kTapMs;
            } else {
                tapped[b] = since[b] && t - since[b] < kTapMs; // released quickly: acts for this poll
                since[b] = 0;
            }
        }
        for (int i = 0; i < kCount; i++)
            for (const PadBind &pb : g_pad[i]) {
                if (pb.button < 0) continue;
                pad_down[i] |= pb.mode == kTap ? tapped[pb.button] : pb.mode == kHold ? long_held[pb.button] : now_down[pb.button];
            }
        buttons = 0; // the game sees only what the layout presses
        memset(analog, 0, 8);
    }
    if (play && g_cfg.pad_southpaw) {
        std::swap(lx, rx);
        std::swap(ly, ry);
    }
    sprint_key |= pad_down[kSprint];
    for (int i = 0; play && i < kCount && table; i++) {
        bool down = false;
        for (int s = 0; s < kSlots; s++) down |= held(g_keys[i][s]);
        if (i == kAim) down = aim_input(down, sprint_key, pad_down[kAim]);
        else down |= pad_down[i];
        if (i == kWeapon1 || i == kWeapon1 + 1) {
            static bool was[2];
            if (down && !was[i - kWeapon1]) select_slot(i - kWeapon1);
            was[i - kWeapon1] = down;
            continue;
        }
        if (i == kGrenade && g_cfg.grenade_cook) { // player_extras.cpp throws on release (press_game_action)
            grenade_down = down;
            continue;
        }
        if (!down) continue;
        if (i == kPause) pause_down = true;
        if (i == kSprint) sprint_down = true;
        if (kActions[i].game < 0) continue; // the port's own action
        int ch = table[kActions[i].game];
        if (ch < 0 || ch >= 24) {
            static std::bitset<kCount> told;
            if (!told[i]) logf("[input] %ls: the game's controller layout has no button for this action", kActions[i].name);
            told[i] = true;
            continue;
        }
        press_channel(ch, p);
    }
    g_sprint_down = sprint_down;
    g_grenade_down = grenade_down;
    uint64_t forced = g_forced.exchange(0);
    for (int a = 0; play && table && forced && a < 37; a++) {
        if (!(forced >> a & 1)) continue;
        int ch = table[a];
        if (ch >= 0 && ch < 24) press_channel(ch, p);
    }
    bool menu_esc = false;
    int input = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kMenuKeyCount; i++) {
        bool h = !play && held(kMenuKeys[i].vk);
        if (h && !g_menu_down[i]) {
            input = kMenuKeys[i].input;
            g_repeat = kMenuKeys[i].repeats ? i : -1;
            g_repeat_at = now + kRepeatDelay;
        }
        g_menu_down[i] = h;
    }
    if (g_repeat >= 0 && !g_menu_down[g_repeat]) g_repeat = -1;
    if (!input && g_repeat >= 0 && now >= g_repeat_at) {
        input = kMenuKeys[g_repeat].input;
        g_repeat_at = now + kRepeatEvery;
    }
    if (input) {
        g_menu_input_frame = g_frame_count.load();
        g_menu_input = input;
    }
    g_menu_select = g_menu_down[4];
    // Enter is the pad's A for the whole press when, as it went down, a hint stopped the game, or START when no menu
    // page was up (the intro text); a press a menu took stays the menu's.
    enum { kMenu, kA, kStart };
    static bool enter_was;
    static int enter_as, told;
    if (g_menu_select && !enter_was) {
        enter_as = hint_stopped() ? kA : no_menu_page() ? kStart : kMenu;
        if (enter_as != kMenu && told++ < 20) logf("[input] Enter = %s", enter_as == kA ? "A (a hint stops the game)" : "START (no menu page)");
    }
    enter_was = g_menu_select;
    if (g_menu_select && enter_as == kA) p.analog[0] = 255;
    if (g_menu_select && enter_as == kStart) p.buttons |= 0x10;
    if (!play) menu_esc = held(VK_ESCAPE);
    // The pause key in play and Esc in menus also let go of the mouse; play starting or
    // resuming takes it again.
    bool release = play ? pause_down : menu_esc;
    if (release && !g_release_was_down && host3d::mouse_captured()) {
        host3d::request_capture(false);
        g_freed_by_esc = true;
    }
    g_release_was_down = release;
    g_script.reset();

    buttons |= p.buttons;
    for (int i = 0; i < 8; i++) analog[i] = std::max(analog[i], p.analog[i]);
    int16_t *axes[4] = {&lx, &ly, &rx, &ry};
    for (int i = 0; i < 4; i++) { // keys push the stick fully, unless a pad already pushes that axis further
        int v = std::clamp(p.axis[i], -1, 1) * 32767;
        if (v && abs(v) > abs(*axes[i])) *axes[i] = (int16_t)v;
    }
}


// ---- in-game capture (game thread) ----
namespace {
int g_cap_action = -1, g_cap_slot;
bool g_cap_wait_up, g_cap_changed;

bool candidate(int vk) // keys that can be bound: mouse buttons and keyboard keys, Shift/Ctrl/Alt as left/right
{
    if (vk == 3 || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return false;
    return vk >= 1 && vk <= 0xFE;
}
} // namespace

void capture_start(int action, int slot)
{
    if (action < 0 || action >= kCount || fixed(action, slot)) return;
    g_cap_action = action;
    g_cap_slot = slot;
    g_cap_wait_up = true;
    logf("[input] key bindings: press a key for %ls (key %d)", kActions[action].name, slot + 1);
}

bool capturing() { return g_cap_action >= 0; }

bool capture_take_change()
{
    bool c = g_cap_changed;
    g_cap_changed = false;
    return c;
}

bool capture_poll(bool focus, int wheel)
{
    if (g_cap_action < 0) return false;
    ensure_defaults();
    uint16_t pressed = 0;
    bool any = false;
    for (int vk = 1; vk <= 0xFE; vk++) {
        bool down = g_script[vk] || (focus && (GetAsyncKeyState(vk) & 0x8000)); // tests: --press-file k:Name
        if (!candidate(vk) || !down) continue;
        any = true;
        if (!pressed) pressed = (uint16_t)vk;
    }
    g_script.reset(); // apply() does not run while capturing
    if (g_cap_wait_up) { // the key that started the capture is still down
        if (!any) g_cap_wait_up = false;
        return true;
    }
    if (!pressed && wheel && focus) pressed = wheel > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    if (!pressed) return true;
    int a = g_cap_action, s = g_cap_slot;
    g_cap_action = -1;
    g_cap_changed = true;
    for (uint16_t k : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_ESCAPE})
        if (focus && (GetAsyncKeyState(k) & 0x8000)) g_stale[k] = true; // not also a menu press when capture ends
    if (pressed == VK_ESCAPE) {
        logf("[input] key bindings: cancelled");
        return true;
    }
    uint16_t k = pressed == VK_DELETE ? 0 : pressed;
    if (k)
        for (int i = 0; i < kCount; i++)
            for (int j = 0; j < kSlots; j++)
                if ((i != a || j != s) && g_keys[i][j] == k && !fixed(i, j)) g_keys[i][j] = 0; // taken from that action
    g_keys[a][s] = k;
    settings_save();
    logf("[input] key bindings: %ls key %d = %s", kActions[a].name, s + 1, k ? ini_name(k).c_str() : "none");
    return true;
}
} // namespace bindings
