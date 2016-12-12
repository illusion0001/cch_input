#include "vd2\plugin\vdplugin.h"
#include <vd2/VDXFrame/VideoFilterDialog.h>
#include <memory.h>
#include <windows.h>
#include <vfw.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
#include "resource.h"

void init_av();
extern HINSTANCE hInstance;

void copy_rgb24(AVFrame* frame, const VDXPixmapLayout* layout, const void* data)
{
  {for(int y=0; y<layout->h; y++){
    uint8* s = (uint8*)data + layout->data + layout->pitch*y;
    uint8* d = frame->data[0] + frame->linesize[0]*y;

    {for(int x=0; x<layout->w; x++){
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];

      s+=3;
      d+=3;
    }}
  }}
}

void copy_rgb32(AVFrame* frame, const VDXPixmapLayout* layout, const void* data)
{
  {for(int y=0; y<layout->h; y++){
    uint8* s = (uint8*)data + layout->data + layout->pitch*y;
    uint8* d = frame->data[0] + frame->linesize[0]*y;
    memcpy(d,s,layout->w*4);
  }}
}

void copy_rgb64(AVFrame* frame, const VDXPixmapLayout* layout, const void* data)
{
  {for(int y=0; y<layout->h; y++){
    uint16* s = (uint16*)data + layout->data/2 + layout->pitch*y/2;

    uint16* g = (uint16*)frame->data[0] + frame->linesize[0]*y/2;
    uint16* b = (uint16*)frame->data[1] + frame->linesize[1]*y/2;
    uint16* r = (uint16*)frame->data[2] + frame->linesize[2]*y/2;

    {for(int x=0; x<layout->w; x++){
      b[0] = s[0];
      g[0] = s[1];
      r[0] = s[2];

      r++; g++; b++;
      s+=4;
    }}
  }}
}

void copy_yuv(AVFrame* frame, const VDXPixmapLayout* layout, const void* data, int bpp)
{
  int w2 = layout->w;
  int h2 = layout->h;
  switch(layout->format){
  case nsVDXPixmap::kPixFormat_YUV420_Planar:
  case nsVDXPixmap::kPixFormat_YUV420_Planar16:
    w2 = (w2+1)/2;
    h2 = h2/2;
    break;
  case nsVDXPixmap::kPixFormat_YUV422_Planar:
  case nsVDXPixmap::kPixFormat_YUV422_Planar16:
    w2 = (w2+1)/2;
    break;
  }

  {for(int y=0; y<layout->h; y++){
    uint8* s = (uint8*)data + layout->data + layout->pitch*y;
    uint8* d = frame->data[0] + frame->linesize[0]*y;
    memcpy(d,s,layout->w*bpp);
  }}
  {for(int y=0; y<h2; y++){
    uint8* s = (uint8*)data + layout->data2 + layout->pitch2*y;
    uint8* d = frame->data[1] + frame->linesize[1]*y;
    memcpy(d,s,w2*bpp);
  }}
  {for(int y=0; y<h2; y++){
    uint8* s = (uint8*)data + layout->data3 + layout->pitch3*y;
    uint8* d = frame->data[2] + frame->linesize[2]*y;
    memcpy(d,s,w2*bpp);
  }}
}

struct CodecBase{
  enum {
    format_rgb = 1,
    format_rgba = 2,
    format_yuv420 = 3,
    format_yuv422 = 4,
    format_yuv444 = 5,
  };

  struct Config{
    int version;
    int format;
    int bits;

    Config(){ clear(); }
    void clear(){
      version = 0;
      format = format_rgb;
      bits = 8;
    }
  }* config;

  AVRational time_base;
  int keyint;
  LONG frame_total;
  AVColorRange color_range;
  AVColorSpace colorspace;

  AVCodecID codec_id;
  int codec_tag;
  AVCodec* codec;
  AVCodecContext* ctx;
  AVFrame* frame;

