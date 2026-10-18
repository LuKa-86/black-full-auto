// Player extras the game does not have, run once per player update: sprint, the automatic health pack, and the
// player controller for hold-to-aim (input/bindings.cpp).
//
// The hook: player input FUN_001ec450 (ebx = the player controller) calls the movement FUN_001cc490(forward, strafe)
// (stdcall, esi = ebx) once per player update at 0x1ECFBC. That call goes through move_thunk, which runs
// player_update(ctrl) first. Menus, pauses and cutscenes do not run it.
//
// Sprint: a held key (Left Shift by default; PC SETTINGS > CONTROLS > KEY BINDINGS) makes the player run faster
// (PC SETTINGS > AUDIO AND GAME > SPRINT SPEED, `sprint` in black.ini, 0 = off). FUN_001cc490 picks speeds for the
// stance (aiming [[ctrl+0x8C]+0x29C]+0x106: 4.5 forward and back, 3 sideways, at most 3.25; crouched [ctrl+0x40]:
// 3, 3, 1, at most 2; else 6 forward, 5.5 back, 6 sideways, at most 4.5; metres per second), limits the stick vector
// to length 1, and scales both the target velocity and the speed cap by k = 1 + [*0x2D45D4 + 0x514] x 0.3 (computed
// twice, at 0x1CC5D5 and 0x1CC70D: `addss xmm0, [1.0]`). The velocity +0xF0 moves toward the target by at most 0.8
// per update, then is cut to k x the cap. Both k additions also multiply by this update's sprint factor, so the
// target speed and the cap grow together and the game's own acceleration takes the player up to sprint speed.
//
// Health: player object = [ctrl+0x8C]; health [player+0x2F0] (raw), health packs signed byte [player+0x8E1]. The
// game's health fraction FUN_00015bb0 = (raw x s)^2 with s = 1/1200 on difficulties 0-1 and 1/750 on 2-3 (game
// object *0x2D45F0 + 0x2014C); a health pack (FUN_001ea730 mode 0, the action at binding +0x84) adds 0.5 to that
// fraction (at most 1) and takes a pack. The automatic health pack presses that action for the player when the
// fraction is below the setting (PC SETTINGS > AUDIO AND GAME > AUTO HEALTH PACK, `autoheal`), so the game's own
// checks and sounds apply.
//
// Reload speed (PC SETTINGS > CONTROLS > RELOAD SPEED, `reload_speed`): the weapon user [player+0x29C] (FUN_001eefd0)
// is in state 4 (reload), 5/6 (shell by shell: start, insert loop) or 8 (finish) while reloading, and each state ends
// on an event of the player's animation [[player+0x328]+0x54] (pistol: 171 frames, state 4 -> 8 at frame 94). The
// animation system FUN_00092020 advances each animation by dt x FUN_00097f00(anim) = dt x 59.94 x [anim+0x9C8]
// (0 = x1) through FUN_00097c60, which also fires its events. So the hands, sounds, the ammo refill and the weapon
// states follow when the port sets +0x9C8 to the reload speed for the reload and puts the old value back after.
#include "input/player_extras.h"

#include "common.h"
#include "input/bindings.h"
#include "input/cheats.h"
#include "input/hits.h"

#include <atomic>
#include <cmath>
#include <cstring>

extern "C" {
float g_sprint_mul = 1.0f;             // this update's speed factor (read by the k thunk)
uint32_t g_extras_move = 0x001CC490;    // FUN_001cc490
float g_sprint_one = 1.0f;              // the value at 0x295C74 that the replaced addss adds
float g_throw_a = 12.0f, g_throw_b = 7.0f; // the player's grenade launch speed (pitch + 1) x a + b (0x28ABF0, 0x295D40)
void __cdecl player_update(uint8_t *ctrl);
}

