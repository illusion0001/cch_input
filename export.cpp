#include "InputFile2.h"
#include "FileInfo2.h"
#include "VideoSource2.h"
#include "AudioSource2.h"
#include "cineform.h"
#include <string>
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include "resource.h"

#pragma warning(disable:4996)

extern HINSTANCE hInstance;

void utf8_to_widechar(wchar_t *dst, int max_dst, const char *src);
void widechar_to_utf8(char *dst, int max_dst, const wchar_t *src);

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
