#include "fflayer.h"

void LogoFilter::render_xrgb8()
{
  const VDXPixmap& src = *fa->src.mpPixmap;
  const VDXPixmap& dst = *fa->dst.mpPixmap;

  const VDXPixmap& layer = video->GetFrameBuffer();
  const FilterModPixmapInfo& layerInfo = video->GetFrameBufferInfo();

  int x1 = param.pos_x;
  int y1 = param.pos_y;
  int x2 = 0;
  int y2 = 0;
  int w = layer.w;
  int h = layer.h;

	if (x1 < 0) { x2 -= x1; w += x1; x1 = 0; }
	if (y1 < 0) { y2 -= y1; h += y1; y1 = 0; }
	if (x2 < 0) { x1 -= x2; w += x2; x2 = 0; }
	if (y2 < 0) { y1 -= y2; h += y2; y2 = 0; }
	if (x1+w > src.w) { w = src.w - x1; }
	if (y1+h > src.h) { h = src.h - y1; }
	if (x2+w > layer.w) { w = layer.w - x2; }
	if (y2+h > layer.h) { h = layer.h - y2; }

  if(w<=0 || h<=0) return;

  int blendMode = param.blendMode;
  if(layerInfo.alpha_type==0) blendMode = LogoParam::blend_replace;

  {for(int y=0; y<h; y++){
    unsigned char* d = (unsigned char*)dst.data + (y+y1)*dst.pitch + x1*4;
    unsigned char* s = (unsigned char*)layer.data + (y+y2)*layer.pitch + x2*4;
    if(blendMode==LogoParam::blend_replace){for(int x=0; x<w; x++){
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];

      s += 4;
      d += 4;
    }}

    if(blendMode==LogoParam::blend_alpha){for(int x=0; x<w; x++){
      int a = s[3];
      int ra = 255-a;
      d[0] = ((s[0]*a + d[0]*ra)*257)>>16;
      d[1] = ((s[1]*a + d[1]*ra)*257)>>16;
      d[2] = ((s[2]*a + d[2]*ra)*257)>>16;
      d[3] = s[3];

      s += 4;
      d += 4;
    }}

    if(blendMode==LogoParam::blend_alpha_pm){for(int x=0; x<w; x++){
      int ra = (255-s[3])*257;
      int c0 = s[0] + ((d[0]*ra)>>16);
      int c1 = s[1] + ((d[1]*ra)>>16);
      int c2 = s[2] + ((d[2]*ra)>>16);
      if(c0>255) c0 = 255;
      if(c1>255) c1 = 255;
      if(c2>255) c2 = 255;
      d[0] = c0;
      d[1] = c1;
      d[2] = c2;
      d[3] = s[3];

      s += 4;
      d += 4;
    }}
  }}
}
