﻿//  -----------------------------------------------------------------------------------------
//    QSVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  ---------------------------------------------------------------------------------------

#include <fcntl.h>
#include <algorithm>
#include <numeric>
#include <cctype>
#include <cmath>
#include <climits>
#include <memory>
#include "qsv_plugin.h"
#include "qsv_thread.h"
#include "avcodec_reader.h"
#include "avcodec_qsv_log.h"

#ifdef LIBVA_SUPPORT
#include "qsv_hw_va.h"
#include "qsv_allocator.h"
#endif //#if LIBVA_SUPPORT

#if ENABLE_AVCODEC_QSV_READER

static inline void extend_array_size(VideoFrameData *dataset) {
    static int default_capacity = 8 * 1024;
    int current_cap = dataset->capacity;
    dataset->capacity = (current_cap) ? current_cap * 2 : default_capacity;
    dataset->frame = (FramePos *)realloc(dataset->frame, dataset->capacity * sizeof(dataset->frame[0]));
    memset(dataset->frame + current_cap, 0, sizeof(dataset->frame[0]) * (dataset->capacity - current_cap));
}

CAvcodecReader::CAvcodecReader() {
    memset(&m_Demux.format, 0, sizeof(m_Demux.format));
    memset(&m_Demux.video,  0, sizeof(m_Demux.video));
    m_strReaderName = _T("avqsv");
}

CAvcodecReader::~CAvcodecReader() {
    Close();
}

void CAvcodecReader::CloseThread() {
    m_Demux.thread.bAbortInput = true;
    if (m_Demux.thread.thInput.joinable()) {
        m_Demux.qVideoPkt.set_capacity(SIZE_MAX);
        m_Demux.qVideoPkt.set_keep_length(0);
        m_Demux.thread.thInput.join();
        AddMessage(QSV_LOG_DEBUG, _T("Closed Input thread.\n"));
    }
    m_Demux.thread.bAbortInput = false;
}

void CAvcodecReader::CloseFormat(AVDemuxFormat *pFormat) {
    //close video file
    if (pFormat->pFormatCtx) {
        avformat_close_input(&pFormat->pFormatCtx);
        AddMessage(QSV_LOG_DEBUG, _T("Closed avformat context.\n"));
    }
    if (m_Demux.format.pFormatOptions) {
        av_dict_free(&m_Demux.format.pFormatOptions);
    }
    memset(pFormat, 0, sizeof(pFormat[0]));
}

void CAvcodecReader::CloseVideo(AVDemuxVideo *pVideo) {
    //close parser
    if (pVideo->pParserCtx) {
        av_parser_close(pVideo->pParserCtx);
    }
    //close bitstreamfilter
    if (pVideo->pH264Bsfc) {
        av_bitstream_filter_close(pVideo->pH264Bsfc);
    }
    
    if (pVideo->pExtradata) {
        av_free(pVideo->pExtradata);
    }

    memset(pVideo, 0, sizeof(pVideo[0]));
    pVideo->nIndex = -1;
}

void CAvcodecReader::CloseStream(AVDemuxStream *pStream) {
    if (pStream->pktSample.data) {
        av_packet_unref(&pStream->pktSample);
    }
    memset(pStream, 0, sizeof(pStream[0]));
    pStream->nIndex = -1;
}

void CAvcodecReader::Close() {
    AddMessage(QSV_LOG_DEBUG, _T("Closing...\n"));
    //リソースの解放
    CloseThread();
    m_Demux.qVideoPkt.close([](AVPacket *pkt) { av_packet_unref(pkt); });
    for (uint32_t i = 0; i < m_Demux.qStreamPktL1.size(); i++) {
        av_packet_unref(&m_Demux.qStreamPktL1[i]);
    }
    m_Demux.qStreamPktL1.clear();
    m_Demux.qStreamPktL2.close([](AVPacket *pkt) { av_packet_unref(pkt); });
    AddMessage(QSV_LOG_DEBUG, _T("Cleared Stream Packet Buffer.\n"));

    CloseFormat(&m_Demux.format);
    CloseVideo(&m_Demux.video);   AddMessage(QSV_LOG_DEBUG, _T("Closed video.\n"));
    for (int i = 0; i < (int)m_Demux.stream.size(); i++) {
        CloseStream(&m_Demux.stream[i]);
        AddMessage(QSV_LOG_DEBUG, _T("Cleared Stream #%d.\n"), i);
    }
    m_Demux.stream.clear();
    m_Demux.chapter.clear();

    m_sTrimParam.list.clear();
    m_sTrimParam.offset = 0;

    m_hevcMp42AnnexbBuffer.clear();

    //free input buffer (使用していない)
    //if (buffer) {
    //    free(buffer);
    //    buffer = nullptr;
    //}
    m_pEncSatusInfo.reset();
    if (m_sFramePosListLog.length()) {
        m_Demux.frames.printList(m_sFramePosListLog.c_str());
    }
    m_Demux.frames.clear();

    AddMessage(QSV_LOG_DEBUG, _T("Closed.\n"));
}

uint32_t CAvcodecReader::getQSVFourcc(uint32_t id) {
    for (int i = 0; i < _countof(QSV_DECODE_LIST); i++)
        if (QSV_DECODE_LIST[i].codec_id == id)
            return QSV_DECODE_LIST[i].qsv_fourcc;
    return 0;
}

vector<int> CAvcodecReader::getStreamIndex(AVMediaType type) {
    vector<int> streams;
    const int n_streams = m_Demux.format.pFormatCtx->nb_streams;
    for (int i = 0; i < n_streams; i++) {
        if (m_Demux.format.pFormatCtx->streams[i]->codec->codec_type == type) {
            streams.push_back(i);
        }
    }
    return std::move(streams);
}

bool CAvcodecReader::vc1StartCodeExists(uint8_t *ptr) {
    uint32_t code = readUB32(ptr);
    return check_range_unsigned(code, 0x010A, 0x010F) || check_range_unsigned(code, 0x011B, 0x011F);
}

void CAvcodecReader::hevcMp42Annexb(AVPacket *pkt) {
    static const uint8_t SC[] = { 0, 0, 0, 1 };
    const uint8_t *ptr, *ptr_fin;
    if (pkt == NULL) {
        m_hevcMp42AnnexbBuffer.reserve(m_Demux.video.nExtradataSize + 128);
        ptr = m_Demux.video.pExtradata;
        ptr_fin = ptr + m_Demux.video.nExtradataSize;
        ptr += 0x16;
    } else {
        m_hevcMp42AnnexbBuffer.reserve(pkt->size + 128);
        ptr = pkt->data;
        ptr_fin = ptr + pkt->size;
    }
    const int numOfArrays = *ptr;
    ptr += !!numOfArrays;

    while (ptr + 6 < ptr_fin) {
        ptr += !!numOfArrays;
        const int count = readUB16(ptr); ptr += 2;
        int units = (numOfArrays) ? count : 1;
        for (int i = (std::max)(1, units); i; i--) {
            uint32_t size = readUB16(ptr); ptr += 2;
            uint32_t uppper = count << 16;
            size += (numOfArrays) ? 0 : uppper;
            m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), SC, SC+4);
            m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), ptr, ptr+size); ptr += size;
        }
    }
    if (pkt) {
        if (pkt->buf->size < (int)m_hevcMp42AnnexbBuffer.size()) {
            av_grow_packet(pkt, (int)m_hevcMp42AnnexbBuffer.size());
        }
        memcpy(pkt->data, m_hevcMp42AnnexbBuffer.data(), m_hevcMp42AnnexbBuffer.size());
        pkt->size = (int)m_hevcMp42AnnexbBuffer.size();
    } else {
        if (m_Demux.video.pExtradata) {
            av_free(m_Demux.video.pExtradata);
        }
        m_Demux.video.pExtradata = (uint8_t *)av_malloc(m_hevcMp42AnnexbBuffer.size());
        m_Demux.video.nExtradataSize = (int)m_hevcMp42AnnexbBuffer.size();
        memcpy(m_Demux.video.pExtradata, m_hevcMp42AnnexbBuffer.data(), m_hevcMp42AnnexbBuffer.size());
    }
    m_hevcMp42AnnexbBuffer.clear();
}

void CAvcodecReader::vc1FixHeader(int nLengthFix) {
    if (m_Demux.video.pCodecCtx->codec_id == AV_CODEC_ID_WMV3) {
        m_Demux.video.nExtradataSize += nLengthFix;
        uint32_t datasize = m_Demux.video.nExtradataSize;
        vector<uint8_t> buffer(20 + datasize, 0);
        uint32_t header = 0xC5000000;
        uint32_t width = m_Demux.video.pCodecCtx->width;
        uint32_t height = m_Demux.video.pCodecCtx->height;
        uint8_t *dataPtr = m_Demux.video.pExtradata - nLengthFix;
        memcpy(buffer.data() +  0, &header, sizeof(header));
        memcpy(buffer.data() +  4, &datasize, sizeof(datasize));
        memcpy(buffer.data() +  8, dataPtr, datasize);
        memcpy(buffer.data() +  8 + datasize, &height, sizeof(height));
        memcpy(buffer.data() + 12 + datasize, &width, sizeof(width));
        m_Demux.video.pExtradata = (uint8_t *)av_realloc(m_Demux.video.pExtradata, sizeof(buffer) + FF_INPUT_BUFFER_PADDING_SIZE);
        m_Demux.video.nExtradataSize = (int)buffer.size();
        memcpy(m_Demux.video.pExtradata, buffer.data(), buffer.size());
    } else {
        m_Demux.video.nExtradataSize += nLengthFix;
        memmove(m_Demux.video.pExtradata, m_Demux.video.pExtradata - nLengthFix, m_Demux.video.nExtradataSize);
    }
}

