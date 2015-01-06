#ifndef cineform_header
#define cineform_header

struct AVCodecContext;

void av_register_cfhd();
void av_register_vfw_cfhd();
void cfhd_set_buffer(AVCodecContext* avctx, void* buf);

#endif cineform_header