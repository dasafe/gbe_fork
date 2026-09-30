#ifdef EMU_OVERLAY

#include "overlay/vr_toast_image.h"
#include <algorithm>
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
    // Steam-ish dark toast background (#1F2433 ~ 0.12/0.14/0.21).
    cv.fill(31, 36, 51, 255);
    // Border: gold for rare, black otherwise.
    if (req.rare) cv.rect(0, 0, cv.w, cv.h, 32, 24, 8, 255);
    else cv.rect(0, 0, cv.w, cv.h, 0, 0, 0, 255);
    cv.rect(6, 6, cv.w - 6, cv.h - 6, 31, 36, 51, 255);

    const uint32_t icon_box = 216;
    const uint32_t icon_x = 32, icon_y = (cv.h - icon_box) / 2;
    if (!req.icon_rgba.empty() && req.icon_size > 0) {
        cv.blit_rgba(icon_x, icon_y, icon_box, icon_box,
                     reinterpret_cast<const uint8_t *>(req.icon_rgba.data()),
                     req.icon_size, req.icon_size);
    } else {
        // Placeholder trophy-ish square when no icon is cached yet.
        cv.rect(icon_x, icon_y, icon_x + icon_box, icon_y + icon_box, 48, 52, 66, 255);
        cv.rect(icon_x + 88, icon_y + 60, icon_x + 128, icon_y + 156, 200, 180, 60, 255);
    }

    const uint32_t text_x = icon_x + icon_box + 32;
    const uint32_t text_max_w = cv.w - text_x - 32;
    // Title scale 4 (20px rows), desc scale 3.
    auto title_lines = wrap_text(req.title, text_max_w, 4);
    uint32_t y = 36;
    for (size_t i = 0; i < title_lines.size() && i < 2; ++i) {
        cv.draw_text(title_lines[i], text_x, y, 4, 255, 255, 255);
        y += 7 * 4 + 8;
    }
    auto desc_lines = wrap_text(req.description, text_max_w, 3);
    for (size_t i = 0; i < desc_lines.size() && i < 3 && y + 7 * 3 < cv.h - 30; ++i) {
        cv.draw_text(desc_lines[i], text_x, y, 3, 200, 200, 210);
        y += 7 * 3 + 6;
    }

    if (req.for_progress && req.max_progress > 0) {
        uint32_t bx0 = text_x, bx1 = cv.w - 32;
        uint32_t by0 = cv.h - 44, by1 = cv.h - 24;
        cv.rect(bx0, by0, bx1, by1, 20, 22, 30, 255);
        float f = (float)req.progress / (float)req.max_progress;
        if (f < 0) f = 0; if (f > 1) f = 1;
        uint32_t fill = bx0 + (uint32_t)((bx1 - bx0) * f);
        if (fill > bx0) cv.rect(bx0, by0, fill, by1, 70, 140, 220, 255);
    }

    out.rgba = std::move(cv.px);
    return true;
}

bool EncodeVRToastPNG(const VRToastImage &img, std::vector<uint8_t> &out_png)
{
    if (img.rgba.size() != (size_t)VRToastImage::WIDTH * VRToastImage::HEIGHT * 4) return false;
    int len = 0;
    unsigned char *mem = stbi_write_png_to_mem(
        img.rgba.data(), 0,
        (int)VRToastImage::WIDTH, (int)VRToastImage::HEIGHT, 4, &len);
    if (!mem || len <= 0) return false;
    out_png.assign(mem, mem + len);
    STBIW_FREE(mem);
    return true;
}

#endif // EMU_OVERLAY
