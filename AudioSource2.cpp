#include "AudioSource2.h"
#include "InputFile2.h"

VDFFAudioSource::VDFFAudioSource(const VDXInputDriverContext& context)
  :mContext( context )
{
  m_pFormatCtx = 0;
  m_pStreamCtx = 0;
  m_pCodecCtx = 0;
  swr = 0;
  frame = 0;
  buffer = 0;
  next_sample = 0;
  discard_samples = 0;
}

VDFFAudioSource::~VDFFAudioSource()
{
  if(frame) av_frame_free(&frame);
  if(m_pCodecCtx) avcodec_close(m_pCodecCtx);
  if(swr) swr_free(&swr);
  if(m_pFormatCtx) avformat_close_input(&m_pFormatCtx);
  if(buffer){for(int i=0; i<buffer_size; i++) free(buffer[i].p); }
  free(buffer);
}

int VDFFAudioSource::AddRef()
{
  return vdxunknown<IVDXStreamSource>::AddRef();
}

int VDFFAudioSource::Release()
{
  return vdxunknown<IVDXStreamSource>::Release();
}

void *VDXAPIENTRY VDFFAudioSource::AsInterface(uint32_t iid)
{
  if (iid == IVDXAudioSource::kIID)
    return static_cast<IVDXAudioSource *>(this);

  return vdxunknown<IVDXStreamSource>::AsInterface(iid);
}

int	VDFFAudioSource::initStream(VDFFInputFile* pSource, int streamIndex)
{
  m_pFormatCtx = pSource->open_file(AVMEDIA_TYPE_AUDIO);
  if(!m_pFormatCtx) return -1;

  m_pSource = pSource;
  m_streamIndex = streamIndex;
  m_pStreamCtx = m_pFormatCtx->streams[m_streamIndex];
  m_pCodecCtx = m_pStreamCtx->codec;

  AVCodec* pDecoder = avcodec_find_decoder(m_pCodecCtx->codec_id);
  if(!pDecoder){
    char buf[80];
    av_get_codec_tag_string(buf,80,m_pCodecCtx->codec_tag);
    mContext.mpCallbacks->SetError("FFMPEG: Unsupported codec (%s)", buf);
    return -1;
  }
  m_pCodecCtx->thread_count = 1;
  if(avcodec_open2(m_pCodecCtx, pDecoder, 0)<0){
    mContext.mpCallbacks->SetError("FFMPEG: Decoder error.");
    return -1;
  }

  AVRational tb = m_pStreamCtx->time_base;
  // should normally reduce to integer if timebase is derived from sample_rate
  av_reduce(&time_base.num, &time_base.den, m_pCodecCtx->sample_rate*tb.num, tb.den, INT_MAX);

  trust_sample_pos = false;
  if(time_base.den==1) trust_sample_pos = true; // works for mp4

  if(m_pStreamCtx->duration == AV_NOPTS_VALUE){
    /*
    const char* class_name = m_pFormatCtx->iformat->priv_class->class_name;
    if(strcmp(class_name,"avi")==0){
      // pcm avi has it here, maybe bug in avidec
      // not using this now as there is no win
      sample_count = m_pStreamCtx->nb_frames;
    } else*/ {
      // this gives inexact value
      sample_count = m_pFormatCtx->duration * m_pCodecCtx->sample_rate / AV_TIME_BASE;
    }
  } else {
    AVRational tb = m_pStreamCtx->time_base;
    sample_count = m_pStreamCtx->duration * time_base.num / time_base.den;
  }

  // lazy initialized by init_start_time
  // requires video to initialize first
  start_time = AV_NOPTS_VALUE;
  time_adjust = 0;

  m_streamInfo.mSampleCount = sample_count;
  m_streamInfo.mSampleRate.mNumerator = m_pCodecCtx->sample_rate;
  m_streamInfo.mSampleRate.mDenominator = 1;
  m_streamInfo.mPixelAspectRatio.mNumerator = 0;
  m_streamInfo.mPixelAspectRatio.mDenominator = 0;

  if(m_pCodecCtx->channels>32){
    mContext.mpCallbacks->SetError("FFMPEG: Unsupported number of channels (%d)", m_pCodecCtx->channels);
    return -1;
  }
  uint64_t in_layout = m_pCodecCtx->channel_layout;
  if(!in_layout) in_layout = av_get_default_channel_layout(m_pCodecCtx->channels);
  if(!in_layout){
    mContext.mpCallbacks->SetError("FFMPEG: Unsupported channel layout.");
    return -1;
  }

  switch(in_layout){
  case AV_CH_LAYOUT_MONO:
  case AV_CH_LAYOUT_STEREO:
    out_layout = in_layout;
    break;
  default:
    out_layout = AV_CH_LAYOUT_STEREO;
  }

  switch(m_pCodecCtx->sample_fmt){
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    out_fmt = AV_SAMPLE_FMT_U8;
    break;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    out_fmt = AV_SAMPLE_FMT_S16;
    break;
  default:
    out_fmt = AV_SAMPLE_FMT_S16;
  }

  if(m_pSource->head_segment){
    VDFFAudioSource* a0 = m_pSource->head_segment->audio_source;
    out_layout = a0->out_layout;
    out_fmt = a0->out_fmt;
  }

  av_samples_get_buffer_size(&src_linesize,m_pCodecCtx->channels,1,m_pCodecCtx->sample_fmt,1);

  mRawFormat.mFormatTag		= VDXWAVEFORMATEX::kFormatPCM;
  mRawFormat.mChannels		= av_get_channel_layout_nb_channels(out_layout);
  mRawFormat.mSamplesPerSec	= m_pCodecCtx->sample_rate;
  mRawFormat.mBitsPerSample	= av_get_bytes_per_sample(out_fmt)*8;
  mRawFormat.mAvgBytesPerSec	= mRawFormat.mSamplesPerSec*mRawFormat.mChannels*mRawFormat.mBitsPerSample/8;
  mRawFormat.mBlockAlign		= mRawFormat.mChannels*mRawFormat.mBitsPerSample/8;
  mRawFormat.mExtraSize			= 0;

  swr = swr_alloc();
  av_opt_set_int(swr, "in_channel_layout",     in_layout, 0);
  av_opt_set_int(swr, "in_sample_rate",        m_pCodecCtx->sample_rate, 0);
  av_opt_set_sample_fmt(swr, "in_sample_fmt",  m_pCodecCtx->sample_fmt, 0);

  av_opt_set_int(swr, "out_channel_layout",    out_layout, 0);
  av_opt_set_int(swr, "out_sample_rate",       m_pCodecCtx->sample_rate, 0);
  av_opt_set_sample_fmt(swr, "out_sample_fmt", out_fmt,  0);
  int rr = swr_init(swr);
  if(rr<0){
    mContext.mpCallbacks->SetError("FFMPEG: Audio resampler error.");
    return -1;
  }

  frame = av_frame_alloc();

  first_page = 0;
  last_page = 0;
  used_pages = 0;
  used_pages_max = 1024;
  buffer_size = (int)((sample_count + BufferPage::size-1)/BufferPage::size);
  buffer = (BufferPage*)malloc(buffer_size*sizeof(BufferPage));
  memset(buffer,0,buffer_size*sizeof(BufferPage));

  next_sample = -1;

  return 0;
}

