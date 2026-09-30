#ifdef EMU_OVERLAY

#include "overlay/vr_toast_image.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// Private stb_image_write instance (STATIC: each TU gets its own copy,
// same pattern as local_storage.cpp / steam_overlay.cpp stb usage).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

// ---------------------------------------------------------------------------
// Embedded 5x7 bitmap font, column-major: 5 bytes per glyph, bit0 = top row.
// Covers A-Z, 0-9 and common punctuation. Lowercase maps to uppercase.
// Glyphs are hand-drawn simple sans, sized for wrist readability.
// ---------------------------------------------------------------------------

namespace {

struct Glyph { char ch; uint8_t col[5]; };

constexpr Glyph kGlyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'!', {0x00,0x00,0x5F,0x00,0x00}},
    {'"', {0x00,0x07,0x00,0x07,0x00}},
    {'#', {0x14,0x7F,0x14,0x7F,0x14}},
    {'$', {0x24,0x2A,0x7F,0x2A,0x12}},
    {'%', {0x23,0x13,0x08,0x64,0x62}},
    {'&', {0x36,0x49,0x55,0x22,0x50}},
    {'\'', {0x00,0x05,0x03,0x00,0x00}},
    {'(', {0x00,0x1C,0x22,0x41,0x00}},
    {')', {0x00,0x41,0x22,0x1C,0x00}},
    {'*', {0x14,0x08,0x3E,0x08,0x14}},
    {'+', {0x08,0x08,0x3E,0x08,0x08}},
    {',', {0x00,0x50,0x30,0x00,0x00}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {'/', {0x20,0x10,0x08,0x04,0x02}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {';', {0x00,0x56,0x36,0x00,0x00}},
    {'<', {0x08,0x14,0x22,0x41,0x00}},
    {'=', {0x14,0x14,0x14,0x14,0x14}},
    {'>', {0x00,0x41,0x22,0x14,0x08}},
    {'?', {0x02,0x01,0x51,0x09,0x06}},
    {'@', {0x32,0x49,0x79,0x41,0x3E}},
    {'A', {0x7E,0x11,0x11,0x11,0x7E}},
    {'B', {0x7F,0x49,0x49,0x49,0x36}},
    {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'D', {0x7F,0x41,0x41,0x22,0x1C}},
    {'E', {0x7F,0x49,0x49,0x49,0x41}},
    {'F', {0x7F,0x48,0x48,0x48,0x40}},
    {'G', {0x3E,0x41,0x49,0x49,0x7A}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}},
    {'I', {0x00,0x41,0x7F,0x41,0x00}},
    {'J', {0x20,0x40,0x41,0x3F,0x01}},
    {'K', {0x7F,0x08,0x14,0x22,0x41}},
    {'L', {0x7F,0x40,0x40,0x40,0x40}},
    {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}},
    {'O', {0x3E,0x41,0x41,0x41,0x3E}},
    {'P', {0x7F,0x09,0x09,0x09,0x06}},
    {'Q', {0x3E,0x41,0x51,0x21,0x5E}},
    {'R', {0x7F,0x09,0x19,0x29,0x46}},
    {'S', {0x46,0x49,0x49,0x49,0x31}},
    {'T', {0x01,0x01,0x7F,0x01,0x01}},
    {'U', {0x7F,0x40,0x40,0x40,0x7F}},
    {'V', {0x1F,0x20,0x40,0x20,0x1F}},
    {'W', {0x3F,0x40,0x38,0x40,0x3F}},
    {'X', {0x63,0x14,0x08,0x14,0x63}},
    {'Y', {0x07,0x08,0x70,0x08,0x07}},
    {'Z', {0x61,0x51,0x49,0x45,0x43}},
    {'[', {0x00,0x7F,0x41,0x41,0x00}},
    {']', {0x00,0x41,0x41,0x7F,0x00}},
    {'_', {0x40,0x40,0x40,0x40,0x40}},
};

const uint8_t *find_glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const auto &g : kGlyphs) {
        if (g.ch == c) return g.col;
    }
    return nullptr; // unknown -> skip (blank)
}