void CAvcodecReader::vc1AddFrameHeader(AVPacket *pkt) {
    uint32_t size = pkt->size;
    if (m_Demux.video.pCodecCtx->codec_id == AV_CODEC_ID_WMV3) {
        av_grow_packet(pkt, 8);
        memmove(pkt->data + 8, pkt->data, size);
        memcpy(pkt->data, &size, sizeof(size));
        memset(pkt->data + 4, 0, 4);
    } else if (!vc1StartCodeExists(pkt->data)) {
        uint32_t startCode = 0x0D010000;
        av_grow_packet(pkt, sizeof(startCode));
        memmove(pkt->data + sizeof(startCode), pkt->data, size);
        memcpy(pkt->data, &startCode, sizeof(startCode));
    }
}

mfxStatus CAvcodecReader::getFirstFramePosAndFrameRate(const sTrim *pTrimList, int nTrimCount) {
    AVRational fpsDecoder = m_Demux.video.pCodecCtx->framerate;
    const bool fpsDecoderInvalid = (fpsDecoder.den == 0 || fpsDecoder.num == 0);
    //timebaseが60で割り切れない場合には、ptsが完全には割り切れない値である場合があり、より多くのフレーム数を解析する必要がある
    int maxCheckFrames = (m_Demux.format.nAnalyzeSec == 0) ? ((m_Demux.video.pCodecCtx->pkt_timebase.den >= 1000 && m_Demux.video.pCodecCtx->pkt_timebase.den % 60) ? 128 : 48) : 7200;
    int maxCheckSec = (m_Demux.format.nAnalyzeSec == 0) ? INT_MAX : m_Demux.format.nAnalyzeSec;
    AddMessage(QSV_LOG_DEBUG, _T("fps decoder invalid: %s\n"), fpsDecoderInvalid ? _T("true") : _T("false"));

    AVPacket pkt;
    av_init_packet(&pkt);

    const bool bCheckDuration = m_Demux.video.pCodecCtx->pkt_timebase.num * m_Demux.video.pCodecCtx->pkt_timebase.den > 0;
    const double timebase = (bCheckDuration) ? m_Demux.video.pCodecCtx->pkt_timebase.num / (double)m_Demux.video.pCodecCtx->pkt_timebase.den : 1.0;
    m_Demux.video.nStreamFirstKeyPts = 0;
    int i_samples = 0;
    std::vector<int> frameDurationList;
    vector<std::pair<int, int>> durationHistgram;

    for (int i_retry = 0; i_retry < 5; i_retry++) {
        if (i_retry) {
            //フレームレート推定がうまくいかなそうだった場合、もう少しフレームを解析してみる
            maxCheckFrames <<= 1;
            if (maxCheckSec != INT_MAX) {
                maxCheckSec <<= 1;
            }
            //ヒストグラム生成などは最初からやり直すので、一度クリアする
            durationHistgram.clear();
            frameDurationList.clear();
        }
        for (; i_samples < maxCheckFrames && !getSample(&pkt); i_samples++) {
            m_Demux.qVideoPkt.push(pkt);
            if (bCheckDuration) {
                int64_t diff = 0;
                if (pkt.dts != AV_NOPTS_VALUE && m_Demux.frames.list(0).dts != AV_NOPTS_VALUE) {
                    diff = (int)(pkt.dts - m_Demux.frames.list(0).dts);
                } else if (pkt.pts != AV_NOPTS_VALUE && m_Demux.frames.list(0).pts != AV_NOPTS_VALUE) {
                    diff = (int)(pkt.pts - m_Demux.frames.list(0).pts);
                }
                const int duration = (int)((double)diff * timebase + 0.5);
                if (duration >= maxCheckSec) {
                    break;
                }
            }
        }
#if _DEBUG && 0
        for (int i = 0; i < m_Demux.frames.frameNum(); i++) {
            fprintf(stderr, "%3d: pts:%I64d, poc:%3d, duration:%5d, duration2:%5d, repeat:%d\n",
                i, m_Demux.frames.list(i).pts, m_Demux.frames.list(i).poc,
                m_Demux.frames.list(i).duration, m_Demux.frames.list(i).duration2,
                m_Demux.frames.list(i).repeat_pict);
        }
#endif
        //ここまで集めたデータでpts, pocを確定させる
        m_Demux.frames.checkPtsStatus((int)av_rescale_q(1, av_inv_q(fpsDecoder), m_Demux.video.pCodecCtx->pkt_timebase));

        const int nFramesToCheck = m_Demux.frames.fixedNum();
        AddMessage(QSV_LOG_DEBUG, _T("read %d packets.\n"), m_Demux.frames.frameNum());
        AddMessage(QSV_LOG_DEBUG, _T("checking %d frame samples.\n"), nFramesToCheck);

        frameDurationList.reserve(nFramesToCheck);

        for (int i = 0; i < nFramesToCheck; i++) {
#if _DEBUG && 0
            fprintf(stderr, "%3d: pts:%I64d, poc:%3d, duration:%5d, duration2:%5d, repeat:%d\n",
                i, m_Demux.frames.list(i).pts, m_Demux.frames.list(i).poc,
                m_Demux.frames.list(i).duration, m_Demux.frames.list(i).duration2,
                m_Demux.frames.list(i).repeat_pict);
#endif
            if (m_Demux.frames.list(i).poc != AVQSV_POC_INVALID) {
                int duration = m_Demux.frames.list(i).duration + m_Demux.frames.list(i).duration2;
                //RFF用の補正
                if (m_Demux.frames.list(i).repeat_pict > 1) {
                    duration = (int)(duration * 2 / (double)(m_Demux.frames.list(i).repeat_pict + 1) + 0.5);
                }
                frameDurationList.push_back(duration);
            }
        }

        //durationのヒストグラムを作成
        std::for_each(frameDurationList.begin(), frameDurationList.end(), [&durationHistgram](const int& duration) {
            auto target = std::find_if(durationHistgram.begin(), durationHistgram.end(), [duration](const std::pair<int, int>& pair) { return pair.first == duration; });
            if (target != durationHistgram.end()) {
                target->second++;
            } else {
                durationHistgram.push_back(std::make_pair(duration, 1));
            }
        });
        //多い順にソートする
        std::sort(durationHistgram.begin(), durationHistgram.end(), [](const std::pair<int, int>& pairA, const std::pair<int, int>& pairB) { return pairA.second > pairB.second; });

        AddMessage(QSV_LOG_DEBUG, _T("stream timebase %d/%d\n"), m_Demux.video.pCodecCtx->time_base.num, m_Demux.video.pCodecCtx->time_base.den);
        AddMessage(QSV_LOG_DEBUG, _T("decoder fps     %d/%d\n"), fpsDecoder.num, fpsDecoder.den);
        AddMessage(QSV_LOG_DEBUG, _T("duration histgram of %d frames\n"), durationHistgram.size());
        for (const auto& sample : durationHistgram) {
            AddMessage(QSV_LOG_DEBUG, _T("%3d [%3d frames]\n"), sample.first, sample.second);
        }

        //ここでやめてよいか判定する
        if (i_retry == 0) {
            //初回は、唯一のdurationが得られている場合を除き再解析する
            if (durationHistgram.size() <= 1) {
                break;
            }
        } else if (durationHistgram.size() <= 1 //唯一のdurationが得られている
            || durationHistgram[0].second / (double)frameDurationList.size() > 0.95 //大半がひとつのdurationである
            || std::abs(durationHistgram[0].first - durationHistgram[1].first) <= 1) { //durationのブレが貧弱なtimebaseによる丸めによるもの(mkvなど)
            break;
        }
    }

    //durationが0でなく、最も頻繁に出てきたもの
    auto& mostPopularDuration = durationHistgram[durationHistgram.size() > 1 && durationHistgram[0].first == 0];

    struct Rational64 {
        uint64_t num;
        uint64_t den;
    } estimatedAvgFps = { 0 }, nAvgFramerate64 = { 0 }, fpsDecoder64 = { (uint64_t)fpsDecoder.num, (uint64_t)fpsDecoder.den };
    if (mostPopularDuration.first == 0) {
        m_Demux.video.nStreamPtsInvalid |= AVQSV_PTS_ALL_INVALID;
    } else {
        //avgFpsとtargetFpsが近いかどうか
        auto fps_near = [](double avgFps, double targetFps) { return std::abs(1 - avgFps / targetFps) < 0.5; };
        //durationの平均を求める (ただし、先頭は信頼ならないので、cutoff分は計算に含めない)
        //std::accumulateの初期値に"(uint64_t)0"と与えることで、64bitによる計算を実行させ、桁あふれを防ぐ
        //大きすぎるtimebaseの時に必要
        double avgDuration = std::accumulate(frameDurationList.begin(), frameDurationList.end(), (uint64_t)0, [this](const uint64_t sum, const int& duration) { return sum + duration; }) / (double)(frameDurationList.size());
        double avgFps = m_Demux.video.pCodecCtx->pkt_timebase.den / (double)(avgDuration * m_Demux.video.pCodecCtx->pkt_timebase.num);
        double torrelance = (fps_near(avgFps, 25.0) || fps_near(avgFps, 50.0)) ? 0.05 : 0.0008; //25fps, 50fps近辺は基準が甘くてよい
        if (mostPopularDuration.second / (double)frameDurationList.size() > 0.95 && std::abs(1 - mostPopularDuration.first / avgDuration) < torrelance) {
            avgDuration = mostPopularDuration.first;
            AddMessage(QSV_LOG_DEBUG, _T("using popular duration...\n"));
        }
        //durationから求めた平均fpsを計算する
        const uint64_t mul = (uint64_t)ceil(1001.0 / m_Demux.video.pCodecCtx->pkt_timebase.num);
        estimatedAvgFps.num = (uint64_t)(m_Demux.video.pCodecCtx->pkt_timebase.den / avgDuration * (double)m_Demux.video.pCodecCtx->pkt_timebase.num * mul + 0.5);
        estimatedAvgFps.den = (uint64_t)m_Demux.video.pCodecCtx->pkt_timebase.num * mul;
        
        AddMessage(QSV_LOG_DEBUG, _T("fps mul:         %d\n"),    mul);
        AddMessage(QSV_LOG_DEBUG, _T("raw avgDuration: %lf\n"),   avgDuration);
        AddMessage(QSV_LOG_DEBUG, _T("estimatedAvgFps: %I64u/%I64u\n"), estimatedAvgFps.num, estimatedAvgFps.den);
    }

    if (m_Demux.video.nStreamPtsInvalid & AVQSV_PTS_ALL_INVALID) {
        //ptsとdurationをpkt_timebaseで適当に作成する
        nAvgFramerate64 = (fpsDecoderInvalid) ? estimatedAvgFps : fpsDecoder64;
    } else {
        if (fpsDecoderInvalid) {
            nAvgFramerate64 = estimatedAvgFps;
        } else {
            double dFpsDecoder = fpsDecoder.num / (double)fpsDecoder.den;
            double dEstimatedAvgFps = estimatedAvgFps.num / (double)estimatedAvgFps.den;
            //2フレーム分程度がもたらす誤差があっても許容する
            if (std::abs(dFpsDecoder / dEstimatedAvgFps - 1.0) < (2.0 / frameDurationList.size())) {
                AddMessage(QSV_LOG_DEBUG, _T("use decoder fps...\n"));
                nAvgFramerate64 = fpsDecoder64;
            } else {
                double dEstimatedAvgFpsCompare = estimatedAvgFps.num / (double)(estimatedAvgFps.den + ((dFpsDecoder < dEstimatedAvgFps) ? 1 : -1));
                //durationから求めた平均fpsがデコーダの出したfpsの近似値と分かれば、デコーダの出したfpsを採用する
                nAvgFramerate64 = (std::abs(dEstimatedAvgFps - dFpsDecoder) < std::abs(dEstimatedAvgFpsCompare - dFpsDecoder)) ? fpsDecoder64 : estimatedAvgFps;
            }
        }
    }
    AddMessage(QSV_LOG_DEBUG, _T("final AvgFps (raw64): %I64u/%I64u\n"), estimatedAvgFps.num, estimatedAvgFps.den);

    //フレームレートが2000fpsを超えることは考えにくいので、誤判定
    //ほかのなにか使えそうな値で代用する
    if (nAvgFramerate64.num / (double)nAvgFramerate64.den > 2000.0) {
        if (fpsDecoder.den > 0 && fpsDecoder.num > 0) {
            nAvgFramerate64.num = fpsDecoder.num;
            nAvgFramerate64.den = fpsDecoder.den;
        } else if (m_Demux.video.pCodecCtx->framerate.den > 0
                && m_Demux.video.pCodecCtx->framerate.num > 0) {
            nAvgFramerate64.num = m_Demux.video.pCodecCtx->framerate.num;
            nAvgFramerate64.den = m_Demux.video.pCodecCtx->framerate.den;
        } else if (m_Demux.video.pCodecCtx->pkt_timebase.den > 0
                && m_Demux.video.pCodecCtx->pkt_timebase.num > 0) {
            nAvgFramerate64.num = m_Demux.video.pCodecCtx->pkt_timebase.den * m_Demux.video.pCodecCtx->ticks_per_frame;
            nAvgFramerate64.den = m_Demux.video.pCodecCtx->pkt_timebase.num;
        }
    }

    const uint64_t fps_gcd = qsv_gcd(nAvgFramerate64.num, nAvgFramerate64.den);
    nAvgFramerate64.num /= fps_gcd;
    nAvgFramerate64.den /= fps_gcd;
    m_Demux.video.nAvgFramerate = av_make_q((int)nAvgFramerate64.num, (int)nAvgFramerate64.den);
    AddMessage(QSV_LOG_DEBUG, _T("final AvgFps (gcd): %d/%d\n"), m_Demux.video.nAvgFramerate.num, m_Demux.video.nAvgFramerate.den);

    //近似値であれば、分母1001/分母1に合わせる
    double fps = m_Demux.video.nAvgFramerate.num / (double)m_Demux.video.nAvgFramerate.den;
    double fps_n = fps * 1001;
    int fps_n_int = (int)(fps + 0.5) * 1000;
    if (std::abs(fps_n / (double)fps_n_int - 1.0) < 1e-4) {
        m_Demux.video.nAvgFramerate.num = fps_n_int;
        m_Demux.video.nAvgFramerate.den = 1001;
    } else {
        fps_n = fps * 1000;
        fps_n_int = (int)(fps + 0.5) * 1000;
        if (std::abs(fps_n / (double)fps_n_int - 1.0) < 1e-4) {
            m_Demux.video.nAvgFramerate.num = fps_n_int / 1000;
            m_Demux.video.nAvgFramerate.den = 1;
        }
    }
    AddMessage(QSV_LOG_DEBUG, _T("final AvgFps (round): %d/%d\n\n"), m_Demux.video.nAvgFramerate.num, m_Demux.video.nAvgFramerate.den);

    auto trimList = make_vector(pTrimList, nTrimCount);
    //出力時の音声・字幕解析用に1パケットコピーしておく
    if (m_Demux.qStreamPktL1.size()) { //この時点ではまだすべての音声パケットがL1にある
        for (auto streamInfo = m_Demux.stream.begin(); streamInfo != m_Demux.stream.end(); streamInfo++) {
            if (avcodec_get_type(streamInfo->pCodecCtx->codec_id) == AVMEDIA_TYPE_AUDIO) {
                AddMessage(QSV_LOG_DEBUG, _T("checking for stream #%d\n"), streamInfo->nIndex);
                const AVPacket *pkt1 = NULL; //最初のパケット
                const AVPacket *pkt2 = NULL; //2番目のパケット
                for (int j = 0; j < (int)m_Demux.qStreamPktL1.size(); j++) {
                    if (m_Demux.qStreamPktL1[j].stream_index == streamInfo->nIndex) {
                        if (pkt1) {
                            pkt2 = &m_Demux.qStreamPktL1[j];
                            break;
                        }
                        pkt1 = &m_Demux.qStreamPktL1[j];
                    }
                }
                if (pkt1 != NULL) {
                    //1パケット目はたまにおかしいので、可能なら2パケット目を使用する
                    av_copy_packet(&streamInfo->pktSample, (pkt2) ? pkt2 : pkt1);
                    if (m_Demux.video.nStreamPtsInvalid & AVQSV_PTS_ALL_INVALID) {
                        streamInfo->nDelayOfStream = 0;
                    } else {
                        //その音声の属する動画フレーム番号
                        const int vidIndex = getVideoFrameIdx(pkt1->pts, streamInfo->pCodecCtx->pkt_timebase, 0);
                        AddMessage(QSV_LOG_DEBUG, _T("audio track %d first pts: %I64d\n"), streamInfo->nTrackId, pkt1->pts);
                        AddMessage(QSV_LOG_DEBUG, _T("      first pts videoIdx: %d\n"), vidIndex);
                        if (vidIndex >= 0) {
                            //音声の遅れているフレーム数分のdurationを足し上げる
                            int delayOfStream = (frame_inside_range(vidIndex, trimList)) ? (int)(pkt1->pts - m_Demux.frames.list(vidIndex).pts) : 0;
                            for (int iFrame = m_sTrimParam.offset; iFrame < vidIndex; iFrame++) {
                                if (frame_inside_range(iFrame, trimList)) {
                                    delayOfStream += m_Demux.frames.list(iFrame).duration;
                                }
                            }
                            streamInfo->nDelayOfStream = delayOfStream;
                            AddMessage(QSV_LOG_DEBUG, _T("audio track %d delay: %d (timebase=%d/%d)\n"),
                                streamInfo->nIndex, streamInfo->nTrackId,
                                streamInfo->nDelayOfStream, streamInfo->pCodecCtx->pkt_timebase.num, streamInfo->pCodecCtx->pkt_timebase.den);
                        }
                    }
                } else {
                    //音声の最初のサンプルを取得できていない
                    streamInfo = m_Demux.stream.erase(streamInfo) - 1;
                    AddMessage(QSV_LOG_WARN, _T("failed to find stream #%d in preread.\n"), streamInfo->nIndex);
                }
            }
        }
        if (m_Demux.stream.size() == 0) {
            //音声・字幕の最初のサンプルを取得できていないため、音声がすべてなくなってしまった
            AddMessage(QSV_LOG_ERROR, _T("failed to find audio/subtitle stream in preread.\n"));
            return MFX_ERR_UNDEFINED_BEHAVIOR;
        }
    }

    return MFX_ERR_NONE;
}

