/* A single short-lived PSRAM snapshot replaces complex retiring panel trees. */
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
/* The card strip freeze (up to five 334x420 cards) uses this budget in PSRAM. */
#define TRANSITION_CACHE_LIMIT (3u*1024u*1024u)
typedef struct {lv_draw_buf_t draw;void *pixels;uint32_t bytes;} transition_cache;
static uint32_t transition_cache_bytes,transition_cache_peak;
#ifdef GSO_TEST
static uint32_t transition_test_limit=TRANSITION_CACHE_LIMIT;
void desk_ui_test_cache_limit(uint32_t bytes){transition_test_limit=bytes;}
#endif
uint32_t desk_ui_animation_cache_bytes(void){return transition_cache_bytes;}
uint32_t desk_ui_animation_cache_peak_bytes(void){return transition_cache_peak;}
static void transition_pixels_free(void *pixels){
#ifdef ESP_PLATFORM
 heap_caps_free(pixels);
#else
 lv_free(pixels);
#endif
}
static void transition_cache_deleted(lv_event_t *event){
 transition_cache *cache=lv_event_get_user_data(event);
 lv_image_cache_drop(&cache->draw);transition_cache_bytes-=cache->bytes;
 transition_pixels_free(cache->pixels);lv_free(cache);
}
/* The first ancestor with an opaque background: what shows through a card's rounded corners. */
static bool backdrop_color(lv_obj_t *obj,lv_color_t *color){
 for(lv_obj_t *p=lv_obj_get_parent(obj);p;p=lv_obj_get_parent(p))
  if(lv_obj_get_style_bg_opa(p,LV_PART_MAIN)>=LV_OPA_COVER&&lv_obj_get_style_bg_grad_dir(p,LV_PART_MAIN)==LV_GRAD_DIR_NONE){*color=lv_obj_get_style_bg_color(p,LV_PART_MAIN);return true;}
 return false;
}
/* lv_snapshot clears the buffer to transparent black before drawing an object that does not cover its own area. An opaque
   snapshot is drawn the same way onto a buffer filled with the backdrop colour instead, so it can be blitted line by line. */
static bool snapshot_opaque(lv_obj_t *obj,lv_draw_buf_t *buf,lv_color_t backdrop){
 uint32_t w=buf->header.w,h=buf->header.h,stride=buf->header.stride;
 for(uint32_t y=0;y<h;y++){
  uint8_t *row=buf->data+y*stride;
  if(buf->header.cf==LV_COLOR_FORMAT_RGB565){uint16_t v=lv_color_to_u16(backdrop);for(uint32_t x=0;x<w;x++)((uint16_t*)row)[x]=v;}
  else{uint32_t v=lv_color_to_u32(backdrop);for(uint32_t x=0;x<w;x++)((uint32_t*)row)[x]=v;}
 }
 int ext=lv_obj_get_ext_draw_size(obj);lv_area_t area;lv_obj_get_coords(obj,&area);lv_area_increase(&area,ext,ext);
 lv_layer_t layer;lv_layer_init(&layer);
 layer.draw_buf=buf;layer.buf_area=(lv_area_t){area.x1,area.y1,area.x1+(int32_t)w-1,area.y1+(int32_t)h-1};
 layer.color_format=buf->header.cf;layer._clip_area=area;layer.phy_clip_area=area;
 lv_display_t *old=lv_refr_get_disp_refreshing(),*display=lv_obj_get_display(obj);lv_layer_t *old_head=display->layer_head;
 display->layer_head=&layer;lv_refr_set_disp_refreshing(display);
 lv_obj_redraw(&layer,obj);
 while(layer.draw_task_head){lv_draw_dispatch_wait_for_request();lv_draw_dispatch();}
 display->layer_head=old_head;lv_refr_set_disp_refreshing(old);
 return true;
}
/* opaque: blit-only copy in the display's own format with the backdrop baked into the corners (the strip, which only moves
   over its own plain background); otherwise ARGB8888 that can slide over anything. */
static lv_obj_t *transition_snapshot_as(lv_obj_t *source,bool opaque){
 lv_obj_update_layout(source);int ext=lv_obj_get_ext_draw_size(source);
 uint32_t w=lv_obj_get_width(source)+2*ext,h=lv_obj_get_height(source)+2*ext;
 lv_color_t backdrop;
 if(opaque&&!backdrop_color(source,&backdrop))opaque=false;
 lv_color_format_t format=opaque?lv_display_get_color_format(lv_obj_get_display(source)):LV_COLOR_FORMAT_ARGB8888;
 uint32_t bytes=LV_DRAW_BUF_SIZE(w,h,format);
 uint32_t limit=TRANSITION_CACHE_LIMIT;
#ifdef GSO_TEST
 if(transition_test_limit<limit)limit=transition_test_limit;
#endif
 if(!w||!h||bytes>limit||transition_cache_bytes>limit-bytes)return NULL;
 transition_cache *cache=lv_malloc_zeroed(sizeof(*cache));if(!cache)return NULL;
#ifdef ESP_PLATFORM
 cache->pixels=heap_caps_aligned_alloc(64,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 cache->pixels=lv_malloc(bytes);
#endif
 if(!cache->pixels){lv_free(cache);return NULL;}
 bool drawn=lv_draw_buf_init(&cache->draw,w,h,format,LV_STRIDE_AUTO,cache->pixels,bytes)==LV_RESULT_OK&&(opaque?snapshot_opaque(source,&cache->draw,backdrop):lv_snapshot_take_to_draw_buf(source,format,&cache->draw)==LV_RESULT_OK);
 if(!drawn){transition_pixels_free(cache->pixels);lv_free(cache);return NULL;}
 cache->bytes=bytes;transition_cache_bytes+=bytes;if(transition_cache_bytes>transition_cache_peak)transition_cache_peak=transition_cache_bytes;
 lv_obj_t *image=lv_image_create(lv_obj_get_parent(source));
 if(!image){transition_cache_bytes-=bytes;transition_pixels_free(cache->pixels);lv_free(cache);return NULL;}
 lv_obj_set_pos(image,lv_obj_get_x(source)-ext,lv_obj_get_y(source)-ext);
 lv_image_set_src(image,&cache->draw);lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_event_cb(image,transition_cache_deleted,LV_EVENT_DELETE,cache);
 return image;
}
