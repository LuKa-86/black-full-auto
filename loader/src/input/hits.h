// Hits by the player on enemies, for the hit marker (d3d/overlay.cpp).
#pragma once

#include <cstdint>

namespace hits {

void install(); // after the XBE is mapped
void poll();    // once per player update (input/player_extras.cpp)
// The last hit by the player's gunfire or grenade: the frame (g_frame_count) it happened, 0 = none yet, and whether
// it killed.
uint32_t last_frame();
bool last_was_kill();

} // namespace hits
