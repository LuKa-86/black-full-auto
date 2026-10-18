// The port's own HUD additions, drawn over the finished picture in window pixels (so they are sharp at any render
// size) after present_backbuffer: the crosshair, the health percentage, the enemy markers, the hit marker, the grenade
// warning and the ammo warning (each described at its function). All of them only in play
// (player_controller(): a player update in the last 3 frames, so not in menus, pauses, hints or cutscenes).
//
// Crosshair (PC SETTINGS > DISPLAY > CROSSHAIR / CROSSHAIR COLOUR): rectangles or a ring, a dark outline then the
// colour, sizes in 1/1080 of the picture height. The game's own centre dot is hidden meanwhile (hide_game_crosshair in
// draw.cpp).
//
// Health percentage (PC SETTINGS > DISPLAY > HEALTH PERCENTAGE): health_fraction() (the value the game's bar shows) as
// "NN%" inside the health panel's lower strip, right of the health-pack crosses, like the number in the ammo panel.
// The panel is drawn in the game's 640 x 480 HUD space (lower strip x 45-139, y 55-81; the crosses end near x 107), so
// the text is placed there and mapped into the picture. The digits are rendered once per size with GDI (Bahnschrift
// Bold, widened like the HUD's numbers; Arial Black if missing) into a texture.
//
// Enemy markers (PC SETTINGS > DISPLAY > ENEMY MARKERS): the game's character list (manager *0x2D1980: count +0x79B4,
// array +0x79BC) holds the other characters; enemies have the AI flag +0x39C set and health +0x2F0 above 0 (allies:
// flag 0 and infinite health). The world matrix is at +0x70 (RenderWare layout: right, up, at, position at +0xA0;
// Y up, the position at the feet). A red triangle is drawn above the head, projected with the player's camera matrix
// [player+0xD0] (right, up, at, position) and the renderer's current horizontal field of view (*0x2D12B8 + 0x750).
// RenderWare's camera right vector points to screen left (the game's view matrices negate x), so screen x = -right.
#include "d3d/overlay.h"