  CodecBase(){ 
    codec=0; ctx=0; frame=0;
    keyint=1;
    color_range=AVCOL_RANGE_UNSPECIFIED;
    colorspace=AVCOL_SPC_UNSPECIFIED;
    codec_id = AV_CODEC_ID_NONE;
    codec_tag = 0;
    config = 0;
  }

  virtual ~CodecBase(){
    compress_end();
  }

  bool init(){
    codec = avcodec_find_encoder(codec_id);
    return codec!=0;
  }

  virtual void getinfo(ICINFO& info) = 0;

  virtual int config_size(){ return sizeof(Config); }

  virtual void reset_config(){ config->clear(); }

  virtual bool load_config(void* data, size_t size){
    size_t rsize = config_size();
    if(size!=rsize) return false;
    int src_version = *(int*)data;
    if(src_version!=config->version) return false;
    memcpy(config, data, rsize);
    return true;
  }

  virtual bool test_av_format(AVPixelFormat format){
    if(!codec->pix_fmts) return false;
    for(int i=0; ; i++){
      AVPixelFormat f = codec->pix_fmts[i];
      if(f==-1) break;
      if(f==format) return true;
    }
    return false; 
  }

  LRESULT compress_input_format(FilterModPixmapInfo* info){
    if(config->format==format_rgba){
      return nsVDXPixmap::kPixFormat_XRGB8888;
    }

    if(config->format==format_rgb){
      if(config->bits==8){
        if(test_av_format(AV_PIX_FMT_0RGB32)) return nsVDXPixmap::kPixFormat_XRGB8888;
        if(test_av_format(AV_PIX_FMT_RGB24)) return nsVDXPixmap::kPixFormat_RGB888;
      }

      if(config->bits>8){
        int max_value = (1 << config->bits)-1;
        if(info){
          info->ref_r = max_value;
          info->ref_g = max_value;
          info->ref_b = max_value;
        }
        return nsVDXPixmap::kPixFormat_XRGB64;
      }
    }

    if(config->format==format_yuv420){
      if(config->bits==8){
        return nsVDXPixmap::kPixFormat_YUV420_Planar;
      }
      if(config->bits>8){
        int max_value = (1 << config->bits)-1;
        if(info){
          info->ref_r = max_value;
        }
        return nsVDXPixmap::kPixFormat_YUV420_Planar16;
      }
    }

    if(config->format==format_yuv422){
      if(config->bits==8){
        return nsVDXPixmap::kPixFormat_YUV422_Planar;
      }
      if(config->bits>8){
        int max_value = (1 << config->bits)-1;
        if(info){
          info->ref_r = max_value;
        }
        return nsVDXPixmap::kPixFormat_YUV422_Planar16;
      }
    }

    if(config->format==format_yuv444){
      if(config->bits==8){
        return nsVDXPixmap::kPixFormat_YUV444_Planar;
      }
      if(config->bits>8){
        int max_value = (1 << config->bits)-1;
        if(info){
          info->ref_r = max_value;
        }
        return nsVDXPixmap::kPixFormat_YUV444_Planar16;
      }
    }

    return 0;
  }

  LRESULT compress_get_format(BITMAPINFO *lpbiOutput, VDXPixmapLayout* layout)
  {
    BITMAPINFOHEADER *outhdr = &lpbiOutput->bmiHeader;
    int extra_size = 0;
    if(ctx) extra_size = (ctx->extradata_size+1) & ~1;

    if(!lpbiOutput) return sizeof(BITMAPINFOHEADER)+extra_size;

    if(layout->format!=compress_input_format(0)) return ICERR_BADFORMAT;
    int iWidth  = layout->w;
    int iHeight = layout->h;

    if(iWidth <= 0 || iHeight <= 0) return ICERR_BADFORMAT;

    memset(outhdr, 0, sizeof(BITMAPINFOHEADER));
    outhdr->biSize        = sizeof(BITMAPINFOHEADER);
    outhdr->biWidth       = iWidth;
    outhdr->biHeight      = iHeight;
    outhdr->biPlanes      = 1;
    outhdr->biBitCount    = 32;
    if(layout->format==nsVDXPixmap::kPixFormat_XRGB64) outhdr->biBitCount = 48;
    outhdr->biCompression = codec_tag;
    outhdr->biSizeImage   = iWidth*iHeight*6;
    if(ctx){
      uint8* p = ((uint8*)outhdr)+sizeof(BITMAPINFOHEADER);
      memset(p,0,extra_size);
      memcpy(p,ctx->extradata,ctx->extradata_size);
    }

    return ICERR_OK;
  }

