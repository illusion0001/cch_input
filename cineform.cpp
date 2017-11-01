#include "cineform.h"
#include <shlobj.h>
#include <stdint.h>
#include <vfw.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#pragma comment(lib,"vfw32.lib")

// this enables cfhd to operate within ffmpeg
// native version is somewhat faster

typedef struct AVCodecTag {
  enum AVCodecID id;
  unsigned int tag;
} AVCodecTag;

//----------------------------------------------------------------------------------------------

#ifdef USE_CINEFORM_API

#include "cineform/include/CFHDDecoder.h"
#ifdef _WIN64
#pragma comment(lib,"CFHDDecoder64.lib")
#else
#pragma comment(lib,"CFHDDecoderVS2008.lib")
#endif

struct DecoderObj{
  void* buf;
  HINSTANCE dll;
  CFHD_DecoderRef dec;
  int samples;
};

av_cold int cfhd_init_decoder(AVCodecContext* avctx)
{
  DecoderObj* obj = (DecoderObj*)avctx->priv_data;
  obj->samples = 0;
  obj->dec = 0;
  obj->dll = 0;

  wchar_t buf[MAX_PATH+100];
  SHGetSpecialFolderPathW(0,buf,CSIDL_PROGRAM_FILES,false);
  wcscat(buf,L"\\CineForm\\Tools\\CFHDDecoder.dll");
  obj->dll = LoadLibraryW(buf);
  if(!obj->dll) return AVERROR(ENOMEM);

  CFHD_OpenDecoder(&obj->dec,0);
  //avctx->pix_fmt = AV_PIX_FMT_BGRA;
  avctx->pix_fmt = AV_PIX_FMT_BGR24;

  return 0;
}

av_cold int cfhd_close_decoder(AVCodecContext* avctx)
{
  DecoderObj* obj = (DecoderObj*)avctx->priv_data;
  if(obj->dec) CFHD_CloseDecoder(obj->dec);
  if(obj->dll) FreeLibrary(obj->dll);
  return 0;
}

int cfhd_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* avpkt)
{
  DecoderObj* obj = (DecoderObj*)avctx->priv_data;
  if(obj->samples==0){
    int w=0;
    int h=0;
    //CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_BGRA;
    CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_RG24;
    CFHD_PrepareToDecode(obj->dec,0,0,fmt,CFHD_DECODED_RESOLUTION_FULL,0,avpkt->data,avpkt->size,&w,&h,&fmt);
	  unsigned char license[] = {
      0xAD,0x7A,0xFD,0x02,0xD9,0x44,0x1D,0x14,0x93,0x5F,0x81,0xAF,0x65,0x24,0x42,0x9D
	  };
    CFHD_SetLicense(obj->dec,license);
  }
  obj->samples++;

  int buf_size = avpkt->size;
  AVFrame* frame = (AVFrame*)data;
  bool key = (avpkt->flags & AV_PKT_FLAG_KEY)!=0;
  frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
  frame->key_frame = key;

  if(obj->buf){
    int linesize = (avctx->width*3+0xF) & ~0xF;
    CFHD_DecodeSample(obj->dec,avpkt->data,avpkt->size,obj->buf,linesize);

  } else {
    int frame_size = av_image_get_buffer_size(avctx->pix_fmt, avctx->width, avctx->height, 16);
    frame->buf[0] = av_buffer_alloc(frame_size);
    if(!frame->buf[0]) return AVERROR(ENOMEM);
    const uint8_t* buf = frame->buf[0]->data;
    av_image_fill_arrays(frame->data, frame->linesize, buf, avctx->pix_fmt, avctx->width, avctx->height, 16);

    CFHD_DecodeSample(obj->dec,avpkt->data,avpkt->size,frame->data[0],frame->linesize[0]);
  }

  *got_frame = 1;
  return buf_size;
}

AVCodec cfhd_decoder;