#include "common.h"
#include "d3d/draw.h"
#include "d3d/host.h"
#include "input/cheats.h"
#include "input/hits.h"
#include "input/player_extras.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace xd3d {
namespace {

struct V {
    float x, y, z, rhw;
    D3DCOLOR c;
};
struct TV {
    float x, y, z, rhw;
    D3DCOLOR c;
    float u, v;
};

template <class T> T rd(uint32_t a) { return *(const T *)(uintptr_t)a; }

void quad(std::vector<V> &v, float x0, float y0, float x1, float y1, D3DCOLOR c)
{
    V a{x0 - 0.5f, y0 - 0.5f, 0, 1, c}, b{x1 - 0.5f, y0 - 0.5f, 0, 1, c}, d{x0 - 0.5f, y1 - 0.5f, 0, 1, c},
        e{x1 - 0.5f, y1 - 0.5f, 0, 1, c};
    v.insert(v.end(), {a, b, d, b, e, d});
}

// ---- crosshair ----

void crosshair(std::vector<V> &v, const RECT &pic)
{
    int style = g_cfg.crosshair;
    if (style <= 0) return;
    static const uint32_t kColours[] = {0xFFFFFF, 0x40FF40, 0xFF3030, 0xFFE040, 0x40E0FF};
    uint32_t colour = kColours[std::clamp(g_cfg.crosshair_colour, 0, 4)];
    float u = std::max(1.0f, (pic.bottom - pic.top) / 1080.0f);
    float cx = (pic.left + pic.right) * 0.5f, cy = (pic.top + pic.bottom) * 0.5f;
    float t = std::max(1.0f, std::round(2.0f * u)); // line thickness
    struct Box {
        float x0, y0, x1, y1;
    };
    std::vector<Box> boxes;
    auto box = [&](float x0, float y0, float x1, float y1) {
        boxes.push_back({std::round(x0), std::round(y0), std::round(x1), std::round(y1)});
    };
    auto dot = [&](float r) { box(cx - r, cy - r, cx + r, cy + r); };
    auto cross = [&](float gap, float len) {
        float h = t * 0.5f;
        box(cx - gap - len, cy - h, cx - gap, cy + h);
        box(cx + gap, cy - h, cx + gap + len, cy + h);
        box(cx - h, cy - gap - len, cx + h, cy - gap);
        box(cx - h, cy + gap, cx + h, cy + gap + len);
    };
    switch (style) {
    case 1: dot(std::max(1.5f, 2.5f * u)); break;            // DOT
    case 2: cross(3 * u, 7 * u); break;                      // SMALL CROSS
    case 3: cross(5 * u, 12 * u); break;                     // CROSS
    case 4: cross(7 * u, 18 * u); break;                     // LARGE CROSS
    case 5: cross(5 * u, 12 * u), dot(std::max(1.0f, 1.5f * u)); break; // CROSS AND DOT
    default: break;                                          // 6 CIRCLE: below
    }
    auto ring = [&](float r, float w, D3DCOLOR c) { // a circle as 48 quads
        const int n = 48;
        for (int i = 0; i < n; i++) {
            float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            float ri = r - w * 0.5f, ro = r + w * 0.5f;
            V p0{cx + cosf(a0) * ri - 0.5f, cy + sinf(a0) * ri - 0.5f, 0, 1, c};
            V p1{cx + cosf(a0) * ro - 0.5f, cy + sinf(a0) * ro - 0.5f, 0, 1, c};
            V p2{cx + cosf(a1) * ri - 0.5f, cy + sinf(a1) * ri - 0.5f, 0, 1, c};
            V p3{cx + cosf(a1) * ro - 0.5f, cy + sinf(a1) * ro - 0.5f, 0, 1, c};
            v.insert(v.end(), {p0, p1, p2, p1, p3, p2});
        }
    };
    const D3DCOLOR outline = 0x99000000, fill = 0xE6000000 | colour;
    float o = std::max(1.0f, std::round(u)); // outline width
    for (const Box &b : boxes) quad(v, b.x0 - o, b.y0 - o, b.x1 + o, b.y1 + o, outline);
    if (style == 6) ring(14 * u, t + 2 * o, outline), quad(v, cx - u - o, cy - u - o, cx + u + o, cy + u + o, outline);
    for (const Box &b : boxes) quad(v, b.x0, b.y0, b.x1, b.y1, fill);
    if (style == 6) ring(14 * u, t, fill), quad(v, cx - u, cy - u, cx + u, cy + u, fill);
}

// ---- enemy markers ----

void enemy_markers(std::vector<V> &v, const RECT &pic, const uint8_t *ctrl)
{
    if (!g_cfg.enemy_markers) return;
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C), mgr = rd<uint32_t>(0x002D1980), cam_obj = rd<uint32_t>(0x002D12B8);
    if (!player || !mgr || !cam_obj) return;
    const float *m = (const float *)(uintptr_t)(player + 0xD0); // right, up, at, position (rows of 4)
    float fov = rd<float>(cam_obj + 0x750);
    if (!(fov > 1.0f && fov < 170.0f)) return;
    float w = (float)(pic.right - pic.left), h = (float)(pic.bottom - pic.top);
    float tx = tanf(fov * 0.5f * 3.14159265f / 180.0f), ty = tx * h / w; // horizontal fov; the picture's aspect
    float u = std::max(1.0f, h / 1080.0f);
    int n = rd<int>(mgr + 0x79B4);
    uint32_t arr = rd<uint32_t>(mgr + 0x79BC);
    for (int i = 0; arr && i < n && i < 256; i++) {
        uint32_t e = rd<uint32_t>(arr + 4 * i);
        if (!e || !rd<uint8_t>(e + 0x39C)) continue;
        float hp = rd<float>(e + 0x2F0);
        if (!(hp > 0.0f && hp < 1e30f)) continue;
        float p[3] = {rd<float>(e + 0xA0) - m[12], rd<float>(e + 0xA4) + 2.05f - m[13], rd<float>(e + 0xA8) - m[14]};
        float x = p[0] * m[0] + p[1] * m[1] + p[2] * m[2];
        float y = p[0] * m[4] + p[1] * m[5] + p[2] * m[6];
        float z = p[0] * m[8] + p[1] * m[9] + p[2] * m[10];
        if (z < 0.5f || z > 150.0f) continue; // behind or too far
        float sx = pic.left + w * 0.5f * (1.0f - x / (z * tx)), sy = pic.top + h * 0.5f * (1.0f - y / (z * ty));
        if (sx < pic.left - 50 || sx > pic.right + 50 || sy < pic.top - 50 || sy > pic.bottom + 50) continue;
        float s = std::clamp(18.0f / sqrtf(z), 5.0f, 11.0f) * u; // half width: smaller far away
        float o = std::max(1.0f, std::round(u));
        auto tri = [&](float hw, float top, float bottom, D3DCOLOR c) {
            v.push_back({sx - hw - 0.5f, top - 0.5f, 0, 1, c});
            v.push_back({sx + hw - 0.5f, top - 0.5f, 0, 1, c});
            v.push_back({sx - 0.5f, bottom - 0.5f, 0, 1, c});
        };
        tri(s + 2 * o, sy - 1.6f * s - o, sy + 1.5f * o, 0xB0000000); // outline
        tri(s, sy - 1.6f * s, sy, 0xF0FF2A20);
    }
}

