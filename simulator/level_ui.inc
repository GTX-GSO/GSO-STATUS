/* Level page: the display's left/right tilt from the board's accelerometer (shared/gso_level.c does the arithmetic). The
   bubble in the tube moves to the higher side like a real spirit level. The sensor is powered only while this page is open. */
#define LEVEL_TUBE_WIDTH 920
#define LEVEL_BUBBLE 56
#define LEVEL_PIXELS_PER_DEGREE 90
static lv_obj_t *level_angle,*level_direction,*level_bubble,*level_tilt,*level_raw;
static lv_timer_t *level_timer;
static gso_level level_state;
static const char *level_fault;
static unsigned level_failures;
static int level_bubble_x=-1;
static void level_view_close(void){
 if(level_timer){
  lv_timer_delete(level_timer);
  level_timer=NULL;
  gss_imu_close();
 }
 level_angle=level_direction=level_bubble=level_tilt=level_raw=NULL;
 level_bubble_x=-1;
 
}
static void level_show(const char *angle,const char *direction,uint32_t direction_color,int bubble_offset,uint32_t bubble_color,bool bubble_visible){
 changed_label(level_angle,angle);
 changed_label(level_direction,direction);
 lv_obj_set_style_text_color(level_direction,c(direction_color),0);
 int x=(LEVEL_TUBE_WIDTH-6-LEVEL_BUBBLE)/2+bubble_offset;
 if(x!=level_bubble_x){
  level_bubble_x=x;
  lv_obj_set_x(level_bubble,x);
 }
 lv_obj_set_style_bg_color(level_bubble,c(bubble_color),0);
 if(lv_obj_has_flag(level_bubble,LV_OBJ_FLAG_HIDDEN)==bubble_visible){
  if(bubble_visible)lv_obj_remove_flag(level_bubble,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(level_bubble,LV_OBJ_FLAG_HIDDEN);
 }
}
static void level_render(void){
 if(!level_angle)return;
 const gso_level *l=&level_state;
 char line[64];
 if(level_fault){
  level_show("-",level_fault,AMBER,0,MUTED,false);
  changed_label(level_tilt,"-");
  changed_label(level_raw,"-");
  return;
 }
 if(!l->seeded){
  level_show("-",l->state==GSO_LEVEL_MOVING?"请保持屏幕不动":"",AMBER,0,MUTED,false);
  changed_label(level_tilt,"-");
  changed_label(level_raw,"-");
  return;
 }
 snprintf(line,sizeof(line),"x %+.2f   y %+.2f   z %+.2f g",l->filtered.x,l->filtered.y,l->filtered.z);
 changed_label(level_raw,line);
 if(l->tilt_deg>0.05f)snprintf(line,sizeof(line),"后仰 %.1f°",l->tilt_deg);
 else if(l->tilt_deg<-0.05f)snprintf(line,sizeof(line),"前倾 %.1f°",-l->tilt_deg);
 else snprintf(line,sizeof(line),"垂直");
 changed_label(level_tilt,line);
 if(l->state==GSO_LEVEL_FLAT){
  level_show("-","屏幕接近水平放置，没有左右倾斜可测",MUTED,0,MUTED,false);
  return;
 }
 gso_level_band band=gso_level_band_of(l->roll_deg);
 uint32_t color=band==GSO_LEVEL_FLATNESS_LEVEL?GREEN:band==GSO_LEVEL_FLATNESS_NEAR?AMBER:RED;
 float roll=l->roll_deg;
 snprintf(line,sizeof(line),"%.1f°",fabsf(roll));
 const char *direction=band==GSO_LEVEL_FLATNESS_LEVEL?"已摆平":roll>0?"右侧偏低":"左侧偏低";
 if(l->state==GSO_LEVEL_MOVING)direction="请保持屏幕不动";
 /* the bubble moves toward the higher side: the right side low sends it to the left */
 float offset=-roll*LEVEL_PIXELS_PER_DEGREE;
 float limit=(LEVEL_TUBE_WIDTH-6-LEVEL_BUBBLE)/2.0f-4;
 offset=offset>limit?limit:offset<-limit?-limit:offset;
 level_show(line,direction,l->state==GSO_LEVEL_MOVING?AMBER:color,(int)(offset<0?offset-0.5f:offset+0.5f),color,true);
}
static void level_tick(lv_timer_t *t){
 (void)t;
 gso_accel sample;
 if(gss_imu_read(&sample)){
  level_failures=0;
  gso_level_update(&level_state,sample);
 }
 else if(++level_failures>=10){
  level_fault="传感器没有响应";
  gso_level_no_sensor(&level_state);
 }
 level_render();
}
static void level_open(void){
 lv_obj_t *p=panel("水平仪");
 detail_id=19;
 lv_obj_t *card=box(p,24,56,984,232,INSET,14);
 small(card,20,14,"左右倾斜",MUTED);
 level_angle=text(card,0,0,"-",&lv_font_montserrat_48,TEXT);
 lv_obj_set_width(level_angle,400);
 lv_obj_set_style_text_align(level_angle,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_align(level_angle,LV_ALIGN_TOP_MID,0,28);
 level_direction=cn(card,0,0,"",TEXT);
 lv_obj_set_width(level_direction,700);
 lv_obj_set_style_text_align(level_direction,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_align(level_direction,LV_ALIGN_TOP_MID,0,90);
 lv_obj_t *tube=box(card,32,134,LEVEL_TUBE_WIDTH,64,LINE,32);
 lv_obj_t *inner=box(tube,3,3,LEVEL_TUBE_WIDTH-6,58,BG,29);
 int middle=(LEVEL_TUBE_WIDTH-6)/2;
 box(inner,middle-LEVEL_BUBBLE/2-6,10,2,38,MUTED,1);
 box(inner,middle+LEVEL_BUBBLE/2+4,10,2,38,MUTED,1);
 level_bubble=box(inner,0,1,LEVEL_BUBBLE,LEVEL_BUBBLE,MUTED,LEVEL_BUBBLE/2);
 lv_obj_add_flag(level_bubble,LV_OBJ_FLAG_HIDDEN);
 lv_obj_t *pitch=box(p,24,304,480,72,INSET,14);
 small(pitch,20,12,"前后倾斜",MUTED);
 level_tilt=cn(pitch,20,38,"-",TEXT);
 lv_obj_t *raw=box(p,520,304,488,72,INSET,14);
 small(raw,20,12,"加速度读数",MUTED);
 level_raw=small(raw,20,38,"-",TEXT);
 small(p,24,388,"水平仪读取重力方向，不需要预先校准；传感器自身的零偏会带来小角度误差。",MUTED);
 gso_level_reset(&level_state);
 level_fault=NULL;
 level_failures=0;
 level_bubble_x=-1;
 if(!gss_imu_open())level_fault="未检测到加速度传感器";
 level_timer=lv_timer_create(level_tick,50,NULL);
 level_render();
}
