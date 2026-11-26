#include "sdl_ui.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ascii_render.hpp"
#include "bot.hpp"
#include "botched/map.hpp"
#include "ui_model.hpp"
#include "ui_playback.hpp"

namespace botched::ui {

namespace {

using client::order_code;

// ------------------------------------------------------------------ palette

constexpr SDL_Color C(int r, int g, int b, int a = 255) {
    return SDL_Color{static_cast<Uint8>(r), static_cast<Uint8>(g), static_cast<Uint8>(b), static_cast<Uint8>(a)};
}
constexpr SDL_Color kBg = C(12, 16, 24), kPanel = C(22, 29, 41), kPanelEdge = C(52, 66, 88), kInk = C(224, 232, 242),
                    kMuted = C(130, 146, 168), kGold = C(255, 205, 80), kGreen = C(110, 220, 140), kRed = C(240, 96, 96),
                    kCyan = C(90, 220, 230), kAmber = C(255, 170, 80), kViolet = C(190, 150, 255);

SDL_Color player_color(int id) {
    static const SDL_Color c[] = {C(150, 150, 150), C(236, 84, 84), C(84, 148, 244), C(96, 206, 116), C(206, 108, 236)};
    return c[id >= 1 && id <= 4 ? id : 0];
}
SDL_Color group_color(char g) {
    static const SDL_Color c[] = {C(80, 220, 230), C(226, 96, 206), C(240, 214, 90), C(120, 220, 120), C(110, 150, 250)};
    return c[((g - 'A') % 5 + 5) % 5];
}
SDL_Color shade(SDL_Color c, float f) {
    auto k = [&](Uint8 v) { return static_cast<Uint8>(std::clamp(static_cast<int>(v * f), 0, 255)); };
    return SDL_Color{k(c.r), k(c.g), k(c.b), c.a};
}
SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    auto k = [&](Uint8 x, Uint8 y) { return static_cast<Uint8>(x + (y - x) * t); };
    return SDL_Color{k(a.r, b.r), k(a.g, b.g), k(a.b, b.b), 255};
}

SDL_Color kind_color(ScrollLog::Kind k) {
    switch (k) {
        case ScrollLog::Header: return kCyan;
        case ScrollLog::Belt: return kAmber;
        case ScrollLog::Push: return kViolet;
        case ScrollLog::Good: return kGreen;
        case ScrollLog::Bad: return kRed;
        case ScrollLog::Notice: return kGold;
        default: return kInk;
    }
}

// ------------------------------------------------------------------ drawing toolkit

struct Gfx {
    SDL_Renderer* r = nullptr;
    TTF_Font* small = nullptr;
    TTF_Font* body = nullptr;
    TTF_Font* big = nullptr;
    TTF_Font* title = nullptr;
    struct Tex { SDL_Texture* t; int w, h; };
    std::unordered_map<std::string, Tex> cache;

    float alpha = 1.f;  // global opacity multiplier for sprites that fade
    SDL_Color fade(SDL_Color c) const { c.a = static_cast<Uint8>(c.a * alpha); return c; }
    void color(SDL_Color c) { c = fade(c); SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }
    void fill(Rect q, SDL_Color c) { color(c); SDL_Rect s{q.x, q.y, q.w, q.h}; SDL_RenderFillRect(r, &s); }
    void frame(Rect q, SDL_Color c, int t = 1) {
        for (int i = 0; i < t; ++i) { color(c); SDL_Rect s{q.x + i, q.y + i, q.w - 2 * i, q.h - 2 * i}; SDL_RenderDrawRect(r, &s); }
    }
    void poly(const SDL_FPoint* p, int n, SDL_Color c) {  // convex fan
        std::vector<SDL_Vertex> v(static_cast<std::size_t>(n));
        c = fade(c);
        for (int i = 0; i < n; ++i) v[static_cast<std::size_t>(i)] = SDL_Vertex{p[i], c, {0, 0}};
        std::vector<int> idx;
        for (int i = 1; i + 1 < n; ++i) { idx.push_back(0); idx.push_back(i); idx.push_back(i + 1); }
        SDL_RenderGeometry(r, nullptr, v.data(), n, idx.data(), static_cast<int>(idx.size()));
    }
    void tri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c) {
        SDL_FPoint p[3] = {{x1, y1}, {x2, y2}, {x3, y3}};
        poly(p, 3, c);
    }
    void line(float x1, float y1, float x2, float y2, float w, SDL_Color c) {
        float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) return;
        float nx = -dy / len * w / 2, ny = dx / len * w / 2;
        SDL_FPoint p[4] = {{x1 + nx, y1 + ny}, {x2 + nx, y2 + ny}, {x2 - nx, y2 - ny}, {x1 - nx, y1 - ny}};
        poly(p, 4, c);
    }
    void disc(float cx, float cy, float rad, SDL_Color c, int seg = 28) {
        std::vector<SDL_FPoint> p(static_cast<std::size_t>(seg));
        for (int i = 0; i < seg; ++i) {
            float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
            p[static_cast<std::size_t>(i)] = {cx + std::cos(a) * rad, cy + std::sin(a) * rad};
        }
        poly(p.data(), seg, c);
    }
    void ring(float cx, float cy, float rad, float w, SDL_Color c, int seg = 32) {
        for (int i = 0; i < seg; ++i) {
            float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg), a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(seg);
            line(cx + std::cos(a0) * rad, cy + std::sin(a0) * rad, cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, w, c);
        }
    }

    Tex tex(const std::string& s, SDL_Color c, TTF_Font* f) {
        char key[64];
        std::snprintf(key, sizeof key, "%p|%02x%02x%02x|", static_cast<void*>(f), c.r, c.g, c.b);
        std::string k = std::string(key) + s;
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        if (cache.size() > 700) clear_cache();
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if (!surf) return {nullptr, 0, 0};
        Tex t{SDL_CreateTextureFromSurface(r, surf), surf->w, surf->h};
        SDL_FreeSurface(surf);
        cache[k] = t;
        return t;
    }
    void clear_cache() {
        for (auto& kv : cache) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
        cache.clear();
    }
    // align: 0 left, 1 centre, 2 right (about x). Returns the drawn width.
    int text(int x, int y, const std::string& s, SDL_Color c, TTF_Font* f, int align = 0) {
        if (s.empty()) return 0;
        Tex t = tex(s, c, f);
        if (!t.t) return 0;
        SDL_Rect d{align == 1 ? x - t.w / 2 : align == 2 ? x - t.w : x, y, t.w, t.h};
        SDL_SetTextureAlphaMod(t.t, static_cast<Uint8>(255 * alpha));
        SDL_RenderCopy(r, t.t, nullptr, &d);
        return t.w;
    }
    int text_w(const std::string& s, TTF_Font* f) { int w = 0, h = 0; TTF_SizeUTF8(f, s.c_str(), &w, &h); return w; }
    int line_h(TTF_Font* f) { return TTF_FontLineSkip(f); }
};

std::vector<std::string> font_dirs() {
    std::vector<std::string> dirs = {"/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/",
                                     "/usr/share/fonts/dejavu/", "/usr/share/fonts/liberation/", "/usr/share/fonts/truetype/liberation/"};
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home && *home) dirs.push_back(std::string(home) + "/Library/Fonts/");
    for (const char* d : {"/Library/Fonts/", "/opt/homebrew/share/fonts/", "/usr/local/share/fonts/", "/System/Library/Fonts/Supplemental/",
                          "/System/Library/Fonts/"})
        dirs.push_back(d);
#endif
    return dirs;
}

TTF_Font* open_font(const char* const* names, int size) {
    if (const char* o = std::getenv("BOTCHED_FONT")) if (*o) if (TTF_Font* f = TTF_OpenFont(o, size)) return f;
    static const std::vector<std::string> dirs = font_dirs();
    for (const char* const* n = names; *n; ++n)
        for (const std::string& d : dirs)
            if (TTF_Font* f = TTF_OpenFont((d + *n).c_str(), size)) return f;
    return nullptr;
}

// ------------------------------------------------------------------ directional helpers

struct V2 { float x, y; };
V2 fwd(Dir d) {
    switch (d) { case Dir::N: return {0, -1}; case Dir::E: return {1, 0}; case Dir::S: return {0, 1}; case Dir::W: return {-1, 0}; }
    return {0, -1};
}

// Local frame: u along `d`, v to the side, in units of the cell size, about the cell centre.
struct Frame {
    float cx, cy, s;
    V2 f;
    SDL_FPoint at(float u, float v) const { return {cx + (f.x * u - f.y * v) * s, cy + (f.y * u + f.x * v) * s}; }
};

void quad_local(Gfx& g, const Frame& fr, float u0, float u1, float v0, float v1, SDL_Color c) {
    SDL_FPoint p[4] = {fr.at(u0, v0), fr.at(u1, v0), fr.at(u1, v1), fr.at(u0, v1)};
    g.poly(p, 4, c);
}

// ------------------------------------------------------------------ sprites

void draw_crate(Gfx& g, float cx, float cy, float size) {
    const float h = size / 2;
    g.fill({static_cast<int>(cx - h - 1), static_cast<int>(cy - h - 1), static_cast<int>(size + 2), static_cast<int>(size + 2)}, C(60, 36, 12));
    g.fill({static_cast<int>(cx - h), static_cast<int>(cy - h), static_cast<int>(size), static_cast<int>(size)}, C(186, 128, 58));
    g.line(cx - h, cy - h, cx + h, cy + h, std::max(1.f, size * 0.09f), C(128, 82, 34));
    g.line(cx + h, cy - h, cx - h, cy + h, std::max(1.f, size * 0.09f), C(128, 82, 34));
    g.fill({static_cast<int>(cx - h), static_cast<int>(cy - h), static_cast<int>(size), std::max(1, static_cast<int>(size * 0.1f))}, C(226, 178, 104));
    g.frame({static_cast<int>(cx - h), static_cast<int>(cy - h), static_cast<int>(size), static_cast<int>(size)}, C(104, 66, 26), std::max(1, static_cast<int>(size * 0.07f)));
}

void draw_gear(Gfx& g, float cx, float cy, float rad, SDL_Color c) {
    for (int i = 0; i < 8; ++i) {
        float a = 0.7853982f * static_cast<float>(i);
        Frame fr{cx, cy, rad * 2.f, V2{std::cos(a), std::sin(a)}};
        quad_local(g, fr, 0.30f, 0.56f, -0.12f, 0.12f, c);
    }
    g.disc(cx, cy, rad * 0.72f, c, 20);
    g.disc(cx, cy, rad * 0.30f, C(20, 24, 34), 14);
}

void draw_robot(Gfx& g, float cx, float cy, float s, int id, V2 heading, bool carrying, int damage, bool mine, Uint32 ticks) {
    Frame fr{cx, cy, s, heading};
    SDL_Color body = player_color(id), dark = shade(body, 0.45f), light = mix(body, C(255, 255, 255), 0.45f);
    if (mine) {
        float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ticks) * 0.006f);
        g.ring(cx, cy, s * 0.47f, 2.5f, mix(C(255, 255, 255), body, 0.4f + 0.5f * pulse), 36);
    }
    g.disc(cx + s * 0.03f, cy + s * 0.05f, s * 0.36f, C(0, 0, 0, 90));  // shadow
    for (float side : {-1.f, 1.f}) {  // treads
        quad_local(g, fr, -0.38f, 0.38f, side * 0.40f - 0.09f, side * 0.40f + 0.09f, C(24, 26, 32));
        for (int i = 0; i < 5; ++i) {
            float u = -0.32f + 0.16f * static_cast<float>(i);
            quad_local(g, fr, u, u + 0.05f, side * 0.40f - 0.09f, side * 0.40f + 0.09f, C(70, 74, 86));
        }
    }
    quad_local(g, fr, -0.32f, 0.32f, -0.30f, 0.30f, dark);
    quad_local(g, fr, -0.29f, 0.29f, -0.27f, 0.27f, body);
    quad_local(g, fr, 0.10f, 0.29f, -0.27f, 0.27f, light);  // brighter front plate
    SDL_FPoint nose[3] = {fr.at(0.46f, 0), fr.at(0.30f, -0.15f), fr.at(0.30f, 0.15f)};
    g.poly(nose, 3, C(250, 250, 250));
    g.disc(cx, cy, s * 0.15f, C(18, 20, 28));
    g.disc(cx, cy, s * 0.13f, C(236, 240, 246));
    char n[4];
    std::snprintf(n, sizeof n, "%d", id);
    int tw = g.text_w(n, g.body), th = g.line_h(g.body);
    g.text(static_cast<int>(cx) - tw / 2, static_cast<int>(cy) - th / 2, n, C(20, 24, 34), g.body);
    if (damage >= 1) {  // scorch marks
        g.line(fr.at(-0.15f, -0.2f).x, fr.at(-0.15f, -0.2f).y, fr.at(0.0f, -0.05f).x, fr.at(0.0f, -0.05f).y, 2, C(30, 20, 14));
        g.line(fr.at(0.0f, -0.05f).x, fr.at(0.0f, -0.05f).y, fr.at(-0.08f, 0.1f).x, fr.at(-0.08f, 0.1f).y, 2, C(30, 20, 14));
        g.disc(fr.at(-0.2f, 0.18f).x, fr.at(-0.2f, 0.18f).y, s * 0.06f, C(40, 28, 20));
    }
    if (carrying) draw_crate(g, cx + s * 0.27f, cy - s * 0.27f, s * 0.32f);
}

