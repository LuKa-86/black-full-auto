// Damage to enemies (`enemy_damage`, PC SETTINGS > GAMEPLAY > MORE: x1 = the game's own).
//
// A character's damage handler (the function around 0x1E7714; ebx = the character hit) copies its health +0x2F0 to
// [esp+0x1C], lets the hit code lower that copy, and at 0x1E794D checks it against 0 (xorps xmm0, xmm0; comiss xmm0,
// [esp+0x1C]; jb alive): dead -> 0 written at 0x1E795B, alive -> the copy written at 0x1E7A68. Every path of the hit
// joins at 0x1E794D, so the port replaces those two instructions with a call that scales what was taken off the copy
// before the check: new = old - (old - new) x factor, only for characters with the AI enemy flag +0x39C (the enemy
// markers' test), so the player and allies are unchanged. No xmm register is live there (xmm0 is set by the check
// and reloaded at 0x1E7A5C), and the flags of the comiss are the thunk's last.
#include "input/damage.h"

#include "common.h"

#include <cstring>

extern "C" void __cdecl damage_scale(const uint8_t *ch, float *hp);

namespace {

__declspec(naked) void damage_thunk()
{
    __asm {
        pushad
        lea eax, [esp + 32 + 4 + 0x1C] // the caller's [esp+0x1C]: past the saved registers and the return address
        push eax
        push ebx
        call damage_scale
        add esp, 8
        popad
        xorps xmm0, xmm0
        comiss xmm0, dword ptr [esp + 4 + 0x1C]
        ret
    }
}

} // namespace

extern "C" void __cdecl damage_scale(const uint8_t *ch, float *hp)
{
    float k = g_cfg.enemy_damage;
    if (k == 1.0f || !ch || !*(const uint32_t *)(ch + 0x39C)) return;
    float old = *(const float *)(ch + 0x2F0);
    if (!(old > 0.0f && old < 1e30f) || !(*hp < old)) return;
    float hit = old - *hp;
    *hp = old - hit * k;
    static int told;
    if (told < 8) {
        told++;
        logf("damage: enemy %p took %.1f -> %.1f (x%.2f), health %.1f -> %.1f", ch, hit, hit * k, k, old, *hp);
    }
}

void install_damage_patch()
{
    constexpr uint32_t kAt = 0x001E794D;
    static const uint8_t kExpect[8] = {0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x44, 0x24, 0x1C};
    auto *code = (uint8_t *)(uintptr_t)kAt;
    if (memcmp(code, kExpect, sizeof kExpect) != 0) {
        logf("damage: code at 0x%08x does not match this build; damage to enemies stays the game's", kAt);
        return;
    }
    code[0] = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)damage_thunk - (kAt + 5));
    memcpy(code + 1, &rel, 4);
    memset(code + 5, 0x90, 3);
    logf("damage: enemy damage x%.2f (0x1E794D)", g_cfg.enemy_damage);
}
