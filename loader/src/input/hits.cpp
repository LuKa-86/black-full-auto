// Hits by the player, for the hit marker (`hit_marker`, d3d/overlay.cpp).
//
// The port records the enemies' health when one of the player's shots or grenades goes off and compares it over the
// next few player updates (poll; the damage itself lands a frame or two later: 0x1E7A68 in the character's own update
// writes it). The calls, by FUN_001eefd0's shot dispatch on the weapon type [data+0x90] (0x1F01FC):
// - FUN_001dbd50 (stdcall, one argument; [arg+4] = the weapon user, [user+0xF0] = its character): the hitscan shot of
//   weapons of type 0. Only when that character is the player.
// - FUN_001e2b90 (ebx = the same shot object, no stack arguments): weapons of types 1 and 2.
// - FUN_001e4a30 (thiscall ecx = the grenade, one argument dt; a virtual of the projectile objects): the grenade update,
//   which explodes it at 2 s (FUN_001e3680). Only the player's grenades (projectile manager *0x2D1998: object + 0x120 i,
//   kind +0x3600 + 4i = 3), armed when the slot is freed (it went off).
// Enemies: the character list *0x2D1980 (+0x79B4 count, +0x79BC array) with the AI flag +0x39C (as the enemy markers).
// A health that went down is a hit; one that reached 0 a kill. Each hook is a jmp over the function's prologue to a
// wrapper or thunk that runs the original through the replaced prologue and a jmp back. Launcher rockets (type 3,
// FUN_001e8540, the projectiles' second layout) and melee are not covered.
#include "input/hits.h"

#include "common.h"
#include "input/player_extras.h"

#include <atomic>
#include <cstring>

extern "C" {
uint32_t g_hits_gun_resume = 0x001DBD5C;   // after push ebp; mov ebp,esp; and esp,-16; sub esp,2E4h
uint32_t g_hits_gren_resume = 0x001E4A39;  // after push ebp; mov ebp,esp; and esp,-16; sub esp,1Ch
uint32_t g_hits_pellet_resume = 0x001E2B99; // after push ebp; mov ebp,esp; and esp,-16; sub esp,68h
}

namespace hits {
namespace {

std::atomic<uint32_t> g_last_frame;
std::atomic<bool> g_last_kill;

template <class T> T rd(uint32_t a) { return *(const T *)(uintptr_t)a; }

constexpr int kMax = 128;
struct Snapshot {
    int n = 0;
    uint32_t who[kMax];
    float hp[kMax];
};

void take(Snapshot &s)
{
    s.n = 0;
    uint32_t mgr = rd<uint32_t>(0x002D1980);
    if (!mgr) return;
    int n = rd<int>(mgr + 0x79B4);
    uint32_t arr = rd<uint32_t>(mgr + 0x79BC);
    for (int i = 0; arr && i < n && s.n < kMax; i++) {
        uint32_t e = rd<uint32_t>(arr + 4 * i);
        if (!e || !rd<uint8_t>(e + 0x39C)) continue;
        float hp = rd<float>(e + 0x2F0);
        if (!(hp > 0.0f && hp < 1e30f)) continue;
        s.who[s.n] = e;
        s.hp[s.n++] = hp;
    }
}

// The damage itself lands a frame or two after the shot or blast (the hit is handed to the character's own update,
// 0x1E7A68 writes the health), so a shot or blast arms a window of kWindow player updates in which any enemy below
// its health at that moment counts as hit (poll, from the player update). Another shot re-arms it with a new
// snapshot. An ally's hit on the same enemy inside that window would also count.
constexpr int kWindow = 8;
Snapshot g_pending;
int g_pending_left;
const char *g_pending_what = "";

void arm(const Snapshot &s, const char *what)
{
    if (!s.n) return;
    g_pending = s;
    g_pending_left = kWindow;
    g_pending_what = what;
}

uint32_t player()
{
    uint8_t *ctrl = player_controller();
    return ctrl ? *(const uint32_t *)(ctrl + 0x8C) : 0;
}

__declspec(naked) uint32_t __stdcall gun_original(uint32_t)
{
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        sub esp, 2E4h
        jmp dword ptr [g_hits_gun_resume]
    }
}

uint32_t __stdcall gun_wrap(uint32_t arg)
{
    uint32_t user = rd<uint32_t>(arg + 4), p = player();
    if (!g_cfg.hit_marker || !p || !user || rd<uint32_t>(user + 0xF0) != p) return gun_original(arg);
    Snapshot s;
    take(s);
    uint32_t r = gun_original(arg);
    arm(s, "shot");
    return r;
}

// FUN_001e2b90 (ebx = the same shot object as FUN_001dbd50's argument; weapons of types 1 and 2, e.g. pellets): only
// arms the window before the original runs, from a thunk that keeps every register.
extern "C" void __cdecl pellet_pre(uint32_t arg)
{
    uint32_t user = rd<uint32_t>(arg + 4), p = player();
    if (!g_cfg.hit_marker || !p || !user || rd<uint32_t>(user + 0xF0) != p) return;
    Snapshot s;
    take(s);
    arm(s, "shot");
}

__declspec(naked) void pellet_thunk()
{
    __asm {
        pushad
        push ebx
        call pellet_pre
        add esp, 4
        popad
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        sub esp, 68h
        jmp dword ptr [g_hits_pellet_resume]
    }
}

__declspec(naked) void grenade_original()
{ // entered with ecx = the grenade and [esp+4] = dt, as the game's caller left them
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        sub esp, 1Ch
        jmp dword ptr [g_hits_gren_resume]
    }
}

