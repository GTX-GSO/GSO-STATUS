/* Cards reflect observed HA state; only released gestures submit commands. */
#define HOME_TILE_W 322
#define HOME_CHIP_W 88
#define HOME_CHIP_H 46
#define HOME_CHIP_MAX 10
typedef struct {
 lv_obj_t *tile,*name,*state,*temperatures,*feedback,*power,*sliders[2],*slider_labels[2],*mode,*mode_label,*chips[HOME_CHIP_MAX],*covers[2];
 unsigned slider_actions[2],mode_action,chip_count,chip_mode[HOME_CHIP_MAX];
 int need;
 bool dragging[2];
}
home_view;
static home_view home_views[2][GSO_MAX_HOME_ENTITIES];
static lv_obj_t *home_empty[2],*home_hint[2];
static void render_home(void);
static const char *home_state_name(const char *state){
 const char *keys[]={"on","off","cool","heat","dry","fan_only","auto","heat_cool","open","closed","opening","closing","mixed","unknown","unavailable","sleep"};
 const char *names[]={"开启","关闭","制冷","制热","除湿","送风","自动","自动调温","打开","已关","开启中","关闭中","部分开启","未知","不可用","睡眠"};
 for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++){if(!strcmp(state,keys[i]))return names[i];}
 return state[0]?state:"未知";
}
static bool home_active(const gso_home_entity *e){
 return e->state[0]&&strcmp(e->state,"off")&&strcmp(e->state,"closed")&&strcmp(e->state,"unknown")&&strcmp(e->state,"unavailable");
}
/* Progress (queued, sent, confirmed, superseded) is not announced; only a failed command asks the user to check the device. */
static const char *home_result_name(desk_home_result result){
 switch(result.state){
  case DESK_HOME_REJECTED:return "控制请求被拒绝";
  case DESK_HOME_TIMED_OUT:return "未收到目标状态，请核对设备";
  case DESK_HOME_UNKNOWN:return result.busy?"":"结果未知，请核对设备";
  default:return "";
 }
}
static void home_power_draw(lv_event_t *event){
 lv_obj_t *o=lv_event_get_target_obj(event);
 lv_area_t area;
 lv_obj_get_coords(o,&area);
 int cx=(area.x1+area.x2)/2,cy=(area.y1+area.y2)/2,k=(area.x2-area.x1+1)*100/42,width=2+k/160;
 lv_layer_t *layer=lv_event_get_layer(event);
 lv_color_t color=lv_obj_get_style_text_color(o,0);
 /* Open ring (gap at 12 o'clock) and stem; nothing is painted over the button, so no state can leave stray marks. */
 lv_draw_arc_dsc_t ring;
 lv_draw_arc_dsc_init(&ring);
 ring.color=color;
 ring.width=width;
 ring.center=(lv_point_t){cx,cy+k/100};
 ring.radius=8*k/100+1;
 ring.start_angle=300;
 ring.end_angle=240;
 ring.rounded=1;
 lv_draw_arc(layer,&ring);
 lv_draw_line_dsc_t line;
 lv_draw_line_dsc_init(&line);
 line.p1=(lv_point_precise_t){cx,cy-10*k/100};
 line.p2=(lv_point_precise_t){cx,cy+k*1/100};
 line.color=color;
 line.width=width;
 line.round_start=line.round_end=1;
 lv_draw_line(layer,&line);
}
static void home_toggle(lv_event_t *event){
 unsigned index=(unsigned)(uintptr_t)lv_event_get_user_data(event);
 if(index>=model.home_count)return;
 gso_home_entity *e=&model.home_entities[index];
 unsigned action=home_active(e)?GSO_HOME_OFF:GSO_HOME_ON;
 if(!desk_home_command(e->id,action,gso_metric_missing("not_used")))return;
 render_home();
}
static lv_obj_t *home_power(lv_obj_t *parent,int x,int y,int size,unsigned index){
 lv_obj_t *p=box(parent,x,y,size,size,LINE,LV_RADIUS_CIRCLE);
 lv_obj_add_flag(p,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_event_cb(p,home_power_draw,LV_EVENT_DRAW_MAIN,NULL);
 lv_obj_add_event_cb(p,home_toggle,LV_EVENT_CLICKED,(void *)(uintptr_t)index);
 lv_obj_set_style_border_color(p,c(BLUE),LV_STATE_PRESSED);
 lv_obj_set_style_border_width(p,2,LV_STATE_PRESSED);
 return p;
}
static void home_cover_click(lv_event_t *event){
 unsigned code=(unsigned)(uintptr_t)lv_event_get_user_data(event),index=code>>16,action=code&65535;
 if(index<model.home_count){
  desk_home_command(model.home_entities[index].id,action,gso_metric_missing("not_used"));
  render_home();
 }
}
static void home_slider_format(lv_obj_t *label,unsigned action,double number){
 if(action==GSO_HOME_TEMPERATURE)lv_label_set_text_fmt(label,"设定温度  %.1f°C",number);
 else if(action==GSO_HOME_COLOR)lv_label_set_text_fmt(label,"色温  %.0f K",number);
 else lv_label_set_text_fmt(label,action==GSO_HOME_BRIGHTNESS?"亮度  %.0f%%":action==GSO_HOME_SPEED?"风量  %.0f%%":"目标湿度  %.0f%%",number);
}
static void home_slide(lv_event_t *event){
 unsigned code=(unsigned)(uintptr_t)lv_event_get_user_data(event),index=code>>8,slot=code&255;
 if(index>=model.home_count||slot>=2)return;
 home_view *v=&home_views[1][index];
 lv_event_code_t kind=lv_event_get_code(event);
 if(kind==LV_EVENT_PRESSED)v->dragging[slot]=true;
 if(kind==LV_EVENT_VALUE_CHANGED){
  int value=lv_slider_get_value(v->sliders[slot]);
  double number=v->slider_actions[slot]==GSO_HOME_TEMPERATURE?value/10.0:value;
  home_slider_format(v->slider_labels[slot],v->slider_actions[slot],number);
 }
 if(kind==LV_EVENT_RELEASED||kind==LV_EVENT_PRESS_LOST){
  v->dragging[slot]=false;
  if(kind==LV_EVENT_RELEASED){
   gso_home_entity *e=&model.home_entities[index];
   unsigned action=v->slider_actions[slot];
   double number=lv_slider_get_value(v->sliders[slot]);
   if(action==GSO_HOME_TEMPERATURE){
    number/=10;
    number=e->min_temperature.value+round((number-e->min_temperature.value)/e->temperature_step.value)*e->temperature_step.value;
   }
   if(action==GSO_HOME_SPEED&&e->percentage_step.valid)number=fmin(100,round(number/e->percentage_step.value)*e->percentage_step.value);
   if(action==GSO_HOME_COLOR)number=fmin(e->max_kelvin.value,fmax(e->min_kelvin.value,round(number/50)*50));
   desk_home_setting(e->id,action,gso_metric_number(number),NULL);
  }
  render_home();
 }
 }
static void home_mode_chip(lv_event_t *event){
 unsigned code=(unsigned)(uintptr_t)lv_event_get_user_data(event),index=code>>8,k=code&255;
 if(index>=model.home_count)return;
 home_view *v=&home_views[1][index];
 const gso_home_entity *e=&model.home_entities[index];
 if(k<v->chip_count&&v->chip_mode[k]<e->mode_count){
  desk_home_setting(e->id,v->mode_action,gso_metric_missing("not_used"),e->modes[v->chip_mode[k]]);
  render_home();
 }
}
/* Working modes are large touch chips (88 x 46 px, three per row) instead of a dropdown list. */
static void home_create_mode(home_view *v,unsigned i){
 v->mode_label=small(v->tile,20,176,"工作模式",MUTED);v->mode=box(v->tile,0,0,1,1,INSET,0);lv_obj_add_flag(v->mode,LV_OBJ_FLAG_HIDDEN);
 for(unsigned k=0;k<HOME_CHIP_MAX;k++){
  lv_obj_t *chip=box(v->tile,20,200,HOME_CHIP_W,HOME_CHIP_H,LINE,10);
  lv_obj_add_flag(chip,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *label=small(chip,0,0,"",TEXT);
  lv_obj_center(label);
  lv_obj_add_event_cb(chip,home_mode_chip,LV_EVENT_CLICKED,(void *)(uintptr_t)((i<<8)|k));
  v->chips[k]=chip;
 }
}
static void home_summary(lv_obj_t *parent){
 home_empty[0]=cn(parent,20,80,"未接入",AMBER);home_hint[0]=small(parent,20,125,"等待绑定设备",MUTED);
 for(unsigned i=0;i<4;i++){
  home_view *v=&home_views[0][i];
  v->tile=box(parent,16,66+(int)i*90,300,70,INSET,10);
  clickable(v->tile,15);
  v->name=small(v->tile,16,10,"",TEXT);
  lv_obj_set_width(v->name,214);
  lv_label_set_long_mode(v->name,LV_LABEL_LONG_DOT);
  v->temperatures=small(v->tile,16,38,"",MUTED);
  lv_obj_set_width(v->temperatures,214);
  v->state=small(v->tile,16,38,"",AMBER);
  v->power=home_power(v->tile,238,11,48,i);
  lv_obj_add_flag(v->tile,LV_OBJ_FLAG_HIDDEN);
 }
}
static void home_details(lv_obj_t *parent){
 home_hint[1]=small(parent,612,16,"等待绑定设备",MUTED);
 lv_obj_set_width(home_hint[1],400);
 lv_obj_set_style_text_align(home_hint[1],LV_TEXT_ALIGN_RIGHT,0);
 /* Cards flow three per row; each card's height follows the controls the device really has (set in render_home). */
 lv_obj_t *content=box(parent,20,52,992,350,PANEL,0);
 lv_obj_add_flag(content,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_scroll_dir(content,LV_DIR_VER);
 lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_AUTO);
 lv_obj_set_flex_flow(content,LV_FLEX_FLOW_ROW_WRAP);
 lv_obj_set_flex_align(content,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
 lv_obj_set_style_pad_row(content,12,0);
 lv_obj_set_style_pad_column(content,12,0);
 lv_obj_set_style_pad_bottom(content,12,0);
 home_empty[1]=cn(content,4,38,"未接入",AMBER);
 lv_obj_add_flag(home_empty[1],LV_OBJ_FLAG_FLOATING);
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  v->tile=box(content,0,0,HOME_TILE_W,160,INSET,12);
  v->name=small(v->tile,20,16,"",TEXT);
  lv_obj_set_width(v->name,226);
  lv_label_set_long_mode(v->name,LV_LABEL_LONG_DOT);
  v->power=home_power(v->tile,HOME_TILE_W-64,12,52,i);
  v->state=small(v->tile,20,46,"",MUTED);
  v->temperatures=small(v->tile,20,74,"",MUTED);
  lv_obj_set_width(v->temperatures,HOME_TILE_W-40);
  v->feedback=small(v->tile,20,124,"",MUTED);
  lv_obj_set_width(v->feedback,HOME_TILE_W-40);
  lv_label_set_long_mode(v->feedback,LV_LABEL_LONG_DOT);
  for(unsigned slot=0;slot<2;slot++){
   v->slider_labels[slot]=small(v->tile,20,112+slot*66,"",MUTED);
   v->sliders[slot]=lv_slider_create(v->tile);
   lv_obj_set_pos(v->sliders[slot],30,146+slot*66);
   lv_obj_set_size(v->sliders[slot],HOME_TILE_W-60,12);
   lv_obj_set_style_bg_color(v->sliders[slot],c(LINE),LV_PART_MAIN);
   lv_obj_set_style_bg_color(v->sliders[slot],c(GREEN),LV_PART_INDICATOR);
   lv_obj_set_style_bg_color(v->sliders[slot],c(TEXT),LV_PART_KNOB);
   lv_obj_set_style_pad_all(v->sliders[slot],10,LV_PART_KNOB);
   lv_obj_set_style_radius(v->sliders[slot],LV_RADIUS_CIRCLE,LV_PART_KNOB);
   lv_obj_set_style_radius(v->sliders[slot],LV_RADIUS_CIRCLE,LV_PART_MAIN);
   lv_obj_set_style_radius(v->sliders[slot],LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
   lv_obj_add_event_cb(v->sliders[slot],home_slide,LV_EVENT_ALL,(void *)(uintptr_t)((i<<8)|slot));
  }
  for(unsigned a=0;a<2;a++){
   v->covers[a]=box(v->tile,20+a*((HOME_TILE_W-50)/2+10),112,(HOME_TILE_W-50)/2,52,LINE,10);
   lv_obj_t *name=cn(v->covers[a],0,0,a?"关闭":"打开",TEXT);
   lv_obj_center(name);
   lv_obj_add_flag(v->covers[a],LV_OBJ_FLAG_CLICKABLE);
   lv_obj_add_event_cb(v->covers[a],home_cover_click,LV_EVENT_CLICKED,(void *)(uintptr_t)((i<<16)|(a?GSO_HOME_CLOSE:GSO_HOME_OPEN)));
  }
  lv_obj_add_flag(v->tile,LV_OBJ_FLAG_HIDDEN);
 }
}
static void home_widget_enabled(lv_obj_t *o,bool enabled,bool dim){
 if(enabled){if(lv_obj_has_state(o,LV_STATE_DISABLED))lv_obj_remove_state(o,LV_STATE_DISABLED);}
 else if(!lv_obj_has_state(o,LV_STATE_DISABLED))lv_obj_add_state(o,LV_STATE_DISABLED);
 lv_opa_t opacity=enabled||!dim?255:100;if(lv_obj_get_style_opa(o,0)!=opacity)lv_obj_set_style_opa(o,opacity,0);
}
static void home_visible(lv_obj_t *o,bool visible){
 if(visible)lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);
 else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
}
static bool home_group_visible(int group){
#ifdef GSO_GSS_BASELINE
 (void)group;return true;
#else
 return group==0?main_card_visible(3):detail_id==15;
#endif
}
static void render_home(void){
 uint64_t now=desk_data_now_ms();
 desk_home_result command=desk_home_latest();
 bool ready=desk_home_ready()&&desk_data_connected()&&model.home_epoch[0];
 for(int group=0;group<2;group++){
  if(!home_group_visible(group))continue;
  if(home_empty[group])home_visible(home_empty[group],!model.home_count);
  if(home_hint[group]){
   if(!model.home_count){
    if(!group){
     lv_obj_set_pos(home_hint[group],20,125);
     lv_obj_set_width(home_hint[group],LV_SIZE_CONTENT);
     lv_obj_set_style_text_align(home_hint[group],LV_TEXT_ALIGN_LEFT,0);
    }
    lv_label_set_text(home_hint[group],"等待绑定设备");
   }else {
    if(!group){
     lv_obj_set_pos(home_hint[group],100,24);
     lv_obj_set_width(home_hint[group],212);
     lv_obj_set_style_text_align(home_hint[group],LV_TEXT_ALIGN_RIGHT,0);
    }
    lv_label_set_text(home_hint[group],!desk_data_connected()?"连接中断":!ready?"只读 · 控制不可用":"");
   }
  }
  for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
   home_view *v=&home_views[group][i];
   if(!v->tile)continue;
   home_visible(v->tile,i<model.home_count);
   if(i>=model.home_count)continue;
   if(!group){
    /* main card: up to four devices, spaced out; three get more room */int shown=model.home_count>4?4:(int)model.home_count,th=shown>3?70:78,gap=shown>3?20:26;
    lv_obj_set_pos(v->tile,16,66+(int)i*(th+gap));
    lv_obj_set_height(v->tile,th);
    lv_obj_set_y(v->power,(th-48)/2);
   }
   const gso_home_entity *e=&model.home_entities[i];
   gso_quality q=gso_source_quality(e->source,now,45000,120000);
   bool pending=command.busy&&!strcmp(command.entity_id,e->id);
   bool dim=!ready||q!=GSO_OK||pending;
   bool active=q==GSO_OK&&home_active(e),available=ready&&!command.busy&&q==GSO_OK;
   uint32_t accent=q!=GSO_OK?AMBER:active?GREEN:LINE;
   lv_obj_set_style_bg_color(v->tile,c(active?0x203A34:INSET),0);
   lv_obj_set_style_border_color(v->tile,c(accent),0);
   lv_obj_set_style_border_width(v->tile,active?2:1,0);
   lv_obj_set_style_border_opa(v->tile,255,0);
   lv_label_set_text(v->name,e->name);
   lv_obj_set_style_text_color(v->name,c(active?TEXT:MUTED),0);
   lv_label_set_text(v->state,q!=GSO_OK?quality_text(q):group?home_state_name(e->state):"");
   if(group)lv_obj_set_style_text_color(v->state,c(q!=GSO_OK?AMBER:active?GREEN:MUTED),0);
   bool climate=!strncmp(e->id,"climate.",8),light=!strncmp(e->id,"light.",6),fan=!strncmp(e->id,"fan.",4),humidifier=!strncmp(e->id,"humidifier.",11),cover=!strncmp(e->id,"cover.",6);
   if(q!=GSO_OK)lv_label_set_text(v->temperatures,"");
   else if(climate){
    if(e->target_temperature.valid)lv_label_set_text_fmt(v->temperatures,"设定 %.1f°C",e->target_temperature.value);
    else lv_label_set_text(v->temperatures,"设定温度不可用");
   }else if(light&&e->brightness.valid){
    if(e->kelvin.valid)lv_label_set_text_fmt(v->temperatures,"%.0f%% · %.0f K",e->brightness.value,e->kelvin.value);
    else lv_label_set_text_fmt(v->temperatures,"亮度 %.0f%%",e->brightness.value);
   }else if(fan&&e->percentage.valid)lv_label_set_text_fmt(v->temperatures,"%s · %.0f%%",home_state_name(e->mode),e->percentage.value);
   else if(humidifier&&e->humidity.valid)lv_label_set_text_fmt(v->temperatures,"%s · 目标 %.0f%%",home_state_name(e->mode),e->humidity.value);
   else if(e->detail_state[0]){
    if(e->remaining.valid)lv_label_set_text_fmt(v->temperatures,"%s · 剩余 %.0f 分钟",e->detail_state,e->remaining.value);
    else lv_label_set_text(v->temperatures,e->detail_state);
   }else lv_label_set_text(v->temperatures,"");
   home_visible(v->power,!cover);
   lv_obj_set_style_bg_color(v->power,c(active?GREEN:LINE),0);
   lv_obj_set_style_text_color(v->power,c(active?PANEL:MUTED),0);
   bool power_allowed=gso_home_action_allowed(e,active?GSO_HOME_OFF:GSO_HOME_ON,now);
   home_widget_enabled(v->power,available&&power_allowed,dim||!power_allowed);
#ifdef GSO_GSS_BASELINE
 lv_obj_invalidate(v->power);
#endif
   if(!group)continue;
   int yy=112;bool summary=true; /* next control row inside the card; the summary line is dropped when sliders already show the values */
   const char *notice="";
   if(v->feedback){
    bool current=!strcmp(command.entity_id,e->id)&&(command.busy||now-command.updated_ms<15000);
    notice=current&&home_result_name(command)[0]?home_result_name(command):q!=GSO_OK?quality_text(q):!e->actions?"此设备暂不可操作":"";
    lv_label_set_text(v->feedback,notice);
    lv_obj_set_style_text_color(v->feedback,c(notice[0]?AMBER:MUTED),0);
   }
   unsigned actions[2]={climate?GSO_HOME_TEMPERATURE:light?GSO_HOME_BRIGHTNESS:fan?GSO_HOME_SPEED:humidifier?GSO_HOME_HUMIDITY:0,light?GSO_HOME_COLOR:0};
   gso_metric current[2]={climate?e->target_temperature:light?e->brightness:fan?e->percentage:e->humidity,e->kelvin};
   for(unsigned slot=0;slot<2;slot++){
    unsigned action=actions[slot];
    v->slider_actions[slot]=action;
    bool visible=action&&(e->actions&action);
    if(action==GSO_HOME_TEMPERATURE)visible=visible&&e->min_temperature.valid&&e->max_temperature.valid&&e->temperature_step.valid;
    if(action==GSO_HOME_COLOR)visible=visible&&e->min_kelvin.valid&&e->max_kelvin.valid;
    home_visible(v->sliders[slot],visible);
    home_visible(v->slider_labels[slot],visible);
    if(visible){
     if(summary){
      summary=false;
      yy=84;
     }
     lv_obj_set_y(v->slider_labels[slot],yy);
     lv_obj_set_y(v->sliders[slot],yy+34);
     yy+=66;
    }
    if(!visible)continue;
    if(!v->dragging[slot]){
     int min=0,max=100;
     if(action==GSO_HOME_TEMPERATURE){
      min=(int)lround(e->min_temperature.value*10);
      max=(int)lround(e->max_temperature.value*10);
     }else if(action==GSO_HOME_COLOR){
      min=(int)e->min_kelvin.value;
      max=(int)e->max_kelvin.value;
     }else if(action==GSO_HOME_HUMIDITY){
      min=(int)e->min_humidity.value;
      max=(int)e->max_humidity.value;
     }
     lv_slider_set_range(v->sliders[slot],min,max);
     lv_slider_set_value(v->sliders[slot],current[slot].valid?(int)lround(current[slot].value*(action==GSO_HOME_TEMPERATURE?10:1)):min,LV_ANIM_OFF);
     if(current[slot].valid)home_slider_format(v->slider_labels[slot],action,current[slot].value);
     else lv_label_set_text(v->slider_labels[slot],action==GSO_HOME_COLOR?"色温 · 请选择":action==GSO_HOME_BRIGHTNESS?"亮度 · 请选择":"数值暂不可用");
    }
    bool permitted=gso_home_action_allowed(e,action,now);
    home_widget_enabled(v->sliders[slot],available&&permitted,dim||!permitted);
    }
   home_visible(v->temperatures,summary);
   v->mode_action=climate?GSO_HOME_HVAC:fan?GSO_HOME_PRESET:humidifier?GSO_HOME_MODE:0;
   bool show_mode=v->mode_action&&(e->actions&v->mode_action)&&e->mode_count;
   if(show_mode&&!v->mode)home_create_mode(v,i);
   /* Switching off is the power button's job: the air conditioner's mode list leaves "off" out. */
   unsigned listed=0;
   if(show_mode)for(unsigned m=0;m<e->mode_count&&listed<HOME_CHIP_MAX;m++){
    if(climate&&!strcmp(e->modes[m],"off"))continue;
    v->chip_mode[listed++]=m;
   }
   show_mode=show_mode&&listed>0;
   if(v->mode){home_visible(v->mode_label,show_mode);v->chip_count=show_mode?listed:0;
    if(show_mode){lv_obj_set_y(v->mode_label,yy);yy+=30;}
    for(unsigned k=0;k<HOME_CHIP_MAX;k++){
     lv_obj_t *chip=v->chips[k];
     if(!chip)continue;
     bool used=k<v->chip_count;
     home_visible(chip,used);
     if(!used)continue;
     const char *mode_name=e->modes[v->chip_mode[k]];
     bool selected=!strcmp(e->mode,mode_name);
     lv_obj_set_pos(chip,20+(int)(k%3)*(HOME_CHIP_W+8),yy+(int)(k/3)*(HOME_CHIP_H+8));
     lv_obj_set_style_bg_color(chip,c(selected?GREEN:LINE),0);
     lv_obj_t *label=lv_obj_get_child(chip,0);
     lv_label_set_text(label,home_state_name(mode_name));
     lv_obj_set_style_text_color(label,c(selected?PANEL:TEXT),0);
     lv_obj_center(label);
     home_widget_enabled(chip,available,dim);
     }
    if(show_mode)yy+=(int)((v->chip_count+2)/3)*(HOME_CHIP_H+8)+4;
    }
   if(cover){lv_obj_set_y(v->covers[0],yy);lv_obj_set_y(v->covers[1],yy);yy+=62;}
   for(int k=0;k<2;k++){
    unsigned action=k?GSO_HOME_CLOSE:GSO_HOME_OPEN;
    home_visible(v->covers[k],cover);
    bool reverse=command.busy&&!strcmp(command.entity_id,e->id)&&(command.action==GSO_HOME_OPEN||command.action==GSO_HOME_CLOSE)&&command.action!=action;
    bool permitted=gso_home_action_allowed(e,action,now);
    home_widget_enabled(v->covers[k],ready&&q==GSO_OK&&(!command.busy||reverse)&&permitted,dim||!permitted);
   }
   home_visible(v->feedback,notice[0]!=0);lv_obj_set_y(v->feedback,yy+4);v->need=yy+(notice[0]?36:16);
  }
  if(group){
   int tallest=0;
   for(unsigned i=0;i<model.home_count&&i<GSO_MAX_HOME_ENTITIES;i++){
    home_view *v=&home_views[1][i];
    if(v->tile&&v->need>tallest)tallest=v->need;
   }
   if(tallest)for(unsigned i=0;i<model.home_count&&i<GSO_MAX_HOME_ENTITIES;i++){
    home_view *v=&home_views[1][i];
    if(v->tile)lv_obj_set_height(v->tile,tallest);
   }
   }
 }
}
static void home_close_views(void){memset(home_views[1],0,sizeof(home_views[1]));home_empty[1]=home_hint[1]=NULL;}
bool desk_ui_home_power_center(int *x,int *y){
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  if(!v->tile||!v->power||lv_obj_has_flag(v->tile,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_flag(v->power,LV_OBJ_FLAG_HIDDEN))continue;
  lv_obj_update_layout(v->power);
  lv_area_t a;
  lv_obj_get_coords(v->power,&a);
  *x=(a.x1+a.x2)/2;
  *y=(a.y1+a.y2)/2;
  return true;
 }
 return false;
}
bool desk_ui_home_card_heights(int *minimum,int *maximum){
 int low=0,high=0;
 bool any=false;
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  if(!v->tile||lv_obj_has_flag(v->tile,LV_OBJ_FLAG_HIDDEN))continue;
  int h=lv_obj_get_height(v->tile);
  if(!any||h<low)low=h;
  if(!any||h>high)high=h;
  any=true;
 }
 if(minimum)*minimum=low;
 if(maximum)*maximum=high;
 return any;
}
unsigned desk_ui_home_chip_count(const char *label){
 unsigned n=0;
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  if(!v->tile||lv_obj_has_flag(v->tile,LV_OBJ_FLAG_HIDDEN))continue;
  for(unsigned k=0;k<HOME_CHIP_MAX;k++){
   lv_obj_t *chip=v->chips[k];
   if(!chip||lv_obj_has_flag(chip,LV_OBJ_FLAG_HIDDEN))continue;
   if(!label||!strcmp(lv_label_get_text(lv_obj_get_child(chip,0)),label))n++;
  }
 }
 return n;
}
bool desk_ui_home_cover_center(unsigned button,int *x,int *y){
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  if(!v->tile||button>=2||!v->covers[button]||lv_obj_has_flag(v->tile,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_flag(v->covers[button],LV_OBJ_FLAG_HIDDEN))continue;
  lv_obj_scroll_to_view_recursive(v->covers[button],LV_ANIM_OFF);
  lv_obj_update_layout(v->covers[button]);
  lv_area_t a;
  lv_obj_get_coords(v->covers[button],&a);
  *x=(a.x1+a.x2)/2;
  *y=(a.y1+a.y2)/2;
  return true;
 }
 return false;
}
bool desk_ui_home_mode_center(unsigned chip,int *x,int *y){
 for(unsigned i=0;i<GSO_MAX_HOME_ENTITIES;i++){
  home_view *v=&home_views[1][i];
  if(!v->tile||lv_obj_has_flag(v->tile,LV_OBJ_FLAG_HIDDEN)||chip>=v->chip_count||!v->chips[chip]||lv_obj_has_flag(v->chips[chip],LV_OBJ_FLAG_HIDDEN))continue;
  lv_obj_scroll_to_view_recursive(v->chips[chip],LV_ANIM_OFF);
  lv_obj_update_layout(v->chips[chip]);
  lv_area_t a;
  lv_obj_get_coords(v->chips[chip],&a);
  *x=(a.x1+a.x2)/2;
  *y=(a.y1+a.y2)/2;
  return true;
 }
 return false;
}