#pragma warning(push)
#pragma warning(disable:4100)
mfxStatus CAvcodecReader::Init(const TCHAR *strFileName, uint32_t ColorFormat, const void *option, CEncodingThread *pEncThread, shared_ptr<CEncodeStatusInfo> pEncSatusInfo, sInputCrop *pInputCrop) {

    Close();

    const AvcodecReaderPrm *input_prm = (const AvcodecReaderPrm *)option;

    m_Demux.video.bReadVideo = input_prm->bReadVideo;
    if (input_prm->bReadVideo) {
        m_pEncThread = pEncThread;
        m_pEncSatusInfo = pEncSatusInfo;
        memcpy(&m_sInputCrop, pInputCrop, sizeof(m_sInputCrop));
    } else {
        QSV_MEMSET_ZERO(m_sInputCrop);
    }

    if (!check_avcodec_dll()) {
        AddMessage(QSV_LOG_ERROR, error_mes_avcodec_dll_not_found());
        return MFX_ERR_NULL_PTR;
    }
    //if (!checkAvcodecLicense()) {
    //    m_strInputInfo += _T("avcodec: invalid dlls for QSVEncC.\n");
    //    return MFX_ERR_NULL_PTR;
    //}

    for (int i = 0; i < input_prm->nAudioSelectCount; i++) {
        tstring audioLog = strsprintf(_T("select audio track %s, codec %s"),
            (input_prm->ppAudioSelect[i]->nAudioSelect) ? strsprintf(_T("#%d"), input_prm->ppAudioSelect[i]->nAudioSelect).c_str() : _T("all"),
            input_prm->ppAudioSelect[i]->pAVAudioEncodeCodec);
        if (input_prm->ppAudioSelect[i]->pAudioExtractFormat) {
            audioLog += tstring(_T("format ")) + input_prm->ppAudioSelect[i]->pAudioExtractFormat;
        }
        if (input_prm->ppAudioSelect[i]->pAVAudioEncodeCodec != nullptr
            && 0 != _tcscmp(input_prm->ppAudioSelect[i]->pAVAudioEncodeCodec, AVQSV_CODEC_COPY)) {
            audioLog += strsprintf(_T("bitrate %d"), input_prm->ppAudioSelect[i]->nAVAudioEncodeBitrate);
        }
        if (input_prm->ppAudioSelect[i]->pAudioExtractFilename) {
            audioLog += tstring(_T("filename \"")) + input_prm->ppAudioSelect[i]->pAudioExtractFilename + tstring(_T("\""));
        }
        AddMessage(QSV_LOG_DEBUG, audioLog);
    }

    av_register_all();
    avcodec_register_all();
    avformatNetworkInit();
    av_log_set_level((m_pPrintMes->getLogLevel() == QSV_LOG_DEBUG) ?  AV_LOG_DEBUG : QSV_AV_LOG_LEVEL);
    av_qsv_log_set(m_pPrintMes);

    int ret = 0;
    std::string filename_char;
    if (0 == tchar_to_string(strFileName, filename_char, CP_UTF8)) {
        AddMessage(QSV_LOG_ERROR, _T("failed to convert filename to utf-8 characters.\n"));
        return MFX_ERR_INVALID_HANDLE;
    }
    m_Demux.format.bIsPipe = (0 == strcmp(filename_char.c_str(), "-")) || filename_char.c_str() == strstr(filename_char.c_str(), R"(\\.\pipe\)");
    m_Demux.format.pFormatCtx = avformat_alloc_context();
    m_Demux.format.nAnalyzeSec = input_prm->nAnalyzeSec;
    if (m_Demux.format.nAnalyzeSec) {
        if (0 != (ret = av_opt_set_int(m_Demux.format.pFormatCtx, "probesize", m_Demux.format.nAnalyzeSec * AV_TIME_BASE, 0))) {
            AddMessage(QSV_LOG_ERROR, _T("failed to set probesize to %d sec: error %d\n"), m_Demux.format.nAnalyzeSec, ret);
        } else {
            AddMessage(QSV_LOG_DEBUG, _T("set probesize: %d sec\n"), m_Demux.format.nAnalyzeSec);
        }
    }
    if (0 == strcmp(filename_char.c_str(), "-")) {
#if defined(_WIN32) || defined(_WIN64)
        if (_setmode(_fileno(stdin), _O_BINARY) < 0) {
            AddMessage(QSV_LOG_ERROR, _T("failed to switch stdin to binary mode.\n"));
            return MFX_ERR_UNKNOWN;
        }
#endif //#if defined(_WIN32) || defined(_WIN64)
        AddMessage(QSV_LOG_DEBUG, _T("input source set to stdin.\n"));
        filename_char = "pipe:0";
    }
    //ts向けの設定
    av_dict_set(&m_Demux.format.pFormatOptions, "scan_all_pmts", "1", 0);
    //ファイルのオープン
    if (avformat_open_input(&(m_Demux.format.pFormatCtx), filename_char.c_str(), nullptr, &m_Demux.format.pFormatOptions)) {
        AddMessage(QSV_LOG_ERROR, _T("error opening file: \"%s\"\n"), char_to_tstring(filename_char, CP_UTF8).c_str());
        return MFX_ERR_NULL_PTR; // Couldn't open file
    }
    AddMessage(QSV_LOG_DEBUG, _T("opened file \"%s\".\n"), char_to_tstring(filename_char, CP_UTF8).c_str());

    if (m_Demux.format.nAnalyzeSec) {
        if (0 != (ret = av_opt_set_int(m_Demux.format.pFormatCtx, "analyzeduration", m_Demux.format.nAnalyzeSec * AV_TIME_BASE, 0))) {
            AddMessage(QSV_LOG_ERROR, _T("failed to set analyzeduration to %d sec, error %d\n"), m_Demux.format.nAnalyzeSec, ret);
        } else {
            AddMessage(QSV_LOG_DEBUG, _T("set analyzeduration: %d sec\n"), m_Demux.format.nAnalyzeSec);
        }
    }

    if (avformat_find_stream_info(m_Demux.format.pFormatCtx, nullptr) < 0) {
        AddMessage(QSV_LOG_ERROR, _T("error finding stream information.\n"));
        return MFX_ERR_NULL_PTR; // Couldn't find stream information
    }
    AddMessage(QSV_LOG_DEBUG, _T("got stream information.\n"));
    av_dump_format(m_Demux.format.pFormatCtx, 0, filename_char.c_str(), 0);
    //dump_format(dec.pFormatCtx, 0, argv[1], 0);

    //キュー関連初期化
    //getFirstFramePosAndFrameRateで大量にパケットを突っ込む可能性があるので、この段階ではcapacityは無限大にしておく
    m_Demux.qVideoPkt.init(4096, SIZE_MAX, 4);
    m_Demux.qVideoPkt.set_keep_length(AVQSV_FRAME_MAX_REORDER);
    m_Demux.qStreamPktL2.init(4096);

    //音声ストリームを探す
    if (input_prm->nReadAudio || input_prm->bReadSubtitle) {
        vector<int> mediaStreams;
        if (input_prm->nReadAudio) {
            auto audioStreams = getStreamIndex(AVMEDIA_TYPE_AUDIO);
            if (audioStreams.size() == 0) {
                AddMessage(QSV_LOG_ERROR, _T("--audio-encode/--audio-copy/--audio-file is set, but no audio stream found.\n"));
                return MFX_ERR_NOT_FOUND;
            }
            m_Demux.format.nAudioTracks = (int)audioStreams.size();
            vector_cat(mediaStreams, audioStreams);
        }
        if (input_prm->bReadSubtitle) {
            auto subStreams = getStreamIndex(AVMEDIA_TYPE_SUBTITLE);
            if (subStreams.size() == 0) {
                AddMessage(QSV_LOG_ERROR, _T("--sub-copy is set, but no subtitle stream found.\n"));
                return MFX_ERR_NOT_FOUND;
            }
            m_Demux.format.nSubtitleTracks = (int)subStreams.size();
            vector_cat(mediaStreams, subStreams);
        }
        for (int iTrack = 0; iTrack < (int)mediaStreams.size(); iTrack++) {
            const AVCodecID codecId = m_Demux.format.pFormatCtx->streams[mediaStreams[iTrack]]->codec->codec_id;
            bool useStream = false;
            sAudioSelect *pAudioSelect = nullptr; //トラックに対応するsAudioSelect (字幕ストリームの場合はnullptrのまま)
            if (AVMEDIA_TYPE_SUBTITLE == avcodec_get_type(codecId)) {
                //字幕の場合
                for (int i = 0; !useStream && i < input_prm->nSubtitleSelectCount; i++) {
                    if (input_prm->pSubtitleSelect[i] == 0 //特に指定なし = 全指定かどうか
                        || input_prm->pSubtitleSelect[i] == (iTrack - m_Demux.format.nAudioTracks + 1 + input_prm->nSubtitleTrackStart)) {
                        useStream = true;
                    }
                }
            } else {
                //音声の場合
                for (int i = 0; !useStream && i < input_prm->nAudioSelectCount; i++) {
                    if (input_prm->ppAudioSelect[i]->nAudioSelect == 0 //特に指定なし = 全指定かどうか
                        || input_prm->ppAudioSelect[i]->nAudioSelect == (iTrack + input_prm->nAudioTrackStart)) {
                        useStream = true;
                        pAudioSelect = input_prm->ppAudioSelect[i];
                    }
                }
            }
            if (useStream) {
                //存在するチャンネルまでのchannel_layoutのマスクを作成する
                //特に引数を指定せず--audio-channel-layoutを指定したときには、
                //pnStreamChannelsはchannelの存在しない不要なビットまで立っているのをここで修正
                if (pAudioSelect //字幕ストリームの場合は無視
                    && isSplitChannelAuto(pAudioSelect->pnStreamChannelSelect)) {
                    const uint64_t channel_layout_mask = UINT64_MAX >> (sizeof(channel_layout_mask) * 8 - m_Demux.format.pFormatCtx->streams[mediaStreams[iTrack]]->codec->channels);
                    for (uint32_t iSubStream = 0; iSubStream < MAX_SPLIT_CHANNELS; iSubStream++) {
                        pAudioSelect->pnStreamChannelSelect[iSubStream] &= channel_layout_mask;
                    }
                    for (uint32_t iSubStream = 0; iSubStream < MAX_SPLIT_CHANNELS; iSubStream++) {
                        pAudioSelect->pnStreamChannelOut[iSubStream] &= channel_layout_mask;
                    }
                }
                
                //必要であれば、サブストリームを追加する
                for (uint32_t iSubStream = 0; iSubStream == 0 || //初回は字幕・音声含め、かならず登録する必要がある
                    (iSubStream < MAX_SPLIT_CHANNELS //最大サブストリームの上限
                        && pAudioSelect != nullptr //字幕ではない
                        && pAudioSelect->pnStreamChannelSelect[iSubStream]); //audio-splitが指定されている
                    iSubStream++) {
                    AVDemuxStream stream = { 0 };
                    stream.nTrackId = (AVMEDIA_TYPE_SUBTITLE == avcodec_get_type(codecId))
                        ? -(iTrack - m_Demux.format.nAudioTracks + 1 + input_prm->nSubtitleTrackStart) //字幕は -1, -2, -3
                        : iTrack + input_prm->nAudioTrackStart; //音声は1, 2, 3
                    stream.nIndex = mediaStreams[iTrack];
                    stream.nSubStreamId = iSubStream;
                    stream.pCodecCtx = m_Demux.format.pFormatCtx->streams[stream.nIndex]->codec;
                    stream.pStream = m_Demux.format.pFormatCtx->streams[stream.nIndex];
                    if (pAudioSelect) {
                        memcpy(stream.pnStreamChannelSelect, pAudioSelect->pnStreamChannelSelect, sizeof(stream.pnStreamChannelSelect));
                        memcpy(stream.pnStreamChannelOut,    pAudioSelect->pnStreamChannelOut,    sizeof(stream.pnStreamChannelOut));
                    }
                    m_Demux.stream.push_back(stream);
                    AddMessage(QSV_LOG_DEBUG, _T("found %s stream, stream idx %d, trackID %d.%d, %s, frame_size %d, timebase %d/%d\n"),
                        get_media_type_string(codecId).c_str(),
                        stream.nIndex, stream.nTrackId, stream.nSubStreamId, char_to_tstring(avcodec_get_name(codecId)).c_str(),
                        stream.pCodecCtx->frame_size, stream.pCodecCtx->pkt_timebase.num, stream.pCodecCtx->pkt_timebase.den);
                }
            }
        }
        //指定されたすべての音声トラックが発見されたかを確認する
        for (int i = 0; i < input_prm->nAudioSelectCount; i++) {
            //全指定のトラック=0は無視
            if (input_prm->ppAudioSelect[i]->nAudioSelect > 0) {
                bool audioFound = false;
                for (const auto& stream : m_Demux.stream) {
                    if (stream.nTrackId == input_prm->ppAudioSelect[i]->nAudioSelect) {
                        audioFound = true;
                        break;
                    }
                }
                if (!audioFound) {
                    AddMessage(QSV_LOG_ERROR, _T("could not find audio track #%d\n"), input_prm->ppAudioSelect[i]->nAudioSelect);
                    return MFX_ERR_INVALID_AUDIO_PARAM;
                }
            }
        }
    }

    if (input_prm->bReadChapter) {
        m_Demux.chapter = make_vector((const AVChapter **)m_Demux.format.pFormatCtx->chapters, m_Demux.format.pFormatCtx->nb_chapters);
    }

    //動画ストリームを探す
    //動画ストリームは動画を処理しなかったとしても同期のため必要
    auto videoStreams = getStreamIndex(AVMEDIA_TYPE_VIDEO);
    if (videoStreams.size()) {
        m_Demux.video.nIndex = videoStreams[0];
        AddMessage(QSV_LOG_DEBUG, _T("found video stream, stream idx %d\n"), m_Demux.video.nIndex);

        m_Demux.video.pCodecCtx = m_Demux.format.pFormatCtx->streams[m_Demux.video.nIndex]->codec;
    }

    //動画処理の初期化を行う
    if (input_prm->bReadVideo) {
        if (m_Demux.video.pCodecCtx == nullptr) {
            AddMessage(QSV_LOG_ERROR, _T("unable to find video stream.\n"));
            return MFX_ERR_NULL_PTR;
        }

        m_sFramePosListLog.clear();
        if (input_prm->pFramePosListLog) {
            m_sFramePosListLog = input_prm->pFramePosListLog;
        }

        memset(&m_inputFrameInfo, 0, sizeof(m_inputFrameInfo));

        //QSVでデコード可能かチェック
        if (0 == (m_nInputCodec = getQSVFourcc(m_Demux.video.pCodecCtx->codec_id))) {
            AddMessage(QSV_LOG_ERROR, _T("codec "));
            if (m_Demux.video.pCodecCtx->codec && m_Demux.video.pCodecCtx->codec->name) {
                AddMessage(QSV_LOG_ERROR, char_to_tstring(m_Demux.video.pCodecCtx->codec->name) + _T(" "));
            }
            AddMessage(QSV_LOG_ERROR, _T("unable to decode by qsv.\n"));
            return MFX_ERR_NULL_PTR;
        }
        AddMessage(QSV_LOG_DEBUG, _T("can be decoded by qsv.\n"));
        //wmv3はAdvanced Profile (3)のみの対応
        if (m_Demux.video.pCodecCtx->codec_id == AV_CODEC_ID_WMV3 && m_Demux.video.pCodecCtx->profile != 3) {
            AddMessage(QSV_LOG_ERROR, _T("unable to decode by qsv.\n"));
            return MFX_ERR_NULL_PTR;
        }

        m_Demux.format.nAVSyncMode = input_prm->nAVSyncMode;

        //必要ならbitstream filterを初期化
        if (m_Demux.video.pCodecCtx->extradata && m_Demux.video.pCodecCtx->extradata[0] == 1) {
            if (m_nInputCodec == MFX_CODEC_AVC) {
                if (NULL == (m_Demux.video.pH264Bsfc = av_bitstream_filter_init("h264_mp4toannexb"))) {
                    AddMessage(QSV_LOG_ERROR, _T("failed to init h264_mp4toannexb.\n"));
                    return MFX_ERR_NULL_PTR;
                }
                AddMessage(QSV_LOG_DEBUG, _T("initialized h264_mp4toannexb filter.\n"));
            } else if (m_nInputCodec == MFX_CODEC_HEVC) {
                m_Demux.video.bUseHEVCmp42AnnexB = true;
                AddMessage(QSV_LOG_DEBUG, _T("enabled HEVCmp42AnnexB filter.\n"));
            }
        } else if (m_Demux.video.pCodecCtx->extradata == NULL && m_Demux.video.pCodecCtx->extradata_size == 0) {
            AddMessage(QSV_LOG_ERROR, _T("video header not extracted by libavcodec.\n"));
            return MFX_ERR_UNSUPPORTED;
        }
        AddMessage(QSV_LOG_DEBUG, _T("start predecode.\n"));

        mfxStatus sts = MFX_ERR_NONE;
        mfxBitstream bitstream = { 0 };
        if (MFX_ERR_NONE != (sts = GetHeader(&bitstream))) {
            AddMessage(QSV_LOG_ERROR, _T("failed to get header.\n"));
            return sts;
        }
        if (input_prm->fSeekSec > 0.0f) {
            AVPacket firstpkt;
            getSample(&firstpkt); //現在のtimestampを取得する
            const auto pCodecCtx = m_Demux.format.pFormatCtx->streams[m_Demux.video.nIndex]->codec;
            const auto seek_time = av_rescale_q(1, av_d2q((double)input_prm->fSeekSec, 1<<24), pCodecCtx->pkt_timebase);
            int seek_ret = av_seek_frame(m_Demux.format.pFormatCtx, m_Demux.video.nIndex, firstpkt.pts + seek_time, 0);
            if (0 > seek_ret) {
                seek_ret = av_seek_frame(m_Demux.format.pFormatCtx, m_Demux.video.nIndex, firstpkt.pts + seek_time, AVSEEK_FLAG_ANY);
            }
            av_packet_unref(&firstpkt);
            if (0 > seek_ret) {
                AddMessage(QSV_LOG_ERROR, _T("failed to seek %s.\n"), print_time(input_prm->fSeekSec).c_str());
                return MFX_ERR_UNSUPPORTED;
            }
            //seekのために行ったgetSampleの結果は破棄する
            m_Demux.frames.clear();
        }

        //parserはseek後に初期化すること
        if (nullptr == (m_Demux.video.pParserCtx = av_parser_init(m_Demux.video.pCodecCtx->codec_id))) {
            AddMessage(QSV_LOG_ERROR, _T("failed to init parser for %s.\n"), char_to_tstring(m_Demux.video.pCodecCtx->codec->name).c_str());
            return MFX_ERR_NULL_PTR;
        }
        m_Demux.video.pParserCtx->flags |= PARSER_FLAG_COMPLETE_FRAMES;

        if (MFX_ERR_NONE != (sts = getFirstFramePosAndFrameRate(input_prm->pTrimList, input_prm->nTrimCount))) {
            AddMessage(QSV_LOG_ERROR, _T("failed to get first frame position.\n"));
            return sts;
        }

        m_sTrimParam.list = make_vector(input_prm->pTrimList, input_prm->nTrimCount);
        //キーフレームに到達するまでQSVではフレームが出てこない
        //そのぶんのずれを記録しておき、Trim値などに補正をかける
        if (m_sTrimParam.offset) {
            for (int i = (int)m_sTrimParam.list.size() - 1; i >= 0; i--) {
                if (m_sTrimParam.list[i].fin - m_sTrimParam.offset < 0) {
                    m_sTrimParam.list.erase(m_sTrimParam.list.begin() + i);
                } else {
                    m_sTrimParam.list[i].start = (std::max)(0, m_sTrimParam.list[i].start - m_sTrimParam.offset);
                    if (m_sTrimParam.list[i].fin != TRIM_MAX) {
                        m_sTrimParam.list[i].fin = (std::max)(0, m_sTrimParam.list[i].fin - m_sTrimParam.offset);
                    }
                }
            }
            //ずれが存在し、範囲指定がない場合はダミーの全域指定を追加する
            //これにより、自動的に音声側との同期がとれるようになる
            if (m_sTrimParam.list.size() == 0) {
                m_sTrimParam.list.push_back({ 0, TRIM_MAX });
            }
            AddMessage(QSV_LOG_DEBUG, _T("adjust trim by offset %d.\n"), m_sTrimParam.offset);
        }

        //あらかじめfpsが指定されていればそれを採用する
        if (input_prm->nVideoAvgFramerate.first * input_prm->nVideoAvgFramerate.second > 0) {
            m_Demux.video.nAvgFramerate.num = input_prm->nVideoAvgFramerate.first;
            m_Demux.video.nAvgFramerate.den = input_prm->nVideoAvgFramerate.second;
        }
        //getFirstFramePosAndFrameRateをもとにfpsを決定
        m_inputFrameInfo.FrameRateExtN = m_Demux.video.nAvgFramerate.num;
        m_inputFrameInfo.FrameRateExtD = m_Demux.video.nAvgFramerate.den;

        struct pixfmtInfo {
            AVPixelFormat pix_fmt;
            uint16_t bit_depth;
            uint16_t chroma_format;
            uint32_t fourcc;
        };

        static const pixfmtInfo pixfmtDataList[] = {
            { AV_PIX_FMT_YUV420P,      8, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_NV12 },
            { AV_PIX_FMT_YUVJ420P,     8, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_NV12 },
            { AV_PIX_FMT_NV12,         8, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_NV12 },
            { AV_PIX_FMT_NV21,         8, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_NV12 },
            { AV_PIX_FMT_YUV422P,      8, MFX_CHROMAFORMAT_YUV422, 0 },
            { AV_PIX_FMT_YUVJ422P,     8, MFX_CHROMAFORMAT_YUV422, 0 },
            { AV_PIX_FMT_YUYV422,      8, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_YUY2 },
            { AV_PIX_FMT_UYVY422,      8, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_UYVY },
            { AV_PIX_FMT_NV16,         8, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_NV16 },
            { AV_PIX_FMT_YUV444P,      8, MFX_CHROMAFORMAT_YUV444, 0 },
            { AV_PIX_FMT_YUVJ444P,     8, MFX_CHROMAFORMAT_YUV444, 0 },
            { AV_PIX_FMT_YUV420P16LE, 16, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_YUV420P14LE, 14, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_YUV420P12LE, 12, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_YUV420P10LE, 10, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_YUV420P9LE,   9, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_NV20LE,      10, MFX_CHROMAFORMAT_YUV420, MFX_FOURCC_P010 },
            { AV_PIX_FMT_YUV422P16LE, 16, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_P210 },
            { AV_PIX_FMT_YUV422P14LE, 14, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_P210 },
            { AV_PIX_FMT_YUV422P12LE, 12, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_P210 },
            { AV_PIX_FMT_YUV422P10LE, 10, MFX_CHROMAFORMAT_YUV422, MFX_FOURCC_P210 },
            { AV_PIX_FMT_YUV444P16LE, 16, MFX_CHROMAFORMAT_YUV444, 0 },
            { AV_PIX_FMT_YUV444P14LE, 14, MFX_CHROMAFORMAT_YUV444, 0 },
            { AV_PIX_FMT_YUV444P12LE, 12, MFX_CHROMAFORMAT_YUV444, 0 },
            { AV_PIX_FMT_YUV444P10LE, 10, MFX_CHROMAFORMAT_YUV444, 0 }
        };

        const auto pixfmt = m_Demux.video.pCodecCtx->pix_fmt;
        const auto pixfmtData = std::find_if(pixfmtDataList, pixfmtDataList + _countof(pixfmtDataList), [pixfmt](const pixfmtInfo& tableData) {
            return tableData.pix_fmt == pixfmt;
        });
        if (pixfmtData == (pixfmtDataList + _countof(pixfmtDataList)) || pixfmtData->fourcc == 0) {
            AddMessage(QSV_LOG_DEBUG, _T("Invalid pixel format from input file.\n"));
            return MFX_ERR_NONE;
        }

        const auto aspectRatio = m_Demux.video.pCodecCtx->sample_aspect_ratio;
        const bool bAspectRatioUnknown = aspectRatio.num * aspectRatio.den <= 0;

        //情報を格納
        m_inputFrameInfo.CropW          = (uint16_t)m_Demux.video.pCodecCtx->width;
        m_inputFrameInfo.CropH          = (uint16_t)m_Demux.video.pCodecCtx->height;
        m_inputFrameInfo.AspectRatioW   = (uint16_t)((bAspectRatioUnknown) ? 0 : aspectRatio.num);
        m_inputFrameInfo.AspectRatioH   = (uint16_t)((bAspectRatioUnknown) ? 0 : aspectRatio.den);
        m_inputFrameInfo.ChromaFormat   = pixfmtData->chroma_format;
        m_inputFrameInfo.BitDepthLuma   = pixfmtData->bit_depth;
        m_inputFrameInfo.BitDepthChroma = pixfmtData->bit_depth;
        m_inputFrameInfo.FourCC         = pixfmtData->fourcc;
        //インタレの可能性があるときは、MFX_PICSTRUCT_UNKNOWNを返すようにする
        m_inputFrameInfo.PicStruct      = (uint16_t)((m_Demux.frames.getMfxPicStruct() == MFX_PICSTRUCT_PROGRESSIVE) ? MFX_PICSTRUCT_PROGRESSIVE : MFX_PICSTRUCT_UNKNOWN);
        
        //m_inputFrameInfoのWidthとHeightには解像度をそのまま入れて、
        //他の読み込みに合わせる
        //もともとは16ないし32でアラインされた数字が入っている
        m_inputFrameInfo.Width          = m_inputFrameInfo.CropW;
        m_inputFrameInfo.Height         = m_inputFrameInfo.CropH;
        //フレーム数は未定
        uint32_t zero = 0;
        memcpy(&m_inputFrameInfo.FrameId, &zero, sizeof(zero));

        tstring mes = strsprintf(_T("avcodec video: %s, %dx%d, %d/%d fps"), CodecIdToStr(m_nInputCodec),
            m_inputFrameInfo.Width, m_inputFrameInfo.Height, m_inputFrameInfo.FrameRateExtN, m_inputFrameInfo.FrameRateExtD);
        if (input_prm->fSeekSec > 0.0f) {
            mes += strsprintf(_T("\n               seek: %s"), print_time(input_prm->fSeekSec).c_str());
        }
        AddMessage(QSV_LOG_DEBUG, _T("%s, sar %d:%d, bitdepth %d, shift %d\n"), mes.c_str(),
            m_inputFrameInfo.AspectRatioW, m_inputFrameInfo.AspectRatioH, m_inputFrameInfo.BitDepthLuma, m_inputFrameInfo.Shift);
        m_strInputInfo += mes;

        //はじめcapacityを無限大にセットしたので、この段階で制限をかける
        m_Demux.qVideoPkt.set_capacity(256);
        //スレッド関連初期化
        m_Demux.thread.bAbortInput = false;
        m_Demux.thread.nInputThread = input_prm->nInputThread;
        if (m_Demux.thread.nInputThread == QSV_INPUT_THREAD_AUTO) {
            m_Demux.thread.nInputThread = 1;
        }
        if (m_Demux.thread.nInputThread) {
            m_Demux.thread.thInput = std::thread(&CAvcodecReader::ThreadFuncRead, this);
        }
    } else {
        //音声との同期とかに使うので、動画の情報を格納する
        m_Demux.video.nAvgFramerate = av_make_q(input_prm->nVideoAvgFramerate.first, input_prm->nVideoAvgFramerate.second);

        if (input_prm->nTrimCount) {
            m_sTrimParam.list = vector<sTrim>(input_prm->pTrimList, input_prm->pTrimList + input_prm->nTrimCount);
        }
    
        if (m_Demux.video.pCodecCtx) {
            //動画の最初のフレームを取得しておく
            AVPacket pkt;
            av_init_packet(&pkt);
            //音声のみ処理モードでは、動画の先頭をキーフレームとする必要はなく、
            //先頭がキーフレームでなくてもframePosListに追加するようにして、trimをoffsetなしで反映できるようにする
            //そこで、bTreatFirstPacketAsKeyframe=trueにして最初のパケットを処理する
            getSample(&pkt, true);
            av_packet_unref(&pkt);

            m_Demux.frames.checkPtsStatus();
        }

        tstring mes;
        for (const auto& stream : m_Demux.stream) {
            if (mes.length()) mes += _T(", ");
            tstring codec_name = char_to_tstring(avcodec_get_name(stream.pCodecCtx->codec_id));
            mes += codec_name;
            AddMessage(QSV_LOG_DEBUG, _T("avcodec %s: %s from %s\n"),
                get_media_type_string(stream.pCodecCtx->codec_id).c_str(), codec_name.c_str(), strFileName);
        }
        m_strInputInfo += _T("avcodec audio: ") + mes;
    }

    return MFX_ERR_NONE;
}
#pragma warning(pop)

