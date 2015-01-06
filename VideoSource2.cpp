#include "VideoSource2.h"
#include "InputFile2.h"
#include "cineform.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

const int line_align = 16; // should be ok with any usable filter down the pipeline

uint8_t* align_buf(uint8_t* p)
{
  return (uint8_t*)(ptrdiff_t(p+line_align-1) & ~(line_align-1));
}

VDFFVideoSource::VDFFVideoSource(const VDXInputDriverContext& context)
  :mContext(context)
{
  m_pFormatCtx = 0;
  m_pStreamCtx = 0;
  m_pCodecCtx = 0;
  m_pSwsCtx = 0;
  frame = 0;
  CurrentDecoderErrorMode = kErrorModeReportAll;
  m_pixmap_data = 0;
  m_pixmap_frame = -1;
  next_frame = -1;
  frame_array = 0;
  frame_type = 0;
  flip_image = false;
  direct_buffer = false;
  buffer = 0;
  mem = 0;
}

VDFFVideoSource::~VDFFVideoSource() 
{
  if(frame) av_frame_free(&frame);
  if(m_pCodecCtx) avcodec_close(m_pCodecCtx);
  if(m_pSwsCtx) sws_freeContext(m_pSwsCtx);

  if(buffer) {for(int i=0; i<buffer_count; i++){
    BufferPage& p = buffer[i];
    if(p.map_base)
      UnmapViewOfFile(p.map_base);
    else
      free(p.p);
  }}
  if(mem) CloseHandle(mem);
  free(buffer);
  free(frame_array);
  free(frame_type);
  free(m_pixmap_data);
}

int VDFFVideoSource::AddRef() {
  return vdxunknown<IVDXStreamSource>::AddRef();
}

int VDFFVideoSource::Release() {
  return vdxunknown<IVDXStreamSource>::Release();
}

void *VDXAPIENTRY VDFFVideoSource::AsInterface(uint32_t iid)
{
  if (iid == IVDXVideoSource::kIID)
    return static_cast<IVDXVideoSource *>(this);

  if (iid == IVDXVideoDecoder::kIID)
    return static_cast<IVDXVideoDecoder *>(this);

  return vdxunknown<IVDXStreamSource>::AsInterface(iid);
}

