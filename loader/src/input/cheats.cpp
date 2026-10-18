// Cheats and the in-game cheat menu (user, 2026-10-17). The menu opens with the CHEAT MENU key (F1 by default, KEY
// BINDINGS) while playing; arrows choose, left / right / Enter change, Esc or the key closes it. While it is open
// the keys and the pad reach neither the game nor the mouse look (input/bindings.cpp, xinput.cpp) and the menu is
// drawn by d3d/overlay.cpp. The game keeps running behind it.
//
// God mode: damage to the player is skipped in the game's damage function FUN_001ed2b0 (damage_thunk below; found
// with `--watch` on the health, writer 0x1ED5B2) and the health [player+0x2F0] is kept at full every update (the
// game's maximum is 1 / the difficulty's health scale, see health_fraction).
//
// Infinite ammo: each shot ends in FUN_001eefd0 at 0x1EFBBB with `dec word [weapon+0x18]` (the magazine; ebp = the
// weapon user, weapon = [user+0xF4]). The 10 bytes there (that and the following `mov eax, [ebp+0xF4]`) call
// ammo_thunk, which skips the decrement for the player's weapon user ([player+0x29C]) only.
//
// Infinite grenades: the grenade count (*0x2D45D4 + 0x2B6, = player +0x286) is kept at 9.
#include "input/cheats.h"

#include "common.h"
#include "input/player_extras.h"

#include <cctype>
#include <cstring>
#include <cwctype>
#include <string>

extern "C" {
volatile uint32_t g_cheat_ammo_user; // the player's weapon user while infinite ammo is on, else 0
volatile uint32_t g_cheat_god_player; // the player object while god mode is on, else 0
void ammo_thunk();
void damage_thunk();
}

