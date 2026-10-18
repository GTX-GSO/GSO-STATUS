#include "ui.h"
#include <math.h>
#include <time.h>
static bool light_theme;
static int theme_mode=-1;
int desk_ui_theme_mode(void){return theme_mode;}
void desk_ui_theme_mode_set(int mode){theme_mode=mode<0?-1:mode?1:0;desk_ui_theme_auto();}
static const uint32_t palettes[][2]={{0x203A34,0xE4F3EA},{0x101419,0xE9EFF5},{0x1A2129,0xFFFFFF},{0x131A21,0xF2F6FA},{0xE9EEF4,0x182B3D},{0x8997A7,0x526679},{0xA5E5C4,0x16774F},{0x8EBFF5,0x246EC4},{0xF1BF77,0x99620A},{0x2B3641,0xD6E1EB},{0x394A57,0xBFCFDF}};
uint32_t desk_ui_theme_color(uint32_t value){for(unsigned i=0;i<sizeof(palettes)/sizeof(palettes[0]);i++)if(value==palettes[i][0])return palettes[i][light_theme?1:0];return value;}
static lv_color_t mapped(lv_color_t color,bool light){uint32_t value=lv_color_to_int(color)&0xFFFFFF;for(unsigned i=0;i<sizeof(palettes)/sizeof(palettes[0]);i++)if(value==palettes[i][light?0:1])return lv_color_hex(palettes[i][light?1:0]);return color;}
static void recolor(lv_obj_t *o,bool light){
 const lv_part_t parts[]={LV_PART_MAIN,LV_PART_INDICATOR,LV_PART_KNOB,LV_PART_ITEMS};
 for(int i=0;i<4;i++){lv_part_t p=parts[i];lv_obj_set_style_bg_color(o,mapped(lv_obj_get_style_bg_color(o,p),light),p);lv_obj_set_style_text_color(o,mapped(lv_obj_get_style_text_color(o,p),light),p);lv_obj_set_style_arc_color(o,mapped(lv_obj_get_style_arc_color(o,p),light),p);lv_obj_set_style_line_color(o,mapped(lv_obj_get_style_line_color(o,p),light),p);}
 lv_obj_set_style_bg_color(o,mapped(lv_obj_get_style_bg_color(o,LV_PART_MAIN|LV_STATE_PRESSED),light),LV_STATE_PRESSED);
 for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)recolor(lv_obj_get_child(o,i),light);
}
void desk_ui_theme_set(bool light){if(light==light_theme)return;recolor(lv_screen_active(),light);light_theme=light;}
bool desk_ui_solar_day(int year,int month,int day,int minute,int *rise,int *set){
 static const int before[]={0,31,59,90,120,151,181,212,243,273,304,334};
 bool leap=year%4==0&&(year%100!=0||year%400==0);int doy=before[month-1]+day+(leap&&month>2);
 const double pi=3.141592653589793,lat=22.75*pi/180,lon=113.94;
 double g=2*pi/(leap?366:365)*(doy-1);
 double eq=229.18*(.000075+.001868*cos(g)-.032077*sin(g)-.014615*cos(2*g)-.040849*sin(2*g));
 double dec=.006918-.399912*cos(g)+.070257*sin(g)-.006758*cos(2*g)+.000907*sin(2*g)-.002697*cos(3*g)+.00148*sin(3*g);
 double angle=acos(cos(90.833*pi/180)/(cos(lat)*cos(dec))-tan(lat)*tan(dec))*180/pi;
 double noon=720-4*lon-eq+480;
 *rise=(int)round(noon-4*angle);*set=(int)round(noon+4*angle);return minute>=*rise&&minute<*set;
}
void desk_ui_solar_today(int *rise,int *set){time_t now=time(NULL)+8*3600;struct tm *t=gmtime(&now);desk_ui_solar_day(t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour*60+t->tm_min,rise,set);}
void desk_ui_theme_auto(void){if(theme_mode>=0){desk_ui_theme_set(theme_mode==1);return;}time_t now=time(NULL)+8*3600;struct tm *t=gmtime(&now);int rise,set;bool light=desk_ui_solar_day(t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour*60+t->tm_min,&rise,&set);desk_ui_theme_set(light);}
