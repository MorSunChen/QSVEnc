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
#define USE_SSE41 1
#define USE_AVX   1
#define USE_AVX2  1

#include <stdint.h>
#include <string.h>
#include <immintrin.h>

#if _MSC_VER >= 1800 && !defined(__AVX__) && !defined(_DEBUG)
static_assert(false, "do not forget to set /arch:AVX or /arch:AVX2 for this file.");
#endif

template<bool use_stream>
static void __forceinline avx2_memcpy(uint8_t *dst, uint8_t *src, int size) {
    if (size < 128) {
        for (int i = 0; i < size; i++)
            dst[i] = src[i];
        return;
    }
    uint8_t *dst_fin = dst + size;
    uint8_t *dst_aligned_fin = (uint8_t *)(((size_t)(dst_fin + 31) & ~31) - 128);
    __m256i y0, y1, y2, y3;
    const int start_align_diff = (int)((size_t)dst & 31);
    if (start_align_diff) {
        y0 = _mm256_loadu_si256((__m256i*)src);
        _mm256_storeu_si256((__m256i*)dst, y0);
        dst += 32 - start_align_diff;
        src += 32 - start_align_diff;
    }
#define _mm256_stream_switch_si256(x, ymm) ((use_stream) ? _mm256_stream_si256((x), (ymm)) : _mm256_store_si256((x), (ymm)))
    for ( ; dst < dst_aligned_fin; dst += 128, src += 128) {
        y0 = _mm256_loadu_si256((__m256i*)(src +  0));
        y1 = _mm256_loadu_si256((__m256i*)(src + 32));
        y2 = _mm256_loadu_si256((__m256i*)(src + 64));
        y3 = _mm256_loadu_si256((__m256i*)(src + 96));
        _mm256_stream_switch_si256((__m256i*)(dst +  0), y0);
        _mm256_stream_switch_si256((__m256i*)(dst + 32), y1);
        _mm256_stream_switch_si256((__m256i*)(dst + 64), y2);
        _mm256_stream_switch_si256((__m256i*)(dst + 96), y3);
    }
#undef _mm256_stream_switch_si256
    uint8_t *dst_tmp = dst_fin - 128;
    src -= (dst - dst_tmp);
    y0 = _mm256_loadu_si256((__m256i*)(src +  0));
    y1 = _mm256_loadu_si256((__m256i*)(src + 32));
    y2 = _mm256_loadu_si256((__m256i*)(src + 64));
    y3 = _mm256_loadu_si256((__m256i*)(src + 96));
    _mm256_storeu_si256((__m256i*)(dst_tmp +  0), y0);
    _mm256_storeu_si256((__m256i*)(dst_tmp + 32), y1);
    _mm256_storeu_si256((__m256i*)(dst_tmp + 64), y2);
    _mm256_storeu_si256((__m256i*)(dst_tmp + 96), y3);
    _mm256_zeroupper();
}

//本来の256bit alignr
#define MM_ABS(x) (((x) < 0) ? -(x) : (x))
#define _mm256_alignr256_epi8(a, b, i) ((i<=16) ? _mm256_alignr_epi8(_mm256_permute2x128_si256(a, b, (0x00<<4) + 0x03), b, i) : _mm256_alignr_epi8(a, _mm256_permute2x128_si256(a, b, (0x00<<4) + 0x03), MM_ABS(i-16)))

//_mm256_srli_si256, _mm256_slli_si256は
//単に128bitシフト×2をするだけの命令である
#define _mm256_bsrli_epi128 _mm256_srli_si256
#define _mm256_bslli_epi128 _mm256_slli_si256
//本当の256bitシフト
#define _mm256_srli256_si256(a, i) ((i<=16) ? _mm256_alignr_epi8(_mm256_permute2x128_si256(a, a, (0x08<<4) + 0x03), a, i) : _mm256_bsrli_epi128(_mm256_permute2x128_si256(a, a, (0x08<<4) + 0x03), MM_ABS(i-16)))
#define _mm256_slli256_si256(a, i) ((i<=16) ? _mm256_alignr_epi8(a, _mm256_permute2x128_si256(a, a, (0x00<<4) + 0x08), MM_ABS(16-i)) : _mm256_bslli_epi128(_mm256_permute2x128_si256(a, a, (0x00<<4) + 0x08), MM_ABS(i-16)))

