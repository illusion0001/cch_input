#include "InputFile2.h"
#include "FileInfo2.h"
#include "VideoSource2.h"
#include "AudioSource2.h"
#include "cineform.h"
#include "mov_mp4.h"
#include <windows.h>
#include <vfw.h>
#include <aviriff.h>

typedef struct AVCodecTag {
  enum AVCodecID id;
  unsigned int tag;
} AVCodecTag;

void init_av();

int detect_avi(const void *pHeader, int32_t nHeaderSize)
{
  if(nHeaderSize<64) return -1;
  uint8_t* data = (uint8_t*)pHeader;
  int rsize = nHeaderSize;

  RIFFCHUNK ch;
  memcpy(&ch,data,sizeof(ch)); data+=sizeof(ch); rsize-=sizeof(ch);
  if(ch.fcc!=0x46464952) return -1; //RIFF
  DWORD fmt;
  memcpy(&fmt,data,4); data+=4; rsize-=4;
  if(fmt!=0x20495641) return -1; //AVI
  memcpy(&ch,data,sizeof(ch)); data+=sizeof(ch); rsize-=sizeof(ch);
  if(ch.fcc!=0x5453494C) return -1; //LIST
  memcpy(&fmt,data,4); data+=4; rsize-=4;
  if(fmt!=0x6C726468) return -1; //hdrl

  memcpy(&ch,data,sizeof(ch));
  if(ch.fcc!=ckidMAINAVIHEADER) return -1; //avih

  if(rsize<sizeof(AVIMAINHEADER)){
    AVIMAINHEADER mh = {0};
    memcpy(&mh,data,rsize); data+=rsize; rsize=0;
    return 1;
  }

  AVIMAINHEADER mh;
  memcpy(&mh,data,sizeof(mh)); data+=sizeof(mh); rsize-=sizeof(mh);

  if(rsize<sizeof(ch)) return 1;
  memcpy(&ch,data,sizeof(ch)); data+=sizeof(ch); rsize-=sizeof(ch);
  if(ch.fcc!=0x5453494C) return -1; //LIST

  if(rsize<sizeof(fmt)) return 1;
  memcpy(&fmt,data,4); data+=4; rsize-=4;
  if(fmt!=ckidSTREAMLIST) return -1; //strl

  if(rsize<sizeof(AVISTREAMHEADER)) return 1;
  AVISTREAMHEADER sh;
  memcpy(&sh,data,sizeof(sh)); data+=sizeof(sh); rsize-=sizeof(sh);
  if(sh.fcc!=ckidSTREAMHEADER) return -1; //strh

  // reject if there is unsupported video codec
  if(sh.fccType==streamtypeVIDEO){
    init_av();
    bool have_codec = false;
    DWORD h1 = sh.fccHandler;
    DWORD h2 = sh.fccHandler;
    char* ch2 = (char*)(&h2);
    {for(int i=0; i<4; i++){
      int v = ch2[i];
      if(v>='a' && v<='z') ch2[i] = v+'A'-'a';
    }}

    // skip internally supported formats
    if(h1==MKTAG('U', 'Y', 'V', 'Y')) return -1;
    if(h1==MKTAG('Y', 'U', 'Y', 'V')) return -1;
    if(h1==MKTAG('Y', 'U', 'Y', '2')) return -1;
    if(h1==MKTAG('Y', 'V', '2', '4')) return -1;
    if(h1==MKTAG('Y', 'V', '1', '6')) return -1;
    if(h1==MKTAG('Y', 'V', '1', '2')) return -1;
    if(h1==MKTAG('I', '4', '2', '0')) return -1;
    if(h1==MKTAG('I', 'Y', 'U', 'V')) return -1;
    if(h1==MKTAG('Y', 'V', 'U', '9')) return -1;
    if(h1==MKTAG('Y', '8', ' ', ' ')) return -1;
    if(h1==MKTAG('Y', '8', '0', '0')) return -1;
    if(h1==MKTAG('H', 'D', 'Y', 'C')) return -1;
    if(h1==MKTAG('N', 'V', '1', '2')) return -1;

    if(h1==MKTAG('v', '2', '1', '0')) return -1;
    if(h1==MKTAG('b', '6', '4', 'a')) return -1;
    if(h1==MKTAG('B', 'R', 'A', 64)) return -1;
    if(h1==MKTAG('P', '2', '1', '0')) return -1;
    if(h1==MKTAG('P', '2', '1', '6')) return -1;

    AVCodecTag* riff_tag = (AVCodecTag*)avformat_get_riff_video_tags();
    while(riff_tag->id!=AV_CODEC_ID_NONE){
      if(riff_tag->tag==h1 || riff_tag->tag==h2){
        have_codec = true;
        break;
      }
      riff_tag++;
    }
    AVCodecTag* mov_tag = (AVCodecTag*)avformat_get_mov_video_tags();
    while(mov_tag->id!=AV_CODEC_ID_NONE){
      if(mov_tag->tag==h1 || mov_tag->tag==h2){
        have_codec = true;
        break;
      }
      mov_tag++;
    }

    if(!have_codec) return -1;
  }

  return 1;
}