  LRESULT compress_query(BITMAPINFO *lpbiOutput, VDXPixmapLayout* layout)
  {
    BITMAPINFOHEADER *outhdr = &lpbiOutput->bmiHeader;

    if(layout->format!=compress_input_format(0)) return ICERR_BADFORMAT;
    int iWidth  = layout->w;
    int iHeight = layout->h;

    if(iWidth <= 0 || iHeight <= 0) return ICERR_BADFORMAT;

    if(!lpbiOutput) return ICERR_OK;

    if(iWidth != outhdr->biWidth || iHeight != outhdr->biHeight)
      return ICERR_BADFORMAT;

    if(outhdr->biCompression!=codec_tag)
      return ICERR_BADFORMAT;

    return ICERR_OK;
  }

  LRESULT compress_get_size(BITMAPINFO *lpbiOutput)
  {
    return lpbiOutput->bmiHeader.biWidth*lpbiOutput->bmiHeader.biHeight*6 + 4096;
  }

  LRESULT compress_frames_info(ICCOMPRESSFRAMES *icf)
  {
    frame_total = icf->lFrameCount;
    time_base.num = icf->dwRate;
    time_base.den = icf->dwScale;
    keyint = icf->lKeyRate;
    return ICERR_OK;
  }

  virtual bool init_ctx(VDXPixmapLayout* layout)
  {
    return true;
  }

  LRESULT compress_begin(BITMAPINFO *lpbiOutput, VDXPixmapLayout* layout)
  {
    if(compress_query(lpbiOutput, layout) != ICERR_OK){
      return ICERR_BADFORMAT;
    }

    ctx = avcodec_alloc_context3(codec);

    if(config->format==format_rgba){
      switch(config->bits){
      case 8:
        ctx->pix_fmt = AV_PIX_FMT_RGB32;
        break;
      }
    }

    if(config->format==format_rgb){
      switch(config->bits){
      case 16:
        ctx->pix_fmt = AV_PIX_FMT_GBRP16LE;
        break;
      case 14:
        ctx->pix_fmt = AV_PIX_FMT_GBRP14LE;
        break;
      case 12:
        ctx->pix_fmt = AV_PIX_FMT_GBRP12LE;
        break;
      case 10:
        ctx->pix_fmt = AV_PIX_FMT_GBRP10LE;
        break;
      case 9:
        ctx->pix_fmt = AV_PIX_FMT_GBRP9LE;
        break;
      case 8:
        if(test_av_format(AV_PIX_FMT_0RGB32)){
          ctx->pix_fmt = AV_PIX_FMT_0RGB32;
          break;
        }
        if(test_av_format(AV_PIX_FMT_RGB24)){
          ctx->pix_fmt = AV_PIX_FMT_RGB24;
          break;
        }
        break;
      }
    }

    if(config->format==format_yuv420){
      switch(config->bits){
      case 16:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P16LE;
        break;
      case 14:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P14LE;
        break;
      case 12:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P12LE;
        break;
      case 10:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P10LE;
        break;
      case 9:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P9LE;
        break;
      case 8:
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        break;
      }
    }

    if(config->format==format_yuv422){
      switch(config->bits){
      case 16:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P16LE;
        break;
      case 14:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P14LE;
        break;
      case 12:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P12LE;
        break;
      case 10:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
      case 9:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P9LE;
        break;
      case 8:
        ctx->pix_fmt = AV_PIX_FMT_YUV422P;
        break;
      }
    }

    if(config->format==format_yuv444){
      switch(config->bits){
      case 16:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P16LE;
        break;
      case 14:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P14LE;
        break;
      case 12:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P12LE;
        break;
      case 10:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
      case 9:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P9LE;
        break;
      case 8:
        ctx->pix_fmt = AV_PIX_FMT_YUV444P;
        break;
      }
    }

    ctx->bit_rate = 400000;
    ctx->width = layout->w;
    ctx->height = layout->h;
    ctx->time_base = time_base;
    ctx->gop_size = keyint;
    ctx->max_b_frames = 1;

    ctx->color_range = color_range;
    ctx->colorspace = colorspace;

    if(!init_ctx(layout)) return ICERR_BADPARAM;

    if(avcodec_open2(ctx, codec, NULL)<0){ compress_end(); return ICERR_BADPARAM; }

    frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->color_range = ctx->color_range;
    frame->colorspace = ctx->colorspace;
    frame->width  = ctx->width;
    frame->height = ctx->height;

    int ret = av_image_alloc(frame->data, frame->linesize, ctx->width, ctx->height, ctx->pix_fmt, 32);
    if(ret<0){ compress_end(); return ICERR_MEMORY; }

    return ICERR_OK;
  }

