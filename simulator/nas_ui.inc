/* NAS: host status of the machine GSN runs on (load, memory, SoC/GPU/disk temperature) and the capacity of its two volumes.
   The card sits behind NET and is only shown when GSN reports a nas section. The disk temperature is only read while the
   disk spins; a sleeping disk shows 休眠 and is never woken for it. */
typedef struct {
 lv_obj_t *load_arc,*load_value,*memory_arc,*memory_value,*soc,*disk,*state,*wakes;
 lv_obj_t *activity[GSO_MAX_VOLUMES],*cells[GSO_MAX_VOLUMES][3];
 lv_obj_t *names[GSO_MAX_VOLUMES],*totals[GSO_MAX_VOLUMES],*values[GSO_MAX_VOLUMES],*bars[GSO_MAX_VOLUMES],*percents[GSO_MAX_VOLUMES];
} nas_view;
static nas_view nas_views[2]; /* 0 = main card, 1 = detail page */

static bool nas_reported(void){
 return model.nas.reported&&model.nas_source.quality!=GSO_UNCONFIGURED;
}
static gso_quality nas_quality(void){
 return gso_source_quality(model.nas_source,desk_data_now_ms(),45000,120000);
}
/* Disk temperature turns orange from 50 °C and red from 60 °C. */
static uint32_t nas_disk_color(gso_metric m){
 double shown=(double)(long)(m.value+0.5);
 if(!m.valid||shown<50.0)return TEXT;
 return shown<60.0?AMBER:RED;
}
static void nas_temperature(lv_obj_t *label,gso_metric m,bool live,bool disk,bool state_shown){
 if(!label)return;
 if(live&&m.valid){
  lv_label_set_text_fmt(label,"%.0f°C",m.value);
  lv_obj_set_style_text_color(label,c(disk?nas_disk_color(m):temperature_color(m,TEXT)),0);
  return;
 }
 const char *why=!live?"-":disk&&!state_shown&&m.reason&&!strcmp(m.reason,"disk_standby")?"休眠":disk&&!state_shown&&m.reason&&!strcmp(m.reason,"disk_idle")?"空闲":disk&&m.reason&&!strcmp(m.reason,"disk_unavailable")?"无法读取":disk&&m.reason&&!strcmp(m.reason,"not_configured")?"未配置":"-";
 lv_label_set_text(label,why);
 lv_obj_set_style_text_color(label,c(MUTED),0);
}
/* Two states only, from GSN: the disk is awake (a request completed within its idle timeout, or the drive says so) or asleep.
   Until GSN knows, and from an old GSN, the state is not shown. The card shows the wake-ups since midnight in its own label,
   the detail page behind the state. */
static void nas_state(nas_view *v,const gso_nas *n,bool live){
 if(!v->state)return;
 const char *text="-";uint32_t color=MUTED;
 if(live)switch(n->disk_state){
  case GSO_DISK_ACTIVE:
  case GSO_DISK_IDLE:text="已唤醒";color=GREEN;break;
  case GSO_DISK_STANDBY:text="已休眠";color=BLUE;break;
  default:break;
 }
 bool known=strcmp(text,"-")!=0;
 char wakes[16]="";
 if(known&&n->disk_wakes.valid)snprintf(wakes,sizeof(wakes),"今日%.0f次",n->disk_wakes.value);
 if(v->wakes){
  lv_label_set_text(v->state,text);
  lv_label_set_text(v->wakes,wakes);
 }else{
  char both[40];
  snprintf(both,sizeof(both),"%s%s%s",text,wakes[0]?"  ":"",wakes);
  lv_label_set_text(v->state,both);
 }
 lv_obj_set_style_text_color(v->state,c(color),0);
}
/* Busy percent and throughput of the device behind a volume (eMMC and HDD are told apart by the volume label): one line on
   the card, three figures on the detail page. A figure the device has not produced shows "-". */
