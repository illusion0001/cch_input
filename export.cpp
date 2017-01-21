#include "InputFile2.h"
#include "FileInfo2.h"
#include "VideoSource2.h"
#include "AudioSource2.h"
#include "export.h"
#include "cineform.h"
#include <string>
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include "resource.h"
#include <vfw.h>

#pragma warning(disable:4996)

extern HINSTANCE hInstance;

void utf8_to_widechar(wchar_t *dst, int max_dst, const char *src);
void widechar_to_utf8(char *dst, int max_dst, const wchar_t *src);

struct IOBuffer{
  uint8_t* data;
  int64_t size;
  int64_t pos;

  IOBuffer(){
    data=0; size=0; pos=0; 
  }

  ~IOBuffer(){
    av_free(data);
  }

  void copy(void* data, int size){
    alloc(size);
    memcpy(this->data,data,size);
  }

  void alloc(int n){
    data = (uint8_t*)av_malloc(n+AVPROBE_PADDING_SIZE);
    memset(data,0,n+AVPROBE_PADDING_SIZE);
    size = n;
  }

  static int Read(void* obj, uint8_t* buf, int buf_size){
    IOBuffer* t = (IOBuffer*)obj;
    int64_t n = t->pos+buf_size<t->size ? buf_size : t->size-t->pos;
    memcpy(buf,t->data+t->pos,int(n));
    t->pos += n;
    return int(n);
  }

  static int64_t Seek(void* obj, int64_t offset, int whence){
    IOBuffer* t = (IOBuffer*)obj;
    if(whence==AVSEEK_SIZE) return t->size;
    if(whence==SEEK_CUR){ t->pos+=offset; return t->pos; }
    if(whence==SEEK_SET){ t->pos=offset; return t->pos; }
    if(whence==SEEK_END){ t->pos=t->size+offset; return t->pos; }
    return -1;
  }
};

bool VDXAPIENTRY VDFFInputFile::GetExportMenuInfo(int id, char* name, int name_size, bool* enabled)
{
  if(id==0){
    strncpy(name,"Stream copy...",name_size);
    *enabled = !is_image && !is_image_list;
    return true;
  }

  return false;
}

bool VDXAPIENTRY VDFFInputFile::GetExportCommandName(int id, char* name, int name_size)
{
  if(id==0){
    strncpy(name,"Export.StreamCopy",name_size);
    return true;
  }

  return false;
}

bool exportSaveFile(HWND hwnd, wchar_t* path, int max_path) {
  OPENFILENAMEW ofn = {0};
  wchar_t szFile[MAX_PATH];

  if (path)
    wcscpy(szFile,path);
  else
    szFile[0] = 0;

  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrTitle = L"Export Stream Copy";

  std::wstring ext;
  const wchar_t* p = wcsrchr(path,'.');
  if(p) ext = p; else ext = L".";

  wchar_t filter[256];
  swprintf(filter,256,L"Same as source (*%ls)",ext.c_str());
  size_t n = wcslen(filter)+1;
  filter[n] = '*'; n++;
  filter[n] = 0; wcscat(filter+n,ext.c_str()); n+=ext.length();
  filter[n] = 0; n++;
  const wchar_t filter2[] = L"All files (*.*)\0*.*\0";
  memcpy(filter+n,filter2,sizeof(filter2));

  ofn.lpstrFilter = filter;
  ofn.nFilterIndex = 1;
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = sizeof szFile;
  ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_ENABLESIZING | OFN_OVERWRITEPROMPT;

  if (GetSaveFileNameW(&ofn)){
    wcscpy(path,szFile);
    wchar_t* p1 = wcsrchr(szFile,'.');
    if(!p1) wcscat(path,ext.c_str());
    return true;
  }

  return false;
}

struct ProgressDialog: public VDXVideoFilterDialog{
public:
  bool abort;
  DWORD dwLastTime;
  int mSparseCount;
  int mSparseInterval;
  int64_t current_bytes;
  double current_pos;
  bool changed;
  HWND parent;

  ProgressDialog(){ 
    abort=false; mSparseCount=1; mSparseInterval=1; 
    current_bytes=0;
    current_pos=0;
    changed=false;
  }
  ~ProgressDialog(){ Close(); }
  void Show(HWND parent);
  void Close();
  virtual INT_PTR DlgProc(UINT msg, WPARAM wParam, LPARAM lParam);
  void init_bytes(int64_t bytes);
  void init_pos(double p);
  void sync_state();
  HWND getHwnd(){ return mhdlg; }
  void check();
};