static const _declspec(align(32)) uint8_t  Array_INTERLACE_WEIGHT[2][32] = {
    {1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3},
    {3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1}
};
#define yC_INTERLACE_WEIGHT(i) _mm256_load_si256((__m256i*)Array_INTERLACE_WEIGHT[i])

static __forceinline void separate_low_up(__m256i& x0_return_lower, __m256i& x1_return_upper) {
    __m256i x4, x5;
    const __m256i xMaskLowByte = _mm256_srli_epi16(_mm256_cmpeq_epi8(_mm256_setzero_si256(), _mm256_setzero_si256()), 8);
    x4 = _mm256_srli_epi16(x0_return_lower, 8);
    x5 = _mm256_srli_epi16(x1_return_upper, 8);

    x0_return_lower = _mm256_and_si256(x0_return_lower, xMaskLowByte);
    x1_return_upper = _mm256_and_si256(x1_return_upper, xMaskLowByte);

    x0_return_lower = _mm256_packus_epi16(x0_return_lower, x1_return_upper);
    x1_return_upper = _mm256_packus_epi16(x4, x5);
}

#pragma warning (push)
#pragma warning (disable: 4100)
void convert_yuy2_to_nv12_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    uint8_t *srcLine = (uint8_t *)src[0] + src_y_pitch_byte * crop_up + crop_left;
    uint8_t *dstYLine = (uint8_t *)dst[0];
    uint8_t *dstCLine = (uint8_t *)dst[1];
    const int y_fin = height - crop_bottom - crop_up;
    for (int y = 0; y < y_fin; y += 2) {
        uint8_t *p = srcLine;
        uint8_t *pw = p + src_y_pitch_byte;
        const int x_fin = width - crop_right - crop_left;
        __m256i y0, y1, y3;
        for (int x = 0; x < x_fin; x += 32, p += 64, pw += 64) {
            //-----------1行目---------------
            y0 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(p+32)), _mm_loadu_si128((__m128i*)(p+ 0)));
            y1 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(p+48)), _mm_loadu_si128((__m128i*)(p+16)));

            separate_low_up(y0, y1);
            y3 = y1;

            _mm256_storeu_si256((__m256i *)(dstYLine + x), y0);
            //-----------1行目終了---------------

            //-----------2行目---------------
            y0 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(pw+32)), _mm_loadu_si128((__m128i*)(pw+ 0)));
            y1 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(pw+48)), _mm_loadu_si128((__m128i*)(pw+16)));

            separate_low_up(y0, y1);

            _mm256_storeu_si256((__m256i *)(dstYLine + dst_y_pitch_byte + x), y0);
            //-----------2行目終了---------------

            y1 = _mm256_avg_epu8(y1, y3);  //VUVUVUVUVUVUVUVU
            _mm256_storeu_si256((__m256i *)(dstCLine + x), y1);
        }
        srcLine  += src_y_pitch_byte << 1;
        dstYLine += dst_y_pitch_byte << 1;
        dstCLine += dst_y_pitch_byte;
    }
    _mm256_zeroupper();
}
#pragma warning (pop)

static __forceinline __m256i yuv422_to_420_i_interpolate(__m256i y_up, __m256i y_down, int i) {
    __m256i y0, y1;
    y0 = _mm256_unpacklo_epi8(y_down, y_up);
    y1 = _mm256_unpackhi_epi8(y_down, y_up);
    y0 = _mm256_maddubs_epi16(y0, yC_INTERLACE_WEIGHT(i));
    y1 = _mm256_maddubs_epi16(y1, yC_INTERLACE_WEIGHT(i));
    y0 = _mm256_add_epi16(y0, _mm256_set1_epi16(2));
    y1 = _mm256_add_epi16(y1, _mm256_set1_epi16(2));
    y0 = _mm256_srai_epi16(y0, 2);
    y1 = _mm256_srai_epi16(y1, 2);
    y0 = _mm256_packus_epi16(y0, y1);
    return y0;
}