vector<const AVChapter *> CAvcodecReader::GetChapterList() {
    return m_Demux.chapter;
}

int CAvcodecReader::GetSubtitleTrackCount() {
    return m_Demux.format.nSubtitleTracks;
}

int CAvcodecReader::GetAudioTrackCount() {
    return m_Demux.format.nAudioTracks;
}

int64_t CAvcodecReader::GetVideoFirstKeyPts() {
    return m_Demux.video.nStreamFirstKeyPts;
}

int CAvcodecReader::getVideoFrameIdx(int64_t pts, AVRational timebase, int iStart) {
    const int framePosCount = m_Demux.frames.frameNum();
    const AVRational vid_pkt_timebase = (m_Demux.video.pCodecCtx) ? m_Demux.video.pCodecCtx->pkt_timebase : av_inv_q(m_Demux.video.nAvgFramerate);
    for (int i = (std::max)(0, iStart); i < framePosCount; i++) {
        //pts < demux.videoFramePts[i]であるなら、その前のフレームを返す
        if (0 > av_compare_ts(pts, timebase, m_Demux.frames.list(i).pts, vid_pkt_timebase)) {
            return i - 1;
        }
    }
    return framePosCount;
}

int64_t CAvcodecReader::convertTimebaseVidToStream(int64_t pts, const AVDemuxStream *pStream) {
    const AVRational vid_pkt_timebase = (m_Demux.video.pCodecCtx) ? m_Demux.video.pCodecCtx->pkt_timebase : av_inv_q(m_Demux.video.nAvgFramerate);
    return av_rescale_q(pts, vid_pkt_timebase, pStream->pCodecCtx->pkt_timebase);
}

