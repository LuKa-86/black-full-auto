// Mouse look straight into the player's view angles, bypassing the stick path.
//
// FUN_001ec450 (player input, ebx = player controller) reads the remappable stick channels into look values
// (+0xD8 horizontal = right-stick left minus right, +0xDC vertical, an invert flag at +0x101; channels below
// 0.1 are zeroed after the controller's own dead zone) and calls FUN_001bd3f0(vertical, horizontal) with
// esi = the controller. That function shapes the input (power curve with exponent +0xC8, turn acceleration
// near full deflection via +0xC0/+0xC4/+0xD0/+0xD4, smoothing +0xCC into +0xA0/+0xA4, x0.7 while aiming) and
// integrates the view angles in degrees:
//   yaw   [esi+0x18] += speed +0xB8 * input +0xA4 * zoom factor * frame time   (wrapped to -180..180)
//   pitch [esi+0x1C] += speed +0xBC * input +0xA0 * zoom factor * frame time   (clamped to -70..70)
// The zoom factor (1 / (1 + (zoom - 1) * 0.8)) is in xmm2 at both additions.
//
// The two additions are redirected here and also add the mouse angle of the current frame, times the zoom
// factor (scopes slow the mouse down like the stick). The game then wraps and clamps as usual. Menus,
// pauses and cutscenes do not run this code; each pad poll replaces the previous frame's unused angle.
//
// Weapon sway: FUN_001e5fa0 eases the first-person weapon toward offsets scaled by the look values (it reads
// them through the player object, where the controller sits at +0x4F0: weapon +0x5C8/+0x5CC = controller
// +0xD8/+0xDC; +0x5D0/+0x5D4 = the move values). Mouse look leaves the look values at 0, so the weapon did not
// swivel. Right after the view update (0x1ECF76, after the call to FUN_001bd3f0, where no xmm register is
// live) the stick deflection that would have turned the view as fast as this frame's mouse angle is added to
// them: rate = angle / (speed * frame time), deflection = rate ^ (1 / curve exponent +0xC8), clamped to 1 and
// smoothed a little. +0xD8 has the yaw rate's sign, +0xDC the opposite of the pitch rate's (as the game
// stores it). Later readers in the frame are the weapon sway; the values are rebuilt from the pad next frame.
//
// Controller look settings (black-config.exe; look_* in black.ini). The game's stick turns at most 80 degrees a
// second (+0xB8) and looks up and down at 25 (+0xBC), squares the deflection (+0xC8 = 2, so half way turns at a
// quarter speed), and only past 0.98 deflection (h^2 + v^2) adds a boost +0xD0 that grows by deflection * frame time
// * +0xC0 (1) up to +0xC4 (0.5) before the curve: 2.25 times the speed after half a second. Heavy next to a modern
// shooter. The stick share of each step is scaled by look_x / look_y in the two thunks; the two loads of the exponent
// (fld [esi+0xC8] at 0x1BD5A1 and 0x1BD5F3) read g_ml_curve instead (1.5 or 1 for the modern and linear curves); and
// the boost is written into the controller after each update: up to (2.25 ^ (1 / curve)) - 1, so full tilt still
// ends at 2.25 times, built over 0.5 s (the game's) or 0.2 s, or none. Mouse look is not changed by any of this.
#include "input/mouselook.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
float g_ml_pending_yaw, g_ml_pending_pitch; // degrees for the next view update (set at each pad poll)
float g_ml_used_yaw, g_ml_used_pitch;       // what the last view update used (for the weapon sway)
float g_ml_xmm7;                            // scratch save
float g_ml_look_x = 1.0f, g_ml_look_y = 1.0f; // controller look speed factors (look_x / look_y)
float g_ml_curve = 2.0f;                    // the stick response exponent the game uses (its own: +0xC8)
void __cdecl ml_sway(uint8_t *ctrl);
}

