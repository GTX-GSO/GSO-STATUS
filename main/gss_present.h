#pragma once
/* Which parts of the portrait panel framebuffers must be rewritten for a landscape LVGL frame.
 *
 * LVGL renders the full 1280x452 picture into one buffer (DIRECT mode) and flush converts it into one of the two DPI
 * framebuffers, rotated. The framebuffer written next still holds the picture from two frames ago, so a frame must
 * convert the area it changed plus the area the previous frame changed; everything else is already identical.
 * After a flip (the panel is rotated 180 degrees) or at start-up both buffers are stale and two full frames resync them.
 * Pure C with no ESP-IDF types so tests/present_test.c can check it against a reference rotation on the host. */
#include <stdbool.h>
#include <stdint.h>

#define GSS_PRESENT_MAX_RECTS 12u
#define GSS_PRESENT_MAX_PLAN (2u*GSS_PRESENT_MAX_RECTS)

typedef struct {int x1,y1,x2,y2;} gss_rect; /* landscape pixels, inclusive */
typedef struct {gss_rect rect[GSS_PRESENT_MAX_RECTS];unsigned count;} gss_dirty_list;
typedef struct {gss_dirty_list previous,current;unsigned full_frames;bool flipped;} gss_present_state;
/* One PPA operation: block of the landscape picture and where its rotated copy lands in the portrait framebuffer. */
typedef struct {uint32_t in_x,in_y,in_w,in_h,out_x,out_y;} gss_block;

static inline void gss_present_init(gss_present_state *s){
 s->previous.count=s->current.count=0;
 s->full_frames=2;
 s->flipped=false;
}
/* Both framebuffers are stale: the next two frames convert everything. */
static inline void gss_present_invalidate(gss_present_state *s){s->full_frames=2;}
static inline int gss_rect_min(int a,int b){return a<b?a:b;}
static inline int gss_rect_max(int a,int b){return a>b?a:b;}
static inline gss_rect gss_rect_union(gss_rect a,gss_rect b){
 gss_rect r={gss_rect_min(a.x1,b.x1),gss_rect_min(a.y1,b.y1),gss_rect_max(a.x2,b.x2),gss_rect_max(a.y2,b.y2)};
 return r;
}
static inline gss_rect gss_rect_clamp(gss_rect r,int width,int height){
 r.x1=gss_rect_max(r.x1,0);r.y1=gss_rect_max(r.y1,0);
 r.x2=gss_rect_min(r.x2,width-1);r.y2=gss_rect_min(r.y2,height-1);
 return r;
}
static inline int64_t gss_rect_area(gss_rect r){return (int64_t)(r.x2-r.x1+1)*(r.y2-r.y1+1);}
/* Records one area LVGL changed in the current frame; too many separate areas collapse into their bounding box. */
static inline void gss_present_add(gss_present_state *s,gss_rect r,int width,int height){
 r=gss_rect_clamp(r,width,height);
 if(r.x1>r.x2||r.y1>r.y2)return;
 gss_dirty_list *list=&s->current;
 if(list->count==GSS_PRESENT_MAX_RECTS){
  gss_rect all=list->rect[0];
  for(unsigned i=1;i<list->count;i++)all=gss_rect_union(all,list->rect[i]);
  list->rect[0]=all;list->count=1;
 }
 list->rect[list->count++]=r;
}
/* Fills out[] (capacity GSS_PRESENT_MAX_PLAN) with the areas to convert for the frame just rendered, then makes the
   current areas the previous ones. A single full-frame area is returned for the resync frames. When the separate areas
   already cover most of the picture they are converted as their bounding box in one operation: dragging the card strip
   changes 75% of the screen, and the sidebar beside it is not converted again. */
static inline unsigned gss_present_plan(gss_present_state *s,bool flipped,gss_rect *out,int width,int height){
 if(flipped!=s->flipped){s->flipped=flipped;s->full_frames=2;}
 gss_rect full={0,0,width-1,height-1};
 unsigned count=0;
 bool everything=s->full_frames>0;
 if(!everything){
  int64_t covered=0;
  for(unsigned i=0;i<s->previous.count;i++){out[count++]=s->previous.rect[i];covered+=gss_rect_area(s->previous.rect[i]);}
  for(unsigned i=0;i<s->current.count;i++){out[count++]=s->current.rect[i];covered+=gss_rect_area(s->current.rect[i]);}
  everything=count==0;
  if(!everything&&covered*5>=gss_rect_area(full)*3){
   gss_rect all=out[0];
   for(unsigned i=1;i<count;i++)all=gss_rect_union(all,out[i]);
   out[0]=all;count=1;
  }
 }
 if(everything){out[0]=full;count=1;}
 if(s->full_frames>0)s->full_frames--;
 s->previous=s->current;
 s->current.count=0;
 return count;
}
/* The panel is portrait (452x1280); the picture is rotated 270 degrees counterclockwise normally and 90 when flipped
   (PPA angles are counterclockwise). A landscape pixel (x,y) lands at (H-1-y, x) normally and at (y, W-1-x) flipped. */
static inline gss_block gss_panel_block(gss_rect r,bool flipped,int width,int height){
 gss_block b={(uint32_t)r.x1,(uint32_t)r.y1,(uint32_t)(r.x2-r.x1+1),(uint32_t)(r.y2-r.y1+1),0,0};
 if(flipped){b.out_x=(uint32_t)r.y1;b.out_y=(uint32_t)(width-1-r.x2);}
 else{b.out_x=(uint32_t)(height-1-r.y2);b.out_y=(uint32_t)r.x1;}
 return b;
}
