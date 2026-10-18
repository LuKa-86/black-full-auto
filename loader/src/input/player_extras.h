// Player extras run in each player update: sprint and the automatic health pack.
#pragma once

#include <cstdint>

void install_player_extras(); // after the XBE is mapped; the options are read from g_cfg every update
uint8_t *player_controller(); // the player controller while in play (updated in the last 3 frames), else null
float health_fraction();      // the game's health 0-1 (FUN_00015bb0), 0 when not in play
struct Ammo {
    int mag, size, spare; // rounds in the magazine, magazine size, spare rounds of its type
    uint32_t state;       // the weapon user's state (0 idle, 4/5/6/8 reloading)
};
bool ammo_info(Ammo *out);    // the player's current weapon; false when not in play or no weapon