void av_register_cfhd()
{
  cfhd_decoder.name = "CineForm HD (sdk)";
  cfhd_decoder.long_name = "CineForm HD (sdk)";
  cfhd_decoder.type = AVMEDIA_TYPE_VIDEO;
  cfhd_decoder.id = CFHD_ID;
  cfhd_decoder.priv_data_size = sizeof(DecoderObj);
  cfhd_decoder.init = cfhd_init_decoder;
  cfhd_decoder.close = cfhd_close_decoder;
  cfhd_decoder.decode = cfhd_decode;
  cfhd_decoder.priv_class = 0;
  cfhd_decoder.capabilities = 0;

  /*
  AVCodecTag* tag = (AVCodecTag*)avformat_get_riff_video_tags();
  while(tag->id!=AV_CODEC_ID_NONE){
    if(tag->id==AV_CODEC_ID_KGV1){
      DWORD old_protect;
      VirtualProtect(tag,sizeof(*tag),PAGE_READWRITE,&old_protect);
      tag->id = cfhd_decoder.id;
      tag->tag = CFHD_TAG;
      VirtualProtect(tag,sizeof(*tag),old_protect,0);
      break;
    }
    tag++;
  }*/

  avcodec_register(&cfhd_decoder);
}

#else

void av_register_cfhd()
{
}

#endif

//----------------------------------------------------------------------------------------------

struct DecoderObjVfw{
  void* buf;
  HIC codec;
  BITMAPINFOHEADER bmin;
  BITMAPINFOHEADER bmout;
  int samples;
  bool use_v210;
};

void cfhd_set_buffer(AVCodecContext* avctx, void* buf)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;
  obj->buf = buf;
}

void cfhd_set_rgb(AVCodecContext* avctx)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;
  obj->use_v210 = false;

  avctx->pix_fmt = AV_PIX_FMT_BGR24;
  BITMAPINFOHEADER& bmout = obj->bmout;
  memset(&bmout,0,sizeof(bmout));
  bmout.biSize = sizeof(bmout);
  bmout.biWidth = avctx->width;
  bmout.biHeight = avctx->height;
  bmout.biPlanes = 1;
  bmout.biBitCount = 24;
  bmout.biCompression = BI_RGB;
  bmout.biSizeImage = avctx->width*avctx->height*3;
  bmout.biXPelsPerMeter = 1;
  bmout.biYPelsPerMeter = 1;

  if(obj->samples){
    obj->samples = 0;
    ICDecompressEnd(obj->codec);
  }
}

void cfhd_set_v210(AVCodecContext* avctx)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;
  obj->use_v210 = true;

  avctx->pix_fmt = AV_PIX_FMT_BGR24; // best match
  BITMAPINFOHEADER& bmout = obj->bmout;
  memset(&bmout,0,sizeof(bmout));
  bmout.biSize = sizeof(bmout);
  bmout.biWidth = avctx->width;
  bmout.biHeight = avctx->height;
  bmout.biPlanes = 1;
  bmout.biBitCount = 30;
  bmout.biCompression = MAKEFOURCC('v','2','1','0');
  // 6 px per 16 bytes, 128 byte aligned
  int row = (avctx->width+47)/48*128;
  bmout.biSizeImage = row*avctx->height;
  bmout.biXPelsPerMeter = 1;
  bmout.biYPelsPerMeter = 1;

  if(obj->samples){
    obj->samples = 0;
    ICDecompressEnd(obj->codec);
  }
}

av_cold int vfw_cfhd_init_decoder(AVCodecContext* avctx)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;
  obj->buf = 0;
  obj->samples = 0;

  BITMAPINFOHEADER& bmin = obj->bmin;
  memset(&bmin,0,sizeof(bmin));

  bmin.biSize = sizeof(bmin);
  bmin.biWidth = avctx->width;
  bmin.biHeight = avctx->height;
  bmin.biPlanes = 1;
  bmin.biBitCount = 24;
  bmin.biCompression = MAKEFOURCC('C','F','H','D');
  bmin.biSizeImage = avctx->width*avctx->height*3;
  bmin.biXPelsPerMeter = 1;
  bmin.biYPelsPerMeter = 1;

  obj->codec = ICOpen(ICTYPE_VIDEO, MAKEFOURCC('C','F','H','D'), ICMODE_DECOMPRESS);
  if(!obj->codec) return AVERROR(ENOMEM);

  cfhd_set_rgb(avctx);

  return 0;
}