namespace cheats {
namespace {

bool g_god, g_ammo, g_grenades;
bool g_open;
int g_sel;
bool g_installed_ammo;
int g_weapon;           // the weapon type chosen in the menu
int g_give = -1;        // a weapon type to hand over at the next player update
std::wstring g_message; // shown under the menu (the last give)

template <class T> T rd(uint32_t a) { return *(const T *)(uintptr_t)a; }

enum Row { kGod, kAmmo, kGrenades, kRefill, kWeapon, kGive, kClose, kRowN };

// The weapon definitions: *(*0x2D12D0) = {+1 byte count, +4 array of 32-byte entries}. An entry: +0/+4 the model's
// hash pair (FUN_001d4f30 waits for it to be loaded), +8 the weapon data (+0x60 byte magazine size, +0x64 ammo type),
// +0x18 the HUD data (+0xC the name's text hash).
uint32_t weapon_defs(int *count)
{
    uint32_t p = rd<uint32_t>(0x002D12D0), defs = p ? rd<uint32_t>(p) : 0;
    *count = defs ? rd<uint8_t>(defs + 1) : 0;
    return defs ? rd<uint32_t>(defs + 4) : 0;
}

// The game's text by hash: FUN_001f3a80 (thiscall, ecx = text database *0x2D45D8; 0 until the text is loaded). The\n// weapon names are single-byte text ("ak 47", "spas 12", ...).
const char *game_text(uint32_t hash)
{
    uint32_t db = rd<uint32_t>(0x002D45D8);
    if (!db || !rd<uint8_t>(0x002F3B0E)) return nullptr;
    const char *r;
    __asm {
        mov ecx, db
        push hash
        mov eax, 0x001F3A80
        call eax
        mov r, eax
    }
    return r;
}

// A weapon can only be handed over when the current level has its first-person model: the swap loads
// Levels\Level_NN\fpguns\<code>.db (codes like "bg1_ak1"), and a missing file stops the game ("disc dirty or damaged"; each level ships
// a different set). The code is packed in the entry's model hash pair; FUN_000c44c0 (cdecl, edi = output, the two
// hash words) unpacks it (FUN_001d1c60 does the same, lower-cased up to the first space). The level number is byte
// [*0x2D45D4 + 0x5AEC] (as the path builder FUN_00147900 uses it).
std::string model_code(uint32_t lo, uint32_t hi)
{
    char buf[64] = {};
    char *dst = buf;
    __asm {
        push hi
        push lo
        mov edi, dst
        mov eax, 0x000C44C0
        call eax
        add esp, 8
    }
    std::string s;
    for (int i = 0; i < 12 && buf[i] && buf[i] != ' '; i++) s += (char)tolower((unsigned char)buf[i]);
    return s;
}

bool weapon_available(int type)
{
    static int level = -1;
    static std::vector<int8_t> known; // per type: -1 unknown, 0 no, 1 yes
    uint32_t store = rd<uint32_t>(0x002D45D4);
    int lv = store ? rd<int8_t>(store + 0x5AEC) : -1;
    int n = 0;
    uint32_t arr = weapon_defs(&n);
    if (lv < 0 || !arr || type < 0 || type >= n) return false;
    if (lv != level) {
        level = lv;
        known.assign(n, -1);
    }
    if ((int)known.size() < n) known.resize(n, -1);
    if (known[type] < 0) {
        std::string code = model_code(rd<uint32_t>(arr + type * 32), rd<uint32_t>(arr + type * 32 + 4));
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\Levels\\Level_%02d\\fpguns\\%hs.db", host_full_path(g_cfg.game_dir).c_str(), lv, code.c_str());
        known[type] = !code.empty() && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        logf("[cheats] weapon %d model '%s' in level %d: %s", type, code.c_str(), lv, known[type] ? "yes" : "no");
    }
    return known[type] == 1;
}

// The next weapon (dir +1 / -1) from `from` whose model this level has; -1 if none.
int next_available(int from, int dir)
{
    int n = 0;
    weapon_defs(&n);
    for (int k = 1; k <= n; k++) {
        int t = ((from + dir * k) % n + n) % n;
        if (weapon_available(t)) return t;
    }
    return -1;
}

std::wstring weapon_name(int type)
{
    int n = 0;
    uint32_t arr = weapon_defs(&n);
    if (!arr || type < 0 || type >= n) return L"-";
    uint32_t hud = rd<uint32_t>(arr + type * 32 + 0x18);
    const char *s = hud ? game_text(rd<uint32_t>(hud + 0xC)) : nullptr; // ASCII
    std::wstring out;
    for (int i = 0; s && s[i] && i < 40; i++) out += (wchar_t)towupper((unsigned char)s[i]);
    return out.empty() ? L"WEAPON " + std::to_wstring(type) : out;
}

// Hand the player a weapon the way picking one up does (FUN_001c9dd0, without the pickup in the world): the inventory
// (player +0x278) gets the new type +0x44, its magazine +0x40, the model hashes +0x30/+0x34, a delay +0x3C and the
// swap state +0x38 = 1 with +0x45 set; the current weapon user ([inv+0x24]) goes to state 0xB (lowering). The weapon
// update FUN_001f0650 then waits for the model and calls FUN_001e2920, which puts the new weapon into a free slot, or
// drops the current one as a pickup and takes its place. The ammo type's spare count (*0x2D45D4 + 0x2A8 + 2 x type)
// gets at least four magazines.
void give_weapon(uint32_t player, int type)
{
    int n = 0;
    uint32_t arr = weapon_defs(&n);
    uint32_t inv = player + 0x278, user = rd<uint32_t>(inv + 0x24), store = rd<uint32_t>(0x002D45D4);
    if (!arr || type < 0 || type >= n || !user || !store) return;
    if (!weapon_available(type)) {
        g_message = L"NOT IN THIS LEVEL";
        return;
    }
    if (rd<uint32_t>(inv + 0x38) != 0) {
        g_message = L"BUSY, TRY AGAIN";
        return;
    }
    uint32_t slots = rd<uint32_t>(inv + 0x20);
    for (int s = 0; slots && s < rd<uint8_t>(inv + 0x42); s++) {
        uint32_t w = rd<uint32_t>(slots + 4 * s);
        if (w && rd<int8_t>(w) == type) {
            g_message = L"ALREADY CARRIED";
            return;
        }
    }
    uint32_t e = arr + type * 32, data = rd<uint32_t>(e + 8);
    int clip = data ? rd<uint8_t>(data + 0x60) : 0;
    uint32_t ammo_type = data ? rd<uint32_t>(data + 0x64) : 99;
    if (clip <= 0) clip = 30;
    if (ammo_type < 16) {
        auto *spare = (uint16_t *)(uintptr_t)(store + 0x2A8 + 2 * ammo_type);
        if (*spare < clip * 4) *spare = (uint16_t)(clip * 4);
    }
    *(int8_t *)(uintptr_t)(inv + 0x44) = (int8_t)type;
    *(int16_t *)(uintptr_t)(inv + 0x40) = (int16_t)clip;
    *(uint32_t *)(uintptr_t)(inv + 0x30) = rd<uint32_t>(e);
    *(uint32_t *)(uintptr_t)(inv + 0x34) = rd<uint32_t>(e + 4);
    *(float *)(uintptr_t)(inv + 0x3C) = 0.5f;
    *(uint8_t *)(uintptr_t)(inv + 0x45) = 1;
    *(uint32_t *)(uintptr_t)(user + 0xD8) = 0xB;
    *(uint32_t *)(uintptr_t)(inv + 0x38) = 1;
    g_message = L"GIVEN: " + weapon_name(type);
    logf("[cheats] weapon %d given (magazine %d, ammo type %u)", type, clip, ammo_type);
}

} // namespace
} // namespace cheats

