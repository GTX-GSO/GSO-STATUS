/* Settings: screen brightness (auto or manual), theme, the board's own state (cores, memory, flash, temperature) and the
   entry to the level page (level_ui.inc). Wi-Fi has its own page (wifi_ui.inc), opened from the sidebar icon. */
static lv_obj_t *brightness_percent_label,*brightness_hint,*brightness_slider,*brightness_save_label,*brightness_auto_button,*theme_modes[3];
#define SYSTEM_CELLS 6
static lv_obj_t *system_values[SYSTEM_CELLS],*system_bars[SYSTEM_CELLS],*system_uptime;
static uint32_t system_rendered_at;
static bool system_rendered;
static void settings_open(void);
static void settings_view_close(void){
 brightness_percent_label=brightness_hint=brightness_slider=brightness_save_label=brightness_auto_button=NULL;
 memset(theme_modes,0,sizeof(theme_modes));
 memset(system_values,0,sizeof(system_values));
 memset(system_bars,0,sizeof(system_bars));
 system_uptime=NULL;
 system_rendered=false;
}
/* Segmented control: a row of touch segments, the selected one filled. */
static void segment_select(lv_obj_t *const *segments,unsigned count,unsigned selected){
 for(unsigned i=0;i<count;i++){
  if(!segments[i])continue;
  bool on=i==selected;
  lv_obj_set_style_bg_opa(segments[i],on?255:0,0);
  lv_obj_set_style_text_color(lv_obj_get_child(segments[i],0),c(on?PANEL:TEXT),0);
 }
}
static void segmented_create(lv_obj_t *parent,int x,int y,int w,int h,const char *const *labels,unsigned count,lv_event_cb_t cb,lv_obj_t **segments){
 lv_obj_t *frame=box(parent,x,y,w,h,LINE,12);int each=(w-8)/(int)count;
 for(unsigned i=0;i<count;i++){
  lv_obj_t *s=box(frame,4+(int)i*each,4,each,h-8,GREEN,9);
  lv_obj_set_style_bg_opa(s,0,0);
  lv_obj_add_flag(s,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t=cn(s,0,0,labels[i],TEXT);
  lv_obj_center(t);
  lv_obj_add_event_cb(s,cb,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
  segments[i]=s;
 }
}
static lv_obj_t *settings_action(lv_obj_t *p,int x,int y,int w,int h,const char *label,lv_event_cb_t cb){
 lv_obj_t *o=box(p,x,y,w,h,LINE,12);
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *t=cn(o,0,0,label,TEXT);
 lv_obj_center(t);
 lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,NULL);
 return o;
}

static void brightness_render(void){
 gso_brightness *b=desk_brightness_state();unsigned shown=gso_brightness_percent(b);
 gso_quality q=gso_source_quality(model.ambient,desk_data_now_ms(),45000,120000);
 bool lit=desk_data_connected()&&q==GSO_OK&&model.illuminance.valid;
 if(brightness_percent_label)lv_label_set_text_fmt(brightness_percent_label,"%u%%",shown);
 if(brightness_slider&&!lv_slider_is_dragged(brightness_slider))lv_slider_set_value(brightness_slider,(int32_t)shown,LV_ANIM_OFF);
 if(brightness_hint){
  /* Only the room light reading is shown, and only while the automatic mode is following it. */
  if(!b->automatic)lv_label_set_text(brightness_hint,"");
  else if(lit)lv_label_set_text_fmt(brightness_hint,"书房 %.0f lx",model.illuminance.value);
  else lv_label_set_text(brightness_hint,"书房 -- lx");
 }
 if(brightness_auto_button){
  lv_obj_set_style_bg_color(brightness_auto_button,c(b->automatic?GREEN:LINE),0);
  lv_obj_set_style_text_color(lv_obj_get_child(brightness_auto_button,0),c(b->automatic?PANEL:MUTED),0);
 }
}
/* One cell of the board state: value text and a bar whose colour warns when it runs high. */
static void system_cell(unsigned cell,const char *value,int percent,uint32_t warn_from,uint32_t alarm_from){
 if(!system_values[cell])return;
 changed_label(system_values[cell],value);
 bool known=percent>=0;
 uint32_t color=!known||(unsigned)percent<warn_from?GREEN:(unsigned)percent<alarm_from?AMBER:RED;
 lv_bar_set_value(system_bars[cell],known?percent:0,LV_ANIM_OFF);
 lv_obj_set_style_bg_color(system_bars[cell],c(color),LV_PART_INDICATOR);
}
static int percent_of(uint32_t part,uint32_t whole){
 if(!whole)return -1;
 uint64_t value=(uint64_t)part*100u/whole;
 return value>100?100:(int)value;
}
/* The board is read once a second, not at the brightness timer's 10 Hz. */
static void settings_system_render(bool force){
 if(!system_values[0])return;
 uint32_t now=lv_tick_get();
 if(!force&&system_rendered&&lv_tick_elaps(system_rendered_at)<1000)return;
 system_rendered=true;
 system_rendered_at=now;
 gss_system_info s;
 gss_system_read(&s);
 char line[48];
 for(unsigned core=0;core<2;core++){
  if(s.cpu_valid){
   snprintf(line,sizeof(line),"%u%%",s.cpu_percent[core]);
   system_cell(core,line,(int)s.cpu_percent[core],70,90);
  }
  else system_cell(core,"-",-1,70,90);
 }
 if(s.temperature_valid){
  snprintf(line,sizeof(line),"%.0f°C",s.temperature_c);
  int scaled=(int)((s.temperature_c-20.0f)*100.0f/80.0f);
  system_cell(2,line,scaled<0?0:scaled>100?100:scaled,70,88);
 }
 else system_cell(2,"-",-1,70,88);
 if(s.memory_valid){
  uint32_t used=s.internal_total-s.internal_free;
  snprintf(line,sizeof(line),"%u / %u KB",(unsigned)(used/1024),(unsigned)(s.internal_total/1024));
  system_cell(3,line,percent_of(used,s.internal_total),80,92);
  if(s.psram_total){
   used=s.psram_total-s.psram_free;
   snprintf(line,sizeof(line),"%.1f / %.0f MB",used/1048576.0,s.psram_total/1048576.0);
   system_cell(4,line,percent_of(used,s.psram_total),80,92);
  }
  else system_cell(4,"-",-1,80,92);
 }
 else{
  system_cell(3,"-",-1,80,92);
  system_cell(4,"-",-1,80,92);
 }
 if(s.storage_valid){
  snprintf(line,sizeof(line),"%.1f / %.1f MB",s.app_used/1048576.0,s.app_total/1048576.0);
  system_cell(5,line,percent_of(s.app_used,s.app_total),80,92);
 }
 else system_cell(5,"-",-1,80,92);
 if(system_uptime){
  char span[32];
  duration_text(span,sizeof(span),s.uptime_s);
  snprintf(line,sizeof(line),"已运行 %s",span);
  changed_label(system_uptime,line);
 }
}
static void settings_render(void){
 brightness_render();
 int mode=desk_ui_theme_mode();segment_select(theme_modes,3,mode<0?0:mode?1:2);
 settings_system_render(false);
}
static void brightness_tick(lv_timer_t *t){
 (void)t;
 gso_quality q=gso_source_quality(model.ambient,desk_data_now_ms(),45000,120000);
 gso_brightness_step(desk_brightness_state(),model.illuminance,desk_data_connected()&&q==GSO_OK,desk_data_now_ms());
 if(!t||!interaction_busy())settings_render();
}
unsigned desk_ui_brightness_percent(void){return gso_brightness_percent(desk_brightness_state());}
static void brightness_save(void){
 bool saved=desk_brightness_save();
 if(brightness_save_label)lv_label_set_text(brightness_save_label,saved?"偏好已保存":"偏好保存失败");
}
static void slider_cb(lv_event_t *e){
 gso_brightness_manual(desk_brightness_state(),(unsigned)lv_slider_get_value(lv_event_get_target_obj(e)));
 brightness_render();
 if(lv_event_get_code(e)==LV_EVENT_RELEASED)brightness_save();
}
static void brightness_auto_cb(lv_event_t *e){
 (void)e;
 gso_brightness *b=desk_brightness_state();
 gso_brightness_mode(b,!b->automatic);
 brightness_tick(NULL);
 brightness_save();
}
static void theme_mode_cb(lv_event_t *e){
 unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);
 desk_ui_theme_mode_set(i==0?-1:i==1?1:0);
 settings_render();
}
static void level_from_settings_cb(lv_event_t *e){
 (void)e;
 return_to_settings=true;
 level_open();
}

