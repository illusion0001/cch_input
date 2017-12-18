#ifndef export_header
#define export_header

#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/Unknown.h>
#include <string>
#include <vector>

uint32 export_avi_fcc(AVStream* src);

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

  void copy(const void* data, int size){
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
    t->pos += buf_size;
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

class FFOutputFile: public  vdxunknown<IVDXOutputFile>{
public:
  const VDXInputDriverContext &mContext;

  struct StreamInfo{
    AVStream* st;
    int64_t frame;
    AVRational time_base;

    StreamInfo(){
      st = 0;
      frame = 0;
      time_base.den = 0;
      time_base.num = 0;
    }
  };

  std::string out_ff_path;
  std::vector<StreamInfo> stream;
  AVFormatContext* ofmt;
  bool header;

  FFOutputFile(const VDXInputDriverContext &pContext);
  ~FFOutputFile();
  void VDXAPIENTRY Init(const wchar_t *path, const char* format);
  uint32 VDXAPIENTRY CreateStream(int type);
  void VDXAPIENTRY SetVideo(uint32 index, const VDXStreamInfo& si, const void *pFormat, int cbFormat);
  void VDXAPIENTRY SetAudio(uint32 index, const VDXStreamInfo& si, const void *pFormat, int cbFormat);
  void VDXAPIENTRY Write(uint32 index, const void *pBuffer, uint32 cbBuffer, PacketInfo& info);
  void Finalize();
  void av_error(int err);
  void adjust_codec_tag(AVStream *st);
  void import_bmp(AVStream *st, const void *pFormat, int cbFormat);
  void import_wav(AVStream *st, const void *pFormat, int cbFormat);
  bool test_header();
};

class VDFFOutputFileDriver: public vdxunknown<IVDXOutputFileDriver>{
public:
  const VDXInputDriverContext &mpContext;

  VDFFOutputFileDriver(const VDXInputDriverContext &pContext)
    :mpContext(pContext)
  {
  }

  virtual bool  VDXAPIENTRY GetStreamControl(const wchar_t *path, const char* format, VDXStreamControl& sc);

  virtual bool	VDXAPIENTRY CreateOutputFile(IVDXOutputFile **ppFile) {
    *ppFile = new FFOutputFile(mpContext);
    return true;
  }

  virtual bool	VDXAPIENTRY EnumFormats(int i, wchar_t* filter, wchar_t* ext, char* name) {
    switch(i){
    case 0:
      wcscpy(filter,L"AVI handled by FFMPEG (*.avi)");
      wcscpy(ext,L"*.avi");
      strcpy(name,"avi");
      return true;
    case 1:
      wcscpy(filter,L"Matroska (*.mkv)");
      wcscpy(ext,L"*.mkv");
      strcpy(name,"matroska");
      return true;
      /*
    case 2:
      wcscpy(filter,L"WebM (*.webm)");
      wcscpy(ext,L"*.webm");
      strcpy(name,"webm");
      return true;
      */
    case 2:
      wcscpy(filter,L"QuickTime / MOV (*.mov)");
      wcscpy(ext,L"*.mov");
      strcpy(name,"mov");
      return true;
    case 3:
      wcscpy(filter,L"MP4 (MPEG-4 Part 14) (*.mp4)");
      wcscpy(ext,L"*.mp4");
      strcpy(name,"mp4");
      return true;
    case 4:
      wcscpy(filter,L"NUT (*.nut)");
      wcscpy(ext,L"*.nut");
      strcpy(name,"nut");
      return true;
    case 5:
      wcscpy(filter,L"any format by FFMPEG (*.*)");
      wcscpy(ext,L"*.*");
      strcpy(name,"");
      return true;
    }
    return false;
  }
};

extern VDXPluginInfo ff_output_info;

#endif