int VDFFVideoSource::initStream( VDFFInputFile* pSource, int streamIndex )
{
  m_pSource = pSource;
  m_streamIndex = streamIndex;

  m_pFormatCtx = pSource->getContext();  
  m_pStreamCtx = m_pFormatCtx->streams[m_streamIndex];
  m_pCodecCtx = m_pStreamCtx->codec;

  AVCodec* pDecoder = avcodec_find_decoder(m_pCodecCtx->codec_id);
  if(!pDecoder){
    char buf[80];
    av_get_codec_tag_string(buf,80,m_pCodecCtx->codec_tag);
    mContext.mpCallbacks->SetError("FFMPEG: Unsupported codec (%s)", buf);
    return -1;
  }

  AVRational tb = m_pStreamCtx->time_base;
  AVRational fr = av_stream_get_r_frame_rate(m_pStreamCtx);
  av_reduce(&time_base.num, &time_base.den, fr.num*tb.num, fr.den*tb.den, INT_MAX);

  if(m_pStreamCtx->duration == AV_NOPTS_VALUE){
    if(m_pFormatCtx->duration == AV_NOPTS_VALUE){
      mContext.mpCallbacks->SetError("FFMPEG: Cannot figure stream duration. Unsupported.");
      return -1;
    } else {
      AVRational m;
      av_reduce(&m.num, &m.den, fr.num, fr.den*AV_TIME_BASE, INT_MAX);
      sample_count = (int)(m_pFormatCtx->duration * m.num/m.den);
    }
  } else {
    int rndd = time_base.den/2;
    sample_count = (int)((m_pStreamCtx->duration * time_base.num + rndd) / time_base.den);
  }

  trust_index = false;
  if(abs(m_pStreamCtx->nb_index_entries - sample_count)<2){
    // hopefully there is index with useful timestamps
    sample_count = m_pStreamCtx->nb_index_entries;
    trust_index = true;
  }
  /*
  if(m_pFormatCtx->iformat->flags & AVFMT_GENERIC_INDEX){
    // generic index only ever collects keyframes, useless
    trust_index = false;
  }*/

  keyframe_gap = 0;
  int d = 1;
  if(trust_index){for(int i=0; i<m_pStreamCtx->nb_index_entries; i++){
    if(m_pStreamCtx->index_entries[i].flags & AVINDEX_KEYFRAME){
      if(d>keyframe_gap) keyframe_gap = d;
      d = 1;
    } else d++;
  }}

  // threading has big negative impact on random access within all-keyframe files
  if(keyframe_gap==1) m_pCodecCtx->thread_count = 1;

  fw_seek_threshold = 10;
  if(keyframe_gap==1) fw_seek_threshold = 0; // assume seek is free with all-keyframe

  m_pCodecCtx->refcounted_frames = 1;

  if(avcodec_open2(m_pCodecCtx, pDecoder, 0)<0){
    mContext.mpCallbacks->SetError("FFMPEG: Decoder error.");
    return -1;
  }

  if(pDecoder->id==MKBETAG('C','F','H','D')){
    flip_image = true;
    if(trust_index) direct_buffer = true;
  }

  frame = av_frame_alloc();
  dead_range_start = -1;
  dead_range_end = -1;
  first_frame = 0;
  last_frame = 0;
  used_frames = 0;
  next_frame = 0;
  buffer_count = keyframe_gap*2;
  if(buffer_count<40) buffer_count = 40;
  if(buffer_count>sample_count) buffer_count = sample_count;
  buffer_reserve = buffer_count;

  buffer = (BufferPage*)malloc(sizeof(BufferPage)*buffer_reserve);
  memset(buffer,0,sizeof(BufferPage)*buffer_reserve);

  int fa_size = sample_count*sizeof(void*);
  frame_array = (BufferPage**)malloc(fa_size);
  memset(frame_array,0,fa_size);
  int ft_size = sample_count;
  frame_type = (char*)malloc(ft_size);
  memset(frame_type,' ',ft_size);
  frame_size = av_image_get_buffer_size(m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height, line_align);

  uint64_t max_heap = 0x20000000;
  uint64_t mem_size = uint64_t(frame_size)*buffer_reserve;
  uint64_t mem_other = 0;
  if(m_pSource->head_segment){
    VDFFInputFile* f1 = m_pSource->head_segment;
    while(1){
      if(!f1->video_source) break;
      mem_other += uint64_t(frame_size)*f1->video_source->buffer_reserve;
      f1 = f1->next_segment;
      if(!f1) break;
    }
  }

  if(mem_size+mem_other>max_heap){
    mem_size = (mem_size|0xFFFF)+1;
    mem = CreateFileMapping(INVALID_HANDLE_VALUE,0,PAGE_READWRITE,mem_size>>32,(DWORD)mem_size,0);
  }

  m_streamInfo.mSampleCount = sample_count;

  m_streamInfo.mSampleRate.mNumerator = fr.num;
  m_streamInfo.mSampleRate.mDenominator = fr.den;

  AVRational ar = av_make_q(1,1);
  if(m_pCodecCtx->sample_aspect_ratio.num) ar = m_pCodecCtx->sample_aspect_ratio;
  if(m_pStreamCtx->sample_aspect_ratio.num) ar = m_pStreamCtx->sample_aspect_ratio;
  AVRational ar1;
  av_reduce(&ar1.num, &ar1.den, ar.num, ar.den, INT_MAX);
  m_streamInfo.mPixelAspectRatio.mNumerator = ar1.num;
  m_streamInfo.mPixelAspectRatio.mDenominator = ar1.den;

  //! hack around start_time
  // looks like ffmpeg will save first packet pts as start_time
  // this is bullshit if the packet belongs to reordered frame (example: BBI frame sequence)
  read_frame(true);
  pSource->video_start_time = start_time;

  return 0;
}

void VDXAPIENTRY VDFFVideoSource::GetStreamSourceInfo(VDXStreamSourceInfo& srcInfo)
{
  srcInfo = m_streamInfo;
}

const void *VDFFVideoSource::GetDirectFormat()
{
  return NULL;
}

int VDFFVideoSource::GetDirectFormatLen() 
{
  return 0;
}

IVDXStreamSource::ErrorMode VDFFVideoSource::GetDecodeErrorMode() 
{
  return CurrentDecoderErrorMode;
  return IVDXStreamSource::kErrorModeReportAll;
}

void VDFFVideoSource::SetDecodeErrorMode(IVDXStreamSource::ErrorMode mode) 
{
  //if ( mode != ErrorMode::kErrorModeCount )
    CurrentDecoderErrorMode = mode;
}

bool VDFFVideoSource::IsDecodeErrorModeSupported(IVDXStreamSource::ErrorMode mode)
{
  return mode == kErrorModeReportAll || mode == kErrorModeDecodeAnyway;
  return mode == kErrorModeReportAll;
}

void VDFFVideoSource::GetVideoSourceInfo(VDXVideoSourceInfo& info)
{
  info.mFlags = 0;
  info.mWidth = m_pCodecCtx->width;
  info.mHeight = m_pCodecCtx->height;
  info.mDecoderModel = VDXVideoSourceInfo::kDecoderModelCustom;
}

bool VDFFVideoSource::CreateVideoDecoder(IVDXVideoDecoder **ppDecoder)
{
  this->AddRef();
  *ppDecoder = this;
  return true;
}

bool VDFFVideoSource::CreateVideoDecoderModel(IVDXVideoDecoderModel **ppModel) 
{
	this->AddRef();
	*ppModel = this;
	return true;
}

