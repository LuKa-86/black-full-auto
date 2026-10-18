// The port's HUD additions over the finished picture: crosshair, health percentage, enemy markers.
#pragma once

#include <d3d9.h>

namespace xd3d {
void draw_overlay(IDirect3DSurface9 *target, const RECT &frame, const RECT &pic); // after the picture is in `target`
}
