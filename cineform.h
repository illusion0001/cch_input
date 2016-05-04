#ifndef cineform_header
#define cineform_header

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

struct AVCodecContext;

const AVCodecID CFHD_ID = (AVCodecID)MKBETAG('C','F','H','D');
const unsigned CFHD_TAG = MKTAG('C', 'F', 'H', 'D');

void av_register_cfhd();
void av_register_vfw_cfhd();
void cfhd_set_buffer(AVCodecContext* avctx, void* buf);

#endif cineform_header