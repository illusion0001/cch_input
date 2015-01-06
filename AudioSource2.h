#pragma once

#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/Unknown.h>
#include "stdint.h"

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

class VDFFInputFile;

/*
 * Note: This is an extension of VDXWAVEFORMATEX,
 *   but since vdinputdriver.h marks VDXWAVEFORMATEX
 *   as pack(8) instead of pack(1) as it is in the original
 *   Windows definition, it cannot be extended directly.
 */
#pragma pack(push, 1)
struct VDXWAVEFORMATEXTENSIBLE
{
  uint16_t mFormatTag;
  uint16_t mChannels;
  uint32_t mSamplesPerSec;
  uint32_t mAvgBytesPerSec;
  uint16_t mBlockAlign;
  uint16_t mBitsPerSample;
  uint16_t mExtraSize;
  /*
  uint16_t mValidBitsPerSample;
  uint32_t mChannelMask;
  uint32_t mSubFormat[4];	// A GUID
  */
};
#pragma pack(pop)
#define SIZEOF_VDXWAVEFORMATEX (sizeof(VDXWAVEFORMATEXTENSIBLE))
//#define SIZEOF_VDXWAVEFORMATEX (offsetof(VDXWAVEFORMATEXTENSIBLE, mValidBitsPerSample))

class VDFFAudioSource : public vdxunknown<IVDXStreamSource>, public IVDXAudioSource {
public:
  VDFFAudioSource(const VDXInputDriverContext& context);
  ~VDFFAudioSource();

  int VDXAPIENTRY AddRef();
  int VDXAPIENTRY Release();
  void *VDXAPIENTRY AsInterface(uint32_t iid);

  void		VDXAPIENTRY GetStreamSourceInfo(VDXStreamSourceInfo& srcInfo) { srcInfo = m_streamInfo; }
  bool		VDXAPIENTRY Read(int64_t lStart, uint32_t lCount, void *lpBuffer, uint32_t cbBuffer, uint32_t *lBytesRead, uint32_t *lSamplesRead);

  const void *VDXAPIENTRY GetDirectFormat() { return &mRawFormat; }
  int			VDXAPIENTRY GetDirectFormatLen() { return SIZEOF_VDXWAVEFORMATEX/* + mRawFormat.mExtraSize*/; }

  ErrorMode VDXAPIENTRY GetDecodeErrorMode() { return  IVDXStreamSource::kErrorModeReportAll; }
  void VDXAPIENTRY SetDecodeErrorMode(ErrorMode mode) {}
  bool VDXAPIENTRY IsDecodeErrorModeSupported(ErrorMode mode) { return mode == IVDXStreamSource::kErrorModeReportAll; }

  bool VDXAPIENTRY IsVBR(){ return false; }
  int64_t VDXAPIENTRY TimeToPositionVBR(int64_t us){ return 0; }
  int64_t VDXAPIENTRY PositionToTimeVBR(int64_t samples){ return 0; }

  void VDXAPIENTRY GetAudioSourceInfo(VDXAudioSourceInfo& info) { info.mFlags = 0; }

public:
  const VDXInputDriverContext& mContext;
  VDXWAVEFORMATEXTENSIBLE mRawFormat;
  VDXStreamSourceInfo	m_streamInfo;

  VDFFInputFile* m_pSource;
  int m_streamIndex;
  int64_t sample_count;
  AVRational time_base;
  int64_t start_time;
  int64_t time_adjust;

  AVFormatContext* m_pFormatCtx;
  AVStream* m_pStreamCtx;
  AVCodecContext* m_pCodecCtx;
  SwrContext* swr;
  AVFrame* frame;
  int src_linesize;
  uint64_t out_layout;
  AVSampleFormat out_fmt;

  struct BufferPage{
    enum {size=0x8000}; // max usable value 0xFFFF

    uint16_t a0,a1; // range a
    uint16_t b0,b1; // range b
    uint8_t* p;

    void reset(){ a0=0; a1=0; b0=0; b1=0; p=0; }
    int copy(int s0, uint32_t count, void* dst, int sample_size);
    int alloc(int s0, uint32_t count, int& changed);
  };

  BufferPage* buffer;
  int buffer_size; // in pages
  int used_pages;
  int used_pages_max;
  int first_page;
  int last_page;

  int64_t next_sample;
  int discard_samples;
  bool trust_sample_pos;

  int initStream( VDFFInputFile* pSource, int streamIndex );
  void init_start_time();
  int read_packet(AVPacket& pkt);
  void insert_silence(int64_t start, uint32_t count);
  void invalidate(int64_t start, uint32_t count);
  void alloc_page(int i);
};
