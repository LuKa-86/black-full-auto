// The bindable actions, the pad buttons and the default controller layout: shared by the game's input
// (input/bindings.cpp) and the configuration tool (config/black_config.cpp), so both read and write the same
// black.ini lines (key_<action> = keys, pad_<action> = pad buttons).
#pragma once

#include "input/bindings.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace bindings {

// Names as on the game's controls screen (FE_ACTION_* strings; the tables built at 0x214820 pair them with the
// binding-table index). The movement axes are split into their stick halves. Actions with game index -1 are the
// port's own (sprint, weapon slots, cheat menu): they press nothing on the pad themselves.
inline const Action kActions[] = {
    {"run_forward", L"Run Forward", 1, {'W', 0}},
    {"run_back", L"Run Back", 2, {'S', 0}},
    {"step_left", L"Sidestep Left", 3, {'A', 0}},
    {"step_right", L"Sidestep Right", 4, {'D', 0}},
    {"sprint", L"Sprint", -1, {VK_LSHIFT, 0}},
    {"fire", L"Fire", 9, {VK_LBUTTON, 0}},
    {"aim", L"Aim", 16, {VK_RBUTTON, 0}},
    {"reload", L"Reload", 11, {'R', 0}},
    {"use", L"Action (Use, Pick Up)", 13, {'E', 0}},
    {"crouch", L"Crouch", 12, {'C', 0}},
    {"grenade", L"Grenade", 27, {'G', 0}},
    {"melee", L"Melee", 32, {'F', 0}},
    {"next_weapon", L"Next Weapon", 14, {KEY_WHEEL_DOWN, 0}},
    {"prev_weapon", L"Previous Weapon", 15, {KEY_WHEEL_UP, 0}},
    {"firing_mode", L"Change Firing Mode", 30, {'Z', 0}},
    {"suppressor", L"Toggle Suppressor", 31, {'Q', 0}},
    {"health_pack", L"Health Pack", 33, {'X', 0}},
    {"music", L"Next Music Track", 34, {0, 0}},
    {"turn_left", L"Turn Left", 5, {0, 0}},
    {"turn_right", L"Turn Right", 6, {0, 0}},
    {"look_up", L"Look Up", 7, {0, 0}},
    {"look_down", L"Look Down", 8, {0, 0}},
    {"pause", L"Pause", 25, {VK_ESCAPE, 0}},
    {"objectives", L"Show Objectives", 36, {VK_SPACE, 0}},
    {"weapon_1", L"Weapon Slot 1", -1, {'1', 0}}, // switch to that inventory slot (bindings.cpp, select_slot)
    {"weapon_2", L"Weapon Slot 2", -1, {'2', 0}},
    {"cheat_menu", L"Cheat Menu", -1, {VK_F1, 0}}, // input/cheats.cpp; keyboard only (the menu needs the arrows)
};
inline constexpr int kActionCount = sizeof kActions / sizeof kActions[0];

inline int action_index(const char *ini)
{
    for (int i = 0; i < kActionCount; i++)
        if (strcmp(kActions[i].ini, ini) == 0) return i;
    return -1;
}

// ---- the port's controller layout (`pad_layout = cod`) ----
// Each action can have up to kPadSlots pad buttons: black.ini pad_<action> = RT, or X.tap,LB. A plain button acts
// while held; ".tap" acts for one poll when the button is released within kTapMs; ".hold" acts while it has been held
// kTapMs or longer (so one button can do two things, like X = reload on a tap, use when held).
inline const char *const kPadNames[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "START", "BACK",
                                        "DUP", "DDOWN", "DLEFT", "DRIGHT"};
inline constexpr int kPadButtons = 16, kPadSlots = 3;
inline constexpr ULONGLONG kTapMs = 300;
enum PadMode : uint8_t { kPress, kTap, kHold };
struct PadBind {
    int8_t button = -1;
    PadMode mode = kPress;
};

// The defaults: the Call of Duty layout (user, 2026-10-18: LT aim, RB grenade, A health pack).
struct PadDefault {
    const char *action, *buttons;
};
inline const PadDefault kPadDefaults[] = {
    {"fire", "RT"}, {"aim", "LT"}, {"grenade", "RB"}, {"health_pack", "LB,A,DDOWN"}, {"crouch", "B"},
    {"next_weapon", "Y"}, {"melee", "R3"}, {"firing_mode", "DUP"}, {"suppressor", "DLEFT"}, {"prev_weapon", "DRIGHT"},
    {"objectives", "BACK"}, {"pause", "START"}, {"sprint", "L3"}, {"reload", "X.tap"}, {"use", "X.hold"},
};

// "X.tap,LB" -> slots; false if a name is not a pad button.
inline bool parse_pad(const std::string &v, PadBind out[kPadSlots])
{
    for (int s = 0; s < kPadSlots; s++) out[s] = PadBind();
    size_t pos = 0;
    for (int s = 0; s < kPadSlots && pos <= v.size(); s++) {
        size_t comma = v.find(',', pos);
        std::string part = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? v.size() + 1 : comma + 1;
        size_t dot = part.find('.');
        std::string name = part.substr(0, dot), mode = dot == std::string::npos ? "" : part.substr(dot + 1);
        if (name.empty() || _stricmp(name.c_str(), "none") == 0) continue;
        for (int b = 0; b < kPadButtons; b++)
            if (_stricmp(kPadNames[b], name.c_str()) == 0) out[s].button = (int8_t)b;
        if (out[s].button < 0) return false;
        out[s].mode = _stricmp(mode.c_str(), "tap") == 0 ? kTap : _stricmp(mode.c_str(), "hold") == 0 ? kHold : kPress;
    }
    return true;
}

inline std::string pad_text(const PadBind b[kPadSlots])
{
    std::string v;
    for (int s = 0; s < kPadSlots; s++) {
        if (b[s].button < 0) continue;
        if (!v.empty()) v += ',';
        v += kPadNames[b[s].button];
        if (b[s].mode == kTap) v += ".tap";
        if (b[s].mode == kHold) v += ".hold";
    }
    return v.empty() ? "none" : v;
}

// The default buttons of action i ("none" if it has none).
inline const char *pad_default(int i)
{
    for (const PadDefault &d : kPadDefaults)
        if (strcmp(d.action, kActions[i].ini) == 0) return d.buttons;
    return "none";
}

} // namespace bindings
