// Reload blur switch: the screen blur the game shows while a weapon reloads (patch in the post-process controller).
#pragma once

void install_reload_blur_patch();
void reload_blur_set(bool on); // live; also sets g_cfg.reload_blur