void VDFFAudioSource::init_start_time()
{
  start_time = m_pStreamCtx->start_time;
  time_adjust = 0;
  // offset start time so that it will match first video frame
  int vs = m_pSource->find_stream(m_pFormatCtx, AVMEDIA_TYPE_VIDEO);
  if(vs!=-1){
    AVStream* video = m_pFormatCtx->streams[vs];
    AVRational at = m_pStreamCtx->time_base;
    AVRational vt = video->time_base;

    // reference formula
    // -video_start_time * (vt.num/vt.den) / (at.num/at.den)

    int64_t d = -(m_pSource->video_start_time*vt.num*at.den)/vt.den/at.num;
    start_time += d;
    time_adjust += d;
    //sample_count += d * time_base.num / time_base.den;
  }
}

bool VDFFAudioSource::Read(int64_t start, uint32_t count, void *lpBuffer, uint32_t cbBuffer, uint32_t *lBytesRead, uint32_t *lSamplesRead)
{
  if(start>=sample_count){
    VDFFAudioSource* a1 = 0;
    if(m_pSource->next_segment) a1 = m_pSource->next_segment->audio_source;
    if(a1) return a1->Read(start-sample_count,count,lpBuffer,cbBuffer,lBytesRead,lSamplesRead);
  }

  if(!lpBuffer){
    *lBytesRead = 0;
    *lSamplesRead = 1;
    return false;
  }

  if(start_time==AV_NOPTS_VALUE) init_start_time();

  int px = (int)(start / BufferPage::size);
  int s0 = start % BufferPage::size;

  if(px<0 || px>=buffer_size){
    *lBytesRead = 0;
    *lSamplesRead = 0;
    return false;
  }

  if(start_time>0){
    int64_t real_start = start_time * time_base.num / time_base.den;
    if(start<real_start){
      // read before actual stream
      int n = start+count<real_start ? count : int(real_start-start);
      insert_silence(start,n);
      next_sample = real_start;
    }
  }

  int n = buffer[px].copy(s0,count,lpBuffer,mRawFormat.mBlockAlign);
  if(n>0){
    *lBytesRead = n*mRawFormat.mBlockAlign;
    *lSamplesRead = n;
    return true;
  }

  if(start>next_sample+m_pCodecCtx->sample_rate || start<next_sample){
    // required to seek
    discard_samples = start>=1024 ? 1024 : 0;
    int64_t pos = (start-discard_samples) * time_base.den / time_base.num - time_adjust;
    if(start==0 && pos>0) pos = 0;
    avcodec_flush_buffers(m_pCodecCtx);
    av_seek_frame(m_pFormatCtx,m_streamIndex,pos,AVSEEK_FLAG_BACKWARD);
    next_sample = -1;
  }

  AVPacket pkt;
  pkt.data = 0;
  pkt.size = 0;

  while(1){
    int rf = av_read_frame(m_pFormatCtx, &pkt);
    if(rf<0){
      // typically end of stream
      // may result from inexact sample_count too
      insert_silence(start,count);
    } else {
      AVPacket orig_pkt = pkt;
      do {
        int s = pkt.size;
        if(pkt.stream_index == m_streamIndex){
          s = read_packet(pkt);
          if(s<0) break;
        }

        pkt.data += s;
        pkt.size -= s;
      } while (pkt.size > 0);
      av_packet_unref(&orig_pkt);
    }

    int n = buffer[px].copy(s0,count,lpBuffer,mRawFormat.mBlockAlign);
    if(n>0){
      *lBytesRead = n*mRawFormat.mBlockAlign;
      *lSamplesRead = n;
      return true;
    }
  }

  *lBytesRead = 0;
  *lSamplesRead = 0;
  return false;
}