void VDFFVideoSource::GetSampleInfo(sint64 sample, VDXVideoFrameInfo& frameInfo) 
{
  if(sample>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    v1->GetSampleInfo(sample-sample_count,frameInfo);
    return;
  }

  frameInfo.mBytePosition = -1;
  frameInfo.mFrameType = kVDXVFT_Independent;
  if(keyframe_gap==1)
    frameInfo.mTypeChar = 'K';
  else
    frameInfo.mTypeChar = frame_type[sample];
}

bool VDFFVideoSource::IsKey(int64_t sample)
{
  if(sample>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    return v1->IsKey(sample-sample_count);
  }

  if(trust_index){
    return (m_pStreamCtx->index_entries[sample].flags & AVINDEX_KEYFRAME)!=0;
  }

  return true;
}

int64_t VDFFVideoSource::GetFrameNumberForSample(int64_t sample_num)
{
  return sample_num;
}

int64_t VDFFVideoSource::GetSampleNumberForFrame(int64_t display_num)
{
  return display_num;
}

int64_t VDFFVideoSource::GetRealFrame(int64_t display_num)
{
  return display_num;
}

int64_t VDFFVideoSource::GetSampleBytePosition(int64_t sample_num)
{
  return -1;
}

void VDFFVideoSource::Reset()
{
}

void VDFFVideoSource::SetDesiredFrame(int64_t frame_num)
{
  desired_frame = frame_num;
  required_count = 1;
}

int64_t VDFFVideoSource::GetNextRequiredSample(bool& is_preroll)
{
  is_preroll = false;
  return required_count ? desired_frame : -1;
}

int VDFFVideoSource::GetRequiredCount() 
{
  return required_count;
}

//////////////////////////////////////////////////////////////////////////
//Decoder
//////////////////////////////////////////////////////////////////////////\

const void* VDFFVideoSource::DecodeFrame(const void* inputBuffer, uint32_t data_len, bool is_preroll, int64_t streamFrame, int64_t targetFrame)
{
  m_pixmap_frame = int(targetFrame);

  if(targetFrame>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    if(!v1) return 0;
    return v1->DecodeFrame(inputBuffer,data_len,is_preroll,streamFrame,targetFrame-sample_count);
  }

  if(is_preroll) return 0;

  if(convertInfo.out_garbage){
    mContext.mpCallbacks->SetError("Segment has incompatible format: try changing decode format to RGBA");
    return align_buf(m_pixmap_data);
  }

  BufferPage* page = frame_array[targetFrame];
  if(!page){
    mContext.mpCallbacks->SetError("Cache overflow: set \"Performance \\ Video buffering\" to 32 or less");
    return 0;
  }
  open_read(page);
  uint8_t* src = align_buf(page->p);

  if(convertInfo.direct_copy){
    set_pixmap_layout(src);
    return src;

  } else {
    int w = m_pixmap.w;
    int h = m_pixmap.h;

    AVPicture pic = {0};
    av_image_fill_arrays(pic.data, pic.linesize, src, m_pCodecCtx->pix_fmt, w, h, line_align);

    AVPicture pic2 = {0};
    pic2.data[0] = (uint8_t*)m_pixmap.data;
    pic2.data[1] = (uint8_t*)m_pixmap.data2;
    pic2.data[2] = (uint8_t*)m_pixmap.data3;
    pic2.linesize[0] = m_pixmap.pitch;
    pic2.linesize[1] = m_pixmap.pitch2;
    pic2.linesize[2] = m_pixmap.pitch3;
    sws_scale(m_pSwsCtx, pic.data, pic.linesize, 0, h, pic2.data, pic2.linesize);
    return align_buf(m_pixmap_data);
  }
}

uint32_t VDFFVideoSource::GetDecodePadding() 
{
  return 0;
}

bool VDFFVideoSource::IsFrameBufferValid()
{
  if(m_pixmap_data) return true;
  if(m_pixmap_frame==-1) return false;

  if(m_pixmap_frame>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    if(!v1) return 0;
    return v1->IsFrameBufferValid();
  }

  if(frame_array[m_pixmap_frame]) return true;
  return false;
}

const VDXPixmap& VDFFVideoSource::GetFrameBuffer()
{
  if(m_pixmap_frame>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    return v1->GetFrameBuffer();
  }

  return m_pixmap;
}

const void* VDFFVideoSource::GetFrameBufferBase()
{
  if(m_pixmap_data) return align_buf(m_pixmap_data);
  if(m_pixmap_frame==-1) return 0;

  if(m_pixmap_frame>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    if(!v1) return 0;
    return v1->GetFrameBufferBase();
  }

  return align_buf(frame_array[m_pixmap_frame]->p);
}

bool VDFFVideoSource::SetTargetFormat(int format, bool useDIBAlignment)
{
  nsVDXPixmap::VDXPixmapFormat opt_format = (nsVDXPixmap::VDXPixmapFormat)format;
  bool r = SetTargetFormat(opt_format,useDIBAlignment,0);
  VDFFInputFile* f1 = m_pSource;
  while(1){
    f1 = f1->next_segment;
    if(!f1) break;
    if(f1->video_source) f1->video_source->SetTargetFormat(opt_format,useDIBAlignment,this);
  }
  return r;
}