void draw_wall(Gfx& g, int x, int y, int s) {
    g.fill({x, y, s, s}, C(86, 94, 110));
    const SDL_Color mortar = C(50, 56, 70);
    for (int row = 0; row < 3; ++row) {
        int y0 = y + row * s / 3;
        if (row) g.fill({x, y0, s, 2}, mortar);
        int off = (row == 1) ? s / 4 : s / 2;
        for (int bx : {off, off + (row == 1 ? s / 2 : 0)})
            if (bx > 0 && bx < s) g.fill({x + bx, y0, 2, s / 3}, mortar);
    }
    g.fill({x, y, s, 2}, C(132, 142, 160));
    g.fill({x, y, 2, s}, C(120, 130, 148));
    g.fill({x, y + s - 2, s, 2}, C(40, 46, 58));
    g.fill({x + s - 2, y, 2, s}, C(40, 46, 58));
}

void draw_pit(Gfx& g, int x, int y, int s) {
    g.fill({x, y, s, s}, C(8, 8, 12));
    float cx = x + s / 2.f, cy = y + s / 2.f;
    g.disc(cx, cy, s * 0.46f, C(70, 14, 18));
    g.disc(cx, cy, s * 0.36f, C(34, 6, 8));
    g.disc(cx, cy, s * 0.24f, C(10, 2, 3));
    g.disc(cx, cy, s * 0.12f, C(0, 0, 0));
    g.frame({x, y, s, s}, C(150, 34, 34), 2);
}

void draw_conveyor(Gfx& g, int x, int y, int s, Dir eff, char group, Uint32 ticks, float glow = 0.f) {
    g.fill({x, y, s, s}, C(38, 44, 56));
    SDL_Color gc = group_color(group);
    Frame fr{x + s / 2.f, y + s / 2.f, static_cast<float>(s), fwd(eff)};
    quad_local(g, fr, -0.5f, 0.5f, -0.46f, -0.38f, shade(gc, 0.75f));  // rails
    quad_local(g, fr, -0.5f, 0.5f, 0.38f, 0.46f, shade(gc, 0.75f));
    float phase = std::fmod(static_cast<float>(ticks) / 900.f, 1.f);
    for (int i = -1; i < 3; ++i) {
        float u = -0.5f + (static_cast<float>(i) + phase) / 3.f * 1.0f + 0.1f;
        if (u < -0.42f || u > 0.42f) continue;
        SDL_FPoint a = fr.at(u - 0.12f, -0.26f), m = fr.at(u + 0.1f, 0), b = fr.at(u - 0.12f, 0.26f);
        g.line(a.x, a.y, m.x, m.y, s * 0.11f, gc);
        g.line(b.x, b.y, m.x, m.y, s * 0.11f, gc);
    }
    if (glow > 0.f) g.frame({x, y, s, s}, C(255, 200, 90, static_cast<int>(230 * std::min(1.f, glow))), std::max(2, s / 14));
}

void draw_console(Gfx& g, int x, int y, int s, char group) {
    SDL_Color gc = group_color(group);
    g.fill({x, y, s, s}, C(30, 38, 48));
    int in = s / 8;
    g.fill({x + in, y + in, s - 2 * in, s - 2 * in}, C(70, 78, 98));
    g.frame({x + in, y + in, s - 2 * in, s - 2 * in}, C(110, 120, 144), 2);
    int sx = x + in * 2, sy = y + in * 2, sw = s - 4 * in, sh = s * 38 / 100;
    g.fill({sx, sy, sw, sh}, C(6, 24, 22));
    g.frame({sx, sy, sw, sh}, gc, 1);
    char t[2] = {group, 0};
    g.text(x + s / 2, sy + (sh - g.line_h(g.small)) / 2, t, gc, g.small, 1);
    for (int i = 0; i < 3; ++i) g.disc(static_cast<float>(sx + sw * (2 * i + 1) / 6), static_cast<float>(sy + sh + s * 0.17f), s * 0.07f, i == 1 ? gc : shade(gc, 0.5f), 14);
    // lever
    g.line(x + s * 0.5f, y + s * 0.88f, x + s * 0.66f, y + s * 0.70f, 3, C(200, 204, 214));
    g.disc(x + s * 0.66f, y + s * 0.70f, s * 0.05f, C(235, 70, 70), 12);
}

void draw_spawn(Gfx& g, int x, int y, int s, int owner) {
    SDL_Color pc = player_color(owner);
    g.fill({x, y, s, s}, C(26, 32, 42));
    float cx = x + s / 2.f, cy = y + s / 2.f;
    g.ring(cx, cy, s * 0.34f, s * 0.07f, pc, 36);
    g.ring(cx, cy, s * 0.20f, s * 0.04f, shade(pc, 0.7f), 28);
    char t[4];
    std::snprintf(t, sizeof t, "S%d", owner);
    g.text(static_cast<int>(cx), static_cast<int>(cy) - g.line_h(g.small) / 2, t, pc, g.small, 1);
}

void draw_dock(Gfx& g, int x, int y, int s, int owner) {
    SDL_Color pc = player_color(owner);
    g.fill({x, y, s, s}, shade(pc, 0.22f));
    int b = std::max(3, s / 12);
    g.frame({x, y, s, s}, pc, b);
    // hazard stripes along the inside edge
    SDL_Color stripe = shade(pc, 0.6f);
    int step = std::max(4, s / 8);
    for (int i = b; i < s - b - step; i += step * 2) {
        g.fill({x + i, y + b, step, std::max(2, s / 24)}, stripe);
        g.fill({x + i, y + s - b - std::max(2, s / 24), step, std::max(2, s / 24)}, stripe);
    }
    float cx = x + s / 2.f, cy = y + s / 2.f;
    g.fill({static_cast<int>(cx - s * 0.2f), static_cast<int>(cy - s * 0.04f), static_cast<int>(s * 0.4f), static_cast<int>(s * 0.24f)}, C(18, 22, 30));
    g.frame({static_cast<int>(cx - s * 0.2f), static_cast<int>(cy - s * 0.04f), static_cast<int>(s * 0.4f), static_cast<int>(s * 0.24f)}, pc, 2);
    g.line(cx, cy - s * 0.28f, cx, cy + s * 0.02f, s * 0.07f, pc);
    g.tri(cx - s * 0.13f, cy - s * 0.04f, cx + s * 0.13f, cy - s * 0.04f, cx, cy + s * 0.1f, pc);
    char t[4];
    std::snprintf(t, sizeof t, "D%d", owner);
    g.text(x + s - 4, y + s - g.line_h(g.small) - 2, t, pc, g.small, 2);
}

void draw_dispenser(Gfx& g, int x, int y, int s, int id, bool loaded) {
    g.fill({x, y, s, s}, C(34, 38, 40));
    int in = s / 10;
    g.fill({x + in, y + in, s - 2 * in, s - 2 * in}, C(150, 118, 40));
    g.frame({x + in, y + in, s - 2 * in, s - 2 * in}, C(214, 176, 70), 2);
    int wx = x + s * 22 / 100, wy = y + s * 20 / 100, ww = s * 56 / 100, wh = s * 44 / 100;
    g.fill({wx, wy, ww, wh}, loaded ? C(70, 52, 20) : C(16, 14, 10));
    g.frame({wx, wy, ww, wh}, C(80, 60, 20), 2);
    if (loaded) draw_crate(g, wx + ww / 2.f, wy + wh / 2.f, wh * 0.78f);
    else {  // empty: faint cross in the window
        g.line(static_cast<float>(wx + ww / 4), static_cast<float>(wy + wh / 4), static_cast<float>(wx + ww * 3 / 4), static_cast<float>(wy + wh * 3 / 4), 2, C(70, 58, 30));
        g.line(static_cast<float>(wx + ww * 3 / 4), static_cast<float>(wy + wh / 4), static_cast<float>(wx + ww / 4), static_cast<float>(wy + wh * 3 / 4), 2, C(70, 58, 30));
    }
    g.fill({x + s * 30 / 100, y + s * 72 / 100, s * 40 / 100, s * 12 / 100}, C(20, 18, 14));  // chute
    char t[4];
    std::snprintf(t, sizeof t, "G%d", id);
    g.text(x + s / 2, y + s - g.line_h(g.small) - 1, t, C(255, 232, 160), g.small, 1);
}

void draw_laser(Gfx& g, int x, int y, int s, Dir d) {
    g.fill({x, y, s, s}, C(40, 26, 30));
    float cx = x + s / 2.f, cy = y + s / 2.f;
    Frame fr{cx, cy, static_cast<float>(s), fwd(d)};
    g.disc(cx, cy, s * 0.30f, C(96, 40, 44));
    quad_local(g, fr, 0.0f, 0.46f, -0.08f, 0.08f, C(210, 220, 230));
    g.disc(cx, cy, s * 0.14f, C(255, 90, 90));
}

// G2-A proximity trap: a squat mine-like device. Armed: owner-coloured ring and spikes that pulse;
// disarmed: grey and flat. The owner colour is white for a neutral trap.
SDL_Color trap_color(int owner) { return owner >= 1 && owner <= kMaxPlayers ? player_color(owner) : C(225, 228, 236); }

void draw_trap(Gfx& g, int x, int y, int s, int id, bool armed, int owner, Uint32 now) {
    g.fill({x, y, s, s}, C(28, 30, 38));
    const float cx = x + s / 2.f, cy = y + s / 2.f;
    const SDL_Color oc = trap_color(owner);
    const float pulse = armed ? 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.008f) : 0.f;
    if (armed) {
        for (int i = 0; i < 8; ++i) {  // spikes
            const float a = 6.2831853f * static_cast<float>(i) / 8.f + 0.39f;
            const float r0 = s * 0.30f, r1 = s * (0.42f + 0.05f * pulse);
            g.line(cx + std::cos(a) * r0, cy + std::sin(a) * r0, cx + std::cos(a) * r1, cy + std::sin(a) * r1, std::max(2.f, s * 0.07f), oc);
        }
    }
    g.disc(cx, cy, s * 0.30f, armed ? C(70, 32, 34) : C(44, 46, 54), 28);
    g.ring(cx, cy, s * 0.30f, std::max(2.f, s * 0.05f), armed ? oc : C(96, 100, 112), 28);
    g.disc(cx, cy, s * 0.12f, armed ? mix(C(255, 70, 70), C(255, 200, 120), pulse) : C(70, 74, 84), 16);
    g.text(x + s / 2, y + 1, "X" + std::to_string(id), armed ? oc : C(110, 114, 126), g.small, 1);
    if (!armed) g.text(x + s / 2, y + s - g.line_h(g.small) - 1, "off", C(110, 114, 126), g.small, 1);
}

void draw_trap_console(Gfx& g, int x, int y, int s, int id, bool armed, int owner) {
    const SDL_Color oc = trap_color(owner);
    g.fill({x, y, s, s}, C(30, 38, 48));
    const int in = s / 8;
    g.fill({x + in, y + in, s - 2 * in, s - 2 * in}, C(70, 78, 98));
    g.frame({x + in, y + in, s - 2 * in, s - 2 * in}, C(110, 120, 144), 2);
    const int sx = x + in * 2, sy = y + in * 2, sw = s - 4 * in, sh = s * 38 / 100;
    g.fill({sx, sy, sw, sh}, C(28, 10, 12));
    g.frame({sx, sy, sw, sh}, oc, 1);
    g.text(x + s / 2, sy + (sh - g.line_h(g.small)) / 2, "K" + std::to_string(id), oc, g.small, 1);
    g.disc(x + s * 0.5f, y + s * 0.74f, s * 0.11f, armed ? C(235, 70, 70) : C(90, 94, 106), 14);   // arm button
    g.ring(x + s * 0.5f, y + s * 0.74f, s * 0.11f, 2, C(200, 204, 214), 14);
}