bool CAvcodecReader::checkStreamPacketToAdd(const AVPacket *pkt, AVDemuxStream *pStream) {
    pStream->nLastVidIndex = getVideoFrameIdx(pkt->pts, pStream->pCodecCtx->pkt_timebase, pStream->nLastVidIndex);

    //該当フレームが-1フレーム未満なら、その音声はこの動画には含まれない
    if (pStream->nLastVidIndex < -1) {
        return false;
    }

    const auto vidFramePos = &m_Demux.frames.list((std::max)(pStream->nLastVidIndex, 0));
    const int64_t vid_fin = convertTimebaseVidToStream(vidFramePos->pts + ((pStream->nLastVidIndex >= 0) ? vidFramePos->duration : 0), pStream);

    const int64_t aud_start = pkt->pts;
    const int64_t aud_fin   = pkt->pts + pkt->duration;

    const bool frame_is_in_range = frame_inside_range(pStream->nLastVidIndex,     m_sTrimParam.list);
    const bool next_is_in_range  = frame_inside_range(pStream->nLastVidIndex + 1, m_sTrimParam.list);

    bool result = true; //動画に含まれる音声かどうか

    if (frame_is_in_range) {
        if (aud_fin < vid_fin || next_is_in_range) {
            ; //完全に動画フレームの範囲内か、次のフレームも範囲内なら、その音声パケットは含まれる
        //              vid_fin
        //動画 <-----------|
        //音声      |-----------|
        //     aud_start     aud_fin
        } else if (pkt->duration / 2 > (aud_fin - vid_fin + pStream->nExtractErrExcess)) {
            //はみ出した領域が少ないなら、その音声パケットは含まれる
            pStream->nExtractErrExcess += aud_fin - vid_fin;
        } else {
            //はみ出した領域が多いなら、その音声パケットは含まれない
            pStream->nExtractErrExcess -= vid_fin - aud_start;
            result = false;
        }
    } else if (next_is_in_range && aud_fin > vid_fin) {
        //             vid_fin
        //動画             |------------>
        //音声      |-----------|
        //     aud_start     aud_fin
        if (pkt->duration / 2 > (vid_fin - aud_start + pStream->nExtractErrExcess)) {
            pStream->nExtractErrExcess += vid_fin - aud_start;
        } else {
            pStream->nExtractErrExcess -= aud_fin - vid_fin;
            result = false;
        }
    } else {
        result = false;
    }
    return result;
}

