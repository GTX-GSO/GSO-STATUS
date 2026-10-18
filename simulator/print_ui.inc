/* PRINT: Bambu Lab printer job status. The card is the last one; while a job is running (or just ended) it moves up to
   sit right behind VIBE. It is only shown when the producer reports a configured printer. */
typedef struct {
 lv_obj_t *state,*when,*job,*remaining,*percent,*bar,*layers,*nozzle,*bed,*error,*arc,*thumb;
} print_view;
static print_view print_views[2]; /* 0 = main card, 1 = detail page */
/* Model picture of the current job: GSN serves it as raw ARGB8888 under the id the snapshot names. */
static lv_image_dsc_t print_thumb_desc={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=GSO_THUMBNAIL_SIZE,.h=GSO_THUMBNAIL_SIZE,.stride=GSO_THUMBNAIL_SIZE*4},.data_size=GSO_THUMBNAIL_BYTES};
static uint8_t *print_thumb_pixels;
static char print_thumb_key[17];
static uint64_t print_attention_until_ms;
static unsigned print_last_state;
static time_t print_finished_at; /* when this display first saw the printer say "finished"; 0 while it does not */
static unsigned print_finished_seen;
#define PRINT_ATTENTION_MS (10u*60u*1000u)

static gso_quality print_quality(void){
 return gso_source_quality(model.printer,desk_data_now_ms(),30000,90000);
}
static bool print_reported(void){
 return model.print.reported&&model.printer.quality!=GSO_UNCONFIGURED;
}
/* The card moves forward while the printer works, and for ten minutes after a job it was watching ends or fails (a printer
   that already says "finished" when the display starts is not news). */
