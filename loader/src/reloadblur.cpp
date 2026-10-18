// Reload blur switch, on by default as in the original (PC SETTINGS > AUDIO AND GAME, --no-reload-blur).
//
// The blur while reloading is mode 1 of the post-process controller at *0x2D1990 + 0x830A0 (mode +0x34, timer +0x14,
// strength +0x10 = the blend alpha of the blur pass FUN_001772E0; see timing.cpp for its hold time). Each frame the
// controller's update (0x15E260, esi = controller) calls FUN_001595f0 for mode 1 with eax = the controller and xmm0 =
// the frame time. That function adds the frame time to the timer, ramps the strength in over +0x1C, holds it for
// +0x20 and ramps it out over +0x24; at the end (0x1596BE) it sets the mode to 0, the colour +0x00..+0x0C to the
// constant at 0x2AD690 and the strength to 0 (xmm3 = 0 there).
//
// The patch puts a check at the entry of FUN_001595f0: when the blur is off it goes straight to that end, so the
// blur stops on the frame it starts and nothing is drawn. The flag is read on every call, so the menu switches it
// live. Other modes of the controller (other screen effects) are untouched.
#include "reloadblur.h"

#include "common.h"

#include <cstring>

extern "C" {
volatile uint8_t g_reload_blur_off;          // read by the thunk on every call
uint32_t g_reload_blur_resume = 0x001595F6; // the instruction after the replaced prologue
uint32_t g_reload_blur_end = 0x001596BE;    // mode = 0, colour reset, strength = xmm3; then the epilogue
}

namespace {

constexpr uint32_t kSite = 0x001595F0;
const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}; // push ebp; mov ebp,esp; and esp,-16 (replaced)
const uint8_t kEndExpect[7] = {0xC7, 0x40, 0x34, 0x00, 0x00, 0x00, 0x00}; // mov dword ptr [eax+34h], 0

__declspec(naked) void reload_blur_thunk()
{
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        cmp byte ptr [g_reload_blur_off], 0
        jne off
        jmp dword ptr [g_reload_blur_resume]
    off:
        xorps xmm3, xmm3
        jmp dword ptr [g_reload_blur_end]
    }
}

} // namespace

void install_reload_blur_patch()
{
    auto *code = (uint8_t *)(uintptr_t)kSite;
    if (memcmp(code, kExpect, sizeof kExpect) != 0 ||
        memcmp((const void *)(uintptr_t)g_reload_blur_end, kEndExpect, sizeof kEndExpect) != 0) {
        logf("reloadblur: code at 0x%08x does not match this build; the reload blur switch does nothing", kSite);
        return;
    }
    reload_blur_set(g_cfg.reload_blur);
    code[0] = 0xE9; // jmp thunk, then a nop over the rest of the 6-byte prologue
    int32_t rel = (int32_t)((uintptr_t)reload_blur_thunk - (kSite + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("reloadblur: switch installed at FUN_001595f0 (reload blur %s)", g_cfg.reload_blur ? "on" : "off");
}

void reload_blur_set(bool on)
{
    g_cfg.reload_blur = on;
    g_reload_blur_off = on ? 0 : 1;
}