AVDemuxStream *CAvcodecReader::getPacketStreamData(const AVPacket *pkt) {
    int streamIndex = pkt->stream_index;
    for (int i = 0; i < (int)m_Demux.stream.size(); i++) {
        if (m_Demux.stream[i].nIndex == streamIndex) {
            return &m_Demux.stream[i];
        }
    }
    return NULL;
}

int CAvcodecReader::getSample(AVPacket *pkt, bool bTreatFirstPacketAsKeyframe) {
    av_init_packet(pkt);
    int i_samples = 0;
    while (av_read_frame(m_Demux.format.pFormatCtx, pkt) >= 0
        //trimからわかるフレーム数の上限値よりfixedNumがある程度の量の処理を進めたら読み込みを打ち切る
        && m_Demux.frames.fixedNum() - TRIM_OVERREAD_FRAMES < getVideoTrimMaxFramIdx()) {
        if (pkt->stream_index == m_Demux.video.nIndex) {
            if (m_Demux.video.pH264Bsfc) {
                uint8_t *data = nullptr;
                int dataSize = 0;
                std::swap(m_Demux.video.pExtradata, m_Demux.video.pCodecCtx->extradata);
                std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
                av_bitstream_filter_filter(m_Demux.video.pH264Bsfc, m_Demux.video.pCodecCtx, nullptr,
                    &data, &dataSize, pkt->data, pkt->size, 0);
                std::swap(m_Demux.video.pExtradata, m_Demux.video.pCodecCtx->extradata);
                std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
                AVPacket pktProp;
                av_init_packet(&pktProp);
                av_packet_copy_props(&pktProp, pkt);
                av_packet_unref(pkt); //メモリ解放を忘れない
                av_packet_copy_props(pkt, &pktProp);
                av_packet_from_data(pkt, data, dataSize);
            }
            if (m_Demux.video.bUseHEVCmp42AnnexB) {
                hevcMp42Annexb(pkt);
            }
            if (m_nInputCodec == MFX_CODEC_VC1) {
                vc1AddFrameHeader(pkt);
            }
            //最初のキーフレームを取得するまではスキップする
            //スキップした枚数はi_samplesでカウントし、trim時に同期を適切にとるため、m_sTrimParam.offsetに格納する
            //  ただし、bTreatFirstPacketAsKeyframeが指定されている場合には、キーフレームでなくてもframePosListへの追加を許可する
            //  このモードは、対象の入力ファイルから--audio-sourceなどで音声のみ拾ってくる場合に使用する
            if (!bTreatFirstPacketAsKeyframe && !m_Demux.video.bGotFirstKeyframe && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                i_samples++;
                continue;
            } else {
                if (!m_Demux.video.bGotFirstKeyframe) {
                    //ここに入った場合は、必ず最初のキーフレーム
                    m_Demux.video.nStreamFirstKeyPts = pkt->pts;
                    m_Demux.video.bGotFirstKeyframe = true;
                    //キーフレームに到達するまでQSVではフレームが出てこない
                    //そのため、getSampleでも最初のキーフレームを取得するまでパケットを出力しない
                    //だが、これが原因でtrimの値とずれを生じてしまう
                    //そこで、そのぶんのずれを記録しておき、Trim値などに補正をかける
                    m_sTrimParam.offset = i_samples;
                }
                FramePos pos = { 0 };
                pos.pts = pkt->pts;
                pos.dts = pkt->dts;
                pos.duration = (int)pkt->duration;
                pos.duration2 = 0;
                pos.poc = AVQSV_POC_INVALID;
                pos.flags = (uint8_t)pkt->flags;
                if (m_Demux.video.pParserCtx) {
                    if (m_Demux.video.pH264Bsfc) {
                        std::swap(m_Demux.video.pExtradata, m_Demux.video.pCodecCtx->extradata);
                        std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
                    }
                    uint8_t *dummy = nullptr;
                    int dummy_size = 0;
                    av_parser_parse2(m_Demux.video.pParserCtx, m_Demux.video.pCodecCtx, &dummy, &dummy_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                    if (m_Demux.video.pH264Bsfc) {
                        std::swap(m_Demux.video.pExtradata, m_Demux.video.pCodecCtx->extradata);
                        std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
                    }
                    pos.pict_type = (uint8_t)std::max(m_Demux.video.pParserCtx->pict_type, 0);
                    switch (m_Demux.video.pParserCtx->picture_structure) {
                    //フィールドとして符号化されている
                    case AV_PICTURE_STRUCTURE_TOP_FIELD:    pos.pic_struct = AVQSV_PICSTRUCT_FIELD_TOP; break;
                    case AV_PICTURE_STRUCTURE_BOTTOM_FIELD: pos.pic_struct = AVQSV_PICSTRUCT_FIELD_BOTTOM; break;
                    //フレームとして符号化されている
                    default:
                        switch (m_Demux.video.pParserCtx->field_order) {
                        case AV_FIELD_TT:
                        case AV_FIELD_TB: pos.pic_struct = AVQSV_PICSTRUCT_FRAME_TFF; break;
                        case AV_FIELD_BT:
                        case AV_FIELD_BB: pos.pic_struct = AVQSV_PICSTRUCT_FRAME_BFF; break;
                        default:          pos.pic_struct = AVQSV_PICSTRUCT_FRAME;     break;
                        }
                    }
                    pos.repeat_pict = (uint8_t)m_Demux.video.pParserCtx->repeat_pict;
                }
                m_Demux.frames.add(pos);
            }
            //ptsの確定したところまで、音声を出力する
            CheckAndMoveStreamPacketList();
            return 0;
        }
        if (getPacketStreamData(pkt) != NULL) {
            //音声/字幕パケットはひとまずすべてバッファに格納する
            m_Demux.qStreamPktL1.push_back(*pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    //ファイルの終わりに到達
    pkt->data = nullptr;
    pkt->size = 0;
    //動画の終端を表す最後のptsを挿入する
    int64_t videoFinPts = 0;
    const int nFrameNum = m_Demux.frames.frameNum();
    if (m_Demux.video.nStreamPtsInvalid & AVQSV_PTS_ALL_INVALID) {
        videoFinPts = nFrameNum * m_Demux.frames.list(0).duration;
    } else if (nFrameNum) {
        const FramePos *lastFrame = &m_Demux.frames.list(nFrameNum - 1);
        videoFinPts = lastFrame->pts + lastFrame->duration;
    }
    //もし選択範囲が手動で決定されていないのなら、音声を最大限取得する
    if (m_sTrimParam.list.size() == 0 || m_sTrimParam.list.back().fin == TRIM_MAX) {
        for (uint32_t i = 0; i < m_Demux.qStreamPktL2.size(); i++) {
            videoFinPts = (std::max)(videoFinPts, m_Demux.qStreamPktL2[i].data.pts);
        }
        for (uint32_t i = 0; i < m_Demux.qStreamPktL1.size(); i++) {
            videoFinPts = (std::max)(videoFinPts, m_Demux.qStreamPktL1[i].pts);
        }
    }
    //最後のフレーム情報をセットし、m_Demux.framesの内部状態を終了状態に移行する
    m_Demux.frames.fin(framePos(videoFinPts, videoFinPts, 0), m_Demux.format.pFormatCtx->duration);
    //映像キューのサイズ維持制限を解除する → パイプラインに最後まで読み取らせる
    m_Demux.qVideoPkt.set_keep_length(0);
    //音声をすべて出力する
    //m_Demux.frames.finをしたので、ここで実行すれば、qAudioPktL1のデータがすべてqAudioPktL2に移される
    CheckAndMoveStreamPacketList();
    //音声のみ読み込みの場合はm_pEncSatusInfoはnullptrなので、nullチェックを行う
    if (m_pEncSatusInfo) {
        m_pEncSatusInfo->UpdateDisplay(0, 100.0);
    }
    return 1;
}

mfxStatus CAvcodecReader::setToMfxBitstream(mfxBitstream *bitstream, AVPacket *pkt) {
    mfxStatus sts = MFX_ERR_NONE;
    if (pkt->data) {
        sts = mfxBitstreamAppend(bitstream, pkt->data, pkt->size);
        auto pts = pkt->pts;
        bitstream->TimeStamp = (m_Demux.format.nAVSyncMode & QSV_AVSYNC_CHECK_PTS) ? pts : 0;
        bitstream->DataFlag  = 0;
    } else {
        sts = MFX_ERR_MORE_BITSTREAM;
    }
    return sts;
}

mfxStatus CAvcodecReader::GetNextBitstream(mfxBitstream *bitstream) {
    AVPacket pkt;
    if (!m_Demux.thread.thInput.joinable() //入力スレッドがなければ、自分で読み込む
        && m_Demux.qVideoPkt.get_keep_length() > 0) { //keep_length == 0なら読み込みは終了していて、これ以上読み込む必要はない
        if (0 == getSample(&pkt)) {
            m_Demux.qVideoPkt.push(pkt);
        }
    }

    bool bGetPacket = false;
    for (int i = 0; false == (bGetPacket = m_Demux.qVideoPkt.front_copy_and_pop_no_lock(&pkt)) && m_Demux.qVideoPkt.size() > 0; i++) {
        sleep_hybrid(i);
    }
    mfxStatus sts = MFX_ERR_MORE_BITSTREAM;
    if (bGetPacket) {
        sts = setToMfxBitstream(bitstream, &pkt);
        av_packet_unref(&pkt);
        m_Demux.video.nSampleGetCount++;
    } else {
        sts = sts;
    }
    return sts;
}

void CAvcodecReader::GetAudioDataPacketsWhenNoVideoRead() {
    m_Demux.video.nSampleGetCount++;

    AVPacket pkt;
    av_init_packet(&pkt);
    if (m_Demux.video.pCodecCtx) {
        //動画に映像がある場合、getSampleを呼んで1フレーム分の音声データをm_Demux.qStreamPktL1に取得する
        //同時に映像フレームをロードし、ロードしたptsデータを突っ込む
        if (!getSample(&pkt)) {
            //動画データ自体は不要なので解放
            av_packet_unref(&pkt);
            CheckAndMoveStreamPacketList();
        }
        return;
    } else {
        const double vidEstDurationSec = m_Demux.video.nSampleGetCount * (double)m_Demux.video.nAvgFramerate.den / (double)m_Demux.video.nAvgFramerate.num; //1フレームの時間(秒)
        //動画に映像がない場合、
        //およそ1フレーム分のパケットを取得する
        while (av_read_frame(m_Demux.format.pFormatCtx, &pkt) >= 0) {
            if (m_Demux.format.pFormatCtx->streams[pkt.stream_index]->codec->codec_type != AVMEDIA_TYPE_AUDIO) {
                av_packet_unref(&pkt);
            } else {
                AVDemuxStream *pStream = getPacketStreamData(&pkt);
                if (checkStreamPacketToAdd(&pkt, pStream)) {
                    m_Demux.qStreamPktL1.push_back(pkt);
                } else {
                    av_packet_unref(&pkt); //Writer側に渡さないパケットはここで開放する
                }

                //最初のパケットは参照用にコピーしておく
                if (pStream->pktSample.data == nullptr) {
                    av_copy_packet(&pStream->pktSample, &pkt);
                }
                uint64_t pktt = (pkt.pts == AV_NOPTS_VALUE) ? pkt.dts : pkt.pts;
                uint64_t pkt_dist = pktt - pStream->pktSample.pts;
                //1フレーム分のサンプルを取得したら終了
                if (pkt_dist * (double)pStream->pCodecCtx->pkt_timebase.num / (double)pStream->pCodecCtx->pkt_timebase.den > vidEstDurationSec) {
                    //およそ1フレーム分のパケットを設定する
                    int64_t pts = m_Demux.video.nSampleGetCount;
                    m_Demux.frames.add(framePos(pts, pts, 1, 0, m_Demux.video.nSampleGetCount, AV_PKT_FLAG_KEY));
                    if (m_Demux.frames.getStreamPtsStatus() == AVQSV_PTS_UNKNOWN) {
                        m_Demux.frames.checkPtsStatus();
                    }
                    CheckAndMoveStreamPacketList();
                    return;
                }
            }
        }
        //読み込みが終了
        int64_t pts = m_Demux.video.nSampleGetCount;
        m_Demux.frames.fin(framePos(pts, pts, 1, 0, m_Demux.video.nSampleGetCount, AV_PKT_FLAG_KEY), m_Demux.video.nSampleGetCount);
    }
}

const AVDictionary *CAvcodecReader::GetInputFormatMetadata() {
    return m_Demux.format.pFormatCtx->metadata;
}

const AVCodecContext *CAvcodecReader::GetInputVideoCodecCtx() {
    return m_Demux.video.pCodecCtx;
}

//qStreamPktL1をチェックし、framePosListから必要な音声パケットかどうかを判定し、
//必要ならqStreamPktL2に移し、不要ならパケットを開放する
void CAvcodecReader::CheckAndMoveStreamPacketList() {
    if (m_Demux.frames.fixedNum() == 0) {
        return;
    }
    //出力するパケットを選択する
    const AVRational vid_pkt_timebase = (m_Demux.video.pCodecCtx) ? m_Demux.video.pCodecCtx->pkt_timebase : av_inv_q(m_Demux.video.nAvgFramerate);
    while (!m_Demux.qStreamPktL1.empty()) {
        auto pkt = m_Demux.qStreamPktL1.front();
        AVDemuxStream *pStream = getPacketStreamData(&pkt);
        //音声のptsが映像の終わりのptsを行きすぎたらやめる
        if (0 < av_compare_ts(pkt.pts, pStream->pCodecCtx->pkt_timebase, m_Demux.frames.list(m_Demux.frames.fixedNum()).pts, vid_pkt_timebase)) {
            break;
        }
        if (checkStreamPacketToAdd(&pkt, pStream)) {
            pkt.flags = (pkt.flags & 0xffff) | (pStream->nTrackId << 16); //flagsの上位16bitには、trackIdへのポインタを格納しておく
            m_Demux.qStreamPktL2.push(pkt); //Writer側に渡したパケットはWriter側で開放する
        } else {
            av_packet_unref(&pkt); //Writer側に渡さないパケットはここで開放する
        }
        m_Demux.qStreamPktL1.pop_front();
    }
}

vector<AVPacket> CAvcodecReader::GetStreamDataPackets() {
    if (!m_Demux.video.bReadVideo) {
        GetAudioDataPacketsWhenNoVideoRead();
    }

    //出力するパケットを選択する
    vector<AVPacket> packets;
    AVPacket pkt;
    while (m_Demux.qStreamPktL2.front_copy_and_pop_no_lock(&pkt)) {
        packets.push_back(pkt);
    }
    return std::move(packets);
}

vector<AVDemuxStream> CAvcodecReader::GetInputStreamInfo() {
    return vector<AVDemuxStream>(m_Demux.stream.begin(), m_Demux.stream.end());
}

mfxStatus CAvcodecReader::GetHeader(mfxBitstream *bitstream) {
    if (bitstream == nullptr)
        return MFX_ERR_NULL_PTR;
    if (bitstream->Data == nullptr)
        if (MFX_ERR_NONE != mfxBitstreamInit(bitstream, AVCODEC_READER_INPUT_BUF_SIZE))
            return MFX_ERR_NULL_PTR;

    if (m_Demux.video.pExtradata == nullptr) {
        m_Demux.video.nExtradataSize = m_Demux.video.pCodecCtx->extradata_size;
        //ここでav_mallocを使用しないと正常に動作しない
        m_Demux.video.pExtradata = (uint8_t *)av_malloc(m_Demux.video.pCodecCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
        //ヘッダのデータをコピーしておく
        memcpy(m_Demux.video.pExtradata, m_Demux.video.pCodecCtx->extradata, m_Demux.video.nExtradataSize);
        memset(m_Demux.video.pExtradata + m_Demux.video.nExtradataSize, 0, FF_INPUT_BUFFER_PADDING_SIZE);

        if (m_Demux.video.bUseHEVCmp42AnnexB) {
            hevcMp42Annexb(NULL);
        } else if (m_Demux.video.pH264Bsfc && m_Demux.video.pExtradata[0] == 1) {
            uint8_t *dummy = NULL;
            int dummy_size = 0;
            std::swap(m_Demux.video.pExtradata,     m_Demux.video.pCodecCtx->extradata);
            std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
            av_bitstream_filter_filter(m_Demux.video.pH264Bsfc, m_Demux.video.pCodecCtx, nullptr, &dummy, &dummy_size, nullptr, 0, 0);
            std::swap(m_Demux.video.pExtradata,     m_Demux.video.pCodecCtx->extradata);
            std::swap(m_Demux.video.nExtradataSize, m_Demux.video.pCodecCtx->extradata_size);
        } else if (m_nInputCodec == MFX_CODEC_VC1) {
            int lengthFix = (0 == strcmp(m_Demux.format.pFormatCtx->iformat->name, "mpegts")) ? 0 : -1;
            vc1FixHeader(lengthFix);
        }
    }
    
    memcpy(bitstream->Data, m_Demux.video.pExtradata, m_Demux.video.nExtradataSize);
    bitstream->DataLength = m_Demux.video.nExtradataSize;
    return MFX_ERR_NONE;
}

#pragma warning(push)
#pragma warning(disable:4100)
mfxStatus CAvcodecReader::LoadNextFrame(mfxFrameSurface1 *pSurface) {
    if (m_Demux.qVideoPkt.size() == 0) {
        //m_Demux.qVideoPkt.size() == 0となるのは、最後まで読み込んだときか、中断した時しかありえない
        return MFX_ERR_MORE_DATA;
    }
    //進捗のみ表示
    m_pEncSatusInfo->m_nInputFrames++;
    double progressPercent = 0.0;
    if (m_Demux.format.pFormatCtx->duration) {
        progressPercent = m_Demux.frames.duration() * (m_Demux.video.pCodecCtx->pkt_timebase.num / (double)m_Demux.video.pCodecCtx->pkt_timebase.den) / (m_Demux.format.pFormatCtx->duration * (1.0 / (double)AV_TIME_BASE)) * 100.0;
    }
    return m_pEncSatusInfo->UpdateDisplay(0, progressPercent);
}
#pragma warning(pop)

HANDLE CAvcodecReader::getThreadHandleInput() {
    return m_Demux.thread.thInput.native_handle();
}

mfxStatus CAvcodecReader::ThreadFuncRead() {
    while (!m_Demux.thread.bAbortInput) {
        AVPacket pkt;
        if (getSample(&pkt)) {
            break;
        }
        m_Demux.qVideoPkt.push(pkt);
    }
    return MFX_ERR_NONE;
}

#endif //ENABLE_AVCODEC_QSV_READER