bool VDFFVideoSource::SetTargetFormat(nsVDXPixmap::VDXPixmapFormat opt_format, bool useDIBAlignment,VDFFVideoSource* head)
{
  // this function will select one of the modes:
  // 1) XRGB - works slow
  // 2) convert to arbitrary rgb
  // 3) direct decoded format - usually some sort of yuv
  // 4) upsample arbitrary yuv to 444

  // yuv->rgb has chance to apply correct source color space matrix, range etc 
  // but! currently I notice huge color upsampling error

  // which is best default? rgb afraid to use; sws can do either fast-bad or slow-good, but vd can do good-fast-enough
  bool default_rgb = false;
  bool fast_rgb = false;

  using namespace nsVDXPixmap;
  VDXPixmapFormat base_format = kPixFormat_Null;
  VDXPixmapFormat trigger = kPixFormat_Null;
  VDXPixmapFormat perfect_format = kPixFormat_Null;
  AVPixelFormat perfect_av_fmt = m_pCodecCtx->pix_fmt;
  AVPixelFormat src_fmt = m_pCodecCtx->pix_fmt;
  bool perfect_bitexact = false;
  bool skip_colorspace = false;

  convertInfo.req_format = opt_format;
  convertInfo.req_dib = useDIBAlignment;
  convertInfo.direct_copy = false;
  convertInfo.in_yuv = false; //isYUV(m_pCodecCtx->pix_fmt); why is it internal
  convertInfo.in_subs = false;
  convertInfo.out_rgb = false;
  convertInfo.out_garbage = false;

  switch(m_pCodecCtx->pix_fmt){
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    src_fmt = AV_PIX_FMT_YUV420P;
    perfect_format = kPixFormat_YUV420_Planar;
    trigger = kPixFormat_YUV420_Planar;
    convertInfo.in_yuv = true;
    convertInfo.in_subs = true;
    perfect_bitexact = true;
    // examples: xvid
    // examples: gopro avc (FR)
    break;

  case AV_PIX_FMT_YUV422P:
  case AV_PIX_FMT_YUVJ422P:
    src_fmt = AV_PIX_FMT_YUV422P;
    perfect_format = kPixFormat_YUV422_Planar;
    trigger = kPixFormat_YUV422_Planar;
    convertInfo.in_yuv = true;
    convertInfo.in_subs = true;
    perfect_bitexact = true;
    // examples: jpeg (FR)
    break;

  case AV_PIX_FMT_YUV440P:
  case AV_PIX_FMT_YUVJ440P:
    src_fmt = AV_PIX_FMT_YUV440P;
    perfect_format = kPixFormat_YUV444_Planar;
    perfect_av_fmt = AV_PIX_FMT_YUV444P;
    trigger = kPixFormat_YUV444_Planar;
    convertInfo.in_yuv = true;
    convertInfo.in_subs = true;
    perfect_bitexact = false;
    // examples: 422 jpeg lossless-transposed (FR)
    break;

  case AV_PIX_FMT_UYVY422:
    perfect_format = kPixFormat_YUV422_UYVY;
    trigger = kPixFormat_YUV422_UYVY;
    convertInfo.in_yuv = true;
    convertInfo.in_subs = true;
    perfect_bitexact = true;
    //! not tested at all
    break;

  case AV_PIX_FMT_YUYV422:
    perfect_format = kPixFormat_YUV422_YUYV;
    trigger = kPixFormat_YUV422_YUYV;
    convertInfo.in_yuv = true;
    convertInfo.in_subs = true;
    perfect_bitexact = true;
    //! not tested at all
    break;

  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    src_fmt = AV_PIX_FMT_YUV444P;
    perfect_format = kPixFormat_YUV444_Planar;
    perfect_av_fmt = AV_PIX_FMT_YUV444P;
    trigger = kPixFormat_YUV444_Planar;
    convertInfo.in_yuv = true;
    perfect_bitexact = true;
    // examples: 444 jpeg by photoshop (FR)
    break;

  case AV_PIX_FMT_BGR24:
    perfect_format = kPixFormat_RGB888;
    trigger = kPixFormat_RGB888;
    perfect_bitexact = true;
    // examples: tga24
    break;

  case AV_PIX_FMT_BGRA:
    perfect_format = kPixFormat_XRGB8888;
    trigger = kPixFormat_XRGB8888;
    perfect_bitexact = true;
    // examples: tga32
    break;
  }

  if(opt_format==0){
    if(default_rgb){
      base_format = kPixFormat_XRGB8888;
      convertInfo.av_fmt = AV_PIX_FMT_BGRA;
      if(base_format==perfect_format) convertInfo.direct_copy = perfect_bitexact;
    } else {
      base_format = perfect_format;
      convertInfo.av_fmt = perfect_av_fmt;
      convertInfo.direct_copy = perfect_bitexact;
    }

    if(!convertInfo.in_yuv && (useDIBAlignment^flip_image)) convertInfo.direct_copy = false;

  } else {
    switch(opt_format){
    case kPixFormat_YUV420_Planar:
    case kPixFormat_YUV422_Planar:
    case kPixFormat_YUV422_UYVY:
    case kPixFormat_YUV422_YUYV:
      if(opt_format==trigger){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
        skip_colorspace = true;
      } else return false;
      break;

    case kPixFormat_YUV444_Planar:
      if(opt_format==trigger){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
        skip_colorspace = true;

      } else if(convertInfo.in_yuv){
        base_format = kPixFormat_YUV444_Planar;
        convertInfo.av_fmt = AV_PIX_FMT_YUV444P;
        skip_colorspace = true;
      } else return false;
      break;

    case kPixFormat_XRGB1555:
      if(opt_format==perfect_format){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
      } else {
        base_format = kPixFormat_XRGB1555;
        convertInfo.av_fmt = AV_PIX_FMT_RGB555;
        convertInfo.direct_copy = false;
      }
      break;

    case kPixFormat_RGB565:
      if(opt_format==perfect_format){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
      } else {
        base_format = kPixFormat_RGB565;
        convertInfo.av_fmt = AV_PIX_FMT_RGB565;
        convertInfo.direct_copy = false;
      }
      break;

    case kPixFormat_RGB888:
      if(opt_format==perfect_format){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
      } else {
        base_format = kPixFormat_RGB888;
        convertInfo.av_fmt = AV_PIX_FMT_BGR24;
        convertInfo.direct_copy = false;
      }
      break;

    case kPixFormat_XRGB8888:
      if(opt_format==perfect_format){
        base_format = perfect_format;
        convertInfo.av_fmt = perfect_av_fmt;
        convertInfo.direct_copy = perfect_bitexact;
      } else {
        base_format = kPixFormat_XRGB8888;
        convertInfo.av_fmt = AV_PIX_FMT_BGRA;
        convertInfo.direct_copy = false;
      }
      break;

    default:
      return false;
    }
  }

  //convertInfo.out_rgb = isRGB(convertInfo.av_fmt); why is it internal
  switch(convertInfo.av_fmt){
  case AV_PIX_FMT_BGRA:
  case AV_PIX_FMT_BGR24:
  case AV_PIX_FMT_RGB565:
  case AV_PIX_FMT_RGB555:
    convertInfo.out_rgb = true;
  }

  // tweak output yuv formats for VD here
  VDXPixmapFormat format = base_format;

  if(!skip_colorspace && m_pCodecCtx->color_range==AVCOL_RANGE_JPEG) switch(format){
  case kPixFormat_YUV420_Planar:
    format = kPixFormat_YUV420_Planar_FR;
    break;
  case kPixFormat_YUV422_Planar:
    format = kPixFormat_YUV422_Planar_FR;
    break;
  case kPixFormat_YUV444_Planar:
    format = kPixFormat_YUV444_Planar_FR;
    break;
  case kPixFormat_YUV422_UYVY:
    format = kPixFormat_YUV422_UYVY_FR;
    break;
  case kPixFormat_YUV422_YUYV:
    format = kPixFormat_YUV422_YUYV_FR;
    break;
  }

  if(!skip_colorspace && m_pCodecCtx->colorspace==AVCOL_SPC_BT709) switch(format){
  case kPixFormat_YUV420_Planar:
    format = kPixFormat_YUV420_Planar_709;
    break;
  case kPixFormat_YUV420_Planar_FR:
    format = kPixFormat_YUV420_Planar_709_FR;
    break;
  case kPixFormat_YUV422_Planar:
    format = kPixFormat_YUV422_Planar_709;
    break;
  case kPixFormat_YUV422_Planar_FR:
    format = kPixFormat_YUV422_Planar_709_FR;
    break;
  case kPixFormat_YUV444_Planar:
    format = kPixFormat_YUV444_Planar_709;
    break;
  case kPixFormat_YUV444_Planar_FR:
    format = kPixFormat_YUV444_Planar_709_FR;
    break;
  case kPixFormat_YUV422_UYVY:
    format = kPixFormat_YUV422_UYVY_709;
    break;
  case kPixFormat_YUV422_UYVY_FR:
    format = kPixFormat_YUV422_UYVY_709_FR;
    break;
  case kPixFormat_YUV422_YUYV:
    format = kPixFormat_YUV422_YUYV_709;
    break;
  case kPixFormat_YUV422_YUYV_FR:
    format = kPixFormat_YUV422_YUYV_709_FR;
    break;
  }

  if(head && head->m_pixmap.format!=format){
    format = (VDXPixmapFormat)head->m_pixmap.format;
    convertInfo.av_fmt = head->convertInfo.av_fmt;
    convertInfo.direct_copy = true;
    convertInfo.out_garbage = true;
  }

  memset(&m_pixmap,0,sizeof(m_pixmap));
  m_pixmap.format = format;
  int32_t w = m_pCodecCtx->width;
  int32_t h = m_pCodecCtx->height;
  m_pixmap.w = w;
  m_pixmap.h = h;
  if(convertInfo.direct_copy){
    // now pixmap_data should be useless
    // but VD thinks its a good idea to call GetFrameBuffer immediately and use it as destination for something in "fast recompress"
    
    //this hackery helps to run analysis pass in fast recompress, but it is impossible to write a file anyway due to some VD bugs
    uint32_t size = av_image_get_buffer_size(convertInfo.av_fmt,w,h,line_align);
    m_pixmap_data = (uint8_t*)realloc(m_pixmap_data, size+line_align-1);
    set_pixmap_layout(align_buf(m_pixmap_data));
    if(convertInfo.out_garbage) memset(m_pixmap_data,0,size+line_align-1);
    
    //free(m_pixmap_data);
    //m_pixmap_data = 0;

  } else {
    uint32_t size = av_image_get_buffer_size(convertInfo.av_fmt,w,h,line_align);
    m_pixmap_data = (uint8_t*)realloc(m_pixmap_data, size+line_align-1);
    set_pixmap_layout(align_buf(m_pixmap_data));
    if(m_pSwsCtx) sws_freeContext(m_pSwsCtx);
    int flags = 0;
    if(convertInfo.in_subs){
      // bicubic is needed to best preserve detail, ex color_420.jpg
      // bilinear also not bad if the material is blurry
      flags |= SWS_BICUBIC;
    } else {
      // this is needed to fight smearing when no upsampling is actually used, ex color_444.jpg
      flags |= SWS_POINT;
    }

    // these flags are needed to avoid really bad optimized conversion resulting in corrupt image, ex color_420.jpg
    // however this is also 5x slower!
    flags |= SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND;
    if(fast_rgb) flags = SWS_POINT;
    #ifdef FFDEBUG
    flags |= SWS_PRINT_INFO;
    #endif
    m_pSwsCtx = sws_getContext(w, h, src_fmt, w, h, convertInfo.av_fmt, flags, 0, 0, 0);
    if(convertInfo.in_yuv && convertInfo.out_rgb){
      // range and color space only makes sence for yuv->rgb
      // rgb->rgb is always exact
      // rgb->yuv is useless as input
      // yuv->yuv better keep unchanged to save precision
      // rgb output is always full range
      const int* src_matrix = sws_getCoefficients(m_pCodecCtx->colorspace);
      int src_range = m_pCodecCtx->color_range==AVCOL_RANGE_JPEG ? 1:0;

      int* t1; int* t2; int r1,r2; int p0,p1,p2;
      sws_getColorspaceDetails(m_pSwsCtx,&t1,&r1,&t2,&r2,&p0,&p1,&p2);
      sws_setColorspaceDetails(m_pSwsCtx, src_matrix, src_range, t2, r2, p0,p1,p2);
    }
  }

  return true;
}

