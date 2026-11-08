// F3DEX2 GBI definitions and RDRAM readers for the embedded Fast3D core (lib/fast3d).
//
// Why this exists: upstream Fast3D includes the game's <PR/gbi.h> and reads display lists, vertices and lights through
// host structs. Here the display lists live in the recompiled game's RDRAM, so this header (a) defines only the F3DEX2
// constants the interpreter uses and (b) reads RDRAM the way N64Recomp lays it out.
//
// RDRAM layout (N64Recomp): memory is stored in 32-bit words that already hold the N64's big-endian value in host byte
// order. A 32-bit word at N64 address A reads natively as *(uint32_t *)(rdram + A) (A 4-byte aligned). A byte at A
// lives at rdram[A ^ 3] and a halfword at A lives at *(uint16_t *)(rdram + (A ^ 2)). RT64 does the same (rdp.cpp
// `RDRAM[addr ^ 3]`). So Gfx commands (two words) and fixed-point matrices (words) are readable directly, while
// vertices, lights, viewports and texture/palette bytes need the byte/halfword accessors below.
//
// Opcode and flag values are the F3DEX2 ones; checked against lib/rt64 gbi/rt64_gbi_f3dex2.cpp and
// shared/rt64_f3d_defines.h.
#ifndef F3D_GBI_H
#define F3D_GBI_H

#include <stdint.h>

#define F3DEX_GBI_2 1

typedef struct Gfx {
    struct {
        uint32_t w0;
        uint32_t w1;
    } words;
} Gfx;

// RSP commands (F3DEX2).
#define G_VTX 0x01
#define G_TRI1 0x05
#define G_TRI2 0x06
#define G_QUAD 0x07
#define G_POPMTX 0xD8
#define G_TEXTURE 0xD7
#define G_GEOMETRYMODE 0xD9
#define G_MTX 0xDA
#define G_MOVEWORD 0xDB
#define G_MOVEMEM 0xDC
#define G_DL 0xDE
#define G_ENDDL 0xDF
#define G_SETOTHERMODE_L 0xE2
#define G_SETOTHERMODE_H 0xE3

// RDP commands.
#define G_TEXRECT 0xE4
#define G_TEXRECTFLIP 0xE5
#define G_SETSCISSOR 0xED
#define G_LOADTLUT 0xF0
#define G_SETTILESIZE 0xF2
#define G_LOADBLOCK 0xF3
#define G_LOADTILE 0xF4
#define G_SETTILE 0xF5
#define G_FILLRECT 0xF6
#define G_SETFILLCOLOR 0xF7
#define G_SETFOGCOLOR 0xF8
#define G_SETPRIMCOLOR 0xFA
#define G_SETENVCOLOR 0xFB
#define G_SETCOMBINE 0xFC
#define G_SETTIMG 0xFD
#define G_SETZIMG 0xFE
#define G_SETCIMG 0xFF

// G_MTX flags (F3DEX2 flips the push bit, handled in the interpreter).
#define G_MTX_MODELVIEW 0x00
#define G_MTX_PROJECTION 0x04
#define G_MTX_MUL 0x00
#define G_MTX_LOAD 0x02
#define G_MTX_NOPUSH 0x00
#define G_MTX_PUSH 0x01

// Geometry mode.
#define G_ZBUFFER 0x00000001
#define G_SHADE 0x00000004
#define G_CULL_FRONT 0x00000200
#define G_CULL_BACK 0x00000400
#define G_CULL_BOTH 0x00000600
#define G_FOG 0x00010000
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000

// G_MOVEMEM / G_MOVEWORD indices.
#define G_MV_VIEWPORT 8
#define G_MV_LIGHT 10
#define G_MW_NUMLIGHT 0x02
#define G_MW_SEGMENT 0x06
#define G_MW_FOG 0x08

// Image formats and sizes.
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV 1
#define G_IM_FMT_CI 2
#define G_IM_FMT_IA 3
#define G_IM_FMT_I 4
#define G_IM_SIZ_4b 0
#define G_IM_SIZ_8b 1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3

// Texture wrap flags (cms/cmt in G_SETTILE).
#define G_TX_MIRROR 0x1
#define G_TX_CLAMP 0x2

#define G_TX_LOADTILE 7
#define G_TX_RENDERTILE 0
#define G_TEXTURE_IMAGE_FRAC 2

