// Cheats (god mode, infinite ammo and grenades, refills) and the in-game cheat menu.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cheats {

void install();                    // after the XBE is mapped (the infinite-ammo patch)
void update(const uint8_t *ctrl);  // each player update (player_extras.cpp)
bool god();                        // god mode on (the damage patch asks)
float max_health();                // the player's full raw health on this difficulty

// The menu (bindings.cpp feeds it keys while it is open; overlay.cpp draws it).
enum Key { kUp = 1, kDown, kLeft, kRight, kSelect, kBack };
bool menu_open();
void toggle_menu();
void menu_key(int key);
struct MenuRow {
    std::wstring label, value;
};
std::vector<MenuRow> menu_rows(int *selected);

} // namespace cheats