namespace {

std::atomic<uint8_t *> g_ctrl;     // the player controller of the last update
std::atomic<uint32_t> g_ctrl_frame; // and its frame
uint32_t g_heal_wait;               // frames until the automatic health pack may press again
uint32_t g_rate_anim;               // the animation the reload speed was set on (0: none)
float g_rate_old;                   // its +0x9C8 before
bool g_throw_patched;               // the launch-speed operands point at g_throw_a / g_throw_b

// 0x1ECFBC: call FUN_001cc490 (its two arguments already pushed; esi = ebx = the player controller)
__declspec(naked) void move_thunk()
{
    __asm {
        pushad
        push esi
        call player_update
        add esp, 4
        popad
        jmp dword ptr [g_extras_move] // returns to the game's caller
    }
}

// 0x1CC5D5 and 0x1CC70D: addss xmm0, [0x295C74] (k = 1 + x * 0.3); xmm1-xmm7 untouched
__declspec(naked) void k_thunk()
{
    __asm {
        addss xmm0, dword ptr [g_sprint_one]
        mulss xmm0, dword ptr [g_sprint_mul]
        ret
    }
}

struct Site {
    uint32_t addr;
    uint8_t expect[8];
    size_t len;
    void (*thunk)();
};

const Site kSites[] = {
    {0x001ECFBC, {0xE8, 0xCF, 0xF4, 0xFD, 0xFF}, 5, move_thunk},                     // call FUN_001cc490
    {0x001CC5D5, {0xF3, 0x0F, 0x58, 0x05, 0x74, 0x5C, 0x29, 0x00}, 8, k_thunk}, // addss xmm0, [1.0] (target)
    {0x001CC70D, {0xF3, 0x0F, 0x58, 0x05, 0x74, 0x5C, 0x29, 0x00}, 8, k_thunk}, // addss xmm0, [1.0] (cap)
};

template <class T> T rd(uint32_t a) { return *(const T *)(uintptr_t)a; }

void sprint(const uint8_t *ctrl)
{
    float mul = 1.0f;
    if (g_cfg.sprint > 1.0f && bindings::sprint_held()) {
        float forward = *(const float *)(ctrl + 0xE4);
        bool crouched = ctrl[0x40] != 0, aiming = ctrl[0x45] != 0;
        uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
        uint32_t stance = player ? rd<uint32_t>(player + 0x29C) : 0;
        if (stance && rd<uint8_t>(stance + 0x106)) aiming = true; // the movement's own aiming test
        if (forward > 0.1f && !crouched && !aiming) mul = g_cfg.sprint;
    }
    g_sprint_mul = mul;
}

void auto_heal(const uint8_t *ctrl)
{
    if (g_heal_wait) g_heal_wait--;
    if (g_cfg.autoheal <= 0.0f || g_heal_wait) return;
    float f = health_fraction();
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
    if (!player || f <= 0.0f || f >= g_cfg.autoheal || rd<int8_t>(player + 0x8E1) <= 0) return;
    bindings::press_game_action(33); // health pack
    g_heal_wait = 60;                 // the game takes the pack this frame or the next; then look again
    logf("[extras] health %.0f%%: health pack (%d left before)", f * 100, rd<int8_t>(player + 0x8E1));
}

void reload_speed(const uint8_t *ctrl)
{
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
    uint32_t user = player ? rd<uint32_t>(player + 0x29C) : 0, slot = player ? rd<uint32_t>(player + 0x328) : 0;
    uint32_t anim = slot ? rd<uint32_t>(slot + 0x54) : 0;
    uint32_t state = user ? rd<uint32_t>(user + 0xD8) : 0;
    bool reloading = g_cfg.reload_speed > 1.0f && anim && (state == 4 || state == 5 || state == 6 || state == 8);
    if (g_rate_anim && (!reloading || anim != g_rate_anim)) { // reload over (or another animation): put it back
        *(float *)(uintptr_t)(g_rate_anim + 0x9C8) = g_rate_old;
        g_rate_anim = 0;
    }
    if (!reloading) return;
    float *rate = (float *)(uintptr_t)(anim + 0x9C8);
    if (!g_rate_anim) {
        g_rate_anim = anim;
        g_rate_old = *rate;
    }
    *rate = (g_rate_old != 0.0f ? g_rate_old : 1.0f) * g_cfg.reload_speed;
}

// Grenade cooking (`grenade_cook`, on by default; `--no-grenade-cook`). With it the grenade key does not reach the
// game while held (bindings.cpp); the port counts how long it is held and presses the game's grenade action when it
// is released. The game then plays the throw: the grenade (projectile manager *0x2D1998: 32 slots, in use +0x3700 + i,
// kind +0x3600 + 4i, 3 = the player's grenade, object + 0x120 i) appears at once with its fuse timer +0x110 = 0; the
// throw animation's release event (0x1E8925: grenades [player+0x286] - 1, FUN_001e32a0) launches it about 0.45 s
// later with speed (camera pitch + 1) x 12 + 7, and FUN_001e4a30 adds the frame time to the timer and explodes it at
// 2.0 s (0x295C70; a warning at 1.5 s). The port
// - makes the throw further the longer the key was held: both launch constants (operands at 0x1E33DB and 0x1E33E1,
//   only the player's throw uses them) x 0.7 for a tap up to x 1.5 after 1.5 s;
// - cooks the fuse: the new grenade's timer starts at hold x 1.5 / 4, so about 4 s of holding leaves nothing after the
//   release;
// - holding 4 s throws at once with the timer at 1.95 s and the player's health near zero: the grenade goes off in
//   the hand and kills the player.
constexpr float kCookLimit = 4.0f, kFuse = 2.0f;
float g_hold;            // seconds the grenade key has been held
bool g_hold_was;         // held at the last update
int g_pending = -1;      // after a press: -1 none, else frames left to find the new grenade
float g_pending_timer;   // its timer to set
bool g_overcooked;       // the pending grenade goes off in the hand

void grenade_cook(const uint8_t *ctrl)
{
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
    uint32_t clock = rd<uint32_t>(0x002D45D4);
    if (!player || !clock || !g_throw_patched) return;
    float dt = rd<float>(clock + 0x1C);
    bool held = g_cfg.grenade_cook && bindings::grenade_held() && rd<uint16_t>(player + 0x286) > 0;
    auto press = [&](float hold, bool overcook) {
        float k = 0.7f + 0.8f * std::min(hold / 1.5f, 1.0f);
        g_throw_a = 12.0f * (overcook ? 0.05f : k);
        g_throw_b = 7.0f * (overcook ? 0.05f : k);
        g_pending = 30;
        g_pending_timer = overcook ? kFuse - 0.05f : std::min(hold, kCookLimit) * (kFuse - 0.5f) / kCookLimit;
        g_overcooked = overcook;
        bindings::press_game_action(27); // grenade
        logf("[extras] grenade %s after %.2f s (throw x%.2f, fuse starts at %.2f s)", overcook ? "goes off in the hand" : "thrown",
             hold, overcook ? 0.05f : k, g_pending_timer);
    };
    if (held && g_pending < 0) {
        g_hold += dt;
        if (g_hold >= kCookLimit) { // too long: it goes off in the hand
            press(g_hold, true);
            g_hold = -1e9f; // ignore the rest of this press
        }
    } else if (!held && g_hold_was && g_hold > 0.0f) {
        press(g_hold, false);
    }
    if (!held) g_hold = 0.0f;
    g_hold_was = held;
    if (g_pending < 0) return;
    uint32_t pm = rd<uint32_t>(0x002D1998);
    for (int s = 0; pm && s < 32; s++) { // the new grenade: the player's, still in the hand, timer near 0
        if (!rd<uint8_t>(pm + 0x3700 + s) || rd<uint32_t>(pm + 0x3600 + 4 * s) != 3) continue;
        uint32_t o = pm + s * 0x120;
        float *timer = (float *)(uintptr_t)(o + 0x110);
        if (*timer > 0.2f || rd<uint8_t>(o + 0x116)) continue;
        *timer += g_pending_timer;
        if (g_overcooked && !cheats::god()) *(float *)(uintptr_t)(player + 0x2F0) = 1.0f; // the blast in the hand is fatal
        g_pending = -1;
        return;
    }
    if (--g_pending < 0) logf("[extras] grenade: the throw did not start (the weapon was busy?)");
}

// The launch constants stay scaled until the throw's release event has used them: reset once the grenade has left
// the hand (timer past 0.6 s) or none is pending.
void grenade_reset()
{
    if (g_pending >= 0) return;
    uint32_t pm = rd<uint32_t>(0x002D1998);
    for (int s = 0; pm && s < 32; s++)
        if (rd<uint8_t>(pm + 0x3700 + s) && rd<uint32_t>(pm + 0x3600 + 4 * s) == 3 && rd<uint8_t>(pm + s * 0x120 + 0x116) == 0)
            return; // still in the hand
    g_throw_a = 12.0f;
    g_throw_b = 7.0f;
}

} // namespace

