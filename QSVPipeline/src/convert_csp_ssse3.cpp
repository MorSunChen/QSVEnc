﻿//  -----------------------------------------------------------------------------------------
//    QSVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  ---------------------------------------------------------------------------------------

#define USE_SSE2  1
#define USE_SSSE3 1
#define USE_SSE41 0
#define USE_AVX   0
#define USE_AVX2  0
#include "convert_csp_simd.h"

void convert_yuy2_to_nv12_i_ssse3(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
	convert_yuy2_to_nv12_i_simd(dst, src, width, src_y_pitch_byte, src_uv_pitch_byte, dst_y_pitch_byte, height, crop);
}

void convert_rgb3_to_rgb4_ssse3(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
	convert_rgb3_to_rgb4_simd(dst, src, width, src_y_pitch_byte, src_uv_pitch_byte, dst_y_pitch_byte, height, crop);
}