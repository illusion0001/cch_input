#pragma once

#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/Unknown.h>


extern "C"
{
#include <libavutil/frame.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <windows.h>

class VDFFInputFile;

class VDFFVideoSource : public vdxunknown<IVDXStreamSource>, public IVDXVideoSource, public IVDXVideoDecoder, public IVDXVideoDecoderModel, public IFilterModVideoDecoder{
public:
  VDFFVideoSource(const VDXInputDriverContext& context);
  ~VDFFVideoSource();

  int VDXAPIENTRY AddRef();
  int VDXAPIENTRY Release();
  void *VDXAPIENTRY AsInterface(uint32_t iid);

  //Stream Interface
  void		VDXAPIENTRY GetStreamSourceInfo(VDXStreamSourceInfo&);
  bool		VDXAPIENTRY Read(int64_t lStart, uint32_t lCount, void *lpBuffer, uint32_t cbBuffer, uint32_t *lBytesRead, uint32_t *lSamplesRead);

  const void *VDXAPIENTRY GetDirectFormat();
  int			VDXAPIENTRY GetDirectFormatLen();

  ErrorMode VDXAPIENTRY GetDecodeErrorMode();
  void VDXAPIENTRY SetDecodeErrorMode(ErrorMode mode);
  bool VDXAPIENTRY IsDecodeErrorModeSupported(ErrorMode mode);

  bool VDXAPIENTRY IsVBR(){ return false; }
  sint64 VDXAPIENTRY TimeToPositionVBR(int64_t us){ return 0; }
  sint64 VDXAPIENTRY PositionToTimeVBR(int64_t samples){ return 0; }

  void VDXAPIENTRY GetVideoSourceInfo(VDXVideoSourceInfo& info);

  bool VDXAPIENTRY CreateVideoDecoderModel(IVDXVideoDecoderModel **ppModel);
  bool VDXAPIENTRY CreateVideoDecoder(IVDXVideoDecoder **ppDecoder);

  void		VDXAPIENTRY GetSampleInfo(int64_t sample_num, VDXVideoFrameInfo& frameInfo);

  bool		VDXAPIENTRY IsKey(int64_t lSample);
  int64_t		VDXAPIENTRY GetFrameNumberForSample(int64_t sample_num);
  int64_t		VDXAPIENTRY GetSampleNumberForFrame(int64_t display_num);
  int64_t		VDXAPIENTRY GetRealFrame(int64_t display_num);

  int64_t		VDXAPIENTRY GetSampleBytePosition(int64_t sample_num);

  //Model Interface
  void	VDXAPIENTRY Reset();
  void	VDXAPIENTRY SetDesiredFrame(int64_t frame_num);
  int64_t	VDXAPIENTRY GetNextRequiredSample(bool& is_preroll);
  int		VDXAPIENTRY GetRequiredCount();

  //Decoder Interface
  const void *VDXAPIENTRY DecodeFrame(const void *inputBuffer, uint32_t data_len, bool is_preroll, int64_t streamFrame, int64_t targetFrame);
  uint32_t	VDXAPIENTRY GetDecodePadding();
  bool		VDXAPIENTRY IsFrameBufferValid();
  const VDXPixmap& VDXAPIENTRY GetFrameBuffer();
	const FilterModPixmapInfo& VDXAPIENTRY GetFrameBufferInfo();
  bool		VDXAPIENTRY SetTargetFormat(int format, bool useDIBAlignment);
  bool		SetTargetFormat(nsVDXPixmap::VDXPixmapFormat format, bool useDIBAlignment, VDFFVideoSource* head);
  bool		VDXAPIENTRY SetDecompressedFormat(const VDXBITMAPINFOHEADER *pbih);

  const void *VDXAPIENTRY GetFrameBufferBase();
  bool		VDXAPIENTRY IsDecodable(int64_t sample_num);

public:
  //Internal
  const VDXInputDriverContext&	mContext;
  VDFFInputFile* m_pSource;
  int m_streamIndex;
  AVFormatContext* m_pFormatCtx;
  AVStream*	m_pStreamCtx;
  AVCodecContext* m_pCodecCtx;
  VDXStreamSourceInfo	m_streamInfo;
  AVRational time_base;
  int64_t start_time;

  AVFrame* frame;
  SwsContext* m_pSwsCtx;
  VDXPixmap	m_pixmap;
  FilterModPixmapInfo m_pixmap_info;
  uint8_t* m_pixmap_data;
  int m_pixmap_frame;

  struct ConvertInfo{
    nsVDXPixmap::VDXPixmapFormat req_format;
    bool req_dib;

    AVPixelFormat av_fmt;
    bool direct_copy;
    bool in_yuv;
    bool in_subs;
    bool out_rgb;
    bool out_garbage;
  } convertInfo;

  ErrorMode CurrentDecoderErrorMode;

  struct BufferPage{
    int i;
    int target;
    int refs;
    volatile LONG access;
    void* map_base;
    uint8_t* p;
  };
  BufferPage* buffer;
  int buffer_count;
  int buffer_reserve;
  HANDLE mem;

  int sample_count;
  BufferPage** frame_array;
  char* frame_type;
  int64_t desired_frame;
  int required_count;
  int next_frame;
  int first_frame;
  int last_frame;
  int used_frames;
  int keyframe_gap;
  int fw_seek_threshold;
  int frame_size;

  bool flip_image;
  bool trust_index;
  bool direct_buffer;
  bool is_image_list;
  int64_t dead_range_start;
  int64_t dead_range_end;

  uint64 kPixFormat_XRGB64;

  int	 initStream(VDFFInputFile* pSource, int indexStream);
  void set_pixmap_layout(uint8_t* p);
  void handle_frame();
  void set_start_time();
  bool read_frame(bool init=false);
  void alloc_direct_buffer();
  void alloc_page(int pos);
  void open_page(BufferPage* p, int flag);
  void open_read(BufferPage* p){ open_page(p,1); }
  void open_write(BufferPage* p){ open_page(p,2); }
  void copy(int start, int end, BufferPage* p);
  int64_t frame_to_pts_next(sint64 start);
};