// G2-B charging turret: a square emplacement with a barrel on each side. Two pips show the charge (0, 1, 2);
// at 2 the whole thing glows, because the next tick fires. The owner colour is white for a neutral turret.
void draw_turret(Gfx& g, int x, int y, int s, int id, int charge, int owner, Uint32 now) {
    g.fill({x, y, s, s}, C(28, 30, 38));
    const float cx = x + s / 2.f, cy = y + s / 2.f;
    const SDL_Color oc = trap_color(owner);
    const bool ready = charge >= 2;
    const float pulse = ready ? 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.014f) : 0.f;
    for (int i = 0; i < 4; ++i) {  // barrels N, E, S, W
        const float a = 6.2831853f * static_cast<float>(i) / 4.f;
        g.line(cx + std::cos(a) * s * 0.18f, cy + std::sin(a) * s * 0.18f, cx + std::cos(a) * s * 0.46f, cy + std::sin(a) * s * 0.46f,
               std::max(3.f, s * 0.11f), ready ? mix(oc, C(255, 255, 255), pulse) : C(120, 126, 142));
    }
    g.disc(cx, cy, s * 0.27f, C(44, 48, 60), 24);
    g.ring(cx, cy, s * 0.27f, std::max(2.f, s * 0.06f), oc, 24);
    for (int i = 0; i < 2; ++i) {
        const bool lit = charge > i;
        g.disc(cx + (i == 0 ? -0.09f : 0.09f) * s, cy, s * 0.065f, lit ? mix(C(255, 90, 60), C(255, 230, 150), pulse) : C(66, 70, 82), 12);
    }
    g.text(x + s / 2, y + 1, "Y" + std::to_string(id), oc, g.small, 1);
    if (ready) g.text(x + s / 2, y + s - g.line_h(g.small) - 1, "READY", mix(C(255, 200, 120), C(255, 255, 255), pulse), g.small, 1);
}

// G2-C tar generator: a squat tank with a dripping nozzle, in tar purple. The eight tiles around it are tar.
void draw_tar_generator(Gfx& g, int x, int y, int s, Uint32 now) {
    g.fill({x, y, s, s}, C(28, 24, 36));
    const float cx = x + s / 2.f, cy = y + s / 2.f;
    g.disc(cx, cy, s * 0.36f, C(46, 30, 60), 24);
    g.ring(cx, cy, s * 0.36f, std::max(2.f, s * 0.06f), C(150, 80, 190), 24);
    g.disc(cx, cy - s * 0.04f, s * 0.20f, C(18, 12, 24), 20);
    const float drip = std::fmod(static_cast<float>(now) * 0.0012f, 1.f);
    g.disc(cx, cy + s * (0.10f + 0.22f * drip), s * 0.06f * (1.f - 0.4f * drip), C(120, 60, 160), 10);
    g.text(x + s / 2, y + 1, "TAR", C(190, 120, 230), g.small, 1);
}

// Tar tile overlay: a dark purple film with slow bubbles. Drawn over the floor on the eight tiles around a generator.
void draw_tar_tile(Gfx& g, int x, int y, int s, int seed, Uint32 now) {
    g.fill({x, y, s, s}, C(80, 30, 115, 140));
    g.frame({x + 1, y + 1, s - 2, s - 2}, C(160, 90, 210, 110), 1);
    for (int i = 0; i < 2; ++i) {
        const float ph = std::fmod(static_cast<float>(now) * 0.0006f + 0.37f * static_cast<float>(seed + 3 * i), 1.f);
        const float bx = x + s * (0.25f + 0.5f * static_cast<float>((seed * 7 + i * 5) % 10) / 10.f);
        const float by = y + s * (0.85f - 0.6f * ph);
        g.ring(bx, by, s * 0.05f * (0.5f + ph), 1, C(200, 140, 240, static_cast<int>(180 * (1.f - ph))), 8);
    }
}

// G2-B turret control pad: step on it to claim the turret. The ring shows who owns it now.
void draw_turret_pad(Gfx& g, int x, int y, int s, int id, int owner) {
    const SDL_Color oc = trap_color(owner);
    g.fill({x, y, s, s}, C(34, 36, 50));
    g.frame({x + 2, y + 2, s - 4, s - 4}, oc, 2);
    const float cx = x + s / 2.f, cy = y + s / 2.f;
    g.ring(cx, cy, s * 0.26f, std::max(2.f, s * 0.05f), oc, 24);
    g.disc(cx, cy, s * 0.08f, oc, 12);
    g.text(x + s / 2, y + s - g.line_h(g.small) - 1, "P" + std::to_string(id), oc, g.small, 1);
}

void draw_floor(Gfx& g, int x, int y, int s, bool alt) {
    g.fill({x, y, s, s}, alt ? C(34, 42, 54) : C(30, 37, 48));
    g.frame({x, y, s, s}, C(24, 30, 40), 1);
}

// ------------------------------------------------------------------ order icons

void draw_order_icon(Gfx& g, int order, float cx, float cy, float s, SDL_Color c) {
    const float w = s * 0.11f;
    auto arrow = [&](float x1, float y1, float x2, float y2) {  // shaft + head, in cell fractions about centre
        float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
        float ux = dx / len, uy = dy / len, hs = 0.15f;
        g.line(cx + x1 * s, cy + y1 * s, cx + (x2 - ux * hs) * s, cy + (y2 - uy * hs) * s, w, c);
        g.tri(cx + x2 * s, cy + y2 * s, cx + (x2 - ux * hs - uy * hs) * s, cy + (y2 - uy * hs + ux * hs) * s,
              cx + (x2 - ux * hs + uy * hs) * s, cy + (y2 - uy * hs - ux * hs) * s, c);
    };
    auto seg = [&](float x1, float y1, float x2, float y2) { g.line(cx + x1 * s, cy + y1 * s, cx + x2 * s, cy + y2 * s, w, c); };
    switch (order) {
        case 0: arrow(0, 0.32f, 0, -0.34f); break;                                       // F
        case 1: arrow(-0.14f, 0.34f, -0.14f, -0.34f); arrow(0.16f, 0.34f, 0.16f, -0.34f); break;  // F2
        case 2: arrow(0, -0.32f, 0, 0.34f); break;                                       // B
        case 3: seg(0.1f, 0.34f, 0.1f, -0.12f); arrow(0.1f, -0.12f, -0.36f, -0.12f); break;       // L
        case 4: seg(-0.1f, 0.34f, -0.1f, -0.12f); arrow(-0.1f, -0.12f, 0.36f, -0.12f); break;     // R
        case 5:  // U-turn
            seg(-0.2f, 0.34f, -0.2f, -0.06f); seg(-0.2f, -0.06f, -0.1f, -0.26f); seg(-0.1f, -0.26f, 0.1f, -0.26f);
            seg(0.1f, -0.26f, 0.2f, -0.06f); arrow(0.2f, -0.06f, 0.2f, 0.34f);
            break;
        case 6:  // drop: crate with arrow below
            g.fill({static_cast<int>(cx - s * 0.16f), static_cast<int>(cy - s * 0.36f), static_cast<int>(s * 0.32f), static_cast<int>(s * 0.28f)}, C(186, 128, 58));
            arrow(0, -0.02f, 0, 0.38f);
            break;
        case 7:  // toggle: lever
            g.fill({static_cast<int>(cx - s * 0.3f), static_cast<int>(cy + s * 0.14f), static_cast<int>(s * 0.6f), static_cast<int>(s * 0.16f)}, c);
            seg(0, 0.14f, 0.24f, -0.28f);
            g.disc(cx + s * 0.24f, cy - s * 0.28f, s * 0.09f, c, 14);
            break;
        case 8:  // wait: pause bars
            g.fill({static_cast<int>(cx - s * 0.22f), static_cast<int>(cy - s * 0.28f), static_cast<int>(s * 0.14f), static_cast<int>(s * 0.56f)}, c);
            g.fill({static_cast<int>(cx + s * 0.08f), static_cast<int>(cy - s * 0.28f), static_cast<int>(s * 0.14f), static_cast<int>(s * 0.56f)}, c);
            break;
    }
}

struct OrderKind { const char* token; SDL_Keycode key; const char* keyname; };
constexpr OrderKind kKinds[kOrderKinds] = {
    {"FORWARD_1", SDLK_f, "F"}, {"FORWARD_2", SDLK_2, "2"}, {"BACK_1", SDLK_b, "B"},
    {"TURN_LEFT", SDLK_l, "L"}, {"TURN_RIGHT", SDLK_r, "R"}, {"TURN_AROUND", SDLK_u, "U"},
    {"DROP", SDLK_d, "D"},      {"TOGGLE", SDLK_t, "T"},     {"WAIT", SDLK_w, "W"},
};
int kind_of(const std::string& token) {
    for (int i = 0; i < kOrderKinds; ++i) if (token == kKinds[i].token) return i;
    return -1;
}

// ------------------------------------------------------------------ the application

struct App {
    client::ClientOptions opt;
    client::ClientResult res;
    client::Connection conn;
    explicit App(const client::ClientOptions& options) : opt(options), conn(options.binary) {}
    std::string err;       // last network error
    std::string fatal;     // set when the session cannot continue
    std::string match_id, session;
    int me = 0;

    std::shared_ptr<const Map> map;
    std::unique_ptr<sim::Bot> bot;
    Layout lay = compute_layout(12, 10);

    std::string phase = "LOBBY";
    Json snap, status;
    int joined = 0, capacity = 0, bots = 0;
    std::vector<int> bot_ids;  // seats played by the server
    std::vector<std::string> bot_kinds;  // their personalities (G4), parallel to bot_ids
    bool is_bot(int id) const { return std::find(bot_ids.begin(), bot_ids.end(), id) != bot_ids.end(); }
    // G4: the personality the server reports for a bot seat ("" when it does not).
    std::string bot_kind(int id) const {
        for (std::size_t i = 0; i < bot_ids.size(); ++i)
            if (bot_ids[i] == id && i < bot_kinds.size()) return bot_kinds[i];
        return "";
    }
    bool finished_logged = false;
    std::vector<int> winners;
    // Match rules: from the live snapshot once started, from the lobby status before that.
    int target_score() const { return static_cast<int>((snap.is_null() ? status : snap).get_int("target_score").value_or(0)); }
    int round_cap() const { return static_cast<int>((snap.is_null() ? status : snap).get_int("rounds_total").value_or(0)); }
    std::string rules_text() const {
        const int t = target_score(), c = round_cap();
        if (t > 0) return "First to " + std::to_string(t) + " pts  (cap " + std::to_string(c) + " rounds)";
        return std::to_string(c) + " rounds";
    }
    // Why a finished match ended: someone reached the target, or the round cap was hit.
    std::string end_reason() const {
        const int t = target_score();
        if (t <= 0) return "";
        for (const auto& r : auth.robots) if (r.score >= t) return "Target of " + std::to_string(t) + " points reached";
        return "Round cap of " + std::to_string(round_cap()) + " reached - highest score wins";
    }

    int last_round = 0, revision = 0, events_version = -1;
    bool locked = false;
    std::array<std::string, 3> program{};
    std::string notice;
    ScrollLog log;
    bool dragging_scroll = false;
    int mouse_x = -1, mouse_y = -1;
    bool dismissed_result = false;
    Uint32 bot_due = 0;