// The player's current weapon: the weapon user [player+0x29C] holds the weapon [+0xF4] (magazine: word +0x18) and its
// data [+0xEC] (magazine size: byte +0x60, ammo type: byte +0x64); spare rounds per type are words at
// [*0x2D45D4 + 0x2A8 + 2 x type] (the HUD's second number). Checked with the pistol: 15 / 15 / 30, after a reload
// 15 / 15 / 15. (Fire held on an empty magazine already reloads in the game itself: state 4 at 0 rounds.)
bool ammo_info(Ammo *out)
{
    uint8_t *ctrl = player_controller();
    uint32_t player = ctrl ? *(const uint32_t *)(ctrl + 0x8C) : 0;
    uint32_t user = player ? rd<uint32_t>(player + 0x29C) : 0;
    uint32_t weapon = user ? rd<uint32_t>(user + 0xF4) : 0, data = user ? rd<uint32_t>(user + 0xEC) : 0;
    uint32_t game = rd<uint32_t>(0x002D45D4);
    if (!weapon || !data || !game) return false;
    int type = rd<uint8_t>(data + 0x64);
    if (type > 16) return false;
    out->mag = rd<int16_t>(weapon + 0x18);
    out->size = rd<uint8_t>(data + 0x60);
    out->spare = rd<int16_t>(game + 0x2A8 + 2 * type);
    out->state = rd<uint32_t>(user + 0xD8);
    return out->size > 0;
}