#pragma warning (push)
#pragma warning (disable: 4100)
void convert_yuy2_to_nv12_i_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    uint8_t *srcLine = (uint8_t *)src[0] + src_y_pitch_byte * crop_up + crop_left;
    uint8_t *dstYLine = (uint8_t *)dst[0];
    uint8_t *dstCLine = (uint8_t *)dst[1];
    const int y_fin = height - crop_bottom - crop_up;
    for (int y = 0; y < y_fin; y += 4) {
        for (int i = 0; i < 2; i++) {
            uint8_t *p = srcLine;
            uint8_t *pw = p + (src_y_pitch_byte<<1);
            __m256i y0, y1, y3;
            const int x_fin = width - crop_right - crop_left;
            for (int x = 0; x < x_fin; x += 32, p += 64, pw += 64) {
                //-----------    1+i行目   ---------------
                y0 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(p+32)), _mm_loadu_si128((__m128i*)(p+ 0)));
                y1 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(p+48)), _mm_loadu_si128((__m128i*)(p+16)));

                separate_low_up(y0, y1);
                y3 = y1;

                _mm256_storeu_si256((__m256i *)(dstYLine + x), y0);
                //-----------1+i行目終了---------------

                //-----------3+i行目---------------
                y0 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(pw+32)), _mm_loadu_si128((__m128i*)(pw+ 0)));
                y1 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(pw+48)), _mm_loadu_si128((__m128i*)(pw+16)));

                separate_low_up(y0, y1);

                _mm256_storeu_si256((__m256i *)(dstYLine + (dst_y_pitch_byte<<1) + x), y0);
                //-----------3+i行目終了---------------
                y0 = yuv422_to_420_i_interpolate(y3, y1, i);

                _mm256_storeu_si256((__m256i *)(dstCLine + x), y0);
            }
            srcLine  += src_y_pitch_byte;
            dstYLine += dst_y_pitch_byte;
            dstCLine += dst_y_pitch_byte;
        }
        srcLine  += src_y_pitch_byte << 1;
        dstYLine += dst_y_pitch_byte << 1;
    }
    _mm256_zeroupper();
}
#pragma warning (pop)

#pragma warning (push)
#pragma warning (disable: 4127)
template<bool uv_only>
static void __forceinline convert_yv12_to_nv12_avx2_base(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    //Y成分のコピー
    if (!uv_only) {
        uint8_t *srcYLine = (uint8_t *)src[0] + src_y_pitch_byte * crop_up + crop_left;
        uint8_t *dstLine = (uint8_t *)dst[0];
        const int y_fin = height - crop_bottom;
        const int y_width = width - crop_right - crop_left;
        for (int y = crop_up; y < y_fin; y++, srcYLine += src_y_pitch_byte, dstLine += dst_y_pitch_byte) {
            avx2_memcpy<false>(dstLine, srcYLine, y_width);
        }
    }
    //UV成分のコピー
    uint8_t *srcULine = (uint8_t *)src[1] + (((src_uv_pitch_byte * crop_up) + crop_left) >> 1);
    uint8_t *srcVLine = (uint8_t *)src[2] + (((src_uv_pitch_byte * crop_up) + crop_left) >> 1);
    uint8_t *dstLine = (uint8_t *)dst[1];
    const int uv_fin = (height - crop_bottom) >> 1;
    for (int y = crop_up >> 1; y < uv_fin; y++, srcULine += src_uv_pitch_byte, srcVLine += src_uv_pitch_byte, dstLine += dst_y_pitch_byte) {
        const int x_fin = width - crop_right;
        uint8_t *src_u_ptr = srcULine;
        uint8_t *src_v_ptr = srcVLine;
        uint8_t *dst_ptr = dstLine;
        __m256i y0, y1, y2;
        for (int x = crop_left; x < x_fin; x += 64, src_u_ptr += 32, src_v_ptr += 32, dst_ptr += 64) {
            y0 = _mm256_loadu_si256((const __m256i *)src_u_ptr);
            y1 = _mm256_loadu_si256((const __m256i *)src_v_ptr);

            y0 = _mm256_permute4x64_epi64(y0, _MM_SHUFFLE(3,1,2,0));
            y1 = _mm256_permute4x64_epi64(y1, _MM_SHUFFLE(3,1,2,0));

            y2 = _mm256_unpackhi_epi8(y0, y1);
            y0 = _mm256_unpacklo_epi8(y0, y1);

            _mm256_storeu_si256((__m256i *)(dst_ptr +  0), y0);
            _mm256_storeu_si256((__m256i *)(dst_ptr + 32), y2);
        }
    }
    _mm256_zeroupper();
}
#pragma warning (pop)