// 0x1EFBBB: dec word ptr [eax+0x18]; mov eax, [ebp+0xF4] (eax = [ebp+0xF4] on entry)
extern "C" __declspec(naked) void ammo_thunk()
{
    __asm {
        cmp ebp, dword ptr [g_cheat_ammo_user]
        je skip
        dec word ptr [eax + 0x18]
    skip:
        mov eax, dword ptr [ebp + 0xF4]
        ret
    }
}

// 0x1ED562 in the damage function FUN_001ed2b0 (esi = the character hit): mov eax, [esi+0x384]; the game skips the
// damage when it is not zero (its own "cannot be hurt" state), so god mode makes it 1 for the player.
extern "C" __declspec(naked) void damage_thunk()
{
    __asm {
        mov eax, dword ptr [esi + 0x384]
        cmp esi, dword ptr [g_cheat_god_player]
        jne done
        mov eax, 1
    done:
        ret
    }
}

namespace cheats {

void install()
{
    static const uint8_t kDamage[6] = {0x8B, 0x86, 0x84, 0x03, 0x00, 0x00}; // mov eax, [esi+0x384]
    auto *dmg = (uint8_t *)(uintptr_t)0x001ED562;
    if (memcmp(dmg, kDamage, sizeof kDamage) == 0) {
        dmg[0] = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)damage_thunk - (0x001ED562 + 5));
        memcpy(dmg + 1, &rel, 4);
        dmg[5] = 0x90;
    } else {
        logf("cheats: damage code at 0x001ED562 does not match this build; god mode only refills health");
    }
    static const uint8_t kAmmo[10] = {0x66, 0xFF, 0x48, 0x18, 0x8B, 0x85, 0xF4, 0x00, 0x00, 0x00};
    auto *code = (uint8_t *)(uintptr_t)0x001EFBBB;
    if (memcmp(code, kAmmo, sizeof kAmmo) == 0) {
        code[0] = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)ammo_thunk - (0x001EFBBB + 5));
        memcpy(code + 1, &rel, 4);
        memset(code + 5, 0x90, 5);
        g_installed_ammo = true;
    } else {
        logf("cheats: ammo code at 0x001EFBBB does not match this build; no infinite ammo");
    }
    g_god = g_cfg.cheat_god;
    g_ammo = g_cfg.cheat_ammo;
    g_grenades = g_cfg.cheat_grenades;
    logf("cheats: installed (menu on the CHEAT MENU key)");
}