namespace {

bool g_installed, g_curve_patched;
std::atomic<uint32_t> g_play_frame; // the frame of the last player update (ml_sway runs once in each)

// The look settings for the next update (see the top of the file); ctrl = the player controller.
void look_update(uint8_t *ctrl)
{
    g_ml_look_x = g_cfg.look_x / 100.0f;
    g_ml_look_y = g_cfg.look_y / 100.0f;
    float game_curve = *(const float *)(ctrl + 0xC8);
    g_ml_curve = !g_curve_patched || g_cfg.look_curve == 0 ? game_curve : g_cfg.look_curve == 1 ? 1.5f : 1.0f;
    static bool touched; // the game's own boost stays untouched until a setting differs (the values below are its own)
    if (g_cfg.look_curve == 0 && g_cfg.look_boost == 0 && !touched) return;
    touched = true;
    float boost = g_cfg.look_boost == 2 ? 0.0f : powf(2.25f, 1.0f / g_ml_curve) - 1.0f;
    float ramp = g_cfg.look_boost == 1 ? 0.2f : 0.5f;           // seconds to the full boost
    *(float *)(ctrl + 0xC4) = boost;
    *(float *)(ctrl + 0xC0) = boost > 0.0f ? boost / ramp : 1.0f;
}

// The power function FUN_000a3580 (x87: st1 = base, st0 = exponent; returns base ^ exponent) at the four calls after
// the exponent loads. The game passes the signed deflection and takes the absolute value of the result, which only
// works for whole exponents (a negative base to the power 1.5 is NaN), so the base is made positive first.
__declspec(naked) void pow_thunk()
{
    __asm {
        fxch st(1)
        fabs
        fxch st(1)
        mov eax, 0x000A3580
        jmp eax
    }
}

// 0x1BD6A8: addss xmm3, [esi+0x18]  (xmm3 = this frame's stick yaw step; xmm2 = zoom factor)
__declspec(naked) void yaw_thunk()
{
    __asm {
        mulss xmm3, dword ptr [g_ml_look_x]
        addss xmm3, dword ptr [esi + 0x18]
        movss dword ptr [g_ml_xmm7], xmm7
        movss xmm7, dword ptr [g_ml_pending_yaw]
        movss dword ptr [g_ml_used_yaw], xmm7
        mulss xmm7, xmm2
        addss xmm3, xmm7
        movss xmm7, dword ptr [g_ml_xmm7]
        mov dword ptr [g_ml_pending_yaw], 0
        ret
    }
}

// 0x1BD6C7: addss xmm0, [esi+0x1C]  (xmm0 = this frame's stick pitch step; clamped right after)
__declspec(naked) void pitch_thunk()
{
    __asm {
        mulss xmm0, dword ptr [g_ml_look_y]
        addss xmm0, dword ptr [esi + 0x1C]
        movss dword ptr [g_ml_xmm7], xmm7
        movss xmm7, dword ptr [g_ml_pending_pitch]
        movss dword ptr [g_ml_used_pitch], xmm7
        mulss xmm7, xmm2
        addss xmm0, xmm7
        movss xmm7, dword ptr [g_ml_xmm7]
        mov dword ptr [g_ml_pending_pitch], 0
        ret
    }
}

// 0x1ECF76: movss xmm0, [ebx+0x1C] right after the view update; ebx = the player controller.
__declspec(naked) void sway_thunk()
{
    __asm {
        push ebx
        call ml_sway
        add esp, 4
        movss xmm0, dword ptr [ebx + 0x1C]
        ret
    }
}

struct Site {
    uint32_t addr;
    uint8_t expect[5];
    void (*thunk)();
};

const Site kSites[] = {
    {0x001BD6A8, {0xF3, 0x0F, 0x58, 0x5E, 0x18}, yaw_thunk},
    {0x001BD6C7, {0xF3, 0x0F, 0x58, 0x46, 0x1C}, pitch_thunk},
    {0x001ECF76, {0xF3, 0x0F, 0x10, 0x43, 0x1C}, sway_thunk},
};

} // namespace

