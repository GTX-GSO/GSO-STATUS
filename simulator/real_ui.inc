/* Shared-model UI. No Win32 collection, historical quota header or generated values. */
#include "ui.h"
#ifdef ESP_PLATFORM
#include "gss_build_info.h"
#include "gss_bench_config.h"
#else
#include "build_info.h"
#endif
#include "gss_wifi.h"
#include "gss_system.h"
#include "provider_icons.h"
static lv_obj_t *wifi_icon;
static bool return_to_settings; /* the page opened from settings returns there instead of to the cards */
static bool return_to_nas; /* the disk health page returns to the NAS detail page */
static int source_alert; /* 0, or 1/2 while the sidebar line warns about the disk (tapping it opens the health page) */
static void wifi_view_close(void);
static void settings_view_close(void);
static void level_view_close(void);
#include "data_source.h"
#include "home_client.h"
#include "display_brightness.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
extern const lv_font_t desk_font_20,desk_font_16,desk_font_countdown;
#define BG 0x101419
#define PANEL 0x1A2129
#define INSET 0x131A21
#define TEXT 0xE9EEF4
#define MUTED 0xA3B0BE
#define GREEN 0xA5E5C4
#define BLUE 0x8EBFF5
#define AMBER 0xF1BF77
#define RED 0xF08080
#define LINE 0x2B3641
static lv_obj_t *sidebar_title,*traffic_box,*traffic_arrow[2],*traffic_unit[2];
static lv_obj_t *root,*viewport,*overlay,*sidebar,*side_button,*clock_label,*date_label,*rx_text,*tx_text,*source_label;
static lv_obj_t *stack_labels[2][3],*service_labels[2][4],*vpn_labels[2],*weather_label,*weather_quality_label,*weather_location_label;
static lv_obj_t *vpn_total_labels[2],*vpn_arcs[2],*vpn_percent[2],*vpn_reset[2],*vpn_reset_words[2],*quota_update_words[3][2];
static void render_weather(void);static void close_weather(void);
static lv_obj_t *vpn_quality_labels[2],*game_process_label;
static lv_obj_t *net_detail_values[4];
/* Quota groups: 0 summary card (focused, currently working provider only), 1 detail Codex, 2 detail Claude. */
static lv_obj_t *quota_values[3][2],*quota_bars[3][2],*quota_countdowns[3][2],*quota_updates[3][2],*quota_source_labels[2],*quota_icons[8];
/* icons: 0 card, 1 detail header, 2..5 detail quota boxes (fixed Codex, Claude), 6..7 detail client rows */
static unsigned quota_icon_provider[8];static bool quota_icon_visible[8];
/* VIBE detail client rows (Codex, Claude) and NET detail NAS volumes. */
static lv_obj_t *client_status[GSO_VIBE_PROVIDERS],*client_tokens[GSO_VIBE_PROVIDERS];
static lv_obj_t *vibe_states[2],*vibe_titles[2],*vibe_durations[2];
static int page,detail_id;
static bool paused;
static uint32_t sequence;
static gso_snapshot model;
/* Defer telemetry widgets, never LVGL's input/animation/display processing. */
static uint32_t transition_started,transition_duration;
static bool telemetry_pending;
static bool strip_frozen;
static lv_obj_t *game_boot_panel;
static void interaction_hold(uint32_t duration){
 transition_started=lv_tick_get();transition_duration=duration;
}
static bool interaction_busy(void){
#ifdef GSO_GSS_BASELINE
 return false;
#else
 if(strip_frozen)return true; /* includes snapping after the pointer was released */
 if(lv_tick_elaps(transition_started)<transition_duration)return true;
 for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input)){
  if(lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED||lv_indev_get_scroll_obj(input))return true;
 }
 return false;
#endif
}
typedef struct {
 lv_obj_t *value,*arc,*temperature,*memory,*memory_rate,*memory_bar,*power,*name,*load_bar,*clock;
 uint32_t temperature_normal; /* colour of the temperature while it is in the normal range */
 bool compact;
}
hardware_view;
static hardware_view main_hardware[2],detail_hardware[2];
/* Disk activity of the last second: busy percent beside the model, read/write throughput and the mean response time on the
   right. A figure the disk has not produced (nothing read or written in the interval) shows "-". */
static void disk_ms_text(char *out,size_t size,gso_metric m){
 if(m.valid&&m.value<100)snprintf(out,size,"%.1f ms",m.value);
 else if(m.valid)snprintf(out,size,"%.0f ms",m.value);
 else snprintf(out,size,"-");
}
static void disk_rate_text(char *out,size_t size,gso_metric m){
 if(!m.valid)snprintf(out,size,"-");
 else if(m.value>=1048576.0*100)snprintf(out,size,"%.0f MB/s",m.value/1048576.0);
 else if(m.value>=1048576.0)snprintf(out,size,"%.1f MB/s",m.value/1048576.0);
 else snprintf(out,size,"%.0f KB/s",m.value/1024.0);
}
static lv_color_t c(uint32_t value);
/* The tiny ring beside the busy percentage: green while the disk has room, amber from 70%, red from 90%. */
static void disk_ring_value(lv_obj_t *ring,const gso_disk *d){
 if(!ring)return;
 bool known=d&&d->busy.valid;
 if(d)lv_obj_remove_flag(ring,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(ring,LV_OBJ_FLAG_HIDDEN);
 lv_arc_set_value(ring,known?(int)lround(fmax(0,fmin(100,d->busy.value))):0);
 lv_obj_set_style_arc_color(ring,c(!known?MUTED:d->busy.value>=90?RED:d->busy.value>=70?AMBER:GREEN),LV_PART_INDICATOR);
}
static void disk_activity_text(lv_obj_t *busy_label,lv_obj_t *read_label,lv_obj_t *write_label,lv_obj_t *response_label,const gso_disk *d){
 char text[16]="-";
 if(d&&d->busy.valid)lv_label_set_text_fmt(busy_label,"占用 %.0f%%",d->busy.value);
 else lv_label_set_text(busy_label,d?"占用 -":"");
 if(d)disk_rate_text(text,sizeof(text),d->read_rate);else text[0]=0;
 lv_label_set_text(read_label,text);
 if(d)disk_rate_text(text,sizeof(text),d->write_rate);else text[0]=0;
 lv_label_set_text(write_label,text);
 if(d)disk_ms_text(text,sizeof(text),d->response_ms);else text[0]=0;
 lv_label_set_text(response_label,text);
}
static lv_obj_t *disk_labels[3],*disk_models[3],*disk_totals[3],*disk_free[3],*disk_bars[3],*disk_busy[3],*disk_ring[3],*disk_read[3],*disk_write[3],*disk_response[3],*fps_label,*latency_label;
static lv_obj_t *game_chart,*game_icon_image;
static lv_image_dsc_t game_icon_desc={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=24,.h=24,.stride=96},.data_size=2304};
static lv_chart_series_t *fps_series;
static lv_color_t c(uint32_t value){return lv_color_hex(desk_ui_theme_color(value));}
/* Avoid reallocating text and dirtying unchanged labels on each telemetry poll. */
static void changed_label(lv_obj_t *o,const char *value){if(!value||strcmp(lv_label_get_text(o),value))lv_label_set_text(o,value);}
static void changed_label_fmt(lv_obj_t *o,const char *format,...) LV_FORMAT_ATTRIBUTE(2,3);
static void changed_label_fmt(lv_obj_t *o,const char *format,...){
 char small_buffer[256];
 va_list args,copy;
 va_start(args,format);
 va_copy(copy,args);
 int n=vsnprintf(small_buffer,sizeof(small_buffer),format,args);
 va_end(args);
 if(n>=0&&(size_t)n<sizeof(small_buffer))changed_label(o,small_buffer);
 else if(n>=0){
  char *large=lv_malloc((size_t)n+1);
  if(large){
   vsnprintf(large,(size_t)n+1,format,copy);
   changed_label(o,large);
   lv_free(large);
  }
 }
 va_end(copy);
}
#ifndef GSO_GSS_BASELINE
#define lv_label_set_text changed_label
#define lv_label_set_text_fmt changed_label_fmt
#endif
/* lv_obj_set_style_* always refreshes the object, even to the value it already has; a colour set on every telemetry poll
   would repaint every label each second. Skip the call when the local style already holds that colour. */
static bool local_color_is(lv_obj_t *o,lv_style_prop_t prop,lv_color_t color,lv_style_selector_t selector){
 lv_style_value_t current;
 return lv_obj_get_local_style_prop(o,prop,&current,selector)==LV_STYLE_RES_FOUND&&lv_color_eq(current.color,color);
}
static void changed_text_color(lv_obj_t *o,lv_color_t color,lv_style_selector_t selector){
 if(!local_color_is(o,LV_STYLE_TEXT_COLOR,color,selector))lv_obj_set_style_text_color(o,color,selector);
}
static void changed_bg_color(lv_obj_t *o,lv_color_t color,lv_style_selector_t selector){
 if(!local_color_is(o,LV_STYLE_BG_COLOR,color,selector))lv_obj_set_style_bg_color(o,color,selector);
}
static void changed_arc_color(lv_obj_t *o,lv_color_t color,lv_style_selector_t selector){
 if(!local_color_is(o,LV_STYLE_ARC_COLOR,color,selector))lv_obj_set_style_arc_color(o,color,selector);
}
#ifndef GSO_GSS_BASELINE
#define lv_obj_set_style_text_color changed_text_color
#define lv_obj_set_style_bg_color changed_bg_color
#define lv_obj_set_style_arc_color changed_arc_color
#endif
static lv_obj_t *main_cards[7];static int main_card_position[7]={0,1,2,3,4,5,6}; /* by module: WIN, VIBE, NET, HOME, PRINT, CAR, NAS */
static int shown_cards=4; /* PRINT, CAR and NAS only take a place while a printer, a vehicle or a NAS host report is configured */
static void print_card(void);
static void print_detail(void);
static void print_detail_closed(void);
static void render_print(void);
static bool print_attention(void);
static bool print_reported(void);
static void car_card(void);
static void car_detail(void);
static void car_detail_closed(void);
static void render_car(void);
static bool car_reported(void);
static void nas_card(void);
static void nas_detail(void);
static void nas_detail_closed(void);
static void nas_health_detail(void);
static int nas_health_alert(void);
static void render_nas(void);
static bool nas_reported(void);
static bool main_card_visible(unsigned slot){
#ifdef GSO_GSS_BASELINE
 (void)slot;return true;
#else
 if(overlay||!viewport){return false;}
 int left=(slot<7?main_card_position[slot]:(int)slot)*350,scroll=lv_obj_get_scroll_x(viewport);return left+334>scroll&&left<scroll+1032;
#endif
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius){
 lv_obj_t *o=lv_obj_create(parent);
 lv_obj_remove_style_all(o);
 lv_obj_set_pos(o,x,y);
 lv_obj_set_size(o,w,h);
 lv_obj_set_style_bg_color(o,c(color),0);
 lv_obj_set_style_bg_opa(o,255,0);
 lv_obj_set_style_radius(o,radius,0);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
 return o;
}
static lv_obj_t *text(lv_obj_t *parent,int x,int y,const char *value,const lv_font_t *font,uint32_t color){
 lv_obj_t *o=lv_label_create(parent);
 lv_label_set_text(o,value);
 lv_obj_set_pos(o,x,y);
 lv_obj_set_style_text_font(o,font,0);
 lv_obj_set_style_text_color(o,c(color),0);
 return o;
}
/* Type scale for the 6.2" 1280x452 panel (~219 ppi, 0.116 mm/px): body text is 20 px (1.7 mm cap height); desk_font_16 is
   only kept for crowded secondary lines. */
