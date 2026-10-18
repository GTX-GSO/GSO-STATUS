/* Cached daily/hourly weather; one primitive draw object per symbol. */
static lv_obj_t *weather_detail[5],*forecast_text[12][4],*forecast_tiles[12],*forecast_icons[12],*weather_tabs[2],*weather_scroller,*detail_weather_icon;
static bool weather_hourly;
static void close_weather(void){
 memset(weather_detail,0,sizeof(weather_detail));
 memset(forecast_text,0,sizeof(forecast_text));
 memset(forecast_tiles,0,sizeof(forecast_tiles));
 memset(forecast_icons,0,sizeof(forecast_icons));
 memset(weather_tabs,0,sizeof(weather_tabs));
 weather_scroller=detail_weather_icon=NULL;
}
static void weather_tab_click(lv_event_t *e){
 weather_hourly=(uintptr_t)lv_event_get_user_data(e)!=0;
 if(weather_scroller)lv_obj_scroll_to_x(weather_scroller,0,LV_ANIM_OFF);
 render_weather();
}
static void weather_details(lv_obj_t *p){
 weather_hourly=false;detail_weather_icon=weather_icon_create(p,24,63,56);weather_detail[0]=cn(p,96,80,"天气",TEXT);
 weather_detail[1]=text(p,710,68,"—",&lv_font_montserrat_40,TEXT);
 const char *names[]={"湿度","气压","风速 / 风向"};
 for(int i=0;i<3;i++){
  small(p,24+i*330,146,names[i],MUTED);
  weather_detail[i+2]=small(p,24+i*330,174,"—",TEXT);
  lv_obj_set_width(weather_detail[i+2],310);
 }
 for(int i=0;i<2;i++){
  weather_tabs[i]=button(p,24+i*150,207,140,i?"每小时":"每日",weather_tab_click);
  lv_obj_set_height(weather_tabs[i],32);
  lv_obj_remove_event_cb(weather_tabs[i],weather_tab_click);
  lv_obj_add_event_cb(weather_tabs[i],weather_tab_click,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
 }
 weather_scroller=box(p,20,249,984,148,PANEL,0);
 lv_obj_add_flag(weather_scroller,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_scroll_dir(weather_scroller,LV_DIR_HOR);
 lv_obj_set_scrollbar_mode(weather_scroller,LV_SCROLLBAR_MODE_AUTO);
 for(int i=0;i<12;i++){
  lv_obj_t *tile=forecast_tiles[i]=box(weather_scroller,i*164,0,155,148,INSET,10);
  forecast_icons[i]=weather_icon_create(tile,59,34,36);
  int y[]={10,76,102,124};
  for(int k=0;k<4;k++){
   forecast_text[i][k]=small(tile,10,y[k],"—",k>1?MUTED:TEXT);
   lv_obj_set_width(forecast_text[i][k],135);
   lv_obj_set_style_text_align(forecast_text[i][k],LV_TEXT_ALIGN_CENTER,0);
  }
 }
}
static void render_weather(void){
 if(!weather_detail[0])return;
 weather_icon_update(detail_weather_icon,model.weather_condition);
 lv_label_set_text(weather_detail[0],weather_condition_name(model.weather_condition));
 if(model.weather_temperature.valid)lv_label_set_text_fmt(weather_detail[1],"%.1f°C",model.weather_temperature.value);
 else lv_label_set_text(weather_detail[1],"—");
 if(model.humidity.valid)lv_label_set_text_fmt(weather_detail[2],"%.0f%%",model.humidity.value);
 else lv_label_set_text(weather_detail[2],"—");
 if(model.pressure.valid)lv_label_set_text_fmt(weather_detail[3],"%.1f hPa",model.pressure.value);
 else lv_label_set_text(weather_detail[3],"—");
 if(model.wind.valid){
  const char *directions[]={"北","东北偏北","东北","东北偏东","东","东南偏东","东南","东南偏南","南","西南偏南","西南","西南偏西","西","西北偏西","西北","西北偏北"};
  if(model.wind_bearing.valid)lv_label_set_text_fmt(weather_detail[4],"%.1f km/h · %s",model.wind.value,directions[((int)lround(model.wind_bearing.value/22.5))%16]);
  else lv_label_set_text_fmt(weather_detail[4],"%.1f km/h",model.wind.value);
 }else lv_label_set_text(weather_detail[4],"—");
 for(int tab=0;tab<2;tab++){
  bool selected=weather_hourly==(tab==1);
  lv_obj_set_style_bg_color(weather_tabs[tab],c(selected?BLUE:LINE),0);
  lv_obj_set_style_text_color(lv_obj_get_child(weather_tabs[tab],0),c(selected?BG:TEXT),0);
 }
 unsigned count=weather_hourly?model.hourly_count:model.forecast_count;
 gso_forecast *items=weather_hourly?model.hourly_forecast:model.forecast;
 for(unsigned i=0;i<12;i++){
  if(!weather_hourly&&i>=6){
   lv_obj_add_flag(forecast_tiles[i],LV_OBJ_FLAG_HIDDEN);
   continue;
  }
  lv_obj_remove_flag(forecast_tiles[i],LV_OBJ_FLAG_HIDDEN);
  int width=weather_hourly?114:155;
  lv_obj_set_x(forecast_tiles[i],i*(weather_hourly?124:164));
  lv_obj_set_width(forecast_tiles[i],width);
  lv_obj_set_x(forecast_icons[i],(width-36)/2);
  for(int k=0;k<4;k++)lv_obj_set_width(forecast_text[i][k],width-20);
 if(i<count){
  gso_forecast *day=&items[i];
  time_t date=(time_t)day->date+8*3600;
  struct tm *tm=gmtime(&date);
  if(tm){
   if(weather_hourly)lv_label_set_text_fmt(forecast_text[i][0],"%02d:%02d",tm->tm_hour,tm->tm_min);
   else lv_label_set_text_fmt(forecast_text[i][0],"%02d/%02d",tm->tm_mon+1,tm->tm_mday);
  }
  weather_icon_update(forecast_icons[i],day->condition);
  lv_label_set_text(forecast_text[i][1],weather_condition_name(day->condition));
  if(day->high.valid)lv_label_set_text_fmt(forecast_text[i][2],weather_hourly?"%.1f°C":"高 %.1f°C",day->high.value);
  else lv_label_set_text(forecast_text[i][2],"—");
  if(!weather_hourly&&day->low.valid)lv_label_set_text_fmt(forecast_text[i][3],"低 %.1f°C",day->low.value);
  else lv_label_set_text(forecast_text[i][3],"");
 }else{
  weather_icon_update(forecast_icons[i],"");
  for(int k=0;k<4;k++)lv_label_set_text(forecast_text[i][k],k==1?"暂无预报":"—");
 }
 }
}