// Other mode (low word / high word bits the interpreter reads).
#define G_MDSFT_TEXTFILT 12
#define G_TF_POINT (0 << G_MDSFT_TEXTFILT)
#define G_MDSFT_CYCLETYPE 20
#define G_CYC_1CYCLE (0 << G_MDSFT_CYCLETYPE)
#define G_CYC_2CYCLE (1 << G_MDSFT_CYCLETYPE)
#define G_CYC_COPY (2 << G_MDSFT_CYCLETYPE)
#define G_CYC_FILL (3 << G_MDSFT_CYCLETYPE)
#define G_AC_DITHER 3
#define Z_UPD 0x00000020
#define ZMODE_DEC 0x00000C00
#define CVG_X_ALPHA 0x00001000
#define G_BL_CLR_FOG 3
#define G_BL_A_MEM 1

// Color combiner mux inputs (cycle-1 color; alpha muxes share the low values).
#define G_CCMUX_TEXEL0 1
#define G_CCMUX_TEXEL1 2
#define G_CCMUX_PRIMITIVE 3
#define G_CCMUX_SHADE 4
#define G_CCMUX_ENVIRONMENT 5
#define G_CCMUX_TEXEL0_ALPHA 8
#define G_CCMUX_LOD_FRACTION 13
#define G_ACMUX_TEXEL0 1
#define G_ACMUX_PRIMITIVE 3
#define G_ACMUX_SHADE 4

// Decoded vertex, light and viewport (host order, filled by the RDRAM readers below).
typedef struct Vtx_t {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
} Vtx_t;

typedef struct Vtx_tn {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    int8_t n[3];
    uint8_t a;
} Vtx_tn;

typedef struct Light_t {
    uint8_t col[3];
    char pad1;
    uint8_t colc[3];
    char pad2;
    int8_t dir[3];
    char pad3;
} Light_t;

typedef struct Vp_t {
    int16_t vscale[4];
    int16_t vtrans[4];
} Vp_t;

#define F3D_VTX_SIZE 16
#define F3D_LIGHT_SIZE 24

// Byte and halfword access by RDRAM offset (see the layout note at the top).
static inline uint8_t f3d_rd_u8(const uint8_t *rdram, uint32_t addr) {
    return rdram[addr ^ 3];
}

static inline uint16_t f3d_rd_u16(const uint8_t *rdram, uint32_t addr) {
    return (uint16_t)((f3d_rd_u8(rdram, addr) << 8) | f3d_rd_u8(rdram, addr + 1));
}

static inline void f3d_read_vtx(const uint8_t *rdram, uint32_t addr, Vtx_t *v, Vtx_tn *vn) {
    for (int i = 0; i < 3; i++) {
        v->ob[i] = (int16_t)f3d_rd_u16(rdram, addr + i * 2);
    }
    v->flag = f3d_rd_u16(rdram, addr + 6);
    v->tc[0] = (int16_t)f3d_rd_u16(rdram, addr + 8);
    v->tc[1] = (int16_t)f3d_rd_u16(rdram, addr + 10);
    for (int i = 0; i < 4; i++) {
        v->cn[i] = f3d_rd_u8(rdram, addr + 12 + i);
    }
    // The normal shares its bytes with the vertex color.
    *vn = (Vtx_tn){{v->ob[0], v->ob[1], v->ob[2]}, v->flag, {v->tc[0], v->tc[1]},
                   {(int8_t)v->cn[0], (int8_t)v->cn[1], (int8_t)v->cn[2]}, v->cn[3]};
}

static inline void f3d_read_light(const uint8_t *rdram, uint32_t addr, Light_t *l) {
    for (int i = 0; i < 3; i++) {
        l->col[i] = f3d_rd_u8(rdram, addr + i);
        l->colc[i] = f3d_rd_u8(rdram, addr + 4 + i);
        l->dir[i] = (int8_t)f3d_rd_u8(rdram, addr + 8 + i);
    }
    l->pad1 = l->pad2 = l->pad3 = 0;
}

static inline void f3d_read_vp(const uint8_t *rdram, uint32_t addr, Vp_t *vp) {
    for (int i = 0; i < 4; i++) {
        vp->vscale[i] = (int16_t)f3d_rd_u16(rdram, addr + i * 2);
        vp->vtrans[i] = (int16_t)f3d_rd_u16(rdram, addr + 8 + i * 2);
    }
}

#endif
