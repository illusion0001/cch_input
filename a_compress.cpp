#include "a_compress.h"
#include <string>
#include <Ks.h>
#include <KsMedia.h>

void init_av();

struct IOWBuffer{
  uint8_t* data;
  int64_t size;
  int64_t pos;

  IOWBuffer(){
    data=0; size=0; pos=0; 
  }

  ~IOWBuffer(){
    free(data);
  }

  static int Write(void* obj, uint8_t* buf, int buf_size){
    IOWBuffer* t = (IOWBuffer*)obj;
    int64_t pos = t->pos;
    if(t->size<pos+buf_size){
      t->data = (uint8_t*)realloc(t->data,size_t(pos+buf_size));
      t->size = pos+buf_size;
    }
    memcpy(t->data+pos,buf,buf_size);
    return buf_size;
  }

  static int64_t Seek(void* obj, int64_t offset, int whence){
    IOWBuffer* t = (IOWBuffer*)obj;
    if(whence==AVSEEK_SIZE) return t->size;
    if(whence==SEEK_CUR){ t->pos+=offset; return t->pos; }
    if(whence==SEEK_SET){ t->pos=offset; return t->pos; }
    if(whence==SEEK_END){ t->pos=t->size+offset; return t->pos; }
    return -1;
  }
};

VDFFAudio::VDFFAudio(const VDXInputDriverContext &pContext)
  :mContext(pContext)
{
  codec = 0;
  ctx = 0;
  frame = 0;
  swr = 0;
  sample_buf = 0;
  frame_size = 0;
  frame_pos = 0;
  in_buf = 0;
  in_pos = 0;
  out_format = 0;
  out_format_size = 0;
  pkt.data = 0;
  pkt.size = 0;
  av_init_packet(&pkt);
  total_in = 0;
  total_out = 0;
}

VDFFAudio::~VDFFAudio()
{
  if(ctx){
    avcodec_close(ctx);
    av_free(ctx);
    ctx = 0;
  }
  if(frame){
    av_freep(&frame->data[0]);
    av_frame_free(&frame);
    frame = 0;
  }
  if(swr) swr_free(&swr);
  if(sample_buf){
    av_freep(&sample_buf[0]);
    free(sample_buf);
    free(in_buf);
  }
  free(out_format);
}

void VDFFAudio::SetConfig(void* data, size_t size){
  if(size!=sizeof(config)){
    config.clear();
    return;
  }
  Config* a = (Config*)data;
  if(a->version!=config.version){
    config.clear();
    return;
  }
  memcpy(&config,a,size);
}

void VDFFAudio::SetInputFormat(VDXWAVEFORMATEX* format){
  init_av();
  CreateCodec();

  ctx = avcodec_alloc_context3(codec);

  ctx->channels       = format->mChannels;
  ctx->channel_layout = av_get_default_channel_layout(format->mChannels);
  ctx->sample_rate    = format->mSamplesPerSec;
  ctx->sample_fmt     = codec->sample_fmts[0];
  ctx->bit_rate       = 128*1024;

  AVSampleFormat in_fmt = AV_SAMPLE_FMT_S16;
  if(format->mBitsPerSample==8) in_fmt = AV_SAMPLE_FMT_U8;
  if(format->mFormatTag==WAVE_FORMAT_IEEE_FLOAT) in_fmt = AV_SAMPLE_FMT_FLT;
  if(format->mFormatTag==WAVE_FORMAT_EXTENSIBLE){
    WAVEFORMATEXTENSIBLE* fx = (WAVEFORMATEXTENSIBLE*)(format);
    if(fx->dwChannelMask && av_get_channel_layout_nb_channels(fx->dwChannelMask)==format->mChannels)
      ctx->channel_layout = fx->dwChannelMask;
    if(fx->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) in_fmt = AV_SAMPLE_FMT_FLT;
  }

  //if(ofmt->oformat->flags & AVFMT_GLOBALHEADER)
  //  ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if(avcodec_open2(ctx, codec, NULL)<0){ 
    mContext.mpCallbacks->SetError("FFMPEG: Cannot open codec.");
    return;
  }

  IOWBuffer io;
  int buf_size = 4096;
  void* buf = av_malloc(buf_size);
  AVIOContext* avio_ctx = avio_alloc_context((unsigned char*)buf,buf_size,1,&io,0,&IOWBuffer::Write,&IOWBuffer::Seek);
  AVFormatContext* ofmt = avformat_alloc_context();
  ofmt->pb = avio_ctx;
  ofmt->oformat = av_guess_format("wav", 0, 0);
  AVStream* st = avformat_new_stream(ofmt, 0);
  st->time_base.den = ctx->sample_rate;
  st->time_base.num = 1;
  avcodec_parameters_from_context(st->codecpar, ctx);
  if(avformat_write_header(ofmt, 0)<0){
    mContext.mpCallbacks->SetError("FFMPEG: Audio format is not compatible.");
    return;
  }
  av_write_trailer(ofmt);

  out_format_size = *(int*)(io.data+16);
  WAVEFORMATEXTENSIBLE* ff = (WAVEFORMATEXTENSIBLE*)(io.data+20);
  out_format = (WAVEFORMATEXTENSIBLE*)malloc(out_format_size);
  memcpy(out_format,ff,out_format_size);

  av_free(avio_ctx->buffer);
  av_free(avio_ctx);
  avformat_free_context(ofmt);

  if(swr) swr_free(&swr);
  swr = swr_alloc();
  av_opt_set_int(swr, "in_channel_layout",     ctx->channel_layout, 0);
  av_opt_set_int(swr, "in_sample_rate",        ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr, "in_sample_fmt",  in_fmt, 0);

  av_opt_set_int(swr, "out_channel_layout",    ctx->channel_layout, 0);
  av_opt_set_int(swr, "out_sample_rate",       ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr, "out_sample_fmt", ctx->sample_fmt, 0);
  int rr = swr_init(swr);
  if(rr<0) mContext.mpCallbacks->SetError("FFMPEG: Audio resampler error.");

  frame = av_frame_alloc();
  frame->channel_layout = ctx->channel_layout;
  frame->format         = ctx->sample_fmt;
  frame->sample_rate    = ctx->sample_rate;

  frame_size = ctx->frame_size;
  sample_buf = (uint8**)malloc(ctx->channels*sizeof(void*));
  av_samples_alloc(sample_buf, 0, ctx->channels, frame_size, ctx->sample_fmt, 0);

  av_samples_get_buffer_size(&src_linesize,ctx->channels,1,in_fmt,1);
  in_buf = (uint8*)malloc(src_linesize*frame_size);
}

