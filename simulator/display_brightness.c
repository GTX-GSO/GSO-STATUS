#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "display_brightness.h"
#include "cJSON.h"
static gso_brightness state;
static wchar_t preferences[1200];
gso_brightness *desk_brightness_state(void){return &state;}
void desk_brightness_start(const char *config){
 gso_brightness_init(&state,true,80);preferences[0]=0;if(!config)return;
 wchar_t path[1100];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,config,-1,path,1100))return;
 if(swprintf(preferences,1200,L"%ls.preferences.json",path)<0){preferences[0]=0;return;}
 FILE *f=_wfopen(preferences,L"rb");if(!f)return;char raw[513];size_t n=fread(raw,1,sizeof(raw),f);fclose(f);if(n>=sizeof(raw))return;raw[n]=0;
 cJSON *o=cJSON_Parse(raw);if(!o)return;const cJSON *a=cJSON_GetObjectItemCaseSensitive(o,"automatic"),*m=cJSON_GetObjectItemCaseSensitive(o,"manual_percent");
 if(cJSON_IsBool(a)&&cJSON_IsNumber(m)&&m->valuedouble>=10&&m->valuedouble<=100&&m->valuedouble==m->valueint)gso_brightness_init(&state,cJSON_IsTrue(a),(unsigned)m->valueint);
 cJSON_Delete(o);
}
bool desk_brightness_save(void){
 if(!preferences[0])return true;
 wchar_t temp[1250];if(swprintf(temp,1250,L"%ls.tmp",preferences)<0)return false;
 FILE *f=_wfopen(temp,L"wb");if(!f)return false;
 int n=fprintf(f,"{\"automatic\":%s,\"manual_percent\":%u}\n",state.automatic?"true":"false",state.manual);
 bool ok=n>0;if(fclose(f))ok=false;
 if(ok)ok=MoveFileExW(temp,preferences,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
 if(!ok)DeleteFileW(temp);return ok;
}