void VDFFVideoSource::set_pixmap_layout(uint8_t* p)
{
  using namespace nsVDXPixmap;

  int w = m_pixmap.w;
  int h = m_pixmap.h;

  AVPicture pic = {0};
  av_image_fill_arrays(pic.data, pic.linesize, p, convertInfo.av_fmt, w, h, line_align);

  m_pixmap.palette = 0;
  m_pixmap.data = pic.data[0];
  m_pixmap.data2 = pic.data[1];
  m_pixmap.data3 = pic.data[2];
  m_pixmap.pitch = pic.linesize[0];
  m_pixmap.pitch2 = pic.linesize[1];
  m_pixmap.pitch3 = pic.linesize[2];

  if(convertInfo.req_dib){
    switch(m_pixmap.format){
    case kPixFormat_XRGB1555:
    case kPixFormat_RGB565:
    case kPixFormat_RGB888:
    case kPixFormat_XRGB8888:
      m_pixmap.data = pic.data[0] + pic.linesize[0]*(h-1);
      m_pixmap.pitch = -pic.linesize[0];
      break;
    }
  }
}

bool VDFFVideoSource::SetDecompressedFormat(const VDXBITMAPINFOHEADER *pbih) 
{
  return false;
}

bool VDFFVideoSource::IsDecodable(int64_t sample_num64) 
{
  return true;
}