// A thick line as two triangles.
void line(std::vector<V> &v, float x0, float y0, float x1, float y1, float w, D3DCOLOR c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0.0f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    V a{x0 + nx - 0.5f, y0 + ny - 0.5f, 0, 1, c}, b{x1 + nx - 0.5f, y1 + ny - 0.5f, 0, 1, c}, d{x0 - nx - 0.5f, y0 - ny - 0.5f, 0, 1, c},
        e{x1 - nx - 0.5f, y1 - ny - 0.5f, 0, 1, c};
    v.insert(v.end(), {a, b, d, b, e, d});
}

// ---- hit marker ----

// `hit_marker`: four short diagonal strokes around the centre for a quarter of a second after the player's shot or
// grenade hurt an enemy (input/hits.cpp); red when it killed.
void hit_marker(std::vector<V> &v, const RECT &pic)
{
    uint32_t f = hits::last_frame(), now = g_frame_count;
    if (!g_cfg.hit_marker || !f || now - f > 15) return;
    float u = std::max(1.0f, (pic.bottom - pic.top) / 1080.0f);
    float cx = (pic.left + pic.right) * 0.5f, cy = (pic.top + pic.bottom) * 0.5f;
    float fade = 1.0f - (now - f) / 16.0f;
    bool kill = hits::last_was_kill();
    D3DCOLOR c = (D3DCOLOR)(fade * 255) << 24 | (kill ? 0xFF3020 : 0xFFFFFF), o = (D3DCOLOR)(fade * 160) << 24;
    float r0 = 9 * u, r1 = 19 * u, t = std::max(1.5f, 2.5f * u);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 4; i++) {
            float sx = i & 1 ? 1.0f : -1.0f, sy = i & 2 ? 1.0f : -1.0f, k = 0.7071f;
            float w = pass ? t : t + 2 * u;
            line(v, cx + sx * r0 * k, cy + sy * r0 * k, cx + sx * r1 * k, cy + sy * r1 * k, w, pass ? c : o);
        }
}

// ---- grenade warning ----