struct Canvas {
    std::vector<uint8_t> px; // RGBA
    uint32_t w, h;
    Canvas(uint32_t w_, uint32_t h_) : w(w_), h(h_) { px.assign((size_t)w * h * 4, 0); }

    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        for (size_t i = 0; i < px.size(); i += 4) {
            px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = a;
        }
    }
    void rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x0 >= w || y0 >= h) return;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                size_t i = ((size_t)y * w + x) * 4;
                px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = a;
            }
    }
    void blend_px(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= w || y >= h) return;
        size_t i = ((size_t)y * w + x) * 4;
        px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = 255;
    }
    void rect_outline(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                      uint32_t t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (t == 0) return;
        rect(x0, y0, x1, y0 + t, r, g, b, a);                       // top
        rect(x0, y1 > t ? y1 - t : 0, x1, y1, r, g, b, a);          // bottom
        rect(x0, y0, x0 + t, y1, r, g, b, a);                       // left
        rect(x1 > t ? x1 - t : 0, y0, x1, y1, r, g, b, a);          // right
    }
    // Vertical flip in place (row swap).
    void flip_y() {
        std::vector<uint8_t> row((size_t)w * 4);
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t *a = &px[(size_t)y * w * 4];
            uint8_t *b = &px[(size_t)(h - 1 - y) * w * 4];
            memcpy(row.data(), a, row.size());
            memcpy(a, b, row.size());
            memcpy(b, row.data(), row.size());
        }
    }
    // Nearest-neighbor RGBA blit with alpha-over.
    void blit_rgba(uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                   const uint8_t *src, uint32_t sw, uint32_t sh) {
        if (!src || !sw || !sh || !dw || !dh) return;
        for (uint32_t y = 0; y < dh; ++y) {
            uint32_t sy = y * sh / dh;
            uint32_t yy = dy + y;
            if (yy >= h) break;
            for (uint32_t x = 0; x < dw; ++x) {
                uint32_t sx = x * sw / dw;
                uint32_t xx = dx + x;
                if (xx >= w) break;
                size_t si = ((size_t)sy * sw + sx) * 4;
                uint8_t sr = src[si], sg = src[si+1], sb = src[si+2], sa = src[si+3];
                if (sa == 0) continue;
                size_t di = ((size_t)yy * w + xx) * 4;
                if (sa == 255) {
                    px[di] = sr; px[di+1] = sg; px[di+2] = sb; px[di+3] = 255;
                } else {
                    float a = sa / 255.0f;
                    px[di]   = (uint8_t)(sr * a + px[di] * (1 - a));
                    px[di+1] = (uint8_t)(sg * a + px[di+1] * (1 - a));
                    px[di+2] = (uint8_t)(sb * a + px[di+2] * (1 - a));
                    px[di+3] = 255;
                }
            }
        }
    }
    // scale=1 -> 5x7 px per char, advance 6*scale.
    uint32_t draw_char(char c, uint32_t x, uint32_t y, uint32_t scale, uint8_t r, uint8_t g, uint8_t b) {
        const uint8_t *gl = find_glyph(c);
        if (!gl) return x + 6 * scale;
        for (uint32_t cx = 0; cx < 5; ++cx)
            for (uint32_t cy = 0; cy < 7; ++cy)
                if (gl[cx] & (1u << cy))
                    for (uint32_t sy = 0; sy < scale; ++sy)
                        for (uint32_t sx = 0; sx < scale; ++sx)
                            blend_px(x + cx * scale + sx, y + cy * scale + sy, r, g, b);
        return x + 6 * scale;
    }
    uint32_t text_w(const std::string &s, uint32_t scale) {
        return (uint32_t)s.size() * 6 * scale;
    }
    void draw_text(const std::string &s, uint32_t x, uint32_t y, uint32_t scale, uint8_t r, uint8_t g, uint8_t b) {
        for (char c : s) x = draw_char(c, x, y, scale, r, g, b);
    }
};

