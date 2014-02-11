﻿#ifndef _QSV_UTIL_H_
#define _QSV_UTIL_H_

#include <Windows.h>
#include <tchar.h>
#include <shlwapi.h>
#include <emmintrin.h>
#pragma comment(lib, "shlwapi.lib")
#include <string>
#include "vm/strings_defs.h"
#include "mfxstructures.h"
#include "mfxSession.h"

#ifndef MIN3
#define MIN3(a,b,c) (min((a), min((b), (c))))
#endif
#ifndef MAX3
#define MAX3(a,b,c) (max((a), max((b), (c))))
#endif

#ifndef clamp
#define clamp(x, low, high) (((x) <= (high)) ? (((x) >= (low)) ? (x) : (low)) : (high))
#endif

mfxU32 GCD(mfxU32 a, mfxU32 b);
mfxI64 GCDI64(mfxI64 a, mfxI64 b);

static const mfxVersion LIB_VER_LIST[] = {
	{ 0, 0 },
	{ 0, 1 },
	{ 1, 1 },
	{ 3, 1 },
	{ 4, 1 },
	{ 6, 1 },
	{ 7, 1 },
	{ 8, 1 },
	{ NULL, NULL } 
};

#define MFX_LIB_VERSION_0_0 LIB_VER_LIST[0]
#define MFX_LIB_VERSION_1_1 LIB_VER_LIST[2]
#define MFX_LIB_VERSION_1_3 LIB_VER_LIST[3]
#define MFX_LIB_VERSION_1_4 LIB_VER_LIST[4]
#define MFX_LIB_VERSION_1_6 LIB_VER_LIST[5]
#define MFX_LIB_VERSION_1_7 LIB_VER_LIST[6]
#define MFX_LIB_VERSION_1_8 LIB_VER_LIST[7]

BOOL Check_HWUsed(mfxIMPL impl);
mfxVersion get_mfx_libhw_version();
mfxVersion get_mfx_libsw_version();
mfxVersion get_mfx_lib_version(mfxIMPL impl);
BOOL check_lib_version(mfxVersion value, mfxVersion required);
BOOL check_lib_version(mfxU32 _value, mfxU32 _required);

enum {
	ENC_FEATURE_AUD        = 0x00000001,
	ENC_FEATURE_PIC_STRUCT = 0x00000002,
	ENC_FEATURE_CAVLC      = 0x00000004,
	ENC_FEATURE_RDO        = 0x00000008,
	ENC_FEATURE_ADAPTIVE_I = 0x00000010,
	ENC_FEATURE_ADAPTIVE_B = 0x00000020,
	ENC_FEATURE_B_PYRAMID  = 0x00000040,
	ENC_FEATURE_TRELLIS    = 0x00000080,
	ENC_FEATURE_EXT_BRC    = 0x00000100,
	ENC_FEATURE_MBBRC      = 0x00000200,
};
mfxU32 CheckEncodeFeature(mfxSession session, mfxU16 ratecontrol = MFX_RATECONTROL_VBR);
mfxU32 CheckEncodeFeature(bool hardware, mfxU16 ratecontrol, mfxVersion ver);
mfxU32 CheckEncodeFeature(bool hardware, mfxU16 ratecontrol = MFX_RATECONTROL_VBR);
void MakeFeatureListStr(mfxU32 features, std::basic_string<msdk_char>& str);

bool check_if_d3d11_necessary();

void adjust_sar(int *sar_w, int *sar_h, int width, int height);

//拡張子が一致するか確認する
static BOOL _tcheck_ext(const TCHAR *filename, const TCHAR *ext) {
	return (_tcsicmp(PathFindExtension(filename), ext) == NULL) ? TRUE : FALSE;
}

BOOL check_OS_Win8orLater();
bool isHaswellOrLater();

mfxStatus ParseY4MHeader(char *buf, mfxFrameInfo *info);

const TCHAR *get_err_mes(int sts);

static void __forceinline sse_memcpy(BYTE *dst, const BYTE *src, int size) {
	BYTE *dst_fin = dst + size;
	BYTE *dst_aligned_fin = (BYTE *)(((size_t)dst_fin & ~15) - 64);
	__m128 x0, x1, x2, x3;
	const int start_align_diff = (int)((size_t)dst & 15);
	if (start_align_diff) {
		x0 = _mm_loadu_ps((float*)src);
		_mm_storeu_ps((float*)dst, x0);
		dst += start_align_diff;
		src += start_align_diff;
	}
	for ( ; dst < dst_aligned_fin; dst += 64, src += 64) {
		x0 = _mm_loadu_ps((float*)(src +  0));
		x1 = _mm_loadu_ps((float*)(src + 16));
		x2 = _mm_loadu_ps((float*)(src + 32));
		x3 = _mm_loadu_ps((float*)(src + 48));
		_mm_store_ps((float*)(dst +  0), x0);
		_mm_store_ps((float*)(dst + 16), x1);
		_mm_store_ps((float*)(dst + 32), x2);
		_mm_store_ps((float*)(dst + 48), x3);
	}
	BYTE *dst_tmp = dst_fin - 64;
	src -= (dst - dst_tmp);
	x0 = _mm_loadu_ps((float*)(src +  0));
	x1 = _mm_loadu_ps((float*)(src + 16));
	x2 = _mm_loadu_ps((float*)(src + 32));
	x3 = _mm_loadu_ps((float*)(src + 48));
	_mm_storeu_ps((float*)(dst_tmp +  0), x0);
	_mm_storeu_ps((float*)(dst_tmp + 16), x1);
	_mm_storeu_ps((float*)(dst_tmp + 32), x2);
	_mm_storeu_ps((float*)(dst_tmp + 48), x3);
}

const int MAX_FILENAME_LEN = 1024;

#endif //_QSV_UTIL_H_