bool VDFFAudio::Convert(bool flush, bool requireOutput){
  if(pkt.size) return true;

  if(in_pos>=frame_size || (in_pos>0 && flush)){
    const uint8_t* src[] = {in_buf};
    int done_swr = swr_convert(swr,sample_buf,frame_size,src,in_pos);

    frame->nb_samples = in_pos;
    av_samples_fill_arrays(frame->data,frame->linesize,sample_buf[0],ctx->channels,frame_size,ctx->sample_fmt,0);
    avcodec_send_frame(ctx,frame);

    in_pos = 0;

  } else if(flush){
    avcodec_send_frame(ctx,0);
  }

  av_packet_unref(&pkt);
  pkt.data = 0;
  pkt.size = 0;
  av_init_packet(&pkt);
  avcodec_receive_packet(ctx,&pkt);
  if(pkt.size) return true;

  return false;
}

unsigned VDFFAudio::GetInputLevel() const {
  return in_pos*src_linesize;
}

unsigned VDFFAudio::GetInputSpace() const { 
  return (frame_size-in_pos)*src_linesize;
}

unsigned VDFFAudio::GetOutputLevel() const {
  return pkt.size;
}

void* VDFFAudio::LockInputBuffer(unsigned& bytes){
  bytes = (frame_size-in_pos)*src_linesize;
  return in_buf + in_pos*src_linesize;
}

void VDFFAudio::UnlockInputBuffer(unsigned bytes){
  in_pos += bytes/src_linesize;
  total_in += bytes/src_linesize;
}

const void* VDFFAudio::LockOutputBuffer(unsigned& bytes){ 
  return 0; 
}

void VDFFAudio::UnlockOutputBuffer(unsigned bytes){
}

unsigned VDFFAudio::CopyOutput(void *dst, unsigned bytes, sint64& duration){
  if(!pkt.size){
    duration = -1;
    return 0;
  }

  total_out += pkt.duration;
  duration = pkt.duration;
  if(bytes>unsigned(pkt.size)) bytes = pkt.size;
  memcpy(dst,pkt.data,bytes);
  av_packet_unref(&pkt);
  pkt.data = 0;
  pkt.size = 0;
  av_init_packet(&pkt);
  return bytes;
}

//-----------------------------------------------------------------------------------

void VDFFAudio_aac::CreateCodec()
{
  codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
}

void VDFFAudio_mp3::CreateCodec()
{
  codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
}

bool VDXAPIENTRY ff_create_aacenc(const VDXInputDriverContext *pContext, IVDXAudioEnc **ppDriver)
{
  VDFFAudio *p = new VDFFAudio_aac(*pContext);
  if(!p) return false;
  *ppDriver = p;
  p->AddRef();
  return true;
}

VDXAudioEncDefinition ff_aacenc={
  sizeof(VDXAudioEncDefinition),
  0, //flags
  L"FFMpeg AAC",
  "ffmpeg_aac",
  ff_create_aacenc
};

VDXPluginInfo ff_aacenc_info={
  sizeof(VDXPluginInfo),
  L"FFMpeg AAC",
  L"Anton Shekhovtsov",
  L"Encode audio to AAC format.",
  1,
  kVDXPluginType_AudioEnc,
  0,
  12, // min api version
  kVDXPlugin_APIVersion,
  kVDXPlugin_AudioEncAPIVersion,  // min output api version
  kVDXPlugin_AudioEncAPIVersion,
  &ff_aacenc
};

//-----------------------------------------------------------------------------------

bool VDXAPIENTRY ff_create_mp3enc(const VDXInputDriverContext *pContext, IVDXAudioEnc **ppDriver)
{
  VDFFAudio *p = new VDFFAudio_mp3(*pContext);
  if(!p) return false;
  *ppDriver = p;
  p->AddRef();
  return true;
}

VDXAudioEncDefinition ff_mp3enc={
  sizeof(VDXAudioEncDefinition),
  0, //flags
  L"FFMpeg Lame MP3",
  "ffmpeg_mp3",
  ff_create_mp3enc
};

VDXPluginInfo ff_mp3enc_info={
  sizeof(VDXPluginInfo),
  L"FFMpeg Lame MP3",
  L"Anton Shekhovtsov",
  L"Encode audio to MP3 format.",
  1,
  kVDXPluginType_AudioEnc,
  0,
  12, // min api version
  kVDXPlugin_APIVersion,
  kVDXPlugin_AudioEncAPIVersion,  // min output api version
  kVDXPlugin_AudioEncAPIVersion,
  &ff_mp3enc
};