// Greedy word wrap for the bitmap font.
std::vector<std::string> wrap_text(const std::string &text, uint32_t max_w, uint32_t scale)
{
    std::vector<std::string> lines;
    std::string cur;
    uint32_t cur_w = 0;
    uint32_t adv = 6 * scale;
    auto flush = [&]() {
        if (!cur.empty() || lines.empty()) lines.push_back(cur);
        cur.clear(); cur_w = 0;
    };
    std::string word;
    auto push_word = [&]() {
        if (word.empty()) return;
        uint32_t ww = (uint32_t)word.size() * adv;
        if (!cur.empty() && cur_w + adv + ww > max_w) flush();
        else if (!cur.empty()) { cur += ' '; cur_w += adv; }
        cur += word; cur_w += ww;
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') { push_word(); flush(); }
        else if (c == ' ' || c == '\t') push_word();
        else word += c;
    }
    push_word();
    flush();
    return lines;
}

} // namespace

bool ComposeVRToastImage(const VRToastImageRequest &req, VRToastImage &out)
{
    if (req.title.empty() && req.description.empty()) return false;

    Canvas cv(VRToastImage::WIDTH, VRToastImage::HEIGHT);
    // Desktop replica at 2x: dark toast background (#1F2433 ~ 0.12/0.14/0.21).
    cv.fill(31, 36, 51, 255);

    // Desktop parity borders (steam_overlay.cpp build_notifications):
    // - normal: thin white inner line (inset 1px/2px-thick @0.82 alpha -> 2x).
    // - rare: bigger goldenrod glow rings + solid bright gold inner line.
    const uint32_t W = cv.w, H = cv.h;
    if (req.rare) {
        // Bigger glow band than desktop (readable at arm's length): 6 fading
        // goldenrod rings filling a 48px band from the canvas edge inward,
        // then the solid bright gold inner line.
        const uint32_t G = 48, steps = 6, band = G / steps + 2;
        for (uint32_t k = 0; k < steps; ++k) {
            uint32_t d = G - k * (G / steps); // 48, 40, ..., 8
            uint8_t a = (uint8_t)(60.0f + 120.0f * (float)k / (float)(steps - 1));
            cv.rect_outline(d, d, W - d, H - d, band, 218, 165, 32, a);
        }
        cv.rect_outline(G, G, W - G, H - G, 4, 255, 215, 90, 255);
    } else {
        cv.rect_outline(2, 2, W - 2, H - 2, 4, 255, 255, 255, 255);
    }

    const uint32_t icon_box = 432;
    const uint32_t icon_x = 64, icon_y = (H - icon_box) / 2;
    if (!req.icon_rgba.empty() && req.icon_size > 0) {
        cv.blit_rgba(icon_x, icon_y, icon_box, icon_box,
                     reinterpret_cast<const uint8_t *>(req.icon_rgba.data()),
                     req.icon_size, req.icon_size);
    } else {
        // Placeholder trophy-ish square when no icon is cached yet.
        cv.rect(icon_x, icon_y, icon_x + icon_box, icon_y + icon_box, 48, 52, 66, 255);
        cv.rect(icon_x + 176, icon_y + 120, icon_x + 256, icon_y + 312, 200, 180, 60, 255);
    }

    const uint32_t text_x = icon_x + icon_box + 64;
    const uint32_t text_max_w = W - text_x - 64;
    // Title scale 8, desc scale 6 (2x the old wrist sizes).
    auto title_lines = wrap_text(req.title, text_max_w, 8);
    uint32_t y = 72;
    for (size_t i = 0; i < title_lines.size() && i < 2; ++i) {
        cv.draw_text(title_lines[i], text_x, y, 8, 255, 255, 255);
        y += 7 * 8 + 16;
    }
    auto desc_lines = wrap_text(req.description, text_max_w, 6);
    for (size_t i = 0; i < desc_lines.size() && i < 3 && y + 7 * 6 < H - 60; ++i) {
        cv.draw_text(desc_lines[i], text_x, y, 6, 200, 200, 210);
        y += 7 * 6 + 12;
    }

    if (req.for_progress && req.max_progress > 0) {
        uint32_t bx0 = text_x, bx1 = W - 64;
        uint32_t by0 = H - 88, by1 = H - 48;
        cv.rect(bx0, by0, bx1, by1, 20, 22, 30, 255);
        float f = (float)req.progress / (float)req.max_progress;
        if (f < 0) f = 0; if (f > 1) f = 1;
        uint32_t fill = bx0 + (uint32_t)((bx1 - bx0) * f);
        if (fill > bx0) cv.rect(bx0, by0, fill, by1, 70, 140, 220, 255);
    }

    // Escape hatch for runtimes whose file loader flips images vertically.
    if (req.flip_y) cv.flip_y();

    out.rgba = std::move(cv.px);
    return true;
}

