// Field of view in play.
//
// The renderer's camera (renderer *0x2D12B8, camera at +0x740) keeps its FOV in degrees at +0x750 (= camera +0x10).
// It is horizontal: the view setup (FUN_000D2F40, FUN_002033A0) takes tan(fov / 2) x near (+0x70) as the half
// width and divides by the aspect +0x74 (1.778 in widescreen, 1.333 in 4:3) for the half height, so the widescreen
// picture shows less height than 4:3 (43 instead of 55 degrees; 86 degrees in widescreen matches the 4:3 height).
// In play the player's weapon code FUN_001BB110 rewrites it every frame as 70 / zoom (zoom 1 at the hip, more
// while aiming: 44.1 with the pistol); the camera system resets it to 70 (FUN_00202850) and the renderer sets it
// and the blend target +0x7A0 to 70 at start (FUN_00203610). Cutscene cameras blend their own authored values.
// All three take 70 from the constant 0x2888C8, which the compiler shares with the pitch clamp (+-70 degrees,
// FUN_001BD3F0) and a few unrelated uses, so the constant stays and those three loads read the port's value
// instead. Aiming keeps its magnification: the aimed FOV is the setting / zoom.
//
// The first-person gun and hands are a render list of their own (renderer *0x2D67F4 + 0xCAE4), drawn by
// FUN_001EE820 at 0x1EE97C or 0x1EEB2B (FUN_00176F60, with the game's flag 0x2F3C44 set around it). Made for 70 degrees, they
// shrink and their animations show their ends at a wider FOV, so that call goes through a thunk that marks the
// list's draws, and the translated vertex shaders scale their clip-space x/y (fov_viewmodel_scale, constant c200)
// so they project as with the game's own FOV (70 / zoom); depth stays, so clipping and sorting do not change.
// The muzzle flash is not on the gun in the original either: it sits on the aim line near the screen centre.
//
// Ultrawide (`ultrawide`, 21:9 or 32:9): the view setup's aspect comes from FUN_0015A960, which loads 1.778 from
// 0x289A18 (its only use) for widescreen; that load reads the picture's aspect instead. The FOV setting stays the
// 16:9 one and the loads above get it widened so the picture keeps the 16:9 height (Hor+): 2 atan(tan(fov / 2) x
// aspect / 1.778). The gun's scale compares against the widened 70, so it keeps its size.
#include "fov.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
volatile uint8_t g_hud_2d;                // set while the 2D batch helper FUN_000D4C50 draws (HUD sprites, text)
uint32_t g_hud_draw = 0x0002AC00;         // the draw function it calls
volatile uint8_t g_fov_viewmodel;         // set while the first-person list draws (the thunk)
uint32_t g_fov_viewmodel_list = 0x00176F60; // the render-list function the patched call goes to
}

namespace {

float g_fov_deg = 70.0f;  // the setting (horizontal at 16:9)
float g_fov_load = 70.0f; // what the game loads: the setting, widened for ultrawide
float g_game_aspect = 16.0f / 9.0f;

float widen(float deg)
{
    float k = picture_aspect() / (16.0f / 9.0f);
    if (!g_cfg.widescreen || k <= 1.0f) return deg;
    const float half = 3.14159265f / 360.0f;
    return std::min(170.0f, 2.0f * std::atan(std::tan(deg * half) * k) / half / 2.0f);
}

// call FUN_00176F60 with the first-person list: before the effects pass, or after it when
// [[0x2D1990]+0x830D4] == 1 (FUN_001EE820)
constexpr uint32_t kViewmodelCalls[] = {0x001EE97C, 0x001EEB2B};

__declspec(naked) void viewmodel_thunk()
{
    __asm {
        mov byte ptr [g_fov_viewmodel], 1
        call dword ptr [g_fov_viewmodel_list]
        mov byte ptr [g_fov_viewmodel], 0
        ret
    }
}

// 0xD4C9C: call 0x2AC00 in FUN_000D4C50, which draws the batched 2D vertices (screen pixels, 640 x 480) of the HUD
// sprites (FUN_001355B0), the HUD text and the objective messages; marked so that ultrawide keeps them 16:9 (draw.cpp).
// The call has stack arguments (cdecl, four), so the thunk's own return address is set aside while it runs (one
// render thread; the batch helper does not nest).
uint32_t g_hud_ret;
__declspec(naked) void hud_thunk()
{
    __asm {
        pop dword ptr [g_hud_ret]
        mov byte ptr [g_hud_2d], 1
        call dword ptr [g_hud_draw]
        mov byte ptr [g_hud_2d], 0
        push dword ptr [g_hud_ret]
        ret
    }
}

struct Site {
    uint32_t addr;
    uint8_t expect[8]; // movss xmmN, dword ptr ds:[0x2888C8]
};
const Site kSites[] = {
    {0x001BB2CA, {0xF3, 0x0F, 0x10, 0x0D, 0xC8, 0x88, 0x28, 0x00}}, // xmm1: 70 / zoom in play (FUN_001BB110)
    {0x00202F92, {0xF3, 0x0F, 0x10, 0x05, 0xC8, 0x88, 0x28, 0x00}}, // xmm0: camera back to the player's FOV (FUN_00202850)
    {0x002038E5, {0xF3, 0x0F, 0x10, 0x05, 0xC8, 0x88, 0x28, 0x00}}, // xmm0: renderer start, +0x750 and +0x7A0 (FUN_00203610)
};

} // namespace

