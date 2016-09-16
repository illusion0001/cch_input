#include "InputFile2.h"
#include "FileInfo2.h"
#include "VideoSource2.h"
#include "AudioSource2.h"
#include "cineform.h"
#include <windows.h>
#include <commdlg.h>

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

bool exportSaveFile(HWND hwnd, wchar_t* path, int max_path) {
  OPENFILENAMEW ofn = {0};
  wchar_t szFile[MAX_PATH];

  if (path)
    wcscpy(szFile,path);
  else
    szFile[0] = 0;

  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;

  wchar_t filter[256];
  wchar_t* p = wcsrchr(path,'.');
  swprintf(filter,256,L"Same as source (*%s)",p);
  int n = wcslen(filter)+1;
  filter[n] = '*'; n++;
  filter[n] = 0; wcscat(filter+n,p); n+=wcslen(p);
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
    if(!p1) wcscat(path,p);
    return true;
  }

  return false;
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

    wchar_t* p = wcsrchr(path,'.');
    wchar_t path2[MAX_PATH];
    wcsncpy(path2,path,p-path);
    path2[p-path] = 0;
    wcscat(path2,L"-01");
    wcscat(path2,p);
    if(!exportSaveFile((HWND)parent,path2,MAX_PATH)) return false;

    const int ff_path_size = MAX_PATH*4; // utf8, worst case
    char ff_path[ff_path_size];
    widechar_to_utf8(ff_path, ff_path_size, path);
    char out_ff_path[ff_path_size];
    widechar_to_utf8(out_ff_path, ff_path_size, path2);

    AVFormatContext* fmt = 0;
    AVFormatContext* ofmt = 0;
    bool v_end;
    bool a_end;
    int64_t vt_end=-1;
    int64_t at_end=-1;
    int64_t pos0,pos1;
    int video = -1;
    int audio = -1;
    AVStream* out_video=0;
    AVStream* out_audio=0;

    int err = 0; 
    err = avformat_open_input(&fmt, ff_path, 0, 0);
    if(err<0) goto end;
    err = avformat_find_stream_info(fmt, 0);
    if(err<0) goto end;

    avformat_alloc_output_context2(&ofmt, 0, 0, out_ff_path);
    if(!ofmt){ err=AVERROR_UNKNOWN; goto end; }

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
      if(ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

      out_stream->time_base = in_stream->time_base;
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

    while(1){
      if(v_end && a_end) break;

      AVPacket pkt;
      err = av_read_frame(fmt, &pkt);
      if(err<0){ err=0; break; }

      AVStream* in_stream = fmt->streams[pkt.stream_index];
      int64_t t = pkt.pts;
      if(t==AV_NOPTS_VALUE) t = pkt.dts;

      AVStream* out_stream = 0;
      if(pkt.stream_index==video){
        if(vt_end!=-1 && t>=vt_end) v_end=true; else out_stream = out_video;
      }
      if(pkt.stream_index==audio){
        if(at_end!=-1 && t>=at_end) a_end=true; else out_stream = out_audio;
        out_stream = out_audio;
      }

      if(out_stream){
        av_packet_rescale_ts(&pkt,in_stream->time_base,out_stream->time_base);
        pkt.pos = -1;
        pkt.stream_index = out_stream->index;
        err = av_interleaved_write_frame(ofmt, &pkt);
        if(err<0) break;
      }

      av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt);

end:
    avformat_close_input(&fmt);
    if(ofmt && !(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);

    if(err<0){
      char buf[1024];
      char buf2[1024];
      av_strerror(err,buf2,1024);
      strcpy(buf,"Operation failed.\nInternal error (FFMPEG): ");
      strcat(buf,buf2);
      MessageBox((HWND)parent,buf,"Stream copy",MB_ICONSTOP|MB_OK);
      return false;
    } else {
      MessageBox((HWND)parent,"Operation completed successfully.","Stream copy",MB_OK);
      return true;
    }
  }

  return false;
}
