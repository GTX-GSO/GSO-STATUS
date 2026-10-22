#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/gss_present.h"

/* Small panel so every frame can be compared pixel by pixel: 40 wide, 18 high landscape, 18x40 portrait. */
#define W 40
#define H 18
static uint32_t image[W*H];       /* the landscape picture LVGL keeps complete */
static uint32_t panel[2][H*W];    /* the two portrait framebuffers, pixel (px,py) at py*H+px */
static int failures;
static void check(int ok,const char *what){if(!ok){printf("FAIL: %s\n",what);failures++;}}

/* What PPA does for one block: copy the block of the landscape picture, rotated, to the panel at the given offset. */
static void ppa_block(uint32_t *target,gss_block b,bool flipped){
 for(uint32_t y=0;y<b.in_h;y++)for(uint32_t x=0;x<b.in_w;x++){
  uint32_t value=image[(b.in_y+y)*W+b.in_x+x];
  /* output block geometry follows from the rotation: normal = 270 CCW, flipped = 90 CCW */
  uint32_t ox=flipped?y:b.in_h-1-y,oy=flipped?b.in_w-1-x:x;
  target[(b.out_y+oy)*H+b.out_x+ox]=value;
 }
}
/* Independent definition of the full rotation, from the pixel formulas rather than block offsets. */
static uint32_t expected(int px,int py,bool flipped){
 /* normal: (x,y) -> (H-1-y, x); flipped: (x,y) -> (y, W-1-x) */
 int x,y;
 if(flipped){y=px;x=W-1-py;}else{y=H-1-px;x=py;}
 return image[y*W+x];
}
static int panel_matches(int which,bool flipped){
 for(int py=0;py<W;py++)for(int px=0;px<H;px++)if(panel[which][py*H+px]!=expected(px,py,flipped))return 0;
 return 1;
}
static unsigned seed=12345;
static int next(int n){seed=seed*1103515245u+12345u;return (int)((seed>>16)%(unsigned)n);}

/* One frame: change some rectangles of the picture, report them as LVGL does, then present like flush(). */
static int frame(gss_present_state *s,unsigned *shown,bool flipped,int rects,int max_size,uint32_t tag){
 for(int i=0;i<rects;i++){
  int w=1+next(max_size),h=1+next(max_size),x=next(W-w+1),y=next(H-h+1);
  for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)image[yy*W+xx]=tag*1000u+(uint32_t)(yy*W+xx);
  gss_present_add(s,(gss_rect){x,y,x+w-1,y+h-1},W,H);
 }
 gss_rect plan[GSS_PRESENT_MAX_PLAN];
 unsigned count=gss_present_plan(s,flipped,plan,W,H);
 unsigned back=*shown^1u;
 for(unsigned i=0;i<count;i++)ppa_block(panel[back],gss_panel_block(plan[i],flipped,W,H),flipped);
 *shown=back;
 return panel_matches((int)back,flipped);
}
int main(void){
 /* a landscape pixel's rotated position follows the documented formulas */
 {gss_block normal=gss_panel_block((gss_rect){3,5,9,8},false,W,H),flipped=gss_panel_block((gss_rect){3,5,9,8},true,W,H);
  check(normal.out_x==(uint32_t)(H-1-8)&&normal.out_y==3&&normal.in_w==7&&normal.in_h==4,"normal block offset");
  check(flipped.out_x==5&&flipped.out_y==(uint32_t)(W-1-9)&&flipped.in_w==7&&flipped.in_h==4,"flipped block offset");}
 for(int i=0;i<W*H;i++)image[i]=7000u+(uint32_t)i;
 memset(panel,0,sizeof(panel));
 gss_present_state s;gss_present_init(&s);unsigned shown=1;int ok=1;
 /* start-up: two resync frames build both buffers from a stale state */
 ok&=frame(&s,&shown,false,1,3,1);
 ok&=frame(&s,&shown,false,1,3,2);
 check(ok,"resync frames give a correct picture");
 /* small incremental frames: only the union of this and the previous change is converted */
 int incremental=1;
 for(unsigned n=0;n<400;n++)incremental&=frame(&s,&shown,false,1+next(3),6,3+n);
 check(incremental,"400 incremental frames match a full conversion");
 /* flipping rotates both buffers; two resync frames follow even though only a small area changed */
 int flipping=1;
 bool flipped=false;
 for(unsigned n=0;n<200;n++){
  if(next(6)==0)flipped=!flipped;
  flipping&=frame(&s,&shown,flipped,1+next(2),5,500+n);
 }
 check(flipping,"frames stay correct while the flip state toggles at random");
 /* many separate areas collapse into a bounding box instead of overflowing */
 int busy=1;
 for(unsigned n=0;n<50;n++)busy&=frame(&s,&shown,flipped,GSS_PRESENT_MAX_RECTS+5,4,900+n);
 check(busy,"more separate areas than the list holds still convert correctly");
 /* large changes fall back to one full conversion */
 {gss_present_state t;gss_present_init(&t);t.full_frames=0;gss_rect plan[GSS_PRESENT_MAX_PLAN];
  gss_present_add(&t,(gss_rect){0,0,W-1,H-1},W,H);
  check(gss_present_plan(&t,false,plan,W,H)==1&&plan[0].x2==W-1&&plan[0].y2==H-1,"a full-screen change is one full area");
  gss_present_add(&t,(gss_rect){2,2,4,4},W,H);
  unsigned count=gss_present_plan(&t,false,plan,W,H);
  check(count==1,"a small change after a full frame converts both (covered area is large, so one full area)");
  gss_present_add(&t,(gss_rect){2,2,3,3},W,H);
  count=gss_present_plan(&t,false,plan,W,H);
  check(count==2,"two small areas stay separate");
  gss_rect strip={7,1,W-1,H-2}; /* like the card strip: three quarters of the screen beside the sidebar */
  gss_present_add(&t,strip,W,H);gss_present_plan(&t,false,plan,W,H);
  gss_present_add(&t,strip,W,H);count=gss_present_plan(&t,false,plan,W,H);
  check(count==1&&plan[0].x1==7&&plan[0].x2==W-1&&plan[0].y1==1&&plan[0].y2==H-2,"a large repeated change converts its bounding box, not the whole screen");}
 /* areas outside the picture are clamped or ignored */
 {gss_present_state t;gss_present_init(&t);t.full_frames=0;gss_rect plan[GSS_PRESENT_MAX_PLAN];
  gss_present_add(&t,(gss_rect){-5,-5,2,2},W,H);gss_present_add(&t,(gss_rect){W+3,1,W+9,4},W,H);
  unsigned count=gss_present_plan(&t,false,plan,W,H);
  check(count==1&&plan[0].x1==0&&plan[0].y1==0&&plan[0].x2==2&&plan[0].y2==2,"areas are clamped to the picture and off-screen ones dropped");}
 printf(failures?"present tests FAILED\n":"PASS: partial presentation keeps both framebuffers correct\n");
 return failures?1:0;
}