#define DESK_BODY_FONT desk_font_20
static lv_obj_t *small(lv_obj_t *p,int x,int y,const char *v,uint32_t color){return text(p,x,y,v,&DESK_BODY_FONT,color);}
static lv_obj_t *fine(lv_obj_t *p,int x,int y,const char *v,uint32_t color){return text(p,x,y,v,&desk_font_16,color);}
/* A small word label (16 px) next to its value label (20 px). word_left: the value is anchored and the word sits to its left;
   otherwise the word is anchored and the value follows it. */
static void refresh_set(lv_obj_t *value,lv_obj_t *word,const char *label,bool word_left){
 if(!value)return;
 lv_label_set_text(value,label);
 if(!word)return;
 lv_obj_update_layout(value);
 if(word_left)lv_obj_align_to(word,value,LV_ALIGN_OUT_LEFT_MID,-6,0);else lv_obj_align_to(value,word,LV_ALIGN_OUT_RIGHT_MID,6,0);
}
static lv_obj_t *cn(lv_obj_t *p,int x,int y,const char *v,uint32_t color){return text(p,x,y,v,&desk_font_20,color);}
int desk_ui_card_slot(unsigned module){return module<7?main_card_position[module]:-1;}
/* VIBE task state with a short hold. It rides on the 1 Hz Windows telemetry, so a few seconds without a fresh snapshot
   (slow sensor read, a missed GSN poll, a GSN reconnect, a transient "unknown") must not flip the label to 未运行 or move
   the card to the end; only a gap longer than vibe_hold_ms does. 1 = running, 2 = ready, 0 = not running. */
static uint32_t vibe_hold_ms=20000;static int vibe_last_state;static uint64_t vibe_last_good_ms;static bool vibe_live;
void desk_ui_vibe_hold(uint32_t ms){vibe_hold_ms=ms;vibe_last_state=0;vibe_last_good_ms=0;}
static int vibe_state_now(void){
 uint64_t now=desk_data_now_ms();int current=!strcmp(model.task_state,"running")?1:!strcmp(model.task_state,"idle")?2:0;
 vibe_live=current&&gso_source_quality(model.windows,now,3000,10000)==GSO_OK;
 if(vibe_live){vibe_last_state=current;vibe_last_good_ms=now;return current;}
 return vibe_last_good_ms&&now-vibe_last_good_ms<vibe_hold_ms?vibe_last_state:0;
}
/* Order of the main cards: WIN first (last while GSW is offline); VIBE and PRINT sit up front while they work and move to the end while they do not
   (VIBE before PRINT at the end); NET and HOME fill the rest, with CAR right behind HOME. PRINT is hidden unless a printer is
   configured, CAR unless a vehicle is, NAS unless GSN reports the host status (it sits right behind NET). */