void call_grenade_original(uint32_t self, float dt)
{
    __asm {
        push dt
        mov ecx, self
        call grenade_original
    }
}

// The player's grenade's in-use flag (manager + 0x3700 + slot), or null.
const uint8_t *players_grenade(uint32_t self)
{
    uint32_t pm = rd<uint32_t>(0x002D1998);
    if (!pm || self < pm || self >= pm + 32 * 0x120 || (self - pm) % 0x120) return nullptr;
    uint32_t slot = (self - pm) / 0x120;
    return rd<uint32_t>(pm + 0x3600 + 4 * slot) == 3 ? (const uint8_t *)(uintptr_t)(pm + 0x3700 + slot) : nullptr;
}

void __fastcall grenade_wrap(uint32_t self, uint32_t, float dt)
{
    const uint8_t *in_use = g_cfg.hit_marker && player() ? players_grenade(self) : nullptr;
    if (!in_use) {
        call_grenade_original(self, dt);
        return;
    }
    Snapshot s;
    take(s);
    call_grenade_original(self, dt);
    if (!*in_use) arm(s, "grenade"); // it went off (the slot was freed)
}

bool hook(uint32_t at, const uint8_t *expect, size_t len, void *to)
{
    auto *code = (uint8_t *)(uintptr_t)at;
    if (memcmp(code, expect, len) != 0) return false;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(code + 1, &rel, 4);
    for (size_t i = 5; i < len; i++) code[i] = 0x90;
    return true;
}

} // namespace

void install()
{
    static const uint8_t kGun[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xE4, 0x02, 0x00, 0x00};
    static const uint8_t kGren[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x1C};
    bool gun = hook(0x001DBD50, kGun, sizeof kGun, (void *)&gun_wrap);
    bool gren = hook(0x001E4A30, kGren, sizeof kGren, (void *)&grenade_wrap);
    static const uint8_t kPellet[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x68};
    bool pellet = hook(0x001E2B90, kPellet, sizeof kPellet, (void *)&pellet_thunk);
    logf("hits: hooks %s (gunfire), %s (types 1-2), %s (grenade)", gun ? "installed" : "NOT installed",
         pellet ? "installed" : "NOT installed", gren ? "installed" : "NOT installed");
}

void poll()
{
    if (g_pending_left <= 0) return;
    g_pending_left--;
    bool hit = false, kill = false;
    for (int i = 0; i < g_pending.n; i++) {
        float hp = rd<float>(g_pending.who[i] + 0x2F0);
        if (hp < g_pending.hp[i]) hit = true, kill |= hp <= 0.0f;
    }
    if (!hit) return;
    g_pending_left = 0;
    g_last_kill = kill;
    g_last_frame = g_frame_count.load();
    static int told;
    if (told++ < 20) logf("[hits] the player's %s %s an enemy", g_pending_what, kill ? "killed" : "hit");
}

uint32_t last_frame() { return g_last_frame; }
bool last_was_kill() { return g_last_kill; }

} // namespace hits
