/* One segmented control, with a sliding selection and explicit pending state. */
#include "clash_client.h"
static lv_obj_t *clash_segments[2][3],*clash_names[2][3],*clash_hints[2],*clash_pills[2];
static int clash_shown[2]={-2,-2},clash_cell[2],clash_pending=-1;
static const char *clash_modes[]={"rule","global","direct"};
static const char *clash_names_cn[]={"规则模式","全局模式","直连模式"};
static void net_controls_render(void);
static void clash_slide(void *pill,int32_t x){lv_obj_set_x(pill,x);}
static void clash_slide_finished(lv_anim_t *animation){lv_obj_set_x(animation->var,animation->end_value);}
static void clash_changed(lv_event_t *event){
 unsigned index=(unsigned)(uintptr_t)lv_event_get_user_data(event);if(index>=3)return;
 desk_clash_result r=desk_clash_latest();const char *actual=r.configured?r.mode:model.services[2].raw_state;
 if(strcmp(actual,clash_modes[index])&&desk_clash_mode(clash_modes[index]))clash_pending=(int)index;
 net_controls_render();
}
static void net_mode_controls(lv_obj_t *parent,unsigned group,int x,int y,int width){
 small(parent,x,y,"CLASH",MUTED);
 clash_hints[group]=small(parent,x+78,y,"",MUTED);
 lv_obj_set_width(clash_hints[group],width-78);
 lv_obj_set_style_text_align(clash_hints[group],LV_TEXT_ALIGN_RIGHT,0);
 lv_label_set_long_mode(clash_hints[group],LV_LABEL_LONG_DOT);
 lv_obj_t *track=box(parent,x,y+28,width,48,LINE,14);
 lv_obj_remove_flag(track,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
 int cell=(width-8)/3;clash_cell[group]=cell;clash_shown[group]=-2;
 clash_pills[group]=box(track,4,4,cell,40,GREEN,10);
 lv_obj_remove_flag(clash_pills[group],LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_flag(clash_pills[group],LV_OBJ_FLAG_HIDDEN);
 for(unsigned i=0;i<3;i++){
  lv_obj_t *segment=lv_button_create(track);
  clash_segments[group][i]=segment;
  lv_obj_remove_style_all(segment);
  lv_obj_set_pos(segment,4+(int)i*cell,4);
  lv_obj_set_size(segment,cell,40);
  lv_obj_set_style_bg_opa(segment,LV_OPA_TRANSP,0);
  lv_obj_set_style_opa(segment,255,LV_STATE_DISABLED);
  clash_names[group][i]=small(segment,0,0,clash_names_cn[i],MUTED);lv_obj_center(clash_names[group][i]);
  lv_obj_remove_flag(segment,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(segment,clash_changed,LV_EVENT_CLICKED,(void*)(uintptr_t)i);
 }
}
static void net_service_row(lv_obj_t *parent,unsigned group,int y,int width){
 const int slots[]={0,1,3};const char *names[]={"PROXY","DHCP","AntiAD"};int cell=width/3;
 for(int i=0;i<3;i++){
  lv_obj_t *label=small(parent,20+i*cell,y,names[i],MUTED);
  lv_obj_set_width(label,cell);
  lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
  service_labels[group][slots[i]]=small(parent,20+i*cell,y+24,"未接入",AMBER);
  lv_obj_set_width(service_labels[group][slots[i]],cell);
  lv_obj_set_style_text_align(service_labels[group][slots[i]],LV_TEXT_ALIGN_CENTER,0);
 }
}
/* The services of this project itself: GSN (the link to the NAS aggregator), GSW (Windows collector) and HA (Home Assistant). */
static void net_stack_row(lv_obj_t *parent,unsigned group,int y,int width){
 const char *names[]={"GSN","GSW","HA"};int cell=width/3;
 for(int i=0;i<3;i++){
  lv_obj_t *label=small(parent,20+i*cell,y,names[i],MUTED);
  lv_obj_set_width(label,cell);
  lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
  stack_labels[group][i]=small(parent,20+i*cell,y+24,"未接入",AMBER);
  lv_obj_set_width(stack_labels[group][i],cell);
  lv_obj_set_style_text_align(stack_labels[group][i],LV_TEXT_ALIGN_CENTER,0);
 }
}
static void net_controls_render(void){
 desk_clash_result r=desk_clash_latest();gso_quality q=gso_source_quality(model.services[2].source,desk_data_now_ms(),45000,120000);
 bool fresh=r.observed_ms&&desk_data_now_ms()>=r.observed_ms&&desk_data_now_ms()-r.observed_ms<=30000;
 const char *actual=r.configured?(fresh?r.mode:""):q==GSO_OK?model.services[2].raw_state:"";
 int selected=-1;for(int i=0;i<3;i++)if(!strcmp(actual,clash_modes[i]))selected=i;
 bool pending=r.busy&&clash_pending>=0;if(pending)selected=clash_pending;else if(!r.busy)clash_pending=-1;
 const char *hint=!r.configured?"控制未配置":r.busy?"":r.state==CLASH_AUTH?"认证失败":r.state==CLASH_UNCONFIRMED?"状态未确认":!fresh||r.state==CLASH_UNAVAILABLE?"控制端离线":"";
 for(int group=0;group<2;group++){
  if(clash_hints[group])lv_label_set_text(clash_hints[group],hint);
  lv_obj_t *pill=clash_pills[group];if(pill){
   if(selected<0){lv_anim_delete(pill,clash_slide);lv_obj_add_flag(pill,LV_OBJ_FLAG_HIDDEN);}
   else{
    lv_obj_remove_flag(pill,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(pill,c(pending?AMBER:GREEN),0);
    if(selected!=clash_shown[group]){
     int target=4+selected*clash_cell[group];
     lv_anim_delete(pill,clash_slide);
     if(clash_shown[group]<0)lv_obj_set_x(pill,target);
     else{
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a,pill);
      lv_anim_set_exec_cb(&a,clash_slide);
      lv_anim_set_values(&a,lv_obj_get_x(pill),target);
      lv_anim_set_duration(&a,160);
      lv_anim_set_completed_cb(&a,clash_slide_finished);
      lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
      lv_anim_start(&a);
     }
    }
   }
   clash_shown[group]=selected;
  }
  for(int i=0;i<3;i++)if(clash_segments[group][i]){
   bool active=selected==i;
   if(active)lv_obj_add_state(clash_segments[group][i],LV_STATE_CHECKED);
   else lv_obj_remove_state(clash_segments[group][i],LV_STATE_CHECKED);
   if(!r.configured||r.busy)lv_obj_add_state(clash_segments[group][i],LV_STATE_DISABLED);
   else lv_obj_remove_state(clash_segments[group][i],LV_STATE_DISABLED);
   lv_obj_set_style_text_color(clash_names[group][i],c(active?INSET:MUTED),0);
  }
 }
}
static void net_close_controls(void){
 memset(clash_segments[1],0,sizeof(clash_segments[1]));
 memset(clash_names[1],0,sizeof(clash_names[1]));
 clash_hints[1]=clash_pills[1]=NULL;
 clash_shown[1]=-2;
}
