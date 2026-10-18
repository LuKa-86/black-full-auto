# TODO

Open work on the BLACK PC port. Findings and addresses live in `NOTES.md`; this is the short list.

## Verify (fixed, waiting for a playtest)

- [x] Fades: the game's gamma ramp (fade factor), now applied at present (`ramp_texture` in loader/src/d3d/device.cpp).
      User confirmed the fade works, and the in-game brightness slider now works too.
- [ ] Picture vs the Xbox Series: compare screenshots of the same still screen with the same brightness slider setting
      (method in NOTES.md, "To check"). Also check the other fades: loading screen before missions, mission start after
      the intro (played and skipped), intro cinematic -> loading screen, level-end trigger -> fade to black.
- [ ] Anything else that is timed in seconds (blur ramps, objective text typing, HUD animations) now runs at the
      right speed.
- [ ] Higher internal resolution (`--scale N`, default 3 = 2560x1440 widescreen, window fitted to the screen,
      DPI-aware): check that every effect still lines up (light shafts, reload blur, bloom, fades, video, menus) at 1-4.
- [ ] Widescreen (default; the game's own 16:9 mode, `--normal-aspect` for 4:3): menus, HUD, videos, briefings.
- [ ] Window resizing at a fixed aspect, fullscreen (PC SETTINGS), black bars when maximised.
- [x] Mouse look: rotates the view directly (user: "feels awesome").
- [ ] Seams of destructible objects: vertex snap 1/8 Xbox pixel + seam fill pass (PC SETTINGS > DISPLAY); user to confirm.
- [ ] Weapon sway with mouse look (the gun swivels like with the stick): user to check the feel and amount.
- [ ] Auto-aim switch (PC SETTINGS > AUDIO AND GAME; default off since 2026-10-12, `--autoaim`): confirm in play that
      the view no longer drifts onto enemies with it off, and that nothing else changes.
- [ ] Key bindings (keys bound to the game's actions, PC SETTINGS > CONTROLS > KEY BINDINGS): play with them; Esc pauses and frees
      the mouse, resuming takes it again. The game's on-screen button prompts still show pad buttons.
- [ ] Aspect ratio / frame rate (main menu PC SETTINGS > DISPLAY only): the choice shows NEXT START, is saved for the
      next run, and nothing changes until then (confirm in play).
- [ ] PC SETTINGS pages (2026-10-12): every row on the main menu and in the pause menu, the sliders, KEY BINDINGS
      (scrolling, capture, reset), FULLSCREEN from the menu. The pause page loads into a small heap: open it in every
      mission (a null allocation for PseMenu.bin would hang the pause menu; NOTES.md).
- [ ] 60 fps (the default since 2026-10-12, `--fps 30` = original): world step 1/60 s, audio fine, weapon refire and reload blur now match
      30 fps (measured). Check the rest in play: other weapons/fire modes, animations, AI, effects, scripted scenes;
      60 became the default (user).
- [ ] 60 fps slowdowns in busy scenes (user; also on PS2): the port's CPU overhead on the one game thread is the limit
      (perf recorder, NOTES.md). Round 1 done (texture checks once per frame, faster vertex conversion, shader key reuse,
      XInput): test scene 10.8 -> 5.1 ms; busy scene 78% -> 99.5% game speed. Round 2: stable textures checked every 16th frame.
      Stutter at 100% game speed: fixed (vblank follows the display; user: "silky smooth").
      Weak laptop (i5-4300U, HD 4400): 73.5% at 60 fps; play at 30 there. Round 3: redundant shader/decl calls skipped, game on
      the last core with its sibling idle. Laptop crash at the first level (256-byte immediate-mode vertices on Intel) fixed;
      user played the first level through on the laptop. Re-record there at 30 and 60 fps to see what the changes gave. Next: VS constants only when changed (broke the
      picture, see NOTES), cache converted vertices in host buffers, log writes off the render thread, game CPU other than CPU 0.
- [ ] Skip any movie (default; Game menu, `--no-movie-skip`): mission movies later in the game, end-of-mission movies,
      credits.
- [ ] Field of view (PC SETTINGS > VIDEO, `--fov`): feel in play, aiming and scopes (aimed FOV = setting / zoom),
      cutscene transitions back to the player's view; gun and hands at the game's view (reload, weapon switch, melee).
- [ ] Reload blur off (PC SETTINGS > AUDIO AND GAME, `--no-reload-blur`; 2026-10-17): checked in a test run (pistol
      reload sharp, original blurred); user to confirm with other weapons.
- [ ] Sprint (Left Shift, SPRINT SPEED; 2026-10-17): measured 4.5 -> 6.75 m/s at 1.5x in mission 1. User to check the
      feel, animations and footsteps at sprint speed, and any scripted spot that expects walking pace.
- [ ] Aim hold / toggle retry / sprint cancels aim, short-click latch (2026-10-17): checked with the pistol in test
      runs; user to check the sniper rifle and other weapons with the weapon flag +0xC0.
- [ ] Automatic health pack (2026-10-17): checked with forced low health; user to check in a real fight.
- [ ] Reload speed (2026-10-17, default 1.5x): checked with the pistol; user to check the shotgun (shell loop,
      states 5/6), other weapons, and the reload blur's length against the shorter reload.
- [ ] Hit detection (user: "hit and miss"): short clicks were being dropped (fixed); next look at weapon spread.
- [ ] Crosshair options (2026-10-17): the game's dot is hidden under a port crosshair, which now stays while aiming;
      user to check the sniper scope and that nothing else centred disappears.
- [ ] Health percentage (2026-10-17): checked in widescreen; check 4:3 placement.
- [ ] Enemy markers (2026-10-17): check against visible enemies (left/right and height), and in later missions.
- [ ] Grenade cooking (2026-10-17): checked tap, 1.5 s hold, 4 s in the hand (mission failed). User to check the
      feel and distances in play; the controller's grenade button is not cooked; no menu row (pages full).
- [ ] Cheat menu (F1, 2026-10-18): god mode, infinite ammo/grenades, refill, weapon select checked in test runs. User
      to check giving weapons when both slots are full (the current one is dropped), the RPG/M203, and later levels.
- [ ] Controller Call of Duty layout (2026-10-18): checked with scripted pad presses; user to play with a real pad.
- [x] Item markers for collectibles and secondary destroy targets: removed at the user's request (2026-10-18); the
      findings stay in NOTES.md "Item markers".
- [x] Auto health pack vs manual: the user had pressed near full health, where the game refuses a pack.
- [ ] Reverb (bin 11 send into an FDN reverb): level and character against the Xbox (`--reverb-gain`).

## Next

- [x] Custom render resolution: `--resolution WxH | H | native`, fractional `--scale` (any size, not just multiples).
- [x] Render size changes while running: `native` follows the window (and fullscreen); PC SETTINGS sets it.
- [x] Faint grid in the game's blur at high render sizes: the small post-processing buffers keep their Xbox size.
      (Also checks: glow and light cones look as on the Xbox; nothing else renders into small targets.)
- [ ] Resolution: better downsampling when the render size is much larger than the window (box filter instead of
      one bilinear tap).
- [x] Rebindable keys: bound to the game's actions (PC SETTINGS > CONTROLS > KEY BINDINGS, black.ini).
- [x] Settings remembered between launches (black.ini: render size, AA, fps, aspect, fullscreen, gain/gamma, mouse, reverb).
- [ ] Also remember the window size and position.
- [x] Port options in the game's own menus: Options > PC SETTINGS and the pause menu (built at start from the user's
      files, src/fe/; NOTES.md "PC SETTINGS in the game's menus"). The Windows menu bar is gone.

## Audio

- [ ] Reverb: exact room from the DSP image (effect 16's state is DSP-format; the HLE uses I3DL2 values by ear).
- [ ] Bins 17/18 (effects 13/14 -> outputs 0xE20/0xE40) and 27-30 (delay effects 18-21) are not mixed.
- [ ] LFE (subwoofer) bin is dropped.
- [ ] 3D sound: cones, doppler and the per-voice rolloff curve are ignored (distance + pan only).
- [ ] Output often reaches full scale (soft limiter active); check voice/mix-bin headroom against the Xbox.

## Open questions

- [ ] Where the Xbox re-fits the viewport to a smaller render target (the light-shaft pass relies on it; the HLE
      re-fits on every SetRenderTarget, see `fit_viewport`).
