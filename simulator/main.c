#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "ui.h"
#include "build_info.h"
#include "data_source.h"
#include "home_client.h"
#include "clash_client.h"
#include "gss_wifi.h"
#include "system_preview.h"
#include "display_brightness.h"
#include "ui_model.h"
#ifdef GSO_TEST
#include "src/misc/lv_area_private.h"
#endif

#define WIDTH 1280
#define HEIGHT 452
static HWND window;
static uint32_t framebuffer[WIDTH*HEIGHT];
static uint32_t drawbuffer[WIDTH*64];
static lv_point_t mouse;
static bool pressed;
static BITMAPINFO bitmap;
static lv_display_t *display;
static uint64_t flushed_pixels;
static unsigned flush_count;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *px) {
    int w=a->x2-a->x1+1;
    flushed_pixels+=(uint64_t)w*(a->y2-a->y1+1);flush_count++;
    for(int y=a->y1;y<=a->y2;y++) memcpy(framebuffer+y*WIDTH+a->x1,px+(y-a->y1)*w*4,w*4);
    if(lv_display_flush_is_last(d) && window) InvalidateRect(window,NULL,FALSE);
    lv_display_flush_ready(d);
}
static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void pointer_read(lv_indev_t *d,lv_indev_data_t *out) {
    (void)d; out->point=mouse; out->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void set_mouse(HWND h,LPARAM l) {
    RECT r; GetClientRect(h,&r);
    mouse.x=GET_X_LPARAM(l)*WIDTH/(r.right?r.right:1);
    mouse.y=GET_Y_LPARAM(l)*HEIGHT/(r.bottom?r.bottom:1);
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_PAINT: {
        PAINTSTRUCT p; HDC dc=BeginPaint(h,&p); RECT r;GetClientRect(h,&r);
        SetStretchBltMode(dc,HALFTONE);
        StretchDIBits(dc,0,0,r.right,r.bottom,0,0,WIDTH,HEIGHT,framebuffer,&bitmap,DIB_RGB_COLORS,SRCCOPY);
        EndPaint(h,&p); return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_LBUTTONDOWN:set_mouse(h,l);pressed=true;SetCapture(h);return 0;
    case WM_MOUSEMOVE:set_mouse(h,l);return 0;
    case WM_LBUTTONUP:set_mouse(h,l);pressed=false;ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:pressed=false;return 0;
    case WM_KEYDOWN:
        if(w==VK_ESCAPE) desk_ui_close_detail();
        if(w==VK_RIGHT) desk_ui_page(desk_ui_current_page()+1);
        if(w==VK_LEFT) desk_ui_page(desk_ui_current_page()-1);
        if(w=='1'||w=='2'||w=='3')desk_ui_page((int)w-'1');
        return 0;
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
#ifdef GSO_TEST
static int dump(const char *path) {
    lv_refr_now(display);
    FILE *f=fopen(path,"wb"); if(!f) return 0;
    BITMAPFILEHEADER fh={0}; fh.bfType=0x4D42; fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);
    fh.bfSize=fh.bfOffBits+sizeof(framebuffer);
    fwrite(&fh,sizeof(fh),1,f);fwrite(&bitmap.bmiHeader,sizeof(BITMAPINFOHEADER),1,f);
    fwrite(framebuffer,sizeof(framebuffer),1,f);fclose(f);return 1;
}
/* Pixels that differ visibly; edge anti-aliasing of text on a snapshot differs by a few levels and is ignored. */
static size_t frames_differ(const uint32_t *a,const uint32_t *b){size_t n=0;for(size_t i=0;i<WIDTH*HEIGHT;i++)for(int shift=0;shift<24;shift+=8)if(abs((int)((a[i]>>shift)&255)-(int)((b[i]>>shift)&255))>48){n++;break;}return n;}
static void pump(int ms) {
    uint64_t until=GetTickCount64()+ms;
    while(GetTickCount64()<until) {lv_timer_handler();Sleep(5);}
}
static void click_at(int x,int y) { lv_obj_update_layout(lv_screen_active());mouse.x=x;mouse.y=y;pressed=true;pump(70);pressed=false;pump(70); }
/* True when some label starts with prefix and ends with suffix. */
static int label_matches(lv_obj_t *parent,const char *prefix,const char *suffix){
    if(lv_obj_check_type(parent,&lv_label_class)){const char *t=lv_label_get_text(parent);size_t n=strlen(t),p=strlen(prefix),s=strlen(suffix);if(n>=p+s&&!strncmp(t,prefix,p)&&!strcmp(t+n-s,suffix))return 1;}
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++)if(label_matches(lv_obj_get_child(parent,i),prefix,suffix))return 1;
    return 0;
}
static int has_label(lv_obj_t *parent,const char *value);
static int vpn_reset_shown(lv_obj_t *panel){return has_label(panel,"刷新")&&label_matches(panel,"","-24");}
static int has_label(lv_obj_t *parent,const char *value){
    if(lv_obj_check_type(parent,&lv_label_class)&&strcmp(lv_label_get_text(parent),value)==0)return 1;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++)if(has_label(lv_obj_get_child(parent,i),value))return 1;
    return 0;
}
static lv_obj_t *find_label_any(lv_obj_t *parent,const char *value){
    if(lv_obj_check_type(parent,&lv_label_class)&&!strcmp(lv_label_get_text(parent),value))return parent;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++){lv_obj_t *found=find_label_any(lv_obj_get_child(parent,i),value);if(found)return found;}
    return NULL;
}
static bool label_has_color(const char *value,uint32_t palette_color){
    lv_obj_t *label=find_label_any(lv_screen_active(),value);
    return label&&(lv_color_to_u32(lv_obj_get_style_text_color(label,0))&0xFFFFFF)==desk_ui_theme_color(palette_color);
}
static lv_obj_t *find_label_object(lv_obj_t *parent,const char *value){if(lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN))return NULL;if(lv_obj_check_type(parent,&lv_label_class)&&!strcmp(lv_label_get_text(parent),value)&&lv_obj_get_event_count(lv_obj_get_parent(parent))>0)return parent;for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++){lv_obj_t *found=find_label_object(lv_obj_get_child(parent,i),value);if(found)return found;}return NULL;}
static bool click_label_button(const char *value){lv_obj_t *label=find_label_object(lv_screen_active(),value);if(!label)return false;lv_obj_t *button=lv_obj_get_parent(label);lv_obj_scroll_to_view_recursive(button,LV_ANIM_OFF);pump(80);lv_obj_update_layout(button);lv_area_t area;lv_obj_get_coords(button,&area);click_at((area.x1+area.x2)/2,(area.y1+area.y2)/2);return true;}
static lv_obj_t *find_type(lv_obj_t *parent,const lv_obj_class_t *type){
    if(lv_obj_check_type(parent,type))return parent;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++){lv_obj_t *found=find_type(lv_obj_get_child(parent,i),type);if(found)return found;}return NULL;
}
static lv_obj_t *find_visible_type(lv_obj_t *parent,const lv_obj_class_t *type){
    if(lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN))return NULL;
    if(lv_obj_check_type(parent,type))return parent;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++){lv_obj_t *found=find_visible_type(lv_obj_get_child(parent,i),type);if(found)return found;}return NULL;
}
static unsigned collect_segments(lv_obj_t *parent,lv_obj_t **out,unsigned capacity){
 unsigned n=0;if(lv_obj_check_type(parent,&lv_button_class)){if(capacity)out[0]=parent;return 1;}
 for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++){unsigned count=collect_segments(lv_obj_get_child(parent,i),out+(n<capacity?n:capacity),n<capacity?capacity-n:0);n+=count;}return n;
}
static int selftest(void) {
    FILE *f=fopen("selftest.txt","w");if(!f)return 2;
    fprintf(f,"Version: %s\n",GSO_VERSION);
    int pass=1;desk_ui_pause();desk_ui_theme_mode_set(0);pump(100); /* the colour checks below assume the night palette whatever the wall clock says */
    int unavailable=has_label(lv_screen_active(),"未运行")&&!has_label(lv_screen_active(),"Codex")&&!has_label(lv_screen_active(),"Claude")&&has_label(lv_screen_active(),"天气未接入")&&!has_label(lv_screen_active(),"就绪")&&!has_label(lv_screen_active(),"127.21");
    fprintf(f,"Live default has no demo or historical fallback: %s\n",unavailable?"PASS":"FAIL");pass&=unavailable;
    gso_snapshot input;gso_snapshot_empty(&input);input.cpu.load=gso_metric_number(0);input.cpu.clock=gso_metric_number(4300);input.cpu.memory_rate=gso_metric_number(3600);input.cpu.memory_clock=gso_metric_number(1800);input.cpu.memory_clock_configured=true;input.gpu.clock=gso_metric_number(2700);input.gpu.used=gso_metric_number(1.2*1073741824.0);input.gpu.total=gso_metric_number(12.0*1073741824.0);input.gpu.memory_clock=gso_metric_number(10501);input.cpu.used=gso_metric_number(8589934592.0);input.cpu.total=gso_metric_number(17179869184.0);strcpy(input.cpu.model,"TEST CPU");input.windows=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};
    desk_ui_apply_snapshot(&input);int summary=has_label(lv_screen_active(),"0")&&has_label(lv_screen_active(),"8.0/16.0 GB  3600 MT/s");dump("preview-live-home.bmp");
    click_at(320,165);pump(50);int detail=desk_ui_detail_is_open()&&has_label(lv_screen_active(),"TEST CPU")&&has_label(lv_screen_active(),"8.0/16.0 GB")&&has_label(lv_screen_active(),"3600 MT/s")&&has_label(lv_screen_active(),"-");dump("preview-live-detail.bmp");
    fprintf(f,"Shared snapshot drives summary and detail; real zero survives: %s\n",summary&&detail?"PASS":"FAIL");pass&=summary&&detail;
    desk_ui_close_detail();
    input.home_count=2;input.home=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};
    strcpy(input.home_entities[0].id,"light.test");strcpy(input.home_entities[0].name,"Test light");strcpy(input.home_entities[0].state,"on");input.home_entities[0].source=input.home;
    strcpy(input.home_entities[1].id,"climate.test");strcpy(input.home_entities[1].name,"Test climate");strcpy(input.home_entities[1].state,"cool");input.home_entities[1].source=input.home;input.home_entities[1].target_temperature=gso_metric_number(25);
    desk_ui_apply_snapshot(&input);desk_ui_page(1);pump(50);int home_summary=has_label(lv_screen_active(),"Test light")&&has_label(lv_screen_active(),"设定 25.0°C");click_label_button("Test light");pump(250);lv_obj_t *home_detail=lv_obj_get_child(lv_screen_active(),-1);int home_same=desk_ui_detail_is_open()&&has_label(home_detail,"Test light")&&has_label(home_detail,"设定 25.0°C");fprintf(f,"HOME summary opens shared detail; missing room temperature remains absent: %s\n",home_summary&&home_same?"PASS":"FAIL");pass&=home_summary&&home_same;desk_ui_close_detail();desk_ui_page(0);
    input.home_count=9;const char *rich_ids[]={"climate.study","light.study","light.screen","fan.room","climate.living","cover.group","fan.air_purifier","humidifier.room","switch.washer"};const char *rich_names[]={"书房空调","书房灯","屏幕挂灯","风扇","客厅空调","客厅窗帘","空气净化器","抽湿机","洗衣机"};
    for(unsigned h=0;h<9;h++){gso_home_entity *e=&input.home_entities[h];memset(e,0,sizeof(*e));strcpy(e->id,rich_ids[h]);strcpy(e->name,rich_names[h]);strcpy(e->state,h%2?"on":"off");e->source=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};e->actions=GSO_HOME_ON|GSO_HOME_OFF;}
    for(unsigned h=0;h<5;h+=4){gso_home_entity *e=&input.home_entities[h];e->target_temperature=gso_metric_number(27);e->min_temperature=gso_metric_number(16);e->max_temperature=gso_metric_number(30);e->temperature_step=gso_metric_number(1);e->actions|=GSO_HOME_TEMPERATURE|GSO_HOME_HVAC;strcpy(e->modes[0],"cool");strcpy(e->modes[1],"heat");strcpy(e->modes[2],"off");e->mode_count=3;strcpy(e->mode,"off");}
    for(unsigned h=1;h<=2;h++){gso_home_entity *e=&input.home_entities[h];e->actions|=GSO_HOME_BRIGHTNESS|GSO_HOME_COLOR;e->brightness=gso_metric_number(h==1?95:80);e->kelvin=gso_metric_number(4600);e->min_kelvin=gso_metric_number(2700);e->max_kelvin=gso_metric_number(6500);}
    for(unsigned h=3;h<=6;h+=3){gso_home_entity *e=&input.home_entities[h];e->actions|=GSO_HOME_SPEED|GSO_HOME_PRESET;e->percentage=gso_metric_number(50);e->percentage_step=gso_metric_number(25);strcpy(e->mode,"自然风");strcpy(e->modes[0],"直吹风");strcpy(e->modes[1],"自然风");e->mode_count=2;}
    strcpy(input.home_entities[5].state,"closed");input.home_entities[5].actions=GSO_HOME_OPEN|GSO_HOME_CLOSE;
    gso_home_entity *humid=&input.home_entities[7];humid->actions|=GSO_HOME_MODE|GSO_HOME_HUMIDITY;humid->humidity=gso_metric_number(40);humid->min_humidity=gso_metric_number(30);humid->max_humidity=gso_metric_number(70);strcpy(humid->modes[0],"智能模式");strcpy(humid->modes[1],"sleep");strcpy(humid->mode,"智能模式");humid->mode_count=2;strcpy(input.home_entities[8].detail_state,"已关机");
    desk_ui_apply_snapshot(&input);desk_ui_page(1);pump(220);dump("preview-home-cards.bmp");desk_ui_detail(15);pump(250);dump("preview-home-rich-detail.bmp");lv_obj_t *home_screen=lv_screen_active();int mode_x=0,mode_y=0,card_min=0,card_max=0;int rich_home=has_label(home_screen,"书房灯")&&has_label(home_screen,"工作模式")&&has_label(home_screen,"设定温度  27.0°C")&&has_label(home_screen,"亮度  95%")&&has_label(home_screen,"色温  4600 K")&&!has_label(home_screen,"室温")&&!has_label(home_screen,"停止")&&find_type(home_screen,&lv_slider_class)&&desk_ui_home_mode_center(0,&mode_x,&mode_y)&&desk_ui_home_chip_count("关闭")==0&&desk_ui_home_chip_count(NULL)>0&&desk_ui_home_card_heights(&card_min,&card_max)&&card_min==card_max&&card_max>0;desk_ui_close_detail();pump(220);desk_ui_page(0);fprintf(f,"HOME state cards, circular power, sliders, actual modes and compact curtain actions: %s\n",rich_home?"PASS":"FAIL");pass&=rich_home;
    input.vpn_total=gso_metric_number(200.0*1073741824.0);input.vpn_remaining=gso_metric_number(127.21*1073741824.0);input.vpn=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.weather_temperature=gso_metric_number(25.5);strcpy(input.weather_location,"深圳市光明区");strcpy(input.weather_condition,"cloudy");input.weather=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.humidity=gso_metric_number(59);input.pressure=gso_metric_number(1016.9);input.wind=gso_metric_number(12.2);input.wind_bearing=gso_metric_number(112.5);for(int d=0;d<6;d++){input.forecast[d].date=1792108800+d*86400;strcpy(input.forecast[d].condition,"cloudy");input.forecast[d].high=gso_metric_number(31.9);input.forecast[d].low=gso_metric_number(25.1);}input.forecast_count=6;for(int h=0;h<12;h++){input.hourly_forecast[h].date=1792108800+h*3600;strcpy(input.hourly_forecast[h].condition,h%2?"sunny":"partlycloudy");input.hourly_forecast[h].high=gso_metric_number(30.3);}input.hourly_count=12;desk_ui_apply_snapshot(&input);
    for(int i=0;i<4;i++){input.services[i].source=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};strcpy(input.services[i].raw_state,i==2?"rule":i==3?"拦截已开启":"运行中");input.net_details[i]=input.services[i].source;}input.subscription_updated=(int64_t)time(NULL)-300;input.antiad_updated=(int64_t)time(NULL)-172800;input.antiad_count=gso_metric_number(102535);input.dhcp_leases=gso_metric_number(4);desk_ui_apply_snapshot(&input);
    desk_ui_detail(11);pump(240);lv_obj_t *vpn_test_panel=lv_obj_get_child(lv_screen_active(),-1);lv_obj_t *vpn_test_arc=find_type(vpn_test_panel,&lv_arc_class);int vpn_ok=vpn_test_arc&&lv_arc_get_value(vpn_test_arc)==64&&has_label(vpn_test_panel,"127.21")&&has_label(vpn_test_panel,"共200GB")&&has_label(vpn_test_panel,"GB 剩余")&&vpn_reset_shown(vpn_test_panel)&&!has_label(vpn_test_panel,"VPN")&&!has_label(vpn_test_panel,"GiB")&&has_label(vpn_test_panel,"63.6%")&&has_label(lv_screen_active(),"阴 25.5°C")&&has_label(lv_screen_active(),"深圳市光明区");dump("preview-vpn-200G.bmp");desk_ui_close_detail();pump(220);
    input.storage=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.volume_count=2;strcpy(input.volumes[0].label,"eMMC");input.volumes[0].total=gso_metric_number(4000787030016.0);input.volumes[0].free=gso_metric_number(1500301910016.0);strcpy(input.volumes[1].label,"HDD");input.volumes[1].total=gso_metric_number(2000398934016.0);input.volumes[1].free=gso_metric_number(812345678901.0);desk_ui_apply_snapshot(&input);
    desk_ui_detail(11);pump(230);lv_obj_t *net_panel=lv_obj_get_child(lv_screen_active(),-1);int net_ok=!has_label(net_panel,"WAN / MB/s")&&has_label(net_panel,"代理订阅更新时间")&&has_label(net_panel,"拦截规则更新时间")&&has_label(net_panel,"生效规则数量")&&has_label(net_panel,"已接入设备台数")&&has_label(net_panel,"102535 条")&&has_label(net_panel,"4 台")&&has_label(net_panel,"5分钟前")&&has_label(net_panel,"2天前")&&has_label(net_panel,"运行中")&&!has_label(net_panel,"拦截已开启")&&!has_label(net_panel,"eMMC")&&!has_label(net_panel,"HDD")&&has_label(net_panel,"CLASH")&&has_label(net_panel,"GSN")&&has_label(net_panel,"GSW")&&has_label(net_panel,"HA");
    const char *modes[]={"rule","global","direct"},*translated[]={"规则模式","全局模式","直连模式"};for(int mode=0;mode<3;mode++){strcpy(input.services[2].raw_state,modes[mode]);desk_ui_apply_snapshot(&input);net_ok=net_ok&&has_label(net_panel,translated[mode])&&!has_label(net_panel,modes[mode]);lv_obj_t *switches[3];net_ok=net_ok&&collect_segments(net_panel,switches,3)==3;for(int j=0;j<3;j++)net_ok=net_ok&&(lv_obj_has_state(switches[j],LV_STATE_CHECKED)==(j==mode));if(mode>0){lv_obj_t *track=lv_obj_get_parent(switches[0]),*pill=lv_obj_get_child(track,0);int before=lv_obj_get_x(pill);pump(200);lv_obj_update_layout(track);net_ok=net_ok&&before!=lv_obj_get_x(pill)&&lv_obj_get_x(pill)==lv_obj_get_x(switches[mode]);}}strcpy(input.services[2].raw_state,"rule");input.dhcp_leases=gso_metric_number(0);desk_ui_apply_snapshot(&input);net_ok=net_ok&&has_label(net_panel,"0 台");input.dhcp_leases=gso_metric_number(4);desk_ui_apply_snapshot(&input);dump("preview-net-details.bmp");input.net_details[0].quality=GSO_OFFLINE;desk_ui_apply_snapshot(&input);net_ok=net_ok&&!has_label(net_panel,"5分钟前")&&has_label(net_panel,"来源离线");input.net_details[0].quality=GSO_OK;desk_ui_apply_snapshot(&input);desk_ui_close_detail();pump(210);fprintf(f,"NET HA metadata, zero leases, source quality and translated modes: %s\n",net_ok?"PASS":"FAIL");pass&=net_ok;
    int home_before=desk_ui_card_slot(3);
    input.nas_source=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.nas.reported=true;input.nas.cpu=gso_metric_number(22.5);input.nas.memory=gso_metric_number(72.3);input.nas.memory_total=gso_metric_number(2056081408.0);input.nas.soc=gso_metric_number(44.4);input.nas.gpu=gso_metric_number(43.8);input.nas.disk=gso_metric_missing("disk_standby");input.nas.disk_busy=gso_metric_number(12.4);input.nas.disk_read_rate=gso_metric_number(5242880.0);input.nas.disk_write_rate=gso_metric_number(0.0);input.nas.disk_wakes=gso_metric_number(3.0);input.nas.emmc_busy=gso_metric_number(3.4);input.nas.emmc_read_rate=gso_metric_number(1048576.0);input.nas.emmc_write_rate=gso_metric_number(2048.0);desk_ui_apply_snapshot(&input);pump(80);
    int nas_ok=desk_ui_card_slot(6)==desk_ui_card_slot(2)+1&&desk_ui_card_slot(3)==home_before+1&&has_label(lv_screen_active(),"eMMC")&&has_label(lv_screen_active(),"HDD")&&has_label(lv_screen_active(),"可用 1.36TB")&&has_label(lv_screen_active(),"共3.64TB")&&has_label(lv_screen_active(),"可用 757GB")&&has_label(lv_screen_active(),"44°C")&&has_label(lv_screen_active(),"休眠")&&has_label(lv_screen_active(),"22%")&&has_label(lv_screen_active(),"72%");
    dump("preview-nas-card.bmp");
    input.nas.disk=gso_metric_number(41.2);desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"41°C")&&!has_label(lv_screen_active(),"休眠");
    input.nas.disk=gso_metric_missing("disk_unavailable");desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"无法读取");input.nas.disk=gso_metric_missing("disk_idle");desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"空闲")&&!has_label(lv_screen_active(),"GPU 温度");
    /* With a reported disk state the state has its own label; the temperature then shows only a figure, "-" or an error. */
    input.nas.disk_state=GSO_DISK_ACTIVE;input.nas.disk=gso_metric_number(41.2);desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"已唤醒")&&has_label(lv_screen_active(),"今日3次")&&has_label(lv_screen_active(),"占用 12%  读 5.0 MB/s  写 0 KB/s")&&has_label(lv_screen_active(),"占用 3%  读 1.0 MB/s  写 2 KB/s")&&has_label(lv_screen_active(),"41°C");dump("preview-nas-card-active.bmp");
    input.nas.disk_state=GSO_DISK_STANDBY;input.nas.disk=gso_metric_missing("disk_standby");desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"已休眠")&&!has_label(lv_screen_active(),"已唤醒")&&has_label(lv_screen_active(),"今日3次")&&!has_label(lv_screen_active(),"41°C");
    input.nas.disk_state=GSO_DISK_QUIET;input.nas.disk=gso_metric_missing("disk_idle");desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&!has_label(lv_screen_active(),"已休眠")&&!has_label(lv_screen_active(),"已唤醒")&&!has_label(lv_screen_active(),"今日3次");
    input.nas.disk_state=GSO_DISK_IDLE;desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&has_label(lv_screen_active(),"已唤醒");
    input.nas.disk_state=GSO_DISK_UNKNOWN;input.nas.disk=gso_metric_missing("disk_unavailable");desk_ui_apply_snapshot(&input);
    desk_ui_detail(21);pump(240);lv_obj_t *nas_panel=lv_obj_get_child(lv_screen_active(),-1);nas_ok=nas_ok&&desk_ui_detail_is_open()&&has_label(nas_panel,"NAS · 主机状态")&&has_label(nas_panel,"RK3566 4-Core@1.8GHz")&&has_label(nas_panel,"2GB DDR4")&&has_label(nas_panel,"HDD 状态")&&has_label(nas_panel,"占用 12%")&&has_label(nas_panel,"读 5.0 MB/s")&&has_label(nas_panel,"写 0 KB/s")&&has_label(nas_panel,"占用 3%")&&has_label(nas_panel,"读 1.0 MB/s")&&has_label(nas_panel,"写 2 KB/s")&&has_label(nas_panel,"HDD 温度")&&!has_label(nas_panel,"硬盘 温度")&&!has_label(nas_panel,"GPU 温度")&&has_label(nas_panel,"eMMC")&&has_label(nas_panel,"HDD")&&has_label(nas_panel,"62%")&&has_label(nas_panel,"59%")&&has_label(nas_panel,"可用 1.36TB");dump("preview-nas-details.bmp");
    input.storage.quality=GSO_OFFLINE;desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&!has_label(nas_panel,"可用 1.36TB")&&has_label(nas_panel,"来源离线");input.storage.quality=GSO_OK;
    input.nas_source.quality=GSO_OFFLINE;desk_ui_apply_snapshot(&input);pump(40);nas_ok=nas_ok&&!has_label(nas_panel,"44°C")&&!has_label(nas_panel,"22%");input.nas_source.quality=GSO_OK;
    {gso_nas *hn=&input.nas;hn->disk_health=GSO_DISK_HEALTH_WATCH;hn->disk_hours=gso_metric_number(11234);hn->disk_reallocated=gso_metric_number(4);hn->disk_pending=gso_metric_number(0);hn->disk_crc=gso_metric_number(2);hn->disk_power_cycles=gso_metric_number(56);hn->disk_load_cycles=gso_metric_number(3000);
     desk_ui_apply_snapshot(&input);pump(40);int opened=click_label_button("硬盘健康");pump(250);lv_obj_t *hp=lv_obj_get_child(lv_screen_active(),-1);
     int health_ok=opened&&has_label(hp,"NAS · 硬盘健康")&&has_label(hp,"需要留意")&&has_label(hp,"1 年 3 个月")&&has_label(hp,"累计通电 11234 小时")&&has_label(hp,"已替换 4 个")&&has_label(hp,"已替换 4 个，待处理 0 个")&&has_label(hp,"2 次")&&has_label(hp,"56 次")&&has_label(hp,"磁头加载 3000 次，每次休眠后唤醒都会增加");
     dump("preview-nas-health-watch.bmp");
     hn->disk_health=GSO_DISK_HEALTH_BAD;hn->disk_pending=gso_metric_number(12);desk_ui_apply_snapshot(&input);pump(40);health_ok=health_ok&&has_label(hp,"有风险")&&has_label(hp,"待处理 12 个")&&!has_label(hp,"需要留意");dump("preview-nas-health-bad.bmp");
     hn->disk_health=GSO_DISK_HEALTH_GOOD;hn->disk_reallocated=gso_metric_number(0);hn->disk_pending=gso_metric_number(0);hn->disk_crc=gso_metric_number(0);desk_ui_apply_snapshot(&input);pump(40);health_ok=health_ok&&has_label(hp,"良好")&&has_label(hp,"没有发现坏扇区或其他自检异常，状态正常。")&&has_label(hp,"没有")&&!has_label(hp,"2 次");dump("preview-nas-health-good.bmp");
     hn->disk_health=GSO_DISK_HEALTH_UNKNOWN;hn->disk_hours=gso_metric_missing("source_unavailable");hn->disk_reallocated=hn->disk_pending=hn->disk_crc=hn->disk_power_cycles=hn->disk_load_cycles=hn->disk_hours;desk_ui_apply_snapshot(&input);pump(40);health_ok=health_ok&&has_label(hp,"还没有读到硬盘自检数据；硬盘休眠时不会特意唤醒它，等它工作时会自动读取。")&&!has_label(hp,"良好");
     input.nas_source.quality=GSO_OFFLINE;hn->disk_health=GSO_DISK_HEALTH_BAD;hn->disk_hours=gso_metric_number(100);desk_ui_apply_snapshot(&input);pump(40);health_ok=health_ok&&!has_label(hp,"有风险");input.nas_source.quality=GSO_OK;
     click_label_button("返回");pump(250);health_ok=health_ok&&desk_ui_detail_is_open()&&has_label(lv_screen_active(),"NAS · 主机状态");
     uint64_t saved_received=input.received_ms;desk_data_test_status(DESK_ONLINE);input.received_ms=desk_data_now_ms();hn->disk_health=GSO_DISK_HEALTH_WATCH;hn->disk_hours=gso_metric_number(11234);desk_ui_apply_snapshot(&input);pump(40);
     int alert_ok=has_label(lv_screen_active(),"硬盘需留意")&&!has_label(lv_screen_active(),"实时");
     hn->disk_health=GSO_DISK_HEALTH_BAD;input.received_ms=desk_data_now_ms();desk_ui_apply_snapshot(&input);pump(40);
     lv_obj_t *alert_label=find_label_any(lv_screen_active(),"硬盘有风险");alert_ok=alert_ok&&alert_label;
     if(alert_label){lv_area_t aa;lv_obj_get_coords(alert_label,&aa);click_at((aa.x1+aa.x2)/2,(aa.y1+aa.y2)/2);pump(250);alert_ok=alert_ok&&has_label(lv_screen_active(),"NAS · 硬盘健康");dump("preview-sidebar-disk-alert.bmp");click_label_button("返回");pump(250);alert_ok=alert_ok&&!desk_ui_detail_is_open();}
     hn->disk_health=GSO_DISK_HEALTH_GOOD;desk_ui_apply_snapshot(&input);pump(40);alert_ok=alert_ok&&!has_label(lv_screen_active(),"硬盘有风险")&&!has_label(lv_screen_active(),"硬盘需留意");
     hn->disk_health=GSO_DISK_HEALTH_BAD;desk_data_test_status(DESK_GSN_OFFLINE);desk_ui_apply_snapshot(&input);pump(40);alert_ok=alert_ok&&!has_label(lv_screen_active(),"硬盘有风险")&&has_label(lv_screen_active(),"GSN 离线");
     desk_data_test_status(DESK_ONLINE);hn->disk_health=GSO_DISK_HEALTH_UNKNOWN;input.received_ms=saved_received;desk_ui_apply_snapshot(&input);pump(40);
     health_ok=health_ok&&alert_ok;if(!alert_ok)fprintf(f,"  sidebar disk alert failed (label=%d)\n",alert_label!=NULL);
     nas_ok=nas_ok&&health_ok;if(!health_ok)fprintf(f,"  NAS health page failed (opened=%d)\n",opened);}
    desk_ui_apply_snapshot(&input);desk_ui_close_detail();pump(210);
    input.nas.reported=false;input.nas_source.quality=GSO_UNCONFIGURED;desk_ui_apply_snapshot(&input);pump(80);nas_ok=nas_ok&&desk_ui_card_slot(3)==home_before;
    {gso_freshness windows=input.windows;int last=3;input.windows.quality=GSO_OFFLINE;desk_ui_apply_snapshot(&input);pump(80);nas_ok=nas_ok&&desk_ui_card_slot(0)==last&&desk_ui_card_slot(2)==0;input.windows=windows;desk_ui_apply_snapshot(&input);pump(80);nas_ok=nas_ok&&desk_ui_card_slot(0)==0;}
    fprintf(f,"NAS card, volumes moved from NET, standby disk, detail and disk health page: %s\n",nas_ok?"PASS":"FAIL");pass&=nas_ok;
    fprintf(f,"VPN 200G ratio, monthly reset and trusted HA weather: %s\n",vpn_ok?"PASS":"FAIL");pass&=vpn_ok;
    click_at(100,160);pump(240);int weather_ok=desk_ui_detail_is_open()&&has_label(lv_screen_active(),"59%")&&has_label(lv_screen_active(),"1016.9 hPa")&&has_label(lv_screen_active(),"12.2 km/h · 东南偏东")&&has_label(lv_screen_active(),"高 31.9°C");dump("preview-weather-detail.bmp");click_at(470,238);pump(120);int hourly_ok=has_label(lv_screen_active(),"30.3°C")&&!has_label(lv_screen_active(),"高 31.9°C");dump("preview-weather-hourly.bmp");lv_obj_t *hours_scroller=lv_obj_get_child(lv_obj_get_child(lv_screen_active(),-1),-1);mouse.x=1120;mouse.y=342;pressed=true;pump(50);for(int step=1;step<=10;step++){mouse.x=1120-step*45;pump(20);}pressed=false;pump(180);hourly_ok=hourly_ok&&lv_obj_get_scroll_x(hours_scroller)>100;weather_ok=weather_ok&&hourly_ok;desk_ui_close_detail();pump(200);desk_ui_detail(10);pump(240);int clocks_ok=has_label(lv_screen_active(),"4300 MHz")&&has_label(lv_screen_active(),"8.0/16.0 GB")&&has_label(lv_screen_active(),"3600 MT/s")&&has_label(lv_screen_active(),"2700 MHz")&&has_label(lv_screen_active(),"1.2/12.0 GB")&&has_label(lv_screen_active(),"10501 MHz");dump("preview-clocks.bmp");desk_ui_close_detail();pump(200);fprintf(f,"Weather details, six forecasts and hardware clocks: %s\n",weather_ok&&clocks_ok?"PASS":"FAIL");pass&=weather_ok&&clocks_ok;
    pump(260);unsigned base_children=lv_obj_get_child_count(lv_screen_active());
    desk_ui_detail(10);pump(40);lv_obj_t *motion_panel=lv_obj_get_child(lv_screen_active(),-1);int ordinary_motion=lv_obj_get_style_translate_x(motion_panel,0)==0&&lv_obj_get_style_translate_x(lv_obj_get_child(motion_panel,0),0)>0;pump(220);ordinary_motion=ordinary_motion&&lv_obj_get_style_translate_x(motion_panel,0)==0;
    desk_ui_detail(16);pump(40);motion_panel=lv_obj_get_child(lv_screen_active(),-1);int game_motion=lv_obj_get_style_translate_y(motion_panel,0)==0&&lv_obj_get_style_translate_y(lv_obj_get_child(motion_panel,0),0)>0&&desk_ui_animation_cache_bytes()>0;pump(80);dump("preview-game-boot.bmp");pump(460);game_motion=game_motion&&lv_obj_get_style_translate_y(motion_panel,0)==0&&!has_label(motion_panel,"GAME MODE");dump("preview-game-clocks.bmp");lv_obj_t *latency_box=lv_obj_get_child(motion_panel,4),*latency_value=lv_obj_get_child(latency_box,1);int latency_default=has_label(latency_box,"渲染延迟")&&!strcmp(lv_label_get_text(latency_value),"-")&&lv_obj_get_style_text_font(latency_value,0)==&lv_font_montserrat_40;fprintf(f,"Default render latency uses supported ASCII glyph: %s\n",latency_default?"PASS":"FAIL");pass&=latency_default;
    desk_ui_close_detail();int returning=!desk_ui_detail_is_open()&&lv_obj_get_child_count(lv_screen_active())==base_children;pump(220);returning=returning&&lv_obj_get_child_count(lv_screen_active())==base_children&&desk_ui_animation_cache_bytes()==0&&desk_ui_animation_cache_peak_bytes()<=2*1024*1024;
    fprintf(f,"Distinct game entry and deferred return cleanup: %s\n",ordinary_motion&&game_motion&&returning?"PASS":"FAIL");pass&=ordinary_motion&&game_motion&&returning;
    strcpy(input.game_process,"cs2.exe");input.game=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.latency=gso_metric_number(0.25);input.fps=gso_metric_number(144);input.game_icon_valid=true;for(int pixel=0;pixel<576;pixel++){input.game_icon[pixel*4]=230;input.game_icon[pixel*4+1]=(uint8_t)(pixel%24*10);input.game_icon[pixel*4+2]=30;input.game_icon[pixel*4+3]=255;}desk_ui_apply_snapshot(&input);desk_ui_game_manual_open();pump(600);
    int game_info=has_label(lv_screen_active(),"cs2")&&!has_label(lv_screen_active(),"cs2.exe")&&has_label(lv_screen_active(),"0.25 ms")&&find_type(lv_obj_get_child(lv_screen_active(),-1),&lv_image_class);dump("preview-game-icon-latency.bmp");
    desk_ui_game_source(true);desk_ui_game_source(true);int source_exit=desk_ui_game_mode_active();desk_ui_game_source(false);desk_ui_game_source(true);desk_ui_game_source(true);source_exit=source_exit&&desk_ui_game_mode_active();desk_ui_game_source(true);source_exit=source_exit&&!desk_ui_game_mode_active()&&!desk_ui_detail_is_open();desk_ui_game_source(false);pump(220);
    fprintf(f,"Game icon, extension-free name, sub-ms latency and three consecutive offline polls: %s\n",game_info&&source_exit?"PASS":"FAIL");pass&=game_info&&source_exit;
    int lifecycle=1;const int ids[]={10,5,11,15,16};for(int i=0;i<60;i++){desk_ui_detail(ids[i%5]);if(!desk_ui_detail_is_open())lifecycle=0;desk_ui_close_detail();if(desk_ui_detail_is_open())lifecycle=0;}
    fprintf(f,"Live detail lifecycle (60 cycles): %s\n",lifecycle?"PASS":"FAIL");pass&=lifecycle;
    desk_data_test_status(DESK_ONLINE);input.windows.age_at_receive_ms=11001;desk_ui_apply_snapshot(&input);int offline=has_label(lv_screen_active(),"GSW 离线");
    const desk_connection_status statuses[]={DESK_NETWORK_OFFLINE,DESK_GSN_OFFLINE,DESK_GSN_AUTH,DESK_GSN_RESPONSE};const char *labels[]={"网络离线","GSN 离线","GSN 认证失败","GSN 响应异常"};for(int i=0;i<4;i++){desk_data_test_status(statuses[i]);desk_ui_apply_snapshot(&input);offline=offline&&has_label(lv_screen_active(),labels[i]);}desk_data_test_status(DESK_ONLINE);fprintf(f,"Expired snapshot visibly offline: %s\n",offline?"PASS":"FAIL");pass&=offline;
    desk_ui_close_detail();pump(250);input.ambient=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};input.illuminance=gso_metric_number(31);desk_ui_apply_snapshot(&input);click_at(100,400);pump(250);
    lv_obj_t *settings_root=lv_screen_active();lv_obj_t *auto_label=find_label_object(settings_root,"自动亮度");lv_obj_t *auto_button=auto_label?lv_obj_get_parent(auto_label):NULL;
    int brightness_ok=has_label(settings_root,"屏幕亮度")&&has_label(settings_root,"本机状态")&&has_label(settings_root,"水平仪")&&!has_label(settings_root,"管理 Wi-Fi")&&!has_label(settings_root,"手动亮度")&&has_label(settings_root,"书房 31 lx")&&label_matches(settings_root,"GSO STATUS ","UTC+8")&&desk_brightness_state()->automatic&&auto_button&&(lv_color_to_u32(lv_obj_get_style_bg_color(auto_button,0))&0xFFFFFF)==0xA5E5C4;
    /* Dragging the slider takes over: manual mode, the auto button turns grey and the light reading disappears. */
    lv_obj_t *brightness_control=find_type(lv_obj_get_child(lv_screen_active(),-1),&lv_slider_class);if(brightness_control){lv_slider_set_value(brightness_control,42,LV_ANIM_OFF);lv_obj_send_event(brightness_control,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_send_event(brightness_control,LV_EVENT_RELEASED,NULL);}pump(100);
    brightness_ok=brightness_ok&&desk_ui_brightness_percent()==42&&has_label(lv_screen_active(),"42%")&&!desk_brightness_state()->automatic&&!has_label(lv_screen_active(),"书房 31 lx")&&!label_matches(lv_screen_active(),"已改为","")&&(lv_color_to_u32(lv_obj_get_style_bg_color(auto_button,0))&0xFFFFFF)==0x2B3641;
    click_label_button("自动亮度");pump(150);brightness_ok=brightness_ok&&desk_brightness_state()->automatic&&(lv_color_to_u32(lv_obj_get_style_bg_color(auto_button,0))&0xFFFFFF)==0xA5E5C4;input.ambient.quality=GSO_OFFLINE;desk_ui_apply_snapshot(&input);pump(150);brightness_ok=brightness_ok&&has_label(lv_screen_active(),"书房 -- lx")&&desk_brightness_state()->target==42;dump("preview-brightness-fallback.bmp");
    click_label_button("黑夜");pump(80);brightness_ok=brightness_ok&&desk_ui_theme_mode()==0;click_label_button("白天");pump(80);brightness_ok=brightness_ok&&desk_ui_theme_mode()==1;click_label_button("日出日落");pump(80);brightness_ok=brightness_ok&&desk_ui_theme_mode()<0;desk_ui_theme_mode_set(0);pump(80);
    fprintf(f,"Settings: one auto-brightness button greyed by dragging, light reading only in auto, theme choices, ambient fallback: %s\n",brightness_ok?"PASS":"FAIL");pass&=brightness_ok;

    /* WIN temperatures: normal colour below 75 C, orange from 75 C, red from 95 C, normal again once it cools or is unknown. */
    {desk_ui_close_detail();desk_ui_page(0);pump(100);gso_snapshot hot;desk_data_latest(&hot);
     hot.cpu.temperature=gso_metric_number(62);desk_ui_apply_snapshot(&hot);pump(80);int heat_ok=label_has_color("62°C",0xA5E5C4);
     hot.cpu.temperature=gso_metric_number(74.4);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("74°C",0xA5E5C4);hot.cpu.temperature=gso_metric_number(74.6);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("75°C",0xF1BF77);
     hot.cpu.temperature=gso_metric_number(76);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("76°C",0xF1BF77);
     hot.cpu.temperature=gso_metric_number(90);hot.gpu.temperature=gso_metric_number(82);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("90°C",0xF1BF77)&&label_has_color("82°C",0xF1BF77);dump("preview-win-hot.bmp");
     hot.cpu.temperature=gso_metric_number(96);hot.gpu.temperature=gso_metric_number(55);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("96°C",0xF08080)&&label_has_color("55°C",0x8EBFF5);
     hot.cpu.temperature=gso_metric_number(60);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("60°C",0xA5E5C4);
     hot.cpu.temperature=gso_metric_missing("test");desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&!find_label_any(lv_screen_active(),"60°C");hot.cpu.temperature=gso_metric_number(65);desk_ui_apply_snapshot(&hot);pump(80);heat_ok=heat_ok&&label_has_color("65°C",0xA5E5C4);hot.cpu.temperature=gso_metric_number(88);
     hot.cpu.temperature=gso_metric_number(88);desk_ui_apply_snapshot(&hot);desk_ui_detail(10);pump(200);heat_ok=heat_ok&&label_has_color("88°C",0xF1BF77);dump("preview-win-detail-hot.bmp");
     desk_ui_close_detail();pump(100);
     fprintf(f,"WIN temperature colours: normal, orange from 75, red from 95, back to normal, main card and detail page: %s\n",heat_ok?"PASS":"FAIL");pass&=heat_ok;
    }
    {gso_snapshot disk;desk_data_latest(&disk);disk.disk_count=1;strcpy(disk.disks[0].id,"C:\\");strcpy(disk.disks[0].model,"Test SSD");
     disk.disks[0].total=gso_metric_number(1073741824.0*500);disk.disks[0].free=gso_metric_number(1073741824.0*200);
     disk.disks[0].busy=gso_metric_number(12.4);disk.disks[0].read_ms=gso_metric_number(4.26);disk.disks[0].write_ms=gso_metric_missing("disk_no_io");
     disk.disks[0].response_ms=gso_metric_number(4.26);disk.disks[0].read_rate=gso_metric_number(5.0*1048576.0);disk.disks[0].write_rate=gso_metric_number(0);
     desk_ui_apply_snapshot(&disk);desk_ui_detail(10);pump(200);int disk_ok=has_label(lv_screen_active(),"占用 12%")&&has_label(lv_screen_active(),"读速率")&&has_label(lv_screen_active(),"写速率")&&has_label(lv_screen_active(),"响应时间")&&has_label(lv_screen_active(),"5.0 MB/s")&&has_label(lv_screen_active(),"0 KB/s")&&has_label(lv_screen_active(),"4.3 ms");dump("preview-win-detail-disk.bmp");
     disk.disks[0].busy=gso_metric_number(100);disk.disks[0].response_ms=gso_metric_number(130.2);disk.disks[0].read_rate=gso_metric_number(612.0*1024.0);disk.disks[0].write_rate=gso_metric_number(250.0*1048576.0);desk_ui_apply_snapshot(&disk);pump(80);disk_ok=disk_ok&&has_label(lv_screen_active(),"占用 100%")&&has_label(lv_screen_active(),"612 KB/s")&&has_label(lv_screen_active(),"250 MB/s")&&has_label(lv_screen_active(),"130 ms");
     disk.disks[0].busy=gso_metric_missing("x");disk.disks[0].response_ms=gso_metric_missing("x");disk.disks[0].read_rate=gso_metric_missing("x");disk.disks[0].write_rate=gso_metric_missing("x");desk_ui_apply_snapshot(&disk);pump(80);disk_ok=disk_ok&&has_label(lv_screen_active(),"占用 -")&&!has_label(lv_screen_active(),"130 ms")&&!has_label(lv_screen_active(),"250 MB/s");
desk_ui_close_detail();pump(100);
     fprintf(f,"WIN detail disk busy percent, read/write rate and response time: %s\n",disk_ok?"PASS":"FAIL");pass&=disk_ok;
    }
    /* Board state on the settings page and the level page behind its button. */
    {gss_system_info board={.cpu_valid=true,.cpu_percent={37,12},.memory_valid=true,.internal_total=480u*1024u,.internal_free=200u*1024u,.psram_total=32u<<20,.psram_free=(uint32_t)(26.5*1048576),.storage_valid=true,.flash_total=16u<<20,.app_total=0xFE0000u,.app_used=(uint32_t)(3.4*1048576),.temperature_valid=true,.temperature_c=52.4f,.uptime_s=3*3600+41*60};
     desk_ui_close_detail();pump(250);gss_preview_system(&board);click_at(100,400);pump(250);lv_obj_t *r=lv_screen_active();
     int board_ok=has_label(r,"CPU 核心0")&&has_label(r,"CPU 核心1")&&has_label(r,"芯片温度")&&has_label(r,"内部 RAM")&&has_label(r,"PSRAM")&&has_label(r,"固件存储")&&has_label(r,"37%")&&has_label(r,"12%")&&has_label(r,"52°C")&&has_label(r,"280 / 480 KB")&&has_label(r,"5.5 / 32 MB")&&has_label(r,"3.4 / 15.9 MB")&&has_label(r,"已运行 3小时41分钟");
     dump("preview-settings-board.bmp");
     board.cpu_percent[0]=95;board.temperature_valid=false;board.storage_valid=false;gss_preview_system(&board);pump(1300);r=lv_screen_active();
     board_ok=board_ok&&has_label(r,"95%")&&!has_label(r,"52°C")&&!has_label(r,"3.4 / 15.9 MB");
     board.cpu_valid=false;board.memory_valid=false;gss_preview_system(&board);pump(1300);r=lv_screen_active();board_ok=board_ok&&!has_label(r,"95%")&&!has_label(r,"280 / 480 KB");
     gss_preview_system(NULL);
     /* level page: upright and level, tilted both ways, flat on a desk, moving, no sensor; the page returns to settings */
     const float degrees=3.14159265f/180;gso_accel upright={0,1,0.02f};
     gss_preview_imu(true,upright);int level_ok=click_label_button("水平仪");pump(200);r=lv_screen_active();
     level_ok=level_ok&&desk_ui_detail_is_open()&&has_label(r,"水平仪")&&has_label(r,"左右倾斜")&&has_label(r,"0.0°")&&has_label(r,"已摆平")&&has_label(r,"x +0.00   y +1.00   z +0.02 g");dump("preview-level-flat.bmp");
     gso_accel clockwise={-sinf(1.5f*degrees)*cosf(8*degrees),cosf(1.5f*degrees)*cosf(8*degrees),sinf(8*degrees)};gss_preview_imu(true,clockwise);desk_ui_detail(19);pump(200);r=lv_screen_active();
     level_ok=level_ok&&has_label(r,"1.5°")&&has_label(r,"右侧偏低")&&has_label(r,"后仰 8.0°");dump("preview-level-near.bmp");
     gso_accel counter={sinf(4*degrees)*cosf(5*degrees),cosf(4*degrees)*cosf(5*degrees),-sinf(5*degrees)};gss_preview_imu(true,counter);desk_ui_detail(19);pump(200);r=lv_screen_active();
     level_ok=level_ok&&has_label(r,"4.0°")&&has_label(r,"左侧偏低")&&has_label(r,"前倾 5.0°");dump("preview-level-off.bmp");
     gss_preview_imu(true,(gso_accel){0.01f,0.02f,0.999f});desk_ui_detail(19);pump(200);r=lv_screen_active();level_ok=level_ok&&has_label(r,"屏幕接近水平放置，没有左右倾斜可测");
     gss_preview_imu(true,upright);desk_ui_detail(19);pump(200);gss_preview_imu(true,(gso_accel){0.3f,2.2f,0.1f});pump(200);r=lv_screen_active();level_ok=level_ok&&has_label(r,"请保持屏幕不动")&&has_label(r,"0.0°");
     gss_preview_imu(false,upright);desk_ui_detail(19);pump(200);r=lv_screen_active();level_ok=level_ok&&has_label(r,"未检测到加速度传感器")&&!has_label(r,"已摆平");dump("preview-level-nosensor.bmp");
     gss_preview_imu_sway();desk_ui_detail(19);pump(100);
     level_ok=level_ok&&click_label_button("返回");pump(250);level_ok=level_ok&&has_label(lv_screen_active(),"屏幕亮度");level_ok=level_ok&&click_label_button("水平仪");pump(150);level_ok=level_ok&&click_label_button("返回");pump(250);level_ok=level_ok&&click_label_button("返回");pump(250);level_ok=level_ok&&!desk_ui_detail_is_open();
     for(int n=0;n<20;n++){desk_ui_detail(19);pump(60);desk_ui_close_detail();}pump(60);level_ok=level_ok&&!desk_ui_detail_is_open();
     fprintf(f,"Settings board state (cores, temperature, memory, firmware size, unavailable values, 1 s refresh): %s\n",board_ok?"PASS":"FAIL");pass&=board_ok;
     fprintf(f,"Level page: level, near, off, backward/forward lean, flat on desk, moving, no sensor, back to settings, open/close cycles: %s\n",level_ok?"PASS":"FAIL");pass&=level_ok;
     desk_ui_close_detail();pump(200);
    }
    /* Wi-Fi page opened from the sidebar icon: scan on open, select a listed network, connect, disconnect, forget, back to the cards. */
    click_at(184,40);pump(300);int wifi_ok=has_label(lv_screen_active(),"设置 · Wi-Fi");gss_wifi_status ws;gss_wifi_radio_status(&ws);wifi_ok=wifi_ok&&ws.count==2&&has_label(lv_screen_active(),"Open Preview")&&has_label(lv_screen_active(),"GSS Preview");
    wifi_ok=wifi_ok&&click_label_button("Open Preview");pump(100);lv_obj_t *field=find_type(lv_screen_active(),&lv_textarea_class);lv_obj_t *check=find_type(lv_screen_active(),&lv_checkbox_class);wifi_ok=wifi_ok&&field&&check&&!strcmp(lv_textarea_get_text(field),"Open Preview")&&lv_obj_has_state(check,LV_STATE_CHECKED);
    wifi_ok=wifi_ok&&click_label_button("连接");pump(600);gss_wifi_radio_status(&ws);wifi_ok=wifi_ok&&ws.connected&&has_label(lv_screen_active(),"已连接 · Open Preview");
    dump("preview-wifi.bmp");wifi_ok=wifi_ok&&click_label_button("断开");gss_wifi_radio_status(&ws);wifi_ok=wifi_ok&&!ws.connected;
    wifi_ok=wifi_ok&&click_label_button("忘记")&&!gss_wifi_ssid()[0];pump(100);
    wifi_ok=wifi_ok&&click_label_button("返回");pump(250);wifi_ok=wifi_ok&&!desk_ui_detail_is_open();
    /* The sidebar icon opens the Wi-Fi page directly and back goes to the main page. */
    click_at(184,40);pump(220);wifi_ok=wifi_ok&&has_label(lv_screen_active(),"设置 · Wi-Fi");click_label_button("返回");pump(250);wifi_ok=wifi_ok&&!desk_ui_detail_is_open();
    for(int n=0;n<5;n++){click_at(100,400);pump(180);click_at(184,40);pump(180);wifi_ok=wifi_ok&&has_label(lv_screen_active(),"设置 · Wi-Fi");desk_ui_close_detail();pump(180);}
    fprintf(f,"Shared Wi-Fi UI, preview scan/select/connect/disconnect/forget, back navigation and lifecycle: %s\n",wifi_ok?"PASS":"FAIL");pass&=wifi_ok;
    {gso_snapshot vibe=input;vibe.windows=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};vibe.codex=vibe.windows;vibe.clients[GSO_VIBE_CODEX]=(gso_client){true,false,gso_metric_missing("tokens_unavailable")};vibe.clients[GSO_VIBE_CLAUDE]=(gso_client){true,true,gso_metric_number(1250000)};time_t now=time(NULL);
     strcpy(vibe.task_state,"running");strcpy(vibe.task_title,"VIBE接入Claude");vibe.task_duration_ms=gso_metric_number(1800000);vibe.task_started_at=(int64_t)now-1800;vibe.task_provider=vibe.vibe_provider=GSO_VIBE_CLAUDE;
     vibe.quota_remaining[GSO_VIBE_CODEX][0]=gso_metric_number(67);vibe.quota_remaining[GSO_VIBE_CODEX][1]=gso_metric_number(95);vibe.quota_resets[GSO_VIBE_CODEX][0]=(int64_t)now+7200;vibe.quota_resets[GSO_VIBE_CODEX][1]=(int64_t)now+4*86400;
     vibe.quota_remaining[GSO_VIBE_CLAUDE][0]=gso_metric_number(76.4);vibe.quota_resets[GSO_VIBE_CLAUDE][0]=(int64_t)now+9000;vibe.quota_remaining[GSO_VIBE_CLAUDE][1]=gso_metric_missing("source_unavailable");vibe.quota_resets[GSO_VIBE_CLAUDE][1]=(int64_t)now+5*86400;
     desk_ui_page(0);desk_ui_apply_snapshot(&vibe);pump(120);lv_obj_t *s0=lv_screen_active();
     int vibe_card=has_label(s0,"运行中")&&has_label(s0,"VIBE接入Claude")&&has_label(s0,"Claude")&&has_label(s0,"76%")&&has_label(s0,"-")&&has_label(s0,"已进行 30分钟");dump("preview-vibe-claude-card.bmp");
     {const double spans[]={45000,59000,3723000,7200000,86400000};const char *texts[]={"已进行 45秒","已进行 59秒","已进行 1小时2分钟","已进行 2小时","已进行 24小时"};for(int k=0;k<5;k++){vibe.task_duration_ms=gso_metric_number(spans[k]);desk_ui_apply_snapshot(&vibe);pump(40);vibe_card=vibe_card&&has_label(lv_screen_active(),texts[k]);}vibe.task_duration_ms=gso_metric_number(1800000);desk_ui_apply_snapshot(&vibe);pump(40);}
     desk_ui_detail(5);pump(240);lv_obj_t *vp=lv_obj_get_child(lv_screen_active(),-1);
     int vibe_detail=has_label(vp,"Codex · 5 小时剩余")&&has_label(vp,"Codex · 每周剩余")&&has_label(vp,"Claude · 5 小时剩余")&&has_label(vp,"Claude · 每周剩余")&&has_label(vp,"67%")&&has_label(vp,"95%")&&has_label(vp,"76%")&&has_label(vp,"VIBE接入Claude")&&has_label(vp,"Claude")&&has_label(vp,"在线")&&has_label(vp,"离线")&&has_label(vp,"今日消耗 1.25M tokens")&&has_label(vp,"今日消耗 统计中");dump("preview-vibe-claude-detail.bmp");desk_ui_close_detail();pump(220);
     strcpy(vibe.task_state,"idle");vibe.task_title[0]=0;vibe.task_duration_ms=gso_metric_missing("not_running");for(int w=0;w<2;w++)vibe.quota_remaining[GSO_VIBE_CODEX][w]=gso_metric_missing("source_unavailable");desk_ui_apply_snapshot(&vibe);pump(80);s0=lv_screen_active();
     int vibe_idle=has_label(s0,"就绪")&&has_label(s0,"Claude")&&has_label(s0,"76%")&&!has_label(s0,"VIBE接入Claude");dump("preview-vibe-claude-idle.bmp");
     vibe.vibe_provider=GSO_VIBE_CODEX;vibe.quota_remaining[GSO_VIBE_CODEX][0]=gso_metric_number(67);desk_ui_apply_snapshot(&vibe);pump(80);int vibe_codex=has_label(lv_screen_active(),"Codex")&&has_label(lv_screen_active(),"67%");dump("preview-vibe-codex-card.bmp");
     fprintf(f,"VIBE Claude task/provider label, per-provider quota detail and idle focus: %s\n",vibe_card&&vibe_detail&&vibe_idle&&vibe_codex?"PASS":"FAIL");if(!(vibe_card&&vibe_detail&&vibe_idle&&vibe_codex))fprintf(f,"  card=%d detail=%d idle=%d codex=%d\n",vibe_card,vibe_detail,vibe_idle,vibe_codex);pass&=vibe_card&&vibe_detail&&vibe_idle&&vibe_codex;}
    {desk_ui_close_detail();pump(200);uint32_t normal=desk_ui_sidebar_background();desk_ui_detail(16);pump(80);uint32_t gaming=desk_ui_sidebar_background();dump("preview-game-sidebar.bmp");desk_ui_close_detail();pump(240);uint32_t back=desk_ui_sidebar_background();
     uint32_t night_root=desk_ui_root_background();desk_ui_theme_mode_set(1);pump(120);uint32_t day_normal=desk_ui_sidebar_background(),day_root=desk_ui_root_background();desk_ui_detail(16);pump(80);uint32_t day_game=desk_ui_sidebar_background(),day_game_root=desk_ui_root_background();dump("preview-game-day.bmp");desk_ui_close_detail();pump(240);uint32_t day_back=desk_ui_sidebar_background(),day_back_root=desk_ui_root_background();desk_ui_theme_mode_set(0);pump(120);
     int day_ok=day_normal==0xF2F6FA&&day_root==0xE9EFF5&&day_game==0x0B0C1D&&day_game_root==0x070814&&day_back==day_normal&&day_back_root==day_root&&night_root==0x101419;
     int sidebar_game=day_ok&&normal==0x131A21&&gaming==0x0B0C1D&&back==normal;fprintf(f,"Game mode recolours the sidebar and leaving restores it: %s\n",sidebar_game?"PASS":"FAIL");if(!sidebar_game)fprintf(f,"  normal=%06X game=%06X back=%06X\n",(unsigned)normal,(unsigned)gaming,(unsigned)back);pass&=sidebar_game;}
    {int m1=0,d1=0,m2=0,d2=0,m3=0,d3=0,m4=0,d4=0;
     /* 2026-10-17 12:00 UTC+8, 2026-10-24 (reset day), 2026-12-30 (year roll), 2026-10-23 23:59 */
     int a=desk_ui_vpn_reset_days(1792209600LL,&m1,&d1),b=desk_ui_vpn_reset_days(1792814400LL,&m2,&d2),c3=desk_ui_vpn_reset_days(1798603200LL,&m3,&d3),e=desk_ui_vpn_reset_days(1792814340LL-12*3600,&m4,&d4);
     int vpn_days=a==7&&m1==10&&d1==24&&b==31&&m2==11&&d2==24&&c3==25&&m3==1&&d3==24&&e==1&&m4==10&&d4==24;
     fprintf(f,"VPN reset countdown counts whole days to the next 24th, across month and year: %s\n",vpn_days?"PASS":"FAIL");if(!vpn_days)fprintf(f,"  days=%d,%d,%d,%d\n",a,b,c3,e);pass&=vpn_days;}
    {desk_ui_vibe_hold(300);gso_snapshot order=input;order.windows=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};order.codex=order.windows;strcpy(order.task_state,"idle");order.vibe_provider=GSO_VIBE_CLAUDE;order.quota_remaining[GSO_VIBE_CLAUDE][0]=gso_metric_number(70);desk_ui_page(0);desk_ui_apply_snapshot(&order);pump(80);
     int ready_order=desk_ui_card_slot(0)==0&&desk_ui_card_slot(1)==1&&desk_ui_card_slot(2)==2&&desk_ui_card_slot(3)==3&&has_label(lv_screen_active(),"就绪")&&has_label(lv_screen_active(),"Claude");
     strcpy(order.task_state,"unknown");order.vibe_provider=GSO_VIBE_NONE;for(int p=0;p<GSO_VIBE_PROVIDERS;p++)for(int w=0;w<2;w++)order.quota_remaining[p][w]=gso_metric_missing("not_running");desk_ui_apply_snapshot(&order);pump(80);int held=desk_ui_card_slot(1)==1&&has_label(lv_screen_active(),"就绪");pump(450);desk_ui_apply_snapshot(&order);pump(80);dump("preview-vibe-not-running.bmp");
     int idle_order=desk_ui_card_slot(0)==0&&desk_ui_card_slot(2)==1&&desk_ui_card_slot(3)==2&&desk_ui_card_slot(1)==3&&has_label(lv_screen_active(),"未运行")&&!has_label(lv_screen_active(),"状态未知")&&!has_label(lv_screen_active(),"Codex")&&!has_label(lv_screen_active(),"Claude");
     desk_ui_detail(5);pump(240);lv_obj_t *none_detail=lv_obj_get_child(lv_screen_active(),-1);int none_ok=has_label(none_detail,"未运行")&&has_label(none_detail,"Codex · 5 小时剩余")&&has_label(none_detail,"Claude · 5 小时剩余")&&!has_label(none_detail,"67%")&&!has_label(none_detail,"76%");order.clients[GSO_VIBE_CODEX].reported=order.clients[GSO_VIBE_CLAUDE].reported=false;desk_ui_apply_snapshot(&order);none_ok=none_ok&&has_label(none_detail,"未上报")&&!has_label(none_detail,"在线");desk_ui_close_detail();pump(240);
     strcpy(order.task_state,"running");order.vibe_provider=GSO_VIBE_CLAUDE;desk_ui_apply_snapshot(&order);pump(80);int back=desk_ui_card_slot(1)==1&&desk_ui_card_slot(2)==2&&desk_ui_card_slot(3)==3;
     fprintf(f,"VIBE not-running label, no default provider, and card moves to the rightmost slot: %s\n",ready_order&&held&&idle_order&&none_ok&&back?"PASS":"FAIL");if(!(ready_order&&held&&idle_order&&none_ok&&back))fprintf(f,"  ready=%d held=%d notrunning=%d detail=%d back=%d\n",ready_order,held,idle_order,none_ok,back);pass&=ready_order&&held&&idle_order&&none_ok&&back;}
    {gso_snapshot p=input;uint64_t stamp=desk_data_now_ms();p.windows=(gso_freshness){GSO_OK,true,stamp,0};p.codex=p.windows;strcpy(p.task_state,"running");p.vibe_provider=GSO_VIBE_CLAUDE;p.quota_remaining[GSO_VIBE_CLAUDE][0]=gso_metric_number(70);
     desk_ui_vibe_hold(20000);desk_ui_page(0);desk_ui_apply_snapshot(&p);pump(80);
     desk_ui_page(2);int no_printer=desk_ui_current_page()==1&&!find_label_object(lv_screen_active(),"PRINT");desk_ui_page(0);
     p.print.reported=true;p.print.state=GSO_PRINT_IDLE;p.printer=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};
     p.print.progress=gso_metric_number(0);p.print.nozzle=gso_metric_number(31);p.print.bed=gso_metric_number(29.5);
     desk_ui_apply_snapshot(&p);pump(80);
     int idle_last=desk_ui_card_slot(0)==0&&desk_ui_card_slot(1)==1&&desk_ui_card_slot(2)==2&&desk_ui_card_slot(3)==3&&desk_ui_card_slot(4)==4&&has_label(lv_screen_active(),"待机");
     desk_ui_page(2);int third_page=desk_ui_current_page()==2;desk_ui_page(0);
     p.print.state=GSO_PRINT_RUNNING;strcpy(p.print.job,"Benchy");p.print.progress=gso_metric_number(42);p.print.remaining_minutes=gso_metric_number(75);p.print.layer=gso_metric_number(10);p.print.layer_total=gso_metric_number(100);
     p.print.nozzle=gso_metric_number(219.6);p.print.nozzle_target=gso_metric_number(220);p.print.bed=gso_metric_number(60);p.print.bed_target=gso_metric_number(60);p.print.error_code=gso_metric_number(0);
     desk_ui_apply_snapshot(&p);pump(80);lv_obj_t *s0=lv_screen_active();
     int behind_vibe=desk_ui_card_slot(0)==0&&desk_ui_card_slot(1)==1&&desk_ui_card_slot(4)==2&&desk_ui_card_slot(2)==3&&desk_ui_card_slot(3)==4;
     int card_text=has_label(s0,"打印中")&&has_label(s0,"Benchy")&&has_label(s0,"42%")&&has_label(s0,"层 10 / 100")&&has_label(s0,"剩余 1小时15分钟")&&!has_label(s0,"220 / 220°C")&&!has_label(s0,"60 / 60°C");
     dump("preview-print-card.bmp");
     static uint8_t picture[GSO_THUMBNAIL_BYTES];for(unsigned i=0;i<GSO_THUMBNAIL_BYTES;i+=4){picture[i]=0xC8;picture[i+1]=0x78;picture[i+2]=0x28;picture[i+3]=0xFF;}
     strcpy(p.print.thumbnail,"0123456789abcdef");desk_ui_apply_snapshot(&p);pump(80);int thumb_waiting=!desk_ui_print_thumbnail_visible(false);
     desk_data_test_thumbnail("0123456789abcdef",picture);desk_ui_apply_snapshot(&p);pump(80);int thumb_shown=desk_ui_print_thumbnail_visible(false);dump("preview-print-thumbnail.bmp");
     strcpy(p.print.thumbnail,"fedcba9876543210");desk_ui_apply_snapshot(&p);pump(80);int thumb_new_job=!desk_ui_print_thumbnail_visible(false);
     strcpy(p.print.thumbnail,"0123456789abcdef");desk_ui_apply_snapshot(&p);pump(80);
     desk_ui_vibe_hold(0);strcpy(p.task_state,"unknown");p.vibe_provider=GSO_VIBE_NONE;for(int q=0;q<GSO_VIBE_PROVIDERS;q++)for(int w=0;w<2;w++)p.quota_remaining[q][w]=gso_metric_missing("not_running");
     desk_ui_apply_snapshot(&p);pump(80);
     int vibe_idle_print_first=desk_ui_card_slot(1)==4&&desk_ui_card_slot(4)==1&&desk_ui_card_slot(2)==2&&desk_ui_card_slot(3)==3;
     desk_ui_detail(18);pump(240);lv_obj_t *detail=lv_obj_get_child(lv_screen_active(),-1);
     int detail_ok=has_label(detail,"打印中")&&has_label(detail,"Benchy")&&has_label(detail,"42%")&&has_label(detail,"1:15")&&has_label(detail,"10 / 100")&&has_label(detail,"无")&&has_label(detail,"220 / 220°C")&&has_label(detail,"60 / 60°C")&&desk_ui_print_thumbnail_visible(true);dump("preview-print-detail.bmp");desk_ui_close_detail();pump(200);
     p.print.state=GSO_PRINT_FINISHED;desk_ui_apply_snapshot(&p);pump(80);
     int finished_time=0;{char want[2][40];for(int k=0;k<2;k++){time_t t=time(NULL)+8*3600+(k?60:0);struct tm *g=gmtime(&t);snprintf(want[k],sizeof(want[k]),"%02d:%02d 收到",g->tm_hour,g->tm_min);}
      finished_time=has_label(lv_screen_active(),"已完成")&&(has_label(lv_screen_active(),want[0])||has_label(lv_screen_active(),want[1]));dump("preview-print-finished.bmp");
      p.print.progress=gso_metric_number(100);desk_ui_apply_snapshot(&p);pump(80);finished_time=finished_time&&(has_label(lv_screen_active(),want[0])||has_label(lv_screen_active(),want[1]));}
     p.print.state=GSO_PRINT_FAILED;p.print.error_code=gso_metric_number(83886080.0);desk_ui_apply_snapshot(&p);pump(80);
     int failed=has_label(lv_screen_active(),"打印失败")&&has_label(lv_screen_active(),"错误代码 05000000");
     p.printer=(gso_freshness){GSO_OFFLINE,true,desk_data_now_ms(),120000};desk_ui_apply_snapshot(&p);pump(80);
     int thumb_offline=0;int offline=has_label(lv_screen_active(),"未连接")&&!has_label(lv_screen_active(),"打印失败")&&!has_label(lv_screen_active(),"Benchy")&&desk_ui_card_slot(4)==4&&!desk_ui_print_thumbnail_visible(false);
     p.print.reported=false;p.printer=(gso_freshness){GSO_UNCONFIGURED,false,0,0};desk_ui_apply_snapshot(&p);pump(80);desk_ui_page(2);int removed=desk_ui_current_page()==1;desk_ui_page(0);
     desk_ui_vibe_hold(300);
     int ok=no_printer&&idle_last&&third_page&&behind_vibe&&card_text&&finished_time&&thumb_waiting&&thumb_shown&&thumb_new_job&&vibe_idle_print_first&&detail_ok&&failed&&offline&&removed;(void)thumb_offline;
     fprintf(f,"PRINT card: hidden without a printer, last when idle, behind VIBE while printing, before an idle VIBE, model picture on arrival and not for another job, no temperatures on the card, finished state with its received time, offline and failure text, detail page: %s\n",ok?"PASS":"FAIL");
     if(!ok)fprintf(f,"  none=%d idle_last=%d third=%d behind=%d text=%d finished=%d vibe_idle=%d detail=%d failed=%d offline=%d removed=%d thumb_waiting=%d thumb_shown=%d thumb_new_job=%d\n",no_printer,idle_last,third_page,behind_vibe,card_text,finished_time,vibe_idle_print_first,detail_ok,failed,offline,removed,thumb_waiting,thumb_shown,thumb_new_job);pass&=ok;}
    {gso_snapshot k=input;k.windows=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};k.codex=k.windows;k.print.reported=false;k.printer=(gso_freshness){GSO_UNCONFIGURED,false,0,0};k.car.reported=false;k.car_source=(gso_freshness){GSO_UNCONFIGURED,false,0,0};
     desk_ui_vibe_hold(20000);desk_ui_page(0);desk_ui_apply_snapshot(&k);pump(80);
     desk_ui_page(2);int no_car=desk_ui_current_page()==1&&!find_label_object(lv_screen_active(),"CAR");desk_ui_page(0);
     k.car_source=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};
     gso_car *c=&k.car;c->reported=true;c->vehicle=GSO_CAR_PARKED;c->charge=GSO_CAR_CHARGING;c->lock=GSO_CAR_LOCKED;c->actions=GSO_HOME_UNLOCK|GSO_HOME_PRESS;
     strcpy(c->name,"零跑C11");strcpy(c->lock_id,"lock.test_car");strcpy(c->refresh_id,"button.test_car");c->updated_at=(int64_t)time(NULL)-60;
     c->battery=gso_metric_number(76);c->range=gso_metric_number(403);c->remaining_minutes=gso_metric_number(95);c->cabin=gso_metric_number(28);c->odometer=gso_metric_number(20610);c->week=gso_metric_number(132);c->consumption=gso_metric_number(14.8);
     c->tyre[0]=gso_metric_number(2.33);c->tyre[1]=gso_metric_number(2.35);c->tyre[2]=gso_metric_number(2.31);c->tyre[3]=gso_metric_number(2.34);c->tyre_alarm=GSO_CAR_TYRE_OK;c->windows=GSO_CAR_CLOSED;c->trunk=GSO_CAR_OPEN;c->charging_power=gso_metric_number(6.8);
     desk_ui_apply_snapshot(&k);pump(80);lv_obj_t *s0=lv_screen_active();
     int after_home=desk_ui_card_slot(5)==desk_ui_card_slot(3)+1;
     int card_text=has_label(s0,"已锁车 · 停放")&&has_label(s0,"76%")&&has_label(s0,"403 km")&&has_label(s0,"充满还需 1小时35分钟")&&label_matches(s0,"更新 ","")&&has_label(s0,"解锁");
     desk_ui_page(2);pump(400);dump("preview-car-card.bmp");desk_ui_page(0);pump(200);
     desk_ui_detail(20);pump(240);lv_obj_t *detail=lv_obj_get_child(lv_screen_active(),-1);
     int detail_ok=has_label(detail,"总里程")&&has_label(detail,"20610 km")&&has_label(detail,"车内温度")&&has_label(detail,"28°C")&&has_label(detail,"百公里能耗 kWh")&&has_label(detail,"14.8")&&has_label(detail,"近七天里程")&&has_label(detail,"132 km")&&has_label(detail,"刷新车况")&&has_label(detail,"76%")&&has_label(detail,"403 km");
     int extras=has_label(detail,"胎压 bar")&&has_label(detail,"2.33")&&has_label(detail,"2.35")&&has_label(detail,"2.31")&&has_label(detail,"2.34")&&has_label(detail,"正常")&&has_label(detail,"已关闭")&&has_label(detail,"未关好")&&has_label(detail,"6.8 kW");
     dump("preview-car-detail.bmp");
     c->charge=GSO_CAR_UNPLUGGED;c->lock=GSO_CAR_UNLOCKED;c->vehicle=GSO_CAR_DRIVING;c->actions=GSO_HOME_LOCK;desk_ui_apply_snapshot(&k);pump(80);
     int uncharged=has_label(detail,"未充电")&&has_label(detail,"未锁车 · 行驶中")&&!has_label(detail,"刷新车况")&&has_label(detail,"未开启控制");
     c->tyre_alarm=GSO_CAR_TYRE_ALARM;c->windows=GSO_CAR_OPEN;c->tyre[1]=gso_metric_missing("not_reported");desk_ui_apply_snapshot(&k);pump(80);
     int alarm=has_label(detail,"胎压报警")&&!has_label(detail,"正常")&&!has_label(detail,"2.35")&&!has_label(detail,"6.8 kW")&&!has_label(detail,"已关闭");
     dump("preview-car-detail-alarm.bmp");
     desk_ui_close_detail();pump(240);
     c->charge=GSO_CAR_PLUGGED;c->remaining_minutes=gso_metric_missing("not_reported");c->lock=GSO_CAR_JAMMED;desk_ui_apply_snapshot(&k);pump(80);
     int plugged=has_label(lv_screen_active(),"已插枪 · 未充电")&&has_label(lv_screen_active(),"车锁异常 · 行驶中");
     k.car_source=(gso_freshness){GSO_OFFLINE,true,desk_data_now_ms(),150000};desk_ui_apply_snapshot(&k);pump(80);
     int offline=has_label(lv_screen_active(),"车辆离线")&&has_label(lv_screen_active(),"数据来源离线")&&!has_label(lv_screen_active(),"76%")&&!has_label(lv_screen_active(),"403 km");dump("preview-car-offline.bmp");
     k.car.reported=false;k.car_source=(gso_freshness){GSO_UNCONFIGURED,false,0,0};desk_ui_apply_snapshot(&k);pump(80);desk_ui_page(2);int removed=desk_ui_current_page()==1;desk_ui_page(0);
     desk_ui_vibe_hold(300);
     int ok=no_car&&after_home&&card_text&&detail_ok&&extras&&uncharged&&alarm&&plugged&&offline&&removed;
     fprintf(f,"CAR card: hidden without a vehicle, right behind HOME, status/range/charge time/update time and lock button, detail odometer/cabin/consumption/week/tyres/windows/trunk/charging power/refresh, tyre alarm, not-charging, offline and removal: %s\n",ok?"PASS":"FAIL");
     if(!ok)fprintf(f,"  none=%d after_home=%d card=%d detail=%d extras=%d uncharged=%d alarm=%d plugged=%d offline=%d removed=%d\n",no_car,after_home,card_text,detail_ok,extras,uncharged,alarm,plugged,offline,removed);pass&=ok;}
    {gso_snapshot k=input;k.windows=(gso_freshness){GSO_OK,true,desk_data_now_ms(),0};k.codex=k.windows;k.print.reported=false;k.printer=(gso_freshness){GSO_UNCONFIGURED,false,0,0};k.car.reported=false;k.car_source=(gso_freshness){GSO_UNCONFIGURED,false,0,0};
     desk_ui_vibe_hold(20000);desk_ui_page(0);desk_ui_apply_snapshot(&k);pump(400);lv_refr_now(display);
     static uint32_t live[WIDTH*HEIGHT];memcpy(live,framebuffer,sizeof(live));dump("strip-live.bmp");
     uint64_t started=GetTickCount64();desk_ui_test_strip_freeze(0);uint64_t freeze_ms=GetTickCount64()-started;
     unsigned frozen=desk_ui_strip_frozen_cards();lv_obj_t *win_title=find_label_any(lv_screen_active(),"WIN");int live_hidden=win_title&&!lv_obj_is_visible(win_title);
     pump(120);lv_refr_now(display);dump("strip-frozen.bmp");
     size_t differing=frames_differ(live,framebuffer),restored;
     desk_ui_test_strip_thaw();pump(120);lv_refr_now(display);
     restored=frames_differ(live,framebuffer);
     int back=has_label(lv_screen_active(),"WIN")&&has_label(lv_screen_active(),"NET")&&!desk_ui_strip_frozen_cards();
     size_t tolerance=WIDTH*HEIGHT/500; /* the sidebar clock may tick between the two frames */
     /* A real drag: the strip freezes while the finger moves, a short drag still turns exactly one page, and it thaws at the end. */
     desk_ui_page(0);pump(100);mouse.x=800;mouse.y=60;pressed=true;pump(60);
     for(int x=800;x>=440;x-=40){mouse.x=x;pump(30);}
     unsigned during_drag=desk_ui_strip_frozen_cards();
     pressed=false;for(int step=0;step<7;step++)pump(100);
     int page_after=desk_ui_current_page();unsigned frozen_after=desk_ui_strip_frozen_cards();
     int turned=page_after==1&&desk_ui_strip_scroll_x()==350&&!frozen_after&&has_label(lv_screen_active(),"HOME");
     /* A drag far longer than the strip never runs past the last page, and a tiny drag falls back to the first one. */
     desk_ui_page(0);pump(100);mouse.x=1000;pressed=true;pump(60);
     for(int x=1000;x>=100;x-=60){mouse.x=x;pump(30);}
     pressed=false;for(int step=0;step<7;step++)pump(100);
     int clamped=desk_ui_current_page()==1&&desk_ui_strip_scroll_x()==350&&!desk_ui_strip_frozen_cards();
     desk_ui_page(0);pump(100);mouse.x=800;pressed=true;pump(60);
     for(int x=800;x>=770;x-=10){mouse.x=x;pump(30);}
     pressed=false;for(int step=0;step<7;step++)pump(100);
     int fell_back=desk_ui_current_page()==0&&desk_ui_strip_scroll_x()==0&&!desk_ui_strip_frozen_cards();
     desk_ui_page(0);pump(100);
     turned=turned&&clamped&&fell_back;
     int ok=frozen>=3&&during_drag>=3&&turned&&live_hidden&&back&&differing<tolerance&&restored<tolerance&&desk_ui_animation_cache_bytes()==0;
     fprintf(f,"Card strip freeze: visible cards replaced by PSRAM snapshots (frame matches the live cards) and restored after the drag, snapshot of %u cards took %llu ms here, real drag froze %u, turned exactly one page, never past the last page and fell back on a tiny drag: %s\n",frozen,(unsigned long long)freeze_ms,during_drag,ok?"PASS":"FAIL");
     if(!ok)fprintf(f,"  page_after=%d frozen_after=%u drag=%u turned=%d frozen=%u hidden=%d back=%d differing=%zu restored=%zu cache=%u\n",page_after,frozen_after,during_drag,turned,frozen,live_hidden,back,differing,restored,(unsigned)desk_ui_animation_cache_bytes());pass&=ok;}
    fprintf(f,"Overall: %s\n",pass?"PASS":"FAIL");fclose(f);return pass?0:3;
}