    // ---- round playback ----
    Playback play;
    BoardView auth;            // last authoritative snapshot
    bool have_auth = false;
    struct Held { int need; std::vector<std::pair<std::string, ScrollLog::Kind>> lines; };
    std::deque<Held> held;     // narration released tick by tick as the replay reaches it
    const BoardView& shown() const { return play.active() ? play.board() : auth; }
    void release_logs() {
        while (!held.empty() && (!play.active() || held.front().need <= play.tick_steps_started())) {
            for (auto& l : held.front().lines) log.add(l.first, l.second);
            held.pop_front();
        }
    }
    void skip_playback() { play.skip(); release_logs(); }
    void bump_speed(int dir) {
        static const double steps[] = {0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
        int idx = 2;
        for (int i = 0; i < 7; ++i) if (std::abs(steps[i] - play.speed()) < 0.01) idx = i;
        idx = std::clamp(idx + dir, 0, 6);
        play.set_speed(steps[idx]);
    }

    static Json make(const char* type) { Json j = Json::object(); j.set("type", type); return j; }
    bool call(const Json& req, Json& resp) {
        auto r = conn.request(req, err, 4000);
        if (!r) { fatal = err; return false; }
        resp = std::move(*r);
        return true;
    }
    void say(const std::string& t, ScrollLog::Kind k = ScrollLog::Notice) { log.add(t, k); notice = t; }

    bool connect() {
        bool ok = false;
        for (int i = 0; i < 20 && !ok; ++i) {
            ok = conn.open(opt.host, opt.port, err);
            if (!ok) SDL_Delay(100);
        }
        if (!ok) { res.error = err; return false; }
        Json resp;
        match_id = opt.match_id;
        if (opt.create) {
            Json c = make("CREATE_MATCH");
            c.set("player_count", opt.players);
            if (opt.rounds > 0) c.set("rounds", opt.rounds);
            if (opt.target_score > 0) c.set("target_score", opt.target_score);
            c.set("planning_seconds", opt.planning_seconds);
            if (opt.bots > 0) c.set("bots", opt.bots);
            if (!opt.map_id.empty()) c.set("map_id", opt.map_id);
            if (!call(c, resp)) { res.error = err; return false; }
            if (!resp.get_bool("ok").value_or(false)) { res.error = "cannot create match: " + resp.get_string("message").value_or("?"); return false; }
            match_id = *resp.get_string("match_id");
            std::printf("Match %s created. Others join with:  botched-client --ui --join %s --port %d\n", match_id.c_str(), match_id.c_str(), opt.port);
            std::fflush(stdout);
        }
        if (match_id.empty()) { res.error = "no match to join (use --create or --join ID)"; return false; }
        Json j = make("JOIN_MATCH");
        j.set("match_id", match_id);
        if (!call(j, resp)) { res.error = err; return false; }
        if (!resp.get_bool("ok").value_or(false)) {
            res.error = "cannot join " + match_id + ": " + resp.get_string("message").value_or(resp.get_string("code").value_or("?"));
            return false;
        }
        me = static_cast<int>(*resp.get_int("player_id"));
        session = *resp.get_string("session");
        res.player_id = me;
        res.match_id = match_id;
        say("Joined " + match_id + " as player P" + std::to_string(me) + ".");
        return true;
    }

    void relayout() {
        if (!map) return;
        lay = compute_layout(map->width, map->height, capacity > 0 ? capacity : 4,
                             robot_panel_lines(!map->traps.empty(), !map->turrets.empty()));
    }

    bool load_map() {
        Json q = make("GET_MAP"), r;
        q.set("match_id", match_id);
        if (!call(q, r)) return false;
        const Json* mj = r.find("map");
        MapLoad ml = mj ? map_from_json(*mj) : MapLoad{};
        if (!ml.map) { fatal = "could not read the map from the server"; return false; }
        map = std::make_shared<const Map>(std::move(*ml.map));
        relayout();
        bot = std::make_unique<sim::Bot>(me, *map, opt.seed * 1000003ULL + static_cast<std::uint64_t>(me));
        return true;
    }

    bool refresh_status() {
        Json q = make("GET_MATCH_STATUS"), r;
        q.set("match_id", match_id);
        if (!call(q, r)) return false;
        if (const Json* s = r.find("status")) {
            status = *s;
            joined = static_cast<int>(s->get_int("joined").value_or(0));
            const int cap = static_cast<int>(s->get_int("capacity").value_or(0));
            const bool cap_changed = cap != capacity;
            capacity = cap;
            if (cap_changed) relayout();   // the scoreboard is sized by the number of players
            phase = s->get_string("phase").value_or(phase);
            bots = static_cast<int>(s->get_int("bots").value_or(0));
            bot_ids.clear();
            bot_kinds.clear();
            if (const Json* b = s->find("bot_players")) for (const auto& v : b->items()) bot_ids.push_back(static_cast<int>(v.as_int()));
            if (const Json* b = s->find("bot_personalities")) for (const auto& v : b->items()) bot_kinds.push_back(v.is_string() ? v.as_string() : "");
        }
        return true;
    }

    bool submit(bool ready) {
        ++revision;
        Json s = make("SUBMIT_PROGRAM");
        s.set("match_id", match_id);
        s.set("player_id", me);
        s.set("session", session);
        s.set("round", last_round);
        s.set("revision", revision);
        Json a = Json::array();
        for (const auto& o : program) a.push(o);
        s.set("orders", std::move(a));
        s.set("ready", ready);
        s.set("request_id", "p" + std::to_string(me) + "r" + std::to_string(last_round) + "v" + std::to_string(revision));
        Json r;
        if (!call(s, r)) return false;
        if (r.get_bool("ok").value_or(false)) {
            if (ready) { locked = true; say("Locked in. Waiting for the other players..."); }
            else notice = "Orders saved. Press ENTER or click LOCK IN to commit them.";
        } else {
            const std::string code = r.get_string("code").value_or("?");
            if (code == "PLANNING_CLOSED" || code == "WRONG_ROUND") say("Too late: planning has closed for this round.", ScrollLog::Bad);
            else if (code == "PROGRAM_LOCKED") { locked = true; notice = "Already locked in this round."; }
            else say("Rejected: " + r.get_string("message").value_or(code), ScrollLog::Bad);
        }
        return true;
    }

    bool program_complete() const { return !program[0].empty() && !program[1].empty() && !program[2].empty(); }

    // Network poll: runs a few times a second, never blocks for long on a local server.
    void poll() {
        if (!fatal.empty() || phase == "FINISHED") { if (phase == "FINISHED" && !finished_logged) finish_up(); return; }
        if (!map) {
            if (!refresh_status()) return;
            if (phase == "LOBBY") return;
            if (!load_map()) return;
        }
        Json q = make("GET_SNAPSHOT"), r;
        q.set("match_id", match_id);
        if (!call(q, r)) return;
        const Json* sj = r.find("snapshot");
        if (!sj) { fatal = "bad snapshot response"; return; }
        const BoardView prev_board = auth;
        const bool had_prev = have_auth;
        snap = *sj;
        auth = read_board(snap);
        have_auth = true;
        phase = snap.get_string("phase").value_or("?");
        const int round = static_cast<int>(snap.get_int("round").value_or(0));
        const int version = static_cast<int>(snap.get_int("state_version").value_or(0));
        if (events_version < 0) events_version = version;
        if (version != events_version) {
            Json eq = make("GET_EVENTS"), er;
            eq.set("match_id", match_id);
            eq.set("since_version", events_version);
            if (!call(eq, er)) return;
            if (er.get_bool("ok").value_or(false))
                if (const Json* ev = er.find("events")) {
                    const auto lines = client::summarize_events(*ev);
                    if (opt.animate && had_prev && !ev->items().empty() && map) {
                        // Replay the authoritative events; narration is held back and released per tick.
                        const int base = play.active() ? play.ticks_scheduled() : 0;
                        play.start(prev_board, *ev);
                        int k = 0;
                        held.push_back({0, {}});
                        for (const auto& l : lines) {
                            if (l.rfind("-- ", 0) == 0) { ++k; held.push_back({base + k, {}}); }
                            held.back().lines.emplace_back(l, classify_line(l));
                        }
                        release_logs();
                    } else {
                        for (const auto& l : lines) log.add(l, classify_line(l));
                    }
                }
            events_version = version;
        }
        if (round != last_round) {
            last_round = round;
            revision = 0;
            locked = false;
            program = {};
            if (phase != "FINISHED") say(round == 1 ? "Round 1: plan three orders, then lock in." : "Round " + std::to_string(round) + ": plan your three orders.");
        }
        res.final_round = round;
        if (phase == "FINISHED") { finish_up(); return; }
        if (!opt.programs.empty() && phase == "PLANNING" && !locked && round >= 1 && static_cast<std::size_t>(round) <= opt.programs.size()) {
            std::string err2;  // scripted orders per round ("F2 R T"): a demo / test aid
            if (auto prog = client::parse_program(opt.programs[static_cast<std::size_t>(round - 1)], err2)) { program = *prog; submit(true); }
        }
        if (opt.bot && phase == "PLANNING" && !locked && !play.active()) {
            const Uint32 now = SDL_GetTicks();
            if (opt.bot_think_ms > 0) {  // demo pacing without blocking the window
                if (bot_due == 0) bot_due = now + static_cast<Uint32>(opt.bot_think_ms);
                if (now < bot_due) return;
            }
            bot_due = 0;
            program = bot->plan(snap);
            submit(true);
        }
    }

    void finish_up() {
        if (finished_logged) return;
        Json st;
        Json q = make("GET_MATCH_STATUS"), r;
        q.set("match_id", match_id);
        if (!call(q, r)) return;
        if (const Json* s = r.find("status")) {
            st = *s;
            if (const Json* w = st.find("winners"))
                for (const auto& v : w->items()) winners.push_back(static_cast<int>(v.as_int()));
        }
        res.winners = winners;
        res.ok = true;
        finished_logged = true;
        std::string names;
        for (int w : winners) names += (names.empty() ? "P" : " P") + std::to_string(w);
        std::string msg = winners.size() == 1 ? "MATCH OVER - winner: " + names : "MATCH OVER - shared victory: " + names;
        if (!end_reason().empty()) msg += "  (" + end_reason() + ")";
        if (play.active()) held.push_back({1 << 20, {{msg, ScrollLog::Good}}});  // after the replay
        else say(msg, ScrollLog::Good);
    }

    void start_match() {
        Json s = make("START_MATCH"), r;
        s.set("match_id", match_id);
        if (!call(s, r)) return;
        if (!r.get_bool("ok").value_or(false)) say("Cannot start yet: " + r.get_string("message").value_or("?"), ScrollLog::Bad);
    }

    // ---- order entry ----
    bool can_edit() const { return phase == "PLANNING" && !locked && fatal.empty(); }
    void push_order(int kind) {
        if (!can_edit()) return;
        for (auto& slot : program) if (slot.empty()) { slot = kKinds[kind].token; break; }
        if (program_complete()) submit(false);
    }
    void clear_slot(int i) { if (can_edit()) program[static_cast<std::size_t>(i)].clear(); }
    void pop_order() {
        if (!can_edit()) return;
        for (int i = 2; i >= 0; --i) if (!program[static_cast<std::size_t>(i)].empty()) { program[static_cast<std::size_t>(i)].clear(); break; }
    }
    void lock_in() {
        if (phase != "PLANNING") { notice = "Planning is not open right now."; return; }
        if (locked) { notice = "Already locked in."; return; }
        if (!program_complete()) { notice = "Choose three orders first."; return; }
        submit(true);
    }

    // ---- events ----
    void set_scroll_from_y(int y) {
        const Rect& sb = lay.scrollbar;
        if (log.max_offset() == 0) return;
        float t = std::clamp(static_cast<float>(y - sb.y) / static_cast<float>(sb.h), 0.f, 1.f);  // 0 top .. 1 bottom
        int target = static_cast<int>(std::lround((1.f - t) * static_cast<float>(log.max_offset())));
        log.scroll(target - log.offset());
    }
    // returns false to quit
    bool handle(const SDL_Event& e) {
        switch (e.type) {
            case SDL_QUIT: return false;
            case SDL_MOUSEMOTION: {
                // SDL already reports event coordinates in logical units (SDL_RenderSetLogicalSize)
                mouse_x = e.motion.x;
                mouse_y = e.motion.y;
                if (dragging_scroll) set_scroll_from_y(mouse_y);
                break;
            }
            case SDL_MOUSEBUTTONUP: dragging_scroll = false; break;
            case SDL_MOUSEWHEEL: {
                int mx = mouse_x, my = mouse_y;
                if (lay.messages.contains(mx, my)) log.scroll(e.wheel.y * 3);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.button != SDL_BUTTON_LEFT) break;
                mouse_x = e.button.x;
                mouse_y = e.button.y;
                click(e.button.x, e.button.y);
                break;
            }
            case SDL_KEYDOWN: {
                const SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) return false;
                if (k == SDLK_PAGEUP) log.scroll(log.viewport() - 1);
                else if (k == SDLK_PAGEDOWN) log.scroll(-(log.viewport() - 1));
                else if (k == SDLK_SPACE) skip_playback();
                else if (k == SDLK_LEFTBRACKET || k == SDLK_MINUS) bump_speed(-1);
                else if (k == SDLK_RIGHTBRACKET || k == SDLK_EQUALS) bump_speed(+1);
                else if (k == SDLK_HOME) log.to_top();
                else if (k == SDLK_END) log.to_bottom();
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (phase == "LOBBY" && joined >= 2) start_match(); else lock_in(); }
                else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) pop_order();
                else if (!(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT)))
                    for (int i = 0; i < kOrderKinds; ++i) if (k == kKinds[i].key) push_order(i);
                break;
            }
            default: break;
        }
        return true;
    }
    void click(int mx, int my) {
        if (lay.scrollbar.contains(mx, my)) { dragging_scroll = true; set_scroll_from_y(my); return; }
        if (phase == "LOBBY") { if (lay.start_button.contains(mx, my) && joined >= 2) start_match(); return; }
        if (lay.skip_button.contains(mx, my)) { skip_playback(); return; }
        if (phase == "FINISHED") { if (finished_logged) dismissed_result = true; return; }
        for (int i = 0; i < kOrderKinds; ++i) if (lay.palette[static_cast<std::size_t>(i)].contains(mx, my)) { push_order(i); return; }
        for (int i = 0; i < 3; ++i) if (lay.slots[static_cast<std::size_t>(i)].contains(mx, my)) { clear_slot(i); return; }
        if (lay.lock_button.contains(mx, my)) lock_in();
        else if (lay.clear_button.contains(mx, my) && can_edit()) program = {};
    }
};

