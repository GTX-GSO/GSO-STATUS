/* CAR: the Leapmotor vehicle read from Home Assistant by GSN. The card sits behind HOME and is only shown when the producer
   reports a configured vehicle. The lock button and the refresh button submit through the HOME command path, and only
   while the producer offers the action (control token configured and the action listed in GSN's ha.car.actions). */
#include "car_image.h"
typedef struct {
 lv_obj_t *status,*updated,*percent,*range,*bar,*charge,*odometer,*cabin,*consumption,*week,*lock_button,*lock_label,*refresh_button,*refresh_label;
 lv_obj_t *tyre[4],*tyre_alarm,*windows,*trunk,*power;
} car_view;
static car_view car_views[2]; /* 0 = main card, 1 = detail page */
static const lv_image_dsc_t car_image_desc={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=GSO_CAR_IMAGE_W,.h=GSO_CAR_IMAGE_H,.stride=GSO_CAR_IMAGE_W*4},.data_size=sizeof(gso_car_image_pixels),.data=(const uint8_t *)gso_car_image_pixels};
/* Unlocking asks for a second tap within a few seconds; locking and refreshing do not. */
#define CAR_CONFIRM_MS 4000u
static uint64_t car_unlock_armed_until_ms;

static bool car_reported(void){
 return model.car.reported&&model.car_source.quality!=GSO_UNCONFIGURED;
}
static gso_quality car_quality(void){
 return gso_source_quality(model.car_source,desk_data_now_ms(),45000,120000);
}
static bool car_connected(void){
 gso_quality q=car_quality();
 return car_reported()&&(q==GSO_OK||q==GSO_STALE);
}
static const char *car_lock_name(unsigned lock){
 switch(lock){
  case GSO_CAR_LOCKED:return "已锁车";
  case GSO_CAR_UNLOCKED:return "未锁车";
  case GSO_CAR_LOCKING:return "锁车中";
  case GSO_CAR_UNLOCKING:return "解锁中";
  case GSO_CAR_JAMMED:return "车锁异常";
 }
 return "锁状态未知";
}
static uint32_t car_lock_color(unsigned lock){
 switch(lock){
  case GSO_CAR_LOCKED:return GREEN;
  case GSO_CAR_UNLOCKED:return AMBER;
  case GSO_CAR_JAMMED:return RED;
 }
 return MUTED;
}
static uint32_t car_battery_color(double percent){
 return percent<15?RED:percent<30?AMBER:GREEN;
}
/* "更新 13:20" for today, "更新 10-16 13:20" for an earlier day; the display's local time is UTC+8 like the quota resets. */
static void car_updated_text(char *out,size_t capacity,int64_t stamp){
 if(stamp<=0){snprintf(out,capacity,"更新时间未知");return;}
 time_t at=(time_t)stamp+8*3600,now=time(NULL)+8*3600;
 struct tm *a=gmtime(&at);
 if(!a){snprintf(out,capacity,"更新时间未知");return;}
 struct tm then=*a;
 struct tm *b=gmtime(&now);
 if(b&&b->tm_year==then.tm_year&&b->tm_yday==then.tm_yday)snprintf(out,capacity,"更新 %02d:%02d",then.tm_hour,then.tm_min);
 else snprintf(out,capacity,"更新 %02d-%02d %02d:%02d",then.tm_mon+1,then.tm_mday,then.tm_hour,then.tm_min);
}
static const char *car_charge_text(char *out,size_t capacity,bool connected){
 const gso_car *k=&model.car;
 if(!connected)return "-";
 switch(k->charge){
  case GSO_CAR_CHARGING:
   if(k->remaining_minutes.valid&&k->remaining_minutes.value>0){
    char span[32];
    duration_text(span,sizeof(span),(uint64_t)(k->remaining_minutes.value*60));
    snprintf(out,capacity,"充满还需 %s",span);
    return out;
   }
   return k->remaining_minutes.valid?"即将充满":"充电中";
  case GSO_CAR_PLUGGED:return "已插枪 · 未充电";
  case GSO_CAR_UNPLUGGED:return "未充电";
 }
 return "-";
}
/* The one lock action the producer currently offers: unlock for a locked car, lock for an unlocked one. */
static unsigned car_lock_action(void){
 return (model.car.actions&GSO_HOME_UNLOCK)?GSO_HOME_UNLOCK:(model.car.actions&GSO_HOME_LOCK)?GSO_HOME_LOCK:0;
}
static bool car_action_ready(const char *entity_id,unsigned action){
 desk_home_result command=desk_home_latest();
 return action&&entity_id[0]&&car_connected()&&desk_home_ready()&&desk_data_connected()&&model.home_epoch[0]&&!command.busy&&gso_snapshot_action_allowed(&model,entity_id,action,desk_data_now_ms());
}
static void car_button_style(lv_obj_t *button,lv_obj_t *label,const char *text_value,bool enabled,bool armed){
 lv_label_set_text(label,text_value);
 lv_obj_set_style_bg_color(button,c(armed?AMBER:LINE),0);
 lv_obj_set_style_text_color(label,c(armed?BG:TEXT),0);
 home_widget_enabled(button,enabled,true);
}
/* A refused or lost command asks the user to check the car; the result of a successful one shows as the new lock state. */
static const char *car_failure_text(void){
 desk_home_result command=desk_home_latest();
 bool mine=command.entity_id[0]&&(!strcmp(command.entity_id,model.car.lock_id)||!strcmp(command.entity_id,model.car.refresh_id));
 if(!mine||command.busy||desk_data_now_ms()-command.updated_ms>12000)return "";
 switch(command.state){
  case DESK_HOME_REJECTED:return "控制请求被拒绝";
  case DESK_HOME_TIMED_OUT:return "车辆未响应，请核对";
  case DESK_HOME_UNKNOWN:return "结果未知，请核对车辆";
  default:return "";
 }
}
/* Read-only extras of the detail page: closed is green, open amber, unknown muted. */
static void car_open_text(lv_obj_t *label,bool connected,unsigned state){
 bool known=connected&&state!=GSO_CAR_OPEN_UNKNOWN;
 lv_label_set_text(label,!known?"-":state==GSO_CAR_OPEN?"未关好":"已关闭");
 lv_obj_set_style_text_color(label,c(!known?MUTED:state==GSO_CAR_OPEN?AMBER:GREEN),0);
}
static void car_extras(car_view *v,bool connected){
 const gso_car *k=&model.car;
 for(unsigned i=0;i<4;i++){
  if(connected&&k->tyre[i].valid)lv_label_set_text_fmt(v->tyre[i],"%.2f",k->tyre[i].value);
  else lv_label_set_text(v->tyre[i],"-");
  lv_obj_set_style_text_color(v->tyre[i],c(connected&&k->tyre_alarm==GSO_CAR_TYRE_ALARM?RED:TEXT),0);
 }
 bool known=connected&&k->tyre_alarm!=GSO_CAR_TYRE_UNKNOWN;
 lv_label_set_text(v->tyre_alarm,!known?"":k->tyre_alarm==GSO_CAR_TYRE_ALARM?"胎压报警":"正常");
 lv_obj_set_style_text_color(v->tyre_alarm,c(k->tyre_alarm==GSO_CAR_TYRE_ALARM?RED:GREEN),0);
 car_open_text(v->windows,connected,k->windows);
 car_open_text(v->trunk,connected,k->trunk);
 if(connected&&k->charge==GSO_CAR_CHARGING&&k->charging_power.valid)lv_label_set_text_fmt(v->power,"%.1f kW",k->charging_power.value);
 else lv_label_set_text(v->power,"-");
 lv_obj_set_style_text_color(v->power,c(connected&&k->charge==GSO_CAR_CHARGING&&k->charging_power.valid?BLUE:MUTED),0);
}
static void render_car_view(car_view *v,bool detail){
 if(!v->status)return;
 const gso_car *k=&model.car;
 gso_quality q=car_quality();
 bool connected=car_connected();
 char text[80];
 desk_home_result command=desk_home_latest();
 if(!connected)snprintf(text,sizeof(text),"%s",q==GSO_OFFLINE?"车辆离线":"未连接");
 else if(k->vehicle==GSO_CAR_DRIVING)snprintf(text,sizeof(text),"%s · 行驶中",car_lock_name(k->lock));
 else if(k->vehicle==GSO_CAR_PARKED)snprintf(text,sizeof(text),"%s · 停放",car_lock_name(k->lock));
 else snprintf(text,sizeof(text),"%s",car_lock_name(k->lock));
 lv_label_set_text(v->status,text);
 lv_obj_set_style_text_color(v->status,c(connected?car_lock_color(k->lock):AMBER),0);
 if(connected&&k->battery.valid)lv_label_set_text_fmt(v->percent,"%.0f%%",k->battery.value);
 else lv_label_set_text(v->percent,"-");
 if(connected&&k->range.valid)lv_label_set_text_fmt(v->range,"%.0f km",k->range.value);
 else lv_label_set_text(v->range,"-");
 lv_bar_set_value(v->bar,connected&&k->battery.valid?(int)k->battery.value:0,LV_ANIM_OFF);
 lv_obj_set_style_bg_color(v->bar,c(connected&&k->battery.valid?car_battery_color(k->battery.value):GREEN),LV_PART_INDICATOR);
 lv_label_set_text(v->charge,car_charge_text(text,sizeof(text),connected));
 lv_obj_set_style_text_color(v->charge,c(connected&&k->charge==GSO_CAR_CHARGING?BLUE:MUTED),0);
 const char *failure=car_failure_text();
 if(failure[0]){lv_label_set_text(v->updated,failure);lv_obj_set_style_text_color(v->updated,c(RED),0);}
 else{
  if(!connected)snprintf(text,sizeof(text),"%s",q==GSO_OFFLINE?"数据来源离线":"等待数据");
  else{
   char when[40];
   car_updated_text(when,sizeof(when),k->updated_at);
   snprintf(text,sizeof(text),"%s%s",when,q==GSO_STALE?" · 数据过期":"");
  }
  lv_label_set_text(v->updated,text);
  lv_obj_set_style_text_color(v->updated,c(connected&&q==GSO_OK?MUTED:AMBER),0);
 }
 if(v->lock_button){
  unsigned action=car_lock_action();
  bool lock_busy=command.busy&&!strcmp(command.entity_id,k->lock_id)&&(command.action==GSO_HOME_LOCK||command.action==GSO_HOME_UNLOCK);
  bool ready=car_action_ready(k->lock_id,action);
  bool armed=ready&&action==GSO_HOME_UNLOCK&&desk_data_now_ms()<car_unlock_armed_until_ms;
  const char *label=lock_busy?"执行中…":armed?"再点一次确认解锁":action==GSO_HOME_UNLOCK?"解锁":action==GSO_HOME_LOCK?"锁车":connected?"未开启控制":"-";
  car_button_style(v->lock_button,v->lock_label,label,ready,armed);
 }
 if(v->refresh_button){
  bool busy=command.busy&&!strcmp(command.entity_id,k->refresh_id)&&command.action==GSO_HOME_PRESS;
  bool ready=car_action_ready(k->refresh_id,(k->actions&GSO_HOME_PRESS)?GSO_HOME_PRESS:0);
  car_button_style(v->refresh_button,v->refresh_label,busy?"刷新中…":(k->actions&GSO_HOME_PRESS)?"刷新车况":connected?"未开启控制":"-",ready,false);
 }
 if(detail){
  gso_metric none=gso_metric_missing("not_reported");
  struct {lv_obj_t *label;gso_metric value;const char *format;} rows[]={
   {v->odometer,k->odometer,"%.0f km"},{v->cabin,k->cabin,"%.0f°C"},{v->consumption,k->consumption,"%.1f"},{v->week,k->week,"%.0f km"}};
  for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);i++){
   if(!rows[i].label)continue;
   gso_metric value=connected?rows[i].value:none;
   if(value.valid)lv_label_set_text_fmt(rows[i].label,rows[i].format,value.value);
   else lv_label_set_text(rows[i].label,"-");
  }
  car_extras(v,connected);
 }
}
static void render_car(void){
 render_car_view(&car_views[0],false);
 render_car_view(&car_views[1],true);
}
static void car_rearm_timer(lv_timer_t *timer){
 (void)timer;
 car_unlock_armed_until_ms=0;
 render_car();
}
static void car_lock_click(lv_event_t *event){
 (void)event;
 unsigned action=car_lock_action();
 if(!car_action_ready(model.car.lock_id,action))return;
 uint64_t now=desk_data_now_ms();
 if(action==GSO_HOME_UNLOCK&&now>=car_unlock_armed_until_ms){
  car_unlock_armed_until_ms=now+CAR_CONFIRM_MS;
  lv_timer_t *timer=lv_timer_create(car_rearm_timer,CAR_CONFIRM_MS+100,NULL);
  if(timer)lv_timer_set_repeat_count(timer,1);
  render_car();
  return;
 }
 car_unlock_armed_until_ms=0;
 desk_home_command(model.car.lock_id,action,gso_metric_missing("not_used"));
 render_car();
}
static void car_refresh_click(lv_event_t *event){
 (void)event;
 if(!car_action_ready(model.car.refresh_id,GSO_HOME_PRESS))return;
 desk_home_command(model.car.refresh_id,GSO_HOME_PRESS,gso_metric_missing("not_used"));
 render_car();
}
static lv_obj_t *car_picture(lv_obj_t *parent,int x,int y){
 lv_obj_t *image=lv_image_create(parent);
 lv_image_set_src(image,&car_image_desc);
 lv_obj_set_pos(image,x,y);
 lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE);
 return image;
}
static lv_obj_t *car_button(lv_obj_t *parent,int x,int y,int w,lv_event_cb_t callback,lv_obj_t **label){
 lv_obj_t *b=button(parent,x,y,w,"-",callback);
 *label=lv_obj_get_child(b,0);
 return b;
}
/* Main card, 334x420 like the others. */
static void car_card(void){
 lv_obj_t *o=card(5,"CAR",20);
 car_view *v=&car_views[0];
 car_picture(o,(334-GSO_CAR_IMAGE_W)/2,66);
 v->status=cn(o,20,208,"未连接",AMBER);
 lv_obj_set_width(v->status,294);
 lv_label_set_long_mode(v->status,LV_LABEL_LONG_DOT);
 v->percent=text(o,20,236,"-",&lv_font_montserrat_40,TEXT);
 v->range=text(o,160,242,"-",&lv_font_montserrat_32,TEXT);
 lv_obj_set_width(v->range,154);
 lv_obj_set_style_text_align(v->range,LV_TEXT_ALIGN_RIGHT,0);
 v->bar=bar(o,20,288,294,GREEN);
 v->charge=cn(o,20,302,"-",MUTED);
 lv_obj_set_width(v->charge,294);
 lv_label_set_long_mode(v->charge,LV_LABEL_LONG_DOT);
 v->updated=small(o,20,330,"",MUTED);
 lv_obj_set_width(v->updated,294);
 lv_label_set_long_mode(v->updated,LV_LABEL_LONG_DOT);
 v->lock_button=car_button(o,20,362,294,car_lock_click,&v->lock_label);
 lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
}
static void car_detail_closed(void){
 memset(&car_views[1],0,sizeof(car_views[1]));
}
static void car_detail(void){
 lv_obj_t *p=panel("CAR · 车辆");
 detail_id=20;
 car_view *v=&car_views[1];
 car_picture(p,24+(346-GSO_CAR_IMAGE_W)/2,56);
 v->status=cn(p,24,200,"未连接",AMBER);
 lv_obj_set_width(v->status,346);
 lv_label_set_long_mode(v->status,LV_LABEL_LONG_DOT);
 v->percent=text(p,24,230,"-",&lv_font_montserrat_40,TEXT);
 v->range=text(p,170,236,"-",&lv_font_montserrat_32,TEXT);
 lv_obj_set_width(v->range,200);
 lv_obj_set_style_text_align(v->range,LV_TEXT_ALIGN_RIGHT,0);
 v->bar=bar(p,24,280,346,GREEN);
 v->charge=cn(p,24,294,"-",MUTED);
 lv_obj_set_width(v->charge,346);
 lv_label_set_long_mode(v->charge,LV_LABEL_LONG_DOT);
 v->updated=small(p,24,322,"",MUTED);
 lv_obj_set_width(v->updated,346);
 lv_label_set_long_mode(v->updated,LV_LABEL_LONG_DOT);
 v->refresh_button=car_button(p,24,356,346,car_refresh_click,&v->refresh_label);
 static const char *names[4]={"总里程","车内温度","百公里能耗 kWh","近七天里程"};
 lv_obj_t **values[4]={&v->odometer,&v->cabin,&v->consumption,&v->week};
 for(int t=0;t<4;t++){
  lv_obj_t *d=box(p,398+(t%2)*309,56+(t/2)*84,301,76,INSET,12);
  small(d,14,6,names[t],MUTED);
  *values[t]=text(d,14,32,"-",&lv_font_montserrat_32,TEXT);
  lv_obj_set_width(*values[t],270);
 }
 /* Tyre pressures in bar, front axle first, with the car's own alarm flag. */
 lv_obj_t *tyres=box(p,398,228,610,100,INSET,12);
 small(tyres,14,6,"胎压 bar",MUTED);
 v->tyre_alarm=cn(tyres,300,6,"",GREEN);
 lv_obj_set_width(v->tyre_alarm,296);
 lv_obj_set_style_text_align(v->tyre_alarm,LV_TEXT_ALIGN_RIGHT,0);
 static const char *positions[4]={"左前","右前","左后","右后"};
 for(int t=0;t<4;t++){
  small(tyres,14+t*148,36,positions[t],MUTED);
  v->tyre[t]=text(tyres,14+t*148,58,"-",&lv_font_montserrat_32,TEXT);
 }
 static const char *extra_names[3]={"车窗","后备箱","充电功率"};
 lv_obj_t **extra_values[3]={&v->windows,&v->trunk,&v->power};
 for(int t=0;t<3;t++){
  lv_obj_t *d=box(p,398+t*206,340,198,56,INSET,12);
  small(d,12,14,extra_names[t],MUTED);
  *extra_values[t]=cn(d,12,14,"-",MUTED);
  lv_obj_set_width(*extra_values[t],174);
  lv_obj_set_style_text_align(*extra_values[t],LV_TEXT_ALIGN_RIGHT,0);
 }
 render_car_view(v,true);
}