// `grenade_warning`: live grenades (projectile manager *0x2D1998: in use +0x3700 + i, kind +0x3600 + 4i, object +0x120 i
// with its position at +0xA0; the player's own (kind 3) only once it has left the hand, +0x116) within 10 m of the
// player get an arrow on a ring around the centre pointing their way, brighter and larger the closer they are.
void grenade_warning(std::vector<V> &v, const RECT &pic, const uint8_t *ctrl)
{
    if (!g_cfg.grenade_warning) return;
    uint32_t player = *(const uint32_t *)(ctrl + 0x8C), pm = rd<uint32_t>(0x002D1998);
    if (!player || !pm) return;
    const float *m = (const float *)(uintptr_t)(player + 0xD0);
    float h = (float)(pic.bottom - pic.top), u = std::max(1.0f, h / 1080.0f);
    float cx = (pic.left + pic.right) * 0.5f, cy = (pic.top + pic.bottom) * 0.5f, ring = 150 * u;
    for (int i = 0; i < 32; i++) {
        if (!rd<uint8_t>(pm + 0x3700 + i)) continue;
        uint32_t o = pm + 0x120 * i;
        if (rd<uint32_t>(pm + 0x3600 + 4 * i) == 3 && !rd<uint8_t>(o + 0x116)) continue; // in the player's hand
        float d[3] = {rd<float>(o + 0xA0) - rd<float>(player + 0xA0), rd<float>(o + 0xA4) - rd<float>(player + 0xA4),
                      rd<float>(o + 0xA8) - rd<float>(player + 0xA8)};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist < 10.0f)) continue;
        // direction in the camera's frame: screen right = -camera right, forward = camera at
        float x = -(d[0] * m[0] + d[1] * m[1] + d[2] * m[2]), z = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
        float a = atan2f(x, z); // 0 = ahead, clockwise on screen
        float near_k = 1.0f - dist / 10.0f, s = (10 + 8 * near_k) * u;
        float dx = sinf(a), dy = -cosf(a), px = cx + dx * ring, py = cy + dy * ring;
        auto arrow = [&](float grow, D3DCOLOR c) {
            float tip = s * 1.4f + grow, base = s * 0.9f + grow;
            V t{px + dx * tip - 0.5f, py + dy * tip - 0.5f, 0, 1, c};
            V l{px - dy * base - dx * s * 0.4f - 0.5f, py + dx * base - dy * s * 0.4f - 0.5f, 0, 1, c};
            V r{px + dy * base - dx * s * 0.4f - 0.5f, py - dx * base - dy * s * 0.4f - 0.5f, 0, 1, c};
            v.insert(v.end(), {t, l, r});
        };
        uint32_t alpha = (uint32_t)(150 + 105 * near_k);
        arrow(2 * u, 0xB0000000);
        arrow(0, alpha << 24 | 0xFF5020);
        quad(v, px - dx * s * 0.9f - 3 * u, py - dy * s * 0.9f - 3 * u, px - dx * s * 0.9f + 3 * u, py - dy * s * 0.9f + 3 * u,
             alpha << 24 | 0xFF5020); // a dot behind the arrow: the grenade
    }
}

// ---- text ----

// Glyphs rendered once per size with GDI into a texture; a few sizes are kept (the HUD's number, the menu).
const wchar_t kGlyphs[] = L" 0123456789%ABCDEFGHIJKLMNOPQRSTUVWXYZ:-()/.+'<>";
constexpr int kGlyphN = sizeof kGlyphs / sizeof kGlyphs[0] - 1;
struct Font {
    IDirect3DTexture9 *tex = nullptr;
    int size = 0, tw = 0, th = 0;
    int x[kGlyphN] = {}, w[kGlyphN] = {};
    uint32_t used = 0; // frame last used
};
Font g_fonts[4]; // the health number, the ammo warning, the cheat menu's two sizes

int glyph(wchar_t c)
{
    const wchar_t *p = wcschr(kGlyphs, towupper(c));
    return p && c ? (int)(p - kGlyphs) : 0;
}