/* --perf-probe: how much of the screen each kind of interaction repaints. Pixels are architecture independent, so they
   carry over to the P4 renderer; milliseconds are only this PC's software renderer and are for relative comparison. */
typedef struct {const char *name;unsigned frames,drawn;uint64_t pixels,max_pixels;double ms,max_ms;} perf_row;
static perf_row perf_rows[24];
static unsigned perf_count;
static perf_row *perf_open(const char *name){perf_row *r=&perf_rows[perf_count++];memset(r,0,sizeof(*r));r->name=name;return r;}
static void perf_frame(perf_row *r){
    LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);Sleep(35);flushed_pixels=0;flush_count=0;   /* one input-read period per frame */
    QueryPerformanceCounter(&a);lv_timer_handler();QueryPerformanceCounter(&b);
    double ms=(double)(b.QuadPart-a.QuadPart)*1000.0/(double)f.QuadPart;
    r->frames++;if(flush_count){r->drawn++;r->pixels+=flushed_pixels;if(flushed_pixels>r->max_pixels)r->max_pixels=flushed_pixels;r->ms+=ms;if(ms>r->max_ms)r->max_ms=ms;}
}
static unsigned count_objects(lv_obj_t *o){unsigned n=1;for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)n+=count_objects(lv_obj_get_child(o,i));return n;}
typedef struct {unsigned objects,filled,rounded,borders,outlines,shadows,gradients,layered,labels,arcs,images,bars;uint64_t fill_px,round_px,label_px,arc_px;} perf_stats;
static void perf_analyze(lv_obj_t *o,lv_area_t clip,perf_stats *t){
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
    lv_area_t box,area;lv_obj_get_coords(o,&box);if(!lv_area_intersect(&area,&box,&clip))return;
    uint64_t px=(uint64_t)lv_area_get_width(&area)*lv_area_get_height(&area);t->objects++;
    if(lv_obj_get_style_bg_opa(o,LV_PART_MAIN)>0){t->filled++;t->fill_px+=px;if(lv_obj_get_style_radius(o,LV_PART_MAIN)>0){t->rounded++;t->round_px+=px;}}
    if(lv_obj_get_style_border_width(o,LV_PART_MAIN)>0&&lv_obj_get_style_border_opa(o,LV_PART_MAIN)>0)t->borders++;
    if(lv_obj_get_style_outline_width(o,LV_PART_MAIN)>0)t->outlines++;
    if(lv_obj_get_style_shadow_width(o,LV_PART_MAIN)>0)t->shadows++;
    if(lv_obj_get_style_bg_grad_dir(o,LV_PART_MAIN)!=LV_GRAD_DIR_NONE)t->gradients++;
    if(lv_obj_get_style_opa_layered(o,LV_PART_MAIN)<255||lv_obj_get_style_transform_rotation(o,LV_PART_MAIN)||lv_obj_get_style_transform_scale_x(o,LV_PART_MAIN)!=256)t->layered++;
    if(lv_obj_check_type(o,&lv_label_class)){t->labels++;t->label_px+=px;}
    if(lv_obj_check_type(o,&lv_arc_class)){t->arcs++;t->arc_px+=px;}
    if(lv_obj_check_type(o,&lv_image_class))t->images++;
    if(lv_obj_check_type(o,&lv_bar_class))t->bars++;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)perf_analyze(lv_obj_get_child(o,i),area,t);
}
static void perf_report(FILE *f,const char *name,lv_obj_t *o,int64_t region_px){
    perf_stats t={0};lv_area_t clip;lv_obj_get_coords(o,&clip);perf_analyze(o,clip,&t);
    fprintf(f,"%-26s objects=%u filled=%u (rounded %u) borders=%u outlines=%u shadows=%u gradients=%u layered=%u labels=%u arcs=%u bars=%u images=%u | fill overdraw %.2fx (rounded %.2fx) label %.2fx arc %.2fx\n",name,t.objects,t.filled,t.rounded,t.borders,t.outlines,t.shadows,t.gradients,t.layered,t.labels,t.arcs,t.bars,t.images,
        (double)t.fill_px/(double)region_px,(double)t.round_px/(double)region_px,(double)t.label_px/(double)region_px,(double)t.arc_px/(double)region_px);
}
static FILE *perf_areas;
static void perf_invalidated(lv_event_t *e){const lv_area_t *a=lv_event_get_param(e);if(perf_areas&&a)fprintf(perf_areas,"  inv %4d,%4d %4dx%-4d\n",(int)a->x1,(int)a->y1,(int)lv_area_get_width(a),(int)lv_area_get_height(a));}
static int perf_probe(void){
    FILE *f=fopen("perf-probe.txt","w");if(!f)return 2;
    pump(3500);
    lv_display_add_event_cb(display,perf_invalidated,LV_EVENT_INVALIDATE_AREA,NULL);
    lv_obj_t *viewport=lv_obj_get_child(lv_screen_active(),0);
    fprintf(f,"objects idle=%u\n",count_objects(lv_screen_active()));
    perf_report(f,"viewport content (cards)",viewport,1032*420);
    {perf_areas=f;fprintf(f,"telemetry tick invalidations:\n");gso_snapshot s;desk_data_latest(&s);s.cpu.load=gso_metric_number(11);s.cpu.clock=gso_metric_number(4010);desk_ui_apply_snapshot(&s);perf_frame(perf_open("(areas)"));perf_areas=NULL;perf_count--;}
    {perf_row warm={0};gso_snapshot same;desk_data_latest(&same);desk_ui_apply_snapshot(&same);perf_frame(&warm);
     perf_row *r=perf_open("identical snapshot re-applied");for(int i=0;i<10;i++){if(i==3){perf_areas=f;fprintf(f,"identical re-apply invalidations:\n");}desk_ui_apply_snapshot(&same);perf_frame(r);perf_areas=NULL;}}
    {perf_row *r=perf_open("telemetry tick (cpu/net numbers change)");
     for(int i=0;i<20;i++){gso_snapshot s;desk_data_latest(&s);s.cpu.load=gso_metric_number(10+i*4);s.cpu.clock=gso_metric_number(4000+i*20);desk_ui_apply_snapshot(&s);perf_frame(r);}}
    {perf_row *r=perf_open("drag viewport left, 16 px per frame");
     mouse.x=900;mouse.y=200;pressed=true;perf_frame(r);
     for(int i=0;i<24;i++){mouse.x-=16;perf_frame(r);}
     perf_row *t=perf_open("release and momentum");pressed=false;for(int i=0;i<40;i++)perf_frame(t);
     fprintf(f,"viewport scroll_x after drag=%d\n",(int)lv_obj_get_scroll_x(viewport));
     perf_row *b=perf_open("drag viewport right, 16 px per frame");
     mouse.x=500;mouse.y=200;pressed=true;perf_frame(b);
     for(int i=0;i<24;i++){mouse.x+=16;perf_frame(b);}
     pressed=false;perf_row *t2=perf_open("release and momentum (back)");for(int i=0;i<40;i++)perf_frame(t2);}
    const int ids[]={10,5,11,15,16};const char *names[]={"open WIN detail","open VIBE detail","open NET detail","open HOME detail","open GAME detail"};
    for(int k=0;k<5;k++){
        perf_row *r=perf_open(names[k]);desk_ui_detail(ids[k]);
        for(int i=0;i<30;i++)perf_frame(r);
        fprintf(f,"objects %s=%u\n",names[k],count_objects(lv_screen_active()));
        perf_report(f,names[k],lv_obj_get_child(lv_screen_active(),-1),1032*420);
        {perf_row *t=perf_open("  telemetry tick on this page");for(int i=0;i<10;i++){gso_snapshot s;desk_data_latest(&s);s.cpu.load=gso_metric_number(10+i*7);s.gpu.load=gso_metric_number(20+i*5);s.download=gso_metric_number(1e6*(i+1));s.upload=gso_metric_number(2e5*(i+1));s.fps=gso_metric_number(100+i);s.latency=gso_metric_number(8+i);if(i==3&&ids[k]!=10){perf_areas=f;fprintf(f,"%s tick invalidations:\n",names[k]);}desk_ui_apply_snapshot(&s);perf_frame(t);perf_areas=NULL;}}
        perf_row *c=perf_open("  close");desk_ui_close_detail();for(int i=0;i<30;i++)perf_frame(c);
    }
    fprintf(f,"%-44s %6s %6s %10s %10s %9s %8s\n","scenario","frames","drawn","avg_kpx","max_kpx","avg_ms","max_ms");
    for(unsigned i=0;i<perf_count;i++){perf_row *r=&perf_rows[i];
        fprintf(f,"%-44s %6u %6u %10.0f %10.0f %9.2f %8.2f\n",r->name,r->frames,r->drawn,r->drawn?(double)r->pixels/r->drawn/1000.0:0.0,(double)r->max_pixels/1000.0,r->drawn?r->ms/r->drawn:0.0,r->max_ms);}
    fprintf(f,"screen = %d kpx\n",WIDTH*HEIGHT/1000);
    fclose(f);return 0;
}
#endif
int main(int argc,char **argv) {
    bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=WIDTH;
    bitmap.bmiHeader.biHeight=-HEIGHT;bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;
    bitmap.bmiHeader.biCompression=BI_RGB;
    lv_init();lv_tick_set_cb(ticks);
    display=lv_display_create(WIDTH,HEIGHT);lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_flush_cb(display,flush);lv_display_set_buffers(display,drawbuffer,NULL,sizeof(drawbuffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(in,pointer_read);
    const char *configuration="config/display.json";
    for(int i=1;i+1<argc;i++)if(strcmp(argv[i],"--config")==0)configuration=argv[i+1];
#ifdef GSO_TEST
    if(!(argc>1&&strcmp(argv[1],"--selftest")==0))
#endif
    desk_data_start(configuration);
    desk_brightness_start(argc>1&&!strcmp(argv[1],"--selftest")?NULL:configuration);
    gss_wifi_start();gss_wifi_radio_start();
    desk_ui_init();
#ifdef GSO_TEST
    if(argc>1 && strcmp(argv[1],"--selftest")==0)return selftest();
#endif
#ifdef GSO_TEST
    if(argc>1&&!strcmp(argv[1],"--perf-probe"))return perf_probe();
    if(argc>1&&!strcmp(argv[1],"--clash-switch-test")){pump(1200);bool ok=true;const char *modes[]={"rule","global","direct"};const int order[]={1,2,0};for(int view=0;ok&&view<2;view++){if(view==0)desk_ui_detail(11);else desk_ui_close_detail();pump(250);lv_obj_t *switches[3];lv_obj_t *parent=view==0?lv_obj_get_child(lv_screen_active(),-1):lv_screen_active();ok=collect_segments(parent,switches,3)==3;for(int step=0;ok&&step<3;step++){int target=order[step];lv_obj_update_layout(switches[target]);lv_area_t a;lv_obj_get_coords(switches[target],&a);click_at((a.x1+a.x2)/2,(a.y1+a.y2)/2);desk_clash_result r={0};for(int wait=0;wait<80;wait++){pump(50);r=desk_clash_latest();if(!r.busy&&r.state==CLASH_CONFIRMED&&!strcmp(r.mode,modes[target]))break;}pump(1100);ok=desk_ui_current_page()==0&&desk_ui_detail_is_open()==(view==0)&&!r.busy&&r.state==CLASH_CONFIRMED&&!strcmp(r.mode,modes[target]);for(int j=0;j<3;j++)ok=ok&&(lv_obj_has_state(switches[j],LV_STATE_CHECKED)==(j==target));}}desk_data_stop();lv_deinit();return ok?0:3;}
    if(argc>1&&!strcmp(argv[1],"--clash-current-test")){pump(1500);desk_clash_result before=desk_clash_latest();if(!before.mode[0]||!desk_clash_mode(before.mode)){desk_data_stop();return 3;}desk_clash_result after={0};for(int i=0;i<100;i++){pump(50);after=desk_clash_latest();if(!after.busy&&after.state==CLASH_CONFIRMED&&after.observed_ms>before.observed_ms)break;}bool ok=after.state==CLASH_CONFIRMED&&!after.busy&&after.observed_ms>before.observed_ms&&!strcmp(before.mode,after.mode);desk_data_stop();lv_deinit();return ok?0:3;}
    if(argc>1&&!strcmp(argv[1],"--source-status-test")){pump(1800);int status=(int)desk_data_status();desk_data_stop();lv_deinit();return 20+status;}
    if(argc>1&&!strcmp(argv[1],"--home-cover-test")){pump(1800);if(!desk_home_test_isolated()){desk_data_stop();lv_deinit();return 4;}desk_ui_detail(15);pump(250);int ox=0,oy=0,cx=0,cy=0;int clicked=desk_ui_home_cover_center(0,&ox,&oy)&&desk_ui_home_cover_center(1,&cx,&cy);if(clicked){click_at(ox,oy);pump(150);clicked=desk_ui_home_cover_center(1,&cx,&cy);if(clicked)click_at(cx,cy);}desk_home_result result={0};for(int wait_ms=0;wait_ms<6000;wait_ms+=50){pump(50);result=desk_home_latest();if(result.state==DESK_HOME_CONFIRMED&&!result.busy)break;}int ok=clicked&&result.state==DESK_HOME_CONFIRMED&&result.action==GSO_HOME_CLOSE&&!result.busy;if(!ok)fprintf(stderr,"Cover gesture: clicked=%d state=%d action=%u busy=%d\n",clicked,result.state,result.action,result.busy);desk_data_stop();lv_deinit();return ok?0:3;}
    if(argc>1&&!strcmp(argv[1],"--home-click-test")){pump(1800);if(!desk_home_test_isolated()){desk_data_stop();lv_deinit();return 4;}desk_ui_detail(15);pump(250);int px=0,py=0;if(!desk_ui_home_power_center(&px,&py))return 3;click_at(px,py);click_at(px,py);desk_home_result result={0};for(int wait_ms=0;wait_ms<6000;wait_ms+=50){pump(50);result=desk_home_latest();if(result.state==DESK_HOME_CONFIRMED&&!result.busy)break;}int ok=result.state==DESK_HOME_CONFIRMED&&result.action==GSO_HOME_OFF&&!result.busy&&!has_label(lv_screen_active(),"状态已回报")&&!has_label(lv_screen_active(),"等待发送");desk_data_stop();lv_deinit();return ok?0:3;}
    if(argc>1&&(!strcmp(argv[1],"--home-slider-test")||!strcmp(argv[1],"--home-mode-test"))){pump(1800);if(!desk_home_test_isolated()){desk_data_stop();lv_deinit();return 4;}desk_ui_detail(15);pump(250);bool slider=!strcmp(argv[1],"--home-slider-test");if(slider){lv_obj_t *drag=find_visible_type(lv_screen_active(),&lv_slider_class);if(!drag)return 3;lv_obj_update_layout(drag);lv_area_t da;lv_obj_get_coords(drag,&da);int dx=da.x1+(da.x2-da.x1)*3/4,dy=(da.y1+da.y2)/2;mouse.x=dx;mouse.y=dy;pressed=true;pump(80);for(int step=1;step<=10;step++){mouse.x=dx-step*12;pump(25);}pressed=false;pump(100);}else{int mx=0,my=0;if(!desk_ui_home_mode_center(0,&mx,&my))return 3;click_at(mx,my);}desk_home_result result={0};for(int wait_ms=0;wait_ms<6000;wait_ms+=50){pump(50);result=desk_home_latest();if(result.state==DESK_HOME_CONFIRMED&&!result.busy)break;}int ok=result.state==DESK_HOME_CONFIRMED&&result.action==(slider?GSO_HOME_BRIGHTNESS:GSO_HOME_HVAC)&&!result.busy;if(!ok)fprintf(stderr,"HOME setting gesture: state=%d action=%u busy=%d\n",result.state,result.action,result.busy);desk_data_stop();lv_deinit();return ok?0:3;}
    for(int i=1;i+1<argc;i++)if(strcmp(argv[i],"--capture-file")==0){pump(3500);for(int j=1;j+1<argc;j++)if(!strcmp(argv[j],"--capture-theme")){desk_ui_theme_mode_set(!strcmp(argv[j+1],"day")?1:0);pump(100);}for(int j=1;j<argc;j++)if(!strcmp(argv[j],"--capture-settings")){click_at(100,400);pump(300);}for(int j=1;j+1<argc;j++)if(strcmp(argv[j],"--capture-page")==0)desk_ui_page(atoi(argv[j+1]));for(int j=1;j+1<argc;j++)if(strcmp(argv[j],"--capture-detail")==0)desk_ui_detail(atoi(argv[j+1]));pump(50);int ok=dump(argv[i+1])&&desk_data_connected();desk_data_stop();lv_deinit();return ok?0:3;}
#endif
    SetProcessDPIAware();
    WNDCLASSW wc={0};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(NULL);
    wc.lpszClassName=L"SidecarLVGL";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);RegisterClassW(&wc);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT r={0,0,WIDTH,HEIGHT};AdjustWindowRect(&r,style,FALSE);
    window=CreateWindowW(wc.lpszClassName,L"GSO STATUS SCREEN · WIN / VIBE / NET / HOME · LVGL 9.4",style,
        CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,wc.hInstance,NULL);
    if(!window)return 1;
    ShowWindow(window,SW_SHOW);lv_refr_now(display);UpdateWindow(window);
    MSG msg;bool run=true;
    while(run){while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){run=false;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(run){lv_timer_handler();Sleep(5);}}
    gss_wifi_radio_stop();desk_data_stop();lv_deinit();return 0;
}
