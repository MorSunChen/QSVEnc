﻿//  -----------------------------------------------------------------------------------------
//    QSVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  ---------------------------------------------------------------------------------------

#include "qsv_hw_d3d9.h"
#if defined(WIN32) || defined(WIN64)
#pragma warning(disable:4068)

CQSVD3D9Device::CQSVD3D9Device() {
    QSV_MEMSET_ZERO(m_D3DPresentPrm);
    m_resetToken = 0;

    QSV_MEMSET_ZERO(m_backBufferDesc);
    QSV_MEMSET_ZERO(m_targetRect);
    QSV_MEMSET_ZERO(m_VideoDesc);
    QSV_MEMSET_ZERO(m_BltParams);
    QSV_MEMSET_ZERO(m_Sample);

    DXVA2_AYUVSample16 color = {
        0x8000,          // Cr
        0x8000,          // Cb
        0x1000,          // Y
        0xffff           // Alpha
    };

    DXVA2_ExtendedFormat format =   {           // DestFormat
        DXVA2_SampleProgressiveFrame,           // SampleFormat
        DXVA2_VideoChromaSubsampling_MPEG2,     // VideoChromaSubsampling
        DXVA_NominalRange_0_255,                // NominalRange
        DXVA2_VideoTransferMatrix_BT709,        // VideoTransferMatrix
        DXVA2_VideoLighting_bright,             // VideoLighting
        DXVA2_VideoPrimaries_BT709,             // VideoPrimaries
        DXVA2_VideoTransFunc_709                // VideoTransferFunction
    };

    memcpy(&m_VideoDesc.SampleFormat, &format, sizeof(DXVA2_ExtendedFormat));
    m_VideoDesc.SampleWidth                         = 0;
    m_VideoDesc.SampleHeight                        = 0;
    m_VideoDesc.InputSampleFreq.Numerator           = 60;
    m_VideoDesc.InputSampleFreq.Denominator         = 1;
    m_VideoDesc.OutputFrameFreq.Numerator           = 60;
    m_VideoDesc.OutputFrameFreq.Denominator         = 1;

    memcpy(&m_BltParams.DestFormat, &format, sizeof(DXVA2_ExtendedFormat));
    memcpy(&m_BltParams.BackgroundColor, &color, sizeof(DXVA2_AYUVSample16));

    m_Sample.Start = 0;
    m_Sample.End = 1;
    m_Sample.SampleFormat = format;
    m_Sample.PlanarAlpha.Fraction = 0;
    m_Sample.PlanarAlpha.Value = 1;

    m_bIsA2rgb10 = FALSE;
}

mfxStatus CQSVD3D9Device::Init(mfxHDL hWindow, uint32_t nAdapterNum) {
    mfxStatus sts = MFX_ERR_NONE;

    IDirect3D9Ex *pD3D9 = nullptr;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &pD3D9))) {
        return MFX_ERR_DEVICE_FAILED;
    }
    m_pD3D9.reset(pD3D9);

    ZeroMemory(&m_D3DPresentPrm, sizeof(m_D3DPresentPrm));
    m_D3DPresentPrm.Windowed = true;
    m_D3DPresentPrm.hDeviceWindow = (HWND)hWindow;
    m_D3DPresentPrm.Flags                      = D3DPRESENTFLAG_VIDEO;
    m_D3DPresentPrm.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
    m_D3DPresentPrm.PresentationInterval       = D3DPRESENT_INTERVAL_ONE;
    m_D3DPresentPrm.BackBufferCount            = 1;
    m_D3DPresentPrm.BackBufferFormat           = (m_bIsA2rgb10) ? D3DFMT_A2R10G10B10 : D3DFMT_X8R8G8B8;
    m_D3DPresentPrm.BackBufferWidth  = GetSystemMetrics(SM_CYSCREEN);
    m_D3DPresentPrm.BackBufferHeight = GetSystemMetrics(SM_CYSCREEN);

    if (hWindow) {
        RECT r;
        GetClientRect((HWND)hWindow, &r);
        m_D3DPresentPrm.BackBufferWidth  = (std::min<uint32_t>)(m_D3DPresentPrm.BackBufferWidth,  r.right - r.left);
        m_D3DPresentPrm.BackBufferHeight = (std::min<uint32_t>)(m_D3DPresentPrm.BackBufferHeight, r.bottom - r.top);
    }
    //
    // Mark the back buffer lockable if software DXVA2 could be used.
    // This is because software DXVA2 device requires a lockable render target
    // for the optimal performance.
    //
    m_D3DPresentPrm.Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    m_D3DPresentPrm.SwapEffect = D3DSWAPEFFECT_DISCARD;

    IDirect3DDevice9Ex *pD3DD9 = nullptr;
    if (FAILED(m_pD3D9->CreateDeviceEx(
        nAdapterNum, D3DDEVTYPE_HAL, (HWND)hWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &m_D3DPresentPrm, NULL, &pD3DD9))) {
        return MFX_ERR_NULL_PTR;
    }
    m_pD3DDevice9.reset(pD3DD9);

    if (hWindow) {
        if (   FAILED(m_pD3DDevice9->ResetEx(&m_D3DPresentPrm, NULL))
            || FAILED(m_pD3DDevice9->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0))) {
            return MFX_ERR_UNDEFINED_BEHAVIOR;
        }
    }
    UINT resetToken = 0;
    IDirect3DDeviceManager9 *pDeviceManager9 = nullptr;
    if (FAILED(DXVA2CreateDirect3DDeviceManager9(&resetToken, &pDeviceManager9))) {
        return MFX_ERR_NULL_PTR;
    }
    m_pD3DDeviceManager9.reset(pDeviceManager9);

    if (FAILED(m_pD3DDeviceManager9->ResetDevice(m_pD3DDevice9.get(), resetToken))) {
        return MFX_ERR_UNDEFINED_BEHAVIOR;
    }

    m_resetToken = resetToken;

    return sts;
}