void ProgressDialog::Show(HWND parent){
  this->parent = parent;
  VDXVideoFilterDialog::ShowModeless(hInstance, MAKEINTRESOURCE(IDD_EXPORT_PROGRESS), parent);
}

void ProgressDialog::Close(){
  EnableWindow(parent,true);
  DestroyWindow(mhdlg);
}

INT_PTR ProgressDialog::DlgProc(UINT msg, WPARAM wParam, LPARAM lParam){
  switch(msg){
  case WM_INITDIALOG:
    {
      EnableWindow(parent,false);
      dwLastTime = GetTickCount();
      init_bytes(0);
      SendMessage(GetDlgItem(mhdlg, IDC_EXPORT_PROGRESS), PBM_SETRANGE, 0, MAKELPARAM(0, 16384));
      SetTimer(mhdlg, 1, 500, NULL);
      return true;
    }
  case WM_COMMAND:
    switch(LOWORD(wParam)){
    case IDCANCEL:
      abort = true;
      return TRUE;
    }
    return TRUE;
    case WM_TIMER:
      sync_state();
      return TRUE;
  }

  return FALSE;
}

void ProgressDialog::sync_state(){
  if(changed){
    init_bytes(current_bytes);
    init_pos(current_pos);
    changed=false;
  }
}

void ProgressDialog::init_bytes(int64_t bytes){
  double n = double(bytes);
  const char* x = "K";
  n = n/1024;
  if(n/1024>8){
    n = n/1024;
    x = "M";
  }
  if(n/1024>8){
    n = n/1024;
    x = "G";
  }
  char buf[1024];
  sprintf(buf,"%5.2f%s bytes copied",n,x);
  SetDlgItemText(mhdlg,IDC_EXPORT_STATE,buf);
}

void ProgressDialog::init_pos(double p){
  if(p<0) p=0;
  if(p>1) p=1;
  int v = int(p*16384);
  SendMessage(GetDlgItem(mhdlg, IDC_EXPORT_PROGRESS), PBM_SETPOS, v, 0);
}