  LRESULT compress_end()
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
    return ICERR_OK;
  }

  LRESULT compress(ICCOMPRESS *icc, VDXPixmapLayout* layout)
  {
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = 0;
    pkt.size = 0;

    int got_output;
    int ret;

    if(icc->lFrameNum<frame_total){
      frame->pts = icc->lFrameNum;

      switch(layout->format){
      case nsVDXPixmap::kPixFormat_RGB888:
        copy_rgb24(frame,layout,icc->lpInput);
        break;
      case nsVDXPixmap::kPixFormat_XRGB8888:
        copy_rgb32(frame,layout,icc->lpInput);
        break;
      case nsVDXPixmap::kPixFormat_XRGB64:
        copy_rgb64(frame,layout,icc->lpInput);
        break;
      case nsVDXPixmap::kPixFormat_YUV420_Planar:
      case nsVDXPixmap::kPixFormat_YUV422_Planar:
      case nsVDXPixmap::kPixFormat_YUV444_Planar:
        copy_yuv(frame,layout,icc->lpInput,1);
        break;
      case nsVDXPixmap::kPixFormat_YUV420_Planar16:
      case nsVDXPixmap::kPixFormat_YUV422_Planar16:
      case nsVDXPixmap::kPixFormat_YUV444_Planar16:
        copy_yuv(frame,layout,icc->lpInput,2);
        break;
      }

      ret = avcodec_encode_video2(ctx, &pkt, frame, &got_output);
    } else {
      ret = avcodec_encode_video2(ctx, &pkt, 0, &got_output);
    }

    if(ret<0){ compress_end(); return ICERR_MEMORY; }

    if(got_output){
      if(pkt.size>(int)icc->lpbiOutput->biSizeImage){
        av_packet_unref(&pkt);
        return ICERR_MEMORY;
      }

      memcpy(icc->lpOutput,pkt.data,pkt.size);
      icc->lpbiOutput->biSizeImage = pkt.size;
      av_packet_unref(&pkt);

      *icc->lpdwFlags = AVIIF_KEYFRAME;
    } else {
      icc->lpbiOutput->biSizeImage = 0;
      *icc->lpdwFlags = VDCOMPRESS_WAIT;
    }

    return ICERR_OK;
  }

  virtual LRESULT configure(HWND parent) = 0;
};

//---------------------------------------------------------------------------

class ConfigBase: public VDXVideoFilterDialog{
public:
  CodecBase* codec;
  void* old_param;
  int dialog_id;

  ConfigBase()
  {
    codec = 0;
    old_param = 0;
  }

  virtual ~ConfigBase()
  {
    free(old_param);
  }