Font *make_font(int size)
{
    for (Font &f : g_fonts)
        if (f.tex && f.size == size) {
            f.used = g_frame_count;
            return &f;
        }
    Font *slot = &g_fonts[0]; // the least recently used
    for (Font &f : g_fonts)
        if (!f.tex || f.used < slot->used) slot = &f;
    if (slot->tex) slot->tex->Release();
    *slot = Font{};
    Font &g = *slot;
    g.size = size;
    g.used = g_frame_count;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = CreateFontW(-size, (int)(size * 0.62f), 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift");
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[64] = L"";
    GetTextFaceW(dc, 64, face);
    if (_wcsicmp(face, L"Bahnschrift") != 0) {
        SelectObject(dc, old);
        DeleteObject(f);
        f = CreateFontW(-size, 0, 0, 0, FW_BLACK, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH, L"Arial Black");
        old = SelectObject(dc, f);
    }
    int x = 2;
    for (int i = 0; i < kGlyphN; i++) {
        SIZE s;
        GetTextExtentPoint32W(dc, &kGlyphs[i], 1, &s);
        g.x[i] = x;
        g.w[i] = s.cx;
        x += s.cx + 4;
        g.th = std::max(g.th, (int)s.cy + 4);
    }
    g.tw = x;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), g.tw, -g.th, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ oldb = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)g.tw * g.th * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < kGlyphN; i++) TextOutW(dc, g.x[i], 2, &kGlyphs[i], 1);
        GdiFlush();
        IDirect3DTexture9 *t = nullptr;
        if (SUCCEEDED(host3d::dev()->CreateTexture(g.tw, g.th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
                for (int y = 0; y < g.th; y++) {
                    const uint32_t *src = (const uint32_t *)bits + (size_t)y * g.tw;
                    uint32_t *dst = (uint32_t *)((uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
                    for (int i = 0; i < g.tw; i++) dst[i] = ((src[i] >> 8) & 0xFF) << 24 | 0xFFFFFF; // green = coverage
                }
                t->UnlockRect(0);
                g.tex = t;
            } else {
                t->Release();
            }
        }
        SelectObject(dc, oldb);
        DeleteObject(bmp);
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    if (!g.tex) {
        logf("[overlay] could not make the HUD font (size %d)", size);
        return nullptr;
    }
    return &g;
}

// Text quads for one font, drawn after the untextured shapes.
struct TextBatch {
    Font *font;
    std::vector<TV> v;
};

float text_width(const Font &f, const std::wstring &s)
{
    float w = 0;
    for (wchar_t c : s) w += f.w[glyph(c)];
    return w;
}

// x, y: top left; a dark shadow at offset o, then the colour.
void text(TextBatch &b, const std::wstring &s, float x, float y, D3DCOLOR colour, float o)
{
    const Font &f = *b.font;
    for (int pass = 0; pass < 2; pass++) {
        float cx = std::round(x) + (pass ? 0 : o), cy = std::round(y) + (pass ? 0 : o);
        D3DCOLOR c = pass ? colour : (colour & 0xFF000000) / 4 * 3 & 0xFF000000;
        for (wchar_t ch : s) {
            int g = glyph(ch);
            float u0 = (float)f.x[g] / f.tw, u1 = (float)(f.x[g] + f.w[g]) / f.tw;
            float x0 = cx - 0.5f, x1 = cx + f.w[g] - 0.5f, y0 = cy - 0.5f, y1 = cy + f.th - 0.5f;
            TV a{x0, y0, 0, 1, c, u0, 0}, bb{x1, y0, 0, 1, c, u1, 0}, d{x0, y1, 0, 1, c, u0, 1}, e{x1, y1, 0, 1, c, u1, 1};
            b.v.insert(b.v.end(), {a, bb, d, bb, e, d});
            cx += f.w[g];
        }
    }
}

// ---- health percentage ----

void health_text(std::vector<TextBatch> &batches, const RECT &pic)
{
    if (!g_cfg.health_percent) return;
    float f = health_fraction();
    int pct = std::clamp((int)std::lround(f * 100.0f), 0, 100);
    float sx = (pic.right - pic.left) / 640.0f, sy = (pic.bottom - pic.top) / 480.0f;
    Font *font = make_font(std::max(8, (int)std::lround(12.5f * sy))); // about the ammo numbers' height
    if (!font) return;
    std::wstring s = std::to_wstring(pct) + L"%";
    float right = pic.left + 134.0f * sx, mid = pic.top + 68.0f * sy; // right end of the strip's free part, its middle
    TextBatch b{font, {}};
    D3DCOLOR colour = f < 0.25f ? 0xFFE04030 : 0xFFD8D8D0; // the HUD's light grey; red when low
    text(b, s, right - text_width(*font, s), mid - font->th * 0.5f, colour, std::max(1.0f, std::round(sy * 0.6f)));
    batches.push_back(std::move(b));
}

// ---- ammo warning ----

// `ammo_warning`: under the crosshair, RELOAD when the magazine is at a quarter or less (or empty) and spare rounds are
// left, LOW AMMO when it is low with none spare, NO AMMO when both are empty (ammo_info, input/player_extras.cpp).
// Nothing while reloading.
void ammo_text(std::vector<TextBatch> &batches, const RECT &pic)
{
    Ammo a;
    if (!g_cfg.ammo_warning || !ammo_info(&a) || a.state == 4 || a.state == 5 || a.state == 6 || a.state == 8) return;
    bool low = a.mag * 4 <= a.size;
    if (!low) return;
    const wchar_t *s = a.mag <= 0 && a.spare <= 0 ? L"NO AMMO" : a.spare <= 0 ? L"LOW AMMO" : L"RELOAD";
    D3DCOLOR c = a.spare <= 0 ? 0xFFE04030 : 0xFFE8D070;
    float h = (float)(pic.bottom - pic.top), u = h / 1080.0f;
    Font *font = make_font(std::max(9, (int)std::lround(26 * u)));
    if (!font) return;
    TextBatch b{font, {}};
    float cx = (pic.left + pic.right) * 0.5f, y = pic.top + h * 0.5f + 70 * u;
    text(b, s, cx - text_width(*font, s) * 0.5f, y, c, std::max(1.0f, std::round(2 * u)));
    batches.push_back(std::move(b));
}

// ---- cheat menu ----

// A panel left of centre in the HUD's colours: dark translucent box, light grey rows, the chosen row green with a bar.
void cheat_menu(std::vector<V> &v, std::vector<TextBatch> &batches, const RECT &pic)
{
    if (!cheats::menu_open()) return;
    int sel = 0;
    std::vector<cheats::MenuRow> rows = cheats::menu_rows(&sel);
    float h = (float)(pic.bottom - pic.top), u = h / 1080.0f;
    Font *font = make_font(std::max(10, (int)std::lround(30 * u)));
    Font *title = make_font(std::max(12, (int)std::lround(42 * u)));
    if (!font || !title) return;
    float line = font->th * 1.25f, pad = 24 * u;
    const std::wstring help = L"UP/DOWN CHOOSE  ENTER CHANGE  ESC CLOSE";
    // As wide as the widest line (a row's "> label", a gap and its value, or the help line), at least 640/1080 of
    // the height, so no text reaches past the panel.
    float need = std::max(text_width(*font, help), text_width(*title, L"CHEATS"));
    for (const cheats::MenuRow &r : rows)
        need = std::max(need, text_width(*font, L"> " + r.label) + (r.value.empty() ? 0.0f : 40 * u + text_width(*font, r.value)));
    float w = std::max(640 * u, need + 2 * pad), x0 = pic.left + (pic.right - pic.left) * 0.5f - w * 0.5f;
    float total = title->th + pad + line * rows.size() + font->th + pad * 3;
    float y0 = pic.top + h * 0.5f - total * 0.5f;
    quad(v, x0, y0, x0 + w, y0 + total, 0xC8080A08);                       // panel
    quad(v, x0, y0, x0 + w, y0 + std::max(1.0f, 2 * u), 0xFFB8B8B0);        // top rule
    quad(v, x0, y0 + total - std::max(1.0f, 2 * u), x0 + w, y0 + total, 0xFFB8B8B0);
    TextBatch tb{title, {}}, rb{font, {}};
    float o = std::max(1.0f, std::round(2 * u));
    text(tb, L"CHEATS", x0 + pad, y0 + pad, 0xFFD8D8D0, o);
    float y = y0 + pad + title->th + pad * 0.5f;
    for (size_t i = 0; i < rows.size(); i++) {
        bool on = (int)i == sel;
        if (on) quad(v, x0 + pad * 0.5f, y - line * 0.1f, x0 + w - pad * 0.5f, y + line * 0.9f, 0x60208020);
        D3DCOLOR c = on ? 0xFF70E060 : 0xFFD8D8D0;
        text(rb, (on ? L"> " : L"  ") + rows[i].label, x0 + pad, y, c, o);
        if (!rows[i].value.empty()) {
            D3DCOLOR vc = rows[i].value == L"OFF" ? 0xFF909088 : c;
            text(rb, rows[i].value, x0 + w - pad - text_width(*font, rows[i].value), y, vc, o);
        }
        y += line;
    }
    text(rb, help, x0 + pad, y + pad * 0.5f, 0xFF909088, o);
    batches.push_back(std::move(tb));
    batches.push_back(std::move(rb));
}

} // namespace

void draw_overlay(IDirect3DSurface9 *target, const RECT &frame, const RECT &pic)
{
    uint8_t *ctrl = player_controller();
    if (!ctrl) return;
    std::vector<V> v;
    std::vector<TextBatch> batches;
    enemy_markers(v, pic, ctrl);
    grenade_warning(v, pic, ctrl);
    crosshair(v, pic);
    hit_marker(v, pic);
    health_text(batches, pic);
    ammo_text(batches, pic);
    std::vector<V> menu_v;
    std::vector<TextBatch> menu_text;
    cheat_menu(menu_v, menu_text, pic);
    if (v.empty() && batches.empty() && menu_v.empty()) return;

    IDirect3DDevice9 *d = host3d::dev();
    IDirect3DStateBlock9 *sb = nullptr;
    d->CreateStateBlock(D3DSBT_ALL, &sb);
    IDirect3DSurface9 *old_rt = nullptr, *old_z = nullptr;
    d->GetRenderTarget(0, &old_rt);
    d->GetDepthStencilSurface(&old_z);
    d->SetRenderTarget(0, target);
    d->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{0, 0, (DWORD)frame.right, (DWORD)frame.bottom, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    for (D3DRENDERSTATETYPE rs : {D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE, D3DRS_SCISSORTESTENABLE,
                                  D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING})
        d->SetRenderState(rs, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    host3d::begin_scene();
    auto shapes = [&](const std::vector<V> &s) {
        if (s.empty()) return;
        d->SetTexture(0, nullptr);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(s.size() / 3), s.data(), sizeof(V));
    };
    auto texts = [&](const std::vector<TextBatch> &bs) {
        for (const TextBatch &b : bs) {
            if (b.v.empty() || !b.font || !b.font->tex) continue;
            d->SetTexture(0, b.font->tex);
            d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(b.v.size() / 3), b.v.data(), sizeof(TV));
        }
    };
    shapes(v);
    texts(batches);
    shapes(menu_v); // the menu over everything else
    texts(menu_text);
    d->SetRenderTarget(0, old_rt);
    d->SetDepthStencilSurface(old_z);
    if (sb) {
        sb->Apply();
        sb->Release();
    }
    forget_sent_shaders();
    if (old_rt) old_rt->Release();
    if (old_z) old_z->Release();
}

} // namespace xd3d