void convert_yv12_to_nv12_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    convert_yv12_to_nv12_avx2_base<false>(dst, src, width, src_y_pitch_byte, src_uv_pitch_byte, dst_y_pitch_byte, height, crop);
}

void convert_uv_yv12_to_nv12_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    convert_yv12_to_nv12_avx2_base<true>(dst, src, width, src_y_pitch_byte, src_uv_pitch_byte, dst_y_pitch_byte, height, crop);
}

#pragma warning (push)
#pragma warning (disable: 4100)
void convert_rgb3_to_rgb4_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    uint8_t *dstLine = (uint8_t *)dst[0];
    const char __declspec(align(32)) MASK_RGB3_TO_RGB4[] = {
        0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
        0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1
    };
    __m256i yMask = _mm256_load_si256((__m256i*)MASK_RGB3_TO_RGB4);
    for (int y = height - crop_up - 1; y >= crop_bottom; y--, dstLine += dst_y_pitch_byte) {
        uint8_t *ptr_src = (uint8_t *)src[0] + (src_y_pitch_byte * y) + crop_left * 3;
        uint8_t *ptr_dst = dstLine;
        int x = 0, x_fin = width - crop_left - crop_right - 32;
        for ( ; x < x_fin; x += 32, ptr_dst += 128, ptr_src += 96) {
            __m256i y0 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(ptr_src+48)), _mm_loadu_si128((__m128i*)(ptr_src+ 0))); //384,   0
            __m256i y1 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(ptr_src+64)), _mm_loadu_si128((__m128i*)(ptr_src+16))); //512, 128
            __m256i y2 = _mm256_set_m128i(_mm_loadu_si128((__m128i*)(ptr_src+80)), _mm_loadu_si128((__m128i*)(ptr_src+32))); //640, 256
            __m256i y3 = _mm256_srli_si256(y2, 4);
            y3 = _mm256_shuffle_epi8(y3, yMask); // 896, 384
            y2 = _mm256_alignr_epi8(y2, y1, 8);
            y2 = _mm256_shuffle_epi8(y2, yMask); // 768, 256
            y1 = _mm256_alignr_epi8(y1, y0, 12);
            y1 = _mm256_shuffle_epi8(y1, yMask); // 640, 128
            y0 = _mm256_shuffle_epi8(y0, yMask); // 512,   0
            _mm256_storeu_si256((__m256i*)(ptr_dst +  0), _mm256_permute2x128_si256(y0, y1, (2<<4) | 0)); // 128,   0
            _mm256_storeu_si256((__m256i*)(ptr_dst + 32), _mm256_permute2x128_si256(y2, y3, (2<<4) | 0)); // 384, 256
            _mm256_storeu_si256((__m256i*)(ptr_dst + 64), _mm256_permute2x128_si256(y0, y1, (3<<4) | 1)); // 640, 512
            _mm256_storeu_si256((__m256i*)(ptr_dst + 96), _mm256_permute2x128_si256(y2, y3, (3<<4) | 1)); // 896, 768
        }
        x_fin = width - crop_left - crop_right;
        for ( ; x < x_fin; x++, ptr_dst += 4, ptr_src += 3) {
            *(int *)ptr_dst = *(int *)ptr_src;
            ptr_dst[3] = 0;
        }
    }
    _mm256_zeroupper();
}
#pragma warning (pop)