static void nas_volume_activity(nas_view *v,unsigned i,bool live){
 const gso_volume *volume=i<model.volume_count?&model.volumes[i]:NULL;
 const gso_nas *n=&model.nas;
 gso_metric busy={0},read={0},write={0};
 if(live&&volume&&!strcmp(volume->label,"eMMC")){busy=n->emmc_busy;read=n->emmc_read_rate;write=n->emmc_write_rate;}
 else if(live&&volume&&!strcmp(volume->label,"HDD")){busy=n->disk_busy;read=n->disk_read_rate;write=n->disk_write_rate;}
 char busy_text[16]="占用 -",read_text[24]="-",write_text[24]="-";
 if(busy.valid)snprintf(busy_text,sizeof(busy_text),"占用 %.0f%%",busy.value);
 disk_rate_text(read_text,sizeof(read_text),read);
 disk_rate_text(write_text,sizeof(write_text),write);
 bool any=busy.valid||read.valid||write.valid;
 if(v->activity[i]){
  if(any)lv_label_set_text_fmt(v->activity[i],"%s  读 %s  写 %s",busy_text,read_text,write_text);
  else lv_label_set_text(v->activity[i],"");
 }
 if(v->cells[i][0]){
  lv_label_set_text(v->cells[i][0],any?busy_text:"");
  char cell[32]="";
  if(any)snprintf(cell,sizeof(cell),"读 %s",read_text);
  lv_label_set_text(v->cells[i][1],cell);
  cell[0]=0;
  if(any)snprintf(cell,sizeof(cell),"写 %s",write_text);
  lv_label_set_text(v->cells[i][2],cell);
  lv_obj_set_style_text_color(v->cells[i][0],c(busy.valid&&busy.value>=90?RED:busy.valid&&busy.value>=70?AMBER:TEXT),0);
 }
}
static void nas_gauge(lv_obj_t *arc,lv_obj_t *label,gso_metric m,bool live,uint32_t color){
 if(!arc)return;
 bool known=live&&m.valid;
 lv_arc_set_value(arc,known?(int)lround(fmax(0,fmin(100,m.value))):0);
 uint32_t level=!known?MUTED:m.value>=90?RED:m.value>=75?AMBER:color;
 lv_obj_set_style_arc_color(arc,c(level),LV_PART_INDICATOR);
 if(known)lv_label_set_text_fmt(label,"%.0f%%",m.value);
 else lv_label_set_text(label,"-");
}
static void render_nas_volumes(nas_view *v){
 gso_quality quality=gso_source_quality(model.storage,desk_data_now_ms(),90000,180000);
 for(unsigned i=0;i<GSO_MAX_VOLUMES;i++){
  if(!v->names[i])continue;
  const gso_volume *volume=i<model.volume_count?&model.volumes[i]:NULL;
  bool known=volume&&volume->total.valid&&volume->free.valid&&volume->total.value>0;
  bool ok=known&&quality==GSO_OK;
  double free_ratio=known?volume->free.value/volume->total.value:0;
  lv_label_set_text(v->names[i],volume?volume->label:"-");
  if(ok){
   char free_text[16],total_text[16];
   storage_text(free_text,sizeof(free_text),volume->free.value);
   storage_text(total_text,sizeof(total_text),volume->total.value);
   lv_label_set_text_fmt(v->values[i],"可用 %s",free_text);
   lv_label_set_text_fmt(v->totals[i],"共%s",total_text);
  }else{
   lv_label_set_text(v->values[i],!volume?"未接入":quality!=GSO_OK?quality_text(quality):"容量不可用");
   lv_label_set_text(v->totals[i],"");
  }
  if(v->percents[i]){
   if(ok)lv_label_set_text_fmt(v->percents[i],"%d%%",(int)lround((1.0-free_ratio)*100));
   else lv_label_set_text(v->percents[i],"-");
  }
  lv_bar_set_value(v->bars[i],ok?(int)lround((1.0-free_ratio)*100):0,LV_ANIM_OFF);
  lv_obj_set_style_bg_color(v->bars[i],c(!ok?MUTED:free_ratio>=0.2?GREEN:free_ratio>=0.1?AMBER:RED),LV_PART_INDICATOR);
  lv_obj_set_style_text_color(v->values[i],c(ok?TEXT:AMBER),0);
 }
}
static void render_nas_view(nas_view *v){
 if(!v->load_arc)return;
 gso_quality q=nas_quality();
 bool live=nas_reported()&&(q==GSO_OK||q==GSO_STALE);
 const gso_nas *n=&model.nas;
 nas_gauge(v->load_arc,v->load_value,n->cpu,live,GREEN);
 nas_gauge(v->memory_arc,v->memory_value,n->memory,live,BLUE);
 nas_temperature(v->soc,n->soc,live,false,false);
 nas_temperature(v->disk,n->disk,live,true,n->disk_state!=GSO_DISK_UNKNOWN);
 nas_state(v,n,live);
 render_nas_volumes(v);
 for(unsigned i=0;i<GSO_MAX_VOLUMES;i++)nas_volume_activity(v,i,live);
}
/* Disk health page (detail 22), opened from the NAS detail page: GSN judges the SMART counters and this page says it in plain
   words. The counters come with the temperature read, so they stay as last read while the disk sleeps. */
