// Hand-written F3DEX2 display list encoders over a fake N64Recomp-layout RDRAM, shared by the Fast3D tests.
#pragma once
#include <cstdint>

// ---- fake RDRAM and F3DEX2 encoders ----------------------------------------------------------------------------------

static uint8_t *rdram;
static uint32_t dl_cursor;

static inline void w32(uint32_t addr, uint32_t v) { *(uint32_t *)(rdram + addr) = v; }
static inline void b8(uint32_t addr, uint8_t v) { rdram[addr ^ 3] = v; }
static inline void h16(uint32_t addr, uint16_t v) {
    b8(addr, v >> 8);
    b8(addr + 1, v & 0xFF);
}
static inline void cmd(uint32_t w0, uint32_t w1) {
    w32(dl_cursor, w0);
    w32(dl_cursor + 4, w1);
    dl_cursor += 8;
}
static inline void dl_begin(uint32_t addr) { dl_cursor = addr; }

static inline void put_matrix_identity(uint32_t addr) {
    static const uint32_t ident[16] = {0x00010000, 0, 0x00000001, 0, 0, 0x00010000, 0, 0x00000001, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; i++) w32(addr + i * 4, ident[i]);
}
static inline void put_vtx(uint32_t addr, int16_t x, int16_t y, int16_t z, int16_t s, int16_t t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    h16(addr + 0, (uint16_t)x);
    h16(addr + 2, (uint16_t)y);
    h16(addr + 4, (uint16_t)z);
    h16(addr + 6, 0);
    h16(addr + 8, (uint16_t)s);
    h16(addr + 10, (uint16_t)t);
    b8(addr + 12, r);
    b8(addr + 13, g);
    b8(addr + 14, b);
    b8(addr + 15, a);
}

static inline void g_mtx(uint32_t addr, uint32_t flags) { cmd(0xDA380000u | ((flags ^ 1u) & 0xFF), addr); }
static inline void g_vertex(uint32_t addr, uint32_t n, uint32_t v0) { cmd(0x01000000u | (n << 12) | ((v0 + n) << 1), addr); }
static inline void g_tri1(uint32_t a, uint32_t b, uint32_t c) { cmd(0x05000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), 0); }
static inline void g_tri2(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    cmd(0x06000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), (d * 2 << 16) | (e * 2 << 8) | (f * 2));
}
static inline void g_quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    cmd(0x07000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), (a * 2 << 16) | (c * 2 << 8) | (d * 2));
}
static inline void g_segment(uint32_t seg, uint32_t base) { cmd(0xDB000000u | (6u << 16) | (seg * 4), base); }
static inline void g_dl(uint32_t addr, bool branch) { cmd(0xDE000000u | (branch ? 0x10000u : 0), addr); }
static inline void g_enddl() { cmd(0xDF000000u, 0); }
static inline void g_geometry_mode(uint32_t clear, uint32_t set) { cmd(0xD9000000u | (~clear & 0x00FFFFFFu), set); }
// G_SETOTHERMODE_H (F3DEX2): sets `len` bits at `shift` of the high other-mode word.
static inline void g_othermode_h(uint32_t shift, uint32_t len, uint32_t value) { cmd(0xE3000000u | ((32 - shift - len) << 8) | (len - 1), value); }
// G_SETCOMBINE with both cycles set to the same (a - b) * c + d, like the gsDPSetCombineMode macros do. Constant 0 is
// 15 (a, b), 31 (c) or 7 (d and every alpha slot) in the hardware numbering, never 0 (which is COMBINED / LOD).
static inline void g_combine_raw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t aa, uint32_t ab, uint32_t ac, uint32_t ad) {
    cmd(0xFC000000u | (a << 20) | (c << 15) | (aa << 12) | (ac << 9) | (a << 5) | c,
        (b << 28) | (b << 24) | (aa << 21) | (ac << 18) | (d << 15) | (ab << 12) | (ad << 9) | (d << 6) | (ab << 3) | ad);
}
static inline void g_combine_shade() { g_combine_raw(15, 15, 31, 4, 7, 7, 7, 4); }
// TEXEL0 * SHADE for color and alpha.
static inline void g_combine_tex_shade() { g_combine_raw(1, 15, 4, 7, 1, 7, 4, 7); }
static inline void g_settimg(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t addr) {
    cmd(0xFD000000u | (fmt << 21) | (siz << 19) | (width - 1), addr);
}
static inline void g_settile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile) {
    cmd(0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9) | tmem, tile << 24);
}
// dxt 0x800 steps one TMEM line per 64-bit word, i.e. rows one word long (the tests' tiles use line = 1). dxt 0 would leave
// every row on line 0 and the sampler would then swap the odd rows, as the RDP does for data that is not pre-swapped.
static inline void g_loadblock(uint32_t tile, uint32_t lrs, uint32_t dxt = 0x800) { cmd(0xF3000000u, (tile << 24) | (lrs << 12) | dxt); }
static inline void g_settilesize(uint32_t tile, uint32_t w, uint32_t h) { cmd(0xF2000000u, (tile << 24) | (((w - 1) << 2) << 12) | ((h - 1) << 2)); }
// SETTILE with every field: palette, clamp/mirror (cm: bit0 mirror, bit1 clamp) and mask (log2 wrap) for each axis.
static inline void g_settile_full(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                                  uint32_t cmt, uint32_t maskt, uint32_t cms, uint32_t masks) {
    cmd(0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9) | tmem,
        (tile << 24) | (palette << 20) | (cmt << 18) | (maskt << 14) | (cms << 8) | (masks << 4));
}
// SETTILESIZE for the window (x, y) of size w x h texels.
static inline void g_settilesize_at(uint32_t tile, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    cmd(0xF2000000u | ((x << 2) << 12) | (y << 2), (tile << 24) | ((((x + w - 1) << 2)) << 12) | ((y + h - 1) << 2));
}
// LOADTILE of the rectangle (x, y) .. (x + w - 1, y + h - 1) of the texture image.
static inline void g_loadtile(uint32_t tile, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    cmd(0xF4000000u | ((x << 2) << 12) | (y << 2), (tile << 24) | (((x + w - 1) << 2) << 12) | ((y + h - 1) << 2));
}
// LOADTLUT of `count` 16-bit palette entries.
// The gDPLoadTLUT macros put (count - 1) at bits 14..23 as a plain integer (gbi.h: _SHIFTL(count, 14, 10)).
static inline void g_loadtlut(uint32_t tile, uint32_t count) { cmd(0xF0000000u, (tile << 24) | ((count - 1) << 14)); }
// G_TEXTURE with the tile TEXEL0 reads.
static inline void g_texture_tile(uint32_t s, uint32_t t, uint32_t tile) { cmd(0xD7000000u | (tile << 8) | (1u << 1), (s << 16) | t); }
static inline void g_texture(uint32_t s, uint32_t t) { cmd(0xD7000000u | (1u << 1), (s << 16) | t); }
