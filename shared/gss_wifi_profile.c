#include "gss_wifi.h"
#include "gss_wifi_storage.h"
#include <string.h>
#include <ctype.h>
typedef struct {unsigned char version,open_network;char ssid[33],password[65];} wifi_profile;
static wifi_profile profile;
void gss_wifi_start(void){
 size_t size=sizeof(profile);memset(&profile,0,sizeof(profile));
 if(!gss_wifi_storage_read(&profile,&size)||size!=sizeof(profile)||profile.version!=1||!memchr(profile.ssid,0,33)||!memchr(profile.password,0,65))memset(&profile,0,sizeof(profile));
}
const char *gss_wifi_ssid(void){return profile.ssid;}
bool gss_wifi_has_password(void){return profile.password[0]!=0;}
bool gss_wifi_is_open(void){return profile.ssid[0]&&profile.open_network;}
const char *gss_wifi_save(const char *ssid,const char *password,bool open_network){
 size_t n=strlen(ssid),p=strlen(password);if(!n||n>32)return "网络名称需为1–32字节";
 wifi_profile next={.version=1,.open_network=open_network};memcpy(next.ssid,ssid,n);
 if(!open_network){
  if(!p&&!strcmp(ssid,profile.ssid)&&!profile.open_network)memcpy(next.password,profile.password,sizeof(next.password));
  else {if(p<8||p>64)return "密码需为8–63字符或64位十六进制";
   for(size_t i=0;i<p;i++)if((unsigned char)password[i]<32||(unsigned char)password[i]>126||(p==64&&!isxdigit((unsigned char)password[i])))return "密码格式无效";
   memcpy(next.password,password,p);
  }
  if(!next.password[0])return "请输入网络密码";
 }
 if(!gss_wifi_storage_write(&next,sizeof(next))){memset(&next,0,sizeof(next));return "保存失败";}
 profile=next;memset(&next,0,sizeof(next));return NULL;
}
bool gss_wifi_copy(char ssid[33],char password[65],bool *open_network){memcpy(ssid,profile.ssid,33);memcpy(password,profile.password,65);*open_network=profile.open_network;return ssid[0]!=0;}
bool gss_wifi_forget(void){
 bool ok=gss_wifi_storage_forget();if(ok)memset(&profile,0,sizeof(profile));return ok;
}