// ------------------------------------------------------------------ rendering

void panel(Gfx& g, Rect r, const char* caption, SDL_Color accent = kCyan) {
    g.fill(r, kPanel);
    g.frame(r, kPanelEdge, 1);
    g.fill({r.x, r.y, r.w, 3}, accent);
    if (caption && *caption) g.text(r.x + 12, r.y + 7, caption, kMuted, g.small);
}

void draw_title(App& a, Gfx& g, Uint32 now) {
    const Layout& L = a.lay;
    g.fill(L.title, C(18, 24, 36));
    g.frame(L.title, kPanelEdge, 1);
    g.text(L.title.x + 16, L.title.y + 2, "BOTCHED!", kGold, g.title);
    std::string mid;
    const Step* replay_step = a.play.current();
    if (a.phase == "LOBBY") mid = "Lobby   " + std::to_string(a.joined) + "/" + std::to_string(a.capacity) + " players";
    else if (replay_step) {
        const int total = static_cast<int>(a.snap.get_int("rounds_total").value_or(0));
        mid = "Round " + std::to_string(replay_step->round > 0 ? replay_step->round : a.last_round) + (a.target_score() > 0 ? " (cap " : " / ") + std::to_string(total) + (a.target_score() > 0 ? ")" : "") + "     REPLAY";
    }
    else if (a.phase == "FINISHED") mid = "MATCH OVER";
    else {
        const int total = static_cast<int>(a.snap.get_int("rounds_total").value_or(0));
        mid = "Round " + std::to_string(a.last_round) + (a.target_score() > 0 ? " (cap " : " / ") + std::to_string(total) + (a.target_score() > 0 ? ")" : "") + "     " + (a.phase == "PLANNING" ? "PLANNING" : "EXECUTING");
    }
    g.text(L.title.x + 250, L.title.y + 10, mid, kInk, g.big);
    if (a.target_score() > 0) g.text(L.title.x + 800, L.title.y + 15, "First to " + std::to_string(a.target_score()) + " points wins", kGold, g.small);
    g.text(L.title.x + L.title.w - 16, L.title.y + 6, "match " + a.match_id + "   -   you are P" + std::to_string(a.me), player_color(a.me), g.body, 2);
    if (a.phase == "PLANNING") {
        std::string t = "waiting";
        const Json* dl = a.snap.find("planning_deadline_utc");
        if (dl && dl->is_string())
            if (auto ms = client::parse_utc_ms(dl->as_string())) {
                auto nowms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                t = "closes in " + std::to_string(std::max<std::int64_t>(0, (*ms - nowms + 999) / 1000)) + "s";
            }
        g.text(L.title.x + L.title.w - 16, L.title.y + 26, t, kAmber, g.small, 2);
    }
    (void)now;
}

// Draws `text` word-wrapped into at most two lines inside [x0, x1) on the board panel's caption row, so a long
// description can never run into the "FACTORY FLOOR" label or off the panel. A third line is cut with "...".
void caption_text(Gfx& g, const Rect& panel_rect, int x0, int x1, const std::string& text, SDL_Color c) {
    const int maxw = std::max(40, x1 - x0);
    std::vector<std::string> lines;
    std::string cur;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && g.text_w(trial, g.small) > maxw) { lines.push_back(cur); cur = word; }
        else cur = trial;
        i = j + 1;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.size() > 2) {
        std::string last = lines[1] + " " + lines[2];
        lines.resize(2);
        lines[1] = last;
    }
    if (!lines.empty()) {
        std::string& l = lines.back();
        if (g.text_w(l, g.small) > maxw) {
            while (l.size() > 1 && g.text_w(l + "...", g.small) > maxw) l.pop_back();
            l += "...";
        }
    }
    const int lh = g.line_h(g.small);
    const int top = lines.size() > 1 ? panel_rect.y + 3 : panel_rect.y + 7;
    for (std::size_t k = 0; k < lines.size(); ++k) g.text(x0, top + static_cast<int>(k) * lh, lines[k], c, g.small);
}