bool ComposeVRDashboardImage(const std::vector<VRDashboardEntry> &entries,
                             const std::string &header,
                             VRDashboardImage &out,
                             bool flip_y)
{
    Canvas cv(VRDashboardImage::WIDTH, VRDashboardImage::HEIGHT);
    cv.fill(31, 36, 51, 255);
    cv.rect_outline(2, 2, cv.w - 2, cv.h - 2, 4, 255, 255, 255, 120);

    uint32_t y = 48;
    cv.draw_text(header.empty() ? "GSE Achievements" : header, 48, y, 4, 255, 255, 255);
    y += 7 * 4 + 24;
    cv.rect(48, y, cv.w - 48, y + 4, 80, 84, 100, 255);
    y += 28;

    if (entries.empty()) {
        cv.draw_text("No achievements yet.", 48, y, 3, 160, 160, 170);
        cv.draw_text("Unlock one in-game and it", 48, y + 7 * 3 + 10, 3, 160, 160, 170);
        cv.draw_text("will show up here.", 48, y + 2 * (7 * 3 + 10), 3, 160, 160, 170);
        out.rgba = std::move(cv.px);
        return true;
    }

    const uint32_t row_h = 7 * 3 + 12;
    const size_t max_rows = 22;
    size_t shown = 0;
    for (size_t i = 0; i < entries.size() && shown < max_rows; ++i, ++shown) {
        const auto &e = entries[i];
        // Marker box: gold filled when unlocked (+bright edge if rare),
        // gray outline when locked.
        if (e.achieved) {
            cv.rect(48, y + 4, 48 + 22, y + 26, 218, 165, 32, 255);
            if (e.rare) cv.rect_outline(48, y + 4, 48 + 22, y + 26, 3, 255, 215, 90, 255);
            cv.draw_text(e.title, 88, y, 3, 255, 255, 255);
        } else {
            cv.rect_outline(48, y + 4, 48 + 22, y + 26, 3, 130, 130, 140, 255);
            cv.draw_text(e.title, 88, y, 3, 150, 150, 160);
        }
        if (!e.detail.empty()) {
            uint32_t dw = cv.text_w(e.detail, 3);
            uint8_t dr = e.rare ? 255 : 150, dg = e.rare ? 215 : 150, db = e.rare ? 90 : 160;
            cv.draw_text(e.detail, cv.w - 48 - dw, y, 3, dr, dg, db);
        }
        y += row_h;
    }
    if (entries.size() > shown) {
        char more[64];
        snprintf(more, sizeof(more), "... and %u more", (unsigned)(entries.size() - shown));
        cv.draw_text(more, 48, y, 3, 150, 150, 160);
    }

    if (flip_y) cv.flip_y();

    out.rgba = std::move(cv.px);
    return true;
}

static bool encode_png_mem(const uint8_t *rgba, uint32_t w, uint32_t h,
                           std::vector<uint8_t> &out_png)
{
    if (!rgba || !w || !h) return false;
    int len = 0;
    unsigned char *mem = stbi_write_png_to_mem(
        rgba, 0, (int)w, (int)h, 4, &len);
    if (!mem || len <= 0) return false;
    out_png.assign(mem, mem + len);
    STBIW_FREE(mem);
    return true;
}

bool EncodeVRToastPNG(const VRToastImage &img, std::vector<uint8_t> &out_png)
{
    if (img.rgba.size() != (size_t)VRToastImage::WIDTH * VRToastImage::HEIGHT * 4) return false;
    return encode_png_mem(img.rgba.data(), VRToastImage::WIDTH, VRToastImage::HEIGHT, out_png);
}

bool EncodeVRDashboardPNG(const VRDashboardImage &img, std::vector<uint8_t> &out_png)
{
    if (img.rgba.size() != (size_t)VRDashboardImage::WIDTH * VRDashboardImage::HEIGHT * 4) return false;
    return encode_png_mem(img.rgba.data(), VRDashboardImage::WIDTH, VRDashboardImage::HEIGHT, out_png);
}

#endif // EMU_OVERLAY