bool VDFFVideoSource::Read(sint64 start, uint32 lCount, void *lpBuffer, uint32 cbBuffer, uint32 *lBytesRead, uint32 *lSamplesRead)
{
  if(start>=sample_count){
    VDFFVideoSource* v1 = 0;
    if(m_pSource->next_segment) v1 = m_pSource->next_segment->video_source;
    if(v1) return v1->Read(start-sample_count,lCount,lpBuffer,cbBuffer,lBytesRead,lSamplesRead);
  }

  if(start==sample_count){
    *lBytesRead = 0;
    *lSamplesRead = 0;
    return true;
  }

  if(!lpBuffer){
    *lBytesRead = 0;
    *lSamplesRead = 1;
    return false;
  }

  *lBytesRead = 0;
  *lSamplesRead = 1;

  VDFFVideoSource* head = this;
  if(m_pSource->head_segment) head = m_pSource->head_segment->video_source;
  if(head->required_count) head->required_count--;

  int jump = (int)start;

  if(frame_array[start]){
    // prefetch backward seek
    int x = -1;
    {for(int i=(int)start; i>=0; i--){
      if(i<start-keyframe_gap) break;
      if(!frame_array[i]){ x=i; break; }
    }}

    if(x!=-1){
      jump = x;
    } else return true;
  }

  if(trust_index && jump>next_frame){
    int next_key = -1;
    {for(int i=jump; i>next_frame; i--)
      if(m_pStreamCtx->index_entries[i].flags & AVINDEX_KEYFRAME){ next_key=i; break; } }

    if(next_key!=-1 && next_key>next_frame+fw_seek_threshold){
      // required to seek forward
      int64_t pos = m_pStreamCtx->index_entries[next_key].timestamp;
      avcodec_flush_buffers(m_pCodecCtx);
      av_seek_frame(m_pFormatCtx,m_streamIndex,pos,0);
      next_frame = next_key;
    }
  }

  if(trust_index && jump<next_frame){
    // required to seek backward
    int prev_key = 0;
    {for(int i=jump; i>=0; i--)
      if(m_pStreamCtx->index_entries[i].flags & AVINDEX_KEYFRAME){ prev_key=i; break; } }
    int64_t pos = m_pStreamCtx->index_entries[prev_key].timestamp;
    avcodec_flush_buffers(m_pCodecCtx);
    av_seek_frame(m_pFormatCtx,m_streamIndex,pos,0);
    next_frame = prev_key;
  }

  if(!trust_index && jump>next_frame+fw_seek_threshold || jump<next_frame){
    // required to seek
    if(jump>=dead_range_start && jump<=dead_range_end){
      // we already known this does not work so fail fast
      mContext.mpCallbacks->SetError("requested frame not found; next valid frame = %d", next_frame-1);
      return false;
    }
    int64_t pos = jump*time_base.den / time_base.num + start_time;
    if(jump==0 && pos>0) pos = 0;
    avcodec_flush_buffers(m_pCodecCtx);
    av_seek_frame(m_pFormatCtx,m_streamIndex,pos,AVSEEK_FLAG_BACKWARD);
    next_frame = -1;
  }

  while(1){
    if(!read_frame()){
      if(next_frame>0){
        // end of stream, fill with dups
        BufferPage* page = frame_array[next_frame-1];
        if(page){
          copy(next_frame, int(start), page);
          next_frame = int(start)+1;
        }
      } else {
        mContext.mpCallbacks->SetError("unexpected end of stream");
        return false;
      }
    }
    if(frame_array[start]) return true;

    //! missed seek or bad stream, just fail
    // better idea is to build new corrected index maybe
    if(next_frame>start){
      dead_range_start = start;
      dead_range_end = next_frame-2;
      mContext.mpCallbacks->SetError("requested frame not found; next valid frame = %d", next_frame-1);
      return false;
    }
  }

  return false;
}

