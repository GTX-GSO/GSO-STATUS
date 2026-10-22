/* Shared screen and simulator page; platform adapters implement network actions.
   Layout: nearby networks on the left (scanned when the page opens, tap to select), connection status, form and
   actions on the right. Buttons are only enabled when the action can do something. */
#include "gss_wifi.h"
#define WIFI_ROW_HEIGHT 52
#define WIFI_SAVED_ROW 0xFFFFu /* saved network that the last scan did not find */
static lv_obj_t *wifi_name,*wifi_password,*wifi_open,*wifi_message,*wifi_keyboard,*wifi_status,*wifi_status_detail,*wifi_list,*wifi_list_title;
static lv_obj_t *wifi_scan_button,*wifi_connect_button,*wifi_disconnect_button,*wifi_forget_button,*wifi_rows[GSS_WIFI_AP_LIMIT];
static lv_timer_t *wifi_timer;
static unsigned wifi_generation;
static int wifi_selected=-1;
static bool wifi_scanning_shown,wifi_connected_shown;static char wifi_saved_shown[33];
static gss_wifi_status wifi_view;
static void wifi_view_close(void){
 if(wifi_timer){lv_timer_delete(wifi_timer);wifi_timer=NULL;}
 wifi_name=wifi_password=wifi_open=wifi_message=wifi_keyboard=wifi_status=wifi_status_detail=wifi_list=wifi_list_title=NULL;
 wifi_scan_button=wifi_connect_button=wifi_disconnect_button=wifi_forget_button=NULL;memset(wifi_rows,0,sizeof(wifi_rows));wifi_selected=-1;
}
static void wifi_enable(lv_obj_t *o,bool enabled){
 if(!o)return;
 if(enabled){if(lv_obj_has_state(o,LV_STATE_DISABLED))lv_obj_remove_state(o,LV_STATE_DISABLED);}
 else if(!lv_obj_has_state(o,LV_STATE_DISABLED))lv_obj_add_state(o,LV_STATE_DISABLED);
 lv_opa_t opacity=enabled?255:110;if(lv_obj_get_style_opa(o,0)!=opacity)lv_obj_set_style_opa(o,opacity,0);
}
static void wifi_focus(lv_event_t *e){
 lv_keyboard_set_textarea(wifi_keyboard,lv_event_get_target_obj(e));
 lv_obj_remove_flag(wifi_keyboard,LV_OBJ_FLAG_HIDDEN);
}
static void wifi_keyboard_done(lv_event_t *e){
 (void)e;
 lv_keyboard_set_textarea(wifi_keyboard,NULL);
 lv_obj_add_flag(wifi_keyboard,LV_OBJ_FLAG_HIDDEN);
}
static bool wifi_form_open(void){return wifi_open&&lv_obj_has_state(wifi_open,LV_STATE_CHECKED);}
static bool wifi_can_connect(void){
 /* The first refresh runs while the page is still being built (setting the SSID text raises VALUE_CHANGED), when the
    password field does not exist yet; reading a missing object hangs LVGL in its assert loop. */
 if(!wifi_name||!wifi_password)return false;
 const char *ssid=lv_textarea_get_text(wifi_name);if(!ssid[0])return false;
 return wifi_form_open()||lv_textarea_get_text(wifi_password)[0]||(gss_wifi_has_password()&&!strcmp(ssid,gss_wifi_ssid()));
}
static const char *wifi_signal_word(int rssi){return rssi>=-55?"强":rssi>=-70?"中":"弱";}
static void wifi_mark_selected(void){
 for(unsigned i=0;i<GSS_WIFI_AP_LIMIT;i++)if(wifi_rows[i]){
  bool on=(int)i==wifi_selected;
  lv_obj_set_style_border_width(wifi_rows[i],on?2:0,0);
  lv_obj_set_style_border_color(wifi_rows[i],c(BLUE),0);
 }
}
static void wifi_refresh(lv_timer_t *timer);
static void wifi_changed_cb(lv_event_t *e){(void)e;wifi_refresh(NULL);}
static void wifi_row_cb(lv_event_t *e){
 unsigned n=(unsigned)(uintptr_t)lv_event_get_user_data(e);if(!wifi_name)return;
 gss_wifi_ap saved_only;gss_wifi_ap *ap;
 if(n==WIFI_SAVED_ROW){
  memset(&saved_only,0,sizeof(saved_only));
  snprintf(saved_only.ssid,sizeof(saved_only.ssid),"%s",gss_wifi_ssid());
  saved_only.open=gss_wifi_is_open();
  ap=&saved_only;
  wifi_selected=-1;
 }
 else{if(n>=wifi_view.count)return;ap=&wifi_view.aps[n];wifi_selected=(int)n;}
 if(strcmp(ap->ssid,lv_textarea_get_text(wifi_name)))lv_textarea_set_text(wifi_password,"");
 lv_textarea_set_text(wifi_name,ap->ssid);
 if(ap->open)lv_obj_add_state(wifi_open,LV_STATE_CHECKED);
 else lv_obj_remove_state(wifi_open,LV_STATE_CHECKED);
 wifi_mark_selected();wifi_refresh(NULL);
 /* A protected network needs its password next; an open one only needs the connect button. */
 if(ap->open)wifi_keyboard_done(NULL);
 else{
  lv_keyboard_set_textarea(wifi_keyboard,wifi_password);
  lv_obj_remove_flag(wifi_keyboard,LV_OBJ_FLAG_HIDDEN);
 }
}
static void wifi_row_widgets(lv_obj_t *row,const char *ssid,const char *meta,bool known){
 lv_obj_t *name=small(row,14,11,ssid,known?GREEN:TEXT);lv_obj_set_width(name,150);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
 lv_obj_t *tag=small(row,168,11,meta,known?GREEN:MUTED);
 lv_obj_set_width(tag,212);
 lv_label_set_long_mode(tag,LV_LABEL_LONG_CLIP);
 lv_obj_set_style_text_align(tag,LV_TEXT_ALIGN_RIGHT,0);
}
static void wifi_rebuild_list(void){
 if(!wifi_list)return;
 lv_obj_clean(wifi_list);
 memset(wifi_rows,0,sizeof(wifi_rows));
 const char *saved=gss_wifi_ssid();
 bool found=false;
 for(unsigned i=0;i<wifi_view.count&&i<GSS_WIFI_AP_LIMIT;i++)if(saved[0]&&!strcmp(saved,wifi_view.aps[i].ssid))found=true;
 int slot=0;
 /* A saved network stays listed even when the scan did not see it, so it can always be reselected or forgotten. */
 if(saved[0]&&!found){
  lv_obj_t *row=box(wifi_list,0,slot*WIFI_ROW_HEIGHT,392,46,PANEL,10);
  lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row,wifi_row_cb,LV_EVENT_CLICKED,(void *)(uintptr_t)WIFI_SAVED_ROW);
  wifi_row_widgets(row,saved,wifi_view.connected?"已连接":"已保存·未扫描到",true);
  slot++;
 }
 if(!wifi_view.count&&!(saved[0]&&!found)){
  lv_obj_t *hint=small(wifi_list,12,16,wifi_view.scanning?"正在扫描附近网络…":"点击右上角“扫描”查找网络",MUTED);
  lv_obj_set_width(hint,360);
  return;
 }
 for(unsigned i=0;i<wifi_view.count&&i<GSS_WIFI_AP_LIMIT;i++){
  gss_wifi_ap *ap=&wifi_view.aps[i];
  lv_obj_t *row=box(wifi_list,0,slot*WIFI_ROW_HEIGHT,392,46,PANEL,10);
  lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row,wifi_row_cb,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
  bool known=saved[0]&&!strcmp(saved,ap->ssid);
  char meta[64];
  snprintf(meta,sizeof(meta),"%s%s·%s",known?(wifi_view.connected?"已连接·":"已保存·"):"",wifi_signal_word(ap->rssi),ap->open?"开放":"加密");
  wifi_row_widgets(row,ap->ssid,meta,known);wifi_rows[i]=row;slot++;
 }
 wifi_mark_selected();
}
static void wifi_connect_cb(lv_event_t *e){
 (void)e;const char *error=gss_wifi_save(lv_textarea_get_text(wifi_name),lv_textarea_get_text(wifi_password),wifi_form_open());
 lv_label_set_text(wifi_message,error?error:gss_wifi_connect_saved()?"正在连接…":"无线任务未就绪");
 lv_textarea_set_text(wifi_password,"");
 wifi_keyboard_done(NULL);
}
static void wifi_forget_cb(lv_event_t *e){
 (void)e;
 gss_wifi_disconnect();
 bool ok=gss_wifi_forget();
 if(ok){
  lv_textarea_set_text(wifi_name,"");
  lv_textarea_set_text(wifi_password,"");
  lv_obj_remove_state(wifi_open,LV_STATE_CHECKED);
  wifi_selected=-1;
  wifi_mark_selected();
 }
 lv_label_set_text(wifi_message,ok?"已忘记网络":"删除失败");
}
static void wifi_scan_cb(lv_event_t *e){(void)e;lv_label_set_text(wifi_message,gss_wifi_scan()?"正在扫描…":"无线任务未就绪");}
static void wifi_disconnect_cb(lv_event_t *e){(void)e;gss_wifi_disconnect();lv_label_set_text(wifi_message,"已断开");}
static void wifi_refresh(lv_timer_t *timer){
 (void)timer;if(!wifi_status)return;gss_wifi_radio_status(&wifi_view);const char *saved=gss_wifi_ssid();
 if(wifi_view.connected){
  char line[72];
  snprintf(line,sizeof(line),"已连接 · %s",saved[0]?saved:"当前网络");
  lv_label_set_text(wifi_status,line);
  lv_obj_set_style_text_color(wifi_status,c(GREEN),0);
  char detail[96];
  snprintf(detail,sizeof(detail),"IP %s%s%s",wifi_view.ip,wifi_view.rssi?" · 信号":"",wifi_view.rssi?wifi_signal_word(wifi_view.rssi):"");
  lv_label_set_text(wifi_status_detail,detail);
  }
 else{
  lv_label_set_text(wifi_status,saved[0]?"未连接":"未配置网络");
  lv_obj_set_style_text_color(wifi_status,c(AMBER),0);
  lv_label_set_text(wifi_status_detail,wifi_view.message[0]?wifi_view.message:saved[0]?"已保存的网络可重新连接":"从左侧选择网络，或手动输入名称");
 }
 if(wifi_generation!=wifi_view.generation||wifi_scanning_shown!=wifi_view.scanning||wifi_connected_shown!=wifi_view.connected||strcmp(wifi_saved_shown,saved)){
  wifi_generation=wifi_view.generation;
  wifi_scanning_shown=wifi_view.scanning;
  wifi_connected_shown=wifi_view.connected;
  snprintf(wifi_saved_shown,sizeof(wifi_saved_shown),"%s",saved);
  wifi_rebuild_list();
 }
 if(wifi_list_title)lv_label_set_text(wifi_list_title,wifi_view.scanning?"附近网络 · 扫描中…":"附近网络");
 wifi_enable(wifi_scan_button,wifi_view.ready&&!wifi_view.scanning);
 wifi_enable(wifi_connect_button,wifi_can_connect());
 wifi_enable(wifi_disconnect_button,wifi_view.connected);
 wifi_enable(wifi_forget_button,saved[0]!=0);
}
static lv_obj_t *wifi_field(lv_obj_t *p,int y,const char *placeholder,bool password){
 lv_obj_t *o=lv_textarea_create(p);
 lv_obj_set_pos(o,464,y);
 lv_obj_set_size(o,536,48);
 lv_textarea_set_one_line(o,true);
 lv_obj_set_style_pad_all(o,12,0);
 lv_obj_set_height(o,48);
 lv_textarea_set_password_mode(o,password);
 lv_textarea_set_placeholder_text(o,placeholder);
 lv_textarea_set_max_length(o,password?64:32);
 lv_obj_set_style_text_font(o,&desk_font_20,0);
 lv_obj_set_style_bg_color(o,c(INSET),0);
 lv_obj_set_style_bg_opa(o,255,0);
 lv_obj_set_style_text_color(o,c(TEXT),0);
 lv_obj_set_style_border_color(o,c(MUTED),0);
 lv_obj_set_style_border_width(o,1,0);
 lv_obj_set_style_radius(o,8,0);
 lv_obj_add_event_cb(o,wifi_focus,LV_EVENT_FOCUSED,NULL);
 lv_obj_add_event_cb(o,wifi_focus,LV_EVENT_CLICKED,NULL);
 lv_obj_add_event_cb(o,wifi_changed_cb,LV_EVENT_VALUE_CHANGED,NULL);
 return o;
}
static lv_obj_t *wifi_action(lv_obj_t *p,int x,int y,int w,const char *label,lv_event_cb_t cb){
 lv_obj_t *o=box(p,x,y,w,48,LINE,10);
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *t=cn(o,0,0,label,TEXT);
 lv_obj_center(t);
 lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,NULL);
 return o;
}
static void wifi_page(lv_event_t *e){
 (void)e;desk_ui_close_detail();lv_obj_t *p=panel("设置 · Wi-Fi");
 /* Left: nearby networks. */
 lv_obj_t *left=box(p,24,56,424,330,INSET,14);wifi_list_title=small(left,16,14,"附近网络",MUTED);lv_obj_set_width(wifi_list_title,250);
 wifi_scan_button=wifi_action(left,288,6,120,"扫描",wifi_scan_cb);lv_obj_set_height(wifi_scan_button,40);
 wifi_list=box(left,8,56,408,266,INSET,0);
 lv_obj_add_flag(wifi_list,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_scroll_dir(wifi_list,LV_DIR_VER);
 lv_obj_set_scrollbar_mode(wifi_list,LV_SCROLLBAR_MODE_AUTO);
 /* Right: status, form, actions. */
 lv_obj_t *status=box(p,464,56,536,84,INSET,12);
 wifi_status=cn(status,16,10,"正在读取无线状态",AMBER);
 lv_obj_set_width(wifi_status,504);
 wifi_status_detail=small(status,16,44,"",MUTED);
 lv_obj_set_width(wifi_status_detail,504);
 lv_label_set_long_mode(wifi_status_detail,LV_LABEL_LONG_DOT);
 wifi_name=wifi_field(p,152,"网络名称 SSID",false);lv_textarea_set_text(wifi_name,gss_wifi_ssid());
 wifi_password=wifi_field(p,208,gss_wifi_has_password()?"已存密码 · 留空沿用":"网络密码",true);
 wifi_open=lv_checkbox_create(p);
 lv_obj_set_style_text_color(wifi_open,c(TEXT),0);
 lv_obj_set_style_text_font(wifi_open,&desk_font_20,0);
 lv_obj_set_style_pad_column(wifi_open,12,0);
 lv_obj_set_style_bg_color(wifi_open,c(LINE),LV_PART_INDICATOR);
 lv_obj_set_style_bg_color(wifi_open,c(BLUE),LV_PART_INDICATOR|LV_STATE_CHECKED);
 lv_obj_set_style_bg_opa(wifi_open,255,LV_PART_INDICATOR);
 lv_obj_set_pos(wifi_open,466,266);
 lv_checkbox_set_text(wifi_open,"开放网络（无密码）");
 if(gss_wifi_is_open())lv_obj_add_state(wifi_open,LV_STATE_CHECKED);
 lv_obj_add_event_cb(wifi_open,wifi_changed_cb,LV_EVENT_VALUE_CHANGED,NULL);
 wifi_connect_button=wifi_action(p,464,306,176,"连接",wifi_connect_cb);
 wifi_disconnect_button=wifi_action(p,648,306,168,"断开",wifi_disconnect_cb);
 wifi_forget_button=wifi_action(p,824,306,176,"忘记",wifi_forget_cb);
 wifi_message=small(p,464,360,"",MUTED);lv_obj_set_width(wifi_message,536);lv_label_set_long_mode(wifi_message,LV_LABEL_LONG_DOT);
 /* The keyboard covers the checkbox, buttons and messages only while a field is being edited. */
 wifi_keyboard=lv_keyboard_create(p);
 lv_obj_align(wifi_keyboard,LV_ALIGN_TOP_LEFT,464,264); /* the widget defaults to bottom-centre, so set_pos alone leaves it off-screen */
 lv_obj_set_size(wifi_keyboard,536,140);
 lv_obj_set_style_text_font(wifi_keyboard,&lv_font_montserrat_18,LV_PART_ITEMS);
 lv_obj_add_event_cb(wifi_keyboard,wifi_keyboard_done,LV_EVENT_READY,NULL);
 lv_obj_add_event_cb(wifi_keyboard,wifi_keyboard_done,LV_EVENT_CANCEL,NULL);
 lv_obj_add_flag(wifi_keyboard,LV_OBJ_FLAG_HIDDEN);
 /* The LVGL theme paints keys light grey and control keys (checked state) lighter still, so every state is set. */
 lv_obj_set_style_bg_color(wifi_keyboard,c(PANEL),0);
 lv_obj_set_style_bg_opa(wifi_keyboard,255,0);
 lv_obj_set_style_border_width(wifi_keyboard,0,0);
 lv_obj_set_style_pad_all(wifi_keyboard,4,0);
 lv_obj_set_style_pad_gap(wifi_keyboard,6,0);
 lv_obj_set_style_bg_color(wifi_keyboard,c(LINE),LV_PART_ITEMS);
 lv_obj_set_style_bg_color(wifi_keyboard,c(INSET),LV_PART_ITEMS|LV_STATE_CHECKED);
 lv_obj_set_style_bg_color(wifi_keyboard,c(BLUE),LV_PART_ITEMS|LV_STATE_PRESSED);
 lv_obj_set_style_bg_opa(wifi_keyboard,255,LV_PART_ITEMS);
 lv_obj_set_style_bg_opa(wifi_keyboard,255,LV_PART_ITEMS|LV_STATE_CHECKED);
 lv_obj_set_style_text_color(wifi_keyboard,c(TEXT),LV_PART_ITEMS);
 lv_obj_set_style_text_color(wifi_keyboard,c(TEXT),LV_PART_ITEMS|LV_STATE_CHECKED);
 lv_obj_set_style_border_width(wifi_keyboard,0,LV_PART_ITEMS);
 lv_obj_set_style_shadow_width(wifi_keyboard,0,LV_PART_ITEMS);
 lv_obj_set_style_radius(wifi_keyboard,8,LV_PART_ITEMS);
 wifi_generation=~0U;
 wifi_scanning_shown=false;
 wifi_connected_shown=false;
 wifi_saved_shown[0]=0;
 wifi_timer=lv_timer_create(wifi_refresh,500,NULL);
 wifi_refresh(NULL);
 /* Scan once when the page opens and nothing is known yet. */
 if(wifi_view.ready&&!wifi_view.count&&!wifi_view.scanning&&!wifi_view.connected){
  lv_label_set_text(wifi_message,gss_wifi_scan()?"正在扫描…":"无线任务未就绪");
  wifi_refresh(NULL);
 }
}
static void wifi_icon_cb(lv_event_t *e){return_to_settings=false;wifi_page(e);}