void install_mouselook_patches()
{
    for (const Site &s : kSites)
        if (memcmp((const void *)(uintptr_t)s.addr, s.expect, 5) != 0) {
            logf("mouselook: code at 0x%08x does not match this build; mouse look disabled", s.addr);
            return;
        }
    for (const Site &s : kSites) { // call thunk (replaces the 5-byte addss)
        auto *code = (uint8_t *)(uintptr_t)s.addr;
        code[0] = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)s.thunk - (s.addr + 5));
        memcpy(code + 1, &rel, 4);
    }
    g_installed = true;
    logf("mouselook: view angles take mouse input directly (FUN_001bd3f0); weapon sway follows it (0x1ECF76)");
    static const uint8_t kCurveLoad[6] = {0xD9, 0x86, 0xC8, 0x00, 0x00, 0x00}; // fld dword ptr [esi+0xC8]
    const uint32_t curve_sites[2] = {0x001BD5A1, 0x001BD5F3};
    const uint32_t pow_calls[4] = {0x001BD5A9, 0x001BD5CC, 0x001BD5FB, 0x001BD61E};
    auto call_target = [](uint32_t a) { return a + 5 + *(const int32_t *)(uintptr_t)(a + 1); };
    for (uint32_t a : curve_sites)
        if (memcmp((const void *)(uintptr_t)a, kCurveLoad, 6) != 0) {
            logf("mouselook: code at 0x%08x does not match this build; the stick response curve stays the game's", a);
            return;
        }
    for (uint32_t a : pow_calls)
        if (*(const uint8_t *)(uintptr_t)a != 0xE8 || call_target(a) != 0x000A3580) {
            logf("mouselook: code at 0x%08x does not match this build; the stick response curve stays the game's", a);
            return;
        }
    for (uint32_t a : pow_calls) {
        int32_t rel = (int32_t)((uintptr_t)pow_thunk - (a + 5));
        memcpy((uint8_t *)(uintptr_t)a + 1, &rel, 4);
    }
    for (uint32_t a : curve_sites) { // fld dword ptr [g_ml_curve]
        auto *code = (uint8_t *)(uintptr_t)a;
        code[1] = 0x05;
        uint32_t addr = (uint32_t)(uintptr_t)&g_ml_curve;
        memcpy(code + 2, &addr, 4);
    }
    g_curve_patched = true;
    logf("mouselook: controller look speed %d%% / %d%%, curve %d, boost %d", g_cfg.look_x, g_cfg.look_y,
         g_cfg.look_curve, g_cfg.look_boost);
}

bool mouselook_installed() { return g_installed; }

uint32_t mouselook_play_frame() { return g_play_frame; }

extern "C" void __cdecl ml_sway(uint8_t *ctrl)
{
    g_play_frame = std::max(1u, g_frame_count.load());
    static float sh, sv; // smoothed deflections
    auto f = [&](uint32_t off) { return *(const float *)(ctrl + off); };
    float dt = *(const float *)(uintptr_t)(*(const uint32_t *)(uintptr_t)0x002D45D4 + 0x1C);
    look_update(ctrl);
    float curve = g_ml_curve > 0.1f ? g_ml_curve : 1.0f;
    auto deflection = [&](float deg, float speed) {
        if (speed <= 0.0f || dt <= 0.0f || deg == 0.0f) return 0.0f;
        float r = fabsf(deg) / (speed * dt);
        float s = powf(r < 1.0f ? r : 1.0f, 1.0f / curve);
        return deg < 0.0f ? -s : s;
    };
    float h = deflection(g_ml_used_yaw, f(0xB8)), v = -deflection(g_ml_used_pitch, f(0xBC));
    float a = dt > 0.0f ? 1.0f - expf(-dt / 0.05f) : 1.0f; // ~50 ms: mouse counts per frame are uneven
    sh += (h - sh) * a;
    sv += (v - sv) * a;
    g_ml_used_yaw = g_ml_used_pitch = 0.0f;
    float *look_h = (float *)(ctrl + 0xD8), *look_v = (float *)(ctrl + 0xDC);
    *look_h = std::clamp(*look_h + sh, -1.0f, 1.0f);
    *look_v = std::clamp(*look_v + sv, -1.0f, 1.0f);
}

void mouselook_set_frame(float yaw_deg, float pitch_deg)
{
    g_ml_pending_yaw = yaw_deg;
    g_ml_pending_pitch = pitch_deg;
}