static bool print_attention(void){
 if(!print_reported())return false;
 gso_quality q=print_quality();
 if(q==GSO_OFFLINE||q==GSO_ERROR)return false;
 unsigned state=model.print.state;
 uint64_t now=desk_data_now_ms();
 bool was_working=print_last_state==GSO_PRINT_PREPARING||print_last_state==GSO_PRINT_RUNNING||print_last_state==GSO_PRINT_PAUSED;
 if((state==GSO_PRINT_FINISHED||state==GSO_PRINT_FAILED)&&was_working)print_attention_until_ms=now+PRINT_ATTENTION_MS;
 print_last_state=state;
 if(state==GSO_PRINT_PREPARING||state==GSO_PRINT_RUNNING||state==GSO_PRINT_PAUSED)return true;
 return now<print_attention_until_ms;
}
/* Local time (UTC+8, like the rest of the display) of the finished notification: HH:MM today, MM-DD HH:MM on another day. */
static void print_finished_text(char *out,size_t size){
 time_t at=print_finished_at+8*3600,now=time(NULL)+8*3600;
 struct tm then,today;
 struct tm *a=gmtime(&at);
 if(!a){out[0]=0;return;}
 then=*a;
 struct tm *b=gmtime(&now);
 if(!b){out[0]=0;return;}
 today=*b;
 if(then.tm_year==today.tm_year&&then.tm_yday==today.tm_yday)snprintf(out,size,"%02d:%02d 收到",then.tm_hour,then.tm_min);
 else snprintf(out,size,"%02d-%02d %02d:%02d 收到",then.tm_mon+1,then.tm_mday,then.tm_hour,then.tm_min);
}
static void print_note_finished(bool connected){
 unsigned state=connected?model.print.state:GSO_PRINT_UNKNOWN;
 if(state==GSO_PRINT_FINISHED&&print_finished_seen!=GSO_PRINT_FINISHED)print_finished_at=time(NULL);
 else if(state!=GSO_PRINT_FINISHED&&connected)print_finished_at=0;
 if(connected)print_finished_seen=state;
}
static const char *print_state_name(unsigned state,bool connected){
 if(!connected)return "未连接";
 switch(state){
  case GSO_PRINT_IDLE:return "待机";
  case GSO_PRINT_PREPARING:return "准备中";
  case GSO_PRINT_RUNNING:return "打印中";
  case GSO_PRINT_PAUSED:return "已暂停";
  case GSO_PRINT_FINISHED:return "已完成";
  case GSO_PRINT_FAILED:return "打印失败";
 }
 return "状态未知";
}
static uint32_t print_state_color(unsigned state,bool connected){
 if(!connected)return AMBER;
 switch(state){
  case GSO_PRINT_RUNNING:return BLUE;
  case GSO_PRINT_FINISHED:return GREEN;
  case GSO_PRINT_FAILED:return RED;
  case GSO_PRINT_IDLE:return MUTED;
 }
 return AMBER;
}
static void print_temperature(lv_obj_t *label,gso_metric current,gso_metric target,bool connected){
 if(!label)return;
 if(!connected||!current.valid)lv_label_set_text(label,"-");
 else if(target.valid&&target.value>0)lv_label_set_text_fmt(label,"%.0f / %.0f°C",current.value,target.value);
 else lv_label_set_text_fmt(label,"%.0f°C",current.value);
}
static void render_print_view(print_view *v,bool detail){
 if(!v->state)return;
 const gso_printer *p=&model.print;
 gso_quality q=print_quality();
 bool connected=print_reported()&&(q==GSO_OK||q==GSO_STALE);
 bool working=connected&&(p->state==GSO_PRINT_PREPARING||p->state==GSO_PRINT_RUNNING||p->state==GSO_PRINT_PAUSED);
 lv_label_set_text(v->state,print_state_name(p->state,connected));
 if(v->when){
  bool finished=connected&&p->state==GSO_PRINT_FINISHED&&print_finished_at;
  char when[40]="";
  if(finished)print_finished_text(when,sizeof(when));
  lv_label_set_text(v->when,when);
 }
 lv_obj_set_style_text_color(v->state,c(print_state_color(p->state,connected)),0);
 lv_label_set_text(v->job,connected&&p->job[0]&&p->state!=GSO_PRINT_IDLE?p->job:"");
 char text[48];
 if(v->remaining){
  if(working&&p->remaining_minutes.valid){
   if(detail)snprintf(text,sizeof(text),"%.0f:%02.0f",floor(p->remaining_minutes.value/60),fmod(p->remaining_minutes.value,60));
   else {
    char span[32];
    duration_text(span,sizeof(span),(uint64_t)(p->remaining_minutes.value*60));
    snprintf(text,sizeof(text),"剩余 %s",span);
   }
   lv_label_set_text(v->remaining,text);
  }else lv_label_set_text(v->remaining,detail?"-":"");
 }
 bool has_progress=connected&&p->progress.valid&&p->state!=GSO_PRINT_IDLE;
 if(v->percent){
  if(has_progress)lv_label_set_text_fmt(v->percent,"%.0f%%",p->progress.value);
  else lv_label_set_text(v->percent,"-");
 }
 if(v->bar)lv_bar_set_value(v->bar,has_progress?(int)p->progress.value:0,LV_ANIM_OFF);
 if(v->layers){
  if(connected&&p->layer.valid&&p->layer_total.valid&&p->layer_total.value>0&&p->state!=GSO_PRINT_IDLE)lv_label_set_text_fmt(v->layers,detail?"%.0f / %.0f":"层 %.0f / %.0f",p->layer.value,p->layer_total.value);
  else lv_label_set_text(v->layers,detail?"-":"");
 }
 print_temperature(v->nozzle,p->nozzle,p->nozzle_target,connected);
 print_temperature(v->bed,p->bed,p->bed_target,connected);
 if(v->error){
  bool failed=connected&&p->error_code.valid&&p->error_code.value>0;
  if(failed)lv_label_set_text_fmt(v->error,detail?"%08X":"错误代码 %08X",(unsigned)p->error_code.value); /* the printer reports it as a number; the hexadecimal form is how its codes are listed */
  else lv_label_set_text(v->error,detail?(connected?"无":"-"):"");
  lv_obj_set_style_text_color(v->error,c(failed?RED:detail?GREEN:MUTED),0);
 }
}
/* Shows the picture when the one for the current job has arrived; the previous job's picture is never shown for a new job. */
static void print_update_thumbnail(void){
 gso_quality q=print_quality();
 bool shown_job=print_reported()&&(q==GSO_OK||q==GSO_STALE)&&model.print.state!=GSO_PRINT_IDLE&&model.print.state!=GSO_PRINT_UNKNOWN;
 const char *key=shown_job?model.print.thumbnail:"";
 if(!key[0])print_thumb_key[0]=0;
 else if(strcmp(key,print_thumb_key)){
  if(!print_thumb_pixels)print_thumb_pixels=lv_malloc(GSO_THUMBNAIL_BYTES);
  if(print_thumb_pixels&&desk_data_thumbnail(key,print_thumb_pixels)){
   snprintf(print_thumb_key,sizeof(print_thumb_key),"%s",key);
   print_thumb_desc.data=print_thumb_pixels;
   for(int i=0;i<2;i++)if(print_views[i].thumb){
    lv_image_set_src(print_views[i].thumb,&print_thumb_desc);
    lv_obj_invalidate(print_views[i].thumb);
   }
  }
 }
 bool have=key[0]&&!strcmp(key,print_thumb_key);
 for(int i=0;i<2;i++)if(have&&print_views[i].thumb&&lv_image_get_src(print_views[i].thumb)!=&print_thumb_desc){ /* a page opened after the picture arrived */
  lv_image_set_src(print_views[i].thumb,&print_thumb_desc);
  lv_obj_invalidate(print_views[i].thumb);
 }
 for(int i=0;i<2;i++)if(print_views[i].thumb&&lv_obj_has_flag(print_views[i].thumb,LV_OBJ_FLAG_HIDDEN)==have){
  if(have)lv_obj_remove_flag(print_views[i].thumb,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(print_views[i].thumb,LV_OBJ_FLAG_HIDDEN);
 }
}
bool desk_ui_print_thumbnail_visible(bool detail){
 print_view *v=&print_views[detail?1:0];
 return v->thumb&&!lv_obj_has_flag(v->thumb,LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *print_thumb_create(lv_obj_t *parent,int x,int y){
 lv_obj_t *image=lv_image_create(parent);
 lv_obj_set_pos(image,x,y);
 lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);
 return image;
}
static void render_print(void){
 gso_quality q=print_quality();
 print_note_finished(print_reported()&&(q==GSO_OK||q==GSO_STALE));
 render_print_view(&print_views[0],false);
 render_print_view(&print_views[1],true);
 print_update_thumbnail();
}
/* Main card, 334x420 like the others. */
static void print_card(void){
 lv_obj_t *o=card(4,"PRINT",18);
 print_view *v=&print_views[0];
 v->state=cn(o,20,80,"未连接",AMBER);
 v->when=small(o,96,80,"",MUTED);
 v->job=small(o,20,117,"",TEXT);
 lv_obj_set_size(v->job,290,21);
 lv_label_set_long_mode(v->job,LV_LABEL_LONG_DOT);
 v->remaining=small(o,20,146,"",MUTED);
 v->thumb=print_thumb_create(o,(334-GSO_THUMBNAIL_SIZE)/2,176);
 small(o,20,307,"进度",MUTED);
 v->percent=text(o,160,298,"-",&lv_font_montserrat_40,TEXT);
 lv_obj_set_width(v->percent,150);
 lv_obj_set_style_text_align(v->percent,LV_TEXT_ALIGN_RIGHT,0);
 v->bar=bar(o,20,348,290,GREEN);
 v->layers=small(o,20,372,"",MUTED);
 v->error=small(o,0,0,"",RED);
 lv_obj_align(v->error,LV_ALIGN_TOP_RIGHT,-20,372);
 lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
}
static void print_detail_closed(void){
 memset(&print_views[1],0,sizeof(print_views[1]));
}
static void print_detail(void){
 lv_obj_t *p=panel("PRINT · 3D 打印");
 detail_id=18;
 print_view *v=&print_views[1];
 v->state=cn(p,24,66,"未连接",AMBER);
 v->when=small(p,100,66,"",MUTED);
 v->job=small(p,24,100,"",TEXT);
 lv_obj_set_size(v->job,215,54);
 v->thumb=print_thumb_create(p,250,56);
 lv_label_set_long_mode(v->job,LV_LABEL_LONG_DOT);
 for(int r=0;r<2;r++){
  lv_obj_t *d=box(p,24,248+r*76,346,68,INSET,12);
  small(d,16,10,r?"热床":"喷嘴",MUTED);
  lv_obj_t *value=text(d,16,32,"-",&lv_font_montserrat_24,TEXT);
  lv_obj_set_width(value,314);
  if(r)v->bed=value;else v->nozzle=value;
 }
 static const char *names[4]={"进度","剩余时间 时:分","层数","错误代码"};
 for(int t=0;t<4;t++){
  lv_obj_t *d=box(p,398+(t%2)*305,56+(t/2)*176,292,164,INSET,12);
  small(d,18,12,names[t],MUTED);
  if(t==0){
   v->percent=text(d,18,48,"-",&lv_font_montserrat_48,TEXT);
   v->bar=bar(d,18,122,256,GREEN);
  }else if(t==1)v->remaining=text(d,18,56,"-",&lv_font_montserrat_40,TEXT);
  else if(t==2)v->layers=text(d,18,56,"-",&lv_font_montserrat_32,TEXT);
  else v->error=cn(d,18,56,"-",GREEN);
 }
 render_print_view(v,true);
}