void VDFFAudioSource::insert_silence(int64_t start, uint32_t count)
{
  while(count){
    int px = (int)(start / BufferPage::size);
    int s0 = start % BufferPage::size;

    alloc_page(px);
    BufferPage& bp = buffer[px];

    int changed = 0;
    int n = bp.alloc(s0,count,changed);
    if(changed){
      uint8_t* dst = bp.p + s0*mRawFormat.mBlockAlign;
      int src = mRawFormat.mBitsPerSample==8 ? 0x80 : 0;
      memset(dst, src, n*mRawFormat.mBlockAlign);
    }

    start += n;
    count -= n;
  }
}

void VDFFAudioSource::invalidate(int64_t start, uint32_t count)
{
  while(count){
    int px = (int)(start / BufferPage::size);
    int s0 = start % BufferPage::size;

    BufferPage& bp = buffer[px];
    int n = s0+count<BufferPage::size ? count : BufferPage::size-s0;
    if(bp.a0 && s0<=bp.a0){
      bp.a0 = 0;
      bp.a1 = 0;
      bp.b0 = 0;
      bp.b1 = 0;
    } else if(s0<bp.a1){
      bp.a1 = s0;
      bp.b0 = 0;
      bp.b1 = 0;
    } else if(bp.b0 && s0<=bp.b0){
      bp.b0 = 0;
      bp.b1 = 0;
    } else if(s0<bp.b1){
      bp.b1 = s0;
    }

    start += n;
    count -= n;
  }
}