#pragma warning (push)
#pragma warning (disable: 4100)
void convert_rgb4_to_rgb4_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    uint8_t *srcLine = (uint8_t *)src[0] + src_y_pitch_byte * (height - crop_up - 1) + crop_left * 4;
    uint8_t *dstLine = (uint8_t *)dst[0];
    const int y_fin = height - crop_bottom - crop_up;
    const int y_width = width - crop_right - crop_left;
    for (int y = 0; y < y_fin; y++, dstLine += dst_y_pitch_byte, srcLine -= src_y_pitch_byte) {
        avx2_memcpy<false>(dstLine, srcLine, y_width * 4);
    }
    _mm256_zeroupper();
}
#pragma warning (pop)

void convert_yuv42010_to_p101_avx2(void **dst, void **src, int width, int src_y_pitch_byte, int src_uv_pitch_byte, int dst_y_pitch_byte, int height, int *crop) {
    const int crop_left   = crop[0];
    const int crop_up     = crop[1];
    const int crop_right  = crop[2];
    const int crop_bottom = crop[3];
    //Y成分のコピー
    uint8_t *srcYLine = (uint8_t *)src[0] + src_y_pitch_byte * crop_up + crop_left;
    uint8_t *dstLine = (uint8_t *)dst[0];
    const int y_fin = height - crop_bottom;
    const int y_width = width - crop_right - crop_left;
    for (int y = crop_up; y < y_fin; y++, srcYLine += src_y_pitch_byte, dstLine += dst_y_pitch_byte) {
        avx2_memcpy<false>(dstLine, srcYLine, y_width * sizeof(uint16_t));
    }
    //UV成分のコピー
    uint8_t *srcULine = (uint8_t *)src[1] + (((src_uv_pitch_byte * crop_up) + crop_left) >> 1);
    uint8_t *srcVLine = (uint8_t *)src[2] + (((src_uv_pitch_byte * crop_up) + crop_left) >> 1);
    dstLine = (uint8_t *)dst[1];
    const int uv_fin = (height - crop_bottom) >> 1;
    for (int y = crop_up >> 1; y < uv_fin; y++, srcULine += src_uv_pitch_byte, srcVLine += src_uv_pitch_byte, dstLine += dst_y_pitch_byte) {
        const int x_fin = (width - crop_right) * sizeof(uint16_t);
        uint8_t *src_u_ptr = srcULine;
        uint8_t *src_v_ptr = srcVLine;
        uint8_t *dst_ptr = dstLine;
        __m256i y0, y1, y2;
        for (int x = crop_left; x < x_fin; x += 64, src_u_ptr += 32, src_v_ptr += 32, dst_ptr += 64) {
            y0 = _mm256_loadu_si256((const __m256i *)src_u_ptr);
            y1 = _mm256_loadu_si256((const __m256i *)src_v_ptr);

            y0 = _mm256_permute4x64_epi64(y0, _MM_SHUFFLE(3,1,2,0));
            y1 = _mm256_permute4x64_epi64(y1, _MM_SHUFFLE(3,1,2,0));

            y2 = _mm256_unpackhi_epi16(y0, y1);
            y0 = _mm256_unpacklo_epi16(y0, y1);

            _mm256_storeu_si256((__m256i *)(dst_ptr +  0), y0);
            _mm256_storeu_si256((__m256i *)(dst_ptr + 32), y2);
        }
    }
    _mm256_zeroupper();
}
