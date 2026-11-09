// Force-included (-include) into rsp/aspMain.cpp when the build targets a CPU without SSE4.1 (RUSH2_SSE41_EMULATE).
// librecomp's RSP vector unit (rsp_vu_impl.hpp) uses _mm_blendv_epi8, the only SSE4.1 intrinsic it needs, and a compiler
// refuses it below SSE4.1. This replaces it with SSE2: where a byte of `mask` has its top bit set take `b`, else `a`.
#pragma once
#if defined(__SSE2__) && !defined(__SSE4_1__)
#include <emmintrin.h>
#include <immintrin.h> // pulls in every x86 intrinsic header first, so the macro below cannot rename a declaration in one
static inline __m128i rush2_blendv_epi8(__m128i a, __m128i b, __m128i mask) {
    const __m128i select = _mm_cmplt_epi8(mask, _mm_setzero_si128()); // 0xFF where the mask byte's top bit is set
    return _mm_or_si128(_mm_and_si128(select, b), _mm_andnot_si128(select, a));
}
#define _mm_blendv_epi8 rush2_blendv_epi8
#endif