void ProgressDialog::check(){
  MSG msg;

  if (--mSparseCount)
    return;

  DWORD dwTime = GetTickCount();

  mSparseCount = mSparseInterval;

  if (dwTime < dwLastTime + 50) {
    ++mSparseInterval;
  } else if (dwTime > dwLastTime + 150) {
    if (mSparseInterval>1)
      --mSparseInterval;
  }

  dwLastTime = dwTime;

  while(PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
    if (!IsDialogMessage(mhdlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
}

bool VDXAPIENTRY VDFFInputFile::ExecuteExport(int id, VDXHWND parent, IProjectState* state)
{
  if(id==0){
    sint64 start;
    sint64 end;
    if(!state->GetSelection(start,end)){
      start = 0;
      end = video_source->sample_count;
    }

    wchar_t* ext0 = wcsrchr(path,'.');
    wchar_t path2[MAX_PATH];
    if(ext0){
      wcsncpy(path2,path,ext0-path);
      path2[ext0-path] = 0;
    } else {
      wcscpy(path2,path);
    }
    wcscat(path2,L"-01");
    if(ext0) wcscat(path2,ext0);
    if(!exportSaveFile((HWND)parent,path2,MAX_PATH)) return false;

    wchar_t* ext1 = wcsrchr(path2,'.');
    bool same_format = wcscmp(ext0,ext1)==0;

    const int ff_path_size = MAX_PATH*4; // utf8, worst case
    char ff_path[ff_path_size];
    widechar_to_utf8(ff_path, ff_path_size, path);
    char out_ff_path[ff_path_size];
    widechar_to_utf8(out_ff_path, ff_path_size, path2);

    AVOutputFormat* oformat = av_guess_format(0, out_ff_path, 0);
    if(!oformat){
      MessageBox((HWND)parent,"Unable to find a suitable output format","Stream copy",MB_ICONSTOP|MB_OK);
      return false;
    }

    ProgressDialog progress;
    progress.Show((HWND)parent);

    AVFormatContext* fmt = 0;
    AVFormatContext* ofmt = 0;
    bool v_end;
    bool a_end;
    int64_t vt_end=-1;
    int64_t at_end=-1;
    int64_t pos0,pos1;
    int64_t a_bias = 0;
    int video = -1;
    int audio = -1;
    AVStream* out_video=0;
    AVStream* out_audio=0;

    int err = 0; 
    err = avformat_open_input(&fmt, ff_path, 0, 0);
    if(err<0) goto end;
    err = avformat_find_stream_info(fmt, 0);
    if(err<0) goto end;

    err = avformat_alloc_output_context2(&ofmt, 0, 0, out_ff_path);
    if(err<0) goto end;

    video = video_source->m_streamIndex;
    if(audio_source)
      audio = audio_source->m_streamIndex;
 
    {for(int i=0; i<(int)fmt->nb_streams; i++){
      if(i!=video && i!=audio) continue;

      AVStream *in_stream = fmt->streams[i];
      AVStream *out_stream = avformat_new_stream(ofmt, in_stream->codec->codec);
      if(!out_stream){
        err = AVERROR_UNKNOWN;
        goto end;
      }

      err = avcodec_copy_context(out_stream->codec, in_stream->codec);
      if(err<0) goto end;

      out_stream->codec->codec_tag = 0;
      AVCodecID codec_id1 = av_codec_get_id(ofmt->oformat->codec_tag, in_stream->codec->codec_tag);
      unsigned int codec_tag2;
      int have_codec_tag2 = av_codec_get_tag2(ofmt->oformat->codec_tag, in_stream->codec->codec_id, &codec_tag2);
      if(!ofmt->oformat->codec_tag || codec_id1==out_stream->codec->codec_id || !have_codec_tag2)
        out_stream->codec->codec_tag = in_stream->codec->codec_tag;

      if(ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

      out_stream->codec->bit_rate = in_stream->codec->bit_rate;
      out_stream->codec->field_order = in_stream->codec->field_order;

      uint64_t extra_size = (uint64_t)in_stream->codec->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE;
      if (in_stream->codec->extradata_size) {
        out_stream->codec->extradata = (uint8_t*)av_mallocz(size_t(extra_size));
        memcpy(out_stream->codec->extradata, in_stream->codec->extradata, in_stream->codec->extradata_size);
      }
      out_stream->codec->extradata_size = in_stream->codec->extradata_size;

      out_stream->avg_frame_rate = in_stream->avg_frame_rate;
      out_stream->time_base = in_stream->time_base;
      err = avformat_transfer_internal_stream_timing_info(ofmt->oformat, out_stream, in_stream, AVFMT_TBCF_AUTO);
      if(err<0) goto end;

      AVRational r = av_stream_get_r_frame_rate(in_stream);
      av_stream_set_r_frame_rate(out_stream,r);
      out_stream->avg_frame_rate = in_stream->avg_frame_rate;

      if(i==video) out_video = out_stream;
      if(i==audio) out_audio = out_stream;
    }}

    if(!(ofmt->oformat->flags & AVFMT_NOFILE)){
      err = avio_open(&ofmt->pb, out_ff_path, AVIO_FLAG_WRITE);
      if(err<0) goto end;
    }

    err = avformat_write_header(ofmt, 0);
    if(err<0) goto end;

    /*
    int64_t vt_end = video_source->frame_to_pts_next(end);
    int64_t at_end = -1;
    if(out_audio) at_end = audio_source->frame_to_pts(end,video_source->m_pStreamCtx);
    */

    pos1 = end*video_source->time_base.den / video_source->time_base.num + video_source->start_time;
    av_seek_frame(fmt,video,pos1,0);

    while(1){
      AVPacket pkt;
      err = av_read_frame(fmt, &pkt);
      if(err<0) break;

      AVStream* s = fmt->streams[pkt.stream_index];
      int64_t t = pkt.pts;
      if(t==AV_NOPTS_VALUE) t = pkt.dts;
      if(pkt.stream_index==video){
        if(vt_end==-1) vt_end = t;
      }
      if(pkt.stream_index==audio){
        if(at_end==-1) at_end = t;
      }
      av_packet_unref(&pkt);

      if(vt_end!=-1 && (at_end!=-1 || audio==-1)) break;
    }

    pos0 = start*video_source->time_base.den / video_source->time_base.num + video_source->start_time;
    av_seek_frame(fmt,video,pos0,AVSEEK_FLAG_BACKWARD);

    v_end = out_video==0;
    a_end = out_audio==0;
    if(out_audio) a_bias = av_rescale_q(pos0,fmt->streams[video]->time_base,fmt->streams[audio]->time_base);

    while(1){
      if(v_end && a_end) break;
      progress.check();
      if(progress.abort) break;

      AVPacket pkt;
      err = av_read_frame(fmt, &pkt);
      if(err<0){ err=0; break; }

      AVStream* in_stream = fmt->streams[pkt.stream_index];
      int64_t t = pkt.pts;
      if(t==AV_NOPTS_VALUE) t = pkt.dts;

      AVStream* out_stream = 0;
      if(pkt.stream_index==video){
        if(vt_end!=-1 && t>=vt_end) v_end=true; else out_stream = out_video;
        if(pkt.pts!=AV_NOPTS_VALUE) pkt.pts -= pos0;
        if(pkt.dts!=AV_NOPTS_VALUE) pkt.dts -= pos0;
        progress.current_pos = (double(t)-pos0)/(pos1-pos0);
      }
      if(pkt.stream_index==audio){
        if(at_end!=-1 && t>=at_end) a_end=true; else out_stream = out_audio;
        out_stream = out_audio;
        if(pkt.pts!=AV_NOPTS_VALUE) pkt.pts -= a_bias;
        if(pkt.dts!=AV_NOPTS_VALUE) pkt.dts -= a_bias;
      }

      if(out_stream){
        int64_t size = pkt.size;
        av_packet_rescale_ts(&pkt,in_stream->time_base,out_stream->time_base);
        pkt.pos = -1;
        pkt.stream_index = out_stream->index;
        err = av_interleaved_write_frame(ofmt, &pkt);
        if(err<0){
          av_packet_unref(&pkt);
          break;
        }
        progress.current_bytes += size;
        progress.changed = true;
      }

      av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt);

end:
    avformat_close_input(&fmt);
    if(ofmt && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);

    progress.current_pos = 1;
    progress.sync_state();

    if(err<0){
      char buf[1024];
      char buf2[1024];
      av_strerror(err,buf2,1024);
      strcpy(buf,"Operation failed.\nInternal error (FFMPEG): ");
      strcat(buf,buf2);
      MessageBox(progress.getHwnd(),buf,"Stream copy",MB_ICONSTOP|MB_OK);
      return false;
    } else if(!progress.abort){
      MessageBox(progress.getHwnd(),"Operation completed successfully.","Stream copy",MB_OK);
      return true;
    }
  }

  return false;
}

FFOutputFile::FFOutputFile(const VDXInputDriverContext &pContext)
  :mContext(pContext)
{
  ofmt = 0;
  header = false;
}

FFOutputFile::~FFOutputFile()
{
  Finalize();
}

void FFOutputFile::av_error(int err)
{
  char buf[1024];
  char buf2[1024];
  av_strerror(err,buf2,1024);
  strcpy(buf,"Internal error (FFMPEG): ");
  strcat(buf,buf2);
  mContext.mpCallbacks->SetError(buf);
}

/*
void av_log_func2(void* obj, int type, const char* msg, va_list arg)
{
  char buf[1024];
  vsprintf(buf,msg,arg);
  OutputDebugString(buf);
  switch(type){
  case AV_LOG_PANIC:
  case AV_LOG_FATAL:
  case AV_LOG_ERROR:
  case AV_LOG_WARNING:
    ;//DebugBreak();
  }
}
*/

void init_av();

void FFOutputFile::Init(const wchar_t *path, const char* format)
{
  /*av_log_set_callback(av_log_func2);
  av_log_set_level(AV_LOG_INFO);
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  */

  init_av();

  const int ff_path_size = MAX_PATH*4; // utf8, worst case
  char out_ff_path[ff_path_size];
  widechar_to_utf8(out_ff_path, ff_path_size, path);
  this->out_ff_path = out_ff_path;

  int err = 0; 
  AVOutputFormat* oformat = 0;
  if(format && format[0]) oformat = av_guess_format(format, 0, 0);
  if(!oformat) oformat = av_guess_format(0, out_ff_path, 0);
  if(!oformat){
    mContext.mpCallbacks->SetError("Unable to find a suitable output format");
    Finalize();
    return;
  }

  err = avformat_alloc_output_context2(&ofmt, oformat, 0, 0);
  if(err<0){
    av_error(err);
    Finalize();
    return;
  }
}

uint32 FFOutputFile::CreateStream(int type)
{
  uint32 index = stream.size();
  stream.resize(index+1);
  StreamInfo& s = stream[index];
  return index;
}

typedef struct AVCodecTag {
  enum AVCodecID id;
  unsigned int tag;
} AVCodecTag;

struct AVIStreamHeader_fixed {
    uint32		fccType;
    uint32		fccHandler;
    uint32		dwFlags;
    uint16		wPriority;
    uint16		wLanguage;
    uint32		dwInitialFrames;
    uint32		dwScale;	
    uint32		dwRate;
    uint32		dwStart;
    uint32		dwLength;
    uint32		dwSuggestedBufferSize;
    uint32		dwQuality;
    uint32		dwSampleSize;
	struct {
		sint16	left;
		sint16	top;
		sint16	right;
		sint16	bottom;
	} rcFrame;
};

void FFOutputFile::SetVideo(uint32 index, const AVIStreamHeader_fixed& asi, const void *pFormat, int cbFormat)
{
  StreamInfo& s = stream[index];

  AVStream *st = avformat_new_stream(ofmt, 0);
  st->codec->codec_type = AVMEDIA_TYPE_VIDEO;

  import_bmp(st,pFormat,cbFormat);
  adjust_codec_tag(st);

  st->avg_frame_rate = av_make_q(asi.dwRate,asi.dwScale);
  av_stream_set_r_frame_rate(st,st->avg_frame_rate);

  st->time_base = av_make_q(asi.dwScale,asi.dwRate);
  st->codec->time_base = st->time_base;

  s.st = st;
}

void FFOutputFile::SetAudio(uint32 index, const AVIStreamHeader_fixed& asi, const void *pFormat, int cbFormat)
{
  StreamInfo& s = stream[index];

  AVStream *st = avformat_new_stream(ofmt, 0);
  st->codec->codec_type = AVMEDIA_TYPE_AUDIO;

  import_wav(st,pFormat,cbFormat);
  adjust_codec_tag(st);

  st->time_base = av_make_q(asi.dwScale,asi.dwRate);
  st->codec->time_base = st->time_base;

  s.st = st;
}

void FFOutputFile::import_bmp(AVStream *st, const void *pFormat, int cbFormat)
{
  BITMAPINFOHEADER* bm = (BITMAPINFOHEADER*)pFormat;
  DWORD tag = bm->biCompression;

  AVCodecID codec_id = AV_CODEC_ID_NONE;

  if(!codec_id){
    AVCodecTag* riff_tag = (AVCodecTag*)avformat_get_riff_video_tags();
    while(riff_tag->id!=AV_CODEC_ID_NONE){
      if(riff_tag->tag==tag){
        codec_id = riff_tag->id;
        break;
      }
      riff_tag++;
    }
  }

  if(!codec_id){
    AVCodecTag* mov_tag = (AVCodecTag*)avformat_get_mov_video_tags();
    while(mov_tag->id!=AV_CODEC_ID_NONE){
      if(mov_tag->tag==tag){
        codec_id = mov_tag->id;
        break;
      }
      mov_tag++;
    }
  }

  st->codec->codec_id = codec_id;
  st->codec->codec_tag = tag;
  st->codec->width = bm->biWidth;
  st->codec->height = bm->biHeight;

  if(cbFormat>sizeof(BITMAPINFOHEADER)){
    st->codec->extradata_size = cbFormat-sizeof(BITMAPINFOHEADER);
    st->codec->extradata = (uint8_t*)av_mallocz(st->codec->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(st->codec->extradata, ((char*)pFormat)+sizeof(BITMAPINFOHEADER), st->codec->extradata_size);
  }
}

void FFOutputFile::import_wav(AVStream *st, const void *pFormat, int cbFormat)
{
  uint32 dwHeader[20];
  dwHeader[0]	= FOURCC_RIFF;
  dwHeader[1] = 20 - 8 + cbFormat;
  dwHeader[2] = mmioFOURCC('W', 'A', 'V', 'E');
  dwHeader[3] = mmioFOURCC('f', 'm', 't', ' ');
  dwHeader[4] = cbFormat;

  std::vector<char> wav;
  wav.resize(20+cbFormat);
  memcpy(&wav[0], dwHeader, 20);
  memcpy(&wav[20], pFormat, cbFormat);
  if(cbFormat & 1) wav.push_back(0);

  dwHeader[0] = mmioFOURCC('d', 'a', 't', 'a');
  dwHeader[1] = 0;

  int p = wav.size();
  wav.resize(p+8);
  memcpy(&wav[p], dwHeader, 8);

  IOBuffer buf;
  buf.copy(&wav[0],wav.size());
  AVIOContext* avio_ctx = avio_alloc_context(0,0,0,&buf,&IOBuffer::Read,0,0);

  AVFormatContext* fmt_ctx = avformat_alloc_context();
  fmt_ctx->pb = avio_ctx;
  int err = avformat_open_input(&fmt_ctx, 0, 0, 0);
  if(err<0) av_error(err);
  err = avformat_find_stream_info(fmt_ctx, 0);
  if(err<0) av_error(err);

  AVStream *fs = fmt_ctx->streams[0];

  st->codec->codec_id = fs->codec->codec_id;
  st->codec->codec_tag = fs->codec->codec_tag;
  st->codec->channels = fs->codec->channels;
  st->codec->channel_layout = fs->codec->channel_layout;
  st->codec->sample_rate = fs->codec->sample_rate;
  st->codec->block_align = fs->codec->block_align;
  st->codec->sample_fmt = fs->codec->sample_fmt;
  st->codec->bits_per_coded_sample = fs->codec->bits_per_coded_sample;
  st->codec->bit_rate = fs->codec->bit_rate;

  avformat_free_context(fmt_ctx);
  av_free(avio_ctx->buffer);
  av_free(avio_ctx);
}

void FFOutputFile::adjust_codec_tag(AVStream *st)
{
  AVCodecID codec_id = st->codec->codec_id;
  unsigned int tag = st->codec->codec_tag;
  st->codec->codec_tag = 0;
  AVCodecID codec_id1 = av_codec_get_id(ofmt->oformat->codec_tag, tag);
  unsigned int codec_tag2;
  int have_codec_tag2 = av_codec_get_tag2(ofmt->oformat->codec_tag, codec_id, &codec_tag2);
  if(!ofmt->oformat->codec_tag || codec_id1==codec_id || !have_codec_tag2)
    st->codec->codec_tag = tag;

  if(ofmt->oformat->flags & AVFMT_GLOBALHEADER)
    st->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

void FFOutputFile::Write(uint32 index, uint32 flags, const void *pBuffer, uint32 cbBuffer, uint32 samples)
{
  if(!ofmt) return;

  if(!header){
    int err = 0;
    if(!(ofmt->oformat->flags & AVFMT_NOFILE)){
      err = avio_open(&ofmt->pb, out_ff_path.c_str(), AVIO_FLAG_WRITE);
      if(err<0){
        av_error(err);
        Finalize();
        return;
      }
    }

    err = avformat_write_header(ofmt, 0);
    if(err<0){
      av_error(err);
      Finalize();
      return;
    }
    header = true;
  }

  StreamInfo& s = stream[index];
  if(!s.st) return;

  AVPacket pkt;
  av_init_packet(&pkt);
  pkt.data = (uint8*)pBuffer;
  pkt.size = cbBuffer;

  if(flags & AVIIF_KEYFRAME) pkt.flags = AV_PKT_FLAG_KEY;

  pkt.pos = -1;
  pkt.stream_index = s.st->index;
  pkt.pts = s.frame;
  pkt.duration = samples;
  av_packet_rescale_ts(&pkt, s.st->codec->time_base, s.st->time_base);

  s.frame += samples;

  int err = av_interleaved_write_frame(ofmt, &pkt);
  if(err<0) av_error(err);
}

void FFOutputFile::Finalize()
{
  if(header) av_write_trailer(ofmt);

  if(ofmt && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
  avformat_free_context(ofmt);
  ofmt = 0;
}
