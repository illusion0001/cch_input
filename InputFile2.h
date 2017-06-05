#pragma once

#include <stdint.h>
#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/Unknown.h>

extern "C"
{
#include <libavformat/avformat.h>
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class VDFFVideoSource;
class VDFFAudioSource;

class VDFFInputFileDriver : public vdxunknown<IVDXInputFileDriver> {
public:

  enum OpenFlags {
    kOF_None			= 0,
    kOF_Quiet			= 1,
    kOF_AutoSegmentScan	= 2,
    kOF_SingleFile	= 4,
    kOF_Max				= 0xFFFFFFFFUL
  };

  enum FileFlags {
    kFF_Sequence = 1,
  };

  VDFFInputFileDriver(const VDXInputDriverContext& context);
  ~VDFFInputFileDriver();

  int		VDXAPIENTRY DetectBySignature(const void *pHeader, int32_t nHeaderSize, const void *pFooter, int32_t nFooterSize, int64_t nFileSize);
  int		VDXAPIENTRY DetectBySignature2(VDXMediaInfo& info, const void *pHeader, int32_t nHeaderSize, const void *pFooter, int32_t nFooterSize, int64_t nFileSize);
  bool	VDXAPIENTRY CreateInputFile(uint32_t flags, IVDXInputFile **ppFile);

protected:
  const VDXInputDriverContext& mContext;
};

class VDFFInputFileOptions : public vdxunknown<IVDXInputOptions> {
public:
  enum { opt_version=1 };

#pragma pack(push,1)
  struct Data {
    int version;
    bool skip_cfhd_vfw;

    Data(){ version=opt_version; skip_cfhd_vfw=false; }
  } data;
#pragma pack(pop)

  bool Read(const void *src, uint32_t len) {
    if(len!=sizeof(Data)) return false;
    memcpy(&data,src,len);
    if(data.version!=opt_version) return false;
    return true;
  }
  uint32_t VDXAPIENTRY Write(void *buf, uint32_t buflen) {
    if(buflen<sizeof(Data) || !buf) return sizeof(Data);
    memcpy(buf,&data,sizeof(Data));
    return sizeof(Data);
  }
};

class VDFFInputFile : public vdxunknown<IVDXInputFile>, public IFilterModFileTool{
public:
  int64_t video_start_time;
  wchar_t path[MAX_PATH];
  bool auto_append;
  bool single_file_mode;

  bool is_image_list;
  bool is_image;
  bool is_anim_image;

  int cfg_frame_buffers;
  bool cfg_skip_cfhd;

  AVFormatContext* m_pFormatCtx;
  VDFFVideoSource* video_source;
  VDFFAudioSource* audio_source;
  VDFFInputFile* next_segment;
  VDFFInputFile* head_segment;

  int VDXAPIENTRY AddRef() {
    return vdxunknown<IVDXInputFile>::AddRef();
  }

  int VDXAPIENTRY Release() {
    return vdxunknown<IVDXInputFile>::Release();
  }

  void* VDXAPIENTRY AsInterface(uint32_t iid)
  {
    if (iid == IFilterModFileTool::kIID)
      return static_cast<IFilterModFileTool*>(this);

    return vdxunknown<IVDXInputFile>::AsInterface(iid);
  }

  VDFFInputFile(const VDXInputDriverContext& context);
  ~VDFFInputFile();

  void VDXAPIENTRY Init(const wchar_t *szFile, IVDXInputOptions *opts);
  bool VDXAPIENTRY Append(const wchar_t *szFile);

  bool VDXAPIENTRY PromptForOptions(VDXHWND, IVDXInputOptions **r);
  bool VDXAPIENTRY CreateOptions(const void *buf, uint32_t len, IVDXInputOptions **r);
  void VDXAPIENTRY DisplayInfo(VDXHWND hwndParent);

  bool VDXAPIENTRY GetVideoSource(int index, IVDXVideoSource **);
  bool VDXAPIENTRY GetAudioSource(int index, IVDXAudioSource **);
  bool VDXAPIENTRY GetExportMenuInfo(int id, char* name, int name_size, bool* enabled);
  bool VDXAPIENTRY GetExportCommandName(int id, char* name, int name_size);
  bool VDXAPIENTRY ExecuteExport(int id, VDXHWND parent, IProjectState* state);
  int VDXAPIENTRY GetFileFlags();

public:
  AVFormatContext* getContext( void ) { return m_pFormatCtx; }
  int find_stream(AVFormatContext* fmt, AVMediaType type);
  AVFormatContext* open_file(AVMediaType type, int streamIndex=-1);
  bool detect_image_list(wchar_t* dst, int dst_count, char* start, int start_count);
  void do_auto_append(const wchar_t *szFile);

protected:
  const VDXInputDriverContext& mContext;
  static bool test_append(VDFFInputFile* f0, VDFFInputFile* f1);
};
