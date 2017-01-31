#ifndef a_compress_header
#define a_compress_header

#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/Unknown.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include "libavutil/audio_fifo.h"
#include <libswresample/swresample.h>
}
#include <windows.h>
#include <mmreg.h>

class VDFFAudio: public vdxunknown<IVDXAudioEnc>{
public:
  const VDXInputDriverContext &mContext;

  enum {flag_constant_rate=1};

  struct Config{
    int version;
    int quality;
    int bitrate;
    int flags;

    Config(){ clear(); }
    void clear(){ version=0; quality=0; bitrate=0; flags=0; }
  }* config;

  AVCodec* codec;
  AVCodecContext* ctx;
  AVFrame* frame;
  SwrContext* swr;
  uint8_t** sample_buf;
  uint8_t* in_buf;
  unsigned frame_size;
  int frame_pos;
  unsigned in_pos;
  int src_linesize;
  AVPacket pkt;
  sint64 total_in;
  sint64 total_out;

  WAVEFORMATEXTENSIBLE* out_format;
  int out_format_size;

  VDFFAudio(const VDXInputDriverContext &pContext);
  ~VDFFAudio();
  void cleanup();

  void export_wav();

  virtual void CreateCodec() = 0;
  virtual void InitContext();
  virtual void reset_config(){ config->clear(); }

  virtual bool HasAbout(){ return false; }
  virtual bool HasConfig(){ return false; }
  virtual void ShowAbout(VDXHWND parent){}
  virtual void ShowConfig(VDXHWND parent){}
  virtual size_t GetConfigSize(){ return sizeof(Config); }
  virtual void* GetConfig(){ return config; }
  virtual void SetConfig(void* data, size_t size);

  virtual void SetInputFormat(VDXWAVEFORMATEX* format);
  virtual void Shutdown(){}

  virtual bool IsEnded() const { return false; }

  virtual unsigned	GetInputLevel() const;
  virtual unsigned	GetInputSpace() const;
  virtual unsigned	GetOutputLevel() const;
  virtual const VDXWAVEFORMATEX *GetOutputFormat() const { return (VDXWAVEFORMATEX*)out_format; }
  virtual unsigned	GetOutputFormatSize() const { return out_format_size; }

  virtual void		Restart(){}
  virtual bool		Convert(bool flush, bool requireOutput);

  virtual void		*LockInputBuffer(unsigned& bytes);
  virtual void		UnlockInputBuffer(unsigned bytes);
  virtual const void	*LockOutputBuffer(unsigned& bytes);
  virtual void		UnlockOutputBuffer(unsigned bytes);
  virtual unsigned	CopyOutput(void *dst, unsigned bytes, sint64& duration);
};

class VDFFAudio_aac: public VDFFAudio{
public:
  struct Config:public VDFFAudio::Config{
  } codec_config;

  VDFFAudio_aac(const VDXInputDriverContext &pContext):VDFFAudio(pContext){
    config = &codec_config;
    reset_config();
  }
  virtual void CreateCodec();
  virtual size_t GetConfigSize(){ return sizeof(Config); }
  virtual void reset_config();
  virtual bool HasConfig(){ return true; }
  virtual void ShowConfig(VDXHWND parent);
};

class VDFFAudio_mp3: public VDFFAudio{
public:
  enum {flag_jointstereo=2};
  struct Config:public VDFFAudio::Config{
  } codec_config;

  VDFFAudio_mp3(const VDXInputDriverContext &pContext):VDFFAudio(pContext){
    config = &codec_config;
    reset_config();
  }
  virtual void CreateCodec();
  virtual void InitContext();
  virtual size_t GetConfigSize(){ return sizeof(Config); }
  virtual void reset_config();
  virtual bool HasConfig(){ return true; }
  virtual void ShowConfig(VDXHWND parent);
};

extern VDXPluginInfo ff_aacenc_info;
extern VDXPluginInfo ff_mp3enc_info;

#endif