static int page_limit(void){return shown_cards>3?shown_cards-3:0;}
static void arrange_main_cards(bool vibe_last,bool print_shown,bool print_active,bool car_shown,bool nas_shown,bool win_last){
 enum {WIN_CARD=0,VIBE_CARD=1,NET_CARD=2,HOME_CARD=3,PRINT_CARD=4,CAR_CARD=5,NAS_CARD=6};
 int order[7],count=0;
 if(!win_last)order[count++]=WIN_CARD;
 if(!vibe_last)order[count++]=VIBE_CARD;
 if(print_shown&&print_active)order[count++]=PRINT_CARD;
 order[count++]=NET_CARD;
 if(nas_shown)order[count++]=NAS_CARD;
 order[count++]=HOME_CARD;
 if(car_shown)order[count++]=CAR_CARD;
 if(vibe_last)order[count++]=VIBE_CARD;
 if(print_shown&&!print_active)order[count++]=PRINT_CARD;
 if(win_last)order[count++]=WIN_CARD; /* GSW offline: the hardware card has nothing to show */
 for(int slot=0;slot<count;slot++){
  int m=order[slot];
  if(main_card_position[m]!=slot){
   main_card_position[m]=slot;
   if(main_cards[m])lv_obj_set_x(main_cards[m],slot*350);
  }
 }
 if(main_cards[PRINT_CARD]){
  if(print_shown)lv_obj_remove_flag(main_cards[PRINT_CARD],LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(main_cards[PRINT_CARD],LV_OBJ_FLAG_HIDDEN);
 }
 if(main_cards[CAR_CARD]){
  if(car_shown)lv_obj_remove_flag(main_cards[CAR_CARD],LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(main_cards[CAR_CARD],LV_OBJ_FLAG_HIDDEN);
 }
 if(main_cards[NAS_CARD]){
  if(nas_shown)lv_obj_remove_flag(main_cards[NAS_CARD],LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(main_cards[NAS_CARD],LV_OBJ_FLAG_HIDDEN);
 }
 shown_cards=count;
 /* the card strip got shorter under a scroll position that no longer exists */
 if(viewport&&!interaction_busy()&&lv_obj_get_scroll_x(viewport)>page_limit()*350+2)lv_obj_scroll_to_x(viewport,page_limit()*350,LV_ANIM_OFF);
}

/* Provider marks: the OpenAI mark (Codex) and the Claude burst, 20x20 ARGB8888 bitmaps generated by
   scripts/generate-provider-icons.ps1 from the logos in the installed apps. */
static const lv_image_dsc_t provider_icon_desc[GSO_VIBE_PROVIDERS]={
 [GSO_VIBE_CODEX]={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=GSO_PROVIDER_ICON_SIZE,.h=GSO_PROVIDER_ICON_SIZE,.stride=GSO_PROVIDER_ICON_SIZE*4},.data_size=sizeof(gso_codex_icon_pixels),.data=gso_codex_icon_pixels},
 [GSO_VIBE_CLAUDE]={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=GSO_PROVIDER_ICON_SIZE,.h=GSO_PROVIDER_ICON_SIZE,.stride=GSO_PROVIDER_ICON_SIZE*4},.data_size=sizeof(gso_claude_icon_pixels),.data=gso_claude_icon_pixels}};
static void provider_icon_draw(lv_event_t *event){
 lv_obj_t *o=lv_event_get_target_obj(event);
 unsigned provider=*(unsigned *)lv_event_get_user_data(event);
 lv_area_t area;
 lv_obj_get_coords(o,&area);
 lv_draw_image_dsc_t image;
 lv_draw_image_dsc_init(&image);
 image.src=&provider_icon_desc[provider==GSO_VIBE_CLAUDE?GSO_VIBE_CLAUDE:GSO_VIBE_CODEX];
 if(provider!=GSO_VIBE_CLAUDE){
  image.recolor=c(TEXT);
  image.recolor_opa=LV_OPA_COVER;
 }
 /* the OpenAI mark is monochrome: follow the theme text colour (white on dark, dark on light) */
 lv_area_t body={area.x1,area.y1,area.x1+GSO_PROVIDER_ICON_SIZE-1,area.y1+GSO_PROVIDER_ICON_SIZE-1};
 lv_draw_image(lv_event_get_layer(event),&image,&body);
}
/* Location pin (teardrop with a hole) drawn with primitives; 18 x 22 px. */
static lv_obj_t *weather_location_pin;
static void location_pin_draw(lv_event_t *event){
 lv_obj_t *o=lv_event_get_target_obj(event);
 lv_area_t area;
 lv_obj_get_coords(o,&area);
 int cx=(area.x1+area.x2)/2,top=area.y1+1;
 lv_layer_t *layer=lv_event_get_layer(event);
 lv_color_t tint=lv_obj_get_style_text_color(o,0);
 lv_draw_rect_dsc_t head;
 lv_draw_rect_dsc_init(&head);
 head.bg_opa=255;
 head.bg_color=tint;
 head.radius=LV_RADIUS_CIRCLE;
 head.border_width=0;
 lv_area_t disc={cx-7,top,cx+7,top+14};
 lv_draw_rect(layer,&head,&disc);
 lv_draw_triangle_dsc_t tip;
 lv_draw_triangle_dsc_init(&tip);
 tip.color=tint;
 tip.opa=255;
 tip.p[0]=(lv_point_precise_t){cx-6,top+10};
 tip.p[1]=(lv_point_precise_t){cx+6,top+10};
 tip.p[2]=(lv_point_precise_t){cx,top+19};
 lv_draw_triangle(layer,&tip);
 lv_draw_rect_dsc_t hole;
 lv_draw_rect_dsc_init(&hole);
 hole.bg_opa=255;
 hole.bg_color=lv_obj_get_style_bg_color(lv_obj_get_parent(o),0);
 hole.radius=LV_RADIUS_CIRCLE;
 hole.border_width=0;
 lv_area_t dot={cx-3,top+4,cx+3,top+10};
 lv_draw_rect(layer,&hole,&dot);
}
static lv_obj_t *location_pin_create(lv_obj_t *parent,int x,int y){
 lv_obj_t *o=lv_obj_create(parent);
 lv_obj_remove_style_all(o);
 lv_obj_set_pos(o,x,y);
 lv_obj_set_size(o,18,22);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_text_color(o,c(MUTED),0);
 lv_obj_add_event_cb(o,location_pin_draw,LV_EVENT_DRAW_MAIN,NULL);
 return o;
}
static lv_obj_t *provider_icon(lv_obj_t *parent,int x,int y,int slot){
 lv_obj_t *o=lv_obj_create(parent);
 lv_obj_remove_style_all(o);
 lv_obj_set_pos(o,x,y);
 lv_obj_set_size(o,20,20);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
 quota_icon_provider[slot]=GSO_VIBE_CODEX;
 quota_icon_visible[slot]=false;
 lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
 lv_obj_add_event_cb(o,provider_icon_draw,LV_EVENT_DRAW_MAIN,&quota_icon_provider[slot]);
 return o;
}
static void provider_icon_set(int slot,unsigned provider,bool visible){
 lv_obj_t *o=quota_icons[slot];if(!o)return;
 if(quota_icon_provider[slot]!=provider){quota_icon_provider[slot]=provider;lv_obj_invalidate(o);}
 if(quota_icon_visible[slot]!=visible){
  quota_icon_visible[slot]=visible;
  if(visible)lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
 }
}
/* Provider icon + name as one right-aligned block (offset from the parent's right edge); returns the name label. */
static lv_obj_t *provider_badge(lv_obj_t *parent,int slot,int right,int y){
 lv_obj_t *who=lv_obj_create(parent);
 lv_obj_remove_style_all(who);
 lv_obj_set_size(who,LV_SIZE_CONTENT,LV_SIZE_CONTENT);
 lv_obj_remove_flag(who,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_flex_flow(who,LV_FLEX_FLOW_ROW);
 lv_obj_set_flex_align(who,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
 lv_obj_set_style_pad_column(who,8,0);
 lv_obj_align(who,LV_ALIGN_TOP_RIGHT,right,y);
 quota_icons[slot]=provider_icon(who,0,0,slot);
 return small(who,0,0,"未接入",MUTED);
}
static lv_obj_t *bar(lv_obj_t *p,int x,int y,int w,uint32_t color){
 lv_obj_t *o=lv_bar_create(p);
 lv_obj_remove_style_all(o);
 lv_obj_set_pos(o,x,y);
 lv_obj_set_size(o,w,8);
 lv_obj_set_style_bg_color(o,c(LINE),LV_PART_MAIN);
 lv_obj_set_style_bg_opa(o,255,LV_PART_MAIN);
 lv_obj_set_style_bg_color(o,c(color),LV_PART_INDICATOR);
 lv_obj_set_style_bg_opa(o,255,LV_PART_INDICATOR);
 return o;
}
static void detail_cb(lv_event_t *e){desk_ui_detail((int)(intptr_t)lv_event_get_user_data(e));}
static void clickable(lv_obj_t *o,int id){
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_event_cb(o,detail_cb,LV_EVENT_CLICKED,(void *)(intptr_t)id);
}
static void source_alert_cb(lv_event_t *e){
 (void)e;
 if(source_alert)desk_ui_detail(22);
}
static lv_obj_t *button(lv_obj_t *p,int x,int y,int w,const char *label,lv_event_cb_t cb){
 lv_obj_t *o=box(p,x,y,w,44,LINE,10);
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *t=cn(o,0,0,label,TEXT);
 lv_obj_center(t);
 lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,NULL);
 return o;
}
static const char *quality_text(gso_quality q){
 switch(q){
  case GSO_OK:return "";
  case GSO_STALE:return "数据过期";
  case GSO_OFFLINE:return "来源离线";
  case GSO_UNSUPPORTED:return "不支持";
  case GSO_ERROR:return "来源错误";
  default:return "未接入";
 }
}
/* Daily token counts (1.25M) and NAS capacities (3.64TB) in the display's decimal-looking GB convention (GiB values). */
/* Running time in the unit that fits: 45秒, 12分钟, 1小时20分钟. */
static void duration_text(char *out,size_t cap,uint64_t seconds){
 unsigned long long minutes=seconds/60,hours=seconds/3600;
 if(seconds<60)snprintf(out,cap,"%llu秒",(unsigned long long)seconds);
 else if(seconds<3600)snprintf(out,cap,"%llu分钟",minutes);
 else if(minutes%60)snprintf(out,cap,"%llu小时%llu分钟",hours,minutes%60);
 else snprintf(out,cap,"%llu小时",hours);
}
static void token_text(char *out,size_t cap,double n){
 if(n<1000)snprintf(out,cap,"%.0f",n);
 else if(n<1e6)snprintf(out,cap,"%.1fK",n/1e3);
 else if(n<1e9)snprintf(out,cap,"%.2fM",n/1e6);
 else snprintf(out,cap,"%.2fB",n/1e9);
}
static void storage_text(char *out,size_t cap,double bytes){
 double gb=bytes/1073741824.0;
 if(gb<1000)snprintf(out,cap,"%.0fGB",gb);
 else snprintf(out,cap,"%.2fTB",gb/1024.0);
}
static void value_text(lv_obj_t *o,gso_metric m,const char *suffix){
 if(!o)return;
 if(m.valid)lv_label_set_text_fmt(o,"%.0f%s",m.value,suffix);
 else lv_label_set_text(o,"-");
}
/* Temperature turns orange from 75 °C and red from 95 °C (a Ryzen 3900X throttles at 95); unknown keeps the normal colour. */
#define TEMPERATURE_WARN_C 75.0
#define TEMPERATURE_ALARM_C 95.0
static uint32_t temperature_color(gso_metric m,uint32_t normal){
 double shown=(double)(long)(m.value+0.5); /* the colour follows the rounded figure on screen */
 if(!m.valid||shown<TEMPERATURE_WARN_C)return normal;
 return shown<TEMPERATURE_ALARM_C?AMBER:RED;
}
static void render_hardware(hardware_view *v,const gso_hardware *h){
 value_text(v->value,h->load,"");
 if(v->arc)lv_arc_set_value(v->arc,h->load.valid?(int)h->load.value:0);
 value_text(v->temperature,h->temperature,"°C");
 if(v->temperature){
  uint32_t color=temperature_color(h->temperature,v->temperature_normal);
  /* the game panel keeps its own fixed dark colours in either theme */
  lv_obj_set_style_text_color(v->temperature,v->temperature_normal==0xF6F3FF?lv_color_hex(color):c(color),0);
 }
 value_text(v->power,h->power," W");
 if(v->memory){
  /* Capacity and transfer rate: one line when wide ("·"), two spaces on the narrow card, separate right-aligned label in the WIN detail. */
  char capacity[40],rate[24];rate[0]=0;
  if(h->used.valid&&h->total.valid)snprintf(capacity,sizeof(capacity),"%.1f/%.1f GB",h->used.value/1073741824.0,h->total.value/1073741824.0);
  else snprintf(capacity,sizeof(capacity),"—/— GB");
  if(h->memory_rate.valid)snprintf(rate,sizeof(rate),"%.0f MT/s",h->memory_rate.value); /* what the collector reports, never a fixed number */
  else if(h->memory_clock.valid)snprintf(rate,sizeof(rate),"%.0f MHz",h->memory_clock.value);
  if(v->memory_rate){lv_label_set_text(v->memory,capacity);lv_label_set_text(v->memory_rate,rate);}
  else{
   char line[72];
   snprintf(line,sizeof(line),rate[0]?(v->compact?"%s  %s":"%s · %s"):"%s",capacity,rate);
   lv_label_set_text(v->memory,line);
  }
 }
 if(v->clock){if(h->clock.valid)lv_label_set_text_fmt(v->clock,"%.0f MHz",h->clock.value);else lv_label_set_text(v->clock,"— MHz");}

 if(v->load_bar)lv_bar_set_value(v->load_bar,h->load.valid?(int)h->load.value:0,LV_ANIM_OFF);
 if(v->memory_bar){
  lv_bar_set_value(v->memory_bar,h->used.valid&&h->total.valid&&h->total.value>0?(int)(h->used.value/h->total.value*100):0,LV_ANIM_OFF);
 }
 if(v->name)lv_label_set_text(v->name,h->model);
}
#include "home_ui.inc"
static const char *weather_condition_name(const char *condition){
 const char *keys[]={"sunny","clear-night","cloudy","partlycloudy","rainy","pouring","lightning","lightning-rainy","snowy","snowy-rainy","fog","hail","windy","windy-variant","exceptional"};
 const char *names[]={"晴","晴夜","阴","多云","雨","大雨","雷电","雷雨","雪","雨夹雪","雾","冰雹","有风","大风","异常天气"};
 for(int i=0;i<15;i++)if(!strcmp(condition,keys[i]))return names[i];
 return "天气";
}
#include "weather_icons.inc"
static lv_obj_t *weather_summary_icon;
#include "net_ui.inc"
static void render(void){
 net_controls_render();
 render_home();render_weather();weather_icon_update(weather_summary_icon,model.weather_condition);
 int vibe_now=vibe_state_now();arrange_main_cards(vibe_now==0,print_reported(),print_attention(),car_reported(),nas_reported(),gso_source_quality(model.windows,desk_data_now_ms(),3000,10000)==GSO_OFFLINE);
 render_print();render_car();render_nas();
 for(int i=0;i<2;i++)if(vibe_states[i]){
  gso_quality q=gso_source_quality(model.windows,desk_data_now_ms(),3000,10000);
  bool running=vibe_now==1;
  bool idle=vibe_now==2;
  lv_label_set_text(vibe_states[i],running?"运行中":idle?"就绪":q==GSO_OK&&model.task_duration_ms.reason&&!strcmp(model.task_duration_ms.reason,"task_event_timeout")?"任务事件超时":"未运行");
  lv_obj_set_style_text_color(vibe_states[i],c(running?BLUE:idle?GREEN:AMBER),0);
 }
 for(int i=0;i<2;i++){
  bool running=vibe_now==1;
  if(vibe_now&&!vibe_live)continue; /* inside the hold: keep the last title and duration */
  if(vibe_titles[i])lv_label_set_text(vibe_titles[i],running?(model.task_title[0]?model.task_title:"会话名称不可用"):"");
  if(vibe_durations[i]){
   if(running&&model.task_duration_ms.valid){
    uint64_t seconds=(uint64_t)model.task_duration_ms.value/1000;
    char span[32];
    duration_text(span,sizeof(span),seconds);
    lv_label_set_text_fmt(vibe_durations[i],"已进行 %s",span);
   }else lv_label_set_text(vibe_durations[i],running?"持续时长不可用":"");
  }
 }
 gso_quality quota_quality=gso_source_quality(model.codex,desk_data_now_ms(),180000,600000);
 for(int i=0;i<2;i++)if(quota_source_labels[i]){
  unsigned focus=model.vibe_provider;
  bool known=focus<GSO_VIBE_PROVIDERS,has=known&&(model.quota_remaining[focus][0].valid||model.quota_remaining[focus][1].valid);
  lv_label_set_text(quota_source_labels[i],quota_quality==GSO_OK?gso_vibe_provider_name(focus):quota_quality==GSO_ERROR?"额度读取异常":quality_text(quota_quality));
  lv_obj_set_style_text_color(quota_source_labels[i],c(quota_quality==GSO_OK&&has?MUTED:AMBER),0);
 }
 for(unsigned p=0;p<GSO_VIBE_PROVIDERS;p++)if(client_status[p]){
  const gso_client *client=&model.clients[p];
  bool live=client->reported&&quota_quality==GSO_OK;
  char count[24]="";
  if(live&&client->tokens_today.valid)token_text(count,sizeof(count),client->tokens_today.value);
  lv_label_set_text(client_status[p],live?(client->online?"在线":"离线"):client->reported?quality_text(quota_quality):"未上报");
  lv_obj_set_style_text_color(client_status[p],c(live?(client->online?GREEN:MUTED):AMBER),0);
  if(count[0])lv_label_set_text_fmt(client_tokens[p],"今日消耗 %s tokens",count);
  else lv_label_set_text(client_tokens[p],live?"今日消耗 统计中":"今日消耗 -");
  lv_obj_set_style_text_color(client_tokens[p],c(count[0]?TEXT:MUTED),0);
 }
 {unsigned focus_icon=model.vibe_provider;bool known=focus_icon<GSO_VIBE_PROVIDERS;
  for(int slot=0;slot<2;slot++)provider_icon_set(slot,known?focus_icon:GSO_VIBE_CODEX,known&&quota_quality==GSO_OK);
  }
 for(int group=0;group<3;group++){
  /* Card: the focused provider only. Detail: Codex row and Claude row, each with its own data. */
  unsigned provider=group?(unsigned)group-1:model.vibe_provider;bool known=provider<GSO_VIBE_PROVIDERS;
  for(int i=0;i<2;i++){
   gso_metric remaining=known?model.quota_remaining[provider][i]:gso_metric_missing("not_running");
   int64_t resets=known?model.quota_resets[provider][i]:0;
   uint32_t color=!remaining.valid?MUTED:remaining.value>=50?GREEN:remaining.value>=20?AMBER:RED;
   value_text(quota_values[group][i],remaining,"%");
   if(quota_values[group][i])lv_obj_set_style_text_color(quota_values[group][i],c(color),0);
   if(quota_bars[group][i]){
    lv_bar_set_value(quota_bars[group][i],remaining.valid?(int)remaining.value:0,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(quota_bars[group][i],c(color),LV_PART_INDICATOR);
   }
   if(quota_countdowns[group][i]){
    char countdown[64];
    if(resets)desk_ui_countdown_text(resets,time(NULL),countdown,sizeof(countdown));
    else snprintf(countdown,sizeof(countdown),"不可用");
    lv_label_set_text(quota_countdowns[group][i],countdown);
   }
   if(quota_updates[group][i]){
    time_t reset=(time_t)resets+8*3600;
    struct tm *stamp=gmtime(&reset);
    char when[40];
    if(resets&&stamp){
     if(i==1)snprintf(when,sizeof(when),"%02d-%02d %02d:%02d",stamp->tm_mon+1,stamp->tm_mday,stamp->tm_hour,stamp->tm_min);
     else snprintf(when,sizeof(when),"%02d:%02d:%02d",stamp->tm_hour,stamp->tm_min,stamp->tm_sec);
    }else snprintf(when,sizeof(when),"--:--:--");
    refresh_set(quota_updates[group][i],quota_update_words[group][i],when,true);
   }
  }
 }
 for(int group=0;group<2;group++){
  if(vpn_quality_labels[group]){
   gso_quality vq=gso_source_quality(model.vpn,desk_data_now_ms(),45000,120000);
   lv_label_set_text(vpn_quality_labels[group],vq==GSO_OK?"共200GB":quality_text(vq));
   lv_obj_set_style_text_color(vpn_quality_labels[group],c(vq==GSO_OK?MUTED:AMBER),0);
  }
  for(int i=0;i<4;i++)if(service_labels[group][i]){
   gso_service *service=&model.services[i];
   gso_quality q=gso_source_quality(service->source,desk_data_now_ms(),45000,120000);
   const char *display=service->raw_state;
   if(!strcmp(display,"拦截已开启"))display="运行中";
   if(i==2){
    if(!strcmp(display,"rule"))display="规则模式";
    else if(!strcmp(display,"global"))display="全局模式";
    else if(!strcmp(display,"direct"))display="直连模式";
   }
   bool healthy=q==GSO_OK&&(!strcmp(display,"运行中")||!strcmp(display,"running")||(i==2&&(!strcmp(display,"规则模式")||!strcmp(display,"全局模式")||!strcmp(display,"直连模式"))));
   lv_label_set_text(service_labels[group][i],q==GSO_OK&&display[0]?display:quality_text(q));
   lv_obj_set_style_text_color(service_labels[group][i],c(healthy?GREEN:AMBER),0);
  }
  if(stack_labels[group][0]){
   /* GSN is the link this display polls, GSW the Windows collector behind it, HA the Home Assistant source GSN reads. */
   desk_connection_status link=desk_data_status();
   gso_quality windows=gso_source_quality(model.windows,desk_data_now_ms(),3000,10000),home=gso_source_quality(model.home,desk_data_now_ms(),45000,120000);
   const char *gsn=link==DESK_ONLINE?"运行中":link==DESK_CONNECTING?"连接中":link==DESK_NETWORK_OFFLINE?"网络离线":link==DESK_GSN_OFFLINE?"离线":link==DESK_GSN_AUTH?"认证失败":"响应异常";
   const char *states[3]={gsn,windows==GSO_OK?"运行中":quality_text(windows),home==GSO_OK?"运行中":quality_text(home)};
   bool healthy[3]={link==DESK_ONLINE,windows==GSO_OK,home==GSO_OK};
   for(int i=0;i<3;i++){
    lv_label_set_text(stack_labels[group][i],states[i]);
    lv_obj_set_style_text_color(stack_labels[group][i],c(healthy[i]?GREEN:AMBER),0);
   }
  }

  if(vpn_labels[group]){
   if(model.vpn_remaining.valid)lv_label_set_text_fmt(vpn_labels[group],"%.2f",model.vpn_remaining.value/1073741824.0);
   else lv_label_set_text(vpn_labels[group],"-");
  }
  if(vpn_arcs[group]){
   double ratio=model.vpn_remaining.valid?fmax(0,fmin(1,model.vpn_remaining.value/214748364800.0)):0;
   lv_arc_set_value(vpn_arcs[group],(int)lround(ratio*100));
   if(vpn_percent[group]){
    if(model.vpn_remaining.valid)lv_label_set_text_fmt(vpn_percent[group],"%.1f%%",ratio*100);
    else lv_label_set_text(vpn_percent[group],"—");
   }
   {
    uint32_t level=!model.vpn_remaining.valid?MUTED:ratio>=0.5?GREEN:ratio>=0.2?AMBER:RED;
    lv_obj_set_style_arc_color(vpn_arcs[group],c(level),LV_PART_INDICATOR);
    if(vpn_percent[group])lv_obj_set_style_text_color(vpn_percent[group],c(level),0);
   }
  }
  if(vpn_reset[group]){
   int month=0,day=0;
   char when[16];
   desk_ui_vpn_reset_days(time(NULL),&month,&day);
   snprintf(when,sizeof(when),"%02d-%02d",month,day);
   refresh_set(vpn_reset[group],vpn_reset_words[group],when,false);
  }
 }
 for(int i=0;i<4;i++)if(net_detail_values[i]){
  gso_quality q=gso_source_quality(model.net_details[i],desk_data_now_ms(),45000,120000);
  lv_obj_set_style_text_color(net_detail_values[i],c(q==GSO_OK?TEXT:AMBER),0);
  if(q!=GSO_OK){
   lv_label_set_text(net_detail_values[i],quality_text(q));
   continue;
  }
  if(i<2){
   int64_t stamp=i?model.antiad_updated:model.subscription_updated;
   int64_t elapsed=(int64_t)time(NULL)-stamp;
   if(stamp<=0)lv_label_set_text(net_detail_values[i],"-");
   else if(elapsed < -60)lv_label_set_text(net_detail_values[i],"时间异常");
   else if(elapsed<60)lv_label_set_text(net_detail_values[i],"刚刚");
   else if(elapsed<3600)lv_label_set_text_fmt(net_detail_values[i],"%lld分钟前",(long long)(elapsed/60));
   else if(elapsed<86400)lv_label_set_text_fmt(net_detail_values[i],"%lld小时前",(long long)(elapsed/3600));
   else lv_label_set_text_fmt(net_detail_values[i],"%lld天前",(long long)(elapsed/86400));
  }else{
   gso_metric count=i==2?model.antiad_count:model.dhcp_leases;
   if(count.valid)lv_label_set_text_fmt(net_detail_values[i],i==2?"%.0f 条":"%.0f 台",count.value);
   else lv_label_set_text(net_detail_values[i],"-");
  }
 }
 if(weather_label){
  if(model.weather_temperature.valid)lv_label_set_text_fmt(weather_label,"%s %.1f°C",weather_condition_name(model.weather_condition),model.weather_temperature.value);
  else lv_label_set_text(weather_label,"天气未接入");
 }
 if(weather_location_label)lv_label_set_text(weather_location_label,"深圳市光明区");
 if(weather_quality_label){
  gso_quality q=gso_source_quality(model.weather,desk_data_now_ms(),600000,1800000);
  if(q==GSO_OK&&model.humidity.valid)lv_label_set_text_fmt(weather_quality_label,"湿度 %.0f%%",model.humidity.value);
  else lv_label_set_text(weather_quality_label,q==GSO_OK||q==GSO_UNCONFIGURED?"":quality_text(q));
 }

 for(int i=0;i<2;i++){
  const gso_hardware *h=i?&model.gpu:&model.cpu;
  if(main_card_visible(0))render_hardware(&main_hardware[i],h);
  render_hardware(&detail_hardware[i],h);
 }

 if(rx_text){
  if(model.download.valid)lv_label_set_text_fmt(rx_text,"%.2f",model.download.value/1000000);
  else lv_label_set_text(rx_text,"-");
 }
 if(tx_text){if(model.upload.valid)lv_label_set_text_fmt(tx_text,"%.2f",model.upload.value/1000000);else lv_label_set_text(tx_text,"-");}
 if(wifi_icon){
  gss_wifi_status wifi;
  gss_wifi_radio_status(&wifi);
  lv_obj_set_style_text_color(wifi_icon,c(wifi.connected?GREEN:wifi.ready?AMBER:RED),0);
  lv_obj_set_style_opa(wifi_icon,wifi.connected?255:120,0);
 }
 if(source_label){
  gso_quality q=gso_source_quality(model.windows,desk_data_now_ms(),3000,10000);
  desk_connection_status status=desk_data_status();
  const char *label=status==DESK_CONNECTING?"连接 GSN 中":status==DESK_NETWORK_OFFLINE?"网络离线":status==DESK_GSN_OFFLINE?"GSN 离线":status==DESK_GSN_AUTH?"GSN 认证失败":status==DESK_GSN_RESPONSE?"GSN 响应异常":status==DESK_ONLINE&&model.received_ms&&desk_data_now_ms()-model.received_ms>2500?"GSN 重连中":q==GSO_OK?"实时":q==GSO_OFFLINE||q==GSO_UNCONFIGURED?"GSW 离线":q==GSO_STALE?"GSW 数据过期":"GSW 采集异常";
  uint32_t label_color=status==DESK_ONLINE&&q==GSO_OK?GREEN:AMBER;
  /* A hard disk the NAS reports as worn out is told on every page, in place of the connection line (which stays for GSN
     trouble: the figures are only as fresh as that link); tapping the line opens the health page. */
  int disk_alert=status==DESK_ONLINE&&!(model.received_ms&&desk_data_now_ms()-model.received_ms>2500)?nas_health_alert():0;
  if(disk_alert){label=disk_alert>1?"硬盘有风险":"硬盘需留意";label_color=disk_alert>1?RED:AMBER;}
  source_alert=disk_alert;
  lv_label_set_text(source_label,label);
  lv_obj_set_style_text_color(source_label,c(label_color),0);
 }
 for(int i=0;i<3;i++)if(disk_labels[i]){
  if(i<(int)model.disk_count){
   const gso_disk *d=&model.disks[i];
   lv_label_set_text(disk_labels[i],d->id);
   lv_label_set_text(disk_models[i],d->model);
   if(d->total.valid&&d->free.valid){
    lv_label_set_text_fmt(disk_totals[i],"总量 %.1f GB",d->total.value/1073741824.0);
    lv_label_set_text_fmt(disk_free[i],"可用 %.1f GB",d->free.value/1073741824.0);
   }
   else{lv_label_set_text(disk_totals[i],"总量 -");lv_label_set_text(disk_free[i],"可用 -");}
   lv_bar_set_value(disk_bars[i],d->total.valid&&d->free.valid&&d->total.value>0?(int)((d->total.value-d->free.value)/d->total.value*100):0,LV_ANIM_OFF);
   disk_activity_text(disk_busy[i],disk_read[i],disk_write[i],disk_response[i],d);
   disk_ring_value(disk_ring[i],d);
  }else{
   disk_activity_text(disk_busy[i],disk_read[i],disk_write[i],disk_response[i],NULL);
   disk_ring_value(disk_ring[i],NULL);
   lv_label_set_text(disk_labels[i],"-");
   lv_label_set_text(disk_models[i],"未接入");
   lv_label_set_text(disk_totals[i],"");
   lv_label_set_text(disk_free[i],"");
   lv_bar_set_value(disk_bars[i],0,LV_ANIM_OFF);
  }
 }
 value_text(fps_label,model.fps,"");
 if(latency_label){
  if(model.latency.valid){
   lv_obj_set_style_text_font(latency_label,model.latency.value>=1000?&lv_font_montserrat_24:model.latency.value>=100?&lv_font_montserrat_32:&lv_font_montserrat_40,0);
   lv_label_set_text_fmt(latency_label,"%.2f ms",model.latency.value);
  }else{
   lv_obj_set_style_text_font(latency_label,&lv_font_montserrat_40,0);
   lv_label_set_text(latency_label,"-");
  }
 }
 if(game_icon_image){
  if(model.game_icon_valid){
   if(!game_icon_desc.data||memcmp(game_icon_desc.data,model.game_icon,sizeof(model.game_icon))){
    static uint8_t pixels[2304];
    lv_image_cache_drop(&game_icon_desc);
    memcpy(pixels,model.game_icon,sizeof(pixels));
    game_icon_desc.data=pixels;
    lv_image_set_src(game_icon_image,&game_icon_desc);
    lv_obj_invalidate(game_icon_image);
   }
   if(lv_image_get_src(game_icon_image)!=&game_icon_desc)lv_image_set_src(game_icon_image,&game_icon_desc);
   lv_obj_remove_flag(game_icon_image,LV_OBJ_FLAG_HIDDEN);
  }else lv_obj_add_flag(game_icon_image,LV_OBJ_FLAG_HIDDEN);
 }
 if(game_process_label){
  char display_name[641];
  snprintf(display_name,sizeof(display_name),"%s",model.game_process);
  size_t length=strlen(display_name);
  if(length>=4&&(display_name[length-4]=='.')&&(display_name[length-3]=='e'||display_name[length-3]=='E')&&(display_name[length-2]=='x'||display_name[length-2]=='X')&&(display_name[length-1]=='e'||display_name[length-1]=='E'))display_name[length-4]=0;
 gso_quality q=gso_source_quality(model.game,desk_data_now_ms(),3000,10000);
 lv_label_set_text_fmt(game_process_label,q==GSO_OK?"%s":"%s / %s",display_name[0]?display_name:"WAITING FOR GAME",quality_text(q));
 }

 if(game_chart){
  for(unsigned i=0;i<GSO_GAME_HISTORY_POINTS;i++){
   int index=(int)i-((int)GSO_GAME_HISTORY_POINTS-(int)model.fps_history_count);
   gso_metric sample=index>=0?model.fps_history[index]:gso_metric_missing("no_history");
   lv_chart_set_next_value(game_chart,fps_series,sample.valid?(int32_t)fmin(sample.value,240):LV_CHART_POINT_NONE);
  }
 }
}
void desk_ui_apply_snapshot(const gso_snapshot *snapshot){if(snapshot){model=*snapshot;if(strip_frozen)telemetry_pending=true;else render();}}
static void hardware_gauge(lv_obj_t *p,int x,int y,int size,hardware_view *v,const char *name,uint32_t color){
 lv_obj_t *frame=box(p,x,y,size,(size*86+99)/100,0,0);
 lv_obj_set_style_bg_opa(frame,LV_OPA_TRANSP,0);
 lv_obj_remove_flag(frame,LV_OBJ_FLAG_CLICKABLE);
 v->arc=lv_arc_create(frame);
 lv_obj_remove_style_all(v->arc);
 lv_obj_set_pos(v->arc,0,0);
 lv_obj_set_size(v->arc,size,size);
 lv_arc_set_bg_angles(v->arc,135,45);
 lv_arc_set_range(v->arc,0,100);
 lv_obj_remove_flag(v->arc,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_style_arc_color(v->arc,c(LINE),LV_PART_MAIN);
 lv_obj_set_style_arc_width(v->arc,8,LV_PART_MAIN);
 lv_obj_set_style_arc_color(v->arc,c(color),LV_PART_INDICATOR);
 lv_obj_set_style_arc_width(v->arc,8,LV_PART_INDICATOR);
 v->value=text(v->arc,0,0,"-",&lv_font_montserrat_32,TEXT);
 lv_obj_align(v->value,LV_ALIGN_CENTER,0,-12);
 lv_obj_t *label=small(v->arc,0,0,name,MUTED);
 lv_obj_align(label,LV_ALIGN_CENTER,0,13);
 if(!strncmp(name,"CPU",3)||!strncmp(name,"GPU",3)){
  v->clock=small(v->arc,0,0,"— MHz",MUTED);
  lv_obj_align(v->clock,LV_ALIGN_CENTER,0,36);
 }
}
static lv_obj_t *card(int slot,const char *name,int id){
 lv_obj_t *o=box(viewport,main_card_position[slot]*350,0,334,420,PANEL,16);
 if(slot>=0&&slot<7)main_cards[slot]=o;
 /* Header: accent bar, letter-spaced module name, muted caption on the right and a hairline below. */
 lv_obj_t *accent=box(o,20,22,4,24,BLUE,2);
 lv_obj_remove_flag(accent,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *title=text(o,34,20,name,&lv_font_montserrat_24,TEXT);
 lv_obj_set_style_text_letter_space(title,3,0);
 static const char *captions[7]={"硬件","AI 助手","网络","家居","3D 打印","车辆","主机"};
 if(slot>=0&&slot<7){
  lv_obj_t *caption=text(o,0,0,captions[slot],&desk_font_16,MUTED);
  lv_obj_align(caption,LV_ALIGN_TOP_RIGHT,-20,26);
 }
 lv_obj_t *rule=box(o,20,58,294,1,LINE,0);
 lv_obj_remove_flag(rule,LV_OBJ_FLAG_CLICKABLE);
 clickable(o,id);
 return o;
}
static void modules(void){
 lv_obj_t *w=card(0,"WIN",10);
 for(int i=0;i<2;i++){
  int x=20+i*154;
  hardware_view *v=&main_hardware[i];
  hardware_gauge(w,x,68,136,v,i?"GPU %":"CPU %",i?BLUE:GREEN);
  small(w,x,213,"温度",MUTED);
  v->temperature=cn(w,x+44,213,"—",i?BLUE:GREEN);
  v->temperature_normal=i?BLUE:GREEN;
  lv_obj_set_width(v->temperature,92);
  lv_obj_set_style_text_align(v->temperature,LV_TEXT_ALIGN_RIGHT,0);
  small(w,x,249,"功耗",MUTED);
  v->power=cn(w,x+44,249,"—",TEXT);
  lv_obj_set_width(v->power,92);
  lv_obj_set_style_text_align(v->power,LV_TEXT_ALIGN_RIGHT,0);
  small(w,20,300+i*62,i?"VRAM":"RAM",MUTED);
  v->compact=true;
  v->memory=small(w,64,300+i*62,"—",TEXT);
  lv_obj_set_width(v->memory,246);
  lv_label_set_long_mode(v->memory,LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(v->memory,LV_TEXT_ALIGN_RIGHT,0);
  v->memory_bar=bar(w,20,326+i*62,290,i?BLUE:GREEN);
 }


 lv_obj_t *v=card(1,"VIBE",5);
 vibe_states[0]=cn(v,20,80,"未运行",AMBER);
 quota_source_labels[0]=provider_badge(v,0,-20,78);
 vibe_titles[0]=small(v,20,117,"",TEXT);
 lv_obj_set_size(vibe_titles[0],290,21);
 lv_label_set_long_mode(vibe_titles[0],LV_LABEL_LONG_DOT);
 vibe_durations[0]=small(v,20,146,"",MUTED);
 for(int i=0;i<2;i++){
  int y=183+i*120;
  small(v,20,y,i?"每周剩余":"5 小时剩余",MUTED);
  quota_values[0][i]=text(v,160,y-9,"-",&lv_font_montserrat_40,TEXT);
  lv_obj_set_width(quota_values[0][i],150);
  lv_obj_set_style_text_align(quota_values[0][i],LV_TEXT_ALIGN_RIGHT,0);
  quota_bars[0][i]=bar(v,20,y+47,290,i?BLUE:GREEN);
  quota_countdowns[0][i]=text(v,20,y+74,"不可用",&desk_font_countdown,MUTED);
  lv_obj_set_width(quota_countdowns[0][i],118);
  quota_updates[0][i]=small(v,0,0,"",MUTED);
  lv_obj_align(quota_updates[0][i],LV_ALIGN_TOP_RIGHT,-20,y+74);
  quota_update_words[0][i]=fine(v,0,0,"刷新",MUTED);
 }
 lv_obj_t *n=card(2,"NET",11);
 vpn_reset_words[0]=fine(n,20,67,"刷新",MUTED);
 vpn_reset[0]=small(n,56,65,"--",MUTED);
 vpn_quality_labels[0]=small(n,150,65,"未接入",AMBER);
 lv_obj_set_width(vpn_quality_labels[0],160);
 lv_obj_set_style_text_align(vpn_quality_labels[0],LV_TEXT_ALIGN_RIGHT,0);
 hardware_view vpn={0};
 hardware_gauge(n,89,100,156,&vpn,"GB 剩余",BLUE);
 vpn_labels[0]=vpn.value;
 vpn_arcs[0]=vpn.arc;
 vpn_percent[0]=small(vpn.arc,0,0,"—",BLUE);
 lv_obj_align(vpn_percent[0],LV_ALIGN_CENTER,0,36);
 net_mode_controls(n,0,20,254,294);
 net_stack_row(n,0,344,294);
 lv_obj_t *h=card(3,"HOME",15);home_summary(h);
 print_card();
 car_card();
 nas_card();
}
#include "transition_cache.inc"
#include "strip_freeze.inc"
static lv_obj_t *retiring_panel;
static void panel_x(void *p,int32_t v){lv_obj_set_style_translate_x(p,v,0);}
static void panel_y(void *p,int32_t v){lv_obj_set_style_translate_y(p,v,0);}
static void animation_lifetime(void *p,int32_t v){(void)p;(void)v;}
static void panel_finished(lv_anim_t *a){lv_obj_t *p=a->var;if(retiring_panel==p)retiring_panel=NULL;lv_obj_delete(p);}
static void panel_animate(lv_obj_t *p,lv_anim_exec_xcb_t exec,int from,int to,int duration,bool remove){
 interaction_hold((uint32_t)duration+40);
 lv_anim_t a;
 lv_anim_init(&a);
 lv_anim_set_var(&a,p);
 lv_anim_set_exec_cb(&a,exec);
 lv_anim_set_values(&a,from,to);
 lv_anim_set_duration(&a,duration);
 lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
 if(remove)lv_anim_set_completed_cb(&a,panel_finished);
 lv_anim_start(&a);
}
static void panel_retire(lv_obj_t *p,bool game){
 /* Entering only slides the small heading; leaving mirrors that cheaply by removing the page at once. No snapshot, no
    full-panel animation, so returning never costs a 1.7 MB image or a full redraw per frame. */
 (void)game;if(retiring_panel){lv_anim_delete(retiring_panel,NULL);lv_obj_delete(retiring_panel);retiring_panel=NULL;}
 lv_obj_delete(p);
}
/* The always-visible sidebar follows the page palette: game mode uses the deep indigo / cyan / violet scheme of the GAME page. */
static void sidebar_game_theme(bool game){
 if(!sidebar)return;
 uint32_t background=game?0x0B0C1D:INSET,text_color=game?0xF6F3FF:TEXT,muted=game?0x9EABCF:MUTED,rx=game?0x31E6EF:BLUE,tx=game?0xB58BFF:GREEN,key=game?0x252E50:LINE;
 /* The page background behind both panels: raw colours, so the day theme does not turn it light while a dark GAME page is shown. */
 lv_obj_set_style_bg_color(root,game?lv_color_hex(0x070814):c(BG),0);
 if(viewport){
  lv_obj_set_style_bg_color(viewport,game?lv_color_hex(0x070814):c(BG),0);
  if(game)lv_obj_add_flag(viewport,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(viewport,LV_OBJ_FLAG_HIDDEN);
 }
 /* the cards behind the panel would show through its rounded corners */
 lv_obj_set_style_bg_color(sidebar,game?lv_color_hex(background):c(background),0);
 if(traffic_box)lv_obj_set_style_bg_color(traffic_box,game?lv_color_hex(background):c(background),0);
 lv_obj_t *plain[]={sidebar_title,clock_label};for(unsigned i=0;i<2;i++)if(plain[i])lv_obj_set_style_text_color(plain[i],c(text_color),0);
 lv_obj_t *soft[]={date_label,weather_label,weather_quality_label,weather_location_label,weather_location_pin,traffic_unit[0],traffic_unit[1]};
 for(unsigned i=0;i<7;i++)if(soft[i])lv_obj_set_style_text_color(soft[i],c(muted),0);
 if(traffic_arrow[0])lv_obj_set_style_text_color(traffic_arrow[0],c(rx),0);
 if(rx_text)lv_obj_set_style_text_color(rx_text,c(rx),0);
 if(traffic_arrow[1])lv_obj_set_style_text_color(traffic_arrow[1],c(tx),0);
 if(tx_text)lv_obj_set_style_text_color(tx_text,c(tx),0);
 if(side_button){
  lv_obj_set_style_bg_color(side_button,c(key),0);
  lv_obj_set_style_text_color(lv_obj_get_child(side_button,0),c(text_color),0);
 }
 lv_obj_invalidate(sidebar);
}
uint32_t desk_ui_root_background(void){return root?lv_color_to_u32(lv_obj_get_style_bg_color(root,0))&0xFFFFFF:0;}
uint32_t desk_ui_sidebar_background(void){return sidebar?lv_color_to_u32(lv_obj_get_style_bg_color(sidebar,0))&0xFFFFFF:0;}
void desk_ui_close_detail(void){
 net_close_controls();
 wifi_view_close();
settings_view_close();
level_view_close();
bool was_game=detail_id==16;
if(was_game)sidebar_game_theme(false);
home_close_views();
close_weather();
memset(net_detail_values,0,sizeof(net_detail_values));
vibe_states[1]=vibe_titles[1]=vibe_durations[1]=NULL;
for(int g=1;g<3;g++){
 memset(quota_values[g],0,sizeof(quota_values[g]));
 memset(quota_bars[g],0,sizeof(quota_bars[g]));
 memset(quota_countdowns[g],0,sizeof(quota_countdowns[g]));
 memset(quota_updates[g],0,sizeof(quota_updates[g]));
 memset(quota_update_words[g],0,sizeof(quota_update_words[g]));
}
quota_source_labels[1]=NULL;
for(int slot=1;slot<8;slot++)quota_icons[slot]=NULL;
memset(client_status,0,sizeof(client_status));
print_detail_closed();
car_detail_closed();
nas_detail_closed();
memset(client_tokens,0,sizeof(client_tokens));
game_chart=NULL;
game_icon_image=NULL;
fps_series=NULL;
memset(service_labels[1],0,sizeof(service_labels[1]));
memset(stack_labels[1],0,sizeof(stack_labels[1]));
vpn_labels[1]=vpn_total_labels[1]=vpn_arcs[1]=vpn_quality_labels[1]=vpn_percent[1]=vpn_reset[1]=vpn_reset_words[1]=NULL;
game_process_label=NULL;
memset(detail_hardware,0,sizeof(detail_hardware));
memset(disk_labels,0,sizeof(disk_labels));
memset(disk_models,0,sizeof(disk_models));
memset(disk_totals,0,sizeof(disk_totals));
memset(disk_free,0,sizeof(disk_free));
memset(disk_bars,0,sizeof(disk_bars));
memset(disk_busy,0,sizeof(disk_busy));
memset(disk_ring,0,sizeof(disk_ring));
memset(disk_read,0,sizeof(disk_read));
memset(disk_write,0,sizeof(disk_write));
memset(disk_response,0,sizeof(disk_response));
fps_label=latency_label=NULL;
detail_id=0;
desk_ui_game_view_closed();
if(overlay){
 lv_obj_t *old=overlay;
 overlay=NULL;
 panel_retire(old,was_game);
}
if(side_button)lv_label_set_text(lv_obj_get_child(side_button,0),"设置");
}
bool desk_ui_detail_is_open(void){return overlay!=NULL;}
bool desk_ui_network_is_visible(void){return sidebar!=NULL;}
int desk_ui_current_page(void){return page;}
void desk_ui_page(int p){desk_ui_cancel_snapshot_animations();desk_ui_close_detail();page=p<0?0:p>page_limit()?page_limit():p;lv_obj_scroll_to_x(viewport,page*350,LV_ANIM_OFF);render();}
void desk_ui_pause(void){paused=!paused;}
uint32_t desk_ui_sample_sequence(void){return sequence;}
static void game_cb(lv_event_t *e){(void)e;desk_ui_game_manual_open();}
static void boot_progress(void *p,int32_t v){lv_obj_set_width(p,v);}
static void game_boot_deleted(lv_event_t *event){if(game_boot_panel==lv_event_get_target_obj(event))game_boot_panel=NULL;}
void desk_ui_cancel_snapshot_animations(void){
 if(viewport){
  lv_anim_delete(viewport,NULL);
  strip_thaw();
  int target=(lv_obj_get_scroll_x(viewport)+175)/350;
  page=target<0?0:target>page_limit()?page_limit():target;
  lv_obj_scroll_to_x(viewport,page*350,LV_ANIM_OFF);
 }
 if(game_boot_panel)lv_obj_delete(game_boot_panel);
}
static void game_boot(lv_obj_t *parent){
 lv_obj_t *boot=box(parent,0,0,1032,420,0x0A1022,16);lv_obj_add_flag(boot,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *title=text(boot,0,0,"GAME MODE",&lv_font_montserrat_40,0xE5E5FF);lv_obj_align(title,LV_ALIGN_CENTER,0,-38);
 lv_obj_t *subtitle=text(boot,0,0,"PERFORMANCE MONITOR",&lv_font_montserrat_18,0x9EABCF);lv_obj_align(subtitle,LV_ALIGN_CENTER,0,12);
 box(boot,352,257,320,3,0x252E50,2);
 /* Drawn live: a plain panel with two lines of text costs less to draw than a 1032x420 snapshot took to take (board: most
    of the 73 ms the GAME page needed to open), and only the beam changes while it shows. */
 game_boot_panel=boot;
 lv_obj_add_event_cb(boot,game_boot_deleted,LV_EVENT_DELETE,NULL);
 lv_obj_t *beam=box(boot,352,257,1,3,0x31E6EF,2);panel_animate(beam,boot_progress,1,320,340,false);
 panel_animate(boot,animation_lifetime,0,1,380,true);
 interaction_hold(420);
}
static lv_obj_t *panel(const char *title){
 desk_ui_cancel_snapshot_animations();
 desk_ui_close_detail();
 if(retiring_panel){
  lv_anim_delete(retiring_panel,NULL);
  lv_obj_delete(retiring_panel);
  retiring_panel=NULL;
 }
 overlay=box(root,232,16,1032,420,PANEL,16);
 lv_obj_t *heading=cn(overlay,22,12,title,TEXT);
 lv_obj_set_size(heading,550,26);
 lv_label_set_long_mode(heading,LV_LABEL_LONG_DOT);
 lv_label_set_text(lv_obj_get_child(side_button,0),"返回");
 bool game=!strncmp(title,"GAME",4);
/* Page content stays in place; only its small heading enters. */
panel_animate(heading,game?panel_y:panel_x,game?8:12,0,140,false);return overlay;}
#include "weather_ui.inc"
#include "print_ui.inc"
#include "car_ui.inc"
#include "nas_ui.inc"
#include "level_ui.inc"
void desk_ui_detail(int id){
 if(id!=22)return_to_nas=false;
 if(id==10){lv_obj_t *p=panel("WIN · 性能状态");detail_id=id;button(p,820,12,184,"游戏模式",game_cb);
  for(int i=0;i<2;i++){int x=20+i*224;hardware_view *v=&detail_hardware[i];
   v->name=small(p,x,52,"",TEXT);lv_obj_set_size(v->name,208,48);lv_label_set_long_mode(v->name,LV_LABEL_LONG_WRAP);
   hardware_gauge(p,x+24,108,160,v,i?"GPU %":"CPU %",i?BLUE:GREEN);

   small(p,x,268,"温度",MUTED);v->temperature=cn(p,x+58,268,"",i?BLUE:GREEN);v->temperature_normal=i?BLUE:GREEN;
   small(p,x,303,"功耗",MUTED);v->power=cn(p,x+58,303,"",TEXT);
   small(p,x,333,i?"VRAM":"RAM",MUTED);
   v->memory_rate=small(p,x+70,333,"",MUTED);
   lv_obj_set_width(v->memory_rate,138);
   lv_obj_set_style_text_align(v->memory_rate,LV_TEXT_ALIGN_RIGHT,0);
   v->memory=small(p,x,352,"",TEXT);
   lv_obj_set_width(v->memory,208);
   v->memory_bar=bar(p,x,379,208,i?BLUE:GREEN);
  }
  for(int i=0;i<3;i++){
   lv_obj_t *d=box(p,480,76+i*109,524,100,INSET,12);
   disk_labels[i]=cn(d,18,6,"-",BLUE);
   disk_models[i]=small(d,80,10,"未接入",TEXT);
   lv_obj_set_width(disk_models[i],250);
   lv_label_set_long_mode(disk_models[i],LV_LABEL_LONG_DOT);
   disk_busy[i]=small(d,328,10,"",TEXT);
   lv_obj_set_width(disk_busy[i],146);
   lv_obj_set_style_text_align(disk_busy[i],LV_TEXT_ALIGN_RIGHT,0);
   disk_ring[i]=lv_arc_create(d);
   lv_obj_remove_style_all(disk_ring[i]);
   lv_obj_set_pos(disk_ring[i],482,7);
   lv_obj_set_size(disk_ring[i],24,24);
   lv_arc_set_rotation(disk_ring[i],270);
   lv_arc_set_bg_angles(disk_ring[i],0,360);
   lv_arc_set_range(disk_ring[i],0,100);
   lv_obj_remove_flag(disk_ring[i],LV_OBJ_FLAG_CLICKABLE);
   lv_obj_set_style_arc_color(disk_ring[i],c(LINE),LV_PART_MAIN);
   lv_obj_set_style_arc_width(disk_ring[i],4,LV_PART_MAIN);
   lv_obj_set_style_arc_color(disk_ring[i],c(MUTED),LV_PART_INDICATOR);
   lv_obj_set_style_arc_width(disk_ring[i],4,LV_PART_INDICATOR);
   disk_totals[i]=small(d,18,42,"",MUTED);
   disk_free[i]=small(d,166,42,"",TEXT);
   lv_obj_set_width(disk_free[i],158);
   lv_obj_set_style_text_align(disk_free[i],LV_TEXT_ALIGN_RIGHT,0);
   box(d,344,36,1,52,LINE,0);
   const char *words[3]={"读速率","写速率","响应时间"};
   lv_obj_t **values[3]={&disk_read[i],&disk_write[i],&disk_response[i]};
   for(int k=0;k<3;k++){
    fine(d,356,38+k*18,words[k],MUTED);
    *values[k]=fine(d,420,38+k*18,"",TEXT);
    lv_obj_set_width(*values[k],86);
    lv_obj_set_style_text_align(*values[k],LV_TEXT_ALIGN_RIGHT,0);
   }
   disk_bars[i]=bar(d,18,76,306,BLUE);
   lv_obj_set_height(disk_bars[i],6);
  }

 }else if(id==16){
  lv_obj_t *p=panel("GAME · 游戏性能");
  detail_id=id;
  lv_obj_set_style_bg_color(p,lv_color_hex(0x101126),0);
  lv_obj_set_style_text_color(lv_obj_get_child(p,0),lv_color_hex(0xF6F3FF),0);
  game_icon_image=lv_image_create(p);
  lv_obj_set_pos(game_icon_image,578,13);
  lv_obj_set_size(game_icon_image,24,24);
  lv_obj_add_flag(game_icon_image,LV_OBJ_FLAG_HIDDEN);
  game_process_label=small(p,614,16,"WAITING FOR GAME",0x9EABCF);
  lv_obj_set_width(game_process_label,386);
  lv_label_set_long_mode(game_process_label,LV_LABEL_LONG_DOT);
  lv_obj_t *fps=box(p,20,52,214,158,0x191B34,10),*latency=box(p,250,52,222,158,0x191B34,10);
  small(fps,18,12,"帧率 FPS",0x9EABCF);
  fps_label=text(fps,18,49,"-",&lv_font_montserrat_48,0x31E6EF);
  small(latency,18,12,"渲染延迟",0x9EABCF);
  latency_label=text(latency,18,49,"-",&lv_font_montserrat_40,0xB58BFF);
  lv_obj_t *curve=box(p,488,52,516,158,0x191B34,10);
  small(curve,18,12,"帧率曲线",0x9EABCF);
  small(curve,361,12,"最近 24 秒",0x9EABCF);
  game_chart=lv_chart_create(curve);
  lv_obj_remove_style_all(game_chart);
  lv_obj_set_pos(game_chart,42,48);
  lv_obj_set_size(game_chart,454,88);
  lv_chart_set_point_count(game_chart,GSO_HISTORY_POINTS);
  lv_chart_set_axis_range(game_chart,LV_CHART_AXIS_PRIMARY_Y,0,240);
  fps_series=lv_chart_add_series(game_chart,lv_color_hex(0x31E6EF),LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_values(game_chart,fps_series,LV_CHART_POINT_NONE);
  lv_obj_set_style_line_color(game_chart,lv_color_hex(0x343957),LV_PART_MAIN);
  lv_obj_set_style_line_width(game_chart,1,LV_PART_MAIN);
  lv_obj_set_style_line_width(game_chart,2,LV_PART_ITEMS);
  for(int i=0;i<2;i++){
   hardware_view *v=&detail_hardware[i];
   uint32_t accent=i?0xB58BFF:0x31E6EF;
   lv_obj_t *h=box(p,20+i*502,222,482,192,0x191B34,10);
   v->name=small(h,18,12,"",0x9EABCF);
   lv_obj_set_width(v->name,446);
   lv_label_set_long_mode(v->name,LV_LABEL_LONG_DOT);
   small(h,18,44,"核心占用 %",0x9EABCF);
   v->value=text(h,18,66,"-",&lv_font_montserrat_32,accent);
   v->load_bar=bar(h,18,111,143,accent);
   lv_obj_set_style_bg_color(v->load_bar,lv_color_hex(0x343957),LV_PART_MAIN);
   small(h,202,44,"温度",0x9EABCF);
   v->temperature=text(h,202,73,"-",&lv_font_montserrat_24,0xF6F3FF);
   v->temperature_normal=0xF6F3FF;
   small(h,346,44,"功耗",0x9EABCF);
   v->power=text(h,346,73,"-",&lv_font_montserrat_24,0xF6F3FF);
   small(h,18,140,i?"VRAM":"RAM",0x9EABCF);
   v->memory=small(h,74,140,"—",0xF6F3FF);
   lv_obj_set_width(v->memory,390);
   v->clock=small(h,84,66+lv_font_montserrat_32.line_height-lv_font_montserrat_32.base_line-desk_font_16.line_height+desk_font_16.base_line,"— MHz",0x9EABCF);
   lv_obj_set_width(v->clock,96);
   lv_obj_set_style_text_align(v->clock,LV_TEXT_ALIGN_RIGHT,0);
   v->memory_bar=bar(h,18,166,446,accent);
   lv_obj_set_style_bg_color(v->memory_bar,lv_color_hex(0x343957),LV_PART_MAIN);
  }
 }else if(id==5){
  lv_obj_t *p=panel("VIBE · 任务与额度");
  detail_id=id;
  vibe_states[1]=cn(p,24,66,"未运行",AMBER);
  quota_source_labels[1]=provider_badge(p,1,-662,66); /* right-aligned to the left column's edge (x=370) */
  vibe_titles[1]=small(p,24,100,"",TEXT);
  lv_obj_set_size(vibe_titles[1],345,54);
  lv_label_set_long_mode(vibe_titles[1],LV_LABEL_LONG_DOT);
  vibe_durations[1]=small(p,24,164,"",MUTED);
  for(int r=0;r<2;r++){
   lv_obj_t *d=box(p,24,248+r*76,346,68,INSET,12);
   quota_icons[6+r]=provider_icon(d,16,13,6+r);
   provider_icon_set(6+r,(unsigned)r,true);
   small(d,46,10,gso_vibe_provider_name((unsigned)r),TEXT);
   client_status[r]=small(d,150,10,"-",MUTED);
   lv_obj_set_width(client_status[r],180);
   lv_obj_set_style_text_align(client_status[r],LV_TEXT_ALIGN_RIGHT,0);
   client_tokens[r]=small(d,16,37,"-",MUTED);
   lv_obj_set_width(client_tokens[r],314);
  }
  for(int r=0;r<2;r++)for(int i=0;i<2;i++){
   int g=r+1;
   lv_obj_t *q=box(p,397+i*306,56+r*176,286,164,INSET,12);
   int slot=2+r*2+i;
   quota_icons[slot]=provider_icon(q,16,14,slot);
   provider_icon_set(slot,(unsigned)r,true);
   lv_obj_t *name=small(q,44,12,"",MUTED);
   lv_label_set_text_fmt(name,"%s · %s",gso_vibe_provider_name((unsigned)r),i?"每周剩余":"5 小时剩余");
   quota_values[g][i]=text(q,126,46,"-",&lv_font_montserrat_40,TEXT);
   lv_obj_set_width(quota_values[g][i],144);
   lv_obj_set_style_text_align(quota_values[g][i],LV_TEXT_ALIGN_RIGHT,0);
   quota_bars[g][i]=bar(q,16,102,254,i?BLUE:GREEN);
   quota_countdowns[g][i]=text(q,14,128,"不可用",&desk_font_countdown,TEXT);
   lv_obj_set_width(quota_countdowns[g][i],96);
   quota_updates[g][i]=small(q,0,0,"",MUTED);
   lv_obj_align(quota_updates[g][i],LV_ALIGN_TOP_RIGHT,-16,130);
   quota_update_words[g][i]=fine(q,0,0,"刷新",MUTED);
  }
 }else if(id==11||id==14){lv_obj_t *p=panel("NET · 网络服务状态");detail_id=id;
  lv_obj_t *left=box(p,20,60,330,340,INSET,12);
  vpn_reset_words[1]=fine(left,20,20,"刷新",MUTED);
  vpn_reset[1]=small(left,56,18,"--",MUTED);
  hardware_view vpn={0};
  hardware_gauge(left,77,44,176,&vpn,"GB 剩余",BLUE);
  vpn_labels[1]=vpn.value;
  vpn_arcs[1]=vpn.arc;
  vpn_percent[1]=small(vpn.arc,0,0,"-",BLUE);
  lv_obj_align(vpn_percent[1],LV_ALIGN_CENTER,0,36);
  vpn_quality_labels[1]=small(left,150,18,"未接入",AMBER);
  lv_obj_set_width(vpn_quality_labels[1],160);
  lv_obj_set_style_text_align(vpn_quality_labels[1],LV_TEXT_ALIGN_RIGHT,0);
  box(left,20,214,290,1,LINE,0);
  net_mode_controls(left,1,20,230,290);
  lv_obj_t *right=box(p,370,60,634,340,INSET,12);net_service_row(right,1,16,594);net_stack_row(right,1,76,594);
  box(right,20,132,594,1,LINE,0);
  const char *labels[]={"代理订阅更新时间","拦截规则更新时间","生效规则数量","已接入设备台数"};
  for(int i=0;i<4;i++){
   small(right,20,148+i*46,labels[i],MUTED);
   net_detail_values[i]=small(right,270,148+i*46,"-",TEXT);
   lv_obj_set_width(net_detail_values[i],344);
   lv_obj_set_style_text_align(net_detail_values[i],LV_TEXT_ALIGN_RIGHT,0);
  }
 }else if(id==18){print_detail();}else if(id==20){car_detail();}else if(id==21){nas_detail();}else if(id==22){nas_health_detail();}else if(id==19){level_open();}else if(id==17){weather_details(panel("深圳市光明区 · 天气"));detail_id=id;
 }else if(id==15){lv_obj_t *p=panel("HOME · 常用设备");detail_id=id;home_details(p);}
 if(id==16&&overlay){sidebar_game_theme(true);game_boot(overlay);}
 render();
}
#include "wifi_ui.inc"
#include "settings_ui.inc"

/* The VPN quota resets on the 24th of every month. The source gives no time of day, so the countdown is in whole
   calendar days (UTC+8); the reset date is the next 24th. */
static long vpn_civil_days(int year,int month,int day){
 year-=month<=2;
 long era=(year>=0?year:year-399)/400;
 unsigned yoe=(unsigned)(year-era*400);
 unsigned doy=(unsigned)((153*(month+(month>2?-3:9))+2)/5+day-1);
 unsigned doe=yoe*365+yoe/4-yoe/100+doy;
 return era*146097L+(long)doe-719468L;
}
int desk_ui_vpn_reset_days(int64_t now_utc,int *month,int *day){
 time_t local=(time_t)(now_utc+8*3600);struct tm *t=gmtime(&local);if(!t){if(month)*month=0;if(day)*day=0;return 0;}
 int year=t->tm_year+1900,current=t->tm_mon+1,next_month=t->tm_mday<24?current:current+1,next_year=year;
 if(next_month>12){
  next_month=1;
  next_year++;
 }
 if(month)*month=next_month;
 if(day)*day=24;
 return (int)(vpn_civil_days(next_year,next_month,24)-vpn_civil_days(year,current,t->tm_mday));
}
void desk_ui_countdown_text(int64_t reset,int64_t now,char *out,size_t length){
 int64_t seconds=reset>now?reset-now:0;
 if(!seconds){
  snprintf(out,length,"待刷新");
  return;
 }
 if(seconds>=86400)snprintf(out,length,"%lld天 %02lld:%02lld",(long long)(seconds/86400),(long long)(seconds/3600%24),(long long)(seconds/60%60));
 else snprintf(out,length,"%02lld:%02lld:%02lld",(long long)(seconds/3600),(long long)(seconds/60%60),(long long)(seconds%60));
}
static void clock_tick(lv_timer_t *t){
 if(t&&interaction_busy())return;
 time_t now=time(NULL)+8*3600;
 struct tm *tm=gmtime(&now);
 if(tm){
  lv_label_set_text_fmt(clock_label,"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec);
  lv_label_set_text_fmt(date_label,"%02d月%02d日",tm->tm_mon+1,tm->tm_mday);
 }
 desk_ui_theme_auto();
}
static void telemetry_commit(lv_timer_t *t){
 (void)t;
 strip_thaw_if_idle(); /* interrupted snaps release their cache within this 50 ms tick */
 if(paused||!telemetry_pending||interaction_busy())return;
 telemetry_pending=false;
 render();
 gso_quality q=gso_source_quality(model.game,desk_data_now_ms(),3000,10000);
 desk_ui_game_sample(model.game_process,q==GSO_OK&&model.rendering);
 desk_ui_game_source(q==GSO_OFFLINE||q==GSO_ERROR);
}
static void poll(lv_timer_t *t){(void)t;strip_thaw_if_idle();if(!paused){desk_data_latest(&model);sequence++;telemetry_pending=true;telemetry_commit(NULL);}}
static void viewport_scrolled(lv_event_t *event){
 (void)event;
 strip_scroll_end();
 page=(lv_obj_get_scroll_x(viewport)+175)/350;
 telemetry_pending=true;
 telemetry_commit(NULL);
}
/* Buttons, sliders, switches and the like scroll themselves into view when focused, and without a focus group every press
   focuses something; the card strip then snapped that control's card to its left edge, so pressing NET's Clash switch with
   NET in the middle slid the strip. The focused object is not always the one under the finger, so on every press, before
   LVGL handles focus, nothing on screen keeps that behaviour (a few hundred flag writes, including controls created since). */
static void no_scroll_on_focus_tree(lv_obj_t *o){
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLL_ON_FOCUS);
 for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)no_scroll_on_focus_tree(lv_obj_get_child(o,i));
}
static void no_scroll_on_focus(lv_event_t *event){(void)event;no_scroll_on_focus_tree(lv_screen_active());}
void desk_ui_init(void){
 gso_snapshot_empty(&model);
 root=lv_screen_active();
 lv_obj_remove_style_all(root);
 lv_obj_set_style_bg_color(root,c(BG),0);
 lv_obj_set_style_bg_opa(root,255,0);
 lv_obj_set_style_text_font(root,&desk_font_20,0);
 lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
 viewport=box(root,232,16,1032,420,BG,0);
 modules();
 lv_obj_add_flag(viewport,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_scroll_dir(viewport,LV_DIR_HOR);
 lv_obj_set_scrollbar_mode(viewport,LV_SCROLLBAR_MODE_OFF);
 lv_obj_remove_flag(viewport,LV_OBJ_FLAG_SCROLL_ELASTIC);
 lv_obj_set_scroll_snap_x(viewport,LV_SCROLL_SNAP_START);
 lv_obj_remove_flag(viewport,LV_OBJ_FLAG_SCROLL_MOMENTUM);
 lv_obj_add_event_cb(viewport,strip_scroll_begin,LV_EVENT_SCROLL_BEGIN,NULL);
 lv_obj_add_event_cb(viewport,viewport_scrolled,LV_EVENT_SCROLL_END,NULL);
 for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input))if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER){lv_indev_add_event_cb(input,no_scroll_on_focus,LV_EVENT_PRESSED,NULL);lv_indev_add_event_cb(input,strip_pointer_pressed,LV_EVENT_PRESSED,NULL);}
 sidebar=box(root,16,16,196,420,INSET,16);
 sidebar_title=text(sidebar,18,15,"GSO STATUS",&lv_font_montserrat_18,TEXT);
 clock_label=text(sidebar,18,49,"",&lv_font_montserrat_32,TEXT);
 date_label=small(sidebar,18,93,"",MUTED);
 weather_summary_icon=weather_icon_create(sidebar,18,138,30);
 clickable(weather_summary_icon,17);
 weather_label=small(sidebar,54,133,"天气未接入",MUTED);
 lv_obj_set_width(weather_label,140);
 clickable(weather_label,17);
 weather_quality_label=small(sidebar,54,155,"",MUTED);
 clickable(weather_quality_label,17);
 weather_location_pin=location_pin_create(sidebar,18,189);
 clickable(weather_location_pin,17);
 weather_location_label=small(sidebar,42,187,"",MUTED);
 lv_obj_set_width(weather_location_label,140);
 lv_label_set_long_mode(weather_location_label,LV_LABEL_LONG_DOT);
 clickable(weather_location_label,17);
 wifi_icon=text(sidebar,160,15,LV_SYMBOL_WIFI,&lv_font_montserrat_18,MUTED);
 lv_obj_add_flag(wifi_icon,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_event_cb(wifi_icon,wifi_icon_cb,LV_EVENT_CLICKED,NULL);
 lv_obj_t *traffic=box(sidebar,0,226,196,104,INSET,0);
 traffic_box=traffic;
 clickable(traffic,14);
 traffic_arrow[0]=small(traffic,18,10,"↓",BLUE);
 rx_text=text(traffic,54,5,"-",&lv_font_montserrat_24,BLUE);
 traffic_unit[0]=small(traffic,126,12,"MB/s",MUTED);
 traffic_arrow[1]=small(traffic,18,55,"↑",GREEN);
 tx_text=text(traffic,54,50,"-",&lv_font_montserrat_24,GREEN);
 traffic_unit[1]=small(traffic,126,57,"MB/s",MUTED);
 source_label=small(sidebar,18,332,"未接入",AMBER);
 lv_obj_add_flag(source_label,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_ext_click_area(source_label,10);
 lv_obj_add_event_cb(source_label,source_alert_cb,LV_EVENT_CLICKED,NULL);
 side_button=button(sidebar,18,364,160,"设置",side_cb);
 clock_tick(NULL);
 render();
 lv_timer_create(clock_tick,1000,NULL);
 lv_timer_create(poll,GSO_POLL_INTERVAL_MS,NULL);
 lv_timer_create(telemetry_commit,50,NULL);
 lv_timer_create(brightness_tick,100,NULL);
}