bool VDFFVideoSource::read_frame(bool init)
{
  AVPacket pkt;
  pkt.data = 0;
  pkt.size = 0;

  while(1){
    int rf = av_read_frame(m_pFormatCtx, &pkt);
    if(rf<0){
      // end of stream, grab buffered images
      pkt.stream_index = m_streamIndex;
      while(1){
        int got_frame = 0;
        if(direct_buffer) alloc_direct_buffer();
        avcodec_decode_video2(m_pCodecCtx, frame, &got_frame, &pkt);
        if(!got_frame) return false;
        if(init){
          init = false;
          set_start_time();
        }
        handle_frame();
        av_frame_unref(frame);
        return true;
      }

    } else {
      AVPacket orig_pkt = pkt;
      int done_frames = 0;
      do {
        int s = pkt.size;
        if(pkt.stream_index == m_streamIndex){
          int got_frame = 0;
          if(direct_buffer) alloc_direct_buffer();
          s = avcodec_decode_video2(m_pCodecCtx, frame, &got_frame, &pkt);
          if(s<0) break;
          if(got_frame){
            if(init){
              init = false;
              set_start_time();
            }
            handle_frame();
            av_frame_unref(frame);
            done_frames++;
          }
        }

        pkt.data += s;
        pkt.size -= s;
      } while (pkt.size > 0);
      av_free_packet(&orig_pkt);
      if(done_frames>0) return true;
    }
  }
}

void VDFFVideoSource::set_start_time()
{
  int64_t ts = frame->pkt_pts;
  if(ts==AV_NOPTS_VALUE) ts = frame->pkt_dts;
  start_time = ts;
}

