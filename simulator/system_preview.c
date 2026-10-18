#include "system_preview.h"
#include <math.h>
#include <windows.h>
/* Windows stand-in for main/gss_system.c: the numbers are samples that look like the board's, not measurements of this PC. */
static bool imu_fixed,imu_present=true,imu_open,system_fixed;
static gso_accel imu_value;
static gss_system_info system_value;
static uint64_t started_ms;
static uint64_t now_ms(void){
 return GetTickCount64();
}
void gss_preview_imu(bool present,gso_accel sample){
 imu_fixed=true;
 imu_present=present;
 imu_value=sample;
}
void gss_preview_imu_sway(void){
 imu_fixed=false;
 imu_present=true;
}
void gss_preview_system(const gss_system_info *info){
 system_fixed=info!=NULL;
 if(info)system_value=*info;
}
void gss_system_read(gss_system_info *info){
 if(!started_ms)started_ms=now_ms();
 if(system_fixed){
  *info=system_value;
  return;
 }
 double t=(double)(now_ms()-started_ms)/1000.0;
 info->cpu_valid=true;
 info->cpu_percent[0]=(unsigned)(18+10*sin(t/3.1));
 info->cpu_percent[1]=(unsigned)(9+6*sin(t/4.7));
 info->memory_valid=true;
 info->internal_total=480u*1024u;
 info->internal_free=(uint32_t)((178+6*sin(t/9))*1024);
 info->psram_total=32u*1024u*1024u;
 info->psram_free=(uint32_t)((24.6+0.1*sin(t/11))*1024*1024);
 info->storage_valid=true;
 info->flash_total=16u*1024u*1024u;
 info->app_total=0xFE0000u;
 info->app_used=(uint32_t)(3.4*1024*1024);
 info->temperature_valid=true;
 info->temperature_c=(float)(47+3*sin(t/20));
 info->uptime_s=(uint32_t)(t+3*3600+41*60);
}
bool gss_imu_open(void){
 imu_open=imu_present;
 return imu_open;
}
void gss_imu_close(void){
 imu_open=false;
}
bool gss_imu_read(gso_accel *sample){
 if(!imu_open)return false;
 if(imu_fixed){
  *sample=imu_value;
  return true;
 }
 if(!started_ms)started_ms=now_ms();
 double t=(double)(now_ms()-started_ms)/1000.0;
 double roll=1.4*sin(t/2.2)*3.14159265/180;
 *sample=(gso_accel){(float)-sin(roll),(float)cos(roll),0.02f};
 return true;
}