void draw_board(App& a, Gfx& g, Uint32 now) {
    const Layout& L = a.lay;
    panel(g, L.board_panel, "FACTORY FLOOR");
    if (!a.map || a.snap.is_null()) {
        std::string head = a.phase == "LOBBY" ? "WAITING FOR PLAYERS" : "CONNECTING";
        g.text(L.board_panel.x + L.board_panel.w / 2, L.board_panel.y + L.board_panel.h / 2 - 70, head, kInk, g.title, 1);
        if (a.phase == "LOBBY") {
            g.text(L.board_panel.x + L.board_panel.w / 2, L.board_panel.y + L.board_panel.h / 2 - 20,
                   std::to_string(a.joined) + " of " + std::to_string(a.capacity) + " seats filled" +
                       (a.bots > 0 ? "  (" + std::to_string(a.bots) + (a.bots == 1 ? " bot)" : " bots)") : ""), kMuted, g.big, 1);
            g.text(L.board_panel.x + L.board_panel.w / 2, L.board_panel.y + L.board_panel.h / 2 + 8, "Rules: " + a.rules_text(), kGold, g.body, 1);
            const bool ok = a.joined >= 2;
            g.fill(L.start_button, ok ? C(40, 130, 80) : C(48, 54, 66));
            g.frame(L.start_button, ok ? kGreen : kPanelEdge, 2);
            g.text(L.start_button.x + L.start_button.w / 2, L.start_button.y + 13, "START MATCH", ok ? kInk : kMuted, g.big, 1);
            g.text(L.board_panel.x + L.board_panel.w / 2, L.start_button.y + 66, "the match also starts automatically when every seat is filled", kMuted, g.small, 1);
        }
        return;
    }
    const Map& m = *a.map;
    const BoardView& sv = a.shown();
    const Step* st = a.play.current();
    const float pr = static_cast<float>(a.play.progress());
    const float ep = pr * pr * (3.f - 2.f * pr);  // eased progress
    const bool machinery = st && st->phase == Step::Machinery;
    const Uint32 belt_time = (st && !machinery) ? 0u : now;  // belts only run during the machinery phase of a replay
    const int s = L.cell;
    auto cell_c = [&](float cx, float cy) { return V2{L.board.x + (cx + 0.5f) * static_cast<float>(s), L.board.y + (cy + 0.5f) * static_cast<float>(s)}; };
    // clip so oversized sprites never leak out of the panel
    SDL_Rect clip{L.board.x, L.board.y, L.board.w, L.board.h};
    SDL_RenderSetClipRect(g.r, &clip);
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x) {
            const Tile& t = m.tile({x, y});
            const int px = L.board.x + x * s, py = L.board.y + y * s;
            switch (t.terrain) {
                case Terrain::Wall: draw_wall(g, px, py, s); break;
                case Terrain::Floor: draw_floor(g, px, py, s, (x + y) & 1); break;
                case Terrain::Pit: draw_pit(g, px, py, s); break;
                case Terrain::Spawn: draw_spawn(g, px, py, s, t.owner); break;
                case Terrain::Dock: draw_dock(g, px, py, s, t.owner); break;
                case Terrain::Dispenser: draw_dispenser(g, px, py, s, t.owner, sv.crate_at(x, y) != nullptr); break;
                case Terrain::Console: draw_console(g, px, py, s, t.group); break;
                case Terrain::LaserEmitter: draw_laser(g, px, py, s, t.dir); break;
                case Terrain::Trap: {
                    const TrapView* tv = sv.trap(t.owner);
                    draw_trap(g, px, py, s, t.owner, tv ? tv->armed : true, tv ? tv->owner : 0, now);
                    break;
                }
                case Terrain::TarGenerator: draw_tar_generator(g, px, py, s, now); break;
                case Terrain::Turret: {
                    const TurretView* uv = sv.turret(t.owner);
                    draw_turret(g, px, py, s, t.owner, uv ? uv->charge : 0, uv ? uv->owner : 0, now);
                    break;
                }
                case Terrain::TurretPad: {
                    const TurretView* uv = sv.turret(t.owner);
                    draw_turret_pad(g, px, py, s, t.owner, uv ? uv->owner : 0);
                    break;
                }
                case Terrain::TrapConsole: {
                    const TrapView* tv = sv.trap(t.owner);
                    draw_trap_console(g, px, py, s, t.owner, tv ? tv->armed : true, tv ? tv->owner : 0);
                    break;
                }
                case Terrain::Conveyor: {
                    float glow = machinery ? 0.35f : 0.f;
                    if (st && st->kind == Step::Toggle && st->group == t.group) glow = std::sin(pr * 3.14159f);
                    draw_conveyor(g, px, py, s, sv.group_reversed(t.group) ? opposite(t.dir) : t.dir, t.group, belt_time, glow);
                    break;
                }
            }
        }
    // tar: the eight tiles around each generator (always visible; it is a permanent area)
    for (const Pos& tg : m.tar_generators)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const Pos q{tg.x + dx, tg.y + dy};
                if ((!dx && !dy) || !m.in_bounds(q) || m.blocked(q) || m.tile(q).terrain == Terrain::Pit) continue;
                draw_tar_tile(g, L.board.x + q.x * s, L.board.y + q.y * s, s, q.x * 5 + q.y * 3, now);
            }
    // armed traps: tint the eight tiles they will hit, in the owner's colour
    for (const auto& tp : m.traps) {
        const TrapView* tv = sv.trap(tp.id);
        if (!tv || !tv->armed) continue;
        const SDL_Color oc = trap_color(tv->owner);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.006f);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const Pos q{tp.pos.x + dx, tp.pos.y + dy};
                if (!m.in_bounds(q) || m.tile(q).terrain == Terrain::Wall) continue;
                const int qx = L.board.x + q.x * s, qy = L.board.y + q.y * s;
                g.fill({qx, qy, s, s}, C(oc.r, oc.g, oc.b, static_cast<int>(26 + 26 * pulse)));
                g.frame({qx + 1, qy + 1, s - 2, s - 2}, C(oc.r, oc.g, oc.b, 110), 1);
            }
    }
    // turret ranges: a faint cross two tiles long (stopped by blocked tiles), brighter when the next tick fires
    for (const auto& tr : m.turrets) {
        const TurretView* uv = sv.turret(tr.id);
        const SDL_Color oc = trap_color(uv ? uv->owner : 0);
        const int alpha = uv && uv->charge >= 2 ? 64 : 24;
        static constexpr int dxs[4] = {0, 1, 0, -1}, dys[4] = {-1, 0, 1, 0};
        for (int d = 0; d < 4; ++d)
            for (int k = 1; k <= 2; ++k) {
                const Pos q{tr.pos.x + dxs[d] * k, tr.pos.y + dys[d] * k};
                if (m.blocked(q)) break;
                const int qx = L.board.x + q.x * s, qy = L.board.y + q.y * s;
                g.fill({qx, qy, s, s}, C(oc.r, oc.g, oc.b, alpha));
                g.frame({qx + 1, qy + 1, s - 2, s - 2}, C(oc.r, oc.g, oc.b, 90), 1);
            }
    }
    // laser beams
    for (const Pos& e : m.lasers) {
        Dir d = m.tile(e).dir;
        V2 f = fwd(d);
        Pos p{e.x + static_cast<int>(f.x), e.y + static_cast<int>(f.y)};
        int n = 0;
        while (!m.blocked(p)) { ++n; p = {p.x + static_cast<int>(f.x), p.y + static_cast<int>(f.y)}; }
        V2 c0 = cell_c(static_cast<float>(e.x), static_cast<float>(e.y));
        g.line(c0.x, c0.y, c0.x + f.x * (static_cast<float>(n) + 0.5f) * static_cast<float>(s), c0.y + f.y * (static_cast<float>(n) + 0.5f) * static_cast<float>(s), std::max(2.f, s * 0.06f), C(255, 80, 80, 110));
    }
    // crates (loose): belts carry them, destroyed ones shrink, new ones pop in
    for (const auto& c : sv.crates) {
        float fx = static_cast<float>(c.x), fy = static_cast<float>(c.y), scale = 1.f;
        if (st) {
            for (const auto& mv : st->movers)
                if (mv.crate && mv.id == c.id) { fx = static_cast<float>(mv.fx) + static_cast<float>(mv.tx - mv.fx) * ep; fy = static_cast<float>(mv.fy) + static_cast<float>(mv.ty - mv.fy) * ep; }
            if (st->kind == Step::CrateGone && st->cx == c.x && st->cy == c.y) scale = 1.f - ep;
            if (st->kind == Step::Spawn && st->cx == c.x && st->cy == c.y) scale = ep;
        }
        if (m.tile({c.x, c.y}).terrain == Terrain::Dispenser && !(st && st->kind == Step::CrateMove)) { if (scale >= 1.f) continue; }  // drawn in the machine
        V2 cc = cell_c(fx, fy);
        draw_crate(g, cc.x, cc.y, s * 0.52f * scale);
    }
    // robots
    auto ang = [](Dir d) { switch (d) { case Dir::N: return -1.5707963f; case Dir::E: return 0.f; case Dir::S: return 1.5707963f; case Dir::W: return 3.1415927f; } return 0.f; };
    for (const auto& r : sv.robots) {
        if (!r.active) continue;
        float fx = static_cast<float>(r.x), fy = static_cast<float>(r.y), scale = 1.f, alpha = 1.f, heading = ang(r.heading);
        bool belt_move = false;
        float from_x = fx, from_y = fy;
        if (st) {
            for (const auto& mv : st->movers)
                if (!mv.crate && mv.id == r.id) {
                    from_x = static_cast<float>(mv.fx);
                    from_y = static_cast<float>(mv.fy);
                    fx = from_x + static_cast<float>(mv.tx - mv.fx) * ep;
                    fy = from_y + static_cast<float>(mv.ty - mv.fy) * ep;
                    belt_move = st->belt;
                    // trail: where it came from (amber for conveyors, player colour for its own drive)
                    V2 a0 = cell_c(from_x, from_y), a1 = cell_c(fx, fy);
                    g.line(a0.x, a0.y, a1.x, a1.y, s * 0.28f, belt_move ? C(255, 190, 80, 110) : mix(player_color(r.id), C(255, 255, 255), 0.2f));
                }
            if (st->player == r.id) {
                if (st->kind == Step::Turn) {
                    float a0 = ang(st->from), d = ang(st->to) - a0;
                    while (d > 3.1415927f + 0.01f) d -= 6.2831853f;
                    while (d <= -3.1415927f + 0.01f) d += 6.2831853f;
                    heading = a0 + d * ep;
                } else if (st->kind == Step::Blocked) {
                    V2 f = fwd(st->to);
                    float nudge = 0.2f * std::sin(pr * 3.14159f);
                    fx += f.x * nudge;
                    fy += f.y * nudge;
                } else if (st->kind == Step::Disable) {
                    fx += 0.04f * std::sin(pr * 60.f);
                    scale = 1.f + 0.1f * pr;
                    alpha = pr < 0.45f ? 1.f : std::max(0.f, 1.f - (pr - 0.45f) / 0.45f);
                } else if (st->kind == Step::Respawn) {
                    scale = std::max(0.05f, ep);
                    alpha = std::min(1.f, 0.3f + ep);
                }
            }
        }
        V2 cc = cell_c(fx, fy);
        // who is acting right now
        if (st && st->player == r.id && st->phase == Step::Orders && st->kind != Step::Disable) {
            float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.012f);
            g.ring(cc.x, cc.y, s * 0.55f, 3.f, C(255, 255, 255, static_cast<int>(120 + 120 * pulse)), 36);
        }
        g.alpha = alpha;
        draw_robot(g, cc.x, cc.y, static_cast<float>(s) * scale, r.id, V2{std::cos(heading), std::sin(heading)}, r.carry_id != 0, r.damage, r.id == a.me, now);
        g.alpha = 1.f;
        if (belt_move) {  // gear badge: this robot is being moved by machinery, not by its own order
            draw_gear(g, cc.x - s * 0.34f, cc.y - s * 0.34f, s * 0.17f, C(255, 190, 80));
        }
    }
    // one-shot effects for the current step
    if (st) {
        auto burst = [&](V2 c0, float radius, SDL_Color col, int rays) {
            for (int i = 0; i < rays; ++i) {
                float a1 = 6.2831853f * static_cast<float>(i) / static_cast<float>(rays);
                g.line(c0.x + std::cos(a1) * radius * 0.5f, c0.y + std::sin(a1) * radius * 0.5f, c0.x + std::cos(a1) * radius, c0.y + std::sin(a1) * radius, 3, col);
            }
        };
        const float fade_out = 1.f - pr;
        switch (st->kind) {
            case Step::Push: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                if (pr > 0.35f) burst(c0, s * (0.3f + 0.5f * pr), C(255, 255, 255, static_cast<int>(255 * fade_out)), 8);
                break;
            }
            case Step::Blocked: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                SDL_Color red = C(255, 70, 70, static_cast<int>(60 + 195 * std::sin(pr * 3.14159f)));
                g.line(c0.x - s * 0.3f, c0.y - s * 0.3f, c0.x + s * 0.3f, c0.y + s * 0.3f, 5, red);
                g.line(c0.x + s * 0.3f, c0.y - s * 0.3f, c0.x - s * 0.3f, c0.y + s * 0.3f, 5, red);
                break;
            }
            case Step::Pickup:
            case Step::Drop: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                g.ring(c0.x, c0.y, s * (0.3f + 0.4f * pr), 3, C(255, 220, 120, static_cast<int>(255 * fade_out)));
                break;
            }
            case Step::Deliver: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                g.ring(c0.x, c0.y, s * (0.3f + 0.6f * pr), 4, C(255, 215, 80, static_cast<int>(255 * fade_out)));
                g.alpha = std::min(1.f, 2.f * fade_out);
                g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.5f + 0.5f * pr)) - 10, "DELIVERED!", kGold, g.body, 1);
                g.alpha = 1.f;
                break;
            }
            case Step::Laser: {
                const RobotView* rb = sv.robot(st->player);
                if (rb && rb->active) {
                    V2 c0 = cell_c(static_cast<float>(rb->x), static_cast<float>(rb->y));
                    g.disc(c0.x, c0.y, s * 0.5f, C(255, 60, 60, static_cast<int>(140 * std::sin(pr * 3.14159f))));
                }
                break;
            }
            case Step::TrapArm: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                const SDL_Color oc = trap_color(st->owner);
                g.ring(c0.x, c0.y, s * (0.4f + 0.9f * pr), 4, C(oc.r, oc.g, oc.b, static_cast<int>(255 * fade_out)), 40);
                g.alpha = std::min(1.f, 2.f * fade_out);
                g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.7f + 0.4f * pr)) - 10, "ARMED", oc, g.body, 1);
                g.alpha = 1.f;
                break;
            }
            case Step::TrapBurst: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                const SDL_Color oc = trap_color(st->owner);
                const int fl = static_cast<int>(200 * std::sin(pr * 3.14159f));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const Pos q{st->cx + dx, st->cy + dy};
                        if (!m.in_bounds(q) || m.tile(q).terrain == Terrain::Wall) continue;
                        g.fill({L.board.x + q.x * s, L.board.y + q.y * s, s, s}, C(255, 240, 200, fl));
                    }
                g.ring(c0.x, c0.y, s * (0.3f + 1.4f * pr), 5, C(oc.r, oc.g, oc.b, static_cast<int>(255 * fade_out)), 40);
                burst(c0, s * (0.5f + 0.9f * pr), C(255, 210, 120, static_cast<int>(255 * fade_out)), 12);
                break;
            }
            case Step::TarSlow: {
                const RobotView* rb = sv.robot(st->player);
                if (rb && rb->active) {
                    V2 c0 = cell_c(static_cast<float>(rb->x), static_cast<float>(rb->y));
                    g.disc(c0.x, c0.y, s * 0.5f, C(90, 40, 130, static_cast<int>(150 * std::sin(pr * 3.14159f))));
                    g.ring(c0.x, c0.y, s * (0.55f - 0.15f * pr), 3, C(190, 120, 240, static_cast<int>(255 * fade_out)), 28);
                    g.alpha = std::min(1.f, 2.f * fade_out);
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.5f + 0.3f * pr)) - 10, "TAR!", C(210, 150, 255), g.body, 1);
                    g.alpha = 1.f;
                }
                break;
            }
            case Step::TurretCapture: {
                const RobotView* rb = sv.robot(st->player);
                const SDL_Color oc = trap_color(st->owner);
                if (rb && rb->active) {
                    V2 c0 = cell_c(static_cast<float>(rb->x), static_cast<float>(rb->y));
                    g.ring(c0.x, c0.y, s * (0.4f + 0.9f * pr), 4, C(oc.r, oc.g, oc.b, static_cast<int>(255 * fade_out)), 40);
                    g.alpha = std::min(1.f, 2.f * fade_out);
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.7f + 0.4f * pr)) - 10, "CAPTURED", oc, g.body, 1);
                    g.alpha = 1.f;
                }
                break;
            }
            case Step::TurretCharge: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                const SDL_Color oc = trap_color(st->owner);
                g.ring(c0.x, c0.y, s * (0.95f - 0.4f * pr), 3, C(oc.r, oc.g, oc.b, static_cast<int>(200 * pr)), 36);  // energy drawn in
                break;
            }
            case Step::TurretFire: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                const SDL_Color oc = trap_color(st->owner);
                const V2 f = fwd(st->to);
                const V2 c1{c0.x + f.x * static_cast<float>(st->distance) * static_cast<float>(s), c0.y + f.y * static_cast<float>(st->distance) * static_cast<float>(s)};
                const int fl = static_cast<int>(255 * std::sin(pr * 3.14159f));
                g.line(c0.x, c0.y, c1.x, c1.y, std::max(6.f, s * 0.18f), C(oc.r, oc.g, oc.b, fl / 2));
                g.line(c0.x, c0.y, c1.x, c1.y, std::max(2.f, s * 0.07f), C(255, 255, 255, fl));
                g.disc(c1.x, c1.y, s * 0.45f, C(255, 90, 40, static_cast<int>(170 * std::sin(pr * 3.14159f))));
                g.alpha = std::min(1.f, 2.f * fade_out);
                g.text(static_cast<int>(c1.x), static_cast<int>(c1.y - s * (0.5f + 0.4f * pr)) - 10, "-1", kRed, g.big, 1);
                g.alpha = 1.f;
                break;
            }
            case Step::TrapHit: {
                const RobotView* rb = sv.robot(st->player);
                if (rb && rb->active) {
                    V2 c0 = cell_c(static_cast<float>(rb->x), static_cast<float>(rb->y));
                    g.disc(c0.x, c0.y, s * 0.5f, C(255, 90, 40, static_cast<int>(170 * std::sin(pr * 3.14159f))));
                    g.alpha = std::min(1.f, 2.f * fade_out);
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.5f + 0.4f * pr)) - 10, "-1", kRed, g.big, 1);
                    g.alpha = 1.f;
                }
                break;
            }
            case Step::Score: {
                const RobotView* rb = sv.robot(st->player);
                V2 c0 = rb && rb->active ? cell_c(static_cast<float>(rb->x), static_cast<float>(rb->y)) : V2{L.board.x + 20.f, L.board.y + 20.f};
                g.ring(c0.x, c0.y, s * (0.3f + 0.5f * pr), 3, C(255, 215, 80, static_cast<int>(255 * fade_out)), 36);
                g.alpha = std::min(1.f, 2.f * fade_out);
                if (st->shunt) {  // SHUNT!: a shockwave and the big wording, then the points
                    g.ring(c0.x, c0.y, s * (0.5f + 1.2f * pr), 5, C(255, 140, 40, static_cast<int>(255 * fade_out)), 40);
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.6f + 0.5f * pr)) - 30, "SHUNT!", C(255, 150, 50), g.title, 1);
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.6f + 0.5f * pr)) - 4, "+" + std::to_string(st->points), kGold, g.big, 1);
                } else {
                    g.text(static_cast<int>(c0.x), static_cast<int>(c0.y - s * (0.5f + 0.6f * pr)) - 12, "+" + std::to_string(st->points), kGold, g.title, 1);
                }
                g.alpha = 1.f;
                break;
            }
            case Step::Disable: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                g.disc(c0.x, c0.y, s * (0.2f + 0.7f * pr), C(255, 150, 40, static_cast<int>(170 * fade_out)));
                g.disc(c0.x, c0.y, s * (0.1f + 0.4f * pr), C(255, 240, 160, static_cast<int>(220 * fade_out)));
                burst(c0, s * (0.4f + 0.7f * pr), C(255, 200, 80, static_cast<int>(255 * fade_out)), 10);
                break;
            }
            case Step::Respawn: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                g.ring(c0.x, c0.y, s * (0.8f - 0.4f * pr), 4, mix(player_color(st->player), C(255, 255, 255), 0.4f * fade_out), 36);
                break;
            }
            case Step::Spawn: {
                V2 c0 = cell_c(static_cast<float>(st->cx), static_cast<float>(st->cy));
                burst(c0, s * (0.3f + 0.3f * pr), C(255, 225, 130, static_cast<int>(220 * fade_out)), 6);
                break;
            }
            default: break;
        }
    }
    SDL_RenderSetClipRect(g.r, nullptr);
    g.frame({L.board.x - 1, L.board.y - 1, L.board.w + 2, L.board.h + 2}, kPanelEdge, 1);
    // axis labels
    for (int x = 0; x < m.width; ++x) g.text(L.board.x + x * s + s / 2, L.board.y - 15, std::to_string(x), kMuted, g.small, 1);
    // caption row: what the replay is doing, else the hover read-out
    if (st) {
        const char* ph = st->phase == Step::Machinery ? "MACHINERY" : st->phase == Step::RoundEdge ? "NEW ROUND" : "ORDERS";
        const SDL_Color pc = st->phase == Step::Machinery ? kAmber : st->phase == Step::RoundEdge ? kViolet : kCyan;
        g.text(L.board_panel.x + 160, L.board_panel.y + 7, std::string("round ") + std::to_string(st->round) + " tick " + std::to_string(st->tick) + " - " + ph, pc, g.small);
        caption_text(g, L.board_panel, L.board_panel.x + 340, L.board_panel.x + L.board_panel.w - 12, st->caption, kInk);
    } else {
        const int hx = (a.mouse_x - L.board.x), hy = (a.mouse_y - L.board.y);
        if (hx >= 0 && hy >= 0 && hx < L.board.w && hy < L.board.h) {
            const int cx = hx / s, cy = hy / s;
            std::string d = "(" + std::to_string(cx) + "," + std::to_string(cy) + ")  " + sim::describe_tile(m, a.snap, {cx, cy}, a.me);
            if (const auto* r = sv.robot_at(cx, cy)) d += "   |   robot P" + std::to_string(r->id);
            else if (sv.crate_at(cx, cy) && m.tile({cx, cy}).terrain != Terrain::Dispenser) d += "   |   loose crate";
            caption_text(g, L.board_panel, L.board_panel.x + 160, L.board_panel.x + L.board_panel.w - 12, d, kInk);
        } else {
            g.text(L.board_panel.x + L.board_panel.w - 12, L.board_panel.y + 7, "hover a tile for details", kMuted, g.small, 2);
        }
    }
    if (machinery) g.frame({L.board.x - 3, L.board.y - 3, L.board.w + 6, L.board.h + 6}, kAmber, 3);  // machinery phase: amber border
}