int detect_mp4_mov(const void *pHeader, int32_t nHeaderSize, int64_t nFileSize)
{
  MovParser parser(pHeader,nHeaderSize,nFileSize);

  MovAtom a;
  if(!parser.read(a)) return -1;
  if(a.sz>nFileSize && nFileSize>0) return -1;

  switch(a.t){
  case 'wide':
  case 'ftyp':
  case 'skip':
  case 'mdat':
  case 'moov':
    return 1;
  }

  return -1;
}

void widechar_to_utf8(char *dst, int max_dst, const wchar_t *src)
{
  *dst = 0;
  WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, max_dst, 0, 0);
}

void utf8_to_widechar(wchar_t *dst, int max_dst, const char *src)
{
  *dst = 0;
  MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, max_dst);
}

bool FileExist(const wchar_t* name)
{
  DWORD a = GetFileAttributesW(name);
  return ((a!=0xFFFFFFFF) && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

//----------------------------------------------------------------------------------------------

VDFFInputFileDriver::VDFFInputFileDriver(const VDXInputDriverContext& context)
: mContext(context)
{
  select_mode = false;
}

VDFFInputFileDriver::~VDFFInputFileDriver()
{
}

int VDXAPIENTRY VDFFInputFileDriver::DetectBySignature(const void *pHeader, int32_t nHeaderSize, const void *pFooter, int32_t nFooterSize, int64_t nFileSize)
{
  if(!select_mode) return 1;
  if(detect_avi(pHeader,nHeaderSize)==1) return 1;
  if(detect_mp4_mov(pHeader,nHeaderSize,nFileSize)==1) return 1;
  return -1;
}

bool VDXAPIENTRY VDFFInputFileDriver::CreateInputFile(uint32_t flags, IVDXInputFile **ppFile)
{
  VDFFInputFile* p = new VDFFInputFile(mContext);
  if(!p) return false;

  if(flags & kOF_AutoSegmentScan) p->auto_append = true;
  //p->auto_append = true;
  if(!select_mode) p->cfg_skip_cfhd = true;

  *ppFile = p;
  p->AddRef();
  return true;
}

//----------------------------------------------------------------------------------------------

int av_initialized;

void init_av()
{
  if(!av_initialized){
    av_initialized = 1;
    av_register_all();
    avcodec_register_all();
    //av_register_cfhd();
    av_register_vfw_cfhd();
  }
}

void av_log_func(void* obj, int type, const char* msg, va_list arg)
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

VDFFInputFile::VDFFInputFile(const VDXInputDriverContext& context)
  :mContext(context)
{
  //! I wonder what happens if any other piece of system uses shared ffmpeg too
  #ifdef FFDEBUG
  //av_log_set_callback(av_log_func);
  //av_log_set_level(AV_LOG_INFO);
  //av_log_set_flags(AV_LOG_SKIP_REPEATED);
  #endif

  init_av();

  m_pFormatCtx = 0;
  video_start_time = 0;
  video_source = 0;
  audio_source = 0;
  next_segment = 0;
  head_segment = 0;
  auto_append = false;
  is_image = false;
  is_image_list = false;
  is_anim_image = false;

  cfg_frame_buffers = 40;
  cfg_skip_cfhd = false;
}

VDFFInputFile::~VDFFInputFile()
{
  if(next_segment) next_segment->Release();
  if(video_source) video_source->Release();
  if(audio_source) audio_source->Release();
  if(m_pFormatCtx) avformat_close_input(&m_pFormatCtx);	
}

void VDFFInputFile::DisplayInfo(VDXHWND hwndParent) 
{
  VDFFInputFileInfoDialog dlg;
  dlg.Show(hwndParent, this);
}

void VDFFInputFile::Init(const wchar_t *szFile, IVDXInputOptions *opts) 
{
  if(!szFile){
    mContext.mpCallbacks->SetError("No File Given");
    return;
  }

  wcscpy(path,szFile);

  //! this context instance is granted to video stream: wasted in audio-only mode
  // audio will manage its own
  m_pFormatCtx = open_file(AVMEDIA_TYPE_VIDEO);

  if(auto_append) do_auto_append(szFile);
}

void VDFFInputFile::do_auto_append(const wchar_t *szFile)
{
  const wchar_t* ext = wcsrchr(szFile,'.');
  if(!ext) return;
  if(ext-szFile<3) return;
  if(ext[-3]=='.' && ext[-2]=='0' && ext[-1]=='0'){
    int x = 1;
    while(1){
      wchar_t buf[MAX_PATH+128];
      wcsncpy(buf,szFile,ext-szFile-3); buf[ext-szFile-3]=0;
      wchar_t buf1[32];
      swprintf(buf1,32,L".%02d",x);
      wcscat(buf,buf1);
      wcscat(buf,ext);
      if(!FileExist(buf)) break;
      if(!Append(buf)) break;
      x++;
    }
  }
}

bool VDFFInputFile::test_append(VDFFInputFile* f0, VDFFInputFile* f1)
{
  int s0 = f0->find_stream(f0->m_pFormatCtx,AVMEDIA_TYPE_VIDEO);
  int s1 = f1->find_stream(f1->m_pFormatCtx,AVMEDIA_TYPE_VIDEO);
  if(s0==-1 || s1==-1) return false;
  AVStream* v0 = f0->m_pFormatCtx->streams[s0];
  AVStream* v1 = f1->m_pFormatCtx->streams[s1];
  if(v0->codecpar->width != v1->codecpar->width) return false;
  if(v0->codecpar->height != v1->codecpar->height) return false;

  s0 = f0->find_stream(f0->m_pFormatCtx,AVMEDIA_TYPE_AUDIO);
  s1 = f1->find_stream(f1->m_pFormatCtx,AVMEDIA_TYPE_AUDIO);
  AVStream* a0 = f0->m_pFormatCtx->streams[s0];
  AVStream* a1 = f1->m_pFormatCtx->streams[s1];
  if(a0->codecpar->sample_rate!=a1->codecpar->sample_rate) return false;

  return true;
}

bool VDXAPIENTRY VDFFInputFile::Append(const wchar_t* szFile)
{
  if(!szFile) return true;

  VDFFInputFile* f = new VDFFInputFile(mContext);
  f->head_segment = head_segment ? head_segment : this;
  f->Init(szFile,0);

  if(!f->m_pFormatCtx){
    delete f;
    return false;
  }

  if(!test_append(f->head_segment,f)){
    mContext.mpCallbacks->SetError("FFMPEG: Couldn't append incompatible formats.");
    delete f;
    return false;
  }

  next_segment = f;
  next_segment->AddRef();
  if(f->head_segment->video_source){
    if(f->GetVideoSource(0,0)){
      VDFFVideoSource::ConvertInfo& convertInfo = f->head_segment->video_source->convertInfo;
      f->video_source->SetTargetFormat(convertInfo.req_format,convertInfo.req_dib,f->head_segment->video_source);
      VDFFInputFile* f1 = f->head_segment;
      while(1){
        f1->video_source->m_streamInfo.mSampleCount += f->video_source->sample_count;
        f1 = f1->next_segment;
        if(!f1) break;
        if(f1==f) break;
      }
    } else {
      next_segment = 0;
      f->Release();
      return false;
    }
  }

  if(f->head_segment->audio_source){
    if(f->GetAudioSource(0,0)){
      VDFFInputFile* f1 = f->head_segment;
      while(1){
        f1->audio_source->m_streamInfo.mSampleCount += f->audio_source->sample_count;
        f1 = f1->next_segment;
        if(!f1) break;
        if(f1==f) break;
      }
    } else {
      next_segment = 0;
      f->Release();
      return false;
    }
  }

  return true;
}

AVFormatContext* VDFFInputFile::open_file(AVMediaType type, int streamIndex)
{
  const int ff_path_size = MAX_PATH*4; // utf8, worst case
  char ff_path[ff_path_size];
  widechar_to_utf8(ff_path, ff_path_size, path);

  AVFormatContext* fmt = 0;
  int err = 0; 
  err = avformat_open_input(&fmt, ff_path, 0, 0);
  if(err!=0){
    mContext.mpCallbacks->SetError("FFMPEG: Unable to open file.");
    return 0;
  }

  is_image = false;
  is_image_list = false;
  is_anim_image = false;
  if(strcmp(fmt->iformat->name,"image2")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"bmp_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"dds_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"dpx_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"exr_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"j2k_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"jpeg_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"jpegls_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pam_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pbm_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pcx_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pgmyuv_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pgm_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"pictor_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"png_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"ppm_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"psd_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"qdraw_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"sgi_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"sunrast_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"tiff_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"webp_pipe")==0) is_image=true;
  if(strcmp(fmt->iformat->name,"apng")==0) is_anim_image=true;

  AVInputFormat* fmt_image2 = av_find_input_format("image2");

  if(is_image && fmt_image2){
    wchar_t list_path[MAX_PATH];
    char start_number[MAX_PATH];
    if(detect_image_list(list_path,MAX_PATH,start_number,MAX_PATH)){
      is_image_list = true;
      avformat_close_input(&fmt);
      widechar_to_utf8(ff_path, ff_path_size, list_path);
      AVDictionary* options = 0;
      av_dict_set(&options, "start_number", start_number, 0);
      err = avformat_open_input(&fmt, ff_path, fmt_image2, &options);
      av_dict_free(&options);
      if(err!=0){
        mContext.mpCallbacks->SetError("FFMPEG: Unable to open image sequence.");
        return 0;
      }
    }
  }

  if(type==AVMEDIA_TYPE_VIDEO){
    // I absolutely do not want index getting condensed
    fmt->max_index_size = 512 * 1024 * 1024;
  }

  err = avformat_find_stream_info(fmt, 0);
  if(err<0){
    mContext.mpCallbacks->SetError("FFMPEG: Couldn't find stream information of file.");
    return 0;
  }


  int st = streamIndex;
  if(st==-1) st = find_stream(fmt,type);
  if(st!=-1){
    // disable unwanted streams
    bool is_avi = strcmp(fmt->iformat->name,"avi")==0;
    {for(int i=0; i<(int)fmt->nb_streams; i++){
      if(i==st) continue;
      // this has minor impact on performance, ~10% for video
      fmt->streams[i]->discard = AVDISCARD_ALL;

      // this is HUGE if streams are not evenly interleaved (like VD does by default)
      // this fix is hack, I dont know if it will work for other format
      if(is_avi) fmt->streams[i]->nb_index_entries = 0;
    }}
  }

  return fmt;
}

bool VDFFInputFile::detect_image_list(wchar_t* dst, int dst_count, char* start, int start_count)
{
  wchar_t* p = wcsrchr(path,'.');
  if(!p) return false;

  int digit0 = -1;
  int digit1 = -1;

  while(p>path){
    p--;
    int c = *p;
    if(c=='\\' || c=='/') break;
    if(c>='0' && c<='9'){
      if(digit0==-1){
        digit0 = p-path;
        digit1 = digit0;
      } else digit0--;
    } else if(digit0!=-1) break;
  }

  if(digit0==-1) return false;

  char* s = start;
  {for(int i=digit0; i<=digit1; i++){
    int c = path[i];
    *s = c;
    s++;
  }}
  *s = 0;

  wchar_t buf[20];
  swprintf(buf,20,L"%%0%dd",digit1-digit0+1);

  wcsncpy(dst,path,digit0);
  dst[digit0] = 0;
  wcscat(dst,buf);
  wcscat(dst,path+digit1+1);

  return true;
}

int VDFFInputFile::find_stream(AVFormatContext* fmt, AVMediaType type)
{
  int video = av_find_best_stream(fmt,AVMEDIA_TYPE_VIDEO,-1,-1,0,0);
  if(type==AVMEDIA_TYPE_VIDEO) return video;

  return av_find_best_stream(fmt,type,-1,video,0,0);
}

bool VDFFInputFile::GetVideoSource(int index, IVDXVideoSource **ppVS) 
{
  if(ppVS) *ppVS = 0;

  if(!m_pFormatCtx) return false;
  if(index!=0) return false;
 
  index = find_stream(m_pFormatCtx, AVMEDIA_TYPE_VIDEO);

  if(index < 0) return false;

  VDFFVideoSource *pVS = new VDFFVideoSource(mContext);

  if(!pVS) return false;

  if(pVS->initStream(this, index) < 0){
    delete pVS;
    return false;
  }

  video_source = pVS;
  video_source->AddRef();

  if(ppVS){
    *ppVS = pVS;
    pVS->AddRef();
  }

  if(next_segment && next_segment->GetVideoSource(0,0)){
    video_source->m_streamInfo.mSampleCount += next_segment->video_source->sample_count;
  }

  return true;
} 

bool VDFFInputFile::GetAudioSource(int index, IVDXAudioSource **ppAS)
{
  if(ppAS) *ppAS = 0;

  if(!m_pFormatCtx) return false;

  int s_index = find_stream(m_pFormatCtx, AVMEDIA_TYPE_AUDIO);
  if(index>0){
    int n = 0;
    {for(int i=0; i<(int)m_pFormatCtx->nb_streams; i++){
      AVStream* st = m_pFormatCtx->streams[i];
      if(i==s_index) continue;
      if(st->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO) continue;
      if(!st->codecpar->channels) continue;
      if(!st->codecpar->sample_rate) continue;
      n++;
      if(n==index){ s_index=i; break; }
    }}
    if(n!=index) return false;
  }

  if(s_index < 0) return false;

  VDFFAudioSource *pAS = new VDFFAudioSource(mContext);

  if(!pAS) return false;

  if(pAS->initStream(this, s_index) < 0){
    delete pAS;
    return false;
  }

  audio_source = pAS;
  audio_source->AddRef();
  
  if(ppAS){
    *ppAS = pAS;
    pAS->AddRef();
  }

  if(next_segment && next_segment->GetAudioSource(0,0)){
    audio_source->m_streamInfo.mSampleCount += next_segment->audio_source->sample_count;
  }

  return true;
}
