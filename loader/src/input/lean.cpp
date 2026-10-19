// Tactical leaning (`lean`: PC SETTINGS > GAMEPLAY > MORE > LEAN; Lean Left / Lean Right in KEY BINDINGS and
// black-config.exe, unbound by default).
//
// Each character's update (the function around 0x1DFF4C, esi = the character) rebuilds its eye matrix at +0xD0
// (RenderWare: right, up, at, position; found with --watch on the player's +0xD0 / +0x100) from the yaw, then turns
// it by the pitch (FUN_00016260) and copies the result in at 0x1DFFA3-0x1DFFBA. The view, the enemy markers and the
// player's shots (the hitscan and the grenades launch from it) all read that matrix later in the frame, and the next
// update builds it again from scratch, so an offset written right after the copy moves the eye for one frame only and
// never piles up. The port replaces the next instruction (0x1DFFBE movaps xmm0, [esp+0x28]; no other xmm register is
// live there) with a call that, for the player only, slides the eye sideways along its right vector and rolls it
// about its forward axis by the lean, eased in and out over about 0.15 s. Shots follow the leaned eye, so leaning
// round a corner and firing works. The first-person gun is drawn with its own fixed view and stays where it is.
// There is no wall test: the distances are kept short.
#include "input/lean.h"

#include "common.h"
#include "input/bindings.h"
#include "input/player_extras.h"

#include <cmath>
#include <cstring>

extern "C" void __cdecl lean_apply(uint8_t *ch);

namespace {

struct Amount {
    float metres, degrees;
};
// OFF, SMALL, MEDIUM, LARGE
constexpr Amount kAmounts[] = {{0.0f, 0.0f}, {0.25f, 6.0f}, {0.4f, 10.0f}, {0.55f, 14.0f}};

float g_lean;          // -1 (left) .. 1 (right), eased
uint32_t g_last_frame; // the frame of the last player update

__declspec(naked) void lean_thunk()
{
    __asm {
        pushad
        push esi
        call lean_apply
        add esp, 4
        popad
        movaps xmm0, xmmword ptr [esp + 4 + 0x28]
        ret
    }
}

} // namespace

extern "C" void __cdecl lean_apply(uint8_t *ch)
{
    uint8_t *ctrl = player_controller();
    if (!ctrl || ch != (uint8_t *)(uintptr_t)*(const uint32_t *)(ctrl + 0x8C)) return;
    int setting = g_cfg.lean;
    if (setting <= 0 || setting >= (int)std::size(kAmounts)) {
        g_lean = 0.0f;
        return;
    }
    float dt = *(const float *)(uintptr_t)(*(const uint32_t *)(uintptr_t)0x002D45D4 + 0x1C);
    if (!(dt > 0.0f && dt < 0.5f)) dt = 1.0f / 60.0f;
    uint32_t now = g_frame_count.load();
    if (now - g_last_frame > 10) g_lean = 0.0f; // back from a menu or a cutscene: start upright
    g_last_frame = now;
    float want = (float)bindings::lean_dir();
    float a = 1.0f - expf(-dt / 0.05f); // about 0.15 s to settle
    g_lean += (want - g_lean) * a;
    if (fabsf(g_lean) < 1e-3f && want == 0.0f) {
        g_lean = 0.0f;
        return;
    }
    const Amount &k = kAmounts[setting];
    float *m = (float *)(ch + 0xD0); // right +0, up +0x10, at +0x20, position +0x30 (4 floats each)
    float *right = m, *up = m + 4, *pos = m + 12;
    // The game's view negates x (the enemy markers' screen x = -right), so +right on screen is -right in the matrix.
    float side = -g_lean * k.metres;
    for (int i = 0; i < 3; i++) pos[i] += right[i] * side;
    float ang = g_lean * k.degrees * 3.14159265f / 180.0f, c = cosf(ang), s = sinf(ang);
    for (int i = 0; i < 3; i++) { // roll about the forward axis: right and up turn together
        float r = right[i], u = up[i];
        right[i] = r * c + u * s;
        up[i] = u * c - r * s;
    }
}

void install_lean_patch()
{
    constexpr uint32_t kAt = 0x001DFFBE;
    static const uint8_t kExpect[5] = {0x0F, 0x28, 0x44, 0x24, 0x28}; // movaps xmm0, [esp+0x28]
    auto *code = (uint8_t *)(uintptr_t)kAt;
    if (memcmp(code, kExpect, sizeof kExpect) != 0) {
        logf("lean: code at 0x%08x does not match this build; no leaning", kAt);
        return;
    }
    code[0] = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)lean_thunk - (kAt + 5));
    memcpy(code + 1, &rel, 4);
    logf("lean: eye matrix after 0x1DFFBA (amount %d)", g_cfg.lean);
}