  void Show(HWND parent, CodecBase* codec)
  {
    this->codec = codec;
    int rsize = codec->config_size();
    old_param = malloc(rsize);
    memcpy(old_param,codec->config,rsize);
    VDXVideoFilterDialog::Show(hInstance, MAKEINTRESOURCE(dialog_id), parent);
  }

  virtual INT_PTR DlgProc(UINT msg, WPARAM wParam, LPARAM lParam);
  void init_format();
  void init_bits();
  void change_format();
};

void ConfigBase::init_format()
{
  const char* color_names[] = {
    "RGB", 
    "RGB with Alpha",
    "YUV 4:2:0",
    "YUV 4:2:2",
    "YUV 4:4:4",
  };

  SendDlgItemMessage(mhdlg, IDC_COLORSPACE, CB_RESETCONTENT, 0, 0);
  for(int i=0; i<5; i++)
    SendDlgItemMessage(mhdlg, IDC_COLORSPACE, CB_ADDSTRING, 0, (LPARAM)color_names[i]);
  SendDlgItemMessage(mhdlg, IDC_COLORSPACE, CB_SETCURSEL, codec->config->format-1, 0);
}

void ConfigBase::change_format()
{
  switch(codec->config->format){
  case CodecBase::format_rgba:
    codec->config->bits = 8;
    break;
  case CodecBase::format_yuv420:
  case CodecBase::format_yuv422:
  case CodecBase::format_yuv444:
    codec->config->bits = 8;
    break;
  }

  init_bits();
}