av_cold int vfw_cfhd_close_decoder(AVCodecContext* avctx)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;
  if(obj->codec) ICClose(obj->codec);
  return 0;
}

int vfw_cfhd_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* avpkt)
{
  DecoderObjVfw* obj = (DecoderObjVfw*)avctx->priv_data;

  if(obj->samples==0){
    ICDecompressBegin(obj->codec, &obj->bmin, &obj->bmout);
  }
  obj->samples++;

  int buf_size = avpkt->size;
  AVFrame* frame = (AVFrame*)data;
  bool key = (avpkt->flags & AV_PKT_FLAG_KEY)!=0;
  frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
  frame->key_frame = key;

  int flags = 0;
  if(!key) flags |= ICDECOMPRESS_NOTKEYFRAME;

  if(obj->buf){
    frame->buf[0] = av_buffer_alloc(0); // needed to mute avcodec assert
    BITMAPINFOHEADER bm = obj->bmin;
    bm.biSizeImage = avpkt->size;
    ICDecompress(obj->codec,flags,&bm,avpkt->data,&obj->bmout,obj->buf);

  } else {
    int frame_size = av_image_get_buffer_size(avctx->pix_fmt, avctx->width, avctx->height, 16);
    frame->buf[0] = av_buffer_alloc(frame_size);
    if(!frame->buf[0]) return AVERROR(ENOMEM);
    const uint8_t* buf = frame->buf[0]->data;
    av_image_fill_arrays(frame->data, frame->linesize, buf, avctx->pix_fmt, avctx->width, avctx->height, 16);

    BITMAPINFOHEADER bm = obj->bmin;
    bm.biSizeImage = avpkt->size;
    ICDecompress(obj->codec,flags,&bm,avpkt->data,&obj->bmout,(void*)buf);
  }

  #ifdef FFDEBUG
  OutputDebugString("\n");
  #endif

  *got_frame = 1;
  return buf_size;
}

AVCodec vfw_cfhd_decoder;

bool test_cfhd_vfw()
{
  HIC codec = ICOpen(ICTYPE_VIDEO, MAKEFOURCC('C','F','H','D'), ICMODE_DECOMPRESS);
  ICClose(codec);
  if(!codec) return false;
  return true;
}

void av_register_vfw_cfhd()
{
  if(!test_cfhd_vfw()) return;

  vfw_cfhd_decoder.name = "CineForm HD (vfw)";
  vfw_cfhd_decoder.long_name = "CineForm HD (vfw)";
  vfw_cfhd_decoder.type = AVMEDIA_TYPE_VIDEO;
  vfw_cfhd_decoder.id = CFHD_ID;
  vfw_cfhd_decoder.priv_data_size = sizeof(DecoderObjVfw);
  vfw_cfhd_decoder.init = vfw_cfhd_init_decoder;
  vfw_cfhd_decoder.close = vfw_cfhd_close_decoder;
  vfw_cfhd_decoder.decode = vfw_cfhd_decode;
  vfw_cfhd_decoder.priv_class = 0;
  vfw_cfhd_decoder.capabilities = 0;

  /*
  AVCodecTag* tag = (AVCodecTag*)avformat_get_riff_video_tags();
  while(tag->id!=AV_CODEC_ID_NONE){
    if(tag->id==AV_CODEC_ID_KGV1){
      DWORD old_protect;
      VirtualProtect(tag,sizeof(*tag),PAGE_READWRITE,&old_protect);
      tag->id = vfw_cfhd_decoder.id;
      tag->tag = CFHD_TAG;
      VirtualProtect(tag,sizeof(*tag),old_protect,0);
      break;
    }
    tag++;
  }*/

  avcodec_register(&vfw_cfhd_decoder);
}