extern "C" void __cdecl player_update(uint8_t *ctrl)
{
    if (!ctrl) return;
    g_ctrl = ctrl;
    g_ctrl_frame = g_frame_count.load();
    sprint(ctrl);
    auto_heal(ctrl);
    reload_speed(ctrl);
    grenade_cook(ctrl);
    grenade_reset();
    hits::poll();
    cheats::update(ctrl);
}

float health_fraction()
{
    uint8_t *ctrl = player_controller();
    uint32_t player = ctrl ? *(const uint32_t *)(ctrl + 0x8C) : 0;
    uint32_t game = rd<uint32_t>(0x002D45F0);
    if (!player || !game) return 0.0f;
    uint32_t diff = rd<uint32_t>(game + 0x2014C);
    if (diff > 3) return 0.0f;
    float lin = rd<float>(player + 0x2F0) * rd<float>(diff < 2 ? 0x00295E48 : 0x00295E44);
    if (lin < 0.0f) lin = 0.0f;
    return lin * lin;
}

uint8_t *player_controller()
{
    uint32_t f = g_ctrl_frame;
    return f && g_frame_count - f <= 3 ? g_ctrl.load() : nullptr;
}

void install_player_extras()
{
    for (const Site &s : kSites)
        if (memcmp((const void *)(uintptr_t)s.addr, s.expect, s.len) != 0) {
            logf("extras: code at 0x%08x does not match this build; no sprint or auto health pack", s.addr);
            return;
        }
    for (const Site &s : kSites) { // call thunk; nop over the rest of the replaced instruction
        auto *code = (uint8_t *)(uintptr_t)s.addr;
        code[0] = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)s.thunk - (s.addr + 5));
        memcpy(code + 1, &rel, 4);
        for (size_t i = 5; i < s.len; i++) code[i] = 0x90;
    }
    // The player's grenade launch speed (FUN_001e32a0): fmul [0x28ABF0] at 0x1E33D9, fadd [0x295D40] at 0x1E33DF.
    static const uint8_t kMul[6] = {0xD8, 0x0D, 0xF0, 0xAB, 0x28, 0x00}, kAdd[6] = {0xD8, 0x05, 0x40, 0x5D, 0x29, 0x00};
    auto *mul = (uint8_t *)(uintptr_t)0x001E33D9, *add = (uint8_t *)(uintptr_t)0x001E33DF;
    if (!memcmp(mul, kMul, 6) && !memcmp(add, kAdd, 6)) {
        uint32_t a = (uint32_t)(uintptr_t)&g_throw_a, b = (uint32_t)(uintptr_t)&g_throw_b;
        memcpy(mul + 2, &a, 4);
        memcpy(add + 2, &b, 4);
        g_throw_patched = true;
    } else {
        logf("extras: grenade launch code does not match this build; no grenade cooking");
        g_cfg.grenade_cook = false; // the grenade key goes straight to the game
    }
    logf("extras: installed at 0x1ECFBC (sprint x%.2f, auto health pack %.0f%%, aim %s, grenade cooking %s)", g_cfg.sprint,
         g_cfg.autoheal * 100, g_cfg.aim_hold ? "hold" : "toggle", g_throw_patched && g_cfg.grenade_cook ? "on" : "off");
}