int VDFFAudioSource::read_packet(AVPacket& pkt)
{
  int got_frame;
  int ret = avcodec_decode_audio4(m_pCodecCtx, frame, &got_frame, &pkt);
  if(ret<0) return ret;
  if(got_frame){
    int64_t start = (frame->pkt_pts + time_adjust) * time_base.num / time_base.den;
    int count = frame->nb_samples;

    int src_pos = 0;

    // ignore samples to discard
    // this is workaround for some defect with AAC decoding (maybe other format too)
    if(discard_samples){
      if(count>discard_samples){
        int n = discard_samples;
        discard_samples = 0;
        src_pos = n;
        start += n;
        count -= n;
      } else {
        discard_samples -= count;
        start += count;
        count = 0;
      }
    }

    // ignore samples before start
    if(start<0){
      int64_t n = -start;
      if(n<count){
        src_pos = int(n);
        start = 0;
        count -= int(n);

      } else {
        start = 0;
        count = 0;
      }
    }

    // ignore samples after end
    if(start+count>sample_count){
      if(start<sample_count){
        count = int(sample_count-start);
      } else {
        start = sample_count;
        count = 0;
      }
    }

    if(next_sample!=-1 && start!=next_sample){
      trust_sample_pos = false;
      start = next_sample;
    }

    /*
    if(next_sample!=-1 && start>next_sample){
      // found gap between packets (maybe stream error?)
      insert_silence(next_sample,int(start-next_sample));
    }
    */

    while(count){
      int px = (int)(start / BufferPage::size);
      int s0 = start % BufferPage::size;

      alloc_page(px);
      BufferPage& bp = buffer[px];

      int changed = 0;
      int n = bp.alloc(s0,count,changed);
      if(changed){
        uint8_t* dst = bp.p + s0*mRawFormat.mBlockAlign;
        const uint8_t* src[32];
        {for(int i=0; i<frame->channels; i++) src[i] = frame->extended_data[i] + src_pos*src_linesize; }
        swr_convert(swr, &dst, n, src, n);
      }

      src_pos += n;
      start += n;
      count -= n;
    }

    next_sample = start;
    
    // we cannot reliably join cached regions
    // so create gap to force to continue decoding
    if(!trust_sample_pos) invalidate(next_sample,1);
  }
  return FFMIN(ret, pkt.size);
}

void VDFFAudioSource::alloc_page(int i)
{
  BufferPage& bp = buffer[i];
  if(bp.p) return;

  uint8_t* buf = 0;

  if(used_pages>used_pages_max) while(1){
    if(last_page>i){
      if(buffer[last_page].p){
        if(buf) break;
        buf = buffer[last_page].p;
        buffer[last_page].reset();
        used_pages--;
      }
      last_page--;

    } else if(first_page<i){
      if(buffer[first_page].p){
        if(buf) break;
        buf = buffer[first_page].p;
        buffer[first_page].reset();
        used_pages--;
      }
      first_page++;
    }
  }

  if(!buf) buf = (uint8_t*)malloc(BufferPage::size*mRawFormat.mBlockAlign);

  bp.p = buf;
  if(i<first_page) first_page = i;
  if(i>last_page) last_page = i;
  used_pages++;
}

int VDFFAudioSource::BufferPage::copy(int s0, uint32_t count, void* dst, int sample_size)
{
  if(a0<=s0 && a1>s0){
    // copy from range a
    int n = s0+count<a1 ? count : a1-s0;
    memcpy(dst, p+s0*sample_size, n*sample_size);
    return n;
  }
  if(b0<=s0 && b1>s0){
    // copy from range b
    int n = s0+count<b1 ? count : b1-s0;
    memcpy(dst, p+s0*sample_size, n*sample_size);
    return n;
  }
  return 0;
}

int VDFFAudioSource::BufferPage::alloc(int s0, uint32_t count, int& changed)
{
  int n = s0+count<size ? count : size-s0;
  if(a0<=s0 && a1>=s0+n){
    // already in range a
    changed = 0;
    return n;
  }
  if(b0<=s0 && b1>=s0+n){
    // already in range b
    changed = 0;
    return n;
  }

  changed = 1;

  if(a1==0){
    // empty page, just initialize
    a0 = s0;
    a1 = s0+n;
    return n;
  }

  if(a0<=s0+n && a1>=s0+n){
    // extend range a down
    a0 = s0;
    return n;
  }

  if(a0<=s0 && a1>=s0){
    // extend range a up
    a1 = s0+n;
    if(b0 && a1>=b0){
      a1 = b1;
      b0 = 0;
      b1 = 0;
    }
    return n;
  }

  if(b0){
    if(b0<=s0+n && b1>=s0+n){
      // extend range b down
      b0 = s0;
      return n;
    }

    if(b0<=s0 && b1>=s0){
      // extend range b up
      b1 = s0+n;
      return n;
    }

    // going to drop range b, possibly interesting for logging
    changed = 2;
  }

  if(a1<s0){
    // insert after range a
    b0 = s0;
    b1 = s0+n;
  } else {
    // insert before range a
    b0 = a0;
    b1 = a1;
    a0 = s0;
    a1 = s0+n;
  }

  return n;
}