void update(const uint8_t *ctrl)
{
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C);
    if (!player) return;
    uint32_t user = rd<uint32_t>(player + 0x29C);
    g_cheat_ammo_user = g_ammo && g_installed_ammo ? user : 0;
    g_cheat_god_player = g_god ? player : 0;
    if (g_god) *(float *)(uintptr_t)(player + 0x2F0) = max_health();
    if (g_grenades && rd<uint16_t>(player + 0x286) < 9) *(uint16_t *)(uintptr_t)(player + 0x286) = 9;
    if (g_give >= 0) {
        give_weapon(player, g_give);
        g_give = -1;
    }

}

bool god() { return g_god; }

float max_health()
{
    uint32_t game = rd<uint32_t>(0x002D45F0);
    uint32_t diff = game ? rd<uint32_t>(game + 0x2014C) : 0;
    return 1.0f / rd<float>(diff < 2 ? 0x00295E48 : 0x00295E44);
}

bool menu_open() { return g_open; }

void toggle_menu()
{
    g_open = !g_open && player_controller() != nullptr;
    logf("[cheats] menu %s", g_open ? "open" : "closed");
}

void menu_key(int key)
{
    if (!g_open) return;
    if (key == kUp) g_sel = (g_sel + kRowN - 1) % kRowN;
    else if (key == kDown) g_sel = (g_sel + 1) % kRowN;
    else if (key == kBack) g_open = false;
    else if (key == kLeft || key == kRight || key == kSelect) {
        switch (g_sel) {
        case kGod: g_god = !g_god; break;
        case kAmmo: g_ammo = !g_ammo; break;
        case kGrenades: g_grenades = !g_grenades; break;
        case kRefill:
            if (key == kSelect) {
                uint8_t *ctrl = player_controller();
                uint32_t player = ctrl ? *(const uint32_t *)(ctrl + 0x8C) : 0;
                if (player) {
                    *(float *)(uintptr_t)(player + 0x2F0) = max_health();
                    if (rd<int8_t>(player + 0x8E1) < 3) *(int8_t *)(uintptr_t)(player + 0x8E1) = 3;
                    logf("[cheats] health and health packs refilled");
                }
            }
            break;
        case kWeapon: { // only the weapons this level has a model for
            int t = next_available(g_weapon, key == kLeft ? -1 : 1);
            if (t >= 0) g_weapon = t;
            break;
        }
        case kGive:
            if (key == kSelect) g_give = g_weapon;
            break;
        case kClose:
            if (key == kSelect) g_open = false;
            break;
        }
        if (g_sel <= kGrenades) logf("[cheats] god %d, infinite ammo %d, infinite grenades %d", g_god, g_ammo, g_grenades);
    }
}

std::vector<MenuRow> menu_rows(int *selected)
{
    *selected = g_sel;
    auto on_off = [](bool on) { return std::wstring(on ? L"ON" : L"OFF"); };
    return {
        {L"GOD MODE", on_off(g_god)},
        {L"INFINITE AMMO", on_off(g_ammo)},
        {L"INFINITE GRENADES", on_off(g_grenades)},
        {L"REFILL HEALTH AND PACKS", L""},
        {L"WEAPON", weapon_available(g_weapon) || next_available(g_weapon, 1) < 0 ? L"< " + weapon_name(g_weapon) + L" >"
                                                                                  : L"< " + weapon_name(g_weapon = next_available(g_weapon, 1)) + L" >"},
        {L"GIVE WEAPON", g_message},
        {L"CLOSE", L""},
    };
}

} // namespace cheats