void VDFFVideoSource::handle_frame()
{
  dead_range_start = -1;
  dead_range_end = -1;
  int64_t ts = frame->pkt_pts;
  if(ts==AV_NOPTS_VALUE) ts = frame->pkt_dts;
  int pos = next_frame;

  if(!trust_index){
    // guess where we are
    // timestamp to frame number is at times unreliable
    if(ts!=AV_NOPTS_VALUE){
      ts -= start_time;
      int rndd = time_base.den/2;
      pos = int((ts*time_base.num + rndd) / time_base.den);
    } else {
      if(next_frame==-1) return;
      pos = next_frame;
    }
  }

  if(next_frame!=-1 && pos>next_frame){
    // gap between frames, fill with dups
    // caused by non-constant framerate etc
    BufferPage* page = frame_array[next_frame-1];
    if(page) copy(next_frame, pos-1, page);
  }

  // ignore anything outside promised range
  if(pos<0 || pos>=sample_count){
    return;
  }

  if(!frame_array[pos]){
    alloc_page(pos);
    frame_type[pos] = av_get_picture_type_char(frame->pict_type);
    BufferPage* page = frame_array[pos];
    open_write(page);

    uint8_t* dst = align_buf(page->p);
    av_image_copy_to_buffer(dst, frame_size, frame->data, frame->linesize, (AVPixelFormat)frame->format, frame->width, frame->height, line_align);
  } else if(direct_buffer){
    frame_type[pos] = av_get_picture_type_char(frame->pict_type);
  }

  next_frame = pos+1;
}

void VDFFVideoSource::alloc_direct_buffer()
{
  int pos = next_frame;
  if(!frame_array[pos]) alloc_page(pos);
  BufferPage* page = frame_array[pos];
  open_write(page);
  cfhd_set_buffer(m_pCodecCtx,align_buf(page->p));
}

void VDFFVideoSource::alloc_page(int pos)
{
  BufferPage* r = 0;

  if(used_frames>=buffer_count) while(1){
    if(last_frame>pos){
      if(frame_array[last_frame]){
        if(r) break;
        BufferPage* p1 = frame_array[last_frame];
        frame_array[last_frame] = 0;
        p1->refs--;
        if(!p1->refs){
          r = p1;
          used_frames--;
        }
      }
      last_frame--;

    } else if(first_frame<pos){
      if(frame_array[first_frame]){
        if(r) break;
        BufferPage* p1 = frame_array[first_frame];
        frame_array[first_frame] = 0;
        p1->refs--;
        if(!p1->refs){
          r = p1;
          used_frames--;
        }
      }
      first_frame++;
    }
  }

  if(!r){for(int i=0; i<buffer_count; i++){
    if(!buffer[i].refs){
      r = &buffer[i];
      r->i = i;
      if(!mem && !r->p) r->p = (uint8_t*)malloc(frame_size+line_align-1);
      break;
    }
  }}

  r->target = pos;
  r->refs++;
  used_frames++;
  frame_array[pos] = r;
  if(pos>last_frame) last_frame = pos;
  if(pos<first_frame) first_frame = pos;
}

void VDFFVideoSource::open_page(BufferPage* p, int flag)
{
  if(!mem) return;
  if(p->map_base && (p->access & flag)) return;

  {for(int i=0; i<buffer_count; i++){
    BufferPage& p1 = buffer[i];
    if(&p1==p) continue;
    if(!p1.map_base) continue;
    if(p1.access & flag){
      while(1){
        int access0 = p1.access & 3;
        if(InterlockedCompareExchange(&p1.access,4,access0)==access0){
          int access2 = access0 & ~flag;
          if(!access2){
            UnmapViewOfFile(p1.map_base);
            p1.map_base = 0;
            p1.p = 0;
          }
          InterlockedExchange(&p1.access,access2);
          break;
        }
      }
      break;
    }
  }}

  while(1){
    int access0 = p->access & 3;
    if(InterlockedCompareExchange(&p->access,4,access0)==access0){
      if(!p->map_base){
        uint64_t pos = uint64_t(frame_size)*p->i;
        uint64_t pos0 = pos & ~0xFFFF;
        uint64_t pos1 = ((pos+frame_size) | 0xFFFF)+1;

        p->map_base = MapViewOfFile(mem, FILE_MAP_WRITE, pos0>>32, (DWORD)pos0, (SIZE_T)(pos1-pos0));
        if(p->map_base){
          p->p = (uint8_t*)(ptrdiff_t(p->map_base) + pos-pos0);
        } else {
          mContext.mpCallbacks->SetErrorOutOfMemory();
        }
      }
      InterlockedExchange(&p->access,access0|flag);
      break;
    }
  }
}

void VDFFVideoSource::copy(int start, int end, BufferPage* p)
{
  {for(int i=start; i<=end; i++){
    if(frame_array[i]) continue;
    p->refs++;
    frame_array[i] = p;
    frame_type[i] = '+';
  }}
}
