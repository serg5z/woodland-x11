#include "../core/x11_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
typedef struct { uint16_t mem[W * H]; unsigned long reads, writes, fills, pixels_read, pixels_written; } sim;
static uint32_t getp(void *v, int x, int y, int depth) {
  sim *s = (sim *)v; (void)depth; ++s->reads; ++s->pixels_read; return s->mem[y * W + x];
}
static void putp(void *v, int x, int y, int depth, uint32_t p) {
  sim *s = (sim *)v; (void)depth; ++s->writes; ++s->pixels_written; s->mem[y * W + x] = (uint16_t)p;
}
static void fillp(void *v, int x, int y, int w, int h, uint32_t p, int depth) {
  sim *s = (sim *)v; (void)depth; ++s->fills; s->pixels_written += (unsigned long)w * (unsigned long)h;
  for (int yy=y; yy<y+h; ++yy) for (int xx=x; xx<x+w; ++xx) s->mem[yy*W+xx]=(uint16_t)p;
}
static void readsp(void *v,int x,int y,int n,uint16_t *d) {
  sim *s=(sim *)v; ++s->reads; s->pixels_read+=(unsigned long)n; memcpy(d,&s->mem[y*W+x],(size_t)n*2);
}
static void writesp(void *v,int x,int y,int n,const uint16_t *p) {
  sim *s=(sim *)v; ++s->writes; s->pixels_written+=(unsigned long)n; memcpy(&s->mem[y*W+x],p,(size_t)n*2);
}
static const xr_surface_ops ops={getp,putp,fillp,readsp,writesp};
static uint16_t ram[W*H]; static sim gr;
static xr_surface a={ram,W,H,W,16,0,0,0,0}, b={0,W,H,W,16,0,&ops,&gr,0};
static int fail;
#define CHECK(q) do { if (!(q)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#q); ++fail; } } while(0)
static uint16_t pat(int x,int y) { return (uint16_t)((x*73+y*151+(x^y)*31) & 65535); }
static void sync_initial(void) { for(int i=0;i<W*H;++i) ram[i]=gr.mem[i]=pat(i%W,i/W); }
static int equal(void) { for(int i=0;i<W*H;++i) if(ram[i]!=gr.mem[i]) {fprintf(stderr,"pixel %d,%d RAM=%04x GRAM=%04x\n",i%W,i/W,ram[i],gr.mem[i]);return 0;} return 1; }
int main(void) {
  xr_region clip; xr_region_set(&clip,3,5,317,235); xr_paint p={3,0xffff,0x5a3c,0};
  sync_initial(); xr_fill_rect(&a,&clip,&p,-10,20,300,80); xr_fill_rect(&b,&clip,&p,-10,20,300,80); CHECK(equal());
  uint16_t image[96]; for (int i=0;i<96;++i) image[i]=(uint16_t)(i*521u+0x37a1u);
  p.function=3; p.plane_mask=0xffff;
  xr_put_span(&a,&clip,&p,-7,101,image,96); xr_put_span(&b,&clip,&p,-7,101,image,96); CHECK(equal());
  /* Copy on the same drawable in all overlap directions, with clipped destination. */
  const int cases[][4]={{10,10,15,12},{15,12,10,10},{40,40,42,42},{1,1,0,0}};
  for(size_t k=0;k<sizeof(cases)/sizeof(cases[0]);++k) {
    sync_initial(); int sx=cases[k][0],sy=cases[k][1],dx=cases[k][2],dy=cases[k][3];
    xr_copy(&a,&clip,&p,&a,sx,sy,50,35,dx,dy); xr_copy(&b,&clip,&p,&b,sx,sy,50,35,dx,dy); CHECK(equal());
  }
  /* Every GX raster op and masked writes (including reads from non-addressable GRAM). */
  for(int fn=0;fn<16;++fn) for(int m=0;m<4;++m) {
    sync_initial(); p.function=(uint8_t)fn; p.plane_mask=m==0?0xffffu:m==1?0xf00fu:m==2?0x00ffu:0;
    xr_fill_rect(&a,&clip,&p,7,9,91,43); xr_fill_rect(&b,&clip,&p,7,9,91,43); CHECK(equal());
    xr_copy(&a,&clip,&p,&a,4,6,71,29,18,14); xr_copy(&b,&clip,&p,&b,4,6,71,29,18,14); CHECK(equal());
  }
  printf("GRAM drawable differential: %s; callbacks read=%lu (%lu px), write=%lu (%lu px), fill=%lu\n",
         fail?"FAIL":"PASS",gr.reads,gr.pixels_read,gr.writes,gr.pixels_written,gr.fills);
  return fail?1:0;
}
