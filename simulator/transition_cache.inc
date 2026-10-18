/* A single short-lived PSRAM snapshot replaces complex retiring panel trees. */
#include "src/core/lv_obj_draw_private.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
/* Boot transition (one panel) and the card strip freeze (up to five 334x420 cards) share this budget in PSRAM. */
#define TRANSITION_CACHE_LIMIT (3u*1024u*1024u)
typedef struct {lv_draw_buf_t draw;void *pixels;uint32_t bytes;} transition_cache;
static uint32_t transition_cache_bytes,transition_cache_peak;
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
static lv_obj_t *transition_snapshot(lv_obj_t *source){
 lv_obj_update_layout(source);int ext=lv_obj_get_ext_draw_size(source);
 uint32_t w=lv_obj_get_width(source)+2*ext,h=lv_obj_get_height(source)+2*ext;
 uint32_t bytes=LV_DRAW_BUF_SIZE(w,h,LV_COLOR_FORMAT_ARGB8888);
 if(!w||!h||bytes>TRANSITION_CACHE_LIMIT||transition_cache_bytes>TRANSITION_CACHE_LIMIT-bytes)return NULL;
 transition_cache *cache=lv_malloc_zeroed(sizeof(*cache));if(!cache)return NULL;
#ifdef ESP_PLATFORM
 cache->pixels=heap_caps_aligned_alloc(64,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 cache->pixels=lv_malloc(bytes);
#endif
 if(!cache->pixels){lv_free(cache);return NULL;}
 if(lv_draw_buf_init(&cache->draw,w,h,LV_COLOR_FORMAT_ARGB8888,LV_STRIDE_AUTO,cache->pixels,bytes)!=LV_RESULT_OK||lv_snapshot_take_to_draw_buf(source,LV_COLOR_FORMAT_ARGB8888,&cache->draw)!=LV_RESULT_OK){transition_pixels_free(cache->pixels);lv_free(cache);return NULL;}
 cache->bytes=bytes;transition_cache_bytes+=bytes;if(transition_cache_bytes>transition_cache_peak)transition_cache_peak=transition_cache_bytes;
 lv_obj_t *image=lv_image_create(lv_obj_get_parent(source));
 lv_obj_set_pos(image,lv_obj_get_x(source)-ext,lv_obj_get_y(source)-ext);
 lv_image_set_src(image,&cache->draw);lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_event_cb(image,transition_cache_deleted,LV_EVENT_DELETE,cache);
 return image;
}