void draw_queue(App& a, Gfx& g, Uint32 now) {
    const Layout& L = a.lay;
    // While planning (and no replay still running) show the coming round's order instead of the last round's.
    const bool preview = a.phase == "PLANNING" && !a.play.active() && !a.snap.is_null() && !a.auth.robots.empty();
    std::vector<QueueEntry> pq;
    if (preview)
        pq = build_preview_queue(static_cast<int>(a.snap.get_int("round").value_or(a.last_round)),
                                 static_cast<int>(a.auth.robots.size()), a.me,
                                 std::vector<std::string>(a.program.begin(), a.program.end()));
    const auto& q = preview ? pq : a.play.queue();
    g.fill(L.queue, C(14, 19, 29));
    g.frame(L.queue, kPanelEdge, 1);
    if (q.empty()) {
        g.text(L.queue.x + 10, L.queue.y + 14, "Execution order appears here when a round resolves", kMuted, g.small);
    } else {
        const int live = preview ? -1 : a.play.queue_live(), done = preview ? 0 : a.play.queue_done();
        int x = L.queue.x + 6;
        for (std::size_t i = 0; i < q.size(); ++i) {
            const QueueEntry& e = q[i];
            const int w = e.machinery ? 30 : 44;
            Rect c{x, L.queue.y + 4, w, L.queue.h - 8};
            const bool is_live = static_cast<int>(i) == live, is_done = static_cast<int>(i) < done;
            g.alpha = is_done ? 0.38f : 1.f;
            if (e.machinery) {
                g.fill(c, is_live ? C(110, 76, 20) : C(52, 40, 16));
                g.frame(c, is_live ? kAmber : C(120, 90, 40), is_live ? 3 : 1);
                draw_gear(g, c.x + c.w / 2.f, c.y + c.h / 2.f - 1, 9, is_live ? C(255, 220, 130) : kAmber);
            } else {
                const SDL_Color pc = player_color(e.player);
                g.fill(c, is_live ? shade(pc, 0.55f) : shade(pc, 0.22f));
                g.frame(c, is_live ? C(255, 255, 255) : pc, is_live ? 3 : (e.preview && !e.hidden) ? 2 : 1);  // my own preview chips stand out
                g.text(c.x + c.w / 2, c.y + 1, "P" + std::to_string(e.player) + "(" + std::to_string(e.tick) + ")", is_live ? C(255, 255, 255) : pc, g.small, 1);
                std::string code = e.skipped ? "-" : e.order.empty() ? (e.preview && !e.hidden ? "_" : "?") : client::order_code(e.order);
                g.text(c.x + c.w / 2, c.y + c.h - g.line_h(g.small) - 1, code, e.no_op || e.skipped ? kMuted : kGold, g.small, 1);
            }
            g.alpha = 1.f;
            if (is_done) {  // tick mark
                const float bx = static_cast<float>(c.x + c.w - 10), by = static_cast<float>(c.y + c.h - 8);
                g.line(bx, by, bx + 3, by + 3, 2, kGreen);
                g.line(bx + 3, by + 3, bx + 8, by - 4, 2, kGreen);
            }
            x += w + 3;
        }
    }
    const bool playing = a.play.active();
    g.fill(L.skip_button, playing ? C(70, 50, 120) : C(34, 40, 52));
    g.frame(L.skip_button, playing ? kViolet : kPanelEdge, 2);
    g.text(L.skip_button.x + L.skip_button.w / 2, L.skip_button.y + 1, playing ? "SKIP >>" : "SKIP", playing ? kInk : kMuted, g.small, 1);
    char sp[24];
    std::snprintf(sp, sizeof sp, "x%.1f", a.play.speed());
    g.text(L.skip_button.x + L.skip_button.w / 2, L.skip_button.y + 17, sp, kMuted, g.small, 1);
    (void)now;
}

void draw_scores(App& a, Gfx& g) {
    const Layout& L = a.lay;
    const std::string caption = a.target_score() > 0 ? "SCOREBOARD - FIRST TO " + std::to_string(a.target_score()) : "SCOREBOARD";
    panel(g, L.scores, caption.c_str(), kGold);
    if (a.snap.is_null()) { g.text(L.scores.x + 14, L.scores.y + 40, "match not started", kMuted, g.body); return; }
    const BoardView& sv = a.shown();
    int best = 0;
    for (auto& r : sv.robots) best = std::max(best, r.score);
    std::vector<int> ready;
    if (const Json* rp = a.snap.find("ready_players")) for (const auto& v : rp->items()) ready.push_back(static_cast<int>(v.as_int()));
    int y = L.scores.y + 30;
    for (const auto& r : sv.robots) {
        Rect card{L.scores.x + 8, y, L.scores.w - 16, 48};
        SDL_Color pc = player_color(r.id);
        g.fill(card, shade(pc, 0.18f));
        g.frame(card, r.id == a.me ? pc : shade(pc, 0.5f), r.id == a.me ? 2 : 1);
        g.fill({card.x, card.y, 8, card.h}, pc);
        const std::string who = "P" + std::to_string(r.id) + (r.id == a.me ? "  (you)" : a.is_bot(r.id) ? (a.bot_kind(r.id).empty() ? "  BOT" : " BOT") : "");
        g.text(card.x + 18, card.y + 4, who, pc, g.body);
        if (r.id != a.me && a.is_bot(r.id)) {   // G4: the bot's personality, small, after the name
            const std::string kind = a.bot_kind(r.id);
            if (!kind.empty()) g.text(card.x + 18 + g.text_w(who, g.body) + 4, card.y + 9, kind, shade(pc, 0.8f), g.small);
        }
        const bool lead = r.score == best && best > 0;
        g.text(card.x + 18, card.y + 26, "deliveries " + std::to_string(r.deliveries) + (r.sabotage > 0 ? "  sabotage +" + std::to_string(r.sabotage) : ""), kMuted, g.small);
        if (const int tgt = a.target_score(); tgt > 0) {
            // progress toward the target score
            Rect bar{card.x + 18, card.y + card.h - 5, 150, 3};
            g.fill(bar, C(30, 36, 48));
            const int fill = std::min(bar.w, bar.w * std::min(r.score, tgt) / tgt);
            g.fill({bar.x, bar.y, fill, bar.h}, r.score >= tgt ? kGold : pc);
        }
        g.text(card.x + 190, card.y + 2, std::to_string(r.score), lead ? kGold : kInk, g.big, 2);
        g.text(card.x + 190 + 8, card.y + 14, "pts", kMuted, g.small);
        // damage pips (two hits disable)
        for (int i = 0; i < 2; ++i) {
            float px = static_cast<float>(card.x + 236 + i * 20), py = static_cast<float>(card.y + 24);
            g.disc(px, py, 7, i < r.damage ? kRed : C(40, 46, 58), 14);
            g.ring(px, py, 7, 1.5f, kPanelEdge, 14);
        }
        g.text(card.x + 232, card.y + 4, "damage", kMuted, g.small);
        if (r.carry_id != 0) draw_crate(g, static_cast<float>(card.x + 322), static_cast<float>(card.y + 24), 22);
        std::string chip;
        SDL_Color cc = kMuted;
        if (r.disabled) { chip = "DISABLED"; cc = kRed; }
        else if (a.phase == "PLANNING" && std::find(ready.begin(), ready.end(), r.id) != ready.end()) { chip = "READY"; cc = kGreen; }
        else if (a.phase == "PLANNING") { chip = "planning"; cc = kAmber; }
        if (lead && chip.empty()) { chip = "leading"; cc = kGold; }
        g.text(card.x + card.w - 10, card.y + 28, chip, cc, g.small, 2);
        y += 52;
    }
}

void draw_robot_panel(App& a, Gfx& g) {
    const Layout& L = a.lay;
    panel(g, L.robot, "YOUR ROBOT & THE FACTORY", player_color(a.me));
    if (a.snap.is_null() || !a.map) return;
    const BoardView& sv = a.auth;
    const bool has_traps = !a.map->traps.empty();
    const bool has_turrets = !a.map->turrets.empty();
    const int lh = kRobotLineH;  // the panel is sized for exactly these lines (compute_layout)
    int y = L.robot.y + 25;
    for (const auto& r : sv.robots) {
        if (r.id != a.me) continue;
        if (r.disabled) { g.text(L.robot.x + 12, y, "Disabled - respawning at the end of the round", kRed, g.small); break; }
        g.text(L.robot.x + 12, y, "At (" + std::to_string(r.x) + "," + std::to_string(r.y) + ") facing " + to_string(r.heading) +
                                      (r.carry_id != 0 ? "  carrying a crate" : "  empty-handed"), kInk, g.small);
        std::string on = sim::describe_tile(*a.map, a.snap, {r.x, r.y}, a.me);
        g.text(L.robot.x + 12, y + lh, "On: " + on, a.map->tile({r.x, r.y}).terrain == Terrain::Conveyor ? kAmber : kMuted, g.small);
        break;
    }
    std::string belts = "Belts:";
    for (char gr : a.map->groups) {
        bool belt = false;
        for (const auto& t : a.map->tiles) if (t.group == gr && t.terrain == Terrain::Conveyor) belt = true;
        if (belt) belts += std::string("  ") + gr + (sv.group_reversed(gr) ? " REVERSED" : " normal");
    }
    g.text(L.robot.x + 12, y + 2 * lh, belts, kCyan, g.small);
    std::string disp = "Dispensers:";
    for (const auto& d : a.map->dispensers) disp += "  G" + std::to_string(d.id) + (sv.crate_at(d.pos.x, d.pos.y) ? " loaded" : " empty");
    g.text(L.robot.x + 12, y + 3 * lh, disp, kGold, g.small);
    if (has_traps) {
        std::string tr = "Traps:";
        for (const auto& tp : a.map->traps) {
            const TrapView* tv = a.shown().trap(tp.id);  // what the board shows, so it matches the replay
            tr += "  X" + std::to_string(tp.id) + (tv && tv->armed ? " ARMED" : " off") + (tv && tv->owner ? " (P" + std::to_string(tv->owner) + ")" : " (neutral)");
        }
        g.text(L.robot.x + 12, y + 4 * lh, tr, kRed, g.small);
    }
    if (has_turrets) {
        std::string tr = "Turrets:";
        for (const auto& tp : a.map->turrets) {
            const TurretView* tv = a.shown().turret(tp.id);
            tr += "  Y" + std::to_string(tp.id) + " " + (tv && tv->charge >= 2 ? std::string("READY") : std::to_string(tv ? tv->charge : 0) + "/2") +
                  (tv && tv->owner ? " (P" + std::to_string(tv->owner) + ")" : " (neutral)");
        }
        g.text(L.robot.x + 12, y + (has_traps ? 5 : 4) * lh, tr, kRed, g.small);
    }
}