typedef struct {
 lv_obj_t *dot,*verdict,*sentence,*values[4],*notes[4];
} nas_health_view;
static nas_health_view nas_health;
static void nas_age_text(char *out,size_t size,double hours){
 unsigned long h=(unsigned long)hours,days=h/24,months=(unsigned long)(hours/730.0);
 if(h<48)snprintf(out,size,"%lu 小时",h);
 else if(months<1)snprintf(out,size,"%lu 天",days);
 else if(months<12)snprintf(out,size,"%lu 个月",months);
 else if(months%12)snprintf(out,size,"%lu 年 %lu 个月",months/12,months%12);
 else snprintf(out,size,"%lu 年",months/12);
}
static void nas_count_text(char *out,size_t size,gso_metric m,const char *unit){
 if(m.valid)snprintf(out,size,"%.0f %s",m.value,unit);
 else snprintf(out,size,"-");
}
/* 0 = fine or unknown, 1 = worth watching, 2 = at risk; only while the NAS figures are current. */
static int nas_health_alert(void){
 gso_quality q=nas_quality();
 if(!nas_reported()||q!=GSO_OK)return 0;
 return model.nas.disk_health==GSO_DISK_HEALTH_BAD?2:model.nas.disk_health==GSO_DISK_HEALTH_WATCH?1:0;
}
static void render_nas_health(void){
 nas_health_view *v=&nas_health;
 if(!v->verdict)return;
 gso_quality q=nas_quality();
 bool live=nas_reported()&&(q==GSO_OK||q==GSO_STALE);
 const gso_nas *n=&model.nas;
 gso_disk_health health=live?n->disk_health:GSO_DISK_HEALTH_UNKNOWN;
 const char *verdict="-",*sentence;uint32_t color=MUTED;
 switch(health){
  case GSO_DISK_HEALTH_GOOD:verdict="良好";color=GREEN;sentence="没有发现坏扇区或其他自检异常，状态正常。";break;
  case GSO_DISK_HEALTH_WATCH:verdict="需要留意";color=AMBER;sentence="出现了少量坏扇区，目前还能正常使用；建议备份重要数据，并隔一段时间再看是否在增加。";break;
  case GSO_DISK_HEALTH_BAD:verdict="有风险";color=RED;sentence="硬盘可能快要损坏，请尽快备份重要数据，并考虑更换。";break;
  default:sentence=live&&n->disk_hours.valid?"这块硬盘没有提供坏扇区计数，无法判断好坏。":"还没有读到硬盘自检数据；硬盘休眠时不会特意唤醒它，等它工作时会自动读取。";break;
 }
 lv_label_set_text(v->verdict,verdict);
 lv_obj_set_style_text_color(v->verdict,c(color),0);
 lv_obj_set_style_bg_color(v->dot,c(color),0);
 lv_label_set_text(v->sentence,sentence);
 char text[64]="-",note[96]="";
 uint32_t tone=MUTED;
 /* 已使用 */
 if(live&&n->disk_hours.valid){
  char age[32];
  nas_age_text(age,sizeof(age),n->disk_hours.value);
  snprintf(text,sizeof(text),"%s",age);
  snprintf(note,sizeof(note),"累计通电 %.0f 小时",n->disk_hours.value);
  tone=TEXT;
 }
 lv_label_set_text(v->values[0],text);lv_label_set_text(v->notes[0],note);lv_obj_set_style_text_color(v->values[0],c(tone),0);
 /* 坏扇区 */
 strcpy(text,"-");note[0]=0;tone=MUTED;
 if(live&&(n->disk_reallocated.valid||n->disk_pending.valid)){
  double pending=n->disk_pending.valid?n->disk_pending.value:0,replaced=n->disk_reallocated.valid?n->disk_reallocated.value:0;
  if(pending>0){snprintf(text,sizeof(text),"待处理 %.0f 个",pending);tone=pending>=10?RED:AMBER;}
  else if(replaced>0){snprintf(text,sizeof(text),"已替换 %.0f 个",replaced);tone=replaced>=100?RED:AMBER;}
  else{snprintf(text,sizeof(text),"没有");tone=GREEN;}
  char a[24],b[24];
  nas_count_text(a,sizeof(a),n->disk_reallocated,"个");nas_count_text(b,sizeof(b),n->disk_pending,"个");
  snprintf(note,sizeof(note),"已替换 %s，待处理 %s",a,b);
 }
 lv_label_set_text(v->values[1],text);lv_label_set_text(v->notes[1],note);lv_obj_set_style_text_color(v->values[1],c(tone),0);
 /* 接口通信错误 */
 strcpy(text,"-");note[0]=0;tone=MUTED;
 if(live&&n->disk_crc.valid){
  if(n->disk_crc.value>0){snprintf(text,sizeof(text),"%.0f 次",n->disk_crc.value);tone=AMBER;}
  else{snprintf(text,sizeof(text),"没有");tone=GREEN;}
  snprintf(note,sizeof(note),"数据线或接口接触不良时会增加，和盘本身好坏无关");
 }
 lv_label_set_text(v->values[2],text);lv_label_set_text(v->notes[2],note);lv_obj_set_style_text_color(v->values[2],c(tone),0);
 /* 启停次数 */
 strcpy(text,"-");note[0]=0;tone=MUTED;
 if(live&&n->disk_power_cycles.valid){
  nas_count_text(text,sizeof(text),n->disk_power_cycles,"次");tone=TEXT;
  char loads[24];
  nas_count_text(loads,sizeof(loads),n->disk_load_cycles,"次");
  snprintf(note,sizeof(note),"磁头加载 %s，每次休眠后唤醒都会增加",loads);
 }
 lv_label_set_text(v->values[3],text);lv_label_set_text(v->notes[3],note);lv_obj_set_style_text_color(v->values[3],c(tone),0);
}
static void render_nas(void){
 render_nas_view(&nas_views[0]);
 render_nas_view(&nas_views[1]);
 render_nas_health();
}
static lv_obj_t *nas_row(lv_obj_t *parent,int x,int y,int width,const char *name){
 small(parent,x,y,name,MUTED);
 lv_obj_t *value=small(parent,x,y,"-",TEXT);
 lv_obj_set_width(value,width);
 lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
 return value;
}
static void nas_gauges(lv_obj_t *parent,nas_view *v,int x,int y,int size,int gap){
 hardware_view load={0},memory={0};
 hardware_gauge(parent,x,y,size,&load,"负载 %",GREEN);
 hardware_gauge(parent,x+size+gap,y,size,&memory,"内存 %",BLUE);
 v->load_arc=load.arc;v->load_value=load.value;
 v->memory_arc=memory.arc;v->memory_value=memory.value;
}
/* Main card, 334x420 like the others. */
static void nas_card(void){
 lv_obj_t *o=card(6,"NAS",21);
 nas_view *v=&nas_views[0];
 nas_gauges(o,v,20,68,136,18);
 v->soc=nas_row(o,20,212,294,"SoC 温度");
 v->disk=nas_row(o,20,242,294,"HDD 温度");
 v->state=small(o,124,242,"-",MUTED);
 v->wakes=fine(o,186,246,"",MUTED);
 box(o,20,270,294,1,LINE,0);
 for(unsigned i=0;i<GSO_MAX_VOLUMES;i++){
  int y=280+(int)i*68;
  v->names[i]=small(o,20,y,"-",TEXT);
  lv_obj_set_width(v->names[i],64);
  lv_label_set_long_mode(v->names[i],LV_LABEL_LONG_DOT);
  v->totals[i]=fine(o,86,y+4,"",MUTED);
  v->values[i]=small(o,160,y,"未接入",AMBER);
  lv_obj_set_width(v->values[i],154);
  lv_obj_set_style_text_align(v->values[i],LV_TEXT_ALIGN_RIGHT,0);
  v->bars[i]=bar(o,20,y+30,294,GREEN);
  lv_obj_set_height(v->bars[i],6);
  v->activity[i]=fine(o,20,y+40,"",MUTED);
  lv_obj_set_width(v->activity[i],294);
 }
 lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
}
static void nas_detail_closed(void){
 memset(&nas_views[1],0,sizeof(nas_views[1]));
 memset(&nas_health,0,sizeof(nas_health));
}
static void nas_health_cb(lv_event_t *e){
 (void)e;
 desk_ui_detail(22);
 return_to_nas=true;
}
static void nas_health_detail(void){
 lv_obj_t *p=panel("NAS · 硬盘健康");
 detail_id=22;
 nas_health_view *v=&nas_health;
 lv_obj_t *top=box(p,24,56,984,124,INSET,12);
 small(top,20,12,"整体状况",MUTED);
 v->dot=box(top,20,48,20,20,MUTED,10);
 v->verdict=cn(top,52,46,"-",MUTED);
 v->sentence=small(top,20,80,"",TEXT);
 lv_obj_set_width(v->sentence,944);
 lv_label_set_long_mode(v->sentence,LV_LABEL_LONG_WRAP);
 static const char *names[4]={"已使用","坏扇区","接口通信错误","启停次数"};
 for(int k=0;k<4;k++){
  lv_obj_t *d=box(p,24+k*250,196,234,148,INSET,12);
  small(d,16,12,names[k],MUTED);
  v->values[k]=cn(d,16,44,"-",MUTED);
  lv_obj_set_width(v->values[k],206);
  v->notes[k]=fine(d,16,82,"",MUTED);
  lv_obj_set_width(v->notes[k],206);
  lv_label_set_long_mode(v->notes[k],LV_LABEL_LONG_WRAP);
 }
 small(p,24,360,"数据来自硬盘自检（SMART），与温度同一次读取，不会额外读写硬盘。",MUTED);
 render_nas_health();
}
static void nas_detail(void){
 lv_obj_t *p=panel("NAS · 主机状态");
 detail_id=21;
 button(p,820,12,184,"硬盘健康",nas_health_cb);
 nas_view *v=&nas_views[1];
 lv_obj_t *left=box(p,20,60,330,340,INSET,12);
 nas_gauges(left,v,20,16,136,18);
 /* The board is fixed hardware (拾光坞 N3), so its description is fixed text. */
 lv_obj_t *chip=nas_row(left,20,158,290,"SoC");
 lv_label_set_text(chip,"RK3566 4-Core@1.8GHz");
 lv_obj_t *ram=nas_row(left,20,186,290,"内存");
 lv_label_set_text(ram,"2GB DDR4");
 box(left,20,218,290,1,LINE,0);
 v->soc=nas_row(left,20,230,290,"SoC 温度");
 v->disk=nas_row(left,20,264,290,"HDD 温度");
 v->state=nas_row(left,20,298,290,"HDD 状态");
 lv_obj_t *right=box(p,370,60,634,340,INSET,12);
 for(unsigned i=0;i<GSO_MAX_VOLUMES;i++){
  int y=12+(int)i*164;
  v->names[i]=text(right,20,y,"-",&lv_font_montserrat_32,TEXT);
  lv_obj_set_width(v->names[i],300);
  lv_label_set_long_mode(v->names[i],LV_LABEL_LONG_DOT);
  v->percents[i]=text(right,420,y,"-",&lv_font_montserrat_32,TEXT);
  lv_obj_set_width(v->percents[i],194);
  lv_obj_set_style_text_align(v->percents[i],LV_TEXT_ALIGN_RIGHT,0);
  v->bars[i]=bar(right,20,y+56,594,GREEN);
  lv_obj_set_height(v->bars[i],12);
  v->values[i]=small(right,20,y+84,"未接入",AMBER);
  lv_obj_set_width(v->values[i],300);
  v->totals[i]=small(right,320,y+84,"",MUTED);
  lv_obj_set_width(v->totals[i],294);
  lv_obj_set_style_text_align(v->totals[i],LV_TEXT_ALIGN_RIGHT,0);
  for(int k=0;k<3;k++)v->cells[i][k]=small(right,20+k*200,y+114,"",TEXT);
 }
 render_nas_view(v);
}