void ConfigBase::init_bits()
{
  bool enable_8 = false;
  bool enable_9 = false;
  bool enable_10 = false;
  bool enable_12 = false;
  bool enable_14 = false;
  bool enable_16 = false;
  switch(codec->config->format){
  case CodecBase::format_rgba:
    enable_8 = true;
    break;
  case CodecBase::format_rgb:
    enable_8 = true;
    enable_9 = true;
    enable_10 = true;
    enable_12 = true;
    enable_14 = true;
    enable_16 = codec->test_av_format(AV_PIX_FMT_GBRP16LE);
    break;
  case CodecBase::format_yuv420:
    enable_8 = true;
    enable_9 = codec->test_av_format(AV_PIX_FMT_YUV420P9LE);
    enable_10 = codec->test_av_format(AV_PIX_FMT_YUV420P10LE);
    enable_12 = codec->test_av_format(AV_PIX_FMT_YUV420P12LE);
    enable_14 = codec->test_av_format(AV_PIX_FMT_YUV420P14LE);
    enable_16 = codec->test_av_format(AV_PIX_FMT_YUV420P16LE);
    break;
  case CodecBase::format_yuv422:
    enable_8 = true;
    enable_9 = codec->test_av_format(AV_PIX_FMT_YUV422P9LE);
    enable_10 = codec->test_av_format(AV_PIX_FMT_YUV422P10LE);
    enable_12 = codec->test_av_format(AV_PIX_FMT_YUV422P12LE);
    enable_14 = codec->test_av_format(AV_PIX_FMT_YUV422P14LE);
    enable_16 = codec->test_av_format(AV_PIX_FMT_YUV422P16LE);
    break;
  case CodecBase::format_yuv444:
    enable_8 = true;
    enable_9 = codec->test_av_format(AV_PIX_FMT_YUV444P9LE);
    enable_10 = codec->test_av_format(AV_PIX_FMT_YUV444P10LE);
    enable_12 = codec->test_av_format(AV_PIX_FMT_YUV444P12LE);
    enable_14 = codec->test_av_format(AV_PIX_FMT_YUV444P14LE);
    enable_16 = codec->test_av_format(AV_PIX_FMT_YUV444P16LE);
    break;
  }

  EnableWindow(GetDlgItem(mhdlg,IDC_8_BIT),enable_8);
  EnableWindow(GetDlgItem(mhdlg,IDC_9_BIT),enable_9);
  EnableWindow(GetDlgItem(mhdlg,IDC_10_BIT),enable_10);
  EnableWindow(GetDlgItem(mhdlg,IDC_12_BIT),enable_12);
  EnableWindow(GetDlgItem(mhdlg,IDC_14_BIT),enable_14);
  EnableWindow(GetDlgItem(mhdlg,IDC_16_BIT),enable_16);
  CheckDlgButton(mhdlg,IDC_8_BIT,codec->config->bits==8 ? BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(mhdlg,IDC_9_BIT,codec->config->bits==9 ? BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(mhdlg,IDC_10_BIT,codec->config->bits==10 ? BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(mhdlg,IDC_12_BIT,codec->config->bits==12 ? BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(mhdlg,IDC_14_BIT,codec->config->bits==14 ? BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(mhdlg,IDC_16_BIT,codec->config->bits==16 ? BST_CHECKED:BST_UNCHECKED);
}

INT_PTR ConfigBase::DlgProc(UINT msg, WPARAM wParam, LPARAM lParam){
  switch(msg){
  case WM_INITDIALOG:
    {
      SetDlgItemText(mhdlg,IDC_ENCODER_LABEL,LIBAVCODEC_IDENT);
      init_format();
      init_bits();
      return true;
    }

  case WM_COMMAND:
    switch(LOWORD(wParam)){
    case IDOK:
      EndDialog(mhdlg, true);
      return TRUE;

    case IDCANCEL:
      memcpy(codec->config, old_param, codec->config_size());
      EndDialog(mhdlg, false);
      return TRUE;

    case IDC_COLORSPACE:
      if(HIWORD(wParam)==LBN_SELCHANGE){
        codec->config->format = (int)SendDlgItemMessage(mhdlg, IDC_COLORSPACE, CB_GETCURSEL, 0, 0)+1;
        change_format();
        return TRUE;
      }
      break;

    case IDC_8_BIT:
    case IDC_9_BIT:
    case IDC_10_BIT:
    case IDC_12_BIT:
    case IDC_14_BIT:
    case IDC_16_BIT:
      if(IsDlgButtonChecked(mhdlg,IDC_8_BIT)) codec->config->bits = 8;
      if(IsDlgButtonChecked(mhdlg,IDC_9_BIT)) codec->config->bits = 9;
      if(IsDlgButtonChecked(mhdlg,IDC_10_BIT)) codec->config->bits = 10;
      if(IsDlgButtonChecked(mhdlg,IDC_12_BIT)) codec->config->bits = 12;
      if(IsDlgButtonChecked(mhdlg,IDC_14_BIT)) codec->config->bits = 14;
      if(IsDlgButtonChecked(mhdlg,IDC_16_BIT)) codec->config->bits = 16;
      break;
    }
  }

  return FALSE;
}

//---------------------------------------------------------------------------

class ConfigFFV1: public ConfigBase{
public:
  ConfigFFV1(){ dialog_id = IDD_ENC_FFV1; }
};

struct CodecFFV1: public CodecBase{
  enum{ tag=MKTAG('F', 'F', 'V', '1') };
  struct Config: public CodecBase::Config{
  } codec_config;

  CodecFFV1(){
    config = &codec_config;
    codec_id = AV_CODEC_ID_FFV1;
    codec_tag = tag;
  }

  int config_size(){ return sizeof(Config); }

  void getinfo(ICINFO& info){
    info.fccHandler = codec_tag;
    info.dwFlags = VIDCF_COMPRESSFRAMES | VIDCF_FASTTEMPORALC;
    wcscpy(info.szName, L"FFV1");
    wcscpy(info.szDescription, L"FFMPEG FFV1 lossless codec");
  }

  bool init_ctx(VDXPixmapLayout* layout)
  {
    ctx->strict_std_compliance = -2;
    ctx->level = 3;
    ctx->thread_count = 0;
    ctx->slices = 4;
    return true;
  }

  LRESULT configure(HWND parent)
  {
    ConfigFFV1 dlg;
    dlg.Show(parent,this);
    return ICERR_OK;
  }
};

//---------------------------------------------------------------------------

class ConfigHUFF: public ConfigBase{
public:
  ConfigHUFF(){ dialog_id = IDD_ENC_FFVHUFF; }
  INT_PTR DlgProc(UINT msg, WPARAM wParam, LPARAM lParam);
};

struct CodecHUFF: public CodecBase{
  enum{ tag=MKTAG('F', 'F', 'V', 'H') };
  struct Config: public CodecBase::Config{
    int prediction;

    Config(){ prediction=0; }
    void clear(){ CodecBase::Config::clear(); prediction=0; }
  } codec_config;

  CodecHUFF(){
    config = &codec_config;
    codec_id = AV_CODEC_ID_FFVHUFF;
    codec_tag = tag;
  }

  int config_size(){ return sizeof(Config); }
  void reset_config(){ codec_config.clear(); }

  void getinfo(ICINFO& info){
    info.fccHandler = codec_tag;
    info.dwFlags = VIDCF_COMPRESSFRAMES | VIDCF_FASTTEMPORALC;
    wcscpy(info.szName, L"FFVHUFF");
    wcscpy(info.szDescription, L"FFMPEG Huffyuv lossless codec");
  }

  bool init_ctx(VDXPixmapLayout* layout)
  {
    ctx->thread_count = 0;
    int pred = codec_config.prediction;
    if(pred==2 && config->format==format_rgb && config->bits==8) pred = 0;
    av_opt_set_int(ctx->priv_data, "pred", pred, 0);
    return true;
  }

  LRESULT configure(HWND parent)
  {
    ConfigHUFF dlg;
    dlg.Show(parent,this);
    return ICERR_OK;
  }
};

INT_PTR ConfigHUFF::DlgProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch(msg){
  case WM_INITDIALOG:
    {
      const char* pred_names[] = {
        "left", 
        "plane",
        "median",
      };

      SendDlgItemMessage(mhdlg, IDC_PREDICTION, CB_RESETCONTENT, 0, 0);
      for(int i=0; i<3; i++)
        SendDlgItemMessage(mhdlg, IDC_PREDICTION, CB_ADDSTRING, 0, (LPARAM)pred_names[i]);
      CodecHUFF::Config* config = (CodecHUFF::Config*)codec->config;
      SendDlgItemMessage(mhdlg, IDC_PREDICTION, CB_SETCURSEL, config->prediction, 0);
      break;
    }

  case WM_COMMAND:
    switch(LOWORD(wParam)){
    case IDC_PREDICTION:
      if(HIWORD(wParam)==LBN_SELCHANGE){
        CodecHUFF::Config* config = (CodecHUFF::Config*)codec->config;
        config->prediction = (int)SendDlgItemMessage(mhdlg, IDC_PREDICTION, CB_GETCURSEL, 0, 0);
        return TRUE;
      }
      break;
    }
  }
  return ConfigBase::DlgProc(msg,wParam,lParam);
}

//---------------------------------------------------------------------------

extern "C" LRESULT WINAPI DriverProc(DWORD_PTR dwDriverId, HDRVR hDriver, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
  CodecBase* codec = (CodecBase*)dwDriverId;

  switch (uMsg){
  case DRV_LOAD:
    init_av();
    return DRV_OK;

  case DRV_FREE:
    return DRV_OK;

  case DRV_OPEN:
    {
      ICOPEN* icopen = (ICOPEN*)lParam2;
      if(icopen && icopen->fccType != ICTYPE_VIDEO) return 0;

      CodecBase* codec = 0;
      if(icopen->fccHandler==0) codec = new CodecFFV1;
      if(icopen->fccHandler==CodecFFV1::tag) codec = new CodecFFV1;
      if(icopen->fccHandler==CodecHUFF::tag) codec = new CodecHUFF;
      if(codec){
        if(!codec->init()){
          delete codec;
          codec = 0;
        }
      }
      if(!codec){
        if(icopen) icopen->dwError = ICERR_MEMORY;
        return 0;
      }

      if(icopen) icopen->dwError = ICERR_OK;
      return (LRESULT)codec;
    }

  case DRV_CLOSE:
    delete codec;
    return DRV_OK;

  case ICM_GETSTATE:
    {
      int rsize = codec->config_size();
      if(lParam1==0) return rsize;
      if(lParam2!=rsize) return ICERR_BADSIZE;
      memcpy((void*)lParam1, codec->config, rsize);
      return ICERR_OK;
    }

  case ICM_SETSTATE:
    if(lParam1==0){
      codec->reset_config();
      return 0;
    }
    if(!codec->load_config((void*)lParam1,lParam2)) return 0;
    return codec->config_size();

  case ICM_GETINFO:
    {
      ICINFO* icinfo = (ICINFO*)lParam1;
      if(lParam2<sizeof(ICINFO)) return 0;
      icinfo->dwSize = sizeof(ICINFO);
      icinfo->fccType = ICTYPE_VIDEO;
      icinfo->dwVersion = 0;
      icinfo->dwVersionICM = ICVERSION;
      codec->getinfo(*icinfo);
      return sizeof(ICINFO);
    }

  case ICM_CONFIGURE:
    if(lParam1!=-1)
      return codec->configure((HWND)lParam1);
    return ICERR_OK;

  case ICM_ABOUT:
    return ICERR_UNSUPPORTED;

  case ICM_COMPRESS_END:
    return codec->compress_end();

  case ICM_COMPRESS_FRAMES_INFO:
    return codec->compress_frames_info((ICCOMPRESSFRAMES *)lParam1);
  }

  return ICERR_UNSUPPORTED;
}

extern "C" LRESULT WINAPI VDDriverProc(DWORD_PTR dwDriverId, HDRVR hDriver, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
  CodecBase* codec = (CodecBase*)dwDriverId;

  switch (uMsg){
  case VDICM_ENUMFORMATS:
    if(lParam1==0) return CodecFFV1::tag;
    if(lParam1==CodecFFV1::tag) return CodecHUFF::tag;
    return 0;

  case VDICM_COMPRESS_INPUT_FORMAT:
    return codec->compress_input_format((FilterModPixmapInfo*)lParam1);

  case VDICM_COMPRESS_QUERY:
    return codec->compress_query((BITMAPINFO *)lParam2, (VDXPixmapLayout*)lParam1);
  
  case VDICM_COMPRESS_GET_FORMAT:
    return codec->compress_get_format((BITMAPINFO *)lParam2, (VDXPixmapLayout*)lParam1);
  
  case VDICM_COMPRESS_GET_SIZE:
    return codec->compress_get_size((BITMAPINFO *)lParam2);
  
  case VDICM_COMPRESS_BEGIN:
    return codec->compress_begin((BITMAPINFO *)lParam2, (VDXPixmapLayout*)lParam1);
  
  case VDICM_COMPRESS:
    return codec->compress((ICCOMPRESS *)lParam1, (VDXPixmapLayout*)lParam2);

  case VDICM_COMPRESS_MATRIX_INFO:
    {
      int colorSpaceMode = (int)lParam1;
      int colorRangeMode = (int)lParam2;

      switch(colorSpaceMode){
      case nsVDXPixmap::kColorSpaceMode_601:
        codec->colorspace = AVCOL_SPC_BT470BG;
        break;
      case nsVDXPixmap::kColorSpaceMode_709:
        codec->colorspace = AVCOL_SPC_BT709;
        break;
      default:
        codec->colorspace = AVCOL_SPC_UNSPECIFIED;
      }

      switch(colorRangeMode){
      case nsVDXPixmap::kColorRangeMode_Limited:
        codec->color_range = AVCOL_RANGE_MPEG;
        break;
      case nsVDXPixmap::kColorRangeMode_Full:
        codec->color_range = AVCOL_RANGE_JPEG;
        break;
      default:
        codec->color_range = AVCOL_RANGE_UNSPECIFIED;
      }

      return ICERR_OK;
    }
  }

  return ICERR_UNSUPPORTED;
}