void draw_orders(App& a, Gfx& g) {
    const Layout& L = a.lay;
    const bool editable = a.can_edit();
    panel(g, L.orders, a.phase == "PLANNING" ? (a.locked ? "YOUR ORDERS - LOCKED IN" : "YOUR ORDERS - PLANNING") : "YOUR ORDERS", a.locked ? kGreen : kAmber);
    g.text(L.orders.x + 14, L.orders.y + 28, a.notice, a.fatal.empty() ? kInk : kRed, g.small);
    if (!a.fatal.empty()) g.text(L.orders.x + 14, L.orders.y + 44, "Connection problem: " + a.fatal, kRed, g.small);
    for (int i = 0; i < 3; ++i) {
        Rect s = L.slots[static_cast<std::size_t>(i)];
        const std::string& tok = a.program[static_cast<std::size_t>(i)];
        g.fill(s, a.locked ? C(24, 50, 36) : C(14, 18, 28));
        g.frame(s, tok.empty() ? kPanelEdge : (a.locked ? kGreen : kAmber), 2);
        g.text(s.x + 6, s.y + 3, std::to_string(i + 1), kMuted, g.small);
        int k = kind_of(tok);
        if (k >= 0) {
            draw_order_icon(g, k, s.x + s.w / 2.f, s.y + s.h / 2.f - 4, 46, kInk);
            g.text(s.x + s.w / 2, s.y + s.h - 20, order_code(tok), kGold, g.small, 1);
        } else {
            g.text(s.x + s.w / 2, s.y + s.h / 2 - 9, "-", kPanelEdge, g.big, 1);
        }
    }
    const bool can_lock = editable && a.program_complete();
    g.fill(L.lock_button, a.locked ? C(30, 90, 60) : can_lock ? C(40, 130, 80) : C(40, 46, 58));
    g.frame(L.lock_button, a.locked ? kGreen : can_lock ? kGreen : kPanelEdge, 2);
    g.text(L.lock_button.x + L.lock_button.w / 2, L.lock_button.y + 12, a.locked ? "LOCKED" : "LOCK IN", can_lock || a.locked ? kInk : kMuted, g.body, 1);
    g.text(L.lock_button.x + L.lock_button.w / 2, L.lock_button.y + 31, "ENTER", kMuted, g.small, 1);
    g.fill(L.clear_button, C(34, 40, 52));
    g.frame(L.clear_button, kPanelEdge, 1);
    g.text(L.clear_button.x + L.clear_button.w / 2, L.clear_button.y + 4, "clear (Backspace)", editable ? kMuted : C(70, 78, 92), g.small, 1);
    g.text(L.orders.x + 14, L.orders.y + 152, "Click an order (or press its key) - slots fill left to right", kMuted, g.small);
    for (int i = 0; i < kOrderKinds; ++i) {
        Rect b = L.palette[static_cast<std::size_t>(i)];
        const bool hot = b.contains(a.mouse_x, a.mouse_y) && editable;
        g.fill(b, hot ? C(52, 66, 92) : C(30, 38, 54));
        g.frame(b, hot ? kCyan : kPanelEdge, 1);
        draw_order_icon(g, i, b.x + b.w / 2.f, b.y + 22, 28, editable ? kInk : C(90, 98, 112));
        g.text(b.x + b.w / 2, b.y + b.h - 16, kKinds[i].keyname, editable ? kGold : C(90, 98, 112), g.small, 1);
    }
}

void draw_messages(App& a, Gfx& g) {
    const Layout& L = a.lay;
    panel(g, L.messages, "WHAT HAPPENED", kViolet);
    g.text(L.messages.x + L.messages.w - 14, L.messages.y + 7, "mouse wheel / PgUp PgDn / drag the bar to scroll   -   End = jump to newest", kMuted, g.small, 2);
    const int lh = g.line_h(g.small) + 1;
    a.log.set_viewport(L.log_text.h / lh);
    SDL_Rect clip{L.log_text.x, L.log_text.y, L.log_text.w, L.log_text.h};
    SDL_RenderSetClipRect(g.r, &clip);
    const std::size_t first = a.log.first_visible(), n = a.log.visible_count();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& ln = a.log.at(first + i);
        int y = L.log_text.y + static_cast<int>(i) * lh;
        if (ln.kind == ScrollLog::Header) g.fill({L.log_text.x, y, L.log_text.w, lh}, C(30, 44, 62));
        g.text(L.log_text.x + 6, y, ln.text, kind_color(ln.kind), g.small);
    }
    SDL_RenderSetClipRect(g.r, nullptr);
    // scrollbar
    g.fill(L.scrollbar, C(14, 18, 26));
    if (a.log.size() > static_cast<std::size_t>(a.log.viewport())) {
        const float total = static_cast<float>(a.log.size()), vp = static_cast<float>(a.log.viewport());
        const int th = std::max(24, static_cast<int>(L.scrollbar.h * vp / total));
        const float pos = static_cast<float>(a.log.first_visible()) / (total - vp);
        const int ty = L.scrollbar.y + static_cast<int>((L.scrollbar.h - th) * pos);
        g.fill({L.scrollbar.x, ty, L.scrollbar.w, th}, a.dragging_scroll ? kCyan : C(96, 112, 140));
    }
    if (!a.log.following() && a.log.unseen() > 0) {
        std::string t = "  " + std::to_string(a.log.unseen()) + " newer lines - press End  ";
        int w = g.text_w(t, g.small);
        Rect pill{L.log_text.x + L.log_text.w - w - 6, L.log_text.y + L.log_text.h - 22, w, 20};
        g.fill(pill, C(60, 40, 110));
        g.frame(pill, kViolet, 1);
        g.text(pill.x, pill.y + 1, t, kInk, g.small);
    }
}

void draw_result(App& a, Gfx& g) {
    if (a.phase != "FINISHED" || !a.finished_logged || a.dismissed_result || a.play.active()) return;
    g.fill({0, 0, kScreenW, kScreenH}, C(0, 0, 0, 170));
    Rect box{kScreenW / 2 - 300, 220, 600, 330};
    g.fill(box, kPanel);
    g.frame(box, kGold, 3);
    g.text(box.x + box.w / 2, box.y + 14, "MATCH OVER", kGold, g.title, 1);
    std::string names;
    for (int w : a.winners) names += (names.empty() ? "P" : " & P") + std::to_string(w);
    const bool mine = std::find(a.winners.begin(), a.winners.end(), a.me) != a.winners.end();
    g.text(box.x + box.w / 2, box.y + 64, (a.winners.size() == 1 ? "Winner: " : "Shared victory: ") + names, mine ? kGreen : kInk, g.big, 1);
    if (mine) g.text(box.x + box.w / 2, box.y + 98, "That's you!", kGreen, g.body, 1);
    else if (!a.end_reason().empty()) g.text(box.x + box.w / 2, box.y + 98, a.end_reason(), kMuted, g.body, 1);
    if (mine && !a.end_reason().empty()) g.text(box.x + box.w / 2, box.y + 120, a.end_reason(), kMuted, g.small, 1);
    const BoardView& sv = a.auth;
    int y = box.y + 140;
    for (const auto& r : sv.robots) {
        g.fill({box.x + 60, y, 480, 34}, shade(player_color(r.id), 0.25f));
        g.fill({box.x + 60, y, 8, 34}, player_color(r.id));
        g.text(box.x + 80, y + 5, "P" + std::to_string(r.id) + (r.id == a.me ? " (you)" : ""), player_color(r.id), g.body);
        g.text(box.x + 530, y + 3, std::to_string(r.score) + " pts   " + std::to_string(r.deliveries) + " deliveries", kInk, g.body, 2);
        y += 40;
    }
    g.text(box.x + box.w / 2, box.y + box.h - 28, "click to see the board - Esc to quit", kMuted, g.small, 1);
}

bool screenshot(SDL_Renderer* r, const char* path) {
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    bool ok = SDL_RenderReadPixels(r, nullptr, s->format->format, s->pixels, s->pitch) == 0 && SDL_SaveBMP(s, path) == 0;
    SDL_FreeSurface(s);
    return ok;
}

}  // namespace

client::ClientResult run_ui(const client::ClientOptions& opt) {
    App app(opt);
    app.play.set_speed(opt.anim_speed);
    auto fail = [&](const std::string& m) { app.res.ok = false; app.res.error = m; return app.res; };
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) return fail(std::string("SDL: ") + SDL_GetError());
    if (TTF_Init() < 0) { SDL_Quit(); return fail("SDL_ttf could not start"); }
    SDL_Window* win = SDL_CreateWindow("Botched!", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kScreenW, kScreenH,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* rd = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (win && !rd) rd = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    Gfx g;
    client::ClientResult out;
    bool fonts_ok = false;
    if (rd) {
        SDL_SetWindowMinimumSize(win, 960, 640);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_RenderSetLogicalSize(rd, kScreenW, kScreenH);
        SDL_SetRenderDrawBlendMode(rd, SDL_BLENDMODE_BLEND);
        static const char* const sans[] = {"DejaVuSans.ttf", "LiberationSans-Regular.ttf", "Verdana.ttf", "Arial.ttf", nullptr};
        static const char* const bold[] = {"DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "Verdana Bold.ttf", "Arial Bold.ttf", nullptr};
        g.r = rd;
        g.small = open_font(sans, 14);
        g.body = open_font(bold, 17);
        g.big = open_font(bold, 26);
        g.title = open_font(bold, 34);
        fonts_ok = g.small && g.body && g.big && g.title;
    }
    if (!rd || !fonts_ok) {
        std::string m = !rd ? std::string("SDL: ") + SDL_GetError() : "cannot load a font (install DejaVu Sans or set BOTCHED_FONT=/path/to.ttf)";
        if (rd) SDL_DestroyRenderer(rd);
        if (win) SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return fail(m);
    }

    if (!app.connect()) {
        out = app.res;
    } else {
        SDL_Delay(1);
        bool running = true;
        Uint32 last_poll = 0, last_frame = SDL_GetTicks();
        int frames = 0;
        while (running) {
            const Uint32 t0 = SDL_GetTicks();
            SDL_Event e;
            while (SDL_PollEvent(&e)) if (!app.handle(e)) running = false;
            if (opt.ui_hover_x >= 0 && app.map) {   // scripted hover for headless screenshots
                app.mouse_x = app.lay.board.x + opt.ui_hover_x * app.lay.cell + app.lay.cell / 2;
                app.mouse_y = app.lay.board.y + opt.ui_hover_y * app.lay.cell + app.lay.cell / 2;
            }
            app.play.advance(static_cast<int>(t0 - last_frame));
            last_frame = t0;
            app.release_logs();
            if (t0 - last_poll >= (opt.bot ? 150u : 250u) || last_poll == 0) { app.poll(); last_poll = t0; }
            if (app.opt.ui_exit_on_finish && app.finished_logged) running = false;

            g.color(kBg);
            SDL_RenderClear(rd);
            draw_title(app, g, t0);
            draw_board(app, g, t0);
            draw_queue(app, g, t0);
            draw_scores(app, g);
            draw_robot_panel(app, g);
            draw_orders(app, g);
            draw_messages(app, g);
            draw_result(app, g);
            ++frames;
            if (opt.ui_frames > 0 && frames >= opt.ui_frames) {
                if (!opt.ui_screenshot.empty() && !screenshot(rd, opt.ui_screenshot.c_str()))
                    std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
                running = false;
            }
            SDL_RenderPresent(rd);
            const Uint32 spent = SDL_GetTicks() - t0;
            if (spent < 16) SDL_Delay(16 - spent);
        }
        out = app.res;
        if (!app.fatal.empty() && !out.ok) out.error = app.fatal;
        else if (!out.ok && out.error.empty()) out.error = "left the match";
    }
    g.clear_cache();
    TTF_CloseFont(g.small); TTF_CloseFont(g.body); TTF_CloseFont(g.big); TTF_CloseFont(g.title);
    SDL_DestroyRenderer(rd);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return out;
}

}  // namespace botched::ui