static void settings_open(void){
 lv_obj_t *p=panel("设置");
 lv_obj_t *left=box(p,24,56,440,320,INSET,14),*right=box(p,484,56,524,320,INSET,14);
 /* Display */
 small(left,20,14,"屏幕亮度",MUTED);
 brightness_percent_label=text(left,260,4,"",&lv_font_montserrat_32,TEXT);
 lv_obj_set_width(brightness_percent_label,160);
 lv_obj_set_style_text_align(brightness_percent_label,LV_TEXT_ALIGN_RIGHT,0);
 brightness_slider=lv_slider_create(left);
 lv_obj_set_pos(brightness_slider,20,78);
 lv_obj_set_size(brightness_slider,400,14);
 lv_slider_set_range(brightness_slider,10,100);
 lv_slider_set_value(brightness_slider,(int32_t)gso_brightness_percent(desk_brightness_state()),LV_ANIM_OFF);
 lv_obj_set_style_bg_color(brightness_slider,c(LINE),LV_PART_MAIN);
 lv_obj_set_style_bg_color(brightness_slider,c(GREEN),LV_PART_INDICATOR);
 lv_obj_set_style_bg_color(brightness_slider,c(TEXT),LV_PART_KNOB);
 lv_obj_set_style_pad_all(brightness_slider,9,LV_PART_KNOB);
 lv_obj_set_style_radius(brightness_slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
 lv_obj_set_style_radius(brightness_slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);
 lv_obj_set_style_radius(brightness_slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
 lv_obj_add_event_cb(brightness_slider,slider_cb,LV_EVENT_VALUE_CHANGED,NULL);
 lv_obj_add_event_cb(brightness_slider,slider_cb,LV_EVENT_RELEASED,NULL);
 brightness_auto_button=settings_action(left,20,126,170,52,"自动亮度",brightness_auto_cb);
 brightness_hint=small(left,210,141,"",MUTED);
 lv_obj_set_width(brightness_hint,210);
 lv_label_set_long_mode(brightness_hint,LV_LABEL_LONG_DOT);
 box(left,20,208,400,1,LINE,0);
 small(left,20,254,"外观主题",MUTED);
 static const char *const theme_labels[3]={"日出日落","白天","黑夜"};
 segmented_create(left,120,238,300,50,theme_labels,3,theme_mode_cb,theme_modes);
 brightness_save_label=small(left,210,167,"",MUTED);
 lv_obj_set_width(brightness_save_label,210);
 /* The board itself: two columns of three, each a name, a value and a bar. */
 small(right,20,14,"本机状态",MUTED);
 system_uptime=small(right,224,14,"",MUTED);
 lv_obj_set_width(system_uptime,280);
 lv_obj_set_style_text_align(system_uptime,LV_TEXT_ALIGN_RIGHT,0);
 static const char *const system_names[SYSTEM_CELLS]={"CPU 核心0","CPU 核心1","芯片温度","内部 RAM","PSRAM","固件存储"};
 for(unsigned i=0;i<SYSTEM_CELLS;i++){
  int x=20+(int)(i/3)*254,y=52+(int)(i%3)*64;
  small(right,x,y,system_names[i],MUTED);
  system_values[i]=small(right,x+100,y,"-",TEXT);
  lv_obj_set_width(system_values[i],130);
  lv_obj_set_style_text_align(system_values[i],LV_TEXT_ALIGN_RIGHT,0);
  system_bars[i]=bar(right,x,y+32,230,GREEN);
 }
 settings_action(right,20,252,484,52,"水平仪",level_from_settings_cb);
 small(p,24,388,"GSO STATUS " GSO_VERSION " · 编译 " GSO_BUILD_TIME,MUTED);
 settings_render();
 settings_system_render(true);
}
static void side_cb(lv_event_t *e){
 (void)e;
 if(overlay){
  bool back=return_to_settings,nas=return_to_nas;
  desk_ui_close_detail();
  return_to_settings=false;
  return_to_nas=false;
  if(back)settings_open();
  else if(nas)desk_ui_detail(21);
  return;
 }
 settings_open();
}