void fov_set(float degrees)
{
    g_fov_deg = std::clamp(degrees, 50.0f, 120.0f);
    g_fov_load = widen(g_fov_deg);
}

float fov_viewmodel_scale()
{
    float base = widen(70.0f); // the game's 70, widened like the setting
    if (!g_fov_viewmodel || std::fabs(g_fov_load - base) < 0.01f) return 1.0f;
    uint32_t renderer = *(const uint32_t *)(uintptr_t)0x002D12B8;
    if (!renderer) return 1.0f;
    float now = *(const float *)(uintptr_t)(renderer + 0x750); // this frame's FOV: the setting / zoom
    if (!(now > 1.0f && now < 170.0f)) return 1.0f;
    float game = now * base / g_fov_load; // what the game would use: 70 / zoom
    const float half = 3.14159265f / 360.0f;
    return std::tan(now * half) / std::tan(game * half);
}

void install_fov_patch()
{
    fov_set(g_cfg.fov);
    for (const Site &s : kSites)
        if (memcmp((const void *)(uintptr_t)s.addr, s.expect, sizeof s.expect) != 0) {
            logf("fov: code at 0x%08x does not match this build; field of view stays the game's", s.addr);
            return;
        }
    uint32_t target = (uint32_t)(uintptr_t)&g_fov_load;
    for (const Site &s : kSites) memcpy((void *)(uintptr_t)(s.addr + 4), &target, 4); // the load's address operand
    for (uint32_t at : kViewmodelCalls) {
        auto *call = (uint8_t *)(uintptr_t)at;
        int32_t rel;
        memcpy(&rel, call + 1, 4);
        if (call[0] == 0xE8 && at + 5 + rel == g_fov_viewmodel_list) {
            rel = (int32_t)((uintptr_t)&viewmodel_thunk - (at + 5));
            memcpy(call + 1, &rel, 4);
        } else {
            logf("fov: the first-person list call at 0x%08x does not match; the gun follows the FOV there", at);
        }
    }
    static const uint8_t kAspectLoad[8] = {0xF3, 0x0F, 0x10, 0x05, 0x18, 0x9A, 0x28, 0x00}; // movss xmm0, [0x289A18]
    auto *aspect_at = (uint8_t *)(uintptr_t)0x0015A987;
    g_game_aspect = picture_aspect();
    if (g_cfg.widescreen && g_cfg.ultrawide) {
        auto *call = (uint8_t *)(uintptr_t)0x000D4C9C;
        int32_t rel;
        memcpy(&rel, call + 1, 4);
        if (call[0] == 0xE8 && 0x000D4C9C + 5 + rel == g_hud_draw) {
            rel = (int32_t)((uintptr_t)&hud_thunk - (0x000D4C9C + 5));
            memcpy(call + 1, &rel, 4);
        } else {
            logf("fov: the 2D batch call at 0x000D4C9C does not match; the HUD stretches with the picture");
        }
        if (memcmp(aspect_at, kAspectLoad, 8) == 0) {
            uint32_t a = (uint32_t)(uintptr_t)&g_game_aspect;
            memcpy(aspect_at + 4, &a, 4);
        } else {
            logf("fov: code at 0x0015A987 does not match this build; the projection stays 16:9");
        }
    }
    logf("fov: field of view in play %.0f degrees (horizontal at 16:9; %.1f loaded; the game's is 70), aspect %.3f", g_fov_deg,
         g_fov_load, g_game_aspect);
}