mfxStatus CQSVD3D9Device::Reset() {
    m_D3DPresentPrm.BackBufferWidth  = GetSystemMetrics(SM_CXSCREEN);
    m_D3DPresentPrm.BackBufferHeight = GetSystemMetrics(SM_CYSCREEN);
    if (m_D3DPresentPrm.Windowed) {
        RECT r;
        GetClientRect((HWND)m_D3DPresentPrm.hDeviceWindow, &r);
        m_D3DPresentPrm.BackBufferWidth  = (std::min<uint32_t>)(m_D3DPresentPrm.BackBufferWidth,  r.right - r.left);
        m_D3DPresentPrm.BackBufferHeight = (std::min<uint32_t>)(m_D3DPresentPrm.BackBufferHeight, r.bottom - r.top);
    }

    D3DPRESENT_PARAMETERS d3dpp = m_D3DPresentPrm;
    if (   FAILED(m_pD3DDevice9->ResetEx(&d3dpp, NULL))
        || FAILED(m_pD3DDeviceManager9->ResetDevice(m_pD3DDevice9.get(), m_resetToken))) {
        return MFX_ERR_UNDEFINED_BEHAVIOR;
    }

    return MFX_ERR_NONE;
}

void CQSVD3D9Device::Close() {
    m_pDXVAVProcessor.reset();
    m_pDXVAVProcessorService.reset();

    m_pD3DDeviceManager9.reset();
    m_pD3DDevice9.reset();
    m_pD3D9.reset();
}

CQSVD3D9Device::~CQSVD3D9Device() {
    Close();
}

mfxStatus CQSVD3D9Device::GetHandle(mfxHandleType type, mfxHDL *pHdl) {
    if (MFX_HANDLE_DIRECT3D_DEVICE_MANAGER9 == type && pHdl != NULL) {
        *pHdl = m_pD3DDeviceManager9.get();

        return MFX_ERR_NONE;
    }
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus CQSVD3D9Device::CreateVideoProcessors() {
    m_pDXVAVProcessor.reset();

    ZeroMemory(&m_backBufferDesc, sizeof(m_backBufferDesc));
    IDirect3DSurface9 *backBufferTmp = nullptr;
    auto hr = m_pD3DDevice9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferTmp);
    if (NULL != backBufferTmp) {
        backBufferTmp->GetDesc(&m_backBufferDesc);
        backBufferTmp->Release();
    }
    if (FAILED(hr)) {
        return MFX_ERR_UNDEFINED_BEHAVIOR;
    }

    IDirectXVideoProcessor *pDXVAVP = nullptr;
    if (   FAILED(DXVA2CreateVideoService(m_pD3DDevice9.get(), IID_IDirectXVideoProcessorService, (void**)&m_pDXVAVProcessorService))
        || FAILED(m_pDXVAVProcessorService->CreateVideoProcessor(DXVA2_VideoProcProgressiveDevice, &m_VideoDesc, m_D3DPresentPrm.BackBufferFormat, 1, &pDXVAVP))) {
        return MFX_ERR_DEVICE_FAILED;
    }
    m_pDXVAVProcessor.reset(pDXVAVP);

    return MFX_ERR_NONE;
}

#endif // #if defined(WIN32) || defined(WIN64